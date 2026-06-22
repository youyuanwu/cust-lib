/* cust_execution::future — Rust's poll + waker + future, in C.
 *
 * This is the data-only half of Rust's async model. The three
 * concepts port to C directly; only `async`/`await` (the
 * compiler's state-machine transform) has no C analogue and
 * must be written by hand (switch-state machines) or supplied
 * by combinators built on top of these primitives.
 *
 *   Rust                              cust_execution
 *   ----------------------------      ---------------------------
 *   enum Poll<T> { Ready, Pending }   cexec_poll (u8; 0/1)
 *   trait Future { fn poll(..) }      struct cexec_future (vtable)
 *   struct Waker (clone/wake/drop)    struct cexec_waker (vtable)
 *   struct Context<'_>                (folded into the Waker arg)
 *
 * Poll contract:
 *   - `poll` returns READY exactly once. On READY it MAY write
 *     the output value to `out` (size agreed out-of-band by the
 *     concrete future; `out` may be NULL for unit futures).
 *   - On PENDING the future MUST have arranged to be woken
 *     later by calling `wake`/`wake_by_ref` on a clone of the
 *     waker it was handed (directly, or via a reactor). A
 *     future that returns PENDING without registering a waker
 *     stalls forever.
 *   - After READY, the future is spent: do not poll again.
 *     Call `cexec_future_drop` exactly once to release it.
 *
 * No `Pin`: C cannot forbid moves, so the rule is by
 * convention — heap-box a future and only pass it by the
 * `cexec_future` fat pointer; never memcpy a future after its
 * first poll.
 */

#cust use std;

#include <stddef.h>
#include <string.h>

/* ─── Poll ──────────────────────────────────────────────── */

/* A poll result: 0 == Pending, non-zero == Ready. Kept as a
 * plain byte (rather than an enum) so it crosses the module
 * boundary as an ordinary integer with a stable ABI. */
[[cust::pub]] typedef u8 cexec_poll;

[[cust::pub]] cexec_poll cexec_poll_pending(void) { return 0; }
[[cust::pub]] cexec_poll cexec_poll_ready(void)   { return 1; }
[[cust::pub]] bool cexec_poll_is_ready(cexec_poll p) { return p != 0; }

/* ─── Waker ─────────────────────────────────────────────── */

struct cexec_waker_vtable; /* fwd */

/* Type-erased handle the executor hands to a future so the
 * future (or a reactor holding a clone) can ask to be polled
 * again. `data` identifies the task; `vtable` is static. */
struct [[cust::pub_repr]] cexec_waker {
    void                            *data;
    const struct cexec_waker_vtable *vtable;
};

struct [[cust::pub_repr]] cexec_waker_vtable {
    /* Schedule the task and consume this waker. */
    void (*wake)(void *data);
    /* Schedule the task, leaving the waker usable. */
    void (*wake_by_ref)(void *data);
    /* Duplicate (for storing in a reactor). */
    struct cexec_waker (*clone)(void *data);
    /* Release a waker obtained from `clone`. */
    void (*drop)(void *data);
};

[[cust::pub]] void cexec_waker_wake(struct cexec_waker w) {
    w.vtable->wake(w.data);
}
[[cust::pub]] void cexec_waker_wake_by_ref(struct cexec_waker w) {
    w.vtable->wake_by_ref(w.data);
}
[[cust::pub]] struct cexec_waker cexec_waker_clone(struct cexec_waker w) {
    return w.vtable->clone(w.data);
}
[[cust::pub]] void cexec_waker_drop(struct cexec_waker w) {
    w.vtable->drop(w.data);
}

/* ─── Future ────────────────────────────────────────────── */

struct cexec_future_vtable; /* fwd */

/* Fat pointer: erased future state + its static vtable. The
 * empty future {NULL, NULL} is the failure sentinel returned by
 * the leaf constructors below on allocation failure. */
struct [[cust::pub_repr]] cexec_future {
    void                             *self;
    const struct cexec_future_vtable *vtable;
};

struct [[cust::pub_repr]] cexec_future_vtable {
    cexec_poll (*poll)(void *self, struct cexec_waker w, void *out);
    void       (*drop)(void *self);
};

[[cust::pub]] cexec_poll cexec_future_poll(struct cexec_future f,
                                           struct cexec_waker w,
                                           void *out) {
    return f.vtable->poll(f.self, w, out);
}

[[cust::pub]] void cexec_future_drop(struct cexec_future f) {
    if (f.vtable && f.vtable->drop) {
        f.vtable->drop(f.self);
    }
}

[[cust::pub]] bool cexec_future_is_null(struct cexec_future f) {
    return f.vtable == (void *)0;
}

/* The null future {NULL, NULL}: the OOM sentinel returned by
 * leaf/combinator constructors, and the "no work" marker some
 * combinators (e.g. the state machine's pure-branch nodes)
 * use. Never poll it; `cexec_future_drop` on it is a no-op. */
[[cust::pub]] struct cexec_future cexec_future_null(void) {
    struct cexec_future f = {(void *)0, (void *)0};
    return f;
}

/* ─── Leaf future: ready(value) ─────────────────────────── */

/* Immediately-Ready future carrying `size` bytes of payload
 * inline. Naturally-aligned values up to max_align_t only — the
 * box is aligned for its own header, which covers the common
 * scalar/pointer cases this runtime targets. */
struct ready_box {
    struct cstd_alloc alloc;
    usize             size;
    u8                bytes[];
};

static cexec_poll ready_poll(void *self, struct cexec_waker w, void *out) {
    (void)w;
    struct ready_box *b = self;
    if (out && b->size) {
        memcpy(out, b->bytes, b->size);
    }
    return 1;
}

static void ready_drop(void *self) {
    struct ready_box *b = self;
    cstd_alloc_deallocate(b->alloc, b,
                          sizeof(struct ready_box) + b->size,
                          _Alignof(struct ready_box));
}

static const struct cexec_future_vtable ready_vtable = {
    .poll = ready_poll,
    .drop = ready_drop,
};

/* Build a future that is Ready on first poll, yielding a copy
 * of `[value, value+size)`. Returns the null future on OOM. */
[[cust::pub]] struct cexec_future cexec_future_ready_in(struct cstd_alloc a,
                                                        const void *value,
                                                        usize size) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct ready_box *b = cstd_alloc_allocate(a,
                                              sizeof(struct ready_box) + size,
                                              _Alignof(struct ready_box));
    if (!b) {
        return f;
    }
    b->alloc = a;
    b->size  = size;
    if (value && size) {
        memcpy(b->bytes, value, size);
    }
    f.self   = b;
    f.vtable = &ready_vtable;
    return f;
}

/* ─── Leaf future: yield_now() ──────────────────────────── */

/* Cooperative yield: Pending on the first poll (after waking
 * itself so the executor re-queues it), Ready on the second.
 * The canonical demonstration that a Pending future is
 * responsible for arranging its own wake-up. Unit output. */
struct yield_box {
    struct cstd_alloc alloc;
    bool              yielded;
};

static cexec_poll yield_poll(void *self, struct cexec_waker w, void *out) {
    (void)out;
    struct yield_box *b = self;
    if (!b->yielded) {
        b->yielded = true;
        cexec_waker_wake_by_ref(w); /* ask to be polled again */
        return 0;                   /* Pending */
    }
    return 1;                       /* Ready */
}

static void yield_drop(void *self) {
    struct yield_box *b = self;
    cstd_alloc_deallocate(b->alloc, b,
                          sizeof(struct yield_box),
                          _Alignof(struct yield_box));
}

static const struct cexec_future_vtable yield_vtable = {
    .poll = yield_poll,
    .drop = yield_drop,
};

[[cust::pub]] struct cexec_future cexec_yield_now_in(struct cstd_alloc a) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct yield_box *b = cstd_alloc_allocate(a,
                                              sizeof(struct yield_box),
                                              _Alignof(struct yield_box));
    if (!b) {
        return f;
    }
    b->alloc   = a;
    b->yielded = false;
    f.self     = b;
    f.vtable   = &yield_vtable;
    return f;
}

/* ─── unit tests ────────────────────────────────────────── */

/* A waker that just counts how many times it was woken — the
 * minimum needed to observe the poll/wake protocol without a
 * full executor. */
struct wake_counter {
    i32 count;
};

static void wc_wake(void *d) { ((struct wake_counter *)d)->count++; }
static struct cexec_waker wc_make(struct wake_counter *r);
static struct cexec_waker wc_clone(void *d) { return wc_make(d); }
static void wc_noop(void *d) { (void)d; }

static const struct cexec_waker_vtable wc_vtable = {
    .wake        = wc_wake,
    .wake_by_ref = wc_wake,
    .clone       = wc_clone,
    .drop        = wc_noop,
};

static struct cexec_waker wc_make(struct wake_counter *r) {
    struct cexec_waker w;
    w.data   = r;
    w.vtable = &wc_vtable;
    return w;
}

[[cust::test]] int test_poll_helpers(void) {
    cust_assert(!cexec_poll_is_ready(cexec_poll_pending()));
    cust_assert(cexec_poll_is_ready(cexec_poll_ready()));
    return 0;
}

[[cust::test]] int test_ready_future_yields_value(void) {
    struct cstd_alloc a = cstd_alloc_system();
    i32 v = 42;
    struct cexec_future f = cexec_future_ready_in(a, &v, sizeof v);
    cust_assert(!cexec_future_is_null(f));

    struct wake_counter rec = {0};
    struct cexec_waker w = wc_make(&rec);
    i32 out = 0;
    cexec_poll st = cexec_future_poll(f, w, &out);

    cust_assert(cexec_poll_is_ready(st));
    cust_assert_eq(out, 42);
    cust_assert_eq(rec.count, 0); /* a ready future never wakes */
    cexec_future_drop(f);
    return 0;
}

[[cust::test]] int test_yield_now_pends_then_ready(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_future f = cexec_yield_now_in(a);
    cust_assert(!cexec_future_is_null(f));

    struct wake_counter rec = {0};
    struct cexec_waker w = wc_make(&rec);

    cexec_poll st1 = cexec_future_poll(f, w, (void *)0);
    cust_assert(!cexec_poll_is_ready(st1)); /* Pending */
    cust_assert_eq(rec.count, 1);           /* registered its wake-up */

    cexec_poll st2 = cexec_future_poll(f, w, (void *)0);
    cust_assert(cexec_poll_is_ready(st2));  /* Ready second time */
    cexec_future_drop(f);
    return 0;
}
