/* std::rc — single-threaded kref-shaped refcount.
 *
 * Models the Linux kernel's `kref` API in C23: a named struct
 * holding only the counter, plus `init` / `get` / `put` where
 * `put` takes the release callback as a function pointer. The
 * struct itself carries no payload and no allocator — consumers
 * embed `struct std_rc` as a field in their own type and
 * recover the owning struct with offset arithmetic.
 *
 *   struct my_thing {
 *       struct std_rc      rc;
 *       struct cstd_alloc   alloc;
 *       ... payload ...
 *   };
 *
 *   static void my_thing_release(struct std_rc *r) {
 *       struct my_thing *t = (struct my_thing *)
 *           ((u8 *)r - offsetof(struct my_thing, rc));
 *       cstd_alloc_deallocate(t->alloc, t,
 *                            sizeof *t, _Alignof(struct my_thing));
 *   }
 *
 *   struct my_thing *my_thing_new_in(struct cstd_alloc a) {
 *       struct my_thing *t = cstd_alloc_allocate(a, sizeof *t,
 *                                               _Alignof(*t));
 *       if (!t) return (void *)0;
 *       std_rc_init(&t->rc);
 *       t->alloc = a;
 *       return t;
 *   }
 *   struct my_thing *my_thing_get(struct my_thing *t) {
 *       std_rc_get(&t->rc);
 *       return t;
 *   }
 *   void my_thing_put(struct my_thing *t) {
 *       if (t) std_rc_put(&t->rc, my_thing_release);
 *   }
 *
 * Why pass `release` to `put` instead of storing it in the
 * struct? Same reasons the kernel does:
 *   - one word saved per refcounted object;
 *   - the put site syntactically documents what destruction
 *     means for this type;
 *   - the same struct can pick different release policies at
 *     different sites (rare but legal).
 *
 * Thread safety: NONE. For shared-across-threads refcounting
 * use `std::arc` — same API shape with `_Atomic` count and
 * Arc/refcount_t memory orderings.
 *
 * Cardinal rule (also from kref): you may only call `_get` if
 * you already hold a ref. There is no "tryget" / 0→1
 * resurrection primitive; once the count reaches zero, `release`
 * is racing any would-be new owner. If you need weak refs,
 * build them on top with your own lock discipline.
 */

#cust use crate::types;

struct [[cust::pub_repr]] std_rc {
    u32 count;
};

[[cust::pub]] void std_rc_init(struct std_rc *r) {
    r->count = 1;
}

[[cust::pub]] void std_rc_get(struct std_rc *r) {
    r->count++;
}

/* Decrement. Returns true iff this call dropped the count to
 * zero, in which case `release(r)` has been invoked and the
 * surrounding object is gone. Precondition: count > 0. */
[[cust::pub]] bool std_rc_put(struct std_rc *r,
                              void (*release)(struct std_rc *)) {
    if (--r->count == 0) {
        release(r);
        return true;
    }
    return false;
}

/* Read the current count. Mostly useful for debugging / tests;
 * production code should not branch on this value — the right
 * question is "did this `_put` destroy the object", which
 * `std_rc_put` already returns. */
[[cust::pub]] u32 std_rc_count(const struct std_rc *r) {
    return r->count;
}

/* ─── unit tests ────────────────────────────────────────── */

[[cust::test]] int test_rc_init_starts_at_one(void) {
    struct std_rc r;
    std_rc_init(&r);
    cust_assert_eq(std_rc_count(&r), (u32)1);
    return 0;
}

/* `_put` against a trivial release callback that just records
 * the call. Confirms the boolean return value and that the
 * callback fires exactly once, on the final drop. */
static i32 g_dummy_release_count = 0;
static void dummy_release(struct std_rc *r) {
    (void)r;
    g_dummy_release_count++;
}

[[cust::test]] int test_rc_get_put_invokes_release_once(void) {
    struct std_rc r;
    std_rc_init(&r);
    std_rc_get(&r);
    std_rc_get(&r);
    cust_assert_eq(std_rc_count(&r), (u32)3);

    i32 before = g_dummy_release_count;
    cust_assert(!std_rc_put(&r, dummy_release));   /* 2 */
    cust_assert(!std_rc_put(&r, dummy_release));   /* 1 */
    cust_assert_eq(g_dummy_release_count, before); /* not yet */
    cust_assert(std_rc_put(&r, dummy_release));    /* 0 — fires */
    cust_assert_eq(g_dummy_release_count, before + 1);
    return 0;
}

/* End-to-end: a consumer type that embeds `struct std_rc` and
 * uses offsetof-arithmetic in its release callback. Exercises
 * the documented kref-style consumer pattern and verifies the
 * surrounding allocation is freed exactly when the last ref
 * drops. */

#include <stddef.h>    /* offsetof — needed by the consumer
                          recovery pattern */

#cust use crate::alloc;

struct rc_blob {
    struct std_rc      rc;
    struct cstd_alloc   alloc;
    u32                payload;
};

static void rc_blob_release(struct std_rc *r) {
    struct rc_blob *b = (struct rc_blob *)
        ((u8 *)r - offsetof(struct rc_blob, rc));
    cstd_alloc_deallocate(b->alloc, b,
                         sizeof *b, _Alignof(struct rc_blob));
}

static struct rc_blob *rc_blob_new_in(u32 payload, struct cstd_alloc a) {
    struct rc_blob *b =
        cstd_alloc_allocate(a, sizeof *b, _Alignof(struct rc_blob));
    if (!b) return (struct rc_blob *)0;
    std_rc_init(&b->rc);
    b->alloc   = a;
    b->payload = payload;
    return b;
}
static struct rc_blob *rc_blob_get(struct rc_blob *b) {
    std_rc_get(&b->rc);
    return b;
}
static bool rc_blob_put(struct rc_blob *b) {
    if (!b) return false;
    return std_rc_put(&b->rc, rc_blob_release);
}

/* Accounting allocator (same shape as the one in string/vec
 * tests) so we can prove the free happened exactly once. */

static i32 g_rc_acct_live = 0;

static void *rc_acct_allocate(void *st, usize size, usize align) {
    (void)st;
    void *p = cstd_alloc_allocate(cstd_alloc_system(), size, align);
    if (p) g_rc_acct_live++;
    return p;
}
static void rc_acct_deallocate(void *st, void *ptr, usize size, usize align) {
    (void)st;
    if (ptr) g_rc_acct_live--;
    cstd_alloc_deallocate(cstd_alloc_system(), ptr, size, align);
}
static void *rc_acct_reallocate(void *st, void *ptr,
                                usize old_size, usize new_size, usize align) {
    (void)st;
    return cstd_alloc_reallocate(cstd_alloc_system(), ptr,
                                old_size, new_size, align);
}

static const struct cstd_alloc_vtable rc_acct_vtable = {
    .allocate   = rc_acct_allocate,
    .deallocate = rc_acct_deallocate,
    .reallocate = rc_acct_reallocate,
};
static struct cstd_alloc rc_acct_alloc(void) {
    struct cstd_alloc a;
    a.vtable = &rc_acct_vtable;
    a.state  = (void *)0;
    return a;
}

[[cust::test]] int test_rc_end_to_end_lifecycle(void) {
    i32 before = g_rc_acct_live;

    struct rc_blob *a = rc_blob_new_in(42, rc_acct_alloc());
    cust_assert(a != (struct rc_blob *)0);
    cust_assert_eq(g_rc_acct_live, before + 1);

    struct rc_blob *b = rc_blob_get(a);
    struct rc_blob *c = rc_blob_get(a);
    cust_assert(b == a && c == a);
    cust_assert_eq(std_rc_count(&a->rc), (u32)3);

    /* Two out of three releases — storage must NOT be freed. */
    cust_assert(!rc_blob_put(b));
    cust_assert_eq(g_rc_acct_live, before + 1);
    cust_assert(!rc_blob_put(c));
    cust_assert_eq(g_rc_acct_live, before + 1);
    cust_assert_eq(std_rc_count(&a->rc), (u32)1);

    /* Final put frees. */
    cust_assert(rc_blob_put(a));
    cust_assert_eq(g_rc_acct_live, before);
    return 0;
}
