/* Core IA host support for Intel audio DSPs.
 *
 * Copyright (C) 2016 Intel Corporation
 *
 * Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License along
 * with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HW_ADSP_HOST_H
#define HW_ADSP_HOST_H

#include <stdint.h>
#include "qemu/osdep.h"
#include "sysemu/sysemu.h"
#include "hw/pci/pci.h"
#include "hw/adsp/hw.h"

struct qemu_io_msg;

#define SST_DEV_ID_BAYTRAIL          0x0F28
#define SST_DEV_ID_CHERRYTRAIL       0x22A8
#define SST_DEV_ID_LYNX_POINT        0x33C8
#define SST_DEV_ID_WILDCAT_POINT     0x3438
#define SST_DEV_ID_SUNRISE_POINT     0x9d70
#define SST_DEV_ID_BROXTON_P         0x5a98

#define ADSP_MAX_GP_DMAC        3

typedef struct IntelHDAState IntelHDAState;

struct adsp_dma_buffer {
    int chan;
    uint32_t *io;
    struct adsp_mem_desc shm_desc;
    char *name;
};

struct adsp_host {

    PCIDevice dev;
    int shm_idx;

    /* IO mapped from ACPI tables */
    uint32_t *pci_io;
    uint32_t *ram;

    /* shim IO, offsets derived */
    uint32_t *shim_io;
    uint32_t *mbox_io;

    struct adsp_dma_buffer dma_shm_buffer[ADSP_MAX_GP_DMAC][8];

    /* runtime CPU */
    MemoryRegion *system_memory;
    QemuOpts *machine_opts;
    qemu_irq irq;

    /* logging options */
    struct adsp_log *log;

    /* machine init data */
    const struct adsp_desc *desc;
    const char *cpu_model;
    const char *kernel_filename;
};

#define adsp_get_pdata(obj, type) \
    OBJECT_CHECK(struct adsp_host, (obj), type)

#define ADSP_HOST_MBOX_COUNT    6

void adsp_host_init(struct adsp_host *adsp, const struct adsp_desc *board);
void adsp_host_do_dma(struct adsp_host *adsp, struct qemu_io_msg *msg);

#define ADSP_HOST_BYT_NAME        "adsp-byt"
#define ADSP_HOST_CHT_NAME        "adsp-cht"
#define ADSP_HOST_HSW_NAME        "adsp-hsw"
#define ADSP_HOST_BDW_NAME        "adsp-bdw"
#define ADSP_HOST_BXT_NAME        "adsp-bxt"

void adsp_hda_init(IntelHDAState *d, int version, const char *name);
void adsp_create_acpi_devices(Aml *table);

#endif
