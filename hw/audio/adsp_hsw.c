/* Core IA host support for Haswell audio DSP.
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
#include "hw/adsp/hsw.h"
#include "hw/adsp/shim.h"
#include "qemu/io-bridge.h"
#include "trace.h"

#define hsw_region(raddr) info->region[raddr >> 2]

static uint64_t hsw_mbox_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_host_mbox_read(addr, hsw_region(addr));
    return hsw_region(addr);
}

static void hsw_mbox_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_host_mbox_write(addr, val);
    hsw_region(addr) = val;
}

static const MemoryRegionOps hsw_mbox_ops = {
    .read = hsw_mbox_read,
    .write = hsw_mbox_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* driver reads from the SHIM */
static uint64_t hsw_shim_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_host_shim_read(addr, hsw_region(addr));

    switch (size) {
    case 4:
        return hsw_region(addr);
    case 8:
        return *((uint64_t*)&info->region[addr >> 3]);
    default:
        return 0;
    }
}

/* driver writes to the SHIM */
static void hsw_shim_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_host *adsp = info->adsp;
    struct qemu_io_msg_reg32 reg32;
    struct qemu_io_msg_irq irq;
    uint32_t active, isrd;

    trace_adsp_host_shim_write(addr, val);

    /* write value via SHM */
    hsw_region(addr) = val;

    /* most IO is handled by SHM, but there are some exceptions */
    switch (addr) {
    case SHIM_IPCX:

        /* now set/clear status bit */
        isrd = hsw_region(SHIM_ISRD) & ~(SHIM_ISRD_DONE | SHIM_ISRD_BUSY);
        isrd |= val & SHIM_IPCX_BUSY ? SHIM_ISRD_BUSY : 0;
        isrd |= val & SHIM_IPCX_DONE ? SHIM_ISRD_DONE : 0;
        hsw_region(SHIM_ISRD) = isrd;

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
    case SHIM_IPCD:

        /* set/clear status bit */
        isrd = hsw_region(SHIM_ISRD) &
            ~(SHIM_ISRD_DONE | SHIM_ISRD_BUSY);
        isrd |= val & SHIM_IPCD_BUSY ? SHIM_ISRD_BUSY : 0;
        isrd |= val & SHIM_IPCD_DONE ? SHIM_ISRD_DONE : 0;
        hsw_region(SHIM_ISRD) = isrd;

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
        hsw_region(addr) = val;

        active = hsw_region(SHIM_ISRX);
        active &= ~(hsw_region(SHIM_IMRX));

        trace_adsp_host_shim_mask("IMRD", hsw_region(SHIM_ISRD),
                   hsw_region(SHIM_IMRD), active);

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

static const MemoryRegionOps hsw_shim_ops = {
    .read = hsw_shim_read,
    .write = hsw_shim_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint64_t hsw_pci_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_host_pci_read(addr, hsw_region(addr));

    return hsw_region(addr);
}

static void hsw_pci_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;

    hsw_region(addr) = val;

    trace_adsp_host_pci_write(addr, val);
    /* TODO: pass on certain writes to DSP as MQ messages e.g. PM */
}

static const MemoryRegionOps hsw_pci_ops = {
    .read = hsw_pci_read,
    .write = hsw_pci_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void hsw_init_pci(struct adsp_host *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    adsp->pci_io = info->region;
    //memory_region_add_subregion(adsp->system_memory,
     //   board->pci.base, pci);

    pci_register_bar(&adsp->dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY,
        info->space->desc.ptr);
}

static void hsw_init_shim(struct adsp_host *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    adsp->shim_io = info->region;

    pci_register_bar(&adsp->dev, 1, PCI_BASE_ADDRESS_SPACE_MEMORY,
           info->space->desc.ptr);
}

static void hsw_init_mbox(struct adsp_host *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    adsp->mbox_io = info->region;

    pci_register_bar(&adsp->dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY,
           info->space->desc.ptr);
}

static struct adsp_mem_desc hsw_mem[] = {
    {.name = "iram", .base = ADSP_HSW_HOST_IRAM_BASE,
        .size = ADSP_HSW_IRAM_SIZE},
    {.name = "dram", .base = ADSP_HSW_HOST_DRAM_BASE,
        .size = ADSP_HSW_DRAM_SIZE},
};

static struct adsp_reg_space hsw_io[] = {
    { .name = "pci",
        .desc = {.base = ADSP_HSW_PCI_BASE, .size = ADSP_PCI_SIZE},
        .init = (void*)hsw_init_pci,
        .ops = &hsw_pci_ops,
    },
    { .name = "shim",
        .desc = {.base = ADSP_HSW_HOST_SHIM_BASE, .size = ADSP_HSW_SHIM_SIZE},
        .init = (void*)hsw_init_shim,
        .ops = &hsw_shim_ops,
    },
    { .name = "mbox",
        .desc = {.base = ADSP_HSW_HOST_MAILBOX_BASE, .size = ADSP_MAILBOX_SIZE},
        .init = (void*)hsw_init_mbox,
        .ops = &hsw_mbox_ops,
    }
};

static const struct adsp_desc hsw_board = {
    .num_mem = ARRAY_SIZE(hsw_mem),
    .mem_region = hsw_mem,
    .num_io = ARRAY_SIZE(hsw_io),
    .io_dev = hsw_io,
};

static struct adsp_mem_desc bdw_mem[] = {
    {.name = "iram", .base = ADSP_BDW_HOST_IRAM_BASE,
        .size = ADSP_BDW_IRAM_SIZE},
    {.name = "dram", .base = ADSP_BDW_HOST_DRAM_BASE,
        .size = ADSP_BDW_DRAM_SIZE},
};

static struct adsp_reg_space bdw_io[] = {
    { .name = "pci", .ops = &hsw_pci_ops,
        .init = (void*)hsw_init_pci,
        .desc = {.base = ADSP_BDW_PCI_BASE, .size = ADSP_PCI_SIZE},},
    { .name = "shim", .ops = &hsw_shim_ops,
        .init = (void*)hsw_init_shim,
        .desc = {.base = ADSP_BDW_DSP_SHIM_BASE, .size = ADSP_HSW_SHIM_SIZE},},
    { .name = "mbox", .ops = &hsw_mbox_ops,
        .init = (void*)hsw_init_mbox,
        .desc = {.base = ADSP_BDW_DSP_MAILBOX_BASE, .size = ADSP_MAILBOX_SIZE},},
};

static const struct adsp_desc bdw_board = {
    .num_mem = ARRAY_SIZE(bdw_mem),
    .mem_region = bdw_mem,
    .num_io = ARRAY_SIZE(bdw_io),
    .io_dev = bdw_io,
};

static void do_irq(struct adsp_host *adsp, struct qemu_io_msg *msg)
{
    uint32_t *region = adsp->shim_io;
    uint32_t active;

    active = adsp->shim_io[SHIM_ISRX >> 2] & ~(adsp->shim_io[SHIM_IMRX >> 2]);

    trace_adsp_host_dsp_irq("host", region[SHIM_ISRX >> 2],
            region[SHIM_IMRX >> 2], active, region[SHIM_IPCD >> 2]);

    if (active) {
        pci_set_irq(&adsp->dev, 1);
    }
}

static int hsw_bridge_cb(void *data, struct qemu_io_msg *msg)
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

static void hsw_host_init(struct adsp_host *adsp, const char *name)
{
    adsp->desc = &hsw_board;
    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->shm_idx = 0;

    adsp_create_host_memory_regions(adsp);
    adsp_create_host_io_devices(adsp, NULL);

    /* initialise bridge to x86 host driver */
    qemu_io_register_parent(name, &hsw_bridge_cb, (void*)adsp);
}

static void bdw_host_init(struct adsp_host *adsp, const char *name)
{
    adsp->desc = &bdw_board;
    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->shm_idx = 0;

    adsp_create_host_memory_regions(adsp);
    adsp_create_host_io_devices(adsp, NULL);

    /* initialise bridge to x86 host driver */
    qemu_io_register_parent(name, &hsw_bridge_cb, (void*)adsp);
}

static void hsw_reset(DeviceState *dev)
{
}

static void hsw_pci_exit(PCIDevice *pci_dev)
{
}

static void hsw_instance_init(Object *obj)
{

}

static void hsw_pci_realize(PCIDevice *pci_dev, Error **errp)
{
    struct adsp_host *adsp = adsp_get_pdata(pci_dev,
        ADSP_HOST_HSW_NAME);
    uint8_t *pci_conf;

    pci_conf = pci_dev->config;

    pci_set_byte(&pci_conf[PCI_INTERRUPT_PIN], 1); /* interrupt pin A */
    pci_set_byte(&pci_conf[PCI_MIN_GNT], 0);
    pci_set_byte(&pci_conf[PCI_MAX_LAT], 0);

    adsp->irq = pci_allocate_irq(&adsp->dev);

    hsw_host_init(adsp, "hsw");
}

static void bdw_pci_realize(PCIDevice *pci_dev, Error **errp)
{
    struct adsp_host *adsp = adsp_get_pdata(pci_dev,
        ADSP_HOST_BDW_NAME);
    uint8_t *pci_conf;

    pci_conf = pci_dev->config;

    pci_set_byte(&pci_conf[PCI_INTERRUPT_PIN], 1); /* interrupt pin A */
    pci_set_byte(&pci_conf[PCI_MIN_GNT], 0);
    pci_set_byte(&pci_conf[PCI_MAX_LAT], 0);

    adsp->irq = pci_allocate_irq(&adsp->dev);

    bdw_host_init(adsp, "bdw");
}

static void hsw_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = hsw_pci_realize;
    k->exit = hsw_pci_exit;

    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = SST_DEV_ID_LYNX_POINT;
    k->revision = 1;

    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);

    dc->desc = "Intel Audio DSP Haswell";
    dc->reset = hsw_reset;
}

static void bdw_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = bdw_pci_realize;
    k->exit = hsw_pci_exit;

    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->device_id = SST_DEV_ID_WILDCAT_POINT;
    k->revision = 1;

    k->class_id = PCI_CLASS_MULTIMEDIA_AUDIO;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);

    dc->desc = "Intel Audio DSP Broadwell";
    dc->reset = hsw_reset;
}

static const TypeInfo hsw_base_info = {
    .name          = ADSP_HOST_HSW_NAME,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(struct adsp_host),
    .instance_init = hsw_instance_init,
    .class_init    = hsw_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const TypeInfo bdw_base_info = {
    .name          = ADSP_HOST_BDW_NAME,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(struct adsp_host),
    .instance_init = hsw_instance_init,
    .class_init    = bdw_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void adsp_hsw_register_types(void)
{
    type_register_static(&hsw_base_info);
    type_register_static(&bdw_base_info);
}

type_init(adsp_hsw_register_types);
