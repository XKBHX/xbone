/* SPDX-License-Identifier: MIT */
/*
 * Define target-specific register size
 */

#ifndef TCG_TARGET_REG_BITS_H
#define TCG_TARGET_REG_BITS_H

#if UINTPTR_MAX != UINT32_MAX
# error "The WebAssembly TCG backend only supports wasm32 hosts"
#endif

/*
 * Host pointers are 32 bits, but WebAssembly has native i64 arithmetic,
 * so registers are 64 bits wide and 64-bit guest values need no pairs.
 */
#define TCG_TARGET_REG_BITS 64

#endif
