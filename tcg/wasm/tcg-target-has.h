/* SPDX-License-Identifier: MIT */
/*
 * Define target-specific opcode support
 */

#ifndef TCG_TARGET_HAS_H
#define TCG_TARGET_HAS_H

#define TCG_TARGET_HAS_extr_i64_i32     0
#define TCG_TARGET_HAS_qemu_ldst_i128   0
#define TCG_TARGET_HAS_tst              1

#define TCG_TARGET_extract_valid(type, ofs, len)   1
#define TCG_TARGET_sextract_valid(type, ofs, len)  1
#define TCG_TARGET_deposit_valid(type, ofs, len)   1

#endif
