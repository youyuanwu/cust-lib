/* cust_execution::reactor — timer wake source (a `park` impl).
 *
 * The first *external* event source for the executor: a set of
 * pending timers plus a clock. When the executor runs out of
 * runnable tasks but timers are still outstanding, it calls the
 * reactor's `park`, which waits until the earliest deadline and
 * fires the wakers of every timer that has come due.
 *
 *   Rust/Tokio                    cust_execution
 *   --------------------------    ---------------------------
 *   time::Driver (timing wheel)   cexec_reactor (sorted list)
 *   Sleep / sleep(dur)            cexec_sleep_in
 *   Instant::now / Clock          cexec_clock (system | fake)
 *   park_timeout(next_deadline)   reactor_park (clock wait)
 *
 * Tokio uses a hierarchical timing wheel (O(1) arm/cancel) and
 * folds the timer wait into `epoll_wait`'s timeout so one
 * syscall serves timers *and* I/O. This is the stepping-stone
 * version: an intrusive list kept sorted by deadline (O(n) arm,
 * fine for modest timer counts) and a dedicated clock wait. The
 * `park` contract is identical, so an I/O driver or a timing
 * wheel can replace it without touching the executor.
 *
 * Clock indirection: the reactor reads time through a
 * `cexec_clock` vtable so tests can drive a *fake* clock that
 * advances instantly instead of sleeping — exactly Tokio's
 * `time::pause()` / `advance()`. Deterministic, no real waiting.
 *
 * Allocation: timers are intrusive — each `sleep` future embeds
 * its `cexec_timer` and links it directly into the reactor, so
 * arming a sleep costs no extra allocation beyond the future
 * box itself.
 *
 * Threading: NONE (matches the executor). A multi-threaded
 * driver would need a lock around the pending set and atomic
 * timer state.
 */

#cust use std;
#cust use crate::future;
#cust use crate::executor;

#include <stddef.h>
#include <time.h>
#include <errno.h>

/* ─── clock abstraction ─────────────────────────────────── */

struct [[cust::pub_repr]] cexec_clock_vtable {
    u64  (*now)(void *state);              /* monotonic nanoseconds */
    void (*wait_until)(void *state, u64 deadline_ns);
};

struct [[cust::pub_repr]] cexec_clock {
    const struct cexec_clock_vtable *vtable;
    void                            *state;
};

[[cust::pub]] u64 cexec_clock_now(struct cexec_clock c) {
    return c.vtable->now(c.state);
}

/* ─── system clock (CLOCK_MONOTONIC) ────────────────────── */

static u64 sys_now(void *st) {
    (void)st;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000ull + (u64)ts.tv_nsec;
}

static void sys_wait_until(void *st, u64 deadline_ns) {
    (void)st;
    struct timespec ts;
    ts.tv_sec  = (time_t)(deadline_ns / 1000000000ull);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ull);
    /* Absolute monotonic sleep; restart if a signal interrupts. */
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, (void *)0)
           == EINTR) {
        /* retry */
    }
}

static const struct cexec_clock_vtable sys_clock_vtable = {
    .now        = sys_now,
    .wait_until = sys_wait_until,
};

[[cust::pub]] struct cexec_clock cexec_clock_system(void) {
    struct cexec_clock c = {&sys_clock_vtable, (void *)0};
    return c;
}

/* ─── fake clock (deterministic tests) ──────────────────── */

struct [[cust::pub_repr]] cexec_fake_clock {
    u64 now_ns;
};

static u64 fake_now(void *st) {
    return ((struct cexec_fake_clock *)st)->now_ns;
}

/* "Waiting" on a fake clock just jumps time forward to the
 * deadline — no real sleep, so timer tests run instantly. */
static void fake_wait_until(void *st, u64 deadline_ns) {
    struct cexec_fake_clock *c = st;
    if (deadline_ns > c->now_ns) {
        c->now_ns = deadline_ns;
    }
}

static const struct cexec_clock_vtable fake_clock_vtable = {
    .now        = fake_now,
    .wait_until = fake_wait_until,
};

[[cust::pub]] void cexec_fake_clock_init(struct cexec_fake_clock *c) {
    c->now_ns = 0;
}

[[cust::pub]] struct cexec_clock cexec_clock_fake(struct cexec_fake_clock *c) {
    struct cexec_clock cc = {&fake_clock_vtable, c};
    return cc;
}

[[cust::pub]] void cexec_fake_clock_advance(struct cexec_fake_clock *c,
                                            u64 by_ns) {
    c->now_ns += by_ns;
}

[[cust::pub]] u64 cexec_fake_clock_now(const struct cexec_fake_clock *c) {
    return c->now_ns;
}

/* ─── timer entry (intrusive, embedded in a sleep future) ─ */

struct cexec_timer {
    struct std_list_head link;     /* membership in reactor->pending */
    u64                  deadline; /* monotonic ns */
    struct cexec_waker   waker;    /* cloned from the poll's waker */
    bool                 fired;    /* reactor already woke + unlinked it */
};

#define TIMER_OF(node) \
    ((struct cexec_timer *)((u8 *)(node) - offsetof(struct cexec_timer, link)))

/* ─── reactor ───────────────────────────────────────────── */

struct [[cust::pub_repr]] cexec_reactor {
    struct cstd_alloc    alloc;
    struct std_list_head pending; /* timers, kept sorted by deadline ↑ */
    struct cexec_clock   clock;
};

[[cust::pub]] void cexec_reactor_init(struct cexec_reactor *r,
                                      struct cstd_alloc a,
                                      struct cexec_clock clock) {
    r->alloc = a;
    std_list_init(&r->pending);
    r->clock = clock;
}

[[cust::pub]] u64 cexec_reactor_now(const struct cexec_reactor *r) {
    return r->clock.vtable->now(r->clock.state);
}

[[cust::pub]] bool cexec_reactor_is_idle(const struct cexec_reactor *r) {
    return std_list_is_empty(&r->pending);
}

/* Insert `t` keeping `pending` sorted by ascending deadline, so
 * the front is always the next timer to fire. */
static void reactor_insert(struct cexec_reactor *r, struct cexec_timer *t) {
    struct std_list_head *p = r->pending.next;
    while (p != &r->pending) {
        if (TIMER_OF(p)->deadline > t->deadline) {
            break;
        }
        p = p->next;
    }
    /* `add_tail(new, p)` links `new` directly before `p`. */
    std_list_add_tail(&t->link, p);
}

/* The `park` callback handed to the executor: wait for the
 * earliest deadline, then fire every timer now due. Returns
 * true if it woke ≥1 task. */
static bool reactor_park(void *state) {
    struct cexec_reactor *r = state;
    if (std_list_is_empty(&r->pending)) {
        return false; /* nothing to wait for */
    }

    struct cexec_timer *first = TIMER_OF(r->pending.next);
    r->clock.vtable->wait_until(r->clock.state, first->deadline);

    u64 now = r->clock.vtable->now(r->clock.state);
    bool progressed = false;
    while (!std_list_is_empty(&r->pending)) {
        struct cexec_timer *t = TIMER_OF(r->pending.next);
        if (t->deadline > now) {
            break; /* the rest are still in the future */
        }
        std_list_del(&t->link);
        t->fired = true;
        cexec_waker_wake(t->waker); /* consumes the cloned waker */
        progressed = true;
    }
    return progressed;
}

[[cust::pub]] struct cexec_park cexec_reactor_as_park(struct cexec_reactor *r) {
    struct cexec_park p;
    p.park  = reactor_park;
    p.state = r;
    return p;
}

/* ─── sleep future ──────────────────────────────────────── */

struct sleep_box {
    struct cstd_alloc     alloc;
    struct cexec_reactor *reactor;
    struct cexec_timer    timer;
    u64                   dur_ns;
    bool                  armed;
};

static cexec_poll sleep_poll(void *self, struct cexec_waker w, void *out) {
    (void)out;
    struct sleep_box *s = self;
    u64 now = cexec_reactor_now(s->reactor);

    if (!s->armed) {
        u64 deadline = now + s->dur_ns;
        if (now >= deadline) {
            return cexec_poll_ready(); /* zero/past duration: done now */
        }
        s->timer.deadline = deadline;
        s->timer.waker    = cexec_waker_clone(w);
        s->timer.fired    = false;
        reactor_insert(s->reactor, &s->timer);
        s->armed = true;
        return cexec_poll_pending();
    }

    if (now >= s->timer.deadline) {
        return cexec_poll_ready();
    }
    return cexec_poll_pending();
}

static void sleep_drop(void *self) {
    struct sleep_box *s = self;
    /* If still registered (dropped before firing — e.g. the
     * losing arm of a select), unlink from the reactor and
     * release the waker clone the reactor would otherwise own.
     * If already fired, the reactor unlinked it and consumed the
     * waker, so there is nothing to clean up. */
    if (s->armed && !s->timer.fired) {
        std_list_del(&s->timer.link);
        cexec_waker_drop(s->timer.waker);
    }
    cstd_alloc_deallocate(s->alloc, s, sizeof *s, _Alignof(struct sleep_box));
}

static const struct cexec_future_vtable sleep_vtable = {
    .poll = sleep_poll,
    .drop = sleep_drop,
};

/* A future that completes `dur_ns` nanoseconds after it is
 * first polled, driven by `r`'s clock via the executor's park
 * hook. Unit output. Returns the null future on OOM. */
[[cust::pub]] struct cexec_future cexec_sleep_in(struct cstd_alloc a,
                                                 struct cexec_reactor *r,
                                                 u64 dur_ns) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct sleep_box *s = cstd_alloc_allocate(a, sizeof *s,
                                              _Alignof(struct sleep_box));
    if (!s) {
        return f;
    }
    s->alloc   = a;
    s->reactor = r;
    s->dur_ns  = dur_ns;
    s->armed   = false;
    f.self     = s;
    f.vtable   = &sleep_vtable;
    return f;
}

/* ─── unit tests ────────────────────────────────────────── */

#define MS 1000000ull /* ns per millisecond */

/* A waker that does nothing — enough to arm a timer in tests
 * that never let the reactor fire. */
static void noop_wake(void *d) { (void)d; }
static struct cexec_waker noop_waker(void);
static struct cexec_waker noop_clone(void *d) { (void)d; return noop_waker(); }
static void noop_drop(void *d) { (void)d; }

static const struct cexec_waker_vtable noop_waker_vtable = {
    .wake        = noop_wake,
    .wake_by_ref = noop_wake,
    .clone       = noop_clone,
    .drop        = noop_drop,
};

static struct cexec_waker noop_waker(void) {
    struct cexec_waker w;
    w.data   = (void *)0;
    w.vtable = &noop_waker_vtable;
    return w;
}

[[cust::test]] int test_sleep_zero_is_immediate(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_fake_clock fc;
    cexec_fake_clock_init(&fc);
    struct cexec_reactor r;
    cexec_reactor_init(&r, a, cexec_clock_fake(&fc));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_reactor_as_park(&r));

    bool done = cexec_executor_block_on(&ex, cexec_sleep_in(a, &r, 0),
                                        (void *)0);
    cust_assert(done);
    cust_assert(cexec_reactor_is_idle(&r));
    cust_assert_eq((u64)cexec_fake_clock_now(&fc), 0ull); /* never waited */
    return 0;
}

[[cust::test]] int test_sleep_fake_clock_advances_to_deadline(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_fake_clock fc;
    cexec_fake_clock_init(&fc);
    struct cexec_reactor r;
    cexec_reactor_init(&r, a, cexec_clock_fake(&fc));
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    cexec_executor_set_park(&ex, cexec_reactor_as_park(&r));

    bool done = cexec_executor_block_on(&ex, cexec_sleep_in(a, &r, 5 * MS),
                                        (void *)0);
    cust_assert(done);
    cust_assert(cexec_reactor_is_idle(&r));
    /* park jumped the fake clock straight to the deadline. */
    cust_assert_eq((u64)cexec_fake_clock_now(&fc), (u64)(5 * MS));
    return 0;
}

[[cust::test]] int test_dropping_unfired_sleep_unregisters(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_fake_clock fc;
    cexec_fake_clock_init(&fc);
    struct cexec_reactor r;
    cexec_reactor_init(&r, a, cexec_clock_fake(&fc));

    /* Arm a sleep by polling once with a throwaway no-op waker,
     * then drop it: the reactor must end up empty (no dangling
     * pointer to the freed future). */
    struct cexec_future f = cexec_sleep_in(a, &r, 10 * MS);
    /* a no-op waker is enough — we never let the reactor fire */
    cexec_poll st = cexec_future_poll(f, noop_waker(), (void *)0);
    cust_assert(!cexec_poll_is_ready(st));
    cust_assert(!cexec_reactor_is_idle(&r)); /* one timer parked */
    cexec_future_drop(f);
    cust_assert(cexec_reactor_is_idle(&r));  /* unregistered on drop */
    return 0;
}
