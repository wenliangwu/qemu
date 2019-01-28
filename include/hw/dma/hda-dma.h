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

#ifndef __HW_HDA_DMA_H__
#define __HW_HDA_DMA_H__

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "hw/acpi/aml-build.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/pci/pci.h"
#include "elf.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/thread.h"
#include "qemu/io-bridge.h"

struct adsp_dev;
struct adsp_host;
struct adsp_hda_dmac;
struct adsp_log;
struct adsp_io_info;

#define HDA_NUM_CHANS   16

#define DIR_HOST_IN     0
#define DIR_HOST_OUT    1
#define DIR_LINK_IN     2
#define DIR_LINK_OUT     3

#define HDA_NUM_CHANNELS    8

/* context pointer used by timer callbacks */
struct hda_dma_chan {
    struct adsp_hda_dmac *dmac;
    int chan;

    /* buffer */
    uint32_t bytes;
    void *ptr;
    void *base;
    uint32_t tbytes;

    /* endpoint */
    struct qemu_io_msg_dma32 dma_msg;

    /* file output/input */
    int fd;
    int file_idx;

    /* threading */
    QemuThread thread;
    char thread_name[32];
    uint32_t stop;
};

struct adsp_hda_dmac {
    int id;
    int num_chan;
    uint32_t *io;
    int irq_assert;
    int irq;
    int is_pci_dev;
    void (*do_irq)(struct adsp_hda_dmac *dmac, int enable, uint32_t mask);
    struct adsp_log *log;
    int dir;

    struct adsp_io_info *info;
    const struct adsp_reg_space *desc;
    struct adsp_dev *adsp;
    struct dw_host *dw_host;
    struct hda_dma_chan hda_dma_chan[HDA_NUM_CHANS];
};

struct hda_desc {
    const char *name;    /* device name */
    int ia_irq;
    struct adsp_mem_desc pci;

    /* devices */
    int num_dmac;
    struct adsp_reg_space gp_dmac_dev[6];
    struct adsp_reg_space pci_dev;
};

extern const MemoryRegionOps hda_dmac_ops;

void hda_dma_init_dev(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info);
void hda_dma_msg(struct qemu_io_msg *msg);
void hda_dmac_reset(void *opaque);

#endif
