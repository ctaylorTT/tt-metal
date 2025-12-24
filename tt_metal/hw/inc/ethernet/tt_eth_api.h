// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#ifndef TT_ETH_API_H
#define TT_ETH_API_H

#include <stdint.h>

#include "tt_eth_ss_regs.h"

#define ETH_WRITE_REG(addr, val) ((*((volatile uint32_t*)((addr)))) = (val))
#define ETH_READ_REG(addr) (*((volatile uint32_t*)((addr))))

#define ETH_WORD_SIZE_BYTES 16
#define BYTES_TO_ETH_WORD_SHIFT 4

// Doesn't round up/ceil, just truncates for perf where we don't care about the remainder
// and we know the input is already a multiple of ETH_WORD_SIZE_BYTES
FORCE_INLINE  uint32_t bytes_to_eth_words_truncated(uint32_t const num_bytes) { return num_bytes >> BYTES_TO_ETH_WORD_SHIFT; }

FORCE_INLINE  uint32_t bytes_to_eth_words(uint32_t const num_bytes) {
    return (num_bytes + (ETH_WORD_SIZE_BYTES - 1U)) >> BYTES_TO_ETH_WORD_SHIFT;
}

FORCE_INLINE  void eth_txq_reg_write(uint32_t const qnum, uint32_t const offset, uint32_t const val) {
    ETH_WRITE_REG(ETH_TXQ0_REGS_START + (qnum * ETH_TXQ_REGS_SIZE) + offset, val);
}

FORCE_INLINE  uint32_t eth_txq_reg_read(uint32_t const qnum, uint32_t const offset) {
    return ETH_READ_REG(ETH_TXQ0_REGS_START + (qnum * ETH_TXQ_REGS_SIZE) + offset);
}

FORCE_INLINE  void eth_reg_write(uint32_t const addr, uint32_t const val) { ETH_WRITE_REG(addr, val); }

FORCE_INLINE  uint32_t eth_reg_read(uint32_t const addr) { return ETH_READ_REG(addr); }

FORCE_INLINE  void eth_risc_reg_write(uint32_t const offset, uint32_t const val) { ETH_WRITE_REG(ETH_RISC_REGS_START + offset, val); }
FORCE_INLINE  uint32_t eth_risc_reg_read(uint32_t const offset) { return ETH_READ_REG(ETH_RISC_REGS_START + offset); }
FORCE_INLINE  uint64_t eth_read_wall_clock() {
    uint32_t wall_clock_low = eth_risc_reg_read(ETH_RISC_WALL_CLOCK_0);
    uint32_t wall_clock_high = eth_risc_reg_read(ETH_RISC_WALL_CLOCK_1_AT);
    return (((uint64_t)wall_clock_high) << 32) | wall_clock_low;
}

FORCE_INLINE  void eth_wait_cycles(uint32_t const wait_cycles) {
    if (wait_cycles == 0) {
        return;
    }
    uint64_t curr_timer = eth_read_wall_clock();
    uint64_t end_timer = curr_timer + wait_cycles;
    while (curr_timer < end_timer) {
        curr_timer = eth_read_wall_clock();
    }
}

#endif
