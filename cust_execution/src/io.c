/* cust_execution::io — epoll-backed I/O readiness driver.
 *
 * The second external wake source, and the one that subsumes
 * the timer reactor. A thread can only block in one place, so
 * timers and I/O cannot each own a `park`: instead the epoll
 * driver IS the parker, and the timer reactor becomes its
 * timeout source. One `epoll_wait` call services both —
 * readiness events wake I/O futures, and the next timer
 * deadline is passed as the wait timeout so sleeps still fire.
 * This is exactly Tokio's layering (scheduler ⟂ time driver ⟂
 * I/O driver, with the time driver's deadline feeding the I/O
 * park).
 *
 *   Rust/Tokio                   cust_execution
 *   -------------------------    ---------------------------
 *   mio::Poll / epoll            cexec_io_driver (epoll fd)
 *   Interest::READABLE           cexec_readable_in (EPOLLIN)
 *   epoll_wait(next_deadline)    io_driver_park
 *   AsyncFd registration         struct io_reg (in the future)
 *
 * Linux only (epoll) — consistent with cust's Linux-first,
 * clang-only stance.
 *
 * Readiness discipline: epoll readiness is advisory. A real I/O
 * future must do the non-blocking syscall and retry on EAGAIN;
 * `cexec_readable_in` is the readiness primitive those futures
 * build on (it resolves when the fd is *probably* readable; the
 * caller then performs the actual non-blocking read and re-awaits
 * if it sees EAGAIN). Registrations are one-shot (EPOLLONESHOT)
 * and intrusively embedded in the future, so arming an await
 * costs no extra allocation and the kernel never holds a pointer
 * into freed memory (the future deregisters on drop).
 *
 * Threading: NONE. A cross-thread waker would need an eventfd
 * registered in the epoll set so a wake from another thread can
 * break an in-progress epoll_wait; single-threaded with only
 * timers + fds, the wait timeout and the fds themselves are the
 * only wake sources, so no eventfd is required yet.
 */

#cust use std;
#cust use crate::future;
#cust use crate::executor;
#cust use crate::reactor;

#include <stddef.h>
#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>

#define IO_MAX_EVENTS 32

/* ─── registration (intrusive in an I/O future) ─────────── */

struct io_reg {
    struct cexec_waker waker;      /* cloned from the future's poll */
    int                fd;
    bool               registered; /* added to the epoll set */
    bool               fired;      /* epoll reported it; waker consumed */
};

/* ─── driver ────────────────────────────────────────────── */

struct [[cust::pub_repr]] cexec_io_driver {
    struct cstd_alloc    alloc;
    struct cexec_reactor timers;   /* owned: supplies the epoll timeout */
    int                  epfd;      /* epoll instance */
    usize                npending;  /* armed regs not yet fired */
};

/* Create the epoll instance and an embedded real-clock timer
 * reactor. Returns false if epoll_create1 fails. */
[[cust::pub]] bool cexec_io_driver_init(struct cexec_io_driver *d,
                                        struct cstd_alloc a) {
    d->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (d->epfd < 0) {
        return false;
    }
    d->alloc    = a;
    d->npending = 0;
    cexec_reactor_init(&d->timers, a, cexec_clock_system());
    return true;
}

[[cust::pub]] void cexec_io_driver_close(struct cexec_io_driver *d) {
    if (d->epfd >= 0) {
        close(d->epfd);
        d->epfd = -1;
    }
}

/* The timer reactor inside this driver — pass to cexec_sleep_in
 * so sleeps share the driver's single epoll_wait. */
[[cust::pub]] struct cexec_reactor *
cexec_io_driver_reactor(struct cexec_io_driver *d) {
    return &d->timers;
}

/* The unified park: block in one epoll_wait whose timeout is
 * the next timer deadline, then fire ready fds and due timers. */
static bool io_driver_park(void *state) {
    struct cexec_io_driver *d = state;

    u64 deadline;
    bool have_timer = cexec_reactor_earliest(&d->timers, &deadline);
    if (!have_timer && d->npending == 0) {
        return false; /* no fds, no timers: nothing can wake us */
    }

    int timeout_ms = -1; /* block indefinitely when only fds are pending */
    if (have_timer) {
        u64 now = cexec_reactor_now(&d->timers);
        if (deadline <= now) {
            timeout_ms = 0;
        } else {
            u64 ms = (deadline - now + 999999ull) / 1000000ull; /* ns→ms ceil */
            timeout_ms = ms > 2147483647ull ? 2147483647 : (int)ms;
        }
    }

    struct epoll_event evs[IO_MAX_EVENTS];
    int n;
    do {
        n = epoll_wait(d->epfd, evs, IO_MAX_EVENTS, timeout_ms);
    } while (n < 0 && errno == EINTR);

    bool progressed = false;
    for (int i = 0; i < n; i++) {
        struct io_reg *reg = evs[i].data.ptr;
        if (!reg->fired) {
            reg->fired = true;
            d->npending--;
            cexec_waker_wake(reg->waker); /* consumes the clone */
            progressed = true;
        }
    }
    if (have_timer) {
        if (cexec_reactor_fire_due(&d->timers, cexec_reactor_now(&d->timers))) {
            progressed = true;
        }
    }
    return progressed;
}

[[cust::pub]] struct cexec_park
cexec_io_driver_as_park(struct cexec_io_driver *d) {
    struct cexec_park p;
    p.park  = io_driver_park;
    p.state = d;
    return p;
}

/* ─── readable(fd) future ───────────────────────────────── */

struct readable_box {
    struct cstd_alloc       alloc;
    struct cexec_io_driver *driver;
    struct io_reg           reg;
};

static cexec_poll readable_poll(void *self, struct cexec_waker w, void *out) {
    (void)out;
    struct readable_box *b = self;

    if (!b->reg.registered) {
        b->reg.waker = cexec_waker_clone(w);
        b->reg.fired = false;
        struct epoll_event ev;
        ev.events   = EPOLLIN | EPOLLONESHOT;
        ev.data.ptr = &b->reg;
        if (epoll_ctl(b->driver->epfd, EPOLL_CTL_ADD, b->reg.fd, &ev) != 0) {
            /* Cannot watch this fd (closed/invalid): drop the
             * clone and resolve so the caller proceeds to the
             * real read and observes the error itself. */
            cexec_waker_drop(b->reg.waker);
            return cexec_poll_ready();
        }
        b->reg.registered = true;
        b->driver->npending++;
        return cexec_poll_pending();
    }

    return b->reg.fired ? cexec_poll_ready() : cexec_poll_pending();
}

static void readable_drop(void *self) {
    struct readable_box *b = self;
    if (b->reg.registered) {
        /* Remove from the epoll set first so the kernel never
         * holds a data.ptr into this soon-to-be-freed box. */
        epoll_ctl(b->driver->epfd, EPOLL_CTL_DEL, b->reg.fd, (void *)0);
        if (!b->reg.fired) {
            b->driver->npending--;
            cexec_waker_drop(b->reg.waker);
        }
    }
    cstd_alloc_deallocate(b->alloc, b, sizeof *b, _Alignof(struct readable_box));
}

static const struct cexec_future_vtable readable_vtable = {
    .poll = readable_poll,
    .drop = readable_drop,
};

/* A future that resolves when `fd` is readable (EPOLLIN). Unit
 * output — the caller performs the actual non-blocking read and
 * re-awaits on EAGAIN. Returns the null future on OOM. */
[[cust::pub]] struct cexec_future cexec_readable_in(struct cstd_alloc a,
                                                    struct cexec_io_driver *d,
                                                    int fd) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct readable_box *b = cstd_alloc_allocate(a, sizeof *b,
                                                 _Alignof(struct readable_box));
    if (!b) {
        return f;
    }
    b->alloc          = a;
    b->driver         = d;
    b->reg.fd         = fd;
    b->reg.registered = false;
    b->reg.fired      = false;
    f.self            = b;
    f.vtable          = &readable_vtable;
    return f;
}

/* ─── unit tests ────────────────────────────────────────── */

#cust use crate::combinators;

#include <fcntl.h>

#define MS 1000000ull

/* A waker that does nothing — enough to arm a registration in a
 * test that never lets the driver fire. */
static void io_noop_wake(void *d) { (void)d; }
static struct cexec_waker io_noop_waker(void);
static struct cexec_waker io_noop_clone(void *d) { (void)d; return io_noop_waker(); }
static void io_noop_drop(void *d) { (void)d; }

static const struct cexec_waker_vtable io_noop_waker_vtable = {
    .wake        = io_noop_wake,
    .wake_by_ref = io_noop_wake,
    .clone       = io_noop_clone,
    .drop        = io_noop_drop,
};

static struct cexec_waker io_noop_waker(void) {
    struct cexec_waker w;
    w.data   = (void *)0;
    w.vtable = &io_noop_waker_vtable;
    return w;
}

/* Build a non-blocking pipe; returns false on failure. */
static bool make_pipe(int *rfd, int *wfd) {
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    int fl = fcntl(fds[0], F_GETFL, 0);
    fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    *rfd = fds[0];
    *wfd = fds[1];
    return true;
}

[[cust::test]] int test_readable_completes_when_data_present(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    int rfd, wfd;
    cust_assert(make_pipe(&rfd, &wfd));

    /* Make the read end ready before we block (single thread). */
    cust_assert(write(wfd, "x", 1) == 1);

    bool done = cexec_executor_block_on(&ex, cexec_readable_in(a, &io, rfd),
                                        (void *)0);
    cust_assert(done);

    char buf = 0;
    cust_assert(read(rfd, &buf, 1) == 1);
    cust_assert(buf == 'x');

    close(rfd);
    close(wfd);
    cexec_io_driver_close(&io);
    return 0;
}

[[cust::test]] int test_io_park_services_timer(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    /* No fds registered: the only wake source is the timer, so
     * this proves epoll_wait's timeout drives the sleep. */
    u64 start = cexec_reactor_now(cexec_io_driver_reactor(&io));
    bool done = cexec_executor_block_on(
        &ex, cexec_sleep_in(a, cexec_io_driver_reactor(&io), 2 * MS),
        (void *)0);
    cust_assert(done);
    cust_assert(cexec_reactor_now(cexec_io_driver_reactor(&io)) - start
                >= 2 * MS);

    cexec_io_driver_close(&io);
    return 0;
}

[[cust::test]] int test_io_and_timer_share_one_park(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    int rfd, wfd;
    cust_assert(make_pipe(&rfd, &wfd));
    cust_assert(write(wfd, "y", 1) == 1);

    /* One executor, one epoll_wait loop, servicing both a
     * readable fd and a sleep concurrently. */
    struct cexec_future f =
        cexec_join2_in(a, cexec_readable_in(a, &io, rfd), 0,
                       cexec_sleep_in(a, cexec_io_driver_reactor(&io), 2 * MS));
    cust_assert(cexec_executor_block_on(&ex, f, (void *)0));

    char buf = 0;
    cust_assert(read(rfd, &buf, 1) == 1);
    cust_assert(buf == 'y');

    close(rfd);
    close(wfd);
    cexec_io_driver_close(&io);
    return 0;
}

[[cust::test]] int test_dropping_unfired_readable_deregisters(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));

    int rfd, wfd;
    cust_assert(make_pipe(&rfd, &wfd));
    /* No data written: the fd never becomes readable. Arm a
     * readable future by polling once, then drop it; the driver
     * must end with no pending registrations. */
    struct cexec_future f = cexec_readable_in(a, &io, rfd);

    cexec_poll st = cexec_future_poll(f, io_noop_waker(), (void *)0);
    cust_assert(!cexec_poll_is_ready(st));
    cust_assert(io.npending == 1);
    cexec_future_drop(f);
    cust_assert(io.npending == 0);

    close(rfd);
    close(wfd);
    cexec_io_driver_close(&io);
    return 0;
}
