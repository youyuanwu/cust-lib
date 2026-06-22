/* cust_execution::io — epoll-backed I/O readiness driver.
 *
 * The second external wake source, and the one that subsumes
 * the timer reactor. A thread can only block in one place, so
 * timers and I/O cannot each own a `park`: the epoll driver IS
 * the parker, and the timer reactor becomes its timeout source.
 * One `epoll_wait` services both — readiness events wake I/O
 * futures, and the next timer deadline is the wait timeout so
 * sleeps still fire. This is Tokio's layering (scheduler ⟂ time
 * driver ⟂ I/O driver, the deadline feeding the I/O park).
 *
 *   Rust/Tokio                   cust_execution
 *   -------------------------    ---------------------------
 *   mio::Poll / epoll            cexec_io_driver (epoll fd)
 *   ScheduledIo (per-fd state)   struct io_sched (per fd)
 *   Interest READABLE/WRITABLE   cexec_readable_in / writable_in
 *   AsyncFd::new / drop          cexec_io_add / cexec_io_remove
 *
 * Per-fd registration: an fd is registered ONCE (cexec_io_add)
 * and lives until cexec_io_remove. Each fd's `io_sched` holds a
 * separate read and write waker slot, so a socket can have a
 * reader and a writer awaiting it *simultaneously* — the model
 * a single ADD-per-future would break on with EEXIST. The epoll
 * interest mask is recomputed (EPOLL_CTL_MOD) from the armed
 * slots, so a level-triggered EPOLLOUT is only watched while a
 * writer is actually waiting (no busy-spin on always-writable
 * sockets). Linux only.
 *
 * Readiness discipline: epoll readiness is advisory. A real I/O
 * future does the non-blocking syscall and retries on EAGAIN;
 * `cexec_readable_in` / `cexec_writable_in` are the readiness
 * primitives those build on. EPOLLERR/EPOLLHUP are reported by
 * the kernel unconditionally and wake *both* slots, so a writer
 * blocked on connect() learns of a failed connection.
 *
 * SIGPIPE: writing to a peer-closed socket raises SIGPIPE, which
 * kills the process by default. The driver ignores it once at
 * init so a disconnect surfaces as EPIPE on the write instead.
 *
 * Threading: NONE. A cross-thread waker would need an eventfd in
 * the epoll set to break an in-progress epoll_wait; single
 * threaded, the wait timeout and the fds are the only wake
 * sources.
 *
 * Lifetime: futures hold a raw fd and look up its `io_sched`; an
 * fd must stay registered (added, not removed/closed) while any
 * future awaiting it is alive. A single read waiter and a single
 * write waiter per fd are supported (the common case).
 */

#cust use std;
#cust use crate::future;
#cust use crate::executor;
#cust use crate::reactor;

#include <stddef.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/epoll.h>

#define IO_MAX_EVENTS 32

/* ─── per-fd registration ───────────────────────────────── */

struct io_sched {
    struct std_list_head link;        /* driver's fd table */
    int                  fd;
    struct cexec_waker   read_waker;  /* valid iff read_armed */
    struct cexec_waker   write_waker; /* valid iff write_armed */
    bool                 read_armed;
    bool                 write_armed;
};

#define SCHED_OF(node) \
    ((struct io_sched *)((u8 *)(node) - offsetof(struct io_sched, link)))

/* ─── driver ────────────────────────────────────────────── */

struct [[cust::pub_repr]] cexec_io_driver {
    struct cstd_alloc    alloc;
    struct cexec_reactor timers;  /* owned: supplies the epoll timeout */
    struct std_list_head fds;     /* registered io_sched list */
    int                  epfd;     /* epoll instance */
    usize                armed;    /* total armed slots across all fds */
};

[[cust::pub]] bool cexec_io_driver_init(struct cexec_io_driver *d,
                                        struct cstd_alloc a) {
    /* Never die on a write to a closed peer — see EPIPE instead. */
    signal(SIGPIPE, SIG_IGN);
    d->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (d->epfd < 0) {
        return false;
    }
    d->alloc = a;
    d->armed = 0;
    std_list_init(&d->fds);
    cexec_reactor_init(&d->timers, a, cexec_clock_system());
    return true;
}

static struct io_sched *find_sched(struct cexec_io_driver *d, int fd) {
    for (struct std_list_head *p = d->fds.next; p != &d->fds; p = p->next) {
        struct io_sched *s = SCHED_OF(p);
        if (s->fd == fd) {
            return s;
        }
    }
    return (void *)0;
}

/* Recompute the epoll interest mask from the armed slots so we
 * only watch a direction while a future is waiting on it. */
static void sched_update_interest(struct cexec_io_driver *d,
                                  struct io_sched *s) {
    struct epoll_event ev;
    ev.events   = (s->read_armed ? (u32)EPOLLIN : 0u)
                | (s->write_armed ? (u32)EPOLLOUT : 0u);
    ev.data.ptr = s;
    epoll_ctl(d->epfd, EPOLL_CTL_MOD, s->fd, &ev);
}

/* Register `fd` (idempotent). Must be called before awaiting
 * readability/writability on it, and balanced by
 * cexec_io_remove before the fd is closed. */
[[cust::pub]] bool cexec_io_add(struct cexec_io_driver *d, int fd) {
    if (find_sched(d, fd)) {
        return true;
    }
    struct io_sched *s = cstd_alloc_allocate(d->alloc, sizeof *s,
                                             _Alignof(struct io_sched));
    if (!s) {
        return false;
    }
    s->fd          = fd;
    s->read_armed  = false;
    s->write_armed = false;
    struct epoll_event ev;
    ev.events   = 0; /* no interest until a future arms a slot */
    ev.data.ptr = s;
    if (epoll_ctl(d->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        cstd_alloc_deallocate(d->alloc, s, sizeof *s,
                              _Alignof(struct io_sched));
        return false;
    }
    std_list_add_tail(&s->link, &d->fds);
    return true;
}

static void sched_free(struct cexec_io_driver *d, struct io_sched *s) {
    epoll_ctl(d->epfd, EPOLL_CTL_DEL, s->fd, (void *)0);
    if (s->read_armed) {
        d->armed--;
        cexec_waker_drop(s->read_waker);
    }
    if (s->write_armed) {
        d->armed--;
        cexec_waker_drop(s->write_waker);
    }
    std_list_del(&s->link);
    cstd_alloc_deallocate(d->alloc, s, sizeof *s, _Alignof(struct io_sched));
}

/* Deregister `fd` and release any waiting waker clones. Call
 * before closing the fd. */
[[cust::pub]] void cexec_io_remove(struct cexec_io_driver *d, int fd) {
    struct io_sched *s = find_sched(d, fd);
    if (s) {
        sched_free(d, s);
    }
}

[[cust::pub]] void cexec_io_driver_close(struct cexec_io_driver *d) {
    while (!std_list_is_empty(&d->fds)) {
        sched_free(d, SCHED_OF(d->fds.next));
    }
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

/* The unified park: block in one epoll_wait whose timeout is the
 * next timer deadline, then wake ready fds and fire due timers. */
static bool io_driver_park(void *state) {
    struct cexec_io_driver *d = state;

    u64 deadline;
    bool have_timer = cexec_reactor_earliest(&d->timers, &deadline);
    if (!have_timer && d->armed == 0) {
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
        struct io_sched *s = evs[i].data.ptr;
        u32 e = evs[i].events;
        bool err = (e & ((u32)EPOLLERR | (u32)EPOLLHUP)) != 0;

        if (s->read_armed && (((e & (u32)EPOLLIN) != 0) || err)) {
            s->read_armed = false;
            d->armed--;
            cexec_waker_wake(s->read_waker); /* consumes the clone */
            progressed = true;
        }
        if (s->write_armed && (((e & (u32)EPOLLOUT) != 0) || err)) {
            s->write_armed = false;
            d->armed--;
            cexec_waker_wake(s->write_waker);
            progressed = true;
        }
        sched_update_interest(d, s); /* drop the just-fired directions */
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

/* ─── readable/writable(fd) futures ─────────────────────── */

struct interest_box {
    struct cstd_alloc       alloc;
    struct cexec_io_driver *driver;
    int                     fd;
    bool                    want_write; /* false = read */
    bool                    armed;      /* this future set its slot */
};

static cexec_poll interest_poll(void *self, struct cexec_waker w, void *out) {
    (void)out;
    struct interest_box *b = self;
    struct io_sched *s = find_sched(b->driver, b->fd);
    if (!s) {
        /* fd not registered: resolve so the caller does the real
         * syscall and observes the error itself. */
        return cexec_poll_ready();
    }
    bool *armed = b->want_write ? &s->write_armed : &s->read_armed;
    struct cexec_waker *slot = b->want_write ? &s->write_waker : &s->read_waker;

    if (!b->armed) {
        *slot  = cexec_waker_clone(w);
        *armed = true;
        b->driver->armed++;
        sched_update_interest(b->driver, s);
        b->armed = true;
        return cexec_poll_pending();
    }
    /* The park clears the slot's armed flag when it fires it. */
    return *armed ? cexec_poll_pending() : cexec_poll_ready();
}

static void interest_drop(void *self) {
    struct interest_box *b = self;
    if (b->armed) {
        struct io_sched *s = find_sched(b->driver, b->fd);
        if (s) {
            bool *armed = b->want_write ? &s->write_armed : &s->read_armed;
            if (*armed) {
                /* dropped before firing (e.g. a select loser) —
                 * cancel our slot and stop watching it. */
                struct cexec_waker *slot =
                    b->want_write ? &s->write_waker : &s->read_waker;
                *armed = false;
                b->driver->armed--;
                cexec_waker_drop(*slot);
                sched_update_interest(b->driver, s);
            }
        }
    }
    cstd_alloc_deallocate(b->alloc, b, sizeof *b, _Alignof(struct interest_box));
}

static const struct cexec_future_vtable interest_vtable = {
    .poll = interest_poll,
    .drop = interest_drop,
};

static struct cexec_future interest_new(struct cstd_alloc a,
                                        struct cexec_io_driver *d,
                                        int fd, bool want_write) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct interest_box *b = cstd_alloc_allocate(a, sizeof *b,
                                                 _Alignof(struct interest_box));
    if (!b) {
        return f;
    }
    b->alloc      = a;
    b->driver     = d;
    b->fd         = fd;
    b->want_write = want_write;
    b->armed      = false;
    f.self        = b;
    f.vtable      = &interest_vtable;
    return f;
}

/* Resolve when `fd` is readable (EPOLLIN, or err/hup). `fd` must
 * be registered via cexec_io_add. Unit output — the caller does
 * the real non-blocking read and re-awaits on EAGAIN. */
[[cust::pub]] struct cexec_future cexec_readable_in(struct cstd_alloc a,
                                                    struct cexec_io_driver *d,
                                                    int fd) {
    return interest_new(a, d, fd, false);
}

/* Resolve when `fd` is writable (EPOLLOUT, or err/hup) — also
 * how a non-blocking connect() completes. `fd` must be
 * registered via cexec_io_add. Unit output. */
[[cust::pub]] struct cexec_future cexec_writable_in(struct cstd_alloc a,
                                                    struct cexec_io_driver *d,
                                                    int fd) {
    return interest_new(a, d, fd, true);
}

/* ─── unit tests ────────────────────────────────────────── */

#cust use crate::combinators;

#include <fcntl.h>
#include <sys/socket.h>

#define MS 1000000ull

/* A waker that does nothing — enough to arm a slot in a test
 * that never lets the driver fire. */
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

static void set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* Build a non-blocking pipe; returns false on failure. */
static bool make_pipe(int *rfd, int *wfd) {
    int fds[2];
    if (pipe(fds) != 0) {
        return false;
    }
    set_nonblock(fds[0]);
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
    cust_assert(cexec_io_add(&io, rfd));

    /* Make the read end ready before we block (single thread). */
    cust_assert(write(wfd, "x", 1) == 1);

    bool done = cexec_executor_block_on(&ex, cexec_readable_in(a, &io, rfd),
                                        (void *)0);
    cust_assert(done);

    char buf = 0;
    cust_assert(read(rfd, &buf, 1) == 1);
    cust_assert(buf == 'x');

    cexec_io_remove(&io, rfd);
    close(rfd);
    close(wfd);
    cexec_io_driver_close(&io);
    return 0;
}

[[cust::test]] int test_writable_ready_immediately(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    int sv[2];
    cust_assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    set_nonblock(sv[0]);
    cust_assert(cexec_io_add(&io, sv[0]));

    /* A fresh socket has empty send buffers → writable at once. */
    cust_assert(cexec_executor_block_on(&ex, cexec_writable_in(a, &io, sv[0]),
                                        (void *)0));

    cexec_io_remove(&io, sv[0]);
    close(sv[0]);
    close(sv[1]);
    cexec_io_driver_close(&io);
    return 0;
}

[[cust::test]] int test_full_duplex_same_fd(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_io_driver io;
    cust_assert(cexec_io_driver_init(&io, a));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_io_driver_as_park(&io));

    int sv[2];
    cust_assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    set_nonblock(sv[0]);
    set_nonblock(sv[1]);
    cust_assert(cexec_io_add(&io, sv[0]));

    /* Make sv[0] readable too, then await BOTH readable and
     * writable on the SAME fd at once — two slots on one
     * registration, which the per-future ADD model could not do
     * (EEXIST). */
    cust_assert(write(sv[1], "z", 1) == 1);
    struct cexec_future f =
        cexec_join2_in(a, cexec_readable_in(a, &io, sv[0]), 0,
                       cexec_writable_in(a, &io, sv[0]));
    cust_assert(cexec_executor_block_on(&ex, f, (void *)0));

    char buf = 0;
    cust_assert(read(sv[0], &buf, 1) == 1);
    cust_assert(buf == 'z');

    cexec_io_remove(&io, sv[0]);
    close(sv[0]);
    close(sv[1]);
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
    cust_assert(cexec_io_add(&io, rfd));
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

    cexec_io_remove(&io, rfd);
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
    cust_assert(cexec_io_add(&io, rfd));

    /* No data written: the fd never becomes readable. Arm a
     * readable future by polling once, then drop it; the driver
     * must end with no armed slots. */
    struct cexec_future f = cexec_readable_in(a, &io, rfd);
    cexec_poll st = cexec_future_poll(f, io_noop_waker(), (void *)0);
    cust_assert(!cexec_poll_is_ready(st));
    cust_assert(io.armed == 1);
    cexec_future_drop(f);
    cust_assert(io.armed == 0);

    cexec_io_remove(&io, rfd);
    close(rfd);
    close(wfd);
    cexec_io_driver_close(&io);
    return 0;
}
