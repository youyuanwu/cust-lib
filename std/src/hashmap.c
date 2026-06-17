/* std::hashmap — owned, open-addressing hash map.
 *
 * Same design ethos as `std_vec`: allocator-by-value, fallible
 * grow (every op that may allocate returns `bool`), no hidden
 * globals, `[[gnu::cleanup(std_hashmap_free)]]` friendly.
 *
 * Strategy: open addressing with linear probing and a one-byte
 * control array per slot (EMPTY / FULL / TOMBSTONE). One
 * contiguous bucket region per array means a resize is a small
 * fixed number of `cstd_alloc` calls — mirroring how `std_vec`
 * grows — rather than a per-node allocation as chaining would
 * need. The trade-offs:
 *
 *   +  cache-friendly probing, few allocations;
 *   +  the only failure point is resize, so the `bool` OOM
 *      contract stays simple;
 *   -  deletions leave tombstones; a resize reclaims them.
 *
 * Type erasure (as in `std_vec`): key and value types are
 * described by runtime `key_size` / `value_size` (+ alignment)
 * rather than a generic parameter. Hashing and equality are
 * caller-supplied function pointers so no hash algorithm is
 * baked in; pass NULL to use the byte-wise defaults
 * (`std_hash_bytes` / `std_key_eq_bytes`), which are correct
 * for trivially-comparable keys with no padding (e.g. `i32`,
 * `u64`, fixed byte arrays).
 *
 * Ownership model — by-value with move + drain. The map stores
 * `memcpy`'d key/value bytes, so for elements that own heap
 * (`cstd_string`, a file handle, …):
 *
 *   - `std_hashmap_insert` / `std_hashmap_insert_move` *move* the
 *     bytes in: after a successful insert of a new key the caller
 *     must not drop the originals (the map owns them now). Use
 *     `std_hashmap_insert_move` to recover the displaced value on
 *     an overwrite — and note the passed key is NOT stored on an
 *     overwrite, so the caller still owns that key. On OOM nothing
 *     is moved and the caller keeps ownership of both.
 *   - `remove` / `clear` / `free` release only the map's own
 *     storage, never the element pointees. To reclaim owned
 *     elements, *drain* first: walk every entry with
 *     `std_hashmap_iter_new` / `std_hashmap_iter_next`, drop each
 *     key/value, then call `std_hashmap_free`.
 *
 * For trivially-copyable elements (ints, fixed structs, views)
 * none of this matters — insert/overwrite/free just work.
 */

#cust use crate::types;
#cust use crate::alloc;

#include <string.h>

/* Caller-supplied hashing / equality over `key_size` bytes. */
[[cust::pub]] typedef u64  (*std_hash_fn)(const void *key, usize key_size);
[[cust::pub]] typedef bool (*std_key_eq_fn)(const void *a, const void *b,
                                            usize key_size);

struct [[cust::pub_repr]] std_hashmap {
    u8                *ctrl;        /* cap bytes: EMPTY/FULL/TOMBSTONE */
    void              *keys;        /* cap * key_size bytes */
    void              *values;      /* cap * value_size bytes */
    usize              len;         /* live entries */
    usize              tombstones;  /* deleted-but-not-reclaimed slots */
    usize              cap;         /* power-of-two slot count, or 0 */
    usize              key_size;
    usize              value_size;
    usize              key_align;   /* power of two */
    usize              value_align; /* power of two */
    std_hash_fn        hash;
    std_key_eq_fn      key_eq;
    struct cstd_alloc  alloc;
};

enum { SLOT_EMPTY = 0, SLOT_FULL = 1, SLOT_TOMBSTONE = 2 };

/* Forward iterator over live entries. Construct with
 * `std_hashmap_iter_new`; advance with `std_hashmap_iter_next`. */
struct [[cust::pub_repr]] std_hashmap_iter {
    struct std_hashmap *map;
    usize               slot;   /* next slot to examine */
};

/* ─── default hash / equality ───────────────────────────── */

/* FNV-1a (64-bit) over the raw key bytes. */
[[cust::pub]] u64 std_hash_bytes(const void *key, usize key_size) {
    const u8 *p = (const u8 *)key;
    u64 h = 1469598103934665603ull;       /* offset basis */
    for (usize i = 0; i < key_size; i++) {
        h ^= (u64)p[i];
        h *= 1099511628211ull;            /* prime */
    }
    return h;
}

[[cust::pub]] bool std_key_eq_bytes(const void *a, const void *b,
                                    usize key_size) {
    if (key_size == 0) return true;
    return memcmp(a, b, key_size) == 0;
}

/* ─── internal helpers ──────────────────────────────────── */

/* Overflow-checked `a * b`. Writes `*out`, returns false if the
 * product would wrap `usize`. */
static bool checked_mul(usize a, usize b, usize *out) {
    if (a == 0 || b == 0) {
        *out = 0;
        return true;
    }
    if (a > ((usize)-1) / b) return false;
    *out = a * b;
    return true;
}

static void *key_at(struct std_hashmap *m, usize i) {
    return (u8 *)m->keys + i * m->key_size;
}

static void *value_at(struct std_hashmap *m, usize i) {
    return (u8 *)m->values + i * m->value_size;
}

static void free_arrays(struct std_hashmap *m, u8 *ctrl,
                        void *keys, void *values, usize cap) {
    if (ctrl) cstd_alloc_deallocate(m->alloc, ctrl, cap, 1);
    if (keys) {
        usize b = 0;
        (void)checked_mul(cap, m->key_size, &b);   /* checked at alloc */
        cstd_alloc_deallocate(m->alloc, keys, b, m->key_align);
    }
    if (values) {
        usize b = 0;
        (void)checked_mul(cap, m->value_size, &b);
        cstd_alloc_deallocate(m->alloc, values, b, m->value_align);
    }
}

/* Allocate fresh arrays of `new_cap` (a power of two) slots and
 * rehash every live entry into them, dropping tombstones.
 * Returns false on OOM, leaving the map untouched. */
static bool rehash(struct std_hashmap *m, usize new_cap) {
    usize key_bytes = 0;
    usize val_bytes = 0;
    if (!checked_mul(new_cap, m->key_size, &key_bytes))   return false;
    if (!checked_mul(new_cap, m->value_size, &val_bytes)) return false;

    u8 *new_ctrl = (u8 *)cstd_alloc_allocate(m->alloc, new_cap, 1);
    if (!new_ctrl) return false;
    for (usize i = 0; i < new_cap; i++) new_ctrl[i] = SLOT_EMPTY;

    void *new_keys = (void *)0;
    if (key_bytes) {
        new_keys = cstd_alloc_allocate(m->alloc, key_bytes, m->key_align);
        if (!new_keys) {
            cstd_alloc_deallocate(m->alloc, new_ctrl, new_cap, 1);
            return false;
        }
    }

    void *new_values = (void *)0;
    if (val_bytes) {
        new_values = cstd_alloc_allocate(m->alloc, val_bytes, m->value_align);
        if (!new_values) {
            if (new_keys)
                cstd_alloc_deallocate(m->alloc, new_keys, key_bytes,
                                      m->key_align);
            cstd_alloc_deallocate(m->alloc, new_ctrl, new_cap, 1);
            return false;
        }
    }

    usize mask = new_cap - 1;
    for (usize i = 0; i < m->cap; i++) {
        if (m->ctrl[i] != SLOT_FULL) continue;
        void *k = key_at(m, i);
        u64 h = m->hash(k, m->key_size);
        usize j = (usize)h & mask;
        while (new_ctrl[j] == SLOT_FULL) j = (j + 1) & mask;
        new_ctrl[j] = SLOT_FULL;
        if (m->key_size)
            memcpy((u8 *)new_keys + j * m->key_size, k, m->key_size);
        if (m->value_size)
            memcpy((u8 *)new_values + j * m->value_size,
                   value_at(m, i), m->value_size);
    }

    free_arrays(m, m->ctrl, m->keys, m->values, m->cap);
    m->ctrl       = new_ctrl;
    m->keys       = new_keys;
    m->values     = new_values;
    m->cap        = new_cap;
    m->tombstones = 0;
    return true;
}

/* Ensure room for one more insertion. Grows past a 0.75 live
 * load factor; if the slack is mostly tombstones, rehashes at
 * the same size to reclaim them instead of growing. */
static bool ensure_one(struct std_hashmap *m) {
    if (m->cap == 0) return rehash(m, 8);
    if ((m->len + m->tombstones + 1) * 4 > m->cap * 3) {
        usize new_cap = ((m->len + 1) * 4 > m->cap * 3)
                            ? m->cap * 2
                            : m->cap;
        if (new_cap < m->cap) return false; /* overflow guard */
        return rehash(m, new_cap);
    }
    return true;
}

/* Probe for `key`. Returns its slot index, or (usize)-1 if
 * absent. The 0.75 load-factor invariant guarantees an EMPTY
 * slot exists, so the probe always terminates. */
static usize find_index(struct std_hashmap *m, const void *key) {
    if (m->cap == 0) return (usize)-1;
    usize mask = m->cap - 1;
    u64 h = m->hash(key, m->key_size);
    usize j = (usize)h & mask;
    for (;;) {
        u8 c = m->ctrl[j];
        if (c == SLOT_EMPTY) return (usize)-1;
        if (c == SLOT_FULL && m->key_eq(key_at(m, j), key, m->key_size))
            return j;
        j = (j + 1) & mask;
    }
}

/* ─── construction / teardown ───────────────────────────── */

/* Construct an empty map. Pass NULL for `hash` / `key_eq` to
 * use the byte-wise defaults. No allocation happens until the
 * first insert. */
[[cust::pub]] struct std_hashmap std_hashmap_new_in(usize key_size,
                                                    usize key_align,
                                                    usize value_size,
                                                    usize value_align,
                                                    std_hash_fn hash,
                                                    std_key_eq_fn key_eq,
                                                    struct cstd_alloc a) {
    struct std_hashmap m;
    m.ctrl        = (u8 *)0;
    m.keys        = (void *)0;
    m.values      = (void *)0;
    m.len         = 0;
    m.tombstones  = 0;
    m.cap         = 0;
    m.key_size    = key_size;
    m.value_size  = value_size;
    m.key_align   = key_align;
    m.value_align = value_align;
    m.hash        = hash   ? hash   : std_hash_bytes;
    m.key_eq      = key_eq ? key_eq : std_key_eq_bytes;
    m.alloc       = a;
    return m;
}

[[cust::pub]] void std_hashmap_free(struct std_hashmap *m) {
    if (!m || m->cap == 0) return;
    free_arrays(m, m->ctrl, m->keys, m->values, m->cap);
    m->ctrl       = (u8 *)0;
    m->keys       = (void *)0;
    m->values     = (void *)0;
    m->len        = 0;
    m->tombstones = 0;
    m->cap        = 0;
}

[[cust::pub]] usize std_hashmap_len(const struct std_hashmap *m) {
    return m->len;
}

[[cust::pub]] usize std_hashmap_capacity(const struct std_hashmap *m) {
    return m->cap;
}

/* ─── access ────────────────────────────────────────────── */

/* Insert `key` → `value` into a position known to be absent.
 * Caller must have established (via `find_index`) that the key is
 * not present and (via `ensure_one`) that a free slot exists. */
static void insert_absent(struct std_hashmap *m,
                          const void *key, const void *value) {
    usize mask = m->cap - 1;
    u64 h = m->hash(key, m->key_size);
    usize j = (usize)h & mask;
    usize first_tomb = (usize)-1;

    for (;;) {
        u8 c = m->ctrl[j];
        if (c == SLOT_EMPTY) {
            usize slot = (first_tomb != (usize)-1) ? first_tomb : j;
            m->ctrl[slot] = SLOT_FULL;
            if (m->key_size)   memcpy(key_at(m, slot), key, m->key_size);
            if (m->value_size) memcpy(value_at(m, slot), value, m->value_size);
            m->len++;
            if (slot == first_tomb) m->tombstones--;
            return;
        }
        if (c == SLOT_TOMBSTONE && first_tomb == (usize)-1) {
            first_tomb = j;
        }
        /* SLOT_FULL can't match — the key is known absent. */
        j = (j + 1) & mask;
    }
}

/* Move-aware insert (the owning-container contract).
 *
 * Semantics, by case:
 *   - New key: the `key` and `value` bytes are *moved* into the
 *     map — the caller must NOT drop them afterwards. `*replaced`
 *     (if non-NULL) is set false.
 *   - Existing key: the previous value bytes are copied out into
 *     `*old_value` (if non-NULL) for the caller to drop; the new
 *     `value` bytes are moved in. The passed `key` is NOT stored
 *     (the map keeps its original key bytes), so the caller still
 *     owns the `key` they passed and must drop it. `*replaced` is
 *     set true.
 *   - OOM (only possible while inserting a *new* key, which may
 *     resize): returns false, the map is unchanged, and the
 *     caller retains ownership of BOTH `key` and `value`.
 *
 * `old_value` / `replaced` may be NULL. For trivially-copyable
 * element types this is just insert-or-overwrite; the move
 * language only matters when an element owns heap. */
[[cust::pub]] bool std_hashmap_insert_move(struct std_hashmap *m,
                                           const void *key,
                                           const void *value,
                                           void *old_value,
                                           bool *replaced) {
    usize idx = find_index(m, key);
    if (idx != (usize)-1) {
        if (m->value_size) {
            if (old_value) memcpy(old_value, value_at(m, idx), m->value_size);
            memcpy(value_at(m, idx), value, m->value_size);
        }
        if (replaced) *replaced = true;
        return true;
    }
    if (!ensure_one(m)) return false;
    insert_absent(m, key, value);
    if (replaced) *replaced = false;
    return true;
}

/* Insert or overwrite, discarding any previous value. Thin
 * wrapper over `std_hashmap_insert_move`; suitable for
 * trivially-copyable values whose overwritten bytes need no
 * cleanup. Returns false only on OOM while growing for a new
 * key. */
[[cust::pub]] bool std_hashmap_insert(struct std_hashmap *m,
                                      const void *key,
                                      const void *value) {
    return std_hashmap_insert_move(m, key, value, (void *)0, (bool *)0);
}

/* Pointer to the stored value for `key`, or NULL if absent (or
 * for a zero-sized value type, which has no storage). The
 * pointer is valid until the next insert/remove that may
 * rehash. */
[[cust::pub]] void *std_hashmap_get(struct std_hashmap *m, const void *key) {
    usize idx = find_index(m, key);
    if (idx == (usize)-1) return (void *)0;
    if (m->value_size == 0) return (void *)0;
    return value_at(m, idx);
}

[[cust::pub]] const void *std_hashmap_get_const(const struct std_hashmap *m,
                                                const void *key) {
    return std_hashmap_get((struct std_hashmap *)m, key);
}

[[cust::pub]] bool std_hashmap_contains(struct std_hashmap *m,
                                        const void *key) {
    return find_index(m, key) != (usize)-1;
}

/* Remove `key`. Returns false if it was not present. Leaves a
 * tombstone; a later resize reclaims it. */
[[cust::pub]] bool std_hashmap_remove(struct std_hashmap *m,
                                      const void *key) {
    usize idx = find_index(m, key);
    if (idx == (usize)-1) return false;
    m->ctrl[idx] = SLOT_TOMBSTONE;
    m->len--;
    m->tombstones++;
    return true;
}

/* Drop all entries, keeping the allocated capacity. Releases
 * only the map's storage; for owned elements, drain first (see
 * `std_hashmap_iter_next`). */
[[cust::pub]] void std_hashmap_clear(struct std_hashmap *m) {
    if (m->cap == 0) return;
    for (usize i = 0; i < m->cap; i++) m->ctrl[i] = SLOT_EMPTY;
    m->len        = 0;
    m->tombstones = 0;
}

/* ─── iteration ─────────────────────────────────────────── */

/* Iterator positioned before the first entry. Valid until the
 * next insert (which may resize and invalidate it). Freeing
 * element *pointees* during a drain does not move entries, so it
 * is safe to drop keys/values while iterating. */
[[cust::pub]] struct std_hashmap_iter std_hashmap_iter_new(struct std_hashmap *m) {
    struct std_hashmap_iter it;
    it.map  = m;
    it.slot = 0;
    return it;
}

/* Advance to the next live entry. On success writes interior
 * pointers to the stored key/value into `*key_out` / `*value_out`
 * (either may be NULL to ignore; a zero-sized key/value yields a
 * NULL pointer) and returns true. Returns false once exhausted.
 *
 * Drain-to-free pattern for owned elements:
 *
 *     struct std_hashmap_iter it = std_hashmap_iter_new(&m);
 *     void *k, *v;
 *     while (std_hashmap_iter_next(&it, &k, &v)) {
 *         drop_key(k);
 *         drop_value(v);
 *     }
 *     std_hashmap_free(&m);
 */
[[cust::pub]] bool std_hashmap_iter_next(struct std_hashmap_iter *it,
                                         void **key_out, void **value_out) {
    struct std_hashmap *m = it->map;
    while (it->slot < m->cap) {
        usize i = it->slot++;
        if (m->ctrl[i] == SLOT_FULL) {
            if (key_out)   *key_out   = m->key_size   ? key_at(m, i)   : (void *)0;
            if (value_out) *value_out = m->value_size ? value_at(m, i) : (void *)0;
            return true;
        }
    }
    return false;
}

/* ─── unit tests ────────────────────────────────────────── */

[[cust::test]] int test_hashmap_new_and_free(void) {
    struct std_hashmap m = std_hashmap_new_in(sizeof(i32), _Alignof(i32),
                                              sizeof(i32), _Alignof(i32),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    cust_assert_eq(std_hashmap_len(&m), (usize)0);
    cust_assert_eq(std_hashmap_capacity(&m), (usize)0);
    std_hashmap_free(&m);
    /* Idempotent on a zeroed value. */
    std_hashmap_free(&m);
    return 0;
}

[[cust::test]] int test_hashmap_insert_get_i32(void) {
    struct std_hashmap m = std_hashmap_new_in(sizeof(i32), _Alignof(i32),
                                              sizeof(i32), _Alignof(i32),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    for (i32 i = 0; i < 100; i++) {
        i32 v = i * 3 + 1;
        cust_assert(std_hashmap_insert(&m, &i, &v));
    }
    cust_assert_eq(std_hashmap_len(&m), (usize)100);

    for (i32 i = 0; i < 100; i++) {
        i32 *p = (i32 *)std_hashmap_get(&m, &i);
        cust_assert(p != (i32 *)0);
        cust_assert_eq(*p, (i32)(i * 3 + 1));
    }
    /* Absent key → NULL / false. */
    i32 missing = 1000;
    cust_assert(std_hashmap_get(&m, &missing) == (void *)0);
    cust_assert(!std_hashmap_contains(&m, &missing));
    std_hashmap_free(&m);
    return 0;
}

[[cust::test]] int test_hashmap_overwrite_keeps_len(void) {
    struct std_hashmap m = std_hashmap_new_in(sizeof(i32), _Alignof(i32),
                                              sizeof(i32), _Alignof(i32),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    i32 k = 42;
    i32 v1 = 7;
    i32 v2 = 99;
    cust_assert(std_hashmap_insert(&m, &k, &v1));
    cust_assert(std_hashmap_insert(&m, &k, &v2));
    cust_assert_eq(std_hashmap_len(&m), (usize)1);

    i32 *p = (i32 *)std_hashmap_get(&m, &k);
    cust_assert(p != (i32 *)0);
    cust_assert_eq(*p, (i32)99);
    std_hashmap_free(&m);
    return 0;
}

[[cust::test]] int test_hashmap_remove_and_reinsert(void) {
    struct std_hashmap m = std_hashmap_new_in(sizeof(i32), _Alignof(i32),
                                              sizeof(i32), _Alignof(i32),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    for (i32 i = 0; i < 32; i++) {
        i32 v = i;
        cust_assert(std_hashmap_insert(&m, &i, &v));
    }
    /* Remove the even keys. */
    for (i32 i = 0; i < 32; i += 2) {
        cust_assert(std_hashmap_remove(&m, &i));
    }
    cust_assert_eq(std_hashmap_len(&m), (usize)16);
    /* Removing again returns false. */
    i32 zero = 0;
    cust_assert(!std_hashmap_remove(&m, &zero));

    /* Odd keys survive, even keys are gone. */
    for (i32 i = 0; i < 32; i++) {
        i32 *p = (i32 *)std_hashmap_get(&m, &i);
        if (i % 2 == 0) {
            cust_assert(p == (i32 *)0);
        } else {
            cust_assert(p != (i32 *)0);
            cust_assert_eq(*p, i);
        }
    }
    /* Re-insert reuses tombstones. */
    for (i32 i = 0; i < 32; i += 2) {
        i32 v = i * 10;
        cust_assert(std_hashmap_insert(&m, &i, &v));
    }
    cust_assert_eq(std_hashmap_len(&m), (usize)32);
    for (i32 i = 0; i < 32; i += 2) {
        i32 *p = (i32 *)std_hashmap_get(&m, &i);
        cust_assert(p != (i32 *)0);
        cust_assert_eq(*p, (i32)(i * 10));
    }
    std_hashmap_free(&m);
    return 0;
}

[[cust::test]] int test_hashmap_grows_and_retains(void) {
    struct std_hashmap m = std_hashmap_new_in(sizeof(i32), _Alignof(i32),
                                              sizeof(i32), _Alignof(i32),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    /* Force several resizes. */
    for (i32 i = 0; i < 1000; i++) {
        i32 v = i ^ 0x5a5a;
        cust_assert(std_hashmap_insert(&m, &i, &v));
    }
    cust_assert_eq(std_hashmap_len(&m), (usize)1000);
    cust_assert(std_hashmap_capacity(&m) >= (usize)1000);
    for (i32 i = 0; i < 1000; i++) {
        i32 *p = (i32 *)std_hashmap_get(&m, &i);
        cust_assert(p != (i32 *)0);
        cust_assert_eq(*p, (i32)(i ^ 0x5a5a));
    }
    std_hashmap_free(&m);
    return 0;
}

[[cust::test]] int test_hashmap_clear_keeps_capacity(void) {
    struct std_hashmap m = std_hashmap_new_in(sizeof(i32), _Alignof(i32),
                                              sizeof(i32), _Alignof(i32),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    for (i32 i = 0; i < 64; i++) {
        i32 v = i;
        cust_assert(std_hashmap_insert(&m, &i, &v));
    }
    usize cap = std_hashmap_capacity(&m);
    std_hashmap_clear(&m);
    cust_assert_eq(std_hashmap_len(&m), (usize)0);
    cust_assert_eq(std_hashmap_capacity(&m), cap);

    /* Map is usable after clear. */
    i32 k = 5;
    i32 v = 500;
    cust_assert(std_hashmap_insert(&m, &k, &v));
    i32 *p = (i32 *)std_hashmap_get(&m, &k);
    cust_assert(p != (i32 *)0);
    cust_assert_eq(*p, (i32)500);
    std_hashmap_free(&m);
    return 0;
}

/* Wide key + wide value to exercise stride arithmetic on both
 * arrays (a 4-byte key/value would mask off-by-size bugs). */
[[cust::test]] int test_hashmap_wide_key_value(void) {
    struct key { u64 a, b; };
    struct val { u64 a, b, c, d; };
    struct std_hashmap m = std_hashmap_new_in(sizeof(struct key),
                                              _Alignof(struct key),
                                              sizeof(struct val),
                                              _Alignof(struct val),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    for (u64 i = 0; i < 64; i++) {
        struct key k = { i, i * 7 };
        struct val v = { i, i * 2, i * 3, i * 4 };
        cust_assert(std_hashmap_insert(&m, &k, &v));
    }
    cust_assert_eq(std_hashmap_len(&m), (usize)64);
    for (u64 i = 0; i < 64; i++) {
        struct key k = { i, i * 7 };
        struct val *p = (struct val *)std_hashmap_get(&m, &k);
        cust_assert(p != (struct val *)0);
        cust_assert_eq(p->a, (u64)i);
        cust_assert_eq(p->b, (u64)(i * 2));
        cust_assert_eq(p->c, (u64)(i * 3));
        cust_assert_eq(p->d, (u64)(i * 4));
    }
    std_hashmap_free(&m);
    return 0;
}

[[cust::test]] int test_hashmap_iter_visits_every_entry_once(void) {
    struct std_hashmap m = std_hashmap_new_in(sizeof(i32), _Alignof(i32),
                                              sizeof(i32), _Alignof(i32),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    i32 sum_keys = 0;
    i32 sum_vals = 0;
    for (i32 i = 0; i < 50; i++) {
        i32 v = i * 2;
        cust_assert(std_hashmap_insert(&m, &i, &v));
        sum_keys += i;
        sum_vals += v;
    }

    struct std_hashmap_iter it = std_hashmap_iter_new(&m);
    void *kp;
    void *vp;
    usize seen = 0;
    i32 got_keys = 0;
    i32 got_vals = 0;
    while (std_hashmap_iter_next(&it, &kp, &vp)) {
        got_keys += *(i32 *)kp;
        got_vals += *(i32 *)vp;
        seen++;
    }
    cust_assert_eq(seen, (usize)50);
    cust_assert_eq(got_keys, sum_keys);
    cust_assert_eq(got_vals, sum_vals);
    std_hashmap_free(&m);
    return 0;
}

[[cust::test]] int test_hashmap_iter_empty_yields_nothing(void) {
    struct std_hashmap m = std_hashmap_new_in(sizeof(i32), _Alignof(i32),
                                              sizeof(i32), _Alignof(i32),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    struct std_hashmap_iter it = std_hashmap_iter_new(&m);
    void *kp;
    void *vp;
    cust_assert(!std_hashmap_iter_next(&it, &kp, &vp));
    std_hashmap_free(&m);
    return 0;
}

[[cust::test]] int test_hashmap_insert_move_reports_displaced(void) {
    struct std_hashmap m = std_hashmap_new_in(sizeof(i32), _Alignof(i32),
                                              sizeof(i32), _Alignof(i32),
                                              (std_hash_fn)0, (std_key_eq_fn)0,
                                              cstd_alloc_system());
    i32 k = 7;
    i32 v1 = 100;
    i32 v2 = 200;
    i32 old = -1;
    bool replaced = true;

    /* New key: nothing displaced, replaced == false. */
    cust_assert(std_hashmap_insert_move(&m, &k, &v1, &old, &replaced));
    cust_assert(!replaced);
    cust_assert_eq(std_hashmap_len(&m), (usize)1);

    /* Same key: displaces v1, replaced == true. */
    cust_assert(std_hashmap_insert_move(&m, &k, &v2, &old, &replaced));
    cust_assert(replaced);
    cust_assert_eq(old, (i32)100);
    cust_assert_eq(std_hashmap_len(&m), (usize)1);

    i32 *p = (i32 *)std_hashmap_get(&m, &k);
    cust_assert(p != (i32 *)0);
    cust_assert_eq(*p, (i32)200);
    std_hashmap_free(&m);
    return 0;
}
