/* std::arc — thread-safe kref-shaped refcount.
 *
 * Atomic sibling of `std::rc` — same `init` / `get` / `put`
 * shape, same "embed in your type + recover via offsetof"
 * consumer pattern, but the counter is `_Atomic u32` and the
 * operations use the Rust-`Arc<T>` memory orderings:
 *
 *   _get  → Relaxed RMW    (we already hold a ref; the
 *                           increment doesn't synchronize
 *                           anything)
 *   _put  → Release RMW    (publishes prior writes to whoever
 *                           observes the count drop to 0)
 *         + Acquire fence   (the dropper synchronises with all
 *           on drop path    prior Release decrements before
 *                           tearing the object down)
 *
 * The fence-on-the-drop-path trick (instead of `AcqRel` on
 * every put) means uncontended puts pay only the cheaper
 * Release cost; the Acquire only fires when destruction
 * actually happens. That's exactly what `std::sync::Arc` does.
 *
 * Consumer pattern (mirrors `std::rc` exactly, just with this
 * struct in place of `struct std_rc`):
 *
 *   struct my_arc_thing {
 *       struct std_arc     arc;
 *       struct cstd_alloc   alloc;
 *       ... payload ...
 *   };
 *
 *   static void my_arc_thing_release(struct std_arc *a) {
 *       struct my_arc_thing *t = (struct my_arc_thing *)
 *           ((u8 *)a - offsetof(struct my_arc_thing, arc));
 *       cstd_alloc_deallocate(t->alloc, t,
 *                            sizeof *t, _Alignof(*t));
 *   }
 */

#cust use crate::types;

#include <stdatomic.h>

struct [[cust::pub_repr]] std_arc {
    _Atomic u32 count;
};

[[cust::pub]] void std_arc_init(struct std_arc *a) {
    /* Relaxed: the object is not yet shared. The handoff to
     * a second owner happens via a separate synchronization
     * (whatever channel passes the pointer), which provides
     * its own ordering. */
    atomic_store_explicit(&a->count, 1, memory_order_relaxed);
}

[[cust::pub]] void std_arc_get(struct std_arc *a) {
    /* Relaxed: the caller already holds a reference, so the
     * increment doesn't establish happens-before with anything
     * the new owner will read; we're just bumping a count
     * nobody else can drop to zero while we hold ours. */
    atomic_fetch_add_explicit(&a->count, 1, memory_order_relaxed);
}

/* Decrement. Returns true iff this call dropped the count to
 * zero, in which case `release(a)` has been invoked and the
 * surrounding object is gone. Precondition: count > 0.
 *
 * Ordering:
 *   - The decrement itself is `Release` so any writes this thread
 *     made to the object (mutations through its ref) happen-before
 *     a remote destructor that observes the zero.
 *   - On the drop path we issue an `Acquire` thread fence so the
 *     destructor synchronises with EVERY prior Release decrement
 *     across the whole graph of past owners — not just the
 *     immediately-preceding one. Without this, prior owners'
 *     last writes could be reordered past the destructor's reads.
 */
[[cust::pub]] bool std_arc_put(struct std_arc *a,
                               void (*release)(struct std_arc *)) {
    u32 old = atomic_fetch_sub_explicit(&a->count, 1, memory_order_release);
    if (old == 1) {
        atomic_thread_fence(memory_order_acquire);
        release(a);
        return true;
    }
    return false;
}

/* Snapshot read. As in `std::rc`, mostly for debugging/tests
 * — production code should not branch on this; the right
 * question is "did this `_put` destroy the object", which
 * `std_arc_put` already returns. Uses `Relaxed` because no
 * ordering with anything else is being established. */
[[cust::pub]] u32 std_arc_count(const struct std_arc *a) {
    return atomic_load_explicit(&a->count, memory_order_relaxed);
}

/* ─── unit tests ──────────────────────────────────────────
 *
 * Single-threaded only. The orderings above are written to the
 * model spec; verifying them under contention needs TSan + a
 * multi-thread harness, which std doesn't host today. The
 * single-threaded tests still catch arithmetic / sign /
 * return-value bugs and act as living documentation of the API.
 */

[[cust::test]] int test_arc_init_starts_at_one(void) {
    struct std_arc a;
    std_arc_init(&a);
    cust_assert_eq(std_arc_count(&a), (u32)1);
    return 0;
}

static i32 g_arc_dummy_release_count = 0;
static void arc_dummy_release(struct std_arc *a) {
    (void)a;
    g_arc_dummy_release_count++;
}

[[cust::test]] int test_arc_get_put_invokes_release_once(void) {
    struct std_arc a;
    std_arc_init(&a);
    std_arc_get(&a);
    std_arc_get(&a);
    cust_assert_eq(std_arc_count(&a), (u32)3);

    i32 before = g_arc_dummy_release_count;
    cust_assert(!std_arc_put(&a, arc_dummy_release));   /* 2 */
    cust_assert(!std_arc_put(&a, arc_dummy_release));   /* 1 */
    cust_assert_eq(g_arc_dummy_release_count, before);
    cust_assert(std_arc_put(&a, arc_dummy_release));    /* 0 — fires */
    cust_assert_eq(g_arc_dummy_release_count, before + 1);
    return 0;
}

/* End-to-end pattern — same shape as the rc test, atomic
 * variant. Lifecycle correctness alone (no contention). */

#include <stddef.h>    /* offsetof for the consumer recovery */

#cust use crate::alloc;

struct arc_blob {
    struct std_arc     arc;
    struct cstd_alloc   alloc;
    u32                payload;
};

static void arc_blob_release(struct std_arc *a) {
    struct arc_blob *b = (struct arc_blob *)
        ((u8 *)a - offsetof(struct arc_blob, arc));
    cstd_alloc_deallocate(b->alloc, b,
                         sizeof *b, _Alignof(struct arc_blob));
}

static struct arc_blob *arc_blob_new_in(u32 payload, struct cstd_alloc a) {
    struct arc_blob *b =
        cstd_alloc_allocate(a, sizeof *b, _Alignof(struct arc_blob));
    if (!b) return (struct arc_blob *)0;
    std_arc_init(&b->arc);
    b->alloc   = a;
    b->payload = payload;
    return b;
}
static struct arc_blob *arc_blob_get(struct arc_blob *b) {
    std_arc_get(&b->arc);
    return b;
}
static bool arc_blob_put(struct arc_blob *b) {
    if (!b) return false;
    return std_arc_put(&b->arc, arc_blob_release);
}

static i32 g_arc_acct_live = 0;

static void *arc_acct_allocate(void *st, usize size, usize align) {
    (void)st;
    void *p = cstd_alloc_allocate(cstd_alloc_system(), size, align);
    if (p) g_arc_acct_live++;
    return p;
}
static void arc_acct_deallocate(void *st, void *ptr, usize size, usize align) {
    (void)st;
    if (ptr) g_arc_acct_live--;
    cstd_alloc_deallocate(cstd_alloc_system(), ptr, size, align);
}
static void *arc_acct_reallocate(void *st, void *ptr,
                                 usize old_size, usize new_size, usize align) {
    (void)st;
    return cstd_alloc_reallocate(cstd_alloc_system(), ptr,
                                old_size, new_size, align);
}
static const struct cstd_alloc_vtable arc_acct_vtable = {
    .allocate   = arc_acct_allocate,
    .deallocate = arc_acct_deallocate,
    .reallocate = arc_acct_reallocate,
};
static struct cstd_alloc arc_acct_alloc(void) {
    struct cstd_alloc a;
    a.vtable = &arc_acct_vtable;
    a.state  = (void *)0;
    return a;
}

[[cust::test]] int test_arc_end_to_end_lifecycle(void) {
    i32 before = g_arc_acct_live;

    struct arc_blob *a = arc_blob_new_in(7, arc_acct_alloc());
    cust_assert(a != (struct arc_blob *)0);
    cust_assert_eq(g_arc_acct_live, before + 1);

    struct arc_blob *b = arc_blob_get(a);
    struct arc_blob *c = arc_blob_get(a);
    cust_assert(b == a && c == a);
    cust_assert_eq(std_arc_count(&a->arc), (u32)3);

    cust_assert(!arc_blob_put(b));
    cust_assert_eq(g_arc_acct_live, before + 1);
    cust_assert(!arc_blob_put(c));
    cust_assert_eq(g_arc_acct_live, before + 1);
    cust_assert_eq(std_arc_count(&a->arc), (u32)1);

    cust_assert(arc_blob_put(a));
    cust_assert_eq(g_arc_acct_live, before);
    return 0;
}
