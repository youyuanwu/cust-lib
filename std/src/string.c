/* std::string — borrowed string view + owned growable string.
 *
 * Two types, each playing a distinct role (Rust's `&str` vs
 * `String`):
 *
 *   struct cstd_str    — { ptr, len } view. Cheap to pass.
 *                        Not NUL-terminated. The thing 95%
 *                        of read-only APIs should take.
 *   struct cstd_string — owning, growable, allocator-aware.
 *                        Carries its own `cstd_alloc` so the
 *                        caller doesn't have to thread the
 *                        allocator back through `free`.
 *
 * No vtable on `cstd_string` itself: the algorithm is fixed,
 * only the resource (the allocator) is pluggable. The single
 * indirection lives in `cstd_alloc.vtable`.
 *
 * Fallible allocation is in the type signature: every function
 * that may grow returns `bool` (false on OOM). No aborts, no
 * hidden global allocator.
 *
 * Bytes, not NUL: storage is explicit `len`-counted.
 * `push_byte(0)` is legal. Trailing NUL is materialised on
 * demand by `cstd_string_as_cstr` for C interop only.
 */

#cust use crate::types;
#cust use crate::alloc;

/* ─── borrowed view ─────────────────────────────────────── */

struct [[cust::pub_repr]] cstd_str {
    const u8 *data;   /* may be NULL iff len == 0 */
    usize     len;    /* bytes */
};

[[cust::pub]] struct cstd_str cstd_str_from_bytes(const u8 *p, usize n) {
    struct cstd_str r;
    r.data = p;
    r.len  = n;
    return r;
}

/* Length-counted view of a C string. NULL → empty. Walks the
 * input once to find the NUL; cost is unavoidable for the
 * conversion. */
[[cust::pub]] struct cstd_str cstd_str_from_cstr(const char *s) {
    struct cstd_str r;
    r.data = (const u8 *)s;
    if (!s) {
        r.len = 0;
        return r;
    }
    usize n = 0;
    while (s[n] != '\0') n++;
    r.len = n;
    return r;
}

[[cust::pub]] bool cstd_str_eq(struct cstd_str a, struct cstd_str b) {
    if (a.len != b.len) return false;
    for (usize i = 0; i < a.len; i++) {
        if (a.data[i] != b.data[i]) return false;
    }
    return true;
}

/* ─── owned, growable ───────────────────────────────────── */

struct [[cust::pub_repr]] cstd_string {
    u8                *data;   /* owned by `alloc`; layout (cap, 1) */
    usize              len;    /* bytes in use */
    usize              cap;    /* bytes allocated */
    struct cstd_alloc  alloc;  /* by value */
};

[[cust::pub]] struct cstd_string cstd_string_new_in(struct cstd_alloc a) {
    struct cstd_string s;
    s.data  = (u8 *)0;
    s.len   = 0;
    s.cap   = 0;
    s.alloc = a;
    return s;
}

/* Helper: round to a sensible new capacity that fits `need`,
 * doubling from 8 with overflow guard. Returns 0 if the
 * request cannot be represented. */
static usize next_cap(usize current, usize need) {
    usize new_cap = current < 8 ? 8 : current;
    while (new_cap < need) {
        if (new_cap > ((usize)-1) / 2) {
            /* Doubling would overflow; clamp to exact need. */
            return need;
        }
        new_cap *= 2;
    }
    return new_cap;
}

/* Ensure capacity for at least `extra` more bytes past `len`.
 * Returns false on OOM or on `len + extra` overflowing usize. */
[[cust::pub]] bool cstd_string_reserve(struct cstd_string *s, usize extra) {
    if (extra == 0) return true;
    if (s->cap - s->len >= extra) return true;
    if (extra > ((usize)-1) - s->len) return false;

    usize need    = s->len + extra;
    usize new_cap = next_cap(s->cap, need);

    void *p;
    if (s->data) {
        p = cstd_alloc_reallocate(s->alloc, s->data, s->cap, new_cap, 1);
    } else {
        p = cstd_alloc_allocate(s->alloc, new_cap, 1);
    }
    if (!p) return false;

    s->data = (u8 *)p;
    s->cap  = new_cap;
    return true;
}

[[cust::pub]] bool cstd_string_with_capacity_in(struct cstd_string *out,
                                                usize cap,
                                                struct cstd_alloc a) {
    out->data  = (u8 *)0;
    out->len   = 0;
    out->cap   = 0;
    out->alloc = a;
    if (cap == 0) return true;
    return cstd_string_reserve(out, cap);
}

[[cust::pub]] void cstd_string_free(struct cstd_string *s) {
    if (!s || !s->data) return;
    cstd_alloc_deallocate(s->alloc, s->data, s->cap, 1);
    s->data = (u8 *)0;
    s->len  = 0;
    s->cap  = 0;
}

[[cust::pub]] usize cstd_string_len(const struct cstd_string *s) {
    return s->len;
}

[[cust::pub]] usize cstd_string_capacity(const struct cstd_string *s) {
    return s->cap;
}

[[cust::pub]] struct cstd_str cstd_string_as_str(const struct cstd_string *s) {
    struct cstd_str r;
    r.data = s->data;
    r.len  = s->len;
    return r;
}

[[cust::pub]] bool cstd_string_push_byte(struct cstd_string *s, u8 b) {
    if (!cstd_string_reserve(s, 1)) return false;
    s->data[s->len++] = b;
    return true;
}

[[cust::pub]] bool cstd_string_push_str(struct cstd_string *s,
                                        struct cstd_str x) {
    if (x.len == 0) return true;
    if (!cstd_string_reserve(s, x.len)) return false;
    for (usize i = 0; i < x.len; i++) {
        s->data[s->len + i] = x.data[i];
    }
    s->len += x.len;
    return true;
}

[[cust::pub]] bool cstd_string_push_cstr(struct cstd_string *s,
                                         const char *cs) {
    return cstd_string_push_str(s, cstd_str_from_cstr(cs));
}

/* Materialise a trailing NUL (not counted in `len`) and return
 * the buffer for C interop. Reserves one extra byte; returns
 * NULL on OOM. Subsequent mutations may invalidate the pointer. */
[[cust::pub]] const char *cstd_string_as_cstr(struct cstd_string *s) {
    if (!cstd_string_reserve(s, 1)) return (const char *)0;
    s->data[s->len] = 0;
    return (const char *)s->data;
}

[[cust::pub]] void cstd_string_clear(struct cstd_string *s) {
    s->len = 0;
}

/* ─── unit tests ────────────────────────────────────────── */

[[cust::test]] int test_str_from_cstr_basic(void) {
    struct cstd_str v = cstd_str_from_cstr("hello");
    cust_assert_eq(v.len, (usize)5);
    cust_assert(v.data[0] == 'h');
    cust_assert(v.data[4] == 'o');
    return 0;
}

[[cust::test]] int test_str_from_cstr_null_is_empty(void) {
    struct cstd_str v = cstd_str_from_cstr((const char *)0);
    cust_assert_eq(v.len, (usize)0);
    return 0;
}

[[cust::test]] int test_str_eq(void) {
    struct cstd_str a = cstd_str_from_cstr("abc");
    struct cstd_str b = cstd_str_from_cstr("abc");
    struct cstd_str c = cstd_str_from_cstr("abd");
    struct cstd_str d = cstd_str_from_cstr("abcd");
    cust_assert(cstd_str_eq(a, b));
    cust_assert(!cstd_str_eq(a, c));
    cust_assert(!cstd_str_eq(a, d));
    return 0;
}

[[cust::test]] int test_string_new_and_free(void) {
    struct cstd_string s = cstd_string_new_in(cstd_alloc_system());
    cust_assert_eq(cstd_string_len(&s), (usize)0);
    cust_assert_eq(cstd_string_capacity(&s), (usize)0);
    cstd_string_free(&s);
    /* Free is idempotent on a zeroed value. */
    cstd_string_free(&s);
    return 0;
}

[[cust::test]] int test_string_push_byte_grows(void) {
    struct cstd_string s = cstd_string_new_in(cstd_alloc_system());
    for (i32 i = 0; i < 100; i++) {
        cust_assert(cstd_string_push_byte(&s, (u8)('a' + (i % 26))));
    }
    cust_assert_eq(cstd_string_len(&s), (usize)100);
    cust_assert(cstd_string_capacity(&s) >= (usize)100);
    cust_assert(s.data[0]  == 'a');
    cust_assert(s.data[25] == 'z');
    cust_assert(s.data[26] == 'a');
    cstd_string_free(&s);
    return 0;
}

[[cust::test]] int test_string_push_str_concat(void) {
    struct cstd_string s = cstd_string_new_in(cstd_alloc_system());
    cust_assert(cstd_string_push_cstr(&s, "hello"));
    cust_assert(cstd_string_push_cstr(&s, ", "));
    cust_assert(cstd_string_push_cstr(&s, "cstd"));
    cust_assert_eq(cstd_string_len(&s), (usize)11);
    struct cstd_str view = cstd_string_as_str(&s);
    cust_assert(cstd_str_eq(view, cstd_str_from_cstr("hello, cstd")));
    cstd_string_free(&s);
    return 0;
}

[[cust::test]] int test_string_as_cstr_nul_terminated(void) {
    struct cstd_string s = cstd_string_new_in(cstd_alloc_system());
    cust_assert(cstd_string_push_cstr(&s, "abc"));
    const char *cs = cstd_string_as_cstr(&s);
    cust_assert(cs != (const char *)0);
    cust_assert(cs[0] == 'a');
    cust_assert(cs[1] == 'b');
    cust_assert(cs[2] == 'c');
    cust_assert(cs[3] == '\0');
    /* len does NOT count the NUL. */
    cust_assert_eq(cstd_string_len(&s), (usize)3);
    cstd_string_free(&s);
    return 0;
}

[[cust::test]] int test_string_with_capacity_avoids_regrow(void) {
    struct cstd_string s;
    cust_assert(cstd_string_with_capacity_in(&s, 64, cstd_alloc_system()));
    cust_assert(cstd_string_capacity(&s) >= (usize)64);
    cust_assert_eq(cstd_string_len(&s), (usize)0);
    /* Capture pre-push capacity; pushing within budget must
     * not trigger a realloc. */
    usize cap_before = cstd_string_capacity(&s);
    u8 *data_before  = s.data;
    for (i32 i = 0; i < 64; i++) {
        cust_assert(cstd_string_push_byte(&s, (u8)'x'));
    }
    cust_assert_eq(cstd_string_capacity(&s), cap_before);
    cust_assert(s.data == data_before);
    cstd_string_free(&s);
    return 0;
}

[[cust::test]] int test_string_clear_keeps_capacity(void) {
    struct cstd_string s = cstd_string_new_in(cstd_alloc_system());
    cust_assert(cstd_string_push_cstr(&s, "hello"));
    usize cap = cstd_string_capacity(&s);
    cstd_string_clear(&s);
    cust_assert_eq(cstd_string_len(&s), (usize)0);
    cust_assert_eq(cstd_string_capacity(&s), cap);
    cstd_string_free(&s);
    return 0;
}

/* ─── [[gnu::cleanup]] interop ────────────────────────────
 *
 * std doesn't ship a `cust_cleanup` macro, so the user-facing
 * recipe is to spell the GNU attribute at the declaration
 * directly:
 *
 *     [[gnu::cleanup(cstd_string_free)]] struct cstd_string s =
 *         cstd_string_new_in(cstd_alloc_system());
 *
 * The signature of `cstd_string_free` is already what `cleanup`
 * wants (`void (*)(struct cstd_string *)`), so no thunk is
 * needed. The two tests below pin down the behaviour we promise
 * downstream consumers, against a tiny accounting allocator
 * that tracks net live allocations in a static counter.
 */

static i32 g_acct_live = 0;

static void *acct_allocate(void *st, usize size, usize align) {
    (void)st;
    void *p = cstd_alloc_allocate(cstd_alloc_system(), size, align);
    if (p) g_acct_live++;
    return p;
}

static void acct_deallocate(void *st, void *ptr, usize size, usize align) {
    (void)st;
    if (ptr) g_acct_live--;
    cstd_alloc_deallocate(cstd_alloc_system(), ptr, size, align);
}

static void *acct_reallocate(void *st, void *ptr,
                             usize old_size, usize new_size, usize align) {
    (void)st;
    void *p = cstd_alloc_reallocate(cstd_alloc_system(), ptr,
                                    old_size, new_size, align);
    /* Net live count: if `ptr` was non-null then realloc consumed
     * the old block and produced a new one (success) or left the
     * old intact (failure) — either way the count is unchanged.
     * Only the null→non-null transition is a fresh allocation. */
    if (!ptr && p) g_acct_live++;
    return p;
}

static const struct cstd_alloc_vtable acct_vtable = {
    .allocate   = acct_allocate,
    .deallocate = acct_deallocate,
    .reallocate = acct_reallocate,
};

static struct cstd_alloc acct_alloc(void) {
    struct cstd_alloc a;
    a.vtable = &acct_vtable;
    a.state  = (void *)0;
    return a;
}

[[cust::test]] int test_string_gnu_cleanup_on_scope_exit(void) {
    i32 before = g_acct_live;
    {
        [[gnu::cleanup(cstd_string_free)]]
        struct cstd_string s = cstd_string_new_in(acct_alloc());
        cust_assert(cstd_string_push_cstr(&s, "scoped cleanup"));
        cust_assert(cstd_string_capacity(&s) > (usize)0);
        cust_assert_eq(g_acct_live, before + 1);
        /* No explicit free — the attribute fires at the `}` below. */
    }
    cust_assert_eq(g_acct_live, before);
    return 0;
}

/* Helper that returns mid-scope. The whole point is that the
 * cleanup attribute fires on `return`, not just on fall-off. */
static bool early_return_helper(struct cstd_alloc a) {
    [[gnu::cleanup(cstd_string_free)]]
    struct cstd_string s = cstd_string_new_in(a);
    if (!cstd_string_push_cstr(&s, "hello")) return false;
    /* Simulate a fallible operation that aborts the function. */
    if (cstd_string_len(&s) == 5) return true;   /* early return */
    cust_assert(cstd_string_push_cstr(&s, "unreachable"));
    return true;
}

[[cust::test]] int test_string_gnu_cleanup_on_early_return(void) {
    i32 before = g_acct_live;
    cust_assert(early_return_helper(acct_alloc()));
    cust_assert_eq(g_acct_live, before);
    return 0;
}
