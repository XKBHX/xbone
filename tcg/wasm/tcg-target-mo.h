/* SPDX-License-Identifier: MIT */
/*
 * Define target-specific memory model
 */

#ifndef TCG_TARGET_MO_H
#define TCG_TARGET_MO_H

/* Plain WebAssembly loads and stores are unordered between agents. */
#define TCG_TARGET_DEFAULT_MO  0

#endif
