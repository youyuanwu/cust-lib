/* std::types — Rust-aligned primitive type aliases.
 *
 * Defines `i8`/`i16`/`i32`/`i64`/`u8`/`u16`/`u32`/`u64`/`usize`/
 * `isize`/`f32`/`f64` as `[[cust::pub]] typedef`s so consumers
 * reach them by `#cust use std;` rather than by `#include
 * <stdint.h>` themselves.
 *
 * Implementation note: we use clang's `__INT32_TYPE__` /
 * `__UINT64_TYPE__` / `__SIZE_TYPE__` builtin macros so std
 * itself doesn't have to `#include <stdint.h>`.
 *
 * `bool` is intentionally NOT defined here: it is a C23
 * language keyword. Consumers spell it `bool` directly.
 */

[[cust::pub]] typedef __INT8_TYPE__   i8;
[[cust::pub]] typedef __INT16_TYPE__  i16;
[[cust::pub]] typedef __INT32_TYPE__  i32;
[[cust::pub]] typedef __INT64_TYPE__  i64;

[[cust::pub]] typedef __UINT8_TYPE__  u8;
[[cust::pub]] typedef __UINT16_TYPE__ u16;
[[cust::pub]] typedef __UINT32_TYPE__ u32;
[[cust::pub]] typedef __UINT64_TYPE__ u64;

/* Pointer-sized integers. `usize` is the unsigned address-width
 * type (Rust's `usize` / C's `size_t` / `uintptr_t`); `isize` is
 * the signed counterpart (Rust's `isize` / C's `ssize_t` /
 * `intptr_t`). */
[[cust::pub]] typedef __SIZE_TYPE__   usize;
[[cust::pub]] typedef __INTPTR_TYPE__ isize;

/* IEEE-754 binary32 and binary64. */
[[cust::pub]] typedef float           f32;
[[cust::pub]] typedef double          f64;
