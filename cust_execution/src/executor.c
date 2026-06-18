/* cust_execution::executor — single-threaded, poll-driven run loop.
 *
 * The reactor-free half of a Rust runtime: a ready queue of
 * tasks and a loop that polls whatever is runnable. There is no
 * I/O reactor here — futures that return Pending must wake
 * themselves (e.g. `cexec_yield_now`) or be woken by some
 * external event source the caller drives. With nothing left to
 * wake, the loop drains and returns.
 *
 *   Rust tokio/async-std            cust_execution
 *   ----------------------------    ---------------------------
 *   Runtime / LocalSet              struct cexec_executor
 *   spawn(future)                   cexec_executor_spawn
 *   block_on(future)                cexec_executor_block_on
 *   task Waker                      task_waker_vtable (below)
 *
 * Task lifetime: a task is heap-boxed on spawn, lives while its
 * future is Pending, and is freed the instant the future
 * returns Ready. A Pending future therefore always has a live
 * task behind any waker it stored — waking a completed task
 * cannot happen because a completed future is never polled
 * again.
 *
 * The ready queue is the std crate's intrusive `std_list_head`
 * embedded in each task: enqueue/dequeue are O(1) and allocate
 * nothing beyond the task box itself.
 *
 * Threading: NONE. Single producer/consumer, one thread. A
 * multi-threaded executor would need an atomic, locked ready
 * queue and reference-counted wakers (std::arc shaped).
 */

#cust use std;
#cust use crate::future;

#include <stddef.h>

/* ─── park hook ─────────────────────────────────────────── */

/* The executor's single extension point. When the ready queue
 * empties but tasks are still live, the executor calls `park`
 * to wait for an *external* event source (a timer reactor, an
 * I/O driver, a cross-thread channel) to fire wakers. It must
 * return true if it queued at least one task — false means it
 * could make no progress, so the executor stops (a stall /
 * deadlock). A `{NULL, NULL}` park is pure-compute mode: the
 * executor stops as soon as nothing is runnable. The hook is
 * deliberately timer-agnostic so the same executor drives
 * timers, I/O, and a fake test clock without change. */
struct [[cust::pub_repr]] cexec_park {
    bool (*park)(void *state);
    void  *state;
};

/* ─── executor handle ───────────────────────────────────── */

struct [[cust::pub_repr]] cexec_executor {
    struct cstd_alloc    alloc;
    struct std_list_head ready; /* intrusive queue of runnable tasks */
    usize                live;  /* tasks not yet completed */
    struct cexec_park    park;  /* external wake source ({NULL,NULL}=none) */
};

[[cust::pub]] void cexec_executor_init(struct cexec_executor *ex,
                                       struct cstd_alloc a) {
    ex->alloc = a;
    std_list_init(&ex->ready);
    ex->live = 0;
    ex->park.park  = (void *)0;
    ex->park.state = (void *)0;
}

/* Install the external wake source driven when the executor
 * goes idle with work still outstanding. */
[[cust::pub]] void cexec_executor_set_park(struct cexec_executor *ex,
                                           struct cexec_park park) {
    ex->park = park;
}

[[cust::pub]] usize cexec_executor_live(const struct cexec_executor *ex) {
    return ex->live;
}

/* ─── task ──────────────────────────────────────────────── */

/* Private to the executor — never exported. We recover the
 * task from its `link` node via offsetof, so `link` need not be
 * first; keeping it first just makes the cast cheap. */
struct task {
    struct std_list_head   link;  /* ready-queue membership */
    struct cexec_future    fut;
    struct cexec_executor *exec;  /* back-ref for the waker */
    void                  *out;   /* where to write Ready output (or NULL) */
    bool                  *done;  /* optional completion flag (or NULL) */
    bool                   queued;/* currently in the ready queue? */
};

#define TASK_OF(node) \
    ((struct task *)((u8 *)(node) - offsetof(struct task, link)))

/* ─── task waker ────────────────────────────────────────── */

/* Waking a task = put it back on its executor's ready queue,
 * unless it is already there (idempotent within a poll cycle).
 * Wakers carry the bare task pointer; no refcount is needed
 * because the task outlives every waker referencing it. */
static void task_wake(void *data) {
    struct task *t = data;
    if (!t->queued) {
        t->queued = true;
        std_list_add_tail(&t->link, &t->exec->ready);
    }
}

static struct cexec_waker task_waker(struct task *t);
static struct cexec_waker task_waker_clone(void *data) {
    return task_waker(data);
}
static void task_waker_noop(void *data) { (void)data; }

static const struct cexec_waker_vtable task_waker_vtable = {
    .wake        = task_wake,
    .wake_by_ref = task_wake,
    .clone       = task_waker_clone,
    .drop        = task_waker_noop,
};

static struct cexec_waker task_waker(struct task *t) {
    struct cexec_waker w;
    w.data   = t;
    w.vtable = &task_waker_vtable;
    return w;
}

/* ─── scheduling ────────────────────────────────────────── */

static struct task *executor_push(struct cexec_executor *ex,
                                  struct cexec_future fut,
                                  void *out, bool *done) {
    struct task *t = cstd_alloc_allocate(ex->alloc, sizeof *t,
                                         _Alignof(struct task));
    if (!t) {
        return (void *)0;
    }
    t->fut    = fut;
    t->exec   = ex;
    t->out    = out;
    t->done   = done;
    t->queued = true;
    if (done) {
        *done = false;
    }
    std_list_add_tail(&t->link, &ex->ready);
    ex->live++;
    return t;
}

/* Spawn a fire-and-forget task. Returns false (and drops the
 * future) if the task box could not be allocated. */
[[cust::pub]] bool cexec_executor_spawn(struct cexec_executor *ex,
                                        struct cexec_future fut) {
    if (executor_push(ex, fut, (void *)0, (bool *)0)) {
        return true;
    }
    cexec_future_drop(fut);
    return false;
}

/* Drain runnable tasks, then park on the external wake source
 * when the queue empties, until no progress can be made. With
 * no parker installed this stops as soon as nothing is runnable
 * (pure-compute mode); with one it blocks for timers / I/O and
 * resumes when they fire wakers. */
static void executor_drive(struct cexec_executor *ex) {
    for (;;) {
        while (!std_list_is_empty(&ex->ready)) {
            struct std_list_head *node = ex->ready.next;
            struct task *t = TASK_OF(node);
            std_list_del(node);
            t->queued = false;

            struct cexec_waker w = task_waker(t);
            cexec_poll st = cexec_future_poll(t->fut, w, t->out);

            if (cexec_poll_is_ready(st)) {
                /* A future may wake its own task mid-poll (e.g.
                 * one arm of a select) and still complete via a
                 * sibling in the same poll. That re-queues this
                 * task, so unlink it before freeing or the next
                 * iteration would pop freed memory. */
                if (t->queued) {
                    std_list_del(&t->link);
                    t->queued = false;
                }
                if (t->done) {
                    *t->done = true;
                }
                cexec_future_drop(t->fut);
                ex->live--;
                cstd_alloc_deallocate(ex->alloc, t, sizeof *t,
                                      _Alignof(struct task));
            }
            /* else: Pending — parked until its waker re-queues it. */
        }

        /* Ready queue drained. */
        if (ex->live == 0) {
            return; /* everything finished */
        }
        if (!ex->park.park) {
            return; /* no wake source: nothing can re-queue a task */
        }
        if (!ex->park.park(ex->park.state)) {
            return; /* parker made no progress: stalled */
        }
        /* parker fired ≥1 waker → loop and drain the new work */
    }
}

/* Run all spawned tasks until the executor goes idle. */
[[cust::pub]] void cexec_executor_run(struct cexec_executor *ex) {
    executor_drive(ex);
}

/* Spawn `fut`, run the executor to idle, and report whether
 * `fut` completed (writing its output to `out`, which may be
 * NULL for unit futures). Other spawned tasks are driven too.
 * Returns false on OOM or if the future is still Pending when
 * the executor goes idle (i.e. it blocked on a wake that never
 * came). */
[[cust::pub]] bool cexec_executor_block_on(struct cexec_executor *ex,
                                           struct cexec_future fut,
                                           void *out) {
    bool done = false;
    if (!executor_push(ex, fut, out, &done)) {
        cexec_future_drop(fut);
        return false;
    }
    executor_drive(ex);
    return done;
}

/* ─── unit tests ────────────────────────────────────────── */

/* A leaf future with a side effect: bump a counter, then Ready.
 * Used to prove spawned tasks actually run. */
struct inc_box {
    struct cstd_alloc alloc;
    i32              *counter;
};

static cexec_poll inc_poll(void *self, struct cexec_waker w, void *out) {
    (void)w;
    (void)out;
    struct inc_box *b = self;
    (*b->counter)++;
    return cexec_poll_ready();
}

static void inc_drop(void *self) {
    struct inc_box *b = self;
    cstd_alloc_deallocate(b->alloc, b, sizeof *b, _Alignof(struct inc_box));
}

static const struct cexec_future_vtable inc_vtable = {
    .poll = inc_poll,
    .drop = inc_drop,
};

static struct cexec_future inc_future(struct cstd_alloc a, i32 *counter) {
    struct cexec_future f = {(void *)0, (void *)0};
    struct inc_box *b = cstd_alloc_allocate(a, sizeof *b,
                                            _Alignof(struct inc_box));
    if (!b) {
        return f;
    }
    b->alloc   = a;
    b->counter = counter;
    f.self     = b;
    f.vtable   = &inc_vtable;
    return f;
}

[[cust::test]] int test_block_on_ready_value(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    i32 v = 7;
    i32 out = 0;
    struct cexec_future f = cexec_future_ready_in(a, &v, sizeof v);
    bool done = cexec_executor_block_on(&ex, f, &out);

    cust_assert(done);
    cust_assert_eq(out, 7);
    cust_assert_eq((i32)cexec_executor_live(&ex), 0);
    return 0;
}

[[cust::test]] int test_block_on_yield_completes(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    struct cexec_future f = cexec_yield_now_in(a);
    bool done = cexec_executor_block_on(&ex, f, (void *)0);

    cust_assert(done);
    cust_assert_eq((i32)cexec_executor_live(&ex), 0);
    return 0;
}

[[cust::test]] int test_spawn_runs_all_tasks(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    i32 n = 0;
    cust_assert(cexec_executor_spawn(&ex, inc_future(a, &n)));
    cust_assert(cexec_executor_spawn(&ex, inc_future(a, &n)));
    cust_assert(cexec_executor_spawn(&ex, inc_future(a, &n)));

    cexec_executor_run(&ex);

    cust_assert_eq(n, 3);
    cust_assert_eq((i32)cexec_executor_live(&ex), 0);
    return 0;
}

[[cust::test]] int test_spawn_yield_interleaves(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_executor ex;
    cexec_executor_init(&ex, a);

    /* Two yielding tasks + run to idle: each pends once, gets
     * re-queued by its own waker, then completes. */
    cust_assert(cexec_executor_spawn(&ex, cexec_yield_now_in(a)));
    cust_assert(cexec_executor_spawn(&ex, cexec_yield_now_in(a)));
    cexec_executor_run(&ex);

    cust_assert_eq((i32)cexec_executor_live(&ex), 0);
    return 0;
}
