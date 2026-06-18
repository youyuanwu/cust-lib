/* cust_execution — a Rust-style async runtime for C.
 *
 * Brings Rust's `core::future` / `core::task` model to cust:
 * a poll-based `Future`, a type-erased `Waker`, and a small
 * single-threaded executor that drives them. The pieces are
 * plain data + vtables — no compiler magic — so they slot under
 * the same `cstd_alloc` / `std_list` primitives the std crate
 * already ships.
 *
 * Layout:
 *   src/lib.c       — crate root; declares submodules + version
 *   src/future.c    — cexec_poll / cexec_waker / cexec_future
 *                     vtables + leaf futures (ready, yield_now)
 *   src/executor.c  — cexec_executor: ready queue, spawn,
 *                     run, block_on
 *   src/combinators.c — map / then / join2 / select2:
 *                     compose futures into trees
 *   src/reactor.c   — cexec_reactor: timer wake source + sleep
 *                     future + mockable clock (a `park` impl)
 *   src/runtime.c   — cexec_runtime: executor + reactor bundled
 *                     (the batteries-included default)
 *
 * What's here vs. Rust:
 *   - poll + waker + future + executor: faithful.
 *   - `async`/`await`: NOT here — C has no state-machine
 *     transform. Multi-step logic is hand-written as a future
 *     whose `poll` switches on its own progress state, or built
 *     from combinators over these leaves.
 *   - `Pin`: NOT here — "don't move a future after first poll"
 *     is a convention (heap-box and pass the fat pointer).
 */

#cust use std;

#cust mod future;
#cust mod executor;
#cust mod combinators;
#cust mod reactor;
#cust mod runtime;

/* The cust_execution major/minor/patch this crate was authored
 * against. */
[[cust::pub]] u32 cexec_version(void) {
    return (0u << 16) | (1u << 8) | 0u; /* 0.1.0 */
}
