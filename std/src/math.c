/* std::math — integer primitives.
 *
 * All operations are total and branch-free where it costs nothing.
 * Naming is `<op>_i32` so the same module can grow `_i64` / `_u32`
 * companions without colliding.
 *
 * Signatures use std's own `i32` alias (declared in the sibling
 * `types` module), not `int32_t` — the generated `std.h` should
 * not depend on `<stdint.h>`.
 */

#cust use crate::types;

[[cust::pub]] i32 cstd_min_i32(i32 a, i32 b) {
    return a < b ? a : b;
}

[[cust::pub]] i32 cstd_max_i32(i32 a, i32 b) {
    return a > b ? a : b;
}

[[cust::pub]] i32 cstd_abs_i32(i32 x) {
    /* Avoid the I32_MIN UB hazard of `-x` by masking. */
    i32 mask = x >> 31;
    return (x + mask) ^ mask;
}

[[cust::pub]] i32 cstd_clamp_i32(i32 x, i32 lo, i32 hi) {
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

[[cust::test]] int test_max_basic(void) {
    cust_assert_eq(cstd_max_i32(3, 7), 7);
    cust_assert_eq(cstd_max_i32(7, 3), 7);
    cust_assert_eq(cstd_max_i32(-1, -2), -1);
    return 0;
}

[[cust::test]] int test_min_basic(void) {
    cust_assert_eq(cstd_min_i32(3, 7), 3);
    cust_assert_eq(cstd_min_i32(-1, -2), -2);
    return 0;
}

[[cust::test]] void test_abs_total(void) {
    cust_assert(cstd_abs_i32(0) == 0);
    cust_assert(cstd_abs_i32(7) == 7);
    cust_assert(cstd_abs_i32(-7) == 7);
}

[[cust::test]] int test_clamp_inside_and_outside_range(void) {
    cust_assert_eq(cstd_clamp_i32(5, 0, 10), 5);   /* inside */
    cust_assert_eq(cstd_clamp_i32(-1, 0, 10), 0);  /* below lo */
    cust_assert_eq(cstd_clamp_i32(99, 0, 10), 10); /* above hi */
    return 0;
}
