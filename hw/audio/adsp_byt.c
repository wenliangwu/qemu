/* Core IA host support for Baytrail audio DSP.
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

#include "qemu/osdep.h"
#include "hw/qdev-properties.h"
#include "hw/acpi/aml-build.h"
#include "hw/audio/adsp-host.h"
#include "hw/adsp/byt.h"
#include "hw/adsp/shim.h"
#include "qemu/io-bridge.h"
#include "trace.h"

#define byt_region(raddr) info->region[raddr >> 2]

static uint64_t adsp_mbox_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_host_mbox_read(addr, byt_region(addr));
    return byt_region(addr);
}

static void adsp_mbox_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_host_mbox_write(addr, val);
    byt_region(addr) = val;
}

static const MemoryRegionOps byt_mbox_ops = {
    .read = adsp_mbox_read,
    .write = adsp_mbox_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* driver reads from the SHIM */
static uint64_t adsp_shim_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_host_shim_read(addr, byt_region(addr));

    switch (size) {
    case 4:
        return byt_region(addr);
    case 8:
        return *((uint64_t*)&info->region[addr >> 3]);
    default:
        return 0;
    }
}

/* driver writes to the SHIM */
static void adsp_shim_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_host *adsp = info->adsp;
    struct qemu_io_msg_reg32 reg32;
    struct qemu_io_msg_irq irq;
    uint32_t active, isrd;

    trace_adsp_host_shim_write(addr, val);

    /* write value via SHM */
    byt_region(addr) = val;

    /* most IO is handled by SHM, but there are some exceptions */
    switch (addr) {
    case SHIM_IPCXH:

        /* now set/clear status bit */
        isrd = byt_region(SHIM_ISRD) & ~(SHIM_ISRD_DONE | SHIM_ISRD_BUSY);
        isrd |= val & SHIM_IPCX_BUSY ? SHIM_ISRD_BUSY : 0;
        isrd |= val & SHIM_IPCX_DONE ? SHIM_ISRD_DONE : 0;
        byt_region(SHIM_ISRD) = isrd;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCX_BUSY) {

            trace_adsp_host_shim_event("irq: send BUSY interrupt to DSP", val);

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
        isrd = byt_region(SHIM_ISRD) &
            ~(SHIM_ISRD_DONE | SHIM_ISRD_BUSY);
        isrd |= val & SHIM_IPCD_BUSY ? SHIM_ISRD_BUSY : 0;
        isrd |= val & SHIM_IPCD_DONE ? SHIM_ISRD_DONE : 0;
        byt_region(SHIM_ISRD) = isrd;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCD_DONE) {

            trace_adsp_host_shim_event("irq: send DONE interrupt to DSP", val);

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
        byt_region(addr) = val;

        active = byt_region(SHIM_ISRX);
        active &= ~(byt_region(SHIM_IMRX));

        trace_adsp_host_shim_mask("IMRD", byt_region(SHIM_ISRD),
                   byt_region(SHIM_IMRD), active);

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

static const MemoryRegionOps byt_shim_ops = {
    .read = adsp_shim_read,
    .write = adsp_shim_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint64_t byt_pci_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_host_pci_read(addr, byt_region(addr));

    return byt_region(addr);
}

static void byt_pci_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;

    byt_region(addr) = val;

    trace_adsp_host_pci_write(addr, val);
    /* TODO: pass on certain writes to DSP as MQ messages e.g. PM */
}

static const MemoryRegionOps byt_pci_ops = {
    .read = byt_pci_read,
    .write = byt_pci_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static struct adsp_mem_desc byt_mem[] = {
    {.name = "iram", .base = ADSP_BYT_HOST_IRAM_BASE,
        .size = ADSP_BYT_IRAM_SIZE},
    {.name = "dram", .base = ADSP_BYT_HOST_DRAM_BASE,
        .size = ADSP_BYT_DRAM_SIZE},
};

static void byt_init_pci(struct adsp_host *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    adsp->pci_io = info->region;
    //memory_region_add_subregion(adsp->system_memory,
     //   board->pci.base, pci);

    pci_register_bar(&adsp->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY,
        info->space->desc.ptr);
}

static void byt_init_shim(struct adsp_host *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    adsp->shim_io = info->region;

    pci_register_bar(&adsp->dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY,
           info->space->desc.ptr);
}

static void byt_init_mbox(struct adsp_host *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    adsp->mbox_io = info->region;

    pci_register_bar(&adsp->dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY,
           info->space->desc.ptr);
}

static struct adsp_reg_space byt_io[] = {
    { .name = "pci",
        .desc = {.base = ADSP_BYT_PCI_BASE, .size = ADSP_PCI_SIZE},
        .init = (void*)byt_init_pci,
        .ops = &byt_pci_ops,
    },
    { .name = "shim",
        .desc = {.base = ADSP_BYT_HOST_SHIM_BASE, .size = ADSP_BYT_SHIM_SIZE},
        .init = (void*)byt_init_shim,
        .ops = &byt_shim_ops,
    },
    { .name = "mbox",
        .desc = {.base = ADSP_BYT_HOST_MAILBOX_BASE, .size = ADSP_MAILBOX_SIZE},
        .init = (void*)byt_init_mbox,
        .ops = &byt_mbox_ops,
    }
};

static const struct adsp_desc byt_board = {
    .num_mem = ARRAY_SIZE(byt_mem),
    .mem_region = byt_mem,
    .num_io = ARRAY_SIZE(byt_io),
    .io_dev = byt_io,
};

static void do_irq(struct adsp_host *adsp, struct qemu_io_msg *msg)
{
    uint32_t *region = adsp->shim_io;
    uint32_t active;

    active = region[SHIM_ISRX >> 2] & ~region[SHIM_IMRX >> 2];

    trace_adsp_host_dsp_irq("host", region[SHIM_ISRX >> 2],
            region[SHIM_IMRX >> 2], active, region[SHIM_IPCD >> 2]);

    if (active) {
        pci_set_irq(&adsp->dev, 1);
    }
}

static int byt_bridge_cb(void *data, struct qemu_io_msg *msg)
{
    struct adsp_host *adsp = (struct adsp_host *)data;

    trace_adsp_host_dsp_msg("received type", msg->type);

    switch (msg->type) {
    case QEMU_IO_TYPE_REG:
        /* mostly handled by SHM, some exceptions */
        break;
    case QEMU_IO_TYPE_IRQ:
        do_irq(adsp, msg);
        break;
    case QEMU_IO_TYPE_DMA:
        adsp_host_do_dma(adsp, msg);
        break;
    case QEMU_IO_TYPE_MEM:
    case QEMU_IO_TYPE_PM:
    default:
        break;
    }
    return 0;
}

static void byt_host_init(struct adsp_host *adsp, const char *name)
{
    adsp->desc = &byt_board;
    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();

    adsp->shm_idx = 0;

    adsp_create_host_memory_regions(adsp);
    adsp_create_host_io_devices(adsp, NULL);

    /* initialise bridge to x86 host driver */
    qemu_io_register_parent(name, &byt_bridge_cb, (void*)adsp);
}

static void byt_reset(DeviceState *dev)
{
}

static void byt_instance_init(Object *obj)
{
}

static void byt_pci_exit(PCIDevice *pci_dev)
{
}

static void byt_pci_realize(PCIDevice *pci_dev, Error **errp)
{
    struct adsp_host *adsp = adsp_get_pdata(pci_dev,
        ADSP_HOST_BYT_NAME);
    uint8_t *pci_conf;

    pci_conf = pci_dev->config;
    //pci_conf[PCI_INTERRUPT_PIN] = 3; /* interrupt pin A 0=11, A1=? B2=10 C3=? D4= ?*/

    pci_set_byte(&pci_conf[PCI_INTERRUPT_PIN], 1); /* interrupt pin A */
    pci_set_byte(&pci_conf[PCI_MIN_GNT], 0);
    pci_set_byte(&pci_conf[PCI_MAX_LAT], 0);

    adsp->irq = pci_allocate_irq(&adsp->dev);

    byt_host_init(adsp, "byt");
}

static void cht_pci_realize(PCIDevice *pci_dev, Error **errp)
{
    struct adsp_host *adsp = adsp_get_pdata(pci_dev,
        ADSP_HOST_CHT_NAME);
    uint8_t *pci_conf;

    pci_conf = adsp->dev.config;
    pci_conf[PCI_INTERRUPT_PIN] = 1; /* interrupt pin A */

    pci_set_byte(&pci_conf[PCI_INTERRUPT_PIN], 1); /* interrupt pin A */
    pci_set_byte(&pci_conf[PCI_MIN_GNT], 0);
    pci_set_byte(&pci_conf[PCI_MAX_LAT], 0);

    adsp->irq = pci_allocate_irq(&adsp->dev);

    byt_host_init(adsp, "cht");
}

static void byt_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = byt_pci_realize;
    k->exit = byt_pci_exit;

    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = SST_DEV_ID_BAYTRAIL;
    k->revision = 1;

    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);

    dc->desc = "Intel Audio DSP Baytrail";
    dc->reset = byt_reset;
}

static void cht_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = cht_pci_realize;
    k->exit = byt_pci_exit;

    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = SST_DEV_ID_CHERRYTRAIL;
    k->revision = 1;

    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);

    dc->desc = "Intel Audio DSP Cherrytrail/Braswell";
    dc->reset = byt_reset;
}

static const TypeInfo byt_base_info = {
    .name          = ADSP_HOST_BYT_NAME,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(struct adsp_host),
    .instance_init = byt_instance_init,
    .class_init    = byt_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const TypeInfo cht_base_info = {
    .name          = ADSP_HOST_CHT_NAME,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(struct adsp_host),
    .instance_init = byt_instance_init,
    .class_init    = cht_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void adsp_byt_register_types(void)
{
    type_register_static(&byt_base_info);
    type_register_static(&cht_base_info);
}

type_init(adsp_byt_register_types);
