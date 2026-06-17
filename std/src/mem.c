/* std::mem — thin wrappers over libc memory/string primitives.
 *
 * Every export carries `[[cust::pub]]`, so the symbol table of
 * the final binary documents exactly which libc surfaces a
 * downstream crate actually reached for.
 *
 * Public signatures use std's `usize` / `i32` aliases (declared
 * in the sibling `types` module) so the generated `std.h` is
 * `<stddef.h>`-free. The libc wrappers themselves still need
 * `<stddef.h>` / `<string.h>` internally; those includes stay
 * private to this TU.
 */

#cust use crate::types;

#include <stddef.h>
#include <string.h>

[[cust::pub]] usize cstd_strlen(const char *s) {
    return strlen(s);
}

[[cust::pub]] i32 cstd_memcmp(const void *a, const void *b, usize n) {
    return memcmp(a, b, n);
}

[[cust::test]] int test_strlen_empty(void) {
    cust_assert_eq(cstd_strlen(""), (usize)0);
    return 0;
}

[[cust::test]] int test_strlen_hello(void) {
    cust_assert_eq(cstd_strlen("hello"), (usize)5);
    cust_assert_eq(cstd_strlen("hello, cstd"), (usize)11);
    return 0;
}

[[cust::test]] int test_memcmp_equal_and_diff(void) {
    cust_assert_eq(cstd_memcmp("abc", "abc", 3), 0);
    /* Sign of memcmp is implementation-defined, only the
     * sign-vs-zero invariant is portable. */
    cust_assert(cstd_memcmp("abc", "abd", 3) != 0);
    return 0;
}
