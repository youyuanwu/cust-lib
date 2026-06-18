/* cust_execution::runtime — the batteries-included default.
 *
 * Bundles an executor with a timer reactor and wires the
 * executor's park hook to the reactor, the way `#[tokio::main]`
 * hands you a full `Runtime` rather than a bare scheduler. The
 * executor and reactor stay independent, reusable pieces; this
 * is just the convenient default that connects them so callers
 * can `block_on` a future that sleeps without assembling the
 * plumbing themselves.
 *
 *   Rust/Tokio              cust_execution
 *   --------------------    ---------------------------
 *   tokio::runtime::Runtime cexec_runtime
 *   Runtime::block_on       cexec_runtime_block_on
 *   tokio::spawn            cexec_runtime_spawn
 *   tokio::time::sleep      cexec_sleep_in(.., runtime_reactor(rt), ..)
 *
 * Move warning: the runtime registers a pointer to its own
 * embedded reactor in its embedded executor's park hook, so the
 * `cexec_runtime` value must not be relocated after init — hold
 * it by pointer.
 */

#cust use std;
#cust use crate::future;
#cust use crate::executor;
#cust use crate::reactor;
#cust use crate::combinators;

struct [[cust::pub_repr]] cexec_runtime {
    struct cexec_executor exec;
    struct cexec_reactor  reactor;
};

static void runtime_wire(struct cexec_runtime *rt, struct cstd_alloc a,
                         struct cexec_clock clock) {
    cexec_reactor_init(&rt->reactor, a, clock);
    cexec_executor_init(&rt->exec, a);
    cexec_executor_set_park(&rt->exec, cexec_reactor_as_park(&rt->reactor));
}

/* Default runtime: real monotonic clock (timers sleep for real). */
[[cust::pub]] void cexec_runtime_init(struct cexec_runtime *rt,
                                      struct cstd_alloc a) {
    runtime_wire(rt, a, cexec_clock_system());
}

/* Runtime on a caller-supplied clock — pass `cexec_clock_fake`
 * for deterministic, instant-advancing timer tests. */
[[cust::pub]] void cexec_runtime_init_with_clock(struct cexec_runtime *rt,
                                                 struct cstd_alloc a,
                                                 struct cexec_clock clock) {
    runtime_wire(rt, a, clock);
}

[[cust::pub]] struct cexec_reactor *
cexec_runtime_reactor(struct cexec_runtime *rt) {
    return &rt->reactor;
}

[[cust::pub]] struct cexec_executor *
cexec_runtime_executor(struct cexec_runtime *rt) {
    return &rt->exec;
}

[[cust::pub]] bool cexec_runtime_block_on(struct cexec_runtime *rt,
                                          struct cexec_future f, void *out) {
    return cexec_executor_block_on(&rt->exec, f, out);
}

[[cust::pub]] bool cexec_runtime_spawn(struct cexec_runtime *rt,
                                       struct cexec_future f) {
    return cexec_executor_spawn(&rt->exec, f);
}

[[cust::pub]] void cexec_runtime_run(struct cexec_runtime *rt) {
    cexec_executor_run(&rt->exec);
}

/* ─── unit tests ────────────────────────────────────────── */

#define MS 1000000ull /* ns per millisecond */

[[cust::test]] int test_runtime_block_on_sleep_fake(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_fake_clock fc;
    cexec_fake_clock_init(&fc);
    struct cexec_runtime rt;
    cexec_runtime_init_with_clock(&rt, a, cexec_clock_fake(&fc));

    struct cexec_future f =
        cexec_sleep_in(a, cexec_runtime_reactor(&rt), 8 * MS);
    cust_assert(cexec_runtime_block_on(&rt, f, (void *)0));
    cust_assert_eq((u64)cexec_fake_clock_now(&fc), (u64)(8 * MS));
    return 0;
}

[[cust::test]] int test_runtime_two_sleeps_overlap(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_fake_clock fc;
    cexec_fake_clock_init(&fc);
    struct cexec_runtime rt;
    cexec_runtime_init_with_clock(&rt, a, cexec_clock_fake(&fc));
    struct cexec_reactor *r = cexec_runtime_reactor(&rt);

    /* join2(sleep 10ms, sleep 20ms): the sleeps run concurrently,
     * so total elapsed is max(10,20)=20ms, not 30ms. Proves the
     * reactor fires them on one shared timeline rather than
     * serially. */
    struct cexec_future f =
        cexec_join2_in(a, cexec_sleep_in(a, r, 10 * MS), 0,
                       cexec_sleep_in(a, r, 20 * MS));
    cust_assert(cexec_runtime_block_on(&rt, f, (void *)0));
    cust_assert_eq((u64)cexec_fake_clock_now(&fc), (u64)(20 * MS));
    return 0;
}

[[cust::test]] int test_runtime_select_sleep_race(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_fake_clock fc;
    cexec_fake_clock_init(&fc);
    struct cexec_runtime rt;
    cexec_runtime_init_with_clock(&rt, a, cexec_clock_fake(&fc));
    struct cexec_reactor *r = cexec_runtime_reactor(&rt);

    /* select2(sleep 30ms, sleep 5ms): the shorter sleep wins; the
     * clock only advances to 5ms and the loser is dropped (its
     * timer must be unregistered, leaving the reactor idle). */
    struct cexec_future f =
        cexec_select2_in(a, cexec_sleep_in(a, r, 30 * MS),
                         cexec_sleep_in(a, r, 5 * MS));
    cust_assert(cexec_runtime_block_on(&rt, f, (void *)0));
    cust_assert_eq((u64)cexec_fake_clock_now(&fc), (u64)(5 * MS));
    cust_assert(cexec_reactor_is_idle(cexec_runtime_reactor(&rt)));
    return 0;
}

[[cust::test]] int test_runtime_real_clock_smoke(void) {
    struct cstd_alloc a = cstd_alloc_system();
    struct cexec_runtime rt;
    cexec_runtime_init(&rt, a); /* real monotonic clock */

    u64 start = cexec_reactor_now(cexec_runtime_reactor(&rt));
    struct cexec_future f =
        cexec_sleep_in(a, cexec_runtime_reactor(&rt), 2 * MS);
    cust_assert(cexec_runtime_block_on(&rt, f, (void *)0));
    u64 elapsed = cexec_reactor_now(cexec_runtime_reactor(&rt)) - start;
    cust_assert(elapsed >= 2 * MS); /* actually waited */
    return 0;
}
