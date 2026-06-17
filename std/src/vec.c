/* std::vec — owned, growable, type-erased dynamic array.
 *
 * Same shape as `std_string`: allocator-by-value, fallible
 * grow, no hidden globals, `[[gnu::cleanup(std_vec_free)]]`
 * friendly.
 *
 * "Type-erased" means the element type is described by runtime
 * `elem_size` / `elem_align` fields rather than a generic
 * parameter (C has none). The trade-offs that buys / costs:
 *
 *   +  one implementation handles every T;
 *   +  heterogeneous use (FFI buffers, dynamic element sizes)
 *      is straightforward;
 *   -  no compile-time type check — `std_vec_push(&v, &x)`
 *      silently corrupts if `sizeof x != v.elem_size`;
 *   -  every element op pays a runtime `memcpy(elem_size)`
 *      rather than a typed store.
 *
 * Mitigation for the type-check loss: callers typically wrap
 * the type-erased API in a one-line typed adapter at the use
 * site, e.g.
 *
 *     static inline bool push_i32(struct std_vec *v, i32 x) {
 *         return std_vec_push(v, &x);
 *     }
 *
 * No element destructor in v1: `pop` / `clear` / `free` only
 * release storage. If an element type owns heap (`std_string`,
 * file handle, …) the caller must drop it before discarding.
 */

#cust use crate::types;
#cust use crate::alloc;

struct [[cust::pub_repr]] std_vec {
    void              *data;        /* cap * elem_size bytes allocated;
                                       len * elem_size bytes used */
    usize              len;         /* element count */
    usize              cap;         /* allocated element count */
    usize              elem_size;   /* bytes per element; may be 0 (ZST) */
    usize              elem_align;  /* power of two */
    struct cstd_alloc   alloc;
};

[[cust::pub]] struct std_vec std_vec_new_in(usize elem_size,
                                            usize elem_align,
                                            struct cstd_alloc a) {
    struct std_vec v;
    v.data       = (void *)0;
    v.len        = 0;
    v.cap        = 0;
    v.elem_size  = elem_size;
    v.elem_align = elem_align;
    v.alloc      = a;
    return v;
}

/* Growth geometry: double from 8 with overflow guard, clamp to
 * `need` if doubling would wrap. */
static usize next_cap(usize current, usize need) {
    usize new_cap = current < 8 ? 8 : current;
    while (new_cap < need) {
        if (new_cap > ((usize)-1) / 2) {
            return need;
        }
        new_cap *= 2;
    }
    return new_cap;
}

/* Overflow-checked `a * b`. Writes to `*out` and returns true
 * on success, false if the product would wrap. */
static bool checked_mul(usize a, usize b, usize *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if (a > ((usize)-1) / b) return false;
    *out = a * b;
    return true;
}

/* Reserve capacity for at least `extra` more elements past
 * `len`. Returns false on OOM, on `len + extra` overflowing
 * `usize`, or on `new_cap * elem_size` overflowing `usize`.
 * Zero-sized elements never allocate; cap is bumped logically. */
[[cust::pub]] bool std_vec_reserve(struct std_vec *v, usize extra) {
    if (extra == 0) return true;
    if (v->cap - v->len >= extra) return true;
    if (extra > ((usize)-1) - v->len) return false;

    usize need    = v->len + extra;
    usize new_cap = next_cap(v->cap, need);

    if (v->elem_size == 0) {
        /* ZST: no storage, just bookkeeping. cap tracks the
         * logical capacity so `len <= cap` invariants hold. */
        v->cap = new_cap;
        return true;
    }

    usize old_bytes = 0;
    usize new_bytes = 0;
    if (!checked_mul(v->cap, v->elem_size, &old_bytes)) return false;
    if (!checked_mul(new_cap, v->elem_size, &new_bytes)) return false;

    void *p;
    if (v->data) {
        p = cstd_alloc_reallocate(v->alloc, v->data,
                                 old_bytes, new_bytes, v->elem_align);
    } else {
        p = cstd_alloc_allocate(v->alloc, new_bytes, v->elem_align);
    }
    if (!p) return false;

    v->data = p;
    v->cap  = new_cap;
    return true;
}

[[cust::pub]] bool std_vec_with_capacity_in(struct std_vec *out,
                                            usize elem_size,
                                            usize elem_align,
                                            usize cap,
                                            struct cstd_alloc a) {
    *out = std_vec_new_in(elem_size, elem_align, a);
    if (cap == 0) return true;
    return std_vec_reserve(out, cap);
}

[[cust::pub]] void std_vec_free(struct std_vec *v) {
    if (!v || !v->data) return;
    usize bytes = 0;
    /* `v->cap * v->elem_size` was checked at allocation time;
     * recomputing it here cannot overflow. */
    (void)checked_mul(v->cap, v->elem_size, &bytes);
    cstd_alloc_deallocate(v->alloc, v->data, bytes, v->elem_align);
    v->data = (void *)0;
    v->len  = 0;
    v->cap  = 0;
}

[[cust::pub]] usize std_vec_len(const struct std_vec *v) {
    return v->len;
}

[[cust::pub]] usize std_vec_capacity(const struct std_vec *v) {
    return v->cap;
}

/* Copy `elem_size` bytes from `*elem` into the back of the
 * vec. `elem` must point to a properly aligned value of the
 * vec's element type. For ZSTs `elem` is ignored and may be
 * NULL — only the length is bumped. */
[[cust::pub]] bool std_vec_push(struct std_vec *v, const void *elem) {
    if (!std_vec_reserve(v, 1)) return false;
    if (v->elem_size != 0) {
        u8 *dst = (u8 *)v->data + v->len * v->elem_size;
        const u8 *src = (const u8 *)elem;
        for (usize i = 0; i < v->elem_size; i++) dst[i] = src[i];
    }
    v->len++;
    return true;
}

/* Pop the back element into `*out` (`out` may be NULL to
 * discard). Returns false if the vec is empty. Does NOT
 * release storage; pair with `std_vec_free` at end of life. */
[[cust::pub]] bool std_vec_pop(struct std_vec *v, void *out) {
    if (v->len == 0) return false;
    v->len--;
    if (out && v->elem_size != 0) {
        const u8 *src = (const u8 *)v->data + v->len * v->elem_size;
        u8 *dst = (u8 *)out;
        for (usize i = 0; i < v->elem_size; i++) dst[i] = src[i];
    }
    return true;
}

/* Indexed access. Returns NULL on out-of-bounds or on a ZST
 * vec (no storage to point at). Caller casts to the element
 * type; the cast is the type system's only seam here. */
[[cust::pub]] void *std_vec_get(struct std_vec *v, usize i) {
    if (i >= v->len) return (void *)0;
    if (v->elem_size == 0) return (void *)0;
    return (u8 *)v->data + i * v->elem_size;
}

[[cust::pub]] const void *std_vec_get_const(const struct std_vec *v, usize i) {
    if (i >= v->len) return (const void *)0;
    if (v->elem_size == 0) return (const void *)0;
    return (const u8 *)v->data + i * v->elem_size;
}

[[cust::pub]] void std_vec_clear(struct std_vec *v) {
    v->len = 0;
}

/* ─── unit tests ────────────────────────────────────────── */

[[cust::test]] int test_vec_new_and_free(void) {
    struct std_vec v = std_vec_new_in(sizeof(i32), _Alignof(i32),
                                      cstd_alloc_system());
    cust_assert_eq(std_vec_len(&v), (usize)0);
    cust_assert_eq(std_vec_capacity(&v), (usize)0);
    std_vec_free(&v);
    /* Idempotent on a zeroed value. */
    std_vec_free(&v);
    return 0;
}

[[cust::test]] int test_vec_push_pop_i32(void) {
    struct std_vec v = std_vec_new_in(sizeof(i32), _Alignof(i32),
                                      cstd_alloc_system());
    for (i32 i = 0; i < 50; i++) {
        cust_assert(std_vec_push(&v, &i));
    }
    cust_assert_eq(std_vec_len(&v), (usize)50);

    for (i32 i = 49; i >= 0; i--) {
        i32 out = -1;
        cust_assert(std_vec_pop(&v, &out));
        cust_assert_eq(out, i);
    }
    cust_assert_eq(std_vec_len(&v), (usize)0);

    /* Pop on empty returns false; the discard-output form is
     * also legal. */
    cust_assert(!std_vec_pop(&v, (void *)0));
    std_vec_free(&v);
    return 0;
}

[[cust::test]] int test_vec_get_indexes_correctly(void) {
    struct std_vec v = std_vec_new_in(sizeof(i32), _Alignof(i32),
                                      cstd_alloc_system());
    for (i32 i = 0; i < 10; i++) {
        i32 x = i * 7;
        cust_assert(std_vec_push(&v, &x));
    }
    for (usize i = 0; i < 10; i++) {
        i32 *p = (i32 *)std_vec_get(&v, i);
        cust_assert(p != (i32 *)0);
        cust_assert_eq(*p, (i32)(i * 7));
    }
    /* Out-of-bounds returns NULL. */
    cust_assert(std_vec_get(&v, 10) == (void *)0);
    cust_assert(std_vec_get(&v, (usize)-1) == (void *)0);
    std_vec_free(&v);
    return 0;
}

[[cust::test]] int test_vec_with_capacity_avoids_regrow(void) {
    struct std_vec v;
    cust_assert(std_vec_with_capacity_in(&v,
                                         sizeof(i32), _Alignof(i32),
                                         32,
                                         cstd_alloc_system()));
    cust_assert(std_vec_capacity(&v) >= (usize)32);

    usize cap_before  = std_vec_capacity(&v);
    void *data_before = v.data;
    for (i32 i = 0; i < 32; i++) {
        cust_assert(std_vec_push(&v, &i));
    }
    cust_assert_eq(std_vec_capacity(&v), cap_before);
    cust_assert(v.data == data_before);
    std_vec_free(&v);
    return 0;
}

[[cust::test]] int test_vec_clear_keeps_capacity(void) {
    struct std_vec v = std_vec_new_in(sizeof(i32), _Alignof(i32),
                                      cstd_alloc_system());
    for (i32 i = 0; i < 16; i++) cust_assert(std_vec_push(&v, &i));
    usize cap = std_vec_capacity(&v);
    std_vec_clear(&v);
    cust_assert_eq(std_vec_len(&v), (usize)0);
    cust_assert_eq(std_vec_capacity(&v), cap);
    std_vec_free(&v);
    return 0;
}

/* Larger element type to make sure stride arithmetic is right
 * (a 32-byte element catches off-by-`elem_size` bugs that a
 * 4-byte int would mask). */
[[cust::test]] int test_vec_large_element_stride(void) {
    struct big { u64 a, b, c, d; };
    struct std_vec v = std_vec_new_in(sizeof(struct big),
                                      _Alignof(struct big),
                                      cstd_alloc_system());
    for (u64 i = 0; i < 20; i++) {
        struct big x = { i, i * 2, i * 3, i * 4 };
        cust_assert(std_vec_push(&v, &x));
    }
    for (usize i = 0; i < 20; i++) {
        struct big *p = (struct big *)std_vec_get(&v, i);
        cust_assert(p != (struct big *)0);
        cust_assert_eq(p->a, (u64)i);
        cust_assert_eq(p->b, (u64)(i * 2));
        cust_assert_eq(p->c, (u64)(i * 3));
        cust_assert_eq(p->d, (u64)(i * 4));
    }
    std_vec_free(&v);
    return 0;
}

/* ZST: no storage allocated, but len/cap bookkeeping still
 * works. Useful for "set of marker events" / counting patterns. */
[[cust::test]] int test_vec_zst_bookkeeping(void) {
    struct std_vec v = std_vec_new_in(0, 1, cstd_alloc_system());
    for (i32 i = 0; i < 100; i++) {
        cust_assert(std_vec_push(&v, (void *)0));
    }
    cust_assert_eq(std_vec_len(&v), (usize)100);
    /* data must remain NULL — no allocation for ZSTs. */
    cust_assert(v.data == (void *)0);

    i32 sentinel = 42;
    for (i32 i = 0; i < 50; i++) {
        cust_assert(std_vec_pop(&v, &sentinel));
        /* `out` is left untouched for ZSTs. */
        cust_assert_eq(sentinel, 42);
    }
    cust_assert_eq(std_vec_len(&v), (usize)50);

    std_vec_free(&v);
    return 0;
}

/* gnu::cleanup interop — same recipe as std_string. Uses an
 * accounting allocator to prove the drop ran, not just that
 * the attribute parsed. */

static i32 g_vec_acct_live = 0;

static void *vacct_allocate(void *st, usize size, usize align) {
    (void)st;
    void *p = cstd_alloc_allocate(cstd_alloc_system(), size, align);
    if (p) g_vec_acct_live++;
    return p;
}

static void vacct_deallocate(void *st, void *ptr, usize size, usize align) {
    (void)st;
    if (ptr) g_vec_acct_live--;
    cstd_alloc_deallocate(cstd_alloc_system(), ptr, size, align);
}

static void *vacct_reallocate(void *st, void *ptr,
                              usize old_size, usize new_size, usize align) {
    (void)st;
    void *p = cstd_alloc_reallocate(cstd_alloc_system(), ptr,
                                   old_size, new_size, align);
    if (!ptr && p) g_vec_acct_live++;
    return p;
}

static const struct cstd_alloc_vtable vacct_vtable = {
    .allocate   = vacct_allocate,
    .deallocate = vacct_deallocate,
    .reallocate = vacct_reallocate,
};

static struct cstd_alloc vacct_alloc(void) {
    struct cstd_alloc a;
    a.vtable = &vacct_vtable;
    a.state  = (void *)0;
    return a;
}

[[cust::test]] int test_vec_gnu_cleanup_on_scope_exit(void) {
    i32 before = g_vec_acct_live;
    {
        [[gnu::cleanup(std_vec_free)]]
        struct std_vec v = std_vec_new_in(sizeof(i32),
                                          _Alignof(i32),
                                          vacct_alloc());
        for (i32 i = 0; i < 20; i++) cust_assert(std_vec_push(&v, &i));
        cust_assert_eq(g_vec_acct_live, before + 1);
        /* Attribute fires at the closing brace below. */
    }
    cust_assert_eq(g_vec_acct_live, before);
    return 0;
}
