/* std::alloc — pluggable allocator interface.
 *
 * Mirrors (in spirit) Rust's `core::alloc::Allocator`: an
 * allocator is a *value*, not a global. Containers that own
 * heap memory store a `cstd_alloc` alongside their data so
 * `free` doesn't have to be threaded back through every call
 * site. This is the prerequisite for arena / bump / pool
 * allocators slotting under the same string / vec types we
 * ship in later modules.
 *
 * Shape:
 *
 *   struct cstd_alloc {
 *       const struct cstd_alloc_vtable *vtable;
 *       void                           *state;
 *   };
 *
 * Both fields are `pub_repr` so consumers can place an
 * allocator by value on the stack and pass it into the `_in`
 * constructors of any owning type.
 *
 * Convention:
 *   - `size == 0` may return NULL (callers must tolerate).
 *   - `align` MUST be a power of two.
 *   - On `deallocate` / `reallocate`, `size` and `align` MUST
 *     match the values originally passed to `allocate` /
 *     `reallocate`.
 *   - `reallocate` returns NULL on failure and leaves the old
 *     allocation intact; the caller still owns it.
 */

#cust use crate::types;

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* ─── vtable ────────────────────────────────────────────── */

struct [[cust::pub_repr]] cstd_alloc_vtable {
    void *(*allocate)(void *state, usize size, usize align);
    void  (*deallocate)(void *state, void *ptr, usize size, usize align);
    void *(*reallocate)(void *state, void *ptr,
                        usize old_size, usize new_size, usize align);
};

/* ─── allocator handle (fat pointer) ────────────────────── */

struct [[cust::pub_repr]] cstd_alloc {
    const struct cstd_alloc_vtable *vtable;
    void                           *state;
};

/* ─── thin accessors ────────────────────────────────────── */

/* Hide the `a.vtable->slot(a.state, ...)` shape from callers
 * so containers can treat `cstd_alloc` as if its operations
 * were ordinary methods. */

[[cust::pub]] void *cstd_alloc_allocate(struct cstd_alloc a,
                                        usize size, usize align) {
    return a.vtable->allocate(a.state, size, align);
}

[[cust::pub]] void cstd_alloc_deallocate(struct cstd_alloc a, void *ptr,
                                         usize size, usize align) {
    a.vtable->deallocate(a.state, ptr, size, align);
}

[[cust::pub]] void *cstd_alloc_reallocate(struct cstd_alloc a, void *ptr,
                                          usize old_size, usize new_size,
                                          usize align) {
    return a.vtable->reallocate(a.state, ptr, old_size, new_size, align);
}

/* ─── system allocator (libc-backed) ────────────────────── */

/* `state` is unused; the libc heap is process-global. We pick
 * `malloc`/`realloc`/`free` for the common-alignment fast path
 * and fall back to `aligned_alloc` + alloc-copy-free when the
 * caller requests an over-aligned block. C11 guarantees `free`
 * accepts pointers from both `malloc` and `aligned_alloc`. */

static void *sys_allocate(void *st, usize size, usize align) {
    (void)st;
    if (size == 0) return (void *)0;
    if (align <= _Alignof(max_align_t)) {
        return malloc(size);
    }
    /* aligned_alloc requires size to be a multiple of align. */
    usize rounded = (size + align - 1) & ~(align - 1);
    return aligned_alloc(align, rounded);
}

static void sys_deallocate(void *st, void *ptr, usize size, usize align) {
    (void)st;
    (void)size;
    (void)align;
    free(ptr);
}

static void *sys_reallocate(void *st, void *ptr,
                            usize old_size, usize new_size, usize align) {
    (void)st;
    if (new_size == 0) {
        free(ptr);
        return (void *)0;
    }
    if (align <= _Alignof(max_align_t)) {
        return realloc(ptr, new_size);
    }
    /* Over-aligned: realloc can't preserve alignment, so do the
     * alloc/copy/free dance by hand. */
    void *fresh = sys_allocate((void *)0, new_size, align);
    if (!fresh) return (void *)0;
    if (ptr) {
        usize copy = old_size < new_size ? old_size : new_size;
        memcpy(fresh, ptr, copy);
        free(ptr);
    }
    return fresh;
}

static const struct cstd_alloc_vtable cstd_alloc_system_vtable = {
    .allocate   = sys_allocate,
    .deallocate = sys_deallocate,
    .reallocate = sys_reallocate,
};

[[cust::pub]] struct cstd_alloc cstd_alloc_system(void) {
    struct cstd_alloc a;
    a.vtable = &cstd_alloc_system_vtable;
    a.state  = (void *)0;
    return a;
}

/* ─── unit tests ────────────────────────────────────────── */

[[cust::test]] int test_system_alloc_and_free(void) {
    struct cstd_alloc a = cstd_alloc_system();
    void *p = cstd_alloc_allocate(a, 64, 8);
    cust_assert(p != (void *)0);
    /* Touch it so a sanitiser would catch a bad return. */
    ((u8 *)p)[0]  = 0xAA;
    ((u8 *)p)[63] = 0x55;
    cstd_alloc_deallocate(a, p, 64, 8);
    return 0;
}

[[cust::test]] int test_system_alloc_zero_size(void) {
    struct cstd_alloc a = cstd_alloc_system();
    void *p = cstd_alloc_allocate(a, 0, 1);
    /* Documented: size==0 may return NULL. dealloc(NULL) is a
     * no-op (free(NULL) is). */
    cust_assert(p == (void *)0);
    cstd_alloc_deallocate(a, p, 0, 1);
    return 0;
}

[[cust::test]] int test_system_realloc_grow(void) {
    struct cstd_alloc a = cstd_alloc_system();
    u8 *p = (u8 *)cstd_alloc_allocate(a, 16, 1);
    cust_assert(p != (u8 *)0);
    for (i32 i = 0; i < 16; i++) p[i] = (u8)i;

    u8 *q = (u8 *)cstd_alloc_reallocate(a, p, 16, 256, 1);
    cust_assert(q != (u8 *)0);
    /* Contents preserved across grow. */
    for (i32 i = 0; i < 16; i++) cust_assert_eq(q[i], (u8)i);

    cstd_alloc_deallocate(a, q, 256, 1);
    return 0;
}

[[cust::test]] int test_system_realloc_overaligned(void) {
    struct cstd_alloc a = cstd_alloc_system();
    /* 64-byte alignment exceeds max_align_t on every supported
     * target, exercising the alloc-copy-free fallback path. */
    void *p = cstd_alloc_allocate(a, 128, 64);
    cust_assert(p != (void *)0);
    cust_assert_eq(((usize)p) & (usize)63, (usize)0);

    void *q = cstd_alloc_reallocate(a, p, 128, 512, 64);
    cust_assert(q != (void *)0);
    cust_assert_eq(((usize)q) & (usize)63, (usize)0);

    cstd_alloc_deallocate(a, q, 512, 64);
    return 0;
}
