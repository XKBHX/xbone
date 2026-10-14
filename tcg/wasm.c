/* SPDX-License-Identifier: MIT */
/*
 * Runtime for the WebAssembly TCG backend: turns the modules emitted by
 * tcg/wasm/tcg-target.c.inc into callable functions and runs them.
 *
 * WebAssembly cannot jump into bytes it has just written, so each
 * translation block is compiled by the JavaScript engine on first use and
 * added to the function table.  Tables are per thread (per Web Worker), so
 * the table index cached in the block header is only valid for the thread
 * recorded as its owner; other threads keep their own index in a hash
 * table.  Functions are released when the code buffer is flushed.
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/atomic.h"
#include "tcg/tcg.h"
#include "../accel/tcg/tb-context.h"
#include "wasm-runtime.h"
#include <emscripten.h>

__thread uintptr_t *tcg_wasm_ra_ptr;

EM_JS_DEPS(tcg_wasm, "$addFunction,$removeFunction");

/* Returns the function table index, or 0 on failure. */
EM_JS(uint32_t, tcg_wasm_js_instantiate, (const void *buf, uint32_t len), {
    var tcg = Module['tcgWasm'];
    if (!tcg) {
        tcg = Module['tcgWasm'] = {
            imports: {
                e: {
                    m: wasmMemory,
                    t: typeof wasmTable != 'undefined'
                        ? wasmTable : wasmExports['__indirect_function_table'],
                    s: typeof Asyncify != 'undefined'
                        ? () => Asyncify.state : () => 0,
                },
            },
        };
    }
    try {
        /* Compiling from a view of shared memory is not allowed: copy. */
        var bytes = new Uint8Array(wasmMemory.buffer, buf, len).slice();
        var inst = new WebAssembly.Instance(new WebAssembly.Module(bytes),
                                            tcg.imports);
        return addFunction(inst.exports['f'], 'iii');
    } catch (e) {
        err('tcg-wasm: ' + e);
        return 0;
    }
});

EM_JS(void, tcg_wasm_js_release, (uint32_t idx), {
    removeFunction(idx);
});

EM_JS(int, tcg_wasm_js_memory_shared, (void), {
    return typeof SharedArrayBuffer != 'undefined' &&
           wasmMemory.buffer instanceof SharedArrayBuffer;
});

typedef uintptr_t TCGWasmTBFunc(CPUArchState *env, TCGWasmFrame *frame);

typedef struct TCGWasmThread {
    uint32_t token;
    unsigned flush_count;
    /* All functions instantiated by this thread, by block code pointer. */
    GHashTable *funcs;
} TCGWasmThread;

static __thread TCGWasmThread tcg_wasm_thread;
static uint32_t tcg_wasm_next_token;

bool tcg_wasm_memory_is_shared(void)
{
    return tcg_wasm_js_memory_shared();
}

static void tcg_wasm_release_func(gpointer key, gpointer value, gpointer opaque)
{
    tcg_wasm_js_release(GPOINTER_TO_UINT(value));
}

static TCGWasmTBFunc *tcg_wasm_func_slow(TCGWasmThread *t,
                                         TCGWasmTBHeader *h)
{
    unsigned flush_count = qatomic_read(&tb_ctx.tb_flush_count);
    gpointer v;
    uint32_t idx;

    if (unlikely(!t->funcs)) {
        t->token = qatomic_inc_fetch(&tcg_wasm_next_token);
        t->funcs = g_hash_table_new(NULL, NULL);
        t->flush_count = flush_count;
    } else if (t->flush_count != flush_count) {
        /* The code buffer was reset: every function we hold is stale. */
        g_hash_table_foreach(t->funcs, tcg_wasm_release_func, NULL);
        g_hash_table_remove_all(t->funcs);
        t->flush_count = flush_count;
    }

    if (qatomic_read(&h->owner) == t->token) {
        return (TCGWasmTBFunc *)(uintptr_t)h->func;
    }
    v = g_hash_table_lookup(t->funcs, h);
    if (v) {
        return (TCGWasmTBFunc *)(uintptr_t)GPOINTER_TO_UINT(v);
    }

    idx = tcg_wasm_js_instantiate((uint8_t *)h + h->module_ofs,
                                  h->module_len);
    if (!idx) {
        error_report("tcg-wasm: cannot instantiate translation block %p", h);
        abort();
    }
    g_hash_table_insert(t->funcs, h, GUINT_TO_POINTER(idx));

    if (qatomic_read(&h->owner) == 0) {
        h->func = idx;
        smp_wmb();
        qatomic_cmpxchg(&h->owner, 0, t->token);
    }
    return (TCGWasmTBFunc *)(uintptr_t)idx;
}

static TCGWasmTBFunc *tcg_wasm_func(const void *tb_ptr)
{
    TCGWasmThread *t = &tcg_wasm_thread;
    TCGWasmTBHeader *h = (TCGWasmTBHeader *)tb_ptr;

    if (likely(t->funcs) &&
        likely(t->flush_count == qatomic_read(&tb_ctx.tb_flush_count)) &&
        likely(qatomic_read(&h->owner) == t->token)) {
        smp_rmb();
        return (TCGWasmTBFunc *)(uintptr_t)h->func;
    }
    return tcg_wasm_func_slow(t, h);
}

uintptr_t tcg_wasm_tb_exec(CPUArchState *env, const void *tb_ptr)
{
    TCGWasmFrame frame;
    uintptr_t *saved_ra_ptr = tcg_wasm_ra_ptr;
    uintptr_t ret;

    frame.resume = 0;
    frame.ra = 0;
    tcg_wasm_ra_ptr = &frame.ra;

    for (;;) {
        ret = tcg_wasm_func(tb_ptr)(env, &frame);
        if (ret != TCG_WASM_EXIT_CHAIN) {
            break;
        }
        tb_ptr = (const void *)frame.next;
    }

    tcg_wasm_ra_ptr = saved_ra_ptr;
    return ret;
}
