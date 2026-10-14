/* SPDX-License-Identifier: MIT */
/*
 * Tiny Code Generator for QEMU: WebAssembly host
 *
 * Layout of the code generated for one translation block, starting at
 * tb->tc.ptr:
 *
 *   TCGWasmTBHeader
 *   unused space                 (the part of the reserve not needed)
 *   module preamble              (written by tcg_out_tb_end)
 *   function body                (written while translating)
 *
 * The preamble (sections for types, imports, the function and its export)
 * depends on the helper signatures used by the body, so space for it is
 * reserved up front and filled in once the body is complete.
 *
 * The function takes (env, frame) and returns the exit_tb value, or
 * TCG_WASM_EXIT_CHAIN to continue with another block.  TCG labels split
 * the body into segments, laid out as
 *
 *   loop $dispatch
 *     block ... block                    one block per later segment
 *       local.get $seg
 *       br_table                         jump to segment $seg
 *     end
 *     segment 0
 *     end
 *     segment 1
 *     ...
 *   end
 *
 * A branch to a later segment is a plain br out of the enclosing blocks.
 * A branch to an earlier one sets $seg and re-enters the dispatch loop.
 *
 * Helpers can start an Asyncify unwind (a coroutine switch through
 * emscripten_fiber_swap).  Asyncify cannot instrument code generated at
 * run time, so the generated function cooperates: every call that may
 * unwind begins a segment, and when the helper returns while unwinding,
 * the function saves its register locals and segment in the frame and
 * returns.  During the rewind tcg_wasm_tb_exec() calls the function again
 * with the same frame; the function restores its locals and re-enters
 * the call's segment, so the helper is called again and continues the
 * rewind.
 */

#include "wasm-runtime.h"

/* Used for function call generation. */
#define TCG_TARGET_CALL_STACK_OFFSET    0
#define TCG_TARGET_STACK_ALIGN          8
#define TCG_TARGET_CALL_ARG_I32         TCG_CALL_ARG_NORMAL
#define TCG_TARGET_CALL_ARG_I64         TCG_CALL_ARG_NORMAL
/*
 * The wasm32 C ABI passes __int128 as two i64 values and returns it via
 * a hidden pointer; the Int128 struct used without CONFIG_INT128 is an
 * aggregate, passed by reference.
 */
#ifdef CONFIG_INT128
#define TCG_TARGET_CALL_ARG_I128        TCG_CALL_ARG_NORMAL
#else
#define TCG_TARGET_CALL_ARG_I128        TCG_CALL_ARG_BY_REF
#endif
#define TCG_TARGET_CALL_RET_I128        TCG_CALL_RET_BY_REF

/* The TLB is addressed from env with an i32.add, so any offset works. */
#define MIN_TLB_MASK_TABLE_OFS  INT_MIN

typedef enum {
    WASM_UNREACHABLE   = 0x00,
    WASM_BLOCK         = 0x02,
    WASM_LOOP          = 0x03,
    WASM_IF            = 0x04,
    WASM_END           = 0x0b,
    WASM_BR            = 0x0c,
    WASM_BR_IF         = 0x0d,
    WASM_BR_TABLE      = 0x0e,
    WASM_RETURN        = 0x0f,
    WASM_CALL          = 0x10,
    WASM_CALL_INDIRECT = 0x11,
    WASM_SELECT        = 0x1b,

    WASM_LOCAL_GET     = 0x20,
    WASM_LOCAL_SET     = 0x21,
    WASM_LOCAL_TEE     = 0x22,

    WASM_I32_LOAD      = 0x28,
    WASM_I64_LOAD      = 0x29,
    WASM_I64_LOAD8_S   = 0x30,
    WASM_I64_LOAD8_U   = 0x31,
    WASM_I64_LOAD16_S  = 0x32,
    WASM_I64_LOAD16_U  = 0x33,
    WASM_I64_LOAD32_S  = 0x34,
    WASM_I64_LOAD32_U  = 0x35,
    WASM_I32_STORE     = 0x36,
    WASM_I64_STORE     = 0x37,
    WASM_I64_STORE8    = 0x3c,
    WASM_I64_STORE16   = 0x3d,
    WASM_I64_STORE32   = 0x3e,

    WASM_I32_CONST     = 0x41,
    WASM_I64_CONST     = 0x42,

    WASM_I32_EQZ       = 0x45,
    WASM_I32_EQ        = 0x46,
    WASM_I32_NE        = 0x47,
    WASM_I32_LT_S      = 0x48,
    WASM_I32_LT_U      = 0x49,
    WASM_I32_GT_S      = 0x4a,
    WASM_I32_GT_U      = 0x4b,
    WASM_I32_LE_S      = 0x4c,
    WASM_I32_LE_U      = 0x4d,
    WASM_I32_GE_S      = 0x4e,
    WASM_I32_GE_U      = 0x4f,
    WASM_I64_EQZ       = 0x50,
    WASM_I64_EQ        = 0x51,
    WASM_I64_NE        = 0x52,
    WASM_I64_LT_S      = 0x53,
    WASM_I64_LT_U      = 0x54,
    WASM_I64_GT_S      = 0x55,
    WASM_I64_GT_U      = 0x56,
    WASM_I64_LE_S      = 0x57,
    WASM_I64_LE_U      = 0x58,
    WASM_I64_GE_S      = 0x59,
    WASM_I64_GE_U      = 0x5a,

    WASM_I32_CLZ       = 0x67,
    WASM_I32_CTZ       = 0x68,
    WASM_I32_POPCNT    = 0x69,
    WASM_I32_ADD       = 0x6a,
    WASM_I32_SUB       = 0x6b,
    WASM_I32_MUL       = 0x6c,
    WASM_I32_DIV_S     = 0x6d,
    WASM_I32_DIV_U     = 0x6e,
    WASM_I32_REM_S     = 0x6f,
    WASM_I32_REM_U     = 0x70,
    WASM_I32_AND       = 0x71,
    WASM_I32_OR        = 0x72,
    WASM_I32_XOR       = 0x73,
    WASM_I32_SHL       = 0x74,
    WASM_I32_SHR_S     = 0x75,
    WASM_I32_SHR_U     = 0x76,
    WASM_I32_ROTL      = 0x77,
    WASM_I32_ROTR      = 0x78,
    WASM_I64_CLZ       = 0x79,
    WASM_I64_CTZ       = 0x7a,
    WASM_I64_POPCNT    = 0x7b,
    WASM_I64_ADD       = 0x7c,
    WASM_I64_SUB       = 0x7d,
    WASM_I64_MUL       = 0x7e,
    WASM_I64_DIV_S     = 0x7f,
    WASM_I64_DIV_U     = 0x80,
    WASM_I64_REM_S     = 0x81,
    WASM_I64_REM_U     = 0x82,
    WASM_I64_AND       = 0x83,
    WASM_I64_OR        = 0x84,
    WASM_I64_XOR       = 0x85,
    WASM_I64_SHL       = 0x86,
    WASM_I64_SHR_S     = 0x87,
    WASM_I64_SHR_U     = 0x88,
    WASM_I64_ROTL      = 0x89,
    WASM_I64_ROTR      = 0x8a,

    WASM_I32_WRAP_I64     = 0xa7,
    WASM_I64_EXTEND_I32_S = 0xac,
    WASM_I64_EXTEND_I32_U = 0xad,
    WASM_I64_EXTEND8_S    = 0xc2,
    WASM_I64_EXTEND16_S   = 0xc3,
    WASM_I64_EXTEND32_S   = 0xc4,
} WasmOpcode;

#define WASM_TYPE_I32      0x7f
#define WASM_TYPE_I64      0x7e
#define WASM_TYPE_FUNCREF  0x70
#define WASM_TYPE_FUNC     0x60
#define WASM_BLOCK_VOID    0x40

/* Function indices: the Asyncify state import, then the block itself. */
#define WASM_FUNC_ASYNCIFY_STATE  0
#define WASM_FUNC_TB              1

/* Type indices fixed by tcg_wasm_out_tb_start; helper types follow. */
#define WASM_TYPEIDX_TB             0
#define WASM_TYPEIDX_ASYNCIFY_STATE 1

/* Asyncify.State.Unwinding in Emscripten's JavaScript runtime. */
#define WASM_ASYNCIFY_UNWINDING  1

/*
 * Local indices: the parameters, one i64 per allocatable register, then
 * scratch locals private to the backend.
 */
#define WASM_L_ENV         0
#define WASM_L_FRAME       1
#define WASM_L_REG0        2
#define WASM_NB_LOCAL_REGS TCG_AREG0
#define WASM_L_SEG         (WASM_L_REG0 + WASM_NB_LOCAL_REGS)
#define WASM_L_T0          (WASM_L_SEG + 1)
#define WASM_L_T1          (WASM_L_SEG + 2)
#define WASM_NB_I32_LOCALS 3
#define WASM_L_X0          (WASM_L_SEG + WASM_NB_I32_LOCALS)
#define WASM_L_X1          (WASM_L_X0 + 1)
#define WASM_L_CARRY       (WASM_L_X0 + 2)
#define WASM_NB_I64_SCRATCH 3

QEMU_BUILD_BUG_ON(TCG_AREG0 != TCG_TARGET_NB_REGS - 2);
QEMU_BUILD_BUG_ON(TCG_REG_CALL_STACK != TCG_TARGET_NB_REGS - 1);

/* Room for the type section entries of the helpers called by one TB. */
#define WASM_MAX_TYPES     16
#define WASM_MAX_PARAMS    (MAX_CALL_IARGS * 2 + 1)
#define WASM_MAX_TYPE_LEN  (4 + WASM_MAX_PARAMS)

/* Upper bound of the preamble written by tcg_wasm_build_preamble. */
#define WASM_PREAMBLE_MAX \
    (8 + 7 + WASM_MAX_TYPES * WASM_MAX_TYPE_LEN + 40 + 4 + 7 + 12)

/* A bound on the segment count, well below the 64k limit on TB size. */
#define WASM_MAX_SEGMENTS  4096

typedef struct TCGWasmGen {
    TCGWasmTBHeader *header;
    tcg_insn_unit *body;
    int *label_seg;
    unsigned nseg;
    unsigned cur;
    bool resumable;
    uint32_t used_regs;
    unsigned ntypes;
    uint8_t type_len[WASM_MAX_TYPES];
    uint8_t types[WASM_MAX_TYPES][WASM_MAX_TYPE_LEN];
} TCGWasmGen;

static __thread TCGWasmGen tcg_wasm_gen;
static bool tcg_wasm_shared_memory;

static TCGConstraintSetIndex
tcg_target_op_def(TCGOpcode op, TCGType type, unsigned flags)
{
    return C_NotImplemented;
}

static const int tcg_target_reg_alloc_order[] = {
    TCG_REG_R1,  TCG_REG_R2,  TCG_REG_R3,  TCG_REG_R4,
    TCG_REG_R5,  TCG_REG_R6,  TCG_REG_R7,  TCG_REG_R8,
    TCG_REG_R9,  TCG_REG_R10, TCG_REG_R11, TCG_REG_R12,
    TCG_REG_R13, TCG_REG_R14, TCG_REG_R15, TCG_REG_R16,
    TCG_REG_R17, TCG_REG_R18, TCG_REG_R19, TCG_REG_R20,
    TCG_REG_R21, TCG_REG_R22, TCG_REG_R23, TCG_REG_R24,
    TCG_REG_R25, TCG_REG_R26, TCG_REG_R27, TCG_REG_R28,
    TCG_REG_R29,
    /* The only call-clobbered register. */
    TCG_REG_R0,
};

/* No call arguments via registers.  All will be stored on the "stack". */
static const int tcg_target_call_iarg_regs[] = { };

static TCGReg tcg_target_call_oarg_reg(TCGCallReturnKind kind, int slot)
{
    tcg_debug_assert(kind == TCG_CALL_RET_NORMAL);
    tcg_debug_assert(slot == 0);
    return TCG_REG_R0;
}

#ifdef CONFIG_DEBUG_TCG
static const char *const tcg_target_reg_names[TCG_TARGET_NB_REGS] = {
    "r0",  "r1",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
    "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
    "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
    "r24", "r25", "r26", "r27", "r28", "r29", "env", "frame",
};
#endif

static bool patch_reloc(tcg_insn_unit *code_ptr, int type,
                        intptr_t value, intptr_t addend)
{
    /* Branches are resolved through tcg_wasm_gen.label_seg instead. */
    g_assert_not_reached();
}

/*
 * Encoding primitives.
 */

static void tcg_wasm_out_uleb(TCGContext *s, uint64_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        tcg_out8(s, b | (v ? 0x80 : 0));
    } while (v);
}

static void tcg_wasm_out_sleb(TCGContext *s, int64_t v)
{
    bool more;

    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        more = !((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)));
        tcg_out8(s, b | (more ? 0x80 : 0));
    } while (more);
}

static uint8_t *tcg_wasm_put_uleb(uint8_t *p, uint32_t v)
{
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        *p++ = b | (v ? 0x80 : 0);
    } while (v);
    return p;
}

static unsigned tcg_wasm_uleb_len(uint32_t v)
{
    uint8_t tmp[5];
    return tcg_wasm_put_uleb(tmp, v) - tmp;
}

/* Write a 5-byte signed LEB128, so that the value can be patched later. */
static void tcg_wasm_patch_sleb5(tcg_insn_unit *p, int32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = ((v >> (7 * i)) & 0x7f) | 0x80;
    }
    p[4] = (v >> 28) & 0x7f;
}

static void tcg_wasm_out_op(TCGContext *s, WasmOpcode op)
{
    tcg_out8(s, op);
}

static void tcg_wasm_out_op_idx(TCGContext *s, WasmOpcode op, uint32_t idx)
{
    tcg_out8(s, op);
    tcg_wasm_out_uleb(s, idx);
}

static void tcg_wasm_out_i32_const(TCGContext *s, int32_t v)
{
    tcg_out8(s, WASM_I32_CONST);
    tcg_wasm_out_sleb(s, v);
}

static void tcg_wasm_out_i64_const(TCGContext *s, int64_t v)
{
    tcg_out8(s, WASM_I64_CONST);
    tcg_wasm_out_sleb(s, v);
}

/* Emit i32.const with a placeholder, returning where to patch it. */
static tcg_insn_unit *tcg_wasm_out_i32_const_patchable(TCGContext *s)
{
    tcg_insn_unit *p;

    tcg_out8(s, WASM_I32_CONST);
    p = s->code_ptr;
    tcg_wasm_patch_sleb5(p, 0);
    s->code_ptr += 5;
    return p;
}

/* Memory access; alignment hints are left at 0, as accesses may be unaligned. */
static void tcg_wasm_out_mem(TCGContext *s, WasmOpcode op, uint32_t offset)
{
    tcg_out8(s, op);
    tcg_out8(s, 0);
    tcg_wasm_out_uleb(s, offset);
}

static void tcg_wasm_out_block(TCGContext *s, WasmOpcode op)
{
    tcg_out8(s, op);
    tcg_out8(s, WASM_BLOCK_VOID);
}

/*
 * Registers.  Values of TCG_TYPE_I32 live in the low half of the i64
 * locals; the high half is unspecified, as on other 64-bit hosts.
 */

static bool tcg_wasm_reg_is_param(TCGReg r)
{
    return r == TCG_AREG0 || r == TCG_REG_CALL_STACK;
}

static uint32_t tcg_wasm_reg_local(TCGReg r)
{
    if (r == TCG_AREG0) {
        return WASM_L_ENV;
    }
    if (r == TCG_REG_CALL_STACK) {
        return WASM_L_FRAME;
    }
    tcg_wasm_gen.used_regs |= 1u << r;
    return WASM_L_REG0 + r;
}

static void tcg_wasm_out_get64(TCGContext *s, TCGReg r)
{
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, tcg_wasm_reg_local(r));
    if (tcg_wasm_reg_is_param(r)) {
        tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
    }
}

static void tcg_wasm_out_get32(TCGContext *s, TCGReg r)
{
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, tcg_wasm_reg_local(r));
    if (!tcg_wasm_reg_is_param(r)) {
        tcg_wasm_out_op(s, WASM_I32_WRAP_I64);
    }
}

static void tcg_wasm_out_set64(TCGContext *s, TCGReg r)
{
    tcg_debug_assert(!tcg_wasm_reg_is_param(r));
    tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, tcg_wasm_reg_local(r));
}

static void tcg_wasm_out_set32(TCGContext *s, TCGReg r)
{
    tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
    tcg_wasm_out_set64(s, r);
}

/* Push a register or constant operand. */
static void tcg_wasm_out_arg64(TCGContext *s, TCGArg a, bool const_a)
{
    if (const_a) {
        tcg_wasm_out_i64_const(s, a);
    } else {
        tcg_wasm_out_get64(s, a);
    }
}

static void tcg_wasm_out_arg32(TCGContext *s, TCGArg a, bool const_a)
{
    if (const_a) {
        tcg_wasm_out_i32_const(s, a);
    } else {
        tcg_wasm_out_get32(s, a);
    }
}

/*
 * Segments and branches.
 */

static unsigned tcg_wasm_depth_dispatch(void)
{
    return tcg_wasm_gen.nseg - 1 - tcg_wasm_gen.cur;
}

static unsigned tcg_wasm_depth_save(void)
{
    tcg_debug_assert(tcg_wasm_gen.resumable);
    return tcg_wasm_depth_dispatch() + 2;
}

static void tcg_wasm_out_new_segment(TCGContext *s)
{
    tcg_wasm_out_op(s, WASM_END);
    tcg_wasm_gen.cur++;
    tcg_debug_assert(tcg_wasm_gen.cur < tcg_wasm_gen.nseg);
}

static void tcg_out_set_label(TCGContext *s, TCGLabel *l)
{
    tcg_wasm_out_new_segment(s);
    tcg_debug_assert(tcg_wasm_gen.label_seg[l->id] == tcg_wasm_gen.cur);
}

/* Branch to @l, consuming an i32 condition from the stack if @cond. */
static void tcg_wasm_out_branch(TCGContext *s, TCGLabel *l, bool cond)
{
    unsigned target = tcg_wasm_gen.label_seg[l->id];

    if (target > tcg_wasm_gen.cur) {
        tcg_wasm_out_op_idx(s, cond ? WASM_BR_IF : WASM_BR,
                            target - tcg_wasm_gen.cur - 1);
        return;
    }
    if (cond) {
        tcg_wasm_out_block(s, WASM_IF);
    }
    tcg_wasm_out_i32_const(s, target);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_SEG);
    tcg_wasm_out_op_idx(s, WASM_BR, tcg_wasm_depth_dispatch() + cond);
    if (cond) {
        tcg_wasm_out_op(s, WASM_END);
    }
}

/*
 * After a call that began the current segment: if the callee started an
 * Asyncify unwind, save the locals and return; see the top of this file.
 * The call's result, if any, stays on the operand stack.
 */
static void tcg_wasm_out_unwind_check(TCGContext *s)
{
    tcg_wasm_out_op_idx(s, WASM_CALL, WASM_FUNC_ASYNCIFY_STATE);
    tcg_wasm_out_i32_const(s, WASM_ASYNCIFY_UNWINDING);
    tcg_wasm_out_op(s, WASM_I32_EQ);
    tcg_wasm_out_block(s, WASM_IF);
    tcg_wasm_out_i32_const(s, tcg_wasm_gen.cur);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_SEG);
    tcg_wasm_out_op_idx(s, WASM_BR, tcg_wasm_depth_save() + 1);
    tcg_wasm_out_op(s, WASM_END);
}

/* Return the type index for a function type, adding it if needed. */
static uint32_t tcg_wasm_type(TCGContext *s, const uint8_t *params,
                              unsigned nparams, uint8_t result)
{
    TCGWasmGen *g = &tcg_wasm_gen;
    uint8_t t[WASM_MAX_TYPE_LEN];
    unsigned len = 0;

    tcg_debug_assert(nparams <= WASM_MAX_PARAMS);
    t[len++] = WASM_TYPE_FUNC;
    t[len++] = nparams;
    memcpy(t + len, params, nparams);
    len += nparams;
    t[len++] = result != 0;
    if (result) {
        t[len++] = result;
    }

    for (unsigned i = 0; i < g->ntypes; i++) {
        if (g->type_len[i] == len && !memcmp(g->types[i], t, len)) {
            return i;
        }
    }
    if (g->ntypes == WASM_MAX_TYPES) {
        tcg_raise_tb_overflow(s);
    }
    memcpy(g->types[g->ntypes], t, len);
    g->type_len[g->ntypes] = len;
    return g->ntypes++;
}

static void tcg_wasm_out_call_indirect(TCGContext *s, const void *func,
                                       uint32_t typeidx)
{
    tcg_wasm_out_i32_const(s, (uintptr_t)func);
    tcg_wasm_out_op_idx(s, WASM_CALL_INDIRECT, typeidx);
    tcg_out8(s, 0);
}

/* The host address of the current point of the code, for unwinding. */
static int32_t tcg_wasm_code_addr(TCGContext *s)
{
    return (uintptr_t)tcg_splitwx_to_rx(s->code_ptr);
}

/*
 * Host memory accesses with a base register and offset.
 */

static uint32_t tcg_wasm_out_base(TCGContext *s, TCGReg base, intptr_t offset)
{
    tcg_wasm_out_get32(s, base);
    if (offset < 0) {
        tcg_wasm_out_i32_const(s, offset);
        tcg_wasm_out_op(s, WASM_I32_ADD);
        return 0;
    }
    return offset;
}

static void tcg_wasm_out_ld(TCGContext *s, WasmOpcode op, TCGReg dest,
                            TCGReg base, intptr_t offset)
{
    uint32_t ofs = tcg_wasm_out_base(s, base, offset);
    tcg_wasm_out_mem(s, op, ofs);
    tcg_wasm_out_set64(s, dest);
}

static void tcg_wasm_out_st(TCGContext *s, WasmOpcode op, TCGReg data,
                            TCGReg base, intptr_t offset)
{
    uint32_t ofs = tcg_wasm_out_base(s, base, offset);
    tcg_wasm_out_get64(s, data);
    tcg_wasm_out_mem(s, op, ofs);
}

static void tcg_out_ld(TCGContext *s, TCGType type, TCGReg val, TCGReg base,
                       intptr_t offset)
{
    tcg_wasm_out_ld(s, type == TCG_TYPE_I32 ? WASM_I64_LOAD32_U
                                            : WASM_I64_LOAD,
                    val, base, offset);
}

static void tcg_out_st(TCGContext *s, TCGType type, TCGReg val, TCGReg base,
                       intptr_t offset)
{
    tcg_wasm_out_st(s, type == TCG_TYPE_I32 ? WASM_I64_STORE32
                                            : WASM_I64_STORE,
                    val, base, offset);
}

static inline bool tcg_out_sti(TCGContext *s, TCGType type, TCGArg val,
                               TCGReg base, intptr_t ofs)
{
    return false;
}

static bool tcg_out_mov(TCGContext *s, TCGType type, TCGReg ret, TCGReg arg)
{
    if (ret != arg) {
        tcg_wasm_out_get64(s, arg);
        tcg_wasm_out_set64(s, ret);
    }
    return true;
}

static void tcg_out_movi(TCGContext *s, TCGType type,
                         TCGReg ret, tcg_target_long arg)
{
    switch (type) {
    case TCG_TYPE_I32:
        arg = (int32_t)arg;
        break;
    case TCG_TYPE_I64:
        break;
    default:
        g_assert_not_reached();
    }
    tcg_wasm_out_i64_const(s, arg);
    tcg_wasm_out_set64(s, ret);
}

static void tcg_out_extract(TCGContext *s, TCGType type, TCGReg rd,
                            TCGReg rs, unsigned pos, unsigned len)
{
    tcg_wasm_out_get64(s, rs);
    if (pos) {
        tcg_wasm_out_i64_const(s, pos);
        tcg_wasm_out_op(s, WASM_I64_SHR_U);
    }
    if (pos + len < 64) {
        tcg_wasm_out_i64_const(s, MAKE_64BIT_MASK(0, len));
        tcg_wasm_out_op(s, WASM_I64_AND);
    }
    tcg_wasm_out_set64(s, rd);
}

static const TCGOutOpExtract outop_extract = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tcg_out_extract,
};

static void tcg_out_sextract(TCGContext *s, TCGType type, TCGReg rd,
                             TCGReg rs, unsigned pos, unsigned len)
{
    tcg_wasm_out_get64(s, rs);
    if (pos == 0 && len == 8) {
        tcg_wasm_out_op(s, WASM_I64_EXTEND8_S);
    } else if (pos == 0 && len == 16) {
        tcg_wasm_out_op(s, WASM_I64_EXTEND16_S);
    } else if (pos == 0 && len == 32) {
        tcg_wasm_out_op(s, WASM_I64_EXTEND32_S);
    } else {
        if (pos + len < 64) {
            tcg_wasm_out_i64_const(s, 64 - pos - len);
            tcg_wasm_out_op(s, WASM_I64_SHL);
        }
        tcg_wasm_out_i64_const(s, 64 - len);
        tcg_wasm_out_op(s, WASM_I64_SHR_S);
    }
    tcg_wasm_out_set64(s, rd);
}

static const TCGOutOpExtract outop_sextract = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tcg_out_sextract,
};

static const TCGOutOpExtract2 outop_extract2 = {
    .base.static_constraint = C_NotImplemented,
};

static void tcg_out_ext8s(TCGContext *s, TCGType type, TCGReg rd, TCGReg rs)
{
    tcg_out_sextract(s, type, rd, rs, 0, 8);
}

static void tcg_out_ext8u(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_extract(s, TCG_TYPE_REG, rd, rs, 0, 8);
}

static void tcg_out_ext16s(TCGContext *s, TCGType type, TCGReg rd, TCGReg rs)
{
    tcg_out_sextract(s, type, rd, rs, 0, 16);
}

static void tcg_out_ext16u(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_extract(s, TCG_TYPE_REG, rd, rs, 0, 16);
}

static void tcg_out_ext32s(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_sextract(s, TCG_TYPE_I64, rd, rs, 0, 32);
}

static void tcg_out_ext32u(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_wasm_out_get32(s, rs);
    tcg_wasm_out_set32(s, rd);
}

static void tcg_out_exts_i32_i64(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_ext32s(s, rd, rs);
}

static void tcg_out_extu_i32_i64(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_ext32u(s, rd, rs);
}

static void tcg_out_extrl_i64_i32(TCGContext *s, TCGReg rd, TCGReg rs)
{
    tcg_out_mov(s, TCG_TYPE_I32, rd, rs);
}

static bool tcg_out_xchg(TCGContext *s, TCGType type, TCGReg r1, TCGReg r2)
{
    return false;
}

static void tcg_out_addi_ptr(TCGContext *s, TCGReg rd, TCGReg rs,
                             tcg_target_long imm)
{
    tcg_wasm_out_get32(s, rs);
    tcg_wasm_out_i32_const(s, imm);
    tcg_wasm_out_op(s, WASM_I32_ADD);
    tcg_wasm_out_set32(s, rd);
}

/*
 * Helper calls.  Arguments were stored to the stack slots of the frame;
 * load them back onto the operand stack in the wasm32 C ABI order.
 */

static uint8_t tcg_wasm_typecode_type(unsigned typecode)
{
    switch (typecode) {
    case dh_typecode_i64:
    case dh_typecode_s64:
    case dh_typecode_i128:      /* one half, as TCG_CALL_ARG_NORMAL */
        return WASM_TYPE_I64;
    case dh_typecode_i32:
    case dh_typecode_s32:
    case dh_typecode_ptr:
        return WASM_TYPE_I32;
    }
    g_assert_not_reached();
}

static void tcg_wasm_out_ld_arg_slot(TCGContext *s, unsigned slot,
                                     uint8_t type)
{
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_FRAME);
    tcg_wasm_out_mem(s, type == WASM_TYPE_I64 ? WASM_I64_LOAD : WASM_I32_LOAD,
                     TCG_TARGET_CALL_STACK_OFFSET
                     + slot * sizeof(tcg_target_long));
}

static void tcg_out_call(TCGContext *s, const tcg_insn_unit *func,
                         const TCGHelperInfo *info)
{
    bool resumable = !(info->flags & TCG_CALL_NO_SIDE_EFFECTS);
    unsigned ret = info->typemask & 7;
    uint8_t params[WASM_MAX_PARAMS];
    unsigned nparams = 0;
    uint8_t result = 0;
    tcg_insn_unit *ra;

    if (resumable) {
        tcg_wasm_out_new_segment(s);
    }

    /* GETPC() reads the return address from the frame. */
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_FRAME);
    ra = tcg_wasm_out_i32_const_patchable(s);
    tcg_wasm_out_mem(s, WASM_I32_STORE, offsetof(TCGWasmFrame, ra));

    if (info->out_kind == TCG_CALL_RET_BY_REF) {
        params[nparams++] = WASM_TYPE_I32;
        tcg_wasm_out_ld_arg_slot(s, 0, WASM_TYPE_I32);
    }
    for (unsigned i = 0; i < info->nr_in; i++) {
        const TCGCallArgumentLoc *loc = &info->in[i];
        unsigned typecode = extract32(info->typemask, (loc->arg_idx + 1) * 3, 3);
        uint8_t type;

        switch (loc->kind) {
        case TCG_CALL_ARG_NORMAL:
            type = tcg_wasm_typecode_type(typecode);
            break;
        case TCG_CALL_ARG_BY_REF:
            type = WASM_TYPE_I32;
            break;
        case TCG_CALL_ARG_BY_REF_N:
            continue;
        default:
            g_assert_not_reached();
        }
        params[nparams++] = type;
        tcg_wasm_out_ld_arg_slot(s, loc->arg_slot, type);
    }

    if (ret != dh_typecode_void && ret != dh_typecode_noreturn &&
        info->out_kind == TCG_CALL_RET_NORMAL) {
        result = tcg_wasm_typecode_type(ret);
    }
    tcg_wasm_out_call_indirect(s, func,
                               tcg_wasm_type(s, params, nparams, result));
    tcg_wasm_patch_sleb5(ra, tcg_wasm_code_addr(s));

    if (resumable) {
        tcg_wasm_out_unwind_check(s);
    }
    if (result == WASM_TYPE_I32) {
        tcg_wasm_out_set32(s, TCG_REG_R0);
    } else if (result == WASM_TYPE_I64) {
        tcg_wasm_out_set64(s, TCG_REG_R0);
    }
}

/*
 * Control transfer out of the translation block.
 */

static void tcg_out_exit_tb(TCGContext *s, uintptr_t arg)
{
    tcg_wasm_out_i32_const(s, arg);
    tcg_wasm_out_op(s, WASM_RETURN);
}

/* Return TCG_WASM_EXIT_CHAIN, continuing with the code pointer in T0. */
static void tcg_wasm_out_chain_t0(TCGContext *s)
{
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_FRAME);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T0);
    tcg_wasm_out_mem(s, WASM_I32_STORE, offsetof(TCGWasmFrame, next));
    tcg_wasm_out_i32_const(s, TCG_WASM_EXIT_CHAIN);
    tcg_wasm_out_op(s, WASM_RETURN);
}

static void tcg_out_goto_tb(TCGContext *s, int which)
{
    tcg_insn_unit *reset;

    /*
     * Indirect jump method: chain to jmp_target_addr unless it still
     * holds the reset address, which is the code that follows.
     */
    tcg_wasm_out_i32_const(s, get_jmp_target_addr(s, which));
    tcg_wasm_out_mem(s, WASM_I32_LOAD, 0);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_T0);
    reset = tcg_wasm_out_i32_const_patchable(s);
    tcg_wasm_out_op(s, WASM_I32_NE);
    tcg_wasm_out_block(s, WASM_IF);
    tcg_wasm_out_chain_t0(s);
    tcg_wasm_out_op(s, WASM_END);

    set_jmp_reset_offset(s, which);
    tcg_wasm_patch_sleb5(reset, tcg_wasm_code_addr(s));
}

static void tcg_out_goto_ptr(TCGContext *s, TCGReg a0)
{
    tcg_wasm_out_get32(s, a0);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_T0);
    tcg_wasm_out_i32_const(s, (uintptr_t)tcg_code_gen_epilogue);
    tcg_wasm_out_op(s, WASM_I32_EQ);
    tcg_wasm_out_block(s, WASM_IF);
    tcg_wasm_out_i32_const(s, 0);
    tcg_wasm_out_op(s, WASM_RETURN);
    tcg_wasm_out_op(s, WASM_END);
    tcg_wasm_out_chain_t0(s);
}

void tb_target_set_jmp_target(const TranslationBlock *tb, int n,
                              uintptr_t jmp_rx, uintptr_t jmp_rw)
{
    /* Always indirect, nothing to do */
}

/*
 * Integer operations.
 */

/*
 * a0 = a1 op a2.  Operations that are "narrow" depend on the high half
 * of their inputs and are done in i32 for TCG_TYPE_I32; others give the
 * right low half when done in i64.
 */
static void tcg_wasm_out_binary(TCGContext *s, TCGType type, bool narrow,
                                WasmOpcode op32, WasmOpcode op64,
                                TCGReg a0, TCGReg a1, TCGArg a2, bool const_a2)
{
    if (narrow && type == TCG_TYPE_I32) {
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_arg32(s, a2, const_a2);
        tcg_wasm_out_op(s, op32);
        tcg_wasm_out_set32(s, a0);
    } else {
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_arg64(s, a2, const_a2);
        tcg_wasm_out_op(s, op64);
        tcg_wasm_out_set64(s, a0);
    }
}

/* Push ~x for the i64 on top of the stack. */
static void tcg_wasm_out_not(TCGContext *s)
{
    tcg_wasm_out_i64_const(s, -1);
    tcg_wasm_out_op(s, WASM_I64_XOR);
}

static void tgen_add(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_ADD, a0, a1, a2, false);
}

static void tgen_addi(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_ADD, a0, a1, a2, true);
}

static const TCGOutOpBinary outop_add = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_add,
    .out_rri = tgen_addi,
};

/*
 * Carry and borrow live in the CARRY local as 0 or 1.  The register
 * allocator never keeps them live across a helper call.
 */

static void tgen_addco(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, TCGReg a2)
{
    if (type == TCG_TYPE_I32) {
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_get32(s, a2);
        tcg_wasm_out_op(s, WASM_I32_ADD);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_T0);
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_op(s, WASM_I32_LT_U);
        tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_CARRY);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T0);
        tcg_wasm_out_set32(s, a0);
    } else {
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_get64(s, a2);
        tcg_wasm_out_op(s, WASM_I64_ADD);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_X0);
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_op(s, WASM_I64_LT_U);
        tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_CARRY);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_X0);
        tcg_wasm_out_set64(s, a0);
    }
}

static const TCGOutOpBinary outop_addco = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_addco,
};

static void tgen_addci(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_get64(s, a2);
    tcg_wasm_out_op(s, WASM_I64_ADD);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_CARRY);
    tcg_wasm_out_op(s, WASM_I64_ADD);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpAddSubCarry outop_addci = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_addci,
};

static void tgen_addcio(TCGContext *s, TCGType type,
                        TCGReg a0, TCGReg a1, TCGReg a2)
{
    /* t = a1 + a2; r = t + carry; carry = (t < a1) | (r < t) */
    if (type == TCG_TYPE_I32) {
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_get32(s, a2);
        tcg_wasm_out_op(s, WASM_I32_ADD);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_T0);
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_op(s, WASM_I32_LT_U);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T0);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_CARRY);
        tcg_wasm_out_op(s, WASM_I32_WRAP_I64);
        tcg_wasm_out_op(s, WASM_I32_ADD);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_T1);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T0);
        tcg_wasm_out_op(s, WASM_I32_LT_U);
        tcg_wasm_out_op(s, WASM_I32_OR);
        tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_CARRY);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T1);
        tcg_wasm_out_set32(s, a0);
    } else {
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_get64(s, a2);
        tcg_wasm_out_op(s, WASM_I64_ADD);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_X0);
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_op(s, WASM_I64_LT_U);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_X0);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_CARRY);
        tcg_wasm_out_op(s, WASM_I64_ADD);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_X1);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_X0);
        tcg_wasm_out_op(s, WASM_I64_LT_U);
        tcg_wasm_out_op(s, WASM_I32_OR);
        tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_CARRY);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_X1);
        tcg_wasm_out_set64(s, a0);
    }
}

static const TCGOutOpBinary outop_addcio = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_addcio,
};

static void tcg_out_set_carry(TCGContext *s)
{
    tcg_wasm_out_i64_const(s, 1);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_CARRY);
}

static void tgen_and(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_AND, a0, a1, a2, false);
}

static void tgen_andi(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_AND, a0, a1, a2, true);
}

static const TCGOutOpBinary outop_and = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_and,
    .out_rri = tgen_andi,
};

static void tgen_andc(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_get64(s, a2);
    tcg_wasm_out_not(s);
    tcg_wasm_out_op(s, WASM_I64_AND);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpBinary outop_andc = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_andc,
};

/* a0 = a1 != 0 ? op(a1) : a2 */
static void tcg_wasm_out_count_zeros(TCGContext *s, TCGType type,
                                     WasmOpcode op32, WasmOpcode op64,
                                     TCGReg a0, TCGReg a1,
                                     TCGArg a2, bool const_a2)
{
    if (type == TCG_TYPE_I32) {
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_op(s, op32);
        tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
        tcg_wasm_out_arg64(s, a2, const_a2);
        tcg_wasm_out_get32(s, a1);
    } else {
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_op(s, op64);
        tcg_wasm_out_arg64(s, a2, const_a2);
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_op(s, WASM_I64_EQZ);
        tcg_wasm_out_op(s, WASM_I32_EQZ);
    }
    tcg_wasm_out_op(s, WASM_SELECT);
    tcg_wasm_out_set64(s, a0);
}

static void tgen_clz(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_count_zeros(s, type, WASM_I32_CLZ, WASM_I64_CLZ,
                             a0, a1, a2, false);
}

static void tgen_clzi(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_count_zeros(s, type, WASM_I32_CLZ, WASM_I64_CLZ,
                             a0, a1, a2, true);
}

static const TCGOutOpBinary outop_clz = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_clz,
    .out_rri = tgen_clzi,
};

static void tgen_ctz(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_count_zeros(s, type, WASM_I32_CTZ, WASM_I64_CTZ,
                             a0, a1, a2, false);
}

static void tgen_ctzi(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_count_zeros(s, type, WASM_I32_CTZ, WASM_I64_CTZ,
                             a0, a1, a2, true);
}

static const TCGOutOpBinary outop_ctz = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_ctz,
    .out_rri = tgen_ctzi,
};

static void tgen_deposit(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1,
                         TCGReg a2, unsigned ofs, unsigned len)
{
    uint64_t mask = MAKE_64BIT_MASK(0, len);

    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_i64_const(s, ~(mask << ofs));
    tcg_wasm_out_op(s, WASM_I64_AND);
    tcg_wasm_out_get64(s, a2);
    if (ofs + len < 64) {
        tcg_wasm_out_i64_const(s, mask);
        tcg_wasm_out_op(s, WASM_I64_AND);
    }
    if (ofs) {
        tcg_wasm_out_i64_const(s, ofs);
        tcg_wasm_out_op(s, WASM_I64_SHL);
    }
    tcg_wasm_out_op(s, WASM_I64_OR);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpDeposit outop_deposit = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_deposit,
};

/*
 * Division by zero and INT_MIN / -1 trap in WebAssembly.  Guests that can
 * divide by zero test for it before using these opcodes, as TCG requires.
 */

static void tgen_divs(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_DIV_S, WASM_I64_DIV_S,
                        a0, a1, a2, false);
}

static const TCGOutOpBinary outop_divs = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_divs,
};

static const TCGOutOpDivRem outop_divs2 = {
    .base.static_constraint = C_NotImplemented,
};

static void tgen_divu(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_DIV_U, WASM_I64_DIV_U,
                        a0, a1, a2, false);
}

static const TCGOutOpBinary outop_divu = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_divu,
};

static const TCGOutOpDivRem outop_divu2 = {
    .base.static_constraint = C_NotImplemented,
};

static void tgen_eqv(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_get64(s, a2);
    tcg_wasm_out_op(s, WASM_I64_XOR);
    tcg_wasm_out_not(s);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpBinary outop_eqv = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_eqv,
};

static void tgen_extrh_i64_i32(TCGContext *s, TCGType t, TCGReg a0, TCGReg a1)
{
    tcg_out_extract(s, TCG_TYPE_I64, a0, a1, 32, 32);
}

static const TCGOutOpUnary outop_extrh_i64_i32 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_extrh_i64_i32,
};

static void tgen_mul(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_MUL, a0, a1, a2, false);
}

static void tgen_muli(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_MUL, a0, a1, a2, true);
}

static const TCGOutOpBinary outop_mul = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_mul,
    .out_rri = tgen_muli,
};

/*
 * The 64-bit product of two 32-bit values fits an i64.  There is no
 * 64x64->128 multiply in WebAssembly, so the I64 forms are left to the
 * generic expansion.
 */
static TCGConstraintSetIndex cset_mul2(TCGType type, unsigned flags)
{
    return type == TCG_TYPE_I32 ? C_O2_I2(r, r, r, r) : C_NotImplemented;
}

static TCGConstraintSetIndex cset_mulh(TCGType type, unsigned flags)
{
    return type == TCG_TYPE_I32 ? C_O1_I2(r, r, r) : C_NotImplemented;
}

/* Push the 64-bit product of the low halves of a1 and a2. */
static void tcg_wasm_out_mul32x32(TCGContext *s, bool sign,
                                  TCGReg a1, TCGReg a2)
{
    WasmOpcode ext = sign ? WASM_I64_EXTEND_I32_S : WASM_I64_EXTEND_I32_U;

    tcg_wasm_out_get32(s, a1);
    tcg_wasm_out_op(s, ext);
    tcg_wasm_out_get32(s, a2);
    tcg_wasm_out_op(s, ext);
    tcg_wasm_out_op(s, WASM_I64_MUL);
}

static void tcg_wasm_out_mul2(TCGContext *s, bool sign, TCGReg lo, TCGReg hi,
                              TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_mul32x32(s, sign, a1, a2);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_X0);
    tcg_wasm_out_set64(s, lo);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_X0);
    tcg_wasm_out_i64_const(s, 32);
    tcg_wasm_out_op(s, sign ? WASM_I64_SHR_S : WASM_I64_SHR_U);
    tcg_wasm_out_set64(s, hi);
}

static void tgen_muls2(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, TCGReg a2, TCGReg a3)
{
    tcg_wasm_out_mul2(s, true, a0, a1, a2, a3);
}

static const TCGOutOpMul2 outop_muls2 = {
    .base.static_constraint = C_Dynamic,
    .base.dynamic_constraint = cset_mul2,
    .out_rrrr = tgen_muls2,
};

static void tgen_mulsh(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_mul32x32(s, true, a1, a2);
    tcg_wasm_out_i64_const(s, 32);
    tcg_wasm_out_op(s, WASM_I64_SHR_S);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpBinary outop_mulsh = {
    .base.static_constraint = C_Dynamic,
    .base.dynamic_constraint = cset_mulh,
    .out_rrr = tgen_mulsh,
};

static void tgen_mulu2(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, TCGReg a2, TCGReg a3)
{
    tcg_wasm_out_mul2(s, false, a0, a1, a2, a3);
}

static const TCGOutOpMul2 outop_mulu2 = {
    .base.static_constraint = C_Dynamic,
    .base.dynamic_constraint = cset_mul2,
    .out_rrrr = tgen_mulu2,
};

static void tgen_muluh(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_mul32x32(s, false, a1, a2);
    tcg_wasm_out_i64_const(s, 32);
    tcg_wasm_out_op(s, WASM_I64_SHR_U);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpBinary outop_muluh = {
    .base.static_constraint = C_Dynamic,
    .base.dynamic_constraint = cset_mulh,
    .out_rrr = tgen_muluh,
};

static void tgen_nand(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_get64(s, a2);
    tcg_wasm_out_op(s, WASM_I64_AND);
    tcg_wasm_out_not(s);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpBinary outop_nand = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_nand,
};

static void tgen_nor(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_get64(s, a2);
    tcg_wasm_out_op(s, WASM_I64_OR);
    tcg_wasm_out_not(s);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpBinary outop_nor = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_nor,
};

static void tgen_or(TCGContext *s, TCGType type,
                    TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_OR, a0, a1, a2, false);
}

static void tgen_ori(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_OR, a0, a1, a2, true);
}

static const TCGOutOpBinary outop_or = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_or,
    .out_rri = tgen_ori,
};

static void tgen_orc(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_get64(s, a2);
    tcg_wasm_out_not(s);
    tcg_wasm_out_op(s, WASM_I64_OR);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpBinary outop_orc = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_orc,
};

static void tgen_rems(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_REM_S, WASM_I64_REM_S,
                        a0, a1, a2, false);
}

static const TCGOutOpBinary outop_rems = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_rems,
};

static void tgen_remu(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_REM_U, WASM_I64_REM_U,
                        a0, a1, a2, false);
}

static const TCGOutOpBinary outop_remu = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_remu,
};

static void tgen_rotl(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_ROTL, WASM_I64_ROTL,
                        a0, a1, a2, false);
}

static void tgen_rotli(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_ROTL, WASM_I64_ROTL,
                        a0, a1, a2, true);
}

static const TCGOutOpBinary outop_rotl = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_rotl,
    .out_rri = tgen_rotli,
};

static void tgen_rotr(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_ROTR, WASM_I64_ROTR,
                        a0, a1, a2, false);
}

static void tgen_rotri(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_ROTR, WASM_I64_ROTR,
                        a0, a1, a2, true);
}

static const TCGOutOpBinary outop_rotr = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_rotr,
    .out_rri = tgen_rotri,
};

static void tgen_sar(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_SHR_S, WASM_I64_SHR_S,
                        a0, a1, a2, false);
}

static void tgen_sari(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_SHR_S, WASM_I64_SHR_S,
                        a0, a1, a2, true);
}

static const TCGOutOpBinary outop_sar = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_sar,
    .out_rri = tgen_sari,
};

static void tgen_shl(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_SHL, a0, a1, a2, false);
}

static void tgen_shli(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_SHL, a0, a1, a2, true);
}

static const TCGOutOpBinary outop_shl = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_shl,
    .out_rri = tgen_shli,
};

static void tgen_shr(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_SHR_U, WASM_I64_SHR_U,
                        a0, a1, a2, false);
}

static void tgen_shri(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, true, WASM_I32_SHR_U, WASM_I64_SHR_U,
                        a0, a1, a2, true);
}

static const TCGOutOpBinary outop_shr = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_shr,
    .out_rri = tgen_shri,
};

static void tgen_sub(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_SUB, a0, a1, a2, false);
}

static const TCGOutOpSubtract outop_sub = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_sub,
};

static void tgen_subbo(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, TCGReg a2)
{
    if (type == TCG_TYPE_I32) {
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_get32(s, a2);
        tcg_wasm_out_op(s, WASM_I32_LT_U);
        tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
    } else {
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_get64(s, a2);
        tcg_wasm_out_op(s, WASM_I64_LT_U);
        tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
    }
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_get64(s, a2);
    tcg_wasm_out_op(s, WASM_I64_SUB);
    tcg_wasm_out_set64(s, a0);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_CARRY);
}

static const TCGOutOpAddSubCarry outop_subbo = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_subbo,
};

static void tgen_subbi(TCGContext *s, TCGType type,
                       TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_get64(s, a2);
    tcg_wasm_out_op(s, WASM_I64_SUB);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_CARRY);
    tcg_wasm_out_op(s, WASM_I64_SUB);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpAddSubCarry outop_subbi = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_subbi,
};

static void tgen_subbio(TCGContext *s, TCGType type,
                        TCGReg a0, TCGReg a1, TCGReg a2)
{
    /* t = a1 - a2; r = t - borrow; borrow = (a1 < a2) | (t < borrow) */
    if (type == TCG_TYPE_I32) {
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_get32(s, a2);
        tcg_wasm_out_op(s, WASM_I32_LT_U);
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_get32(s, a2);
        tcg_wasm_out_op(s, WASM_I32_SUB);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_T0);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_CARRY);
        tcg_wasm_out_op(s, WASM_I32_WRAP_I64);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_T1);
        tcg_wasm_out_op(s, WASM_I32_LT_U);
        tcg_wasm_out_op(s, WASM_I32_OR);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T0);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T1);
        tcg_wasm_out_op(s, WASM_I32_SUB);
        tcg_wasm_out_set32(s, a0);
    } else {
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_get64(s, a2);
        tcg_wasm_out_op(s, WASM_I64_LT_U);
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_get64(s, a2);
        tcg_wasm_out_op(s, WASM_I64_SUB);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_X0);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_CARRY);
        tcg_wasm_out_op(s, WASM_I64_LT_U);
        tcg_wasm_out_op(s, WASM_I32_OR);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_X0);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_CARRY);
        tcg_wasm_out_op(s, WASM_I64_SUB);
        tcg_wasm_out_set64(s, a0);
    }
    tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_CARRY);
}

static const TCGOutOpAddSubCarry outop_subbio = {
    .base.static_constraint = C_O1_I2(r, r, r),
    .out_rrr = tgen_subbio,
};

static void tcg_out_set_borrow(TCGContext *s)
{
    tcg_out_set_carry(s);  /* borrow == carry */
}

static void tgen_xor(TCGContext *s, TCGType type,
                     TCGReg a0, TCGReg a1, TCGReg a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_XOR, a0, a1, a2, false);
}

static void tgen_xori(TCGContext *s, TCGType type,
                      TCGReg a0, TCGReg a1, tcg_target_long a2)
{
    tcg_wasm_out_binary(s, type, false, 0, WASM_I64_XOR, a0, a1, a2, true);
}

static const TCGOutOpBinary outop_xor = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_xor,
    .out_rri = tgen_xori,
};

static void tgen_ctpop(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1)
{
    if (type == TCG_TYPE_I32) {
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_op(s, WASM_I32_POPCNT);
        tcg_wasm_out_set32(s, a0);
    } else {
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_op(s, WASM_I64_POPCNT);
        tcg_wasm_out_set64(s, a0);
    }
}

static const TCGOutOpUnary outop_ctpop = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_ctpop,
};

static void tgen_bswap16(TCGContext *s, TCGType type,
                         TCGReg a0, TCGReg a1, unsigned flags)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_i64_const(s, 8);
    tcg_wasm_out_op(s, WASM_I64_SHR_U);
    tcg_wasm_out_i64_const(s, 0xff);
    tcg_wasm_out_op(s, WASM_I64_AND);
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_i64_const(s, 0xff);
    tcg_wasm_out_op(s, WASM_I64_AND);
    tcg_wasm_out_i64_const(s, 8);
    tcg_wasm_out_op(s, WASM_I64_SHL);
    tcg_wasm_out_op(s, WASM_I64_OR);
    if (flags & TCG_BSWAP_OS) {
        tcg_wasm_out_op(s, WASM_I64_EXTEND16_S);
    }
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpBswap outop_bswap16 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_bswap16,
};

static void tgen_bswap32(TCGContext *s, TCGType type,
                         TCGReg a0, TCGReg a1, unsigned flags)
{
    /* (rotl(x, 8) & 0x00ff00ff) | (rotr(x, 8) & 0xff00ff00) */
    tcg_wasm_out_get32(s, a1);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_T0);
    tcg_wasm_out_i32_const(s, 8);
    tcg_wasm_out_op(s, WASM_I32_ROTL);
    tcg_wasm_out_i32_const(s, 0x00ff00ff);
    tcg_wasm_out_op(s, WASM_I32_AND);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T0);
    tcg_wasm_out_i32_const(s, 8);
    tcg_wasm_out_op(s, WASM_I32_ROTR);
    tcg_wasm_out_i32_const(s, 0xff00ff00);
    tcg_wasm_out_op(s, WASM_I32_AND);
    tcg_wasm_out_op(s, WASM_I32_OR);
    tcg_wasm_out_op(s, flags & TCG_BSWAP_OS ? WASM_I64_EXTEND_I32_S
                                            : WASM_I64_EXTEND_I32_U);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpBswap outop_bswap32 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_bswap32,
};

/* X0 = ((X0 & mask) << sh) | ((X0 >> sh) & mask) */
static void tcg_wasm_out_swap_lanes(TCGContext *s, uint64_t mask, unsigned sh)
{
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_X0);
    tcg_wasm_out_i64_const(s, mask);
    tcg_wasm_out_op(s, WASM_I64_AND);
    tcg_wasm_out_i64_const(s, sh);
    tcg_wasm_out_op(s, WASM_I64_SHL);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_X0);
    tcg_wasm_out_i64_const(s, sh);
    tcg_wasm_out_op(s, WASM_I64_SHR_U);
    tcg_wasm_out_i64_const(s, mask);
    tcg_wasm_out_op(s, WASM_I64_AND);
    tcg_wasm_out_op(s, WASM_I64_OR);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_X0);
}

static void tgen_bswap64(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_X0);
    tcg_wasm_out_swap_lanes(s, 0x00ff00ff00ff00ffull, 8);
    tcg_wasm_out_swap_lanes(s, 0x0000ffff0000ffffull, 16);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_X0);
    tcg_wasm_out_i64_const(s, 32);
    tcg_wasm_out_op(s, WASM_I64_ROTL);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpUnary outop_bswap64 = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_bswap64,
};

static void tgen_neg(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1)
{
    tcg_wasm_out_i64_const(s, 0);
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_op(s, WASM_I64_SUB);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpUnary outop_neg = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_neg,
};

static void tgen_not(TCGContext *s, TCGType type, TCGReg a0, TCGReg a1)
{
    tcg_wasm_out_get64(s, a1);
    tcg_wasm_out_not(s);
    tcg_wasm_out_set64(s, a0);
}

static const TCGOutOpUnary outop_not = {
    .base.static_constraint = C_O1_I1(r, r),
    .out_rr = tgen_not,
};

/*
 * Comparisons.
 */

/* Push the i32 result of (a1 cond a2). */
static void tcg_wasm_out_cond(TCGContext *s, TCGType type, TCGCond cond,
                              TCGReg a1, TCGArg a2, bool const_a2)
{
    bool is32 = type == TCG_TYPE_I32;
    WasmOpcode op;

    switch (cond) {
    case TCG_COND_NEVER:
        tcg_wasm_out_i32_const(s, 0);
        return;
    case TCG_COND_ALWAYS:
        tcg_wasm_out_i32_const(s, 1);
        return;
    case TCG_COND_TSTEQ:
    case TCG_COND_TSTNE:
        if (is32) {
            tcg_wasm_out_get32(s, a1);
            tcg_wasm_out_arg32(s, a2, const_a2);
            tcg_wasm_out_op(s, WASM_I32_AND);
        } else {
            tcg_wasm_out_get64(s, a1);
            tcg_wasm_out_arg64(s, a2, const_a2);
            tcg_wasm_out_op(s, WASM_I64_AND);
            tcg_wasm_out_op(s, WASM_I64_EQZ);
            if (cond == TCG_COND_TSTNE) {
                tcg_wasm_out_op(s, WASM_I32_EQZ);
            }
            return;
        }
        if (cond == TCG_COND_TSTEQ) {
            tcg_wasm_out_op(s, WASM_I32_EQZ);
        } else {
            tcg_wasm_out_i32_const(s, 0);
            tcg_wasm_out_op(s, WASM_I32_NE);
        }
        return;
    default:
        break;
    }

    if (const_a2 && a2 == 0 && cond == TCG_COND_EQ) {
        if (is32) {
            tcg_wasm_out_get32(s, a1);
            tcg_wasm_out_op(s, WASM_I32_EQZ);
        } else {
            tcg_wasm_out_get64(s, a1);
            tcg_wasm_out_op(s, WASM_I64_EQZ);
        }
        return;
    }

    switch (cond) {
    case TCG_COND_EQ:
        op = is32 ? WASM_I32_EQ : WASM_I64_EQ;
        break;
    case TCG_COND_NE:
        op = is32 ? WASM_I32_NE : WASM_I64_NE;
        break;
    case TCG_COND_LT:
        op = is32 ? WASM_I32_LT_S : WASM_I64_LT_S;
        break;
    case TCG_COND_GE:
        op = is32 ? WASM_I32_GE_S : WASM_I64_GE_S;
        break;
    case TCG_COND_LE:
        op = is32 ? WASM_I32_LE_S : WASM_I64_LE_S;
        break;
    case TCG_COND_GT:
        op = is32 ? WASM_I32_GT_S : WASM_I64_GT_S;
        break;
    case TCG_COND_LTU:
        op = is32 ? WASM_I32_LT_U : WASM_I64_LT_U;
        break;
    case TCG_COND_GEU:
        op = is32 ? WASM_I32_GE_U : WASM_I64_GE_U;
        break;
    case TCG_COND_LEU:
        op = is32 ? WASM_I32_LE_U : WASM_I64_LE_U;
        break;
    case TCG_COND_GTU:
        op = is32 ? WASM_I32_GT_U : WASM_I64_GT_U;
        break;
    default:
        g_assert_not_reached();
    }

    if (is32) {
        tcg_wasm_out_get32(s, a1);
        tcg_wasm_out_arg32(s, a2, const_a2);
    } else {
        tcg_wasm_out_get64(s, a1);
        tcg_wasm_out_arg64(s, a2, const_a2);
    }
    tcg_wasm_out_op(s, op);
}

static void tgen_setcond(TCGContext *s, TCGType type, TCGCond cond,
                         TCGReg dest, TCGReg arg1, TCGReg arg2)
{
    tcg_wasm_out_cond(s, type, cond, arg1, arg2, false);
    tcg_wasm_out_set32(s, dest);
}

static void tgen_setcondi(TCGContext *s, TCGType type, TCGCond cond,
                          TCGReg dest, TCGReg arg1, tcg_target_long arg2)
{
    tcg_wasm_out_cond(s, type, cond, arg1, arg2, true);
    tcg_wasm_out_set32(s, dest);
}

static const TCGOutOpSetcond outop_setcond = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_setcond,
    .out_rri = tgen_setcondi,
};

static void tcg_wasm_out_negsetcond(TCGContext *s, TCGType type, TCGCond cond,
                                    TCGReg dest, TCGReg arg1,
                                    TCGArg arg2, bool const_arg2)
{
    tcg_wasm_out_i64_const(s, 0);
    tcg_wasm_out_cond(s, type, cond, arg1, arg2, const_arg2);
    tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
    tcg_wasm_out_op(s, WASM_I64_SUB);
    tcg_wasm_out_set64(s, dest);
}

static void tgen_negsetcond(TCGContext *s, TCGType type, TCGCond cond,
                            TCGReg dest, TCGReg arg1, TCGReg arg2)
{
    tcg_wasm_out_negsetcond(s, type, cond, dest, arg1, arg2, false);
}

static void tgen_negsetcondi(TCGContext *s, TCGType type, TCGCond cond,
                             TCGReg dest, TCGReg arg1, tcg_target_long arg2)
{
    tcg_wasm_out_negsetcond(s, type, cond, dest, arg1, arg2, true);
}

static const TCGOutOpSetcond outop_negsetcond = {
    .base.static_constraint = C_O1_I2(r, r, ri),
    .out_rrr = tgen_negsetcond,
    .out_rri = tgen_negsetcondi,
};

static void tgen_brcond(TCGContext *s, TCGType type, TCGCond cond,
                        TCGReg arg0, TCGReg arg1, TCGLabel *l)
{
    tcg_wasm_out_cond(s, type, cond, arg0, arg1, false);
    tcg_wasm_out_branch(s, l, true);
}

static void tgen_brcondi(TCGContext *s, TCGType type, TCGCond cond,
                         TCGReg arg0, tcg_target_long arg1, TCGLabel *l)
{
    tcg_wasm_out_cond(s, type, cond, arg0, arg1, true);
    tcg_wasm_out_branch(s, l, true);
}

static const TCGOutOpBrcond outop_brcond = {
    .base.static_constraint = C_O0_I2(r, ri),
    .out_rr = tgen_brcond,
    .out_ri = tgen_brcondi,
};

static void tgen_movcond(TCGContext *s, TCGType type, TCGCond cond,
                         TCGReg ret, TCGReg c1, TCGArg c2, bool const_c2,
                         TCGArg vt, bool const_vt, TCGArg vf, bool const_vf)
{
    tcg_wasm_out_arg64(s, vt, const_vt);
    tcg_wasm_out_arg64(s, vf, const_vf);
    tcg_wasm_out_cond(s, type, cond, c1, c2, const_c2);
    tcg_wasm_out_op(s, WASM_SELECT);
    tcg_wasm_out_set64(s, ret);
}

static const TCGOutOpMovcond outop_movcond = {
    .base.static_constraint = C_O1_I4(r, r, ri, r, r),
    .out = tgen_movcond,
};

static void tcg_out_mb(TCGContext *s, unsigned a0)
{
    /*
     * Only emitted for CF_PARALLEL translation blocks, which need more
     * than one vCPU thread.  WebAssembly atomics would be needed then.
     */
}

static void tcg_out_br(TCGContext *s, TCGLabel *l)
{
    tcg_wasm_out_branch(s, l, false);
}

/*
 * Loads and stores relative to a host register.
 */

static void tgen_ld8u(TCGContext *s, TCGType type, TCGReg dest,
                      TCGReg base, ptrdiff_t offset)
{
    tcg_wasm_out_ld(s, WASM_I64_LOAD8_U, dest, base, offset);
}

static const TCGOutOpLoad outop_ld8u = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld8u,
};

static void tgen_ld8s(TCGContext *s, TCGType type, TCGReg dest,
                      TCGReg base, ptrdiff_t offset)
{
    tcg_wasm_out_ld(s, WASM_I64_LOAD8_S, dest, base, offset);
}

static const TCGOutOpLoad outop_ld8s = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld8s,
};

static void tgen_ld16u(TCGContext *s, TCGType type, TCGReg dest,
                       TCGReg base, ptrdiff_t offset)
{
    tcg_wasm_out_ld(s, WASM_I64_LOAD16_U, dest, base, offset);
}

static const TCGOutOpLoad outop_ld16u = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld16u,
};

static void tgen_ld16s(TCGContext *s, TCGType type, TCGReg dest,
                       TCGReg base, ptrdiff_t offset)
{
    tcg_wasm_out_ld(s, WASM_I64_LOAD16_S, dest, base, offset);
}

static const TCGOutOpLoad outop_ld16s = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld16s,
};

static void tgen_ld32u(TCGContext *s, TCGType type, TCGReg dest,
                       TCGReg base, ptrdiff_t offset)
{
    tcg_wasm_out_ld(s, WASM_I64_LOAD32_U, dest, base, offset);
}

static const TCGOutOpLoad outop_ld32u = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld32u,
};

static void tgen_ld32s(TCGContext *s, TCGType type, TCGReg dest,
                       TCGReg base, ptrdiff_t offset)
{
    tcg_wasm_out_ld(s, WASM_I64_LOAD32_S, dest, base, offset);
}

static const TCGOutOpLoad outop_ld32s = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_ld32s,
};

static void tgen_st8(TCGContext *s, TCGType type, TCGReg data,
                     TCGReg base, ptrdiff_t offset)
{
    tcg_wasm_out_st(s, WASM_I64_STORE8, data, base, offset);
}

static const TCGOutOpStore outop_st8 = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_r = tgen_st8,
};

static void tgen_st16(TCGContext *s, TCGType type, TCGReg data,
                      TCGReg base, ptrdiff_t offset)
{
    tcg_wasm_out_st(s, WASM_I64_STORE16, data, base, offset);
}

static const TCGOutOpStore outop_st16 = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_r = tgen_st16,
};

static const TCGOutOpStore outop_st = {
    .base.static_constraint = C_O0_I2(r, r),
    .out_r = tcg_out_st,
};

/*
 * Guest memory accesses.  The soft-TLB lookup is inline; a miss falls
 * into a segment of its own that calls the helper, so that the call can
 * be resumed after an Asyncify unwind.  A hit branches over it:
 *
 *       TLB lookup and compare
 *       if
 *         access host memory
 *         br $done
 *       end
 *     end                         ; segment: slow path
 *       call helper
 *     end                         ; segment: $done
 */

static WasmOpcode tcg_wasm_ld_opcode(MemOp opc)
{
    switch (opc & MO_SSIZE) {
    case MO_UB:
        return WASM_I64_LOAD8_U;
    case MO_SB:
        return WASM_I64_LOAD8_S;
    case MO_UW:
        return WASM_I64_LOAD16_U;
    case MO_SW:
        return WASM_I64_LOAD16_S;
    case MO_UL:
        return WASM_I64_LOAD32_U;
    case MO_SL:
        return WASM_I64_LOAD32_S;
    case MO_UQ:
        return WASM_I64_LOAD;
    default:
        g_assert_not_reached();
    }
}

static WasmOpcode tcg_wasm_st_opcode(MemOp opc)
{
    switch (opc & MO_SIZE) {
    case MO_8:
        return WASM_I64_STORE8;
    case MO_16:
        return WASM_I64_STORE16;
    case MO_32:
        return WASM_I64_STORE32;
    case MO_64:
        return WASM_I64_STORE;
    default:
        g_assert_not_reached();
    }
}

/* Push env + ofs, for fields in front of env such as the TLB. */
static void tcg_wasm_out_env_ptr(TCGContext *s, int ofs)
{
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_ENV);
    tcg_wasm_out_i32_const(s, ofs);
    tcg_wasm_out_op(s, WASM_I32_ADD);
}

/*
 * Emit the TLB lookup for @addr and an if block that is entered on a hit,
 * with the host address on the stack.
 */
static void tcg_wasm_out_tlb_hit(TCGContext *s, TCGReg addr, MemOpIdx oi,
                                 bool is_ld)
{
    MemOp opc = get_memop(oi);
    TCGAtomAlign aa = atom_and_align_for_opc(s, opc, MO_ATOM_IFALIGN, false);
    unsigned a_mask = (1u << aa.align) - 1;
    unsigned s_mask = (1u << (opc & MO_SIZE)) - 1;
    int fast_ofs = tlb_mask_table_ofs(s, get_mmuidx(oi));

    tcg_debug_assert(s->addr_type == TCG_TYPE_I32);
    tcg_debug_assert(!(opc & MO_BSWAP));

    tcg_wasm_out_get32(s, addr);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_T0);

    /* T1 = table + ((addr >> (page_bits - entry_bits)) & mask) */
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T0);
    tcg_wasm_out_i32_const(s, TARGET_PAGE_BITS - CPU_TLB_ENTRY_BITS);
    tcg_wasm_out_op(s, WASM_I32_SHR_U);
    tcg_wasm_out_env_ptr(s, fast_ofs + offsetof(CPUTLBDescFast, mask));
    tcg_wasm_out_mem(s, WASM_I32_LOAD, 0);
    tcg_wasm_out_op(s, WASM_I32_AND);
    tcg_wasm_out_env_ptr(s, fast_ofs + offsetof(CPUTLBDescFast, table));
    tcg_wasm_out_mem(s, WASM_I32_LOAD, 0);
    tcg_wasm_out_op(s, WASM_I32_ADD);
    tcg_wasm_out_op_idx(s, WASM_LOCAL_TEE, WASM_L_T1);
    tcg_wasm_out_mem(s, WASM_I32_LOAD,
                     is_ld ? offsetof(CPUTLBEntry, addr_read)
                           : offsetof(CPUTLBEntry, addr_write));

    /*
     * For aligned accesses, we check the first byte and include the
     * alignment bits within the address.  For unaligned access, we
     * check that we don't cross pages using the address of the last
     * byte of the access.
     */
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T0);
    if (a_mask < s_mask) {
        tcg_wasm_out_i32_const(s, s_mask - a_mask);
        tcg_wasm_out_op(s, WASM_I32_ADD);
    }
    tcg_wasm_out_i32_const(s, TARGET_PAGE_MASK | a_mask);
    tcg_wasm_out_op(s, WASM_I32_AND);
    tcg_wasm_out_op(s, WASM_I32_EQ);
    tcg_wasm_out_block(s, WASM_IF);

    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T1);
    tcg_wasm_out_mem(s, WASM_I32_LOAD, offsetof(CPUTLBEntry, addend));
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_T0);
    tcg_wasm_out_op(s, WASM_I32_ADD);
}

/* Leave the hit block over the slow path segment, and start that segment. */
static void tcg_wasm_out_tlb_hit_end(TCGContext *s)
{
    tcg_wasm_out_op_idx(s, WASM_BR, 2);
    tcg_wasm_out_op(s, WASM_END);
    tcg_wasm_out_new_segment(s);
}

/* Push env and the zero-extended guest address, for the slow path helper. */
static void tcg_wasm_out_ldst_helper_head(TCGContext *s, TCGReg addr)
{
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_ENV);
    tcg_wasm_out_get32(s, addr);
    tcg_wasm_out_op(s, WASM_I64_EXTEND_I32_U);
}

static void tcg_wasm_out_ldst_helper_call(TCGContext *s, MemOpIdx oi,
                                          const void *helper, uint32_t typeidx)
{
    tcg_insn_unit *ra;

    tcg_wasm_out_i32_const(s, oi);
    ra = tcg_wasm_out_i32_const_patchable(s);
    tcg_wasm_out_call_indirect(s, helper, typeidx);
    tcg_wasm_patch_sleb5(ra, tcg_wasm_code_addr(s));
    tcg_wasm_out_unwind_check(s);
}

static void tgen_qemu_ld(TCGContext *s, TCGType type, TCGReg data,
                         TCGReg addr, MemOpIdx oi)
{
    static const uint8_t params[] = {
        WASM_TYPE_I32, WASM_TYPE_I64, WASM_TYPE_I32, WASM_TYPE_I32
    };
    MemOp opc = get_memop(oi);

    tcg_wasm_out_tlb_hit(s, addr, oi, true);
    tcg_wasm_out_mem(s, tcg_wasm_ld_opcode(opc), 0);
    tcg_wasm_out_set64(s, data);
    tcg_wasm_out_tlb_hit_end(s);

    tcg_wasm_out_ldst_helper_head(s, addr);
    tcg_wasm_out_ldst_helper_call(s, oi, qemu_ld_helpers[opc & MO_SSIZE],
                                  tcg_wasm_type(s, params, ARRAY_SIZE(params),
                                                WASM_TYPE_I64));
    tcg_wasm_out_set64(s, data);
    tcg_wasm_out_new_segment(s);
}

static const TCGOutOpQemuLdSt outop_qemu_ld = {
    .base.static_constraint = C_O1_I1(r, r),
    .out = tgen_qemu_ld,
};

static const TCGOutOpQemuLdSt2 outop_qemu_ld2 = {
    .base.static_constraint = C_NotImplemented,
};

static void tgen_qemu_st(TCGContext *s, TCGType type, TCGReg data,
                         TCGReg addr, MemOpIdx oi)
{
    MemOp opc = get_memop(oi);
    bool is64 = (opc & MO_SIZE) == MO_64;
    const uint8_t params[] = {
        WASM_TYPE_I32, WASM_TYPE_I64, is64 ? WASM_TYPE_I64 : WASM_TYPE_I32,
        WASM_TYPE_I32, WASM_TYPE_I32
    };

    tcg_wasm_out_tlb_hit(s, addr, oi, false);
    tcg_wasm_out_get64(s, data);
    tcg_wasm_out_mem(s, tcg_wasm_st_opcode(opc), 0);
    tcg_wasm_out_tlb_hit_end(s);

    tcg_wasm_out_ldst_helper_head(s, addr);
    if (is64) {
        tcg_wasm_out_get64(s, data);
    } else {
        tcg_wasm_out_get32(s, data);
    }
    tcg_wasm_out_ldst_helper_call(s, oi, qemu_st_helpers[opc & MO_SIZE],
                                  tcg_wasm_type(s, params, ARRAY_SIZE(params),
                                                0));
    tcg_wasm_out_new_segment(s);
}

static const TCGOutOpQemuLdSt outop_qemu_st = {
    .base.static_constraint = C_O0_I2(r, r),
    .out = tgen_qemu_st,
};

static const TCGOutOpQemuLdSt2 outop_qemu_st2 = {
    .base.static_constraint = C_NotImplemented,
};

static bool tcg_out_qemu_ld_slow_path(TCGContext *s, TCGLabelQemuLdst *l)
{
    g_assert_not_reached();
}

static bool tcg_out_qemu_st_slow_path(TCGContext *s, TCGLabelQemuLdst *l)
{
    g_assert_not_reached();
}

bool tcg_target_has_memory_bswap(MemOp memop)
{
    return false;
}

static void tcg_out_op(TCGContext *s, TCGOpcode opc, TCGType type,
                       const TCGArg args[TCG_MAX_OP_ARGS],
                       const int const_args[TCG_MAX_OP_ARGS])
{
    g_assert_not_reached();
}

/* Test if a constant matches the constraint. */
static bool tcg_target_const_match(int64_t val, int ct,
                                   TCGType type, TCGCond cond, int vece)
{
    return ct & TCG_CT_CONST;
}

static void tcg_out_nop_fill(tcg_insn_unit *p, int count)
{
    memset(p, 0, sizeof(*p) * count);
}

/*
 * Translation block framing.
 */

static void tcg_out_tb_start(TCGContext *s)
{
    TCGWasmGen *g = &tcg_wasm_gen;
    size_t space;
    TCGOp *op;

    g->label_seg = tcg_malloc(sizeof(int) * MAX(s->nb_labels, 1));
    g->nseg = 1;
    g->cur = 0;
    g->resumable = false;
    g->used_regs = 0;

    /* Number the segments: see the top of this file. */
    QTAILQ_FOREACH(op, &s->ops, link) {
        switch (op->opc) {
        case INDEX_op_set_label:
            g->label_seg[arg_label(op->args[0])->id] = g->nseg++;
            break;
        case INDEX_op_call:
            if (!(tcg_call_info(op)->flags & TCG_CALL_NO_SIDE_EFFECTS)) {
                g->nseg++;
                g->resumable = true;
            }
            break;
        case INDEX_op_qemu_ld:
        case INDEX_op_qemu_st:
            g->nseg += 2;
            g->resumable = true;
            break;
        default:
            break;
        }
    }
    if (g->nseg > WASM_MAX_SEGMENTS) {
        tcg_raise_tb_overflow(s);
    }

    /* Leave room for the frame below, plus the header and preamble. */
    space = sizeof(TCGWasmTBHeader) + WASM_PREAMBLE_MAX + 32 + 8 * g->nseg;
    if ((void *)s->code_ptr + space > s->code_gen_highwater + TCG_HIGHWATER) {
        siglongjmp(s->jmp_trans, -1);
    }

    g->header = (TCGWasmTBHeader *)s->code_ptr;
    memset(g->header, 0, sizeof(*g->header));
    s->code_ptr += sizeof(TCGWasmTBHeader) + WASM_PREAMBLE_MAX;
    g->body = s->code_ptr;

    g->ntypes = 0;
    {
        static const uint8_t tb_params[] = { WASM_TYPE_I32, WASM_TYPE_I32 };
        uint32_t t;

        t = tcg_wasm_type(s, tb_params, 2, WASM_TYPE_I32);
        tcg_debug_assert(t == WASM_TYPEIDX_TB);
        t = tcg_wasm_type(s, NULL, 0, WASM_TYPE_I32);
        tcg_debug_assert(t == WASM_TYPEIDX_ASYNCIFY_STATE);
    }

    /* Locals: one per register, then the scratch locals. */
    tcg_wasm_out_uleb(s, 3);
    tcg_wasm_out_uleb(s, WASM_NB_LOCAL_REGS);
    tcg_out8(s, WASM_TYPE_I64);
    tcg_wasm_out_uleb(s, WASM_NB_I32_LOCALS);
    tcg_out8(s, WASM_TYPE_I32);
    tcg_wasm_out_uleb(s, WASM_NB_I64_SCRATCH);
    tcg_out8(s, WASM_TYPE_I64);

    if (g->nseg == 1) {
        return;
    }
    if (g->resumable) {
        /* loop $outer, block $save, block $restore */
        tcg_wasm_out_block(s, WASM_LOOP);
        tcg_wasm_out_block(s, WASM_BLOCK);
        tcg_wasm_out_block(s, WASM_BLOCK);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_FRAME);
        tcg_wasm_out_mem(s, WASM_I32_LOAD, offsetof(TCGWasmFrame, resume));
        tcg_wasm_out_op_idx(s, WASM_BR_IF, 0);
    }
    tcg_wasm_out_block(s, WASM_LOOP);
    for (unsigned i = 0; i < g->nseg; i++) {
        tcg_wasm_out_block(s, WASM_BLOCK);
    }
    tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_SEG);
    tcg_wasm_out_op_idx(s, WASM_BR_TABLE, g->nseg);
    for (unsigned i = 0; i < g->nseg; i++) {
        tcg_wasm_out_uleb(s, i);
    }
    tcg_wasm_out_uleb(s, 0);
    tcg_wasm_out_op(s, WASM_END);
}

/* Write the module preamble to @buf, returning its length. */
static size_t tcg_wasm_build_preamble(uint8_t *buf, size_t body_len)
{
    static const uint8_t header[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
    };
    static const uint8_t memory_import[] = {
        1, 'e', 1, 'm', 0x02,
    };
    static const uint8_t other_imports[] = {
        1, 'e', 1, 't', 0x01, WASM_TYPE_FUNCREF, 0x00, 0x00,
        1, 'e', 1, 's', 0x00, WASM_TYPEIDX_ASYNCIFY_STATE,
    };
    static const uint8_t func_and_export[] = {
        /* Function section: one function of type 0. */
        0x03, 2, 1, WASM_TYPEIDX_TB,
        /* Export section: function 1 as "f". */
        0x07, 5, 1, 1, 'f', 0x00, WASM_FUNC_TB,
    };
    TCGWasmGen *g = &tcg_wasm_gen;
    uint8_t imports[64], *ip = imports;
    uint8_t *p = buf;
    uint32_t size;

    memcpy(p, header, sizeof(header));
    p += sizeof(header);

    size = tcg_wasm_uleb_len(g->ntypes);
    for (unsigned i = 0; i < g->ntypes; i++) {
        size += g->type_len[i];
    }
    *p++ = 0x01;
    p = tcg_wasm_put_uleb(p, size);
    p = tcg_wasm_put_uleb(p, g->ntypes);
    for (unsigned i = 0; i < g->ntypes; i++) {
        memcpy(p, g->types[i], g->type_len[i]);
        p += g->type_len[i];
    }

    *ip++ = 3;
    memcpy(ip, memory_import, sizeof(memory_import));
    ip += sizeof(memory_import);
    if (tcg_wasm_shared_memory) {
        /* Shared memories must declare a maximum; allow the whole space. */
        *ip++ = 0x03;
        ip = tcg_wasm_put_uleb(ip, 0);
        ip = tcg_wasm_put_uleb(ip, 65536);
    } else {
        *ip++ = 0x00;
        ip = tcg_wasm_put_uleb(ip, 0);
    }
    memcpy(ip, other_imports, sizeof(other_imports));
    ip += sizeof(other_imports);
    *p++ = 0x02;
    p = tcg_wasm_put_uleb(p, ip - imports);
    memcpy(p, imports, ip - imports);
    p += ip - imports;

    memcpy(p, func_and_export, sizeof(func_and_export));
    p += sizeof(func_and_export);

    *p++ = 0x0a;
    p = tcg_wasm_put_uleb(p, 1 + tcg_wasm_uleb_len(body_len) + body_len);
    *p++ = 1;
    p = tcg_wasm_put_uleb(p, body_len);

    tcg_debug_assert(p - buf <= WASM_PREAMBLE_MAX);
    return p - buf;
}

/* The local kept in TCGWasmFrame.save[i]: a register, or the carry. */
static uint32_t tcg_wasm_save_local(unsigned i)
{
    return i == WASM_NB_LOCAL_REGS ? WASM_L_CARRY : WASM_L_REG0 + i;
}

static int tcg_out_tb_end(TCGContext *s)
{
    TCGWasmGen *g = &tcg_wasm_gen;
    uint8_t preamble[WASM_PREAMBLE_MAX];
    size_t body_len, len;
    uint8_t *module;

    tcg_debug_assert(g->cur == g->nseg - 1);

    if (g->nseg > 1) {
        tcg_wasm_out_op(s, WASM_END);           /* loop $dispatch */
        tcg_wasm_out_op(s, WASM_UNREACHABLE);
    }
    if (g->resumable) {
        tcg_wasm_out_op(s, WASM_END);           /* block $restore */
        for (unsigned r = 0; r <= WASM_NB_LOCAL_REGS; r++) {
            if (r == WASM_NB_LOCAL_REGS || (g->used_regs & (1u << r))) {
                tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_FRAME);
                tcg_wasm_out_mem(s, WASM_I64_LOAD,
                                 offsetof(TCGWasmFrame, save[r]));
                tcg_wasm_out_op_idx(s, WASM_LOCAL_SET,
                                    tcg_wasm_save_local(r));
            }
        }
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_FRAME);
        tcg_wasm_out_mem(s, WASM_I32_LOAD, offsetof(TCGWasmFrame, resume));
        tcg_wasm_out_op_idx(s, WASM_LOCAL_SET, WASM_L_SEG);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_FRAME);
        tcg_wasm_out_i32_const(s, 0);
        tcg_wasm_out_mem(s, WASM_I32_STORE, offsetof(TCGWasmFrame, resume));
        tcg_wasm_out_op_idx(s, WASM_BR, 1);     /* to loop $outer */

        tcg_wasm_out_op(s, WASM_END);           /* block $save */
        for (unsigned r = 0; r <= WASM_NB_LOCAL_REGS; r++) {
            if (r == WASM_NB_LOCAL_REGS || (g->used_regs & (1u << r))) {
                tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_FRAME);
                tcg_wasm_out_op_idx(s, WASM_LOCAL_GET,
                                    tcg_wasm_save_local(r));
                tcg_wasm_out_mem(s, WASM_I64_STORE,
                                 offsetof(TCGWasmFrame, save[r]));
            }
        }
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_FRAME);
        tcg_wasm_out_op_idx(s, WASM_LOCAL_GET, WASM_L_SEG);
        tcg_wasm_out_mem(s, WASM_I32_STORE, offsetof(TCGWasmFrame, resume));
        tcg_wasm_out_i32_const(s, 0);
        tcg_wasm_out_op(s, WASM_RETURN);

        tcg_wasm_out_op(s, WASM_END);           /* loop $outer */
        tcg_wasm_out_op(s, WASM_UNREACHABLE);
    } else if (g->nseg == 1) {
        tcg_wasm_out_op(s, WASM_UNREACHABLE);
    }
    tcg_wasm_out_op(s, WASM_END);               /* function */

    /* The epilogue above stays well within TCG_HIGHWATER. */
    if (unlikely((void *)s->code_ptr > s->code_gen_highwater)) {
        return -1;
    }

    body_len = s->code_ptr - g->body;
    len = tcg_wasm_build_preamble(preamble, body_len);
    module = g->body - len;
    memcpy(module, preamble, len);
    g->header->module_ofs = module - (uint8_t *)g->header;
    g->header->module_len = len + body_len;
    return 0;
}

static void tcg_target_init(TCGContext *s)
{
    tcg_target_available_regs[TCG_TYPE_I32] =
        MAKE_64BIT_MASK(0, TCG_TARGET_NB_REGS);
    tcg_target_available_regs[TCG_TYPE_I64] =
        MAKE_64BIT_MASK(0, TCG_TARGET_NB_REGS);

    /*
     * Registers are locals of the generated function, so helpers cannot
     * clobber them; only the return value register is written.
     */
    tcg_target_call_clobber_regs = 0;
    tcg_regset_set_reg(tcg_target_call_clobber_regs, TCG_REG_R0);

    /* TCG_AREG0 is reserved by tcg_context_init() for the env global. */
    s->reserved_regs = 0;
    tcg_regset_set_reg(s->reserved_regs, TCG_REG_CALL_STACK);

    /* The call arguments come first, followed by the temp storage. */
    tcg_set_frame(s, TCG_REG_CALL_STACK, TCG_STATIC_CALL_ARGS_SIZE,
                  TCG_STATIC_FRAME_SIZE);

    tcg_wasm_shared_memory = tcg_wasm_memory_is_shared();
}

/* Generate global QEMU prologue and epilogue code. */
static inline void tcg_target_qemu_prologue(TCGContext *s)
{
    /*
     * The prologue is the C function tcg_wasm_tb_exec.  The epilogue is
     * only an address for goto_ptr to compare against.
     */
    tcg_qemu_tb_exec = tcg_wasm_tb_exec;
    tcg_code_gen_epilogue = tcg_splitwx_to_rx(s->code_ptr);
    tcg_out32(s, 0);
}
