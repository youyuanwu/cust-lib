/* std::geom — a small `[[cust::pub_repr]]` struct that consumers
 * can construct by value plus a function that takes two by-value
 * `point`s.
 *
 * `point_distance_sq` returns the squared Euclidean distance
 * so the implementation stays int-only and we don't have to
 * pull in `<math.h>` for `sqrt`. Consumers that need the real
 * distance take `sqrt((double)point_distance_sq(a, b))`.
 */

#cust use crate::types;

/* `[[cust::pub_repr]]` exports the full struct body into the
 * fragment header and thence the concatenated crate header.
 * C23 attribute placement: `struct [[…]] tag` (not pre-struct). */
struct [[cust::pub_repr]] cstd_point {
    i32 x;
    i32 y;
};

/* Returns (a.x - b.x)^2 + (a.y - b.y)^2. Stays int-only;
 * callers needing the actual distance take sqrt on the
 * return value themselves. */
[[cust::pub]] i32 cstd_point_distance_sq(struct cstd_point a,
                                         struct cstd_point b) {
    i32 dx = a.x - b.x;
    i32 dy = a.y - b.y;
    return dx * dx + dy * dy;
}

[[cust::test]] int test_point_distance_sq_zero(void) {
    struct cstd_point p = {3, 4};
    cust_assert_eq(cstd_point_distance_sq(p, p), 0);
    return 0;
}

[[cust::test]] int test_point_distance_sq_3_4_5(void) {
    struct cstd_point a = {0, 0};
    struct cstd_point b = {3, 4};
    /* The classic 3-4-5 triangle: dx^2 + dy^2 = 9 + 16 = 25. */
    cust_assert_eq(cstd_point_distance_sq(a, b), 25);
    cust_assert_eq(cstd_point_distance_sq(b, a), 25);
    return 0;
}

[[cust::test]] int test_point_distance_sq_negative(void) {
    struct cstd_point a = {-1, -1};
    struct cstd_point b = {2, 3};
    /* dx = -3, dy = -4 → 9 + 16 = 25. Sign cancels in the square. */
    cust_assert_eq(cstd_point_distance_sq(a, b), 25);
    return 0;
}
