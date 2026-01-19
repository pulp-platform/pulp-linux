// Generated register defines for chs_xilinx

// Copyright information found in source file:
// Copyright 2022 ETH Zurich and University of Bologna.

// Licensing information found in source file:
// Licensed under Solderpad Hardware License, Version 0.51, see LICENSE for details.
// SPDX-License-Identifier: SHL-0.51

#ifndef _CHS_XILINX_REG_DEFS_
#define _CHS_XILINX_REG_DEFS_

#ifdef __cplusplus
extern "C" {
#endif
// Register width
#define CHS_XILINX_PARAM_REG_WIDTH 32

// PWM Setting
#define CHS_XILINX_FAN_CTL_REG_OFFSET 0x0
#define CHS_XILINX_FAN_CTL_FAN_CTL_MASK 0xf
#define CHS_XILINX_FAN_CTL_FAN_CTL_OFFSET 0
#define CHS_XILINX_FAN_CTL_FAN_CTL_FIELD \
  ((bitfield_field32_t) { .mask = CHS_XILINX_FAN_CTL_FAN_CTL_MASK, .index = CHS_XILINX_FAN_CTL_FAN_CTL_OFFSET })

// Override Fan Switches
#define CHS_XILINX_FAN_SW_OVERRIDE_REG_OFFSET 0x4
#define CHS_XILINX_FAN_SW_OVERRIDE_FAN_SW_OVERRIDE_BIT 0

// LED Control
#define CHS_XILINX_LEDS_REG_OFFSET 0x8
#define CHS_XILINX_LEDS_LEDS_MASK 0xff
#define CHS_XILINX_LEDS_LEDS_OFFSET 0
#define CHS_XILINX_LEDS_LEDS_FIELD \
  ((bitfield_field32_t) { .mask = CHS_XILINX_LEDS_LEDS_MASK, .index = CHS_XILINX_LEDS_LEDS_OFFSET })

// DRAM AW Delay in Cycles
#define CHS_XILINX_DRAM_AW_DELAY_REG_OFFSET 0xc
#define CHS_XILINX_DRAM_AW_DELAY_FAN_CTL_MASK 0xffff
#define CHS_XILINX_DRAM_AW_DELAY_FAN_CTL_OFFSET 0
#define CHS_XILINX_DRAM_AW_DELAY_FAN_CTL_FIELD \
  ((bitfield_field32_t) { .mask = CHS_XILINX_DRAM_AW_DELAY_FAN_CTL_MASK, .index = CHS_XILINX_DRAM_AW_DELAY_FAN_CTL_OFFSET })

// DRAM W Delay in Cycles
#define CHS_XILINX_DRAM_W_DELAY_REG_OFFSET 0x10
#define CHS_XILINX_DRAM_W_DELAY_FAN_CTL_MASK 0xffff
#define CHS_XILINX_DRAM_W_DELAY_FAN_CTL_OFFSET 0
#define CHS_XILINX_DRAM_W_DELAY_FAN_CTL_FIELD \
  ((bitfield_field32_t) { .mask = CHS_XILINX_DRAM_W_DELAY_FAN_CTL_MASK, .index = CHS_XILINX_DRAM_W_DELAY_FAN_CTL_OFFSET })

// DRAM B Delay in Cycles
#define CHS_XILINX_DRAM_B_DELAY_REG_OFFSET 0x14
#define CHS_XILINX_DRAM_B_DELAY_FAN_CTL_MASK 0xffff
#define CHS_XILINX_DRAM_B_DELAY_FAN_CTL_OFFSET 0
#define CHS_XILINX_DRAM_B_DELAY_FAN_CTL_FIELD \
  ((bitfield_field32_t) { .mask = CHS_XILINX_DRAM_B_DELAY_FAN_CTL_MASK, .index = CHS_XILINX_DRAM_B_DELAY_FAN_CTL_OFFSET })

// DRAM AR Delay in Cycles
#define CHS_XILINX_DRAM_AR_DELAY_REG_OFFSET 0x18
#define CHS_XILINX_DRAM_AR_DELAY_FAN_CTL_MASK 0xffff
#define CHS_XILINX_DRAM_AR_DELAY_FAN_CTL_OFFSET 0
#define CHS_XILINX_DRAM_AR_DELAY_FAN_CTL_FIELD \
  ((bitfield_field32_t) { .mask = CHS_XILINX_DRAM_AR_DELAY_FAN_CTL_MASK, .index = CHS_XILINX_DRAM_AR_DELAY_FAN_CTL_OFFSET })

// DRAM R Delay in Cycles
#define CHS_XILINX_DRAM_R_DELAY_REG_OFFSET 0x1c
#define CHS_XILINX_DRAM_R_DELAY_FAN_CTL_MASK 0xffff
#define CHS_XILINX_DRAM_R_DELAY_FAN_CTL_OFFSET 0
#define CHS_XILINX_DRAM_R_DELAY_FAN_CTL_FIELD \
  ((bitfield_field32_t) { .mask = CHS_XILINX_DRAM_R_DELAY_FAN_CTL_MASK, .index = CHS_XILINX_DRAM_R_DELAY_FAN_CTL_OFFSET })

#ifdef __cplusplus
}  // extern "C"
#endif
#endif  // _CHS_XILINX_REG_DEFS_
// End generated register defines for chs_xilinx