/* Core Audio DSP
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __HW_ADSP_CAVS_H__
#define __HW_ADSP_CAVS_H__

#include <sys/time.h>
#include <stdio.h>
#include <stdint.h>
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "exec/hwaddr.h"
#include "hw.h"

/* Broxton */
#define ADSP_CAVS_PCI_BASE           0xF2200000
#define ADSP_CAVS_MMIO_BASE          0xF2400000
#define ADSP_CAVS_HOST_IRAM_BASE     (ADSP_CAVS_MMIO_BASE + 0x000c0000)
#define ADSP_CAVS_HOST_DRAM_BASE     (ADSP_CAVS_MMIO_BASE + 0x00100000)
#define ADSP_CAVS_HOST_SHIM_BASE     (ADSP_CAVS_MMIO_BASE + 0x00140000)
#define ADSP_CAVS_HOST_MAILBOX_BASE  (ADSP_CAVS_MMIO_BASE + 0x00144000)

#define ADSP_CAVS_DSP_SHIM_BASE      0x00001000
#define ADSP_CAVS_SHIM_SIZE          0x00000100

#define ADSP_CAVS_DSP_CMD_BASE       0x00001100
#define ADSP_CAVS_DSP_CMD_SIZE       0x00000010

#define ADSP_CAVS_DSP_RES_BASE       0x00001110
#define ADSP_CAVS_DSP_RES_SIZE       0x00000010

#define ADSP_CAVS_DSP_IPC_HOST_BASE  0x00001180
#define ADSP_CAVS_DSP_IPC_HOST_SIZE  0x00000020

#define ADSP_CAVS_DSP_IPC_DSP_SIZE   0x00000080
#define ADSP_CAVS_DSP_IPC_DSP_BASE(x) (0x00001200 + x * ADSP_CAVS_DSP_IPC_DSP_SIZE)

#define ADSP_CAVS_DSP_HOST_WIN_SIZE  0x00000008
#define ADSP_CAVS_DSP_HOST_WIN_BASE(x) (0x00001580 + x * ADSP_CAVS_DSP_HOST_WIN_SIZE)

#define ADSP_CAVS_DSP_L2_BASE       0x00001500
#define ADSP_CAVS_DSP_L2_SIZE       0x00000040

#define ADSP_CAVS_DSP_IRQ_BASE       0x00001600
#define ADSP_CAVS_DSP_IRQ_SIZE       0x00000200

#define ADSP_CAVS_DSP_TIME_BASE      0x00001800
#define ADSP_CAVS_DSP_TIME_SIZE      0x00000200

#define ADSP_CAVS_DSP_MN_BASE        0x00001A00
#define ADSP_CAVS_DSP_MN_SIZE        0x00000200

#define ADSP_CAVS_DSP_LP_GP_DMA_LINK_SIZE    0x00000080
#define ADSP_CAVS_DSP_LP_GP_DMA_LINK_BASE(x) \
    (0x00001C00 + x * ADSP_CAVS_DSP_LP_GP_DMA_LINK_SIZE)

#define ADSP_CAVS_DSP_HP_GP_DMA_LINK_SIZE    0x00000800
#define ADSP_CAVS_DSP_HP_GP_DMA_LINK_BASE(x) \
    (0x00001D00 + x * ADSP_CAVS_DSP_HP_GP_DMA_LINK_SIZE)

#define ADSP_CAVS_DSP_GTW_LINK_OUT_STREAM_SIZE   0x00000020
#define ADSP_CAVS_DSP_GTW_LINK_OUT_STREAM_BASE(x) \
    (0x00002400 + x * ADSP_CAVS_DSP_GTW_LINK_OUT_STREAM_SIZE)

#define ADSP_CAVS_DSP_GTW_LINK_IN_STREAM_SIZE    0x00000020
#define ADSP_CAVS_DSP_GTW_LINK_IN_STREAM_BASE(x) \
    (0x00002600 + x * ADSP_CAVS_DSP_GTW_LINK_IN_STREAM_SIZE)

#define ADSP_CAVS_DSP_GTW_HOST_OUT_STREAM_SIZE   0x00000040
#define ADSP_CAVS_DSP_GTW_HOST_OUT_STREAM_BASE(x) \
    (0x00002800 + x * ADSP_CAVS_DSP_GTW_LINK_OUT_STREAM_SIZE)

#define ADSP_CAVS_DSP_GTW_HOST_IN_STREAM_SIZE    0x00000040
#define ADSP_CAVS_DSP_GTW_HOST_IN_STREAM_BASE(x) \
    (0x00002C00 + x * ADSP_CAVS_DSP_GTW_LINK_IN_STREAM_SIZE)

#define ADSP_CAVS_DSP_GTW_CODE_LDR_SIZE  0x00000040
#define ADSP_CAVS_DSP_GTW_CODE_LDR_BASE  0x00002BC0

#define ADSP_CAVS_DSP_DMIC_BASE      0x00004000
#define ADSP_CAVS_DSP_DMIC_SIZE      0x00004000

#define ADSP_CAVS_DSP_SSP_BASE(x) \
    (0x00008000 + x * ADSP_CAVS_DSP_SSP_SIZE)
#define ADSP_CAVS_DSP_SSP_SIZE       0x00002000

#define ADSP_CAVS_DSP_LP_GP_DMA_SIZE 0x00001000
#define ADSP_CAVS_DSP_LP_GP_DMA_BASE(x) \
    (0x0000C000 + x * ADSP_CAVS_DSP_LP_GP_DMA_SIZE)

#define ADSP_CAVS_DSP_HP_GP_DMA_SIZE 0x00001000
#define ADSP_CAVS_DSP_HP_GP_DMA_BASE(x) \
    (0x0000E000 + x * ADSP_CAVS_DSP_HP_GP_DMA_SIZE)

#define ADSP_CAVS_DSP_GPDMA_CLKCTL_BASE(x)   \
    (0x78404 + x * ADSP_CAVS_DSP_GPDMA_CLKCTL_SIZE)
#define ADSP_CAVS_DSP_GPDMA_CLKCTL_SIZE       0x100

#define ADSP_CAVS_SUE_DSP_L2_BASE         0x71D00
#define ADSP_CAVS_SUE_DSP_L2_SIZE         0xc

#define ADSP_CAVS_SUE_DSP_HS_BASE        0x71D10
#define ADSP_CAVS_SUE_DSP_HS_SIZE        0x30

#define ADSP_CAVS_SUE_DSP_LS_BASE         0x71D50
#define ADSP_CAVS_SUE_DSP_LS_SIZE         0x30


#define SHIM_L2_MECS        (SHIM_BASE + 0xd0)

#define SHIM_LDOCTL     0xA4
#define SHIM_HPMEM_POWER_ON (0x3 << 0)
#define SHIM_LPMEM_POWER_BYPASS (0x1 << 0)

#define DSP_INIT_LPGPDMA(x) (0x71A60 + (2*x))
#define LPGPDMA_CTLOSEL_FLAG    (1 << 15)
#define LPGPDMA_CHOSEL_FLAG (0xFF)

/* L2 SRAM */
#define ADSP_CAVS_DSP_SRAM_BASE      0xA000A000
#define ADSP_CAVS_DSP_SRAM_SIZE          (1024 * 1024 * 2) /* tmp hack - in reality 384kB */

/* HP SRAM */
#define ADSP_CAVS_DSP_HP_SRAM_BASE     0xBE000000
#define ADSP_CAVS_DSP_HP_SRAM_SIZE         ADSP_CAVS_DSP_SRAM_SIZE

/* LP SRAM */
#define ADSP_CAVS_DSP_LP_SRAM_BASE     0xBE800000
#define ADSP_CAVS_DSP_LP_SRAM_SIZE         0x00020000

/* ROM */
#define ADSP_CAVS_DSP_ROM_BASE     0xBEFE0000
#define ADSP_CAVS_DSP_ROM_SIZE         0x0002000

/* Uncache */
#define ADSP_CAVS_DSP_UNCACHE_BASE     (ADSP_CAVS_DSP_HP_SRAM_BASE - 0x20000000)
#define ADSP_CAVS_DSP_UNCACHE_SIZE         0x0080000

/* IMR */
#define ADSP_CAVS_DSP_IMR_BASE     0xB0000000
#define ADSP_CAVS_DSP_IMR_SIZE         0x0080000
#define ADSP_CAVS_DSP_IMR_MAN_OFFSET  0x4000

/* mailbox */
#define ADSP_CAVS_DSP_MAILBOX_SIZE      0x4000


#define ADSP_CAVS_DSP_MAILBOX_BASE \
    (ADSP_CAVS_DSP_HP_SRAM_BASE + ADSP_CAVS_DSP_HP_SRAM_SIZE - ADSP_CAVS_DSP_MAILBOX_SIZE)

#endif
