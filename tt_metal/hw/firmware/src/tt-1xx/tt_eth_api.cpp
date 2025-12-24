// SPDX-FileCopyrightText: © 2023 Tenstorrent Inc.
//
// SPDX-License-Identifier: Apache-2.0

#include "tt_eth_api.h"
#include "ethernet/dataflow_api.h"

FORCE_INLINE void eth_txq_reg_write(uint32_t const qnum, uint32_t const offset, uint32_t const val) {
    ETH_WRITE_REG(ETH_TXQ0_REGS_START + (qnum * ETH_TXQ_REGS_SIZE) + offset, val);
}

FORCE_INLINE uint32_t eth_txq_reg_read(uint32_t const qnum, uint32_t const offset) {
    return ETH_READ_REG(ETH_TXQ0_REGS_START + (qnum * ETH_TXQ_REGS_SIZE) + offset);
}

FORCE_INLINE void eth_send_packet(uint32_t const q_num, uint32_t const src_word_addr, uint32_t const dest_word_addr, uint32_t const num_words) {
    while (internal_::eth_txq_is_busy(q_num)) {
    }
    eth_txq_reg_write(q_num, ETH_TXQ_TRANSFER_START_ADDR, src_word_addr << 4U);
    eth_txq_reg_write(q_num, ETH_TXQ_DEST_ADDR, dest_word_addr << 4U);
    eth_txq_reg_write(q_num, ETH_TXQ_TRANSFER_SIZE_BYTES, num_words << 4U);
    eth_txq_reg_write(q_num, ETH_TXQ_CMD, ETH_TXQ_CMD_START_DATA);
}

FORCE_INLINE void eth_write_remote_reg(uint32_t const q_num, uint32_t const reg_addr, uint32_t const val) {
    while (internal_::eth_txq_is_busy(q_num)) {
    }
    eth_txq_reg_write(q_num, ETH_TXQ_DEST_ADDR, reg_addr);
    eth_txq_reg_write(q_num, ETH_TXQ_REMOTE_REG_DATA, val);
    eth_txq_reg_write(q_num, ETH_TXQ_CMD, ETH_TXQ_CMD_START_REG);
}
