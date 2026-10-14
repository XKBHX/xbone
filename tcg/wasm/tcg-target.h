/* SPDX-License-Identifier: MIT */
/*
 * Tiny Code Generator for QEMU: WebAssembly host
 *
 * Each translation block becomes a self-contained WebAssembly module with
 * one exported function.  The module is compiled and instantiated from
 * JavaScript the first time the block runs, and the function is added to
 * the main module's indirect function table so that tcg_wasm_tb_exec() can
 * call it like any other function pointer.  See docs/wasm/tcg-wasm-backend.md.
 */

#ifndef TCG_TARGET_H
#define TCG_TARGET_H

#define TCG_TARGET_INSN_UNIT_SIZE 1

/*
 * Every translation block in the buffer may own a live WebAssembly
 * instance, so the buffer size bounds the number of instances.
 */
#define MAX_CODE_GEN_BUFFER_SIZE  (128 * MiB)

/*
 * Registers are WebAssembly locals of the generated function, except for
 * TCG_AREG0 and TCG_REG_CALL_STACK, which are its two parameters.
 */
#define TCG_TARGET_NB_REGS 32

typedef enum {
    TCG_REG_R0 = 0,
    TCG_REG_R1,
    TCG_REG_R2,
    TCG_REG_R3,
    TCG_REG_R4,
    TCG_REG_R5,
    TCG_REG_R6,
    TCG_REG_R7,
    TCG_REG_R8,
    TCG_REG_R9,
    TCG_REG_R10,
    TCG_REG_R11,
    TCG_REG_R12,
    TCG_REG_R13,
    TCG_REG_R14,
    TCG_REG_R15,
    TCG_REG_R16,
    TCG_REG_R17,
    TCG_REG_R18,
    TCG_REG_R19,
    TCG_REG_R20,
    TCG_REG_R21,
    TCG_REG_R22,
    TCG_REG_R23,
    TCG_REG_R24,
    TCG_REG_R25,
    TCG_REG_R26,
    TCG_REG_R27,
    TCG_REG_R28,
    TCG_REG_R29,
    TCG_REG_R30,
    TCG_REG_R31,

    TCG_AREG0 = TCG_REG_R30,
    TCG_REG_CALL_STACK = TCG_REG_R31,
} TCGReg;

/*
 * WebAssembly only has structured control flow, so the backend needs to
 * know where each label is placed and when the translation block ends:
 * see tcg_out_set_label() and tcg_out_tb_end().
 */
#define TCG_TARGET_STRUCTURED_CONTROL 1

#endif /* TCG_TARGET_H */
