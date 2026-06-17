/* std — foundational shared crate.
 *
 * Mirrors (in spirit) Rust's `core`: a small set of obvious
 * building blocks that don't pull in anything ambient.
 *
 * Layout:
 *   src/lib.c    — crate root; declares submodules + std_version()
 *   src/types.c  — Rust-aligned primitive aliases
 *                  (i8/.../i64, u8/.../u64, usize, isize, f32, f64)
 *   src/math.c   — integer min/max/abs/clamp over `i32`
 *   src/mem.c    — strlen / memcmp wrappers, returning `usize`/`i32`
 *   src/geom.c   — `[[cust::pub_repr]] struct point` + distance
 *   src/alloc.c  — pluggable `cstd_alloc` (vtable + system impl)
 *   src/string.c — `cstd_str` view + owned growable `cstd_string`
 */

#cust mod types;
#cust mod math;
#cust mod mem;
#cust mod geom;
#cust mod alloc;
#cust mod string;
#cust mod vec;
#cust mod rc;
#cust mod arc;
#cust mod list;

#cust use crate::types;

/* The cust major/minor this crate was authored against. */
[[cust::pub]] u32 std_version(void) {
    return (0u << 16) | (1u << 8) | 0u; /* 0.1.0 */
}

