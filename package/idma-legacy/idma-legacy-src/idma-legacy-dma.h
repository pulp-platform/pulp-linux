/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MMIO helpers matching Cheshire sw/include/dif/dma.h lines 14–29.
 * Register offsets from sw/include/regs/idma.h (idma_reg64_2d).
 *
 * Note: public dma.h has a typo `#define 14(BASE)` (not a valid macro name);
 * use DMA_NEXT_ID_ADDR here.
 */

#ifndef _IDMA_LEGACY_DMA_H
#define _IDMA_LEGACY_DMA_H

#include <linux/io.h>

/* --- Offsets (regs/idma.h) --------------------------------------------- */

#define IDMA_REG64_2D_CONF_REG_OFFSET			0x0
#define IDMA_REG64_2D_CONF_DECOUPLE_AW_BIT		0
#define IDMA_REG64_2D_CONF_DECOUPLE_RW_BIT		1

#define IDMA_REG64_2D_STATUS_0_REG_OFFSET		0x4

#define IDMA_REG64_2D_NEXT_ID_0_REG_OFFSET		0x44

#define IDMA_REG64_2D_DONE_ID_0_REG_OFFSET		0x84

#define IDMA_REG64_2D_DST_ADDR_LOW_REG_OFFSET		0xd0
#define IDMA_REG64_2D_SRC_ADDR_LOW_REG_OFFSET		0xd8
#define IDMA_REG64_2D_LENGTH_LOW_REG_OFFSET		0xe0

#define IDMA_REG64_2D_DST_STRIDE_2_LOW_REG_OFFSET	0xe8
#define IDMA_REG64_2D_SRC_STRIDE_2_LOW_REG_OFFSET	0xf0
#define IDMA_REG64_2D_REPS_2_LOW_REG_OFFSET		0xf8

/* --- Address macros (dif/dma.h style, kernel __iomem base) -------------- */

#define DMA_SRC_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_SRC_ADDR_LOW_REG_OFFSET)
#define DMA_DST_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_DST_ADDR_LOW_REG_OFFSET)
#define DMA_NUMBYTES_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_LENGTH_LOW_REG_OFFSET)
#define DMA_CONF_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_CONF_REG_OFFSET)
#define DMA_STATUS_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_STATUS_0_REG_OFFSET)
#define DMA_NEXT_ID_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_NEXT_ID_0_REG_OFFSET)
#define DMA_DONE_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_DONE_ID_0_REG_OFFSET)
#define DMA_SRC_STRIDE_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_SRC_STRIDE_2_LOW_REG_OFFSET)
#define DMA_DST_STRIDE_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_DST_STRIDE_2_LOW_REG_OFFSET)
#define DMA_NUM_REPS_ADDR(BASE) ((void __iomem *)(BASE) + IDMA_REG64_2D_REPS_2_LOW_REG_OFFSET)

#define DMA_CONF_DECOUPLE_NONE (0)
#define DMA_CONF_DECOUPLE_AW (1U << IDMA_REG64_2D_CONF_DECOUPLE_AW_BIT)
#define DMA_CONF_DECOUPLE_RW (1U << IDMA_REG64_2D_CONF_DECOUPLE_RW_BIT)
#define DMA_CONF_DECOUPLE_ALL (DMA_CONF_DECOUPLE_AW | DMA_CONF_DECOUPLE_RW)

/*
 * iDMA flags for 1D MEMCPY, no completion IRQ — mirrors
 * iis_idma_flags(DMA_MEMCPY, false) in idma-engine.c (AXI incr, serialize).
 */
#define IDMA_LEGACY_AXI_ID		0xffu
#define IDMA_LEGACY_AXI_INCR		0x1u

#define IDMA_LEGACY_CONF_MEMCPY_NOIRQ				\
	((u32)(IDMA_LEGACY_AXI_ID << 16) | (0u << 12) | (0u << 8) | \
	 (0u << 7) | (1u << 6) | (0u << 5) | (IDMA_LEGACY_AXI_INCR << 3) | \
	 (IDMA_LEGACY_AXI_INCR << 1) | (0u << 0))

#endif /* _IDMA_LEGACY_DMA_H */
