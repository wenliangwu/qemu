/* Virtualization support for DesignWare DMA Engine.
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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "elf.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/thread.h"
#include "qemu/io-bridge.h"

#include "hw/pci/pci.h"
#include "hw/audio/adsp-dev.h"
#include "hw/audio/adsp-host.h"
#include "hw/adsp/shim.h"
#include "hw/ssi/ssp.h"
#include "hw/dma/dw-dma.h"

static void dw_dsp_do_irq(struct adsp_gp_dmac *dmac, int enable, uint32_t mask)
{
    struct adsp_io_info *info = dmac->info;

    if (enable) {
        adsp_irq_set(dmac->adsp, info, info->space->irq, mask);
    } else {
        adsp_irq_clear(dmac->adsp, info, info->space->irq, mask);
    }
}

void dw_dma_init_dev(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    struct adsp_gp_dmac *dmac;
    char name[32];
    int j;

    dmac = g_malloc(sizeof(*dmac));
    dmac->adsp = adsp;
    dmac->id = info->io_dev;
    dmac->irq_assert = 0;
    dmac->is_pci_dev = 0;
    dmac->do_irq = dw_dsp_do_irq;
    dmac->desc = info->space;
    dmac->io = info->region;
    dmac->info = info;

    sprintf(name, "dmac%d.io", info->io_dev);

    /* channels */
    for (j = 0; j < NUM_CHANNELS; j++) {
        dmac->dma_chan[j].dmac = dmac;
        dmac->dma_chan[j].fd = 0;
        dmac->dma_chan[j].chan = j;
        dmac->dma_chan[j].file_idx = 0;
        sprintf(dmac->dma_chan[j].thread_name, "dmac:%d.%d", info->io_dev, j);
    }

    info->private = dmac;
}

