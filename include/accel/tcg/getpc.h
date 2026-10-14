/*
 * Get host pc for helper unwinding.
 *
 * Copyright (c) 2003 Fabrice Bellard
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */

#ifndef ACCEL_TCG_GETPC_H
#define ACCEL_TCG_GETPC_H

/* GETPC is the true target of the return instruction that we'll execute.  */
#ifdef CONFIG_TCG_INTERPRETER
extern __thread uintptr_t tci_tb_ptr;
# define GETPC() tci_tb_ptr
#elif defined(CONFIG_TCG_WASM)
/* Points into the frame of the translation block being executed. */
extern __thread uintptr_t *tcg_wasm_ra_ptr;
# define GETPC() (tcg_wasm_ra_ptr ? *tcg_wasm_ra_ptr : 0)
#else
# define GETPC() \
    ((uintptr_t)__builtin_extract_return_addr(__builtin_return_address(0)))
#endif

#endif /* ACCEL_TCG_GETPC_H */
