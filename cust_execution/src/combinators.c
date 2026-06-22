/* cust_execution::combinators — composing futures into trees.
 *
 * A combinator is *itself* a `cexec_future` that owns one or
 * more child futures and implements `poll` by polling those
 * children, forwarding the executor's waker straight down. This
 * is the manual, no-`async` form of Rust's `FutureExt`: where
 * Rust's compiler turns `a.await; b.await` into a state-machine
 * future, here you assemble the same machine by nesting these
 * nodes.
 *
 *   Rust (futures crate)        cust_execution
 *   ------------------------    -----------------------------
 *   fut.map(f)                  cexec_map_in
 *   a.then(|_| b) / a; b        cexec_then_in   (sequential)
 *   join(a, b)                  cexec_join2_in  (wait for both)
 *   select(a, b)                cexec_select2_in(first to finish)
 *   async fn { ... }            cexec_sm_in     (state-machine:
 *                                                sequence/if/loop)
 *
 * Three layers, one `cexec_future` type throughout:
 *
 *   leaf        ready / yield_now / your own poll fn
 *   combinator  map / then / join2 / select2   (this file)
 *   driver      cexec_executor (spawn / block_on)
 *
 * Ownership: every constructor *takes ownership* of the child
 * futures passed in (exactly like the executor takes ownership
 * on spawn). On allocation failure the constructor drops them
 * and returns the null future, so a child is never leaked.
 *
 * Correctness rules every combinator here obeys:
 *   1. Forward the executor's waker unchanged to children — a
 *      Pending combinator has always polled a child that took
 *      the waker, so the whole tree gets re-queued.
 *   2. Never poll a child after it returned Ready; drop it once
 *      it completes (or in the combinator's own `drop`).
 *   3. Drive synchronously across a stage change (see `then`)
 *      so finishing one child lets the next start in the same
 *      wake-up rather than stalling a round.
 *
 * Output channel: outputs flow through the same `out` pointer
 * the executor threads into `poll`, which is stable across
 * re-polls. Combinators that produce a value write it straight
 * into `out` (or a fixed sub-slice of it) — no internal output
 * buffering is needed.
 */

#cust use std;
#cust use crate::future;
#cust use crate::executor;

/* ─── map: transform the output in place ────────────────── */

/* In-place transform applied to a Ready output. The child and
 * the map share an output type/size — `fn` rewrites the bytes
 * the child produced. */
[[cust::pub]] typedef void (*cexec_map_fn)(void *out);

struct map_box {
    struct cstd_alloc   alloc;
    struct cexec_future inner;
    cexec_map_fn        fn;
};

static cexec_poll map_poll(void *self, struct cexec_waker w, void *out) {
    struct map_box *b = self;
    cexec_poll st = cexec_future_poll(b->inner, w, out);
    if (cexec_poll_is_ready(st) && b->fn && out) {
        b->fn(out);
    }
    return st;
}

static void map_drop(void *self) {
    struct map_box *b = self;
    cexec_future_drop(b->inner);
    cstd_alloc_deallocate(b->alloc, b, sizeof *b, _Alignof(struct map_box));
}

static const struct cexec_future_vtable map_vtable = {
    .poll = map_poll,
    .drop = map_drop,
};

/* Wrap `inner` so its Ready output is rewritten by `fn`. */
[[cust::pub]] struct cexec_future cexec_map_in(struct cstd_alloc a,
                                               struct cexec_future inner,
                                               cexec_map_fn fn) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct map_box *b = cstd_alloc_allocate(a, sizeof *b,
                                            _Alignof(struct map_box));
    if (!b) {
        cexec_future_drop(inner);
        return f;
    }
    b->alloc = a;
    b->inner = inner;
    b->fn    = fn;
    f.self   = b;
    f.vtable = &map_vtable;
    return f;
}

/* ─── then: run `a`, then `b`; yield b's output ─────────── */

/* The await-chain: `a.await; b.await` returning b. `a`'s output
 * is discarded (poll with out = NULL). `stage` is the hand-
 * rolled equivalent of the state Rust's compiler would emit. */
struct then_box {
    struct cstd_alloc   alloc;
    struct cexec_future a, b;
    u8                  stage; /* 0 = polling a, 1 = polling b */
};

static cexec_poll then_poll(void *self, struct cexec_waker w, void *out) {
    struct then_box *t = self;
    if (t->stage == 0) {
        cexec_poll st = cexec_future_poll(t->a, w, (void *)0);
        if (!cexec_poll_is_ready(st)) {
            return 0; /* still on a */
        }
        cexec_future_drop(t->a); /* a is spent */
        t->stage = 1;
        /* fall through: start b in the same wake-up */
    }
    return cexec_future_poll(t->b, w, out);
}

static void then_drop(void *self) {
    struct then_box *t = self;
    if (t->stage == 0) {
        cexec_future_drop(t->a); /* a not yet reached completion */
    }
    cexec_future_drop(t->b);
    cstd_alloc_deallocate(t->alloc, t, sizeof *t, _Alignof(struct then_box));
}

static const struct cexec_future_vtable then_vtable = {
    .poll = then_poll,
    .drop = then_drop,
};

/* Run `a` to completion (discarding its output), then run `b`
 * and yield b's output. */
[[cust::pub]] struct cexec_future cexec_then_in(struct cstd_alloc alloc,
                                                struct cexec_future a,
                                                struct cexec_future b) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct then_box *t = cstd_alloc_allocate(alloc, sizeof *t,
                                             _Alignof(struct then_box));
    if (!t) {
        cexec_future_drop(a);
        cexec_future_drop(b);
        return f;
    }
    t->alloc = alloc;
    t->a     = a;
    t->b     = b;
    t->stage = 0;
    f.self   = t;
    f.vtable = &then_vtable;
    return f;
}

/* ─── join2: run both, finish when both finish ──────────── */

/* Concurrent wait-all. Both children share the one waker, so
 * whichever is still Pending re-queues the whole join. Each
 * child's output lands in a fixed sub-slice of `out`: a at
 * offset 0, b at offset `a_size`. Completed children are
 * latched (`*_done`) so they are never re-polled. */
struct join_box {
    struct cstd_alloc   alloc;
    struct cexec_future a, b;
    usize               a_size; /* byte offset of b's output in `out` */
    bool                a_done, b_done;
};

static cexec_poll join_poll(void *self, struct cexec_waker w, void *out) {
    struct join_box *j = self;

    if (!j->a_done) {
        void *a_out = out;
        if (cexec_poll_is_ready(cexec_future_poll(j->a, w, a_out))) {
            j->a_done = true;
            cexec_future_drop(j->a);
        }
    }
    if (!j->b_done) {
        void *b_out = out ? (void *)((u8 *)out + j->a_size) : (void *)0;
        if (cexec_poll_is_ready(cexec_future_poll(j->b, w, b_out))) {
            j->b_done = true;
            cexec_future_drop(j->b);
        }
    }
    return (j->a_done && j->b_done) ? 1 : 0;
}

static void join_drop(void *self) {
    struct join_box *j = self;
    if (!j->a_done) {
        cexec_future_drop(j->a);
    }
    if (!j->b_done) {
        cexec_future_drop(j->b);
    }
    cstd_alloc_deallocate(j->alloc, j, sizeof *j, _Alignof(struct join_box));
}

static const struct cexec_future_vtable join_vtable = {
    .poll = join_poll,
    .drop = join_drop,
};

/* Run `a` and `b` concurrently; Ready once both are. `a_size`
 * is the size of a's output (the offset at which b's output is
 * written into the caller's `out`, which must be at least
 * a_size + sizeof(b's output)). */
[[cust::pub]] struct cexec_future cexec_join2_in(struct cstd_alloc alloc,
                                                 struct cexec_future a,
                                                 usize a_size,
                                                 struct cexec_future b) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct join_box *j = cstd_alloc_allocate(alloc, sizeof *j,
                                             _Alignof(struct join_box));
    if (!j) {
        cexec_future_drop(a);
        cexec_future_drop(b);
        return f;
    }
    j->alloc  = alloc;
    j->a      = a;
    j->b      = b;
    j->a_size = a_size;
    j->a_done = false;
    j->b_done = false;
    f.self    = j;
    f.vtable  = &join_vtable;
    return f;
}

/* ─── select2: race, first to finish wins ───────────────── */

/* Both arms must produce the same output type. The winner's
 * output lands in `out`; the loser is dropped untouched. (If a
 * caller needs to know which arm won, the arms encode it in
 * their own output.) `a` is polled first, so it wins ties. */
struct select_box {
    struct cstd_alloc   alloc;
    struct cexec_future a, b;
};

static cexec_poll select_poll(void *self, struct cexec_waker w, void *out) {
    struct select_box *s = self;
    if (cexec_poll_is_ready(cexec_future_poll(s->a, w, out))) {
        return 1;
    }
    if (cexec_poll_is_ready(cexec_future_poll(s->b, w, out))) {
        return 1;
    }
    return 0;
}

static void select_drop(void *self) {
    struct select_box *s = self;
    /* select goes Ready the instant either arm finishes and is
     * never polled again, so neither arm is freed during poll;
     * both boxes (the spent winner + the abandoned loser) are
     * released here exactly once. */
    cexec_future_drop(s->a);
    cexec_future_drop(s->b);
    cstd_alloc_deallocate(s->alloc, s, sizeof *s, _Alignof(struct select_box));
}

static const struct cexec_future_vtable select_vtable = {
    .poll = select_poll,
    .drop = select_drop,
};

/* Race `a` against `b`; Ready as soon as either is, yielding
 * the winner's output. */
[[cust::pub]] struct cexec_future cexec_select2_in(struct cstd_alloc alloc,
                                                   struct cexec_future a,
                                                   struct cexec_future b) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct select_box *s = cstd_alloc_allocate(alloc, sizeof *s,
                                               _Alignof(struct select_box));
    if (!s) {
        cexec_future_drop(a);
        cexec_future_drop(b);
        return f;
    }
    s->alloc = alloc;
    s->a     = a;
    s->b     = b;
    f.self   = s;
    f.vtable = &select_vtable;
    return f;
}
/* ─── sm: data-driven state machine (sequence / if / loop) ─ */

/* The stackless, macro-free answer to `async fn` control flow.
 * Where `then`/`map` only express a straight line, a state
 * machine expresses an arbitrary control-flow *graph*: each
 * state names its own successor, so plain C `if`s over a user-
 * owned context produce sequence, branch, and loop.
 *
 *   construct   how
 *   ---------   ------------------------------------------
 *   sequence    state returns next = the following index
 *   if / else   a no-I/O state returns one of two indices
 *   loop        a state returns a back-edge to an earlier one
 *   return      a state returns next = cexec_sm_done()
 *
 * The caller allocates and owns the *context* struct holding
 * every value that must live across a suspend (the cross-state
 * locals). Each state is an ordinary function that reads prior
 * results from the context, builds the next future, and says
 * (a) where that future's Ready output should land in the
 * context and (b) which state to run next. The driver below
 * owns only the poll/Pending/drop/advance plumbing — written
 * once here instead of by hand in every multi-step future.
 *
 * Output: the machine itself is a unit future (it produces no
 * aggregate value through `out` — results accumulate in the
 * caller's context), so `block_on(sm, NULL)`.
 *
 * Ownership: the machine owns each in-flight child future (it
 * drops it on completion or in its own drop) but NOT the
 * context, nor the `states` array — both outlive the machine
 * and are released by the caller. */

/* A no-I/O transition: `fut` is the null future, so the driver
 * runs no await and jumps straight to `next`. Used for pure
 * branch/decision states and for states whose only effect is
 * computing on the context. */

/* One state's decision: the future to await (or the null future
 * for a no-I/O transition), where its output lands in the
 * context, and the state to run once it completes. */
struct [[cust::pub_repr]] cexec_step {
    struct cexec_future fut;  /* null => no await, just go to `next` */
    void               *out;  /* where fut's output lands (in ctx), or NULL */
    u32                 next; /* state to run after fut; cexec_sm_done() ends */
};

/* A state function: given the caller's context, decide the next
 * step. Runs at the *start* of the state, so any prior state's
 * output is already in the context and available to branch on. */
[[cust::pub]] typedef struct cexec_step (*cexec_state_fn)(void *ctx);

/* The terminal "state": returning this as a step's `next` ends
 * the machine. Any index >= the state count also terminates. */
[[cust::pub]] u32 cexec_sm_done(void) { return 0xFFFFFFFFu; }

struct sm_box {
    struct cstd_alloc     alloc;
    void                 *ctx;     /* caller-owned cross-state locals */
    const cexec_state_fn *states;  /* caller-owned, length `nstates`  */
    usize                 nstates;
    u32                   state;   /* index of the state to (re)enter */
    struct cexec_future   cur;     /* in-flight child future          */
    void                 *cur_out; /* where cur's output lands         */
    u32                   cur_next;/* state to enter once cur is Ready */
    bool                  have_cur;/* cur built and not yet finished   */
};

static cexec_poll sm_poll(void *self, struct cexec_waker w, void *out) {
    (void)out; /* results live in the caller's context, not in `out` */
    struct sm_box *m = self;
    for (;;) {
        if (m->state >= m->nstates) { /* covers cexec_sm_done() */
            return cexec_poll_ready();
        }
        if (!m->have_cur) {
            struct cexec_step step = m->states[m->state](m->ctx);
            m->cur      = step.fut;
            m->cur_out  = step.out;
            m->cur_next = step.next;
            m->have_cur = true;
        }
        if (cexec_future_is_null(m->cur)) {
            /* no-I/O state: take the edge immediately, no poll */
            m->have_cur = false;
            m->state    = m->cur_next;
            continue;
        }
        if (!cexec_poll_is_ready(cexec_future_poll(m->cur, w, m->cur_out))) {
            return cexec_poll_pending();
        }
        cexec_future_drop(m->cur);
        m->have_cur = false;
        m->state    = m->cur_next;
        /* drive across the transition in this same wake-up */
    }
}

static void sm_drop(void *self) {
    struct sm_box *m = self;
    if (m->have_cur && !cexec_future_is_null(m->cur)) {
        cexec_future_drop(m->cur);
    }
    cstd_alloc_deallocate(m->alloc, m, sizeof *m, _Alignof(struct sm_box));
}

static const struct cexec_future_vtable sm_vtable = {
    .poll = sm_poll,
    .drop = sm_drop,
};

/* Run a state machine over `states` (length `nstates`), beginning
 * at `start`, threading results through the caller-owned `ctx`.
 * The machine does not own `ctx` or `states`; both must outlive
 * the returned future. Returns the null future on OOM. */
[[cust::pub]] struct cexec_future cexec_sm_in(struct cstd_alloc a, void *ctx,
                                              const cexec_state_fn *states,
                                              usize nstates, u32 start) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct sm_box *m = cstd_alloc_allocate(a, sizeof *m, _Alignof(struct sm_box));
    if (!m) {
        return f;
    }
    m->alloc    = a;
    m->ctx      = ctx;
    m->states   = states;
    m->nstates  = nstates;
    m->state    = start;
    m->cur_out  = (void *)0;
    m->cur_next = 0;
    m->have_cur = false;
    f.self      = m;
    f.vtable    = &sm_vtable;
    return f;
}
/* ─── unit tests ────────────────────────────────────────── */

/* A leaf that pends once (waking itself), then yields an i32 —
 * just enough latency to make join/select races meaningful. */
struct dv_box {
    struct cstd_alloc alloc;
    bool              fired;
    i32               val;
};

static cexec_poll dv_poll(void *self, struct cexec_waker w, void *out) {
    struct dv_box *b = self;
    if (!b->fired) {
        b->fired = true;
        cexec_waker_wake_by_ref(w);
        return cexec_poll_pending();
    }
    if (out) {
        *(i32 *)out = b->val;
    }
    return cexec_poll_ready();
}

static void dv_drop(void *self) {
    struct dv_box *b = self;
    cstd_alloc_deallocate(b->alloc, b, sizeof *b, _Alignof(struct dv_box));
}

static const struct cexec_future_vtable dv_vtable = {
    .poll = dv_poll,
    .drop = dv_drop,
};

static struct cexec_future dv_future(struct cstd_alloc a, i32 val) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct dv_box *b = cstd_alloc_allocate(a, sizeof *b, _Alignof(struct dv_box));
    if (!b) {
        return f;
    }
    b->alloc = a;
    b->fired = false;
    b->val   = val;
    f.self   = b;
    f.vtable = &dv_vtable;
    return f;
}

static void map_double(void *out) { *(i32 *)out *= 2; }

[[cust::test]] int test_map_transforms_output(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    i32 ten = 10, out = 0;
    struct cexec_future f =
        cexec_map_in(a, cexec_future_ready_in(a, &ten, sizeof ten), map_double);
    cust_assert(cexec_executor_block_on(&ex, f, &out));
    cust_assert_eq(out, 20);
    return 0;
}

[[cust::test]] int test_then_runs_in_order(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    /* a = yield_now (latency, unit output); b = ready(99). */
    i32 v = 99, out = 0;
    struct cexec_future f = cexec_then_in(a, cexec_yield_now_in(a),
                                          cexec_future_ready_in(a, &v, sizeof v));
    cust_assert(cexec_executor_block_on(&ex, f, &out));
    cust_assert_eq(out, 99);
    return 0;
}

[[cust::test]] int test_join2_waits_for_both(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    struct pair { i32 a, b; } out = {0, 0};
    /* a = delayed(11) (pends once), b = ready(22). join must
     * latch b's early result and wait a's second poll. */
    i32 twenty_two = 22;
    struct cexec_future f =
        cexec_join2_in(a, dv_future(a, 11), sizeof(i32),
                       cexec_future_ready_in(a, &twenty_two, sizeof twenty_two));
    cust_assert(cexec_executor_block_on(&ex, f, &out));
    cust_assert_eq(out.a, 11);
    cust_assert_eq(out.b, 22);
    return 0;
}

[[cust::test]] int test_select2_first_wins(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    /* a = delayed(7) (pends first), b = ready(9). b wins on the
     * first poll; a is dropped without ever completing. */
    i32 nine = 9, out = 0;
    struct cexec_future f =
        cexec_select2_in(a, dv_future(a, 7),
                         cexec_future_ready_in(a, &nine, sizeof nine));
    cust_assert(cexec_executor_block_on(&ex, f, &out));
    cust_assert_eq(out, 9);
    return 0;
}

[[cust::test]] int test_select2_prefers_a_on_tie(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    /* Both ready immediately; `a` is polled first and wins. */
    i32 five = 5, six = 6, out = 0;
    struct cexec_future f =
        cexec_select2_in(a, cexec_future_ready_in(a, &five, sizeof five),
                         cexec_future_ready_in(a, &six, sizeof six));
    cust_assert(cexec_executor_block_on(&ex, f, &out));
    cust_assert_eq(out, 5);
    return 0;
}

[[cust::test]] int test_combinators_nest(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    /* map(double) over the result of then(yield, ready(21)) -> 42. */
    i32 v = 21, out = 0;
    struct cexec_future inner =
        cexec_then_in(a, cexec_yield_now_in(a),
                      cexec_future_ready_in(a, &v, sizeof v));
    struct cexec_future f = cexec_map_in(a, inner, map_double);
    cust_assert(cexec_executor_block_on(&ex, f, &out));
    cust_assert_eq(out, 42);
    return 0;
}

/* ─── state-machine tests: sequence / if-else / loop ────── */

/* sequence: await x, await y, then compute sum in a no-I/O
 * state — outputs threaded through the caller-owned context. */
struct seq_ctx { struct cstd_alloc a; i32 x, y, sum; };
enum { Q_X, Q_Y, Q_SUM };

static struct cexec_step q_x(void *p) {
    struct seq_ctx *c = p;
    return (struct cexec_step){ dv_future(c->a, 3), &c->x, Q_Y };
}
static struct cexec_step q_y(void *p) {
    struct seq_ctx *c = p;
    return (struct cexec_step){ dv_future(c->a, 4), &c->y, Q_SUM };
}
static struct cexec_step q_sum(void *p) {
    struct seq_ctx *c = p;
    c->sum = c->x + c->y; /* both outputs are now in the context */
    return (struct cexec_step){ cexec_future_null(), (void *)0, cexec_sm_done() };
}
static const cexec_state_fn seq_states[] = { q_x, q_y, q_sum };

[[cust::test]] int test_sm_sequence(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    struct seq_ctx c = { a, 0, 0, 0 };
    struct cexec_future f = cexec_sm_in(a, &c, seq_states, 3, Q_X);
    cust_assert(cexec_executor_block_on(&ex, f, (void *)0));
    cust_assert_eq(c.sum, 7);
    return 0;
}

/* if/else: await a value, then a no-I/O decision state forks to
 * one of two terminal states by an ordinary C `if`. */
struct br_ctx { struct cstd_alloc a; i32 n, result; };
enum { B_LOAD, B_DECIDE, B_POS, B_NEG };

static struct cexec_step b_load(void *p) {
    struct br_ctx *c = p;
    return (struct cexec_step){ dv_future(c->a, c->n), &c->n, B_DECIDE };
}
static struct cexec_step b_decide(void *p) {
    struct br_ctx *c = p;
    u32 next = (c->n > 0) ? B_POS : B_NEG; /* the branch */
    return (struct cexec_step){ cexec_future_null(), (void *)0, next };
}
static struct cexec_step b_pos(void *p) {
    struct br_ctx *c = p;
    c->result = 1;
    return (struct cexec_step){ cexec_future_null(), (void *)0, cexec_sm_done() };
}
static struct cexec_step b_neg(void *p) {
    struct br_ctx *c = p;
    c->result = 2;
    return (struct cexec_step){ cexec_future_null(), (void *)0, cexec_sm_done() };
}
static const cexec_state_fn br_states[] = { b_load, b_decide, b_pos, b_neg };

static i32 run_branch(struct cstd_alloc a, i32 input) {
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);
    struct br_ctx c = { a, input, 0 };
    cexec_executor_block_on(&ex, cexec_sm_in(a, &c, br_states, 4, B_LOAD),
                            (void *)0);
    return c.result;
}

[[cust::test]] int test_sm_if_else(void) {
    struct cstd_alloc a = cstd_alloc_system();
    cust_assert_eq(run_branch(a, 5), 1);  /* positive -> B_POS */
    cust_assert_eq(run_branch(a, -2), 2); /* else     -> B_NEG */
    return 0;
}

/* loop: sum 1..=N by a back-edge. L_CHECK is the loop head
 * (continue or exit); L_LOAD awaits the next addend; L_ADD
 * accumulates and jumps back to the head. */
struct loop_ctx { struct cstd_alloc a; i32 i, n, cur, sum; };
enum { L_CHECK, L_LOAD, L_ADD };

static struct cexec_step l_check(void *p) {
    struct loop_ctx *c = p;
    u32 next = (c->i <= c->n) ? L_LOAD : cexec_sm_done();
    return (struct cexec_step){ cexec_future_null(), (void *)0, next };
}
static struct cexec_step l_load(void *p) {
    struct loop_ctx *c = p;
    return (struct cexec_step){ dv_future(c->a, c->i), &c->cur, L_ADD };
}
static struct cexec_step l_add(void *p) {
    struct loop_ctx *c = p;
    c->sum += c->cur;
    c->i   += 1;
    return (struct cexec_step){ cexec_future_null(), (void *)0, L_CHECK };
}
static const cexec_state_fn loop_states[] = { l_check, l_load, l_add };

[[cust::test]] int test_sm_loop(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    struct loop_ctx c = { a, 1, 5, 0, 0 };
    struct cexec_future f = cexec_sm_in(a, &c, loop_states, 3, L_CHECK);
    cust_assert(cexec_executor_block_on(&ex, f, (void *)0));
    cust_assert_eq(c.sum, 15); /* 1+2+3+4+5 */
    return 0;
}
