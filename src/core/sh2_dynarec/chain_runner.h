/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once
#include <cstddef>
#include <cstdint>

// ARM-only private ABI. State offsets are the existing tagSH2 contract.
struct Sh2ChainContext {
  uint32_t *state;
  void *const *rom;
  void *const *low;
  void *const *high;
  uint32_t *memory_cycles;
  uint32_t target_cycles;
  uint32_t rom_helpers;
  uint32_t max_blocks;
  uint32_t completed;
  uint32_t guest_cycles_low;
  uint32_t guest_cycles_high;
  void *last_block;
  uint32_t reason;
};
enum { SH2_CHAIN_BUDGET, SH2_CHAIN_QUOTA, SH2_CHAIN_MISS,
       SH2_CHAIN_INTERRUPT, SH2_CHAIN_LOOP, SH2_CHAIN_MAPPING, SH2_CHAIN_POST_BLOCK };
static_assert(sizeof(void *) == 4, "A32 runner requires 32-bit pointers");
static_assert(offsetof(Sh2ChainContext, state) == 0);
static_assert(offsetof(Sh2ChainContext, rom) == 4);
static_assert(offsetof(Sh2ChainContext, low) == 8);
static_assert(offsetof(Sh2ChainContext, high) == 12);
static_assert(offsetof(Sh2ChainContext, memory_cycles) == 16);
static_assert(offsetof(Sh2ChainContext, target_cycles) == 20);
static_assert(offsetof(Sh2ChainContext, rom_helpers) == 24);
static_assert(offsetof(Sh2ChainContext, max_blocks) == 28);
static_assert(offsetof(Sh2ChainContext, completed) == 32);
static_assert(offsetof(Sh2ChainContext, guest_cycles_low) == 36);
static_assert(offsetof(Sh2ChainContext, guest_cycles_high) == 40);
static_assert(offsetof(Sh2ChainContext, last_block) == 44);
static_assert(offsetof(Sh2ChainContext, reason) == 48);
static_assert(sizeof(Sh2ChainContext) == 52);
extern "C" void VitaSh2RunCached(Sh2ChainContext *);
