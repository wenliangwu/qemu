/* Core DSP Haswell support for audio DSP.
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

#include <stdint.h>

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "sysemu/reset.h"
#include "sysemu/runstate.h"

#include "hw/audio/adsp-dev.h"
#include "hw/adsp/shim.h"
#include "hw/adsp/hsw.h"
#include "hw/ssi/ssp.h"
#include "hw/dma/dw-dma.h"
#include "common.h"
#include "hw/adsp/fw.h"

#define ENABLE_SHM
#ifdef ENABLE_SHM
#include "qemu/io-bridge.h"
#endif

#include "trace.h"

/* IRQ numbers */
#define IRQ_NUM_SOFTWARE0    0
#define IRQ_NUM_TIMER1       1
#define IRQ_NUM_SOFTWARE1    2
#define IRQ_NUM_SOFTWARE2    3
#define IRQ_NUM_TIMER2       5
#define IRQ_NUM_SOFTWARE3    6
#define IRQ_NUM_TIMER3       7
#define IRQ_NUM_SOFTWARE4    8
#define IRQ_NUM_SOFTWARE5    9
#define IRQ_NUM_EXT_IA       10
#define IRQ_NUM_EXT_PMC      11
#define IRQ_NUM_SOFTWARE6    12
#define IRQ_NUM_EXT_DMAC0    13
#define IRQ_NUM_EXT_DMAC1    14
#define IRQ_NUM_EXT_TIMER    15
#define IRQ_NUM_EXT_SSP0     16
#define IRQ_NUM_EXT_SSP1     17
#define IRQ_NUM_EXT_SSP2     18
#define IRQ_NUM_NMI          20

#define hsw_region(raddr) info->region[raddr >> 2]

static void shim_reset(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;

    memset(info->region, 0, space->desc.size);
}

/* SHIM IO from ADSP */
static uint64_t shim_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_shim_read(addr, hsw_region(addr));

    return hsw_region(addr);
}

/* SHIM IO from ADSP */
static void shim_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct qemu_io_msg_reg32 reg32;
    struct qemu_io_msg_irq irq;
    uint32_t active, isrx;

    trace_adsp_dsp_shim_write(addr, val);

    /* special case registers */
    switch (addr) {
    case SHIM_IPCD:
        /* DSP to host IPC command */

        /* set value via SHM */
        hsw_region(addr) = val;

        /* set/clear status bit */
        isrx = hsw_region(SHIM_ISRX) & ~(SHIM_ISRX_DONE | SHIM_ISRX_BUSY);
        isrx |= val & SHIM_IPCD_BUSY ? SHIM_ISRX_BUSY : 0;
        isrx |= val & SHIM_IPCD_DONE ? SHIM_ISRX_DONE : 0;
        hsw_region(SHIM_ISRX) = isrx;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCD_BUSY) {

            trace_adsp_dsp_shim_event("irq: send BUSY interrupt to host", val);

            /* send IRQ to parent */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
        }
        break;
    case SHIM_IPCX:
        /* DSP to host IPC notify */

        /* set value via SHM */
        hsw_region(addr) = val;

        /* set/clear status bit */
        isrx = hsw_region(SHIM_ISRX) & ~(SHIM_ISRX_DONE | SHIM_ISRX_BUSY);
        isrx |= val & SHIM_IPCX_BUSY ? SHIM_ISRX_BUSY : 0;
        isrx |= val & SHIM_IPCX_DONE ? SHIM_ISRX_DONE : 0;
        hsw_region(SHIM_ISRX) = isrx;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCX_DONE) {

            trace_adsp_dsp_shim_event("irq: send DONE interrupt to host", val);

            /* send IRQ to parent */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
        }
        break;
    case SHIM_IMRD:

        /* set value via SHM */
        hsw_region(addr) = val;

        /* DSP IPC interrupt mask */
        active = hsw_region(SHIM_ISRD) & ~(hsw_region(SHIM_IMRD));

        trace_adsp_dsp_shim_mask("IMRD", hsw_region(SHIM_ISRD),
                   hsw_region(SHIM_IMRD), active);

        if (!active) {
            trace_adsp_dsp_shim_irq("irq: de-assert IPC IRQ to host");
            adsp_set_lvl1_irq(adsp, adsp->desc->ia_irq, 0);
        }

        break;
    case SHIM_CSR:

        /* set value via SHM */
        hsw_region(addr) = val;

        /* now send msg to HOST VM to notify register write */
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

/* 32 bit SHIM IO from host */
static void do_shim32(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    struct qemu_io_msg_reg32 *m = (struct qemu_io_msg_reg32 *)msg;

    switch (m->reg) {
    case SHIM_CSR:
        /* check for reset bit and stall bit */
        if (!adsp->in_reset && (m->val & SHIM_CSR_HSW_RST)) {

            trace_adsp_dsp_host_event("dsp reset asserted");

            cpu_reset(CPU(adsp->xtensa[0]->cpu));
            //vm_stop(RUN_STATE_SHUTDOWN); TODO: fix, causes hang
            adsp->in_reset = 1;

        } else if (adsp->in_reset && !(m->val & SHIM_CSR_HSW_STALL)) {

            trace_adsp_dsp_host_event("dsp reset cleared");

            cpu_resume(CPU(adsp->xtensa[0]->cpu));
            vm_start();
            adsp->in_reset = 0;
        }
        break;
    default:
        printf("unknown reg 0x%x val 0x%x\n", m->reg, m->val);
        break;
    }
}

/* 64 bit SHIM IO from host */
static void do_shim64(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    struct qemu_io_msg_reg64 *m = (struct qemu_io_msg_reg64 *)msg;

    switch (m->reg) {
    case SHIM_CSR:
        /* check for reset bit and stall bit */
        if (!adsp->in_reset && (m->val & SHIM_CSR_RST)) {

            trace_adsp_dsp_host_event("dsp reset asserted");

            cpu_reset(CPU(adsp->xtensa[0]->cpu));
            //vm_stop(RUN_STATE_SHUTDOWN); TODO: fix, causes hang
            adsp->in_reset = 1;

        } else if (adsp->in_reset && !(m->val & SHIM_CSR_STALL)) {

            trace_adsp_dsp_host_event("dsp reset cleared");

            cpu_resume(CPU(adsp->xtensa[0]->cpu));
            vm_start();
            adsp->in_reset = 0;
        }
        break;
    default:

        break;
    }
}

static void adsp_bdw_irq_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    struct adsp_io_info *info = adsp->shim;
    uint32_t active;

    active = hsw_region(SHIM_ISRD) & ~(hsw_region(SHIM_IMRD));

    trace_adsp_dsp_host_irq("host", hsw_region(SHIM_ISRD),
            hsw_region(SHIM_IMRD), active, hsw_region(SHIM_IPCX));

    if (active) {
        qemu_mutex_lock_iothread();
        adsp_set_lvl1_irq(adsp, adsp->desc->ia_irq, 1);
        qemu_mutex_unlock_iothread();
    }
}

static void adsp_bdw_shim_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    trace_adsp_dsp_host_msg("shim", msg->msg);

    switch (msg->msg) {
    case QEMU_IO_MSG_REG32W:
        do_shim32(adsp, msg);
        break;
    case QEMU_IO_MSG_REG32R:
        break;
    case QEMU_IO_MSG_REG64W:
        do_shim64(adsp, msg);
        break;
    case QEMU_IO_MSG_REG64R:
        break;
    default:
        break;
    }
}

static int bridge_cb(void *data, struct qemu_io_msg *msg)
{
    struct adsp_dev *adsp = (struct adsp_dev *)data;

    switch (msg->type) {
    case QEMU_IO_TYPE_REG:
        /* mostly handled by SHM, some exceptions */
        adsp_bdw_shim_msg(adsp, msg);
        break;
    case QEMU_IO_TYPE_IRQ:
        adsp_bdw_irq_msg(adsp, msg);
        break;
    case QEMU_IO_TYPE_DMA:
        dw_dma_msg(msg);
        break;
    case QEMU_IO_TYPE_MEM:
    case QEMU_IO_TYPE_PM:
    default:
        break;
    }

    return 0;
}

static void hsw_irq_set(struct adsp_io_info *info, int irq, uint32_t mask)
{
     struct adsp_dev *adsp = info->adsp;
     adsp_set_lvl1_irq(adsp, irq, 1);
}

static void hsw_irq_clear(struct adsp_io_info *info, int irq, uint32_t mask)
{
     struct adsp_dev *adsp = info->adsp;
     adsp_set_lvl1_irq(adsp, irq, 0);
}

struct adsp_dev_ops hsw_ops = {
    .irq_set = hsw_irq_set,
    .irq_clear = hsw_irq_clear,
};

static struct adsp_dev *adsp_init(const struct adsp_desc *board,
    MachineState *machine, unsigned mem_size)
{
    struct adsp_dev *adsp;
    uint8_t *ldata;
    size_t lsize;

    adsp = g_malloc(sizeof(*adsp));
    adsp->desc = board;
    adsp->shm_idx = 0;
    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->kernel_filename = qemu_opt_get(adsp->machine_opts, "kernel");
    adsp->ops = &hsw_ops;

    adsp->xtensa[0] = g_malloc(sizeof(struct adsp_xtensa));
    adsp->xtensa[0]->cpu = XTENSA_CPU(cpu_create(machine->cpu_type));

    if (adsp->xtensa[0]->cpu == NULL) {
        error_report("unable to find CPU definition '%s'", machine->cpu_type);
        exit(EXIT_FAILURE);
    }

    adsp->xtensa[0]->env = &adsp->xtensa[0]->cpu->env;
    adsp->xtensa[0]->env->sregs[PRID] = 0;

    /* Need MMU initialized prior to ELF loading,
     * so that ELF gets loaded into virtual addresses
     */
    cpu_reset(CPU(adsp->xtensa[0]->cpu));

    adsp_create_memory_regions(adsp);

    adsp_create_io_devices(adsp, NULL);

    /* reset all devices to init state */
    qemu_devices_reset();

#ifdef ENABLE_SHM
    /* initialise bridge to x86 host driver */
    qemu_io_register_child(machine->cpu_type, &bridge_cb, (void*)adsp);
#endif

    /* load binary file if one is specified on cmd line otherwise finish */
    if (adsp->kernel_filename == NULL) {
        qemu_log_mask(CPU_LOG_RESET,"%s initialised, waiting for FW load",
            board->name);
        return adsp;
    }

    /* load the binary image and copy to IRAM */
    ldata = g_malloc(mem_size);
    lsize = load_image_size(adsp->kernel_filename, ldata, mem_size);
    adsp_load_modules(adsp, ldata, lsize);

    return adsp;
}

const MemoryRegionOps hsw_shim_ops = {
    .read = shim_read,
    .write = shim_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void adsp_bdw_shim_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    shim_reset(info);
    adsp->shim = info;
}

static struct adsp_mem_desc hsw_mem[] = {
    {.name = "iram", .base = ADSP_HSW_DSP_IRAM_BASE,
        .size = ADSP_HSW_IRAM_SIZE},
    {.name = "dram", .base = ADSP_HSW_DSP_DRAM_BASE,
        .size = ADSP_HSW_DRAM_SIZE},
};

static struct adsp_reg_space hsw_io[] = {
    { .name = "dmac0", .irq = IRQ_NUM_EXT_DMAC0,
        .init = &dw_dma_init_dev, .ops = &dw_dmac_ops,
        .desc = {.base = ADSP_HSW_DMA0_BASE, .size = ADSP_HSW_DMA0_SIZE},},
    { .name = "dmac1", .irq = IRQ_NUM_EXT_DMAC1,
        .init = &dw_dma_init_dev, .ops = &dw_dmac_ops,
        .desc = {.base = ADSP_HSW_DMA1_BASE, .size = ADSP_HSW_DMA1_SIZE},},
    { .name = "ssp0", .irq = IRQ_NUM_EXT_SSP0,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_HSW_SSP0_BASE, .size = ADSP_HSW_SSP0_SIZE},},
    { .name = "ssp1", .irq = IRQ_NUM_EXT_SSP1,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_HSW_SSP1_BASE, .size = ADSP_HSW_SSP1_SIZE},},
    { .name = "shim", .init = &adsp_bdw_shim_init, .ops = &hsw_shim_ops,
        .desc = {.base = ADSP_HSW_DSP_SHIM_BASE, .size = ADSP_HSW_SHIM_SIZE},},
};

/* hardware memory map - TODO: update MBOX from BDW */
static const struct adsp_desc hsw_dsp_desc = {
    .name = "Haswell",
    .ia_irq = 4,

    .num_mem = ARRAY_SIZE(hsw_mem),
    .mem_region = hsw_mem,

    .num_io = ARRAY_SIZE(hsw_io),
    .io_dev = hsw_io,

    .mem_zones = {
        [SOF_FW_BLK_TYPE_IRAM] = {
            .base = ADSP_HSW_DSP_IRAM_BASE,
            .size = ADSP_HSW_IRAM_SIZE,
            .host_offset = ADSP_HSW_HOST_IRAM_OFFSET,
        },
        [SOF_FW_BLK_TYPE_DRAM] = {
            .base = ADSP_HSW_DSP_DRAM_BASE,
            .size = ADSP_HSW_DRAM_SIZE,
           .host_offset = ADSP_HSW_HOST_DRAM_OFFSET,
        },
    },
};

static struct adsp_mem_desc bdw_mem[] = {
    {.name = "iram", .base = ADSP_BDW_DSP_IRAM_BASE,
        .size = ADSP_BDW_IRAM_SIZE},
    {.name = "dram", .base = ADSP_BDW_DSP_DRAM_BASE,
        .size = ADSP_BDW_DRAM_SIZE},
};

static struct adsp_reg_space bdw_io[] = {
    { .name = "dmac0", .irq = IRQ_NUM_EXT_DMAC0,
        .init = &dw_dma_init_dev, .ops = &dw_dmac_ops,
        .desc = {.base = ADSP_BDW_DMA0_BASE, .size = ADSP_HSW_DMA0_SIZE},},
    { .name = "dmac1", .irq = IRQ_NUM_EXT_DMAC1,
        .init = &dw_dma_init_dev, .ops = &dw_dmac_ops,
        .desc = {.base = ADSP_BDW_DMA1_BASE, .size = ADSP_HSW_DMA1_SIZE},},
    { .name = "ssp0", .irq = IRQ_NUM_EXT_SSP0,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BDW_SSP0_BASE, .size = ADSP_HSW_SSP0_SIZE},},
    { .name = "ssp1", .irq = IRQ_NUM_EXT_SSP1,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BDW_SSP1_BASE, .size = ADSP_HSW_SSP1_SIZE},},
    { .name = "shim", .init = &adsp_bdw_shim_init, .ops = &hsw_shim_ops,
        .desc = {.base = ADSP_BDW_DSP_SHIM_BASE, .size = ADSP_HSW_SHIM_SIZE},},
};

/* hardware memory map - TODO: update MBOX from BDW */
static const struct adsp_desc bdw_dsp_desc = {
    .name = "Broadwell",
    .ia_irq = 4,

    .num_mem = ARRAY_SIZE(bdw_mem),
    .mem_region = bdw_mem,

    .num_io = ARRAY_SIZE(bdw_io),
    .io_dev = bdw_io,

    .mem_zones = {
        [SOF_FW_BLK_TYPE_IRAM] = {
            .base = ADSP_BDW_DSP_IRAM_BASE,
            .size = ADSP_BDW_IRAM_SIZE,
            .host_offset = ADSP_BDW_HOST_IRAM_OFFSET,
        },
        [SOF_FW_BLK_TYPE_DRAM] = {
            .base = ADSP_BDW_DSP_DRAM_BASE,
            .size = ADSP_BDW_DRAM_SIZE,
            .host_offset = ADSP_BDW_HOST_DRAM_OFFSET,
        },
    },
};

static void hsw_adsp_init(MachineState *machine)
{
    adsp_init(&hsw_dsp_desc, machine,
        ADSP_HSW_DSP_IRAM_BASE + ADSP_HSW_DSP_DRAM_BASE);
}

static void bdw_adsp_init(MachineState *machine)
{
    adsp_init(&bdw_dsp_desc, machine,
        ADSP_BDW_DSP_IRAM_BASE + ADSP_BDW_DSP_DRAM_BASE);
}

static void xtensa_bdw_machine_init(MachineClass *mc)
{
    mc->desc = "Broadwell HiFi2 Audio DSP";
    mc->is_default = false;
    mc->init = bdw_adsp_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_bdw", xtensa_bdw_machine_init)

static void xtensa_hsw_machine_init(MachineClass *mc)
{
    mc->desc = "Haswell HiFi2 Audio DSP";
    mc->is_default = false;
    mc->init = hsw_adsp_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_hsw", xtensa_hsw_machine_init)
