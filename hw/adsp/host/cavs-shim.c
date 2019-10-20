/* Core IA host SHIM support for Broxton audio DSP.
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

#include "hw/adsp/hw.h"
#include "hw/audio/adsp-host.h"
#include "hw/adsp/log.h"
#include "hw/adsp/cavs.h"
#include "hw/audio/intel-hda.h"

/* driver reads from the SHIM */
static uint64_t adsp_shim_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_host *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    log_area_read(adsp->log, space, addr, size,
        info->region[addr >> 2]);

    switch (size) {
    case 4:
        return info->region[addr >> 2];
    case 8:
        return *((uint64_t*)&info->region[addr >> 3]);
    default:
        printf("shim.io invalid read size %d at 0x%8.8x\n",
            size, (unsigned int)addr);
        return 0;
    }
}

/* driver writes to the SHIM */
static void adsp_shim_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_host *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;
    struct qemu_io_msg_reg32 reg32;
    struct qemu_io_msg_irq irq;
    uint32_t active, isrd;

    log_area_write(adsp->log, space, addr, val, size,
                info->region[addr >> 2]);

    /* write value via SHM */
    info->region[addr >> 2] = val;


    /* most IO is handled by SHM, but there are some exceptions */
    switch (addr) {
    case SHIM_IPCXH:

        /* now set/clear status bit */
        isrd = info->region[SHIM_ISRD >> 2] & ~(SHIM_ISRD_DONE | SHIM_ISRD_BUSY);
        isrd |= val & SHIM_IPCX_BUSY ? SHIM_ISRD_BUSY : 0;
        isrd |= val & SHIM_IPCX_DONE ? SHIM_ISRD_DONE : 0;
        info->region[SHIM_ISRD >> 2] = isrd;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCX_BUSY) {

            log_text(adsp->log, LOG_IRQ_BUSY,
                "irq: send busy interrupt 0x%8.8lx\n", val);

            /* send IRQ to child */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
        }
        break;
    case SHIM_IPCDH:

        /* set/clear status bit */
        isrd = info->region[SHIM_ISRD >> 2] &
            ~(SHIM_ISRD_DONE | SHIM_ISRD_BUSY);
        isrd |= val & SHIM_IPCD_BUSY ? SHIM_ISRD_BUSY : 0;
        isrd |= val & SHIM_IPCD_DONE ? SHIM_ISRD_DONE : 0;
        info->region[SHIM_ISRD >> 2] = isrd;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCD_DONE) {

            log_text(adsp->log, LOG_IRQ_DONE,
                "irq: send done interrupt 0x%8.8lx\n", val);

            /* send IRQ to child */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
        }
        break;
    case SHIM_IMRX:
        /* write value via SHM */
        info->region[addr >> 2] = val;

        active = info->region[SHIM_ISRX >> 2] &
            ~(info->region[SHIM_IMRX >> 2]);

        log_text(adsp->log, LOG_IRQ_ACTIVE,
            "irq: masking %x mask %x active %x\n",
            info->region[SHIM_ISRD >> 2],
            info->region[SHIM_IMRD >> 2], active);

        if (!active) {
            pci_set_irq(&adsp->dev, 0);
        }
        break;
    case SHIM_CSR:
        /* now send msg to DSP VM to notify register write */
        reg32.hdr.type = QEMU_IO_TYPE_REG;
        reg32.hdr.msg = QEMU_IO_MSG_REG32W;
        reg32.hdr.size = sizeof(reg32);
        reg32.reg = addr;
        reg32.val = val;
        qemu_io_send_msg(&reg32.hdr);
        break;
    default:
        break;
    }
}

const MemoryRegionOps adsp_shim_ops = {
    .read = adsp_shim_read,
    .write = adsp_shim_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

#if 0
static void adsp_cavs_init_shim(struct adsp_host *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    adsp->shim_io = info->region;
}
#endif

static void adsp_do_irq(IntelHDAState *d, struct qemu_io_msg *msg)
{
    //struct adsp_host *adsp = d->adsp;
    uint32_t active = 0;

#if 0
    active = adsp->shim_io[SHIM_ISRX >> 2] & ~(adsp->shim_io[SHIM_IMRX >> 2]);

    log_text(adsp->log, LOG_IRQ_ACTIVE,
        "DSP IRQ: status %x mask %x active %x cmd %x\n",
        adsp->shim_io[SHIM_ISRX >> 2],
        adsp->shim_io[SHIM_IMRX >> 2], active,
        adsp->shim_io[SHIM_IPCD >> 2]);
#endif
    if (active) {
        pci_set_irq(&d->pci, 1);
    }
}

static int adsp_bridge_cb(void *data, struct qemu_io_msg *msg)
{
    IntelHDAState *d = data;
    struct adsp_host *adsp = d->adsp;

    switch (msg->type) {
    case QEMU_IO_TYPE_REG:
        /* mostly handled by SHM, some exceptions */
        break;
    case QEMU_IO_TYPE_IRQ:
        adsp_do_irq(d, msg);
        break;
    case QEMU_IO_TYPE_PM:
       // do_pm(msg);
        break;
    case QEMU_IO_TYPE_DMA:
        adsp_host_do_dma(adsp, msg);
        break;
    case QEMU_IO_TYPE_MEM:
    default:
        break;
    }
    return 0;
}

#if 0
static void ipc_v1_5_reset(void *opaque)
{
    //struct adsp_io_info *info = opaque;

    //memset(ipc->io, 0, gp_dmac_dev->desc.size);
}
#endif

static uint64_t ipc_v1_5_read(void *opaque, hwaddr addr,
        unsigned size)
{
#if 0
    struct adsp_io_info *info = opaque;
    struct adsp_hda_dmac *dmac = info->private;
   // const struct adsp_reg_space *gp_dmac_dev = dmac->desc;

    /* only print IO from guest */
    //log_read(dmac->log, gp_dmac_dev, addr, size,
    //        dmac->io[addr >> 2]);

    return dmac->io[addr >> 2];
#endif
    return 0;
}

static void ipc_v1_5_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{

}

const MemoryRegionOps ipc_v1_5_ops = {
   // .reset = ipc_v1_5_reset,
    .read = ipc_v1_5_read,
    .write = ipc_v1_5_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

#if 0
static void ipc_v1_8_reset(void *opaque)
{
    //struct adsp_io_info *info = opaque;

    //memset(ipc->io, 0, gp_dmac_dev->desc.size);
}
#endif

static uint64_t ipc_v1_8_read(void *opaque, hwaddr addr,
        unsigned size)
{
#if 0
    struct adsp_io_info *info = opaque;
    struct adsp_hda_dmac *dmac = info->private;
   // const struct adsp_reg_space *gp_dmac_dev = dmac->desc;

    /* only print IO from guest */
    //log_read(dmac->log, gp_dmac_dev, addr, size,
    //        dmac->io[addr >> 2]);

    return dmac->io[addr >> 2];
#endif
    return 0;
}

static void ipc_v1_8_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{

}

const MemoryRegionOps ipc_v1_8_ops = {
   // .reset = ipc_v1_5_reset,
    .read = ipc_v1_8_read,
    .write = ipc_v1_8_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipc_init_1_5_dev(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    //info->private = dmac;
}

static void ipc_init_1_8_dev(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    //info->private = dmac;
}

static struct adsp_mem_desc adsp_1_5_mem[] = {
    {.name = "l2-sram", .base = ADSP_CAVS_HOST_DSP_SRAM_BASE,
        .size = ADSP_CAVS_1_5_DSP_SRAM_SIZE},
    {.name = "hp-sram", .base = ADSP_CAVS_HOST_DSP_1_5_HP_SRAM_BASE,
        .size = ADSP_CAVS_1_5_DSP_HP_SRAM_SIZE,},
    {.name = "lp-sram", .base = ADSP_CAVS_HOST_DSP_1_5_LP_SRAM_BASE,
        .size = ADSP_CAVS_1_5_DSP_LP_SRAM_SIZE},
    {.name = "imr", .base = ADSP_CAVS_HOST_DSP_1_5_IMR_BASE,
        .size = ADSP_CAVS_1_5_DSP_IMR_SIZE},
    {.name = "rom", .base = ADSP_CAVS_HOST_1_5_ROM_BASE,
        .size = ADSP_CAVS_DSP_ROM_SIZE},
};

static struct adsp_mem_desc adsp_1_8_mem[] = {
    {.name = "l2-sram", .base = ADSP_CAVS_HOST_DSP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_SRAM_SIZE},
    {.name = "hp-sram", .base = ADSP_CAVS_HOST_DSP_1_8_HP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_HP_SRAM_SIZE,},
    {.name = "lp-sram", .base = ADSP_CAVS_HOST_DSP_1_8_LP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_LP_SRAM_SIZE},
    {.name = "imr", .base = ADSP_CAVS_HOST_DSP_1_8_IMR_BASE,
        .size = ADSP_CAVS_1_8_DSP_IMR_SIZE},
    {.name = "rom", .base = ADSP_CAVS_HOST_1_8_ROM_BASE,
        .size = ADSP_CAVS_DSP_ROM_SIZE},
};

static struct adsp_reg_space adsp_1_5_io[] = {
    { .name = "pci",
        .desc = {.base = ADSP_CAVS_PCI_BASE, .size = ADSP_PCI_SIZE},},
    { .name = "ipc-dsp", .reg_count = ARRAY_SIZE(adsp_hsw_shim_map),
        .init = ipc_init_1_5_dev, .ops = &ipc_v1_5_ops,
        .desc = {.base = 0, .size = 0},},
    { .name = "hostwin0", .reg_count = ARRAY_SIZE(adsp_host_mbox_map),
        .desc = {.base = ADSP_CAVS_HOST_1_5_SRAM_WND_BASE(0), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin1", .reg_count = ARRAY_SIZE(adsp_host_mbox_map),
        .desc = {.base = ADSP_CAVS_HOST_1_5_SRAM_WND_BASE(1), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin2", .reg_count = ARRAY_SIZE(adsp_host_mbox_map),
        .desc = {.base = ADSP_CAVS_HOST_1_5_SRAM_WND_BASE(2), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin3", .reg_count = ARRAY_SIZE(adsp_host_mbox_map),
        .desc = {.base = ADSP_CAVS_HOST_1_5_SRAM_WND_BASE(3), .size = ADSP_CAVS_WND_SIZE},},
};

static struct adsp_reg_space adsp_1_8_io[] = {
    { .name = "pci",
        .desc = {.base = ADSP_CAVS_PCI_BASE, .size = ADSP_PCI_SIZE},},
    { .name = "ipc-dsp", .reg_count = ARRAY_SIZE(adsp_hsw_shim_map),
        .init = ipc_init_1_8_dev, .ops = &ipc_v1_8_ops,
        .desc = {.base = 0, .size = 0},},
    { .name = "hostwin0", .reg_count = ARRAY_SIZE(adsp_host_mbox_map),
        .desc = {.base = ADSP_CAVS_HOST_1_8_SRAM_WND_BASE(0), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin1", .reg_count = ARRAY_SIZE(adsp_host_mbox_map),
        .desc = {.base = ADSP_CAVS_HOST_1_8_SRAM_WND_BASE(1), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin2", .reg_count = ARRAY_SIZE(adsp_host_mbox_map),
        .desc = {.base = ADSP_CAVS_HOST_1_8_SRAM_WND_BASE(2), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin3", .reg_count = ARRAY_SIZE(adsp_host_mbox_map),
        .desc = {.base = ADSP_CAVS_HOST_1_8_SRAM_WND_BASE(3), .size = ADSP_CAVS_WND_SIZE},},
};

static const struct adsp_desc adsp_1_5 = {
    .num_mem = ARRAY_SIZE(adsp_1_5_mem),
    .mem_region = adsp_1_5_mem,

    .num_io = ARRAY_SIZE(adsp_1_5_io),
    .io_dev = adsp_1_5_io,
};

static const struct adsp_desc adsp_1_8 = {
    .num_mem = ARRAY_SIZE(adsp_1_8_mem),
    .mem_region = adsp_1_8_mem,

    .num_io = ARRAY_SIZE(adsp_1_8_io),
    .io_dev = adsp_1_8_io,
};

void adsp_hda_init(IntelHDAState *d, int version, const char *name)
{
    struct adsp_host *adsp;

    adsp = calloc(1, sizeof(*adsp));
    if (!adsp) {
  	    fprintf(stderr, "cant alloc DSP\n");
  	    exit(0);
    }
    d->adsp = adsp;

    switch (version) {
    case 15:
        adsp->desc = &adsp_1_5;
        break;
    case 18:
        adsp->desc = &adsp_1_8;
    	break;
    default:
    	fprintf(stderr, "no such version %d\n", version);
    	exit(0);
    }

    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->shm_idx = 0;

    adsp->log = log_init(NULL);    /* TODO: add log name to cmd line */

    adsp_create_host_memory_regions(adsp);

    adsp_create_host_io_devices(adsp, NULL);

    /* initialise bridge to x86 host driver */
    qemu_io_register_parent(name, &adsp_bridge_cb, (void*)d);
}
