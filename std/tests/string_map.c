/* Integration test: exercises the std crate's *public* surface
 * (linked against lib<std>.a + the generated std.h) the way a
 * real downstream consumer would — combining `cstd_string` with
 * `std_hashmap`.
 *
 * Unlike the in-module unit tests, this file reaches the crate
 * only through `#cust use std;`, so it can only touch
 * `[[cust::pub]]` decls.
 */

#cust use std;

/* Hash / compare `struct cstd_str` keys by the *bytes they
 * point at* (the default byte-wise hasher would hash the
 * {ptr,len} struct, i.e. the pointer identity, which is wrong
 * for string-content keys). */
static u64 strview_hash(const void *key, usize key_size) {
    (void)key_size;
    const struct cstd_str *s = (const struct cstd_str *)key;
    return std_hash_bytes(s->data, s->len);
}

static bool strview_eq(const void *a, const void *b, usize key_size) {
    (void)key_size;
    const struct cstd_str *x = (const struct cstd_str *)a;
    const struct cstd_str *y = (const struct cstd_str *)b;
    return cstd_str_eq(*x, *y);
}

/* Count word frequencies through the map, keyed by string-view
 * content, then confirm the tallies. */
[[cust::test]] int test_string_keyed_word_count(void) {
    struct std_hashmap counts =
        std_hashmap_new_in(sizeof(struct cstd_str), _Alignof(struct cstd_str),
                           sizeof(i32), _Alignof(i32),
                           strview_hash, strview_eq,
                           cstd_alloc_system());

    /* String literals have static lifetime, so the views stored
     * as keys stay valid for the life of the map. */
    const char *words[] = {
        "apple", "banana", "apple", "cherry",
        "banana", "apple", "cherry", "apple",
    };
    usize n = sizeof(words) / sizeof(words[0]);

    for (usize i = 0; i < n; i++) {
        struct cstd_str key = cstd_str_from_cstr(words[i]);
        i32 *slot = (i32 *)std_hashmap_get(&counts, &key);
        if (slot) {
            (*slot)++;
        } else {
            i32 one = 1;
            cust_assert(std_hashmap_insert(&counts, &key, &one));
        }
    }

    cust_assert_eq(std_hashmap_len(&counts), (usize)3);

    struct cstd_str apple  = cstd_str_from_cstr("apple");
    struct cstd_str banana = cstd_str_from_cstr("banana");
    struct cstd_str cherry = cstd_str_from_cstr("cherry");
    struct cstd_str missing = cstd_str_from_cstr("durian");

    i32 *pa = (i32 *)std_hashmap_get(&counts, &apple);
    i32 *pb = (i32 *)std_hashmap_get(&counts, &banana);
    i32 *pc = (i32 *)std_hashmap_get(&counts, &cherry);
    cust_assert(pa != (i32 *)0);
    cust_assert(pb != (i32 *)0);
    cust_assert(pc != (i32 *)0);
    cust_assert_eq(*pa, (i32)4);
    cust_assert_eq(*pb, (i32)2);
    cust_assert_eq(*pc, (i32)2);
    cust_assert(!std_hashmap_contains(&counts, &missing));

    std_hashmap_free(&counts);
    return 0;
}

/* Render a "<word>: <count>" line into an owned cstd_string by
 * pulling the count back out of the map — combining both types
 * in one flow. */
[[cust::test]] int test_render_count_to_string(void) {
    struct std_hashmap counts =
        std_hashmap_new_in(sizeof(struct cstd_str), _Alignof(struct cstd_str),
                           sizeof(i32), _Alignof(i32),
                           strview_hash, strview_eq,
                           cstd_alloc_system());

    struct cstd_str key = cstd_str_from_cstr("apple");
    i32 three = 3;
    cust_assert(std_hashmap_insert(&counts, &key, &three));

    i32 *p = (i32 *)std_hashmap_get(&counts, &key);
    cust_assert(p != (i32 *)0);
    cust_assert_eq(*p, (i32)3);

    struct cstd_string line = cstd_string_new_in(cstd_alloc_system());
    cust_assert(cstd_string_push_cstr(&line, "apple"));
    cust_assert(cstd_string_push_cstr(&line, ": "));
    /* Single-digit count → render as one ASCII byte. */
    cust_assert(cstd_string_push_byte(&line, (u8)('0' + *p)));

    struct cstd_str rendered = cstd_string_as_str(&line);
    struct cstd_str expected = cstd_str_from_cstr("apple: 3");
    cust_assert(cstd_str_eq(rendered, expected));

    /* And via the NUL-terminated C view. */
    const char *cstr = cstd_string_as_cstr(&line);
    cust_assert(cstd_str_eq(cstd_str_from_cstr(cstr), expected));

    cstd_string_free(&line);
    std_hashmap_free(&counts);
    return 0;
}

/* ── owned cstd_string keys AND values: move-in, drain-free ──
 *
 * Demonstrates the map's by-value + move + drain ownership model
 * with heap-owning elements on both sides. */

static u64 string_hash(const void *key, usize key_size) {
    (void)key_size;
    struct cstd_str v = cstd_string_as_str((const struct cstd_string *)key);
    return std_hash_bytes(v.data, v.len);
}

static bool string_eq(const void *a, const void *b, usize key_size) {
    (void)key_size;
    return cstd_str_eq(cstd_string_as_str((const struct cstd_string *)a),
                       cstd_string_as_str((const struct cstd_string *)b));
}

static struct cstd_string make_string(const char *s) {
    struct cstd_string str = cstd_string_new_in(cstd_alloc_system());
    cust_assert(cstd_string_push_cstr(&str, s));
    return str;
}

[[cust::test]] int test_owned_string_map_move_and_drain(void) {
    struct std_hashmap m =
        std_hashmap_new_in(sizeof(struct cstd_string),
                           _Alignof(struct cstd_string),
                           sizeof(struct cstd_string),
                           _Alignof(struct cstd_string),
                           string_hash, string_eq,
                           cstd_alloc_system());

    /* Two unique entries. A plain insert MOVES key+value into the
     * map — the locals must not be freed afterwards. */
    struct cstd_string k1 = make_string("red");
    struct cstd_string v1 = make_string("apple");
    cust_assert(std_hashmap_insert(&m, &k1, &v1));

    struct cstd_string k2 = make_string("yellow");
    struct cstd_string v2 = make_string("banana");
    cust_assert(std_hashmap_insert(&m, &k2, &v2));
    cust_assert_eq(std_hashmap_len(&m), (usize)2);

    /* Overwrite "red" → "cherry" via the move-aware insert. The
     * displaced "apple" comes back for us to drop; the duplicate
     * key we passed is NOT stored, so we drop it too; the new
     * value is moved in. */
    struct cstd_string dup_key = make_string("red");
    struct cstd_string v3      = make_string("cherry");
    struct cstd_string displaced;
    bool replaced = false;
    cust_assert(std_hashmap_insert_move(&m, &dup_key, &v3,
                                        &displaced, &replaced));
    cust_assert(replaced);
    cust_assert_eq(std_hashmap_len(&m), (usize)2);
    cust_assert(cstd_str_eq(cstd_string_as_str(&displaced),
                            cstd_str_from_cstr("apple")));
    cstd_string_free(&displaced);   /* drop the displaced value  */
    cstd_string_free(&dup_key);     /* drop our un-stored key     */
    /* v3 was moved into the map — do NOT free it here. */

    /* Look up by a temporary owned query key (dropped after). */
    struct cstd_string q = make_string("red");
    struct cstd_string *got = (struct cstd_string *)std_hashmap_get(&m, &q);
    cust_assert(got != (struct cstd_string *)0);
    cust_assert(cstd_str_eq(cstd_string_as_str(got),
                            cstd_str_from_cstr("cherry")));
    cstd_string_free(&q);

    /* Drain: free every stored key and value, then the map's own
     * arrays. */
    struct std_hashmap_iter it = std_hashmap_iter_new(&m);
    void *kp;
    void *vp;
    usize drained = 0;
    while (std_hashmap_iter_next(&it, &kp, &vp)) {
        cstd_string_free((struct cstd_string *)kp);
        cstd_string_free((struct cstd_string *)vp);
        drained++;
    }
    cust_assert_eq(drained, (usize)2);
    std_hashmap_free(&m);
    return 0;
}
