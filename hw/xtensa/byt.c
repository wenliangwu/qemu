/* Core DSP support for Baytrail audio DSP.
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
#include "hw/adsp/byt.h"
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
#define IRQ_NUM_EXT_DMAC2    19
#define IRQ_NUM_NMI          20

#define byt_region(raddr) info->region[raddr >> 2]

static uint32_t byt_get_timer(struct adsp_dev *adsp, struct adsp_io_info *info)
{
    int64_t qrel_nsecs;
    int32_t time;

    qrel_nsecs= qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - adsp->timer[0].start;
    time = qrel_nsecs / adsp->timer[0].ns_to_clock;

    return time;
}

static void rearm_ext_timer(struct adsp_dev *adsp, struct adsp_io_info *info)
{
    uint32_t wake = byt_region(SHIM_EXT_TIMER_CNTLL);

    byt_region(SHIM_EXT_TIMER_STAT) = byt_get_timer(adsp, info);

    timer_mod(adsp->timer[0].timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
            muldiv64(wake - byt_region(SHIM_EXT_TIMER_STAT),
                1000000, adsp->timer[0].clk_kHz));
}

static void *pmc_work(void *data)
{
    struct adsp_io_info *info = data;
    struct adsp_dev *adsp = info->adsp;

    /* delay for virtual PMC to do the work */
    usleep(50);

    /* perform any action after IRQ - ideally we should do this in thread*/
    switch (adsp->pmc_cmd) {
    case PMC_SET_LPECLK:

        /* set the clock bits */
        byt_region(SHIM_CLKCTL) &= ~SHIM_FR_LAT_CLK_MASK;
        byt_region(SHIM_CLKCTL) |=
            byt_region(SHIM_FR_LAT_REQ) & SHIM_FR_LAT_CLK_MASK;

        /* tell the DSP clock has been updated */
        byt_region(SHIM_CLKCTL) &= ~SHIM_CLKCTL_FRCHNGGO;
        byt_region(SHIM_CLKCTL) |= SHIM_CLKCTL_FRCHNGACK;

        break;
    default:
        break;
    }

    trace_adsp_dsp_shim_event("irq: SC send busy interrupt", adsp->pmc_cmd);

    /* now send IRQ to DSP from SC completion */
    byt_region(SHIM_IPCLPESCH) &= ~SHIM_IPCLPESCH_BUSY;
    byt_region(SHIM_IPCLPESCH) |= SHIM_IPCLPESCH_DONE;

    byt_region(SHIM_ISRLPESC) &= ~SHIM_ISRLPESC_BUSY;
    byt_region(SHIM_ISRLPESC) |= SHIM_ISRLPESC_DONE;

    /* need locking as virtual PMC is another thread */
    qemu_mutex_lock_iothread();
    adsp_set_lvl1_irq(adsp, adsp->desc->pmc_irq, 1);
    qemu_mutex_unlock_iothread();

    return NULL;
}

static void byt_ext_timer_cb(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    uint32_t pisr = byt_region(SHIM_PISR);

    pisr |= SHIM_PISR_EXTT;
    byt_region(SHIM_PISR) = pisr;

    trace_adsp_dsp_shim_timer("timeout", pisr);
    adsp_set_lvl1_irq(adsp, adsp->desc->ext_timer_irq, 1);
}

static void shim_reset(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;

    memset(info->region, 0, space->desc.size);
}

/* DSP FW reads */
static uint64_t shim_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

    switch (addr) {
    case SHIM_EXT_TIMER_STAT:
        byt_region(SHIM_EXT_TIMER_STAT) = byt_get_timer(adsp, info);
        break;
    case SHIM_PISR:
        byt_region(SHIM_PISR) = 0;
        break;
    default:
        break;
    }

    trace_adsp_dsp_shim_read(addr, byt_region(addr));

    return byt_region(addr);
}

/* DSP FW writes */
static void shim_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
#ifdef ENABLE_SHM
    struct qemu_io_msg_reg32 reg32;
    struct qemu_io_msg_irq irq;
#endif
    uint32_t active, isrx, isrlpesc;

    trace_adsp_dsp_shim_write(addr, val);

    /* special case registers */
    switch (addr) {
    case SHIM_IPCDL:

        /* set value via SHM */
        byt_region(SHIM_IPCDL) = val;

        /* reset the CPU and halt if we are dead to ease debugging */
        if ((val & 0xffff0000) == 0xdead0000) {

            trace_adsp_dsp_shim_event("firmware is dead. cpu held in reset", val);

            cpu_reset(CPU(adsp->xtensa[0]->cpu));
            //vm_stop(RUN_STATE_SHUTDOWN); TODO: fix, causes hang
            adsp->in_reset = 1;
        }
        break;
    case SHIM_IPCDH:
        /* DSP to host IPC command */

        /* set value via SHM */
        byt_region(SHIM_IPCDH) = val;

        /* set/clear status bit */
        isrx = byt_region(SHIM_ISRX) & ~(SHIM_ISRX_DONE | SHIM_ISRX_BUSY);
        isrx |= val & SHIM_IPCD_BUSY ? SHIM_ISRX_BUSY : 0;
        isrx |= val & SHIM_IPCD_DONE ? SHIM_ISRX_DONE : 0;
        byt_region(SHIM_ISRX) = isrx;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCD_BUSY) {

            trace_adsp_dsp_shim_event("irq: send BUSY interrupt to host", val);
#ifdef ENABLE_SHM
            /* send IRQ to parent */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
#endif
        }
        break;
    case SHIM_IPCXH:
        /* DSP to host IPC notify */

        /* set value via SHM */
        byt_region(SHIM_IPCXH) = val;

        /* set/clear status bit */
        isrx = byt_region(SHIM_ISRX) & ~(SHIM_ISRX_DONE | SHIM_ISRX_BUSY);
        isrx |= val & SHIM_IPCX_BUSY ? SHIM_ISRX_BUSY : 0;
        isrx |= val & SHIM_IPCX_DONE ? SHIM_ISRX_DONE : 0;
        byt_region(SHIM_ISRX) = isrx;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCX_DONE) {

            trace_adsp_dsp_shim_event("irq: send DONE interrupt to host", val);
#ifdef ENABLE_SHM
            /* send IRQ to parent */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
#endif
        }
        break;
    case SHIM_IMRD:

        /* set value via SHM */
        byt_region(SHIM_IMRD) = val;

        /* DSP IPC interrupt mask */
        active = byt_region(SHIM_ISRD) & ~(byt_region(SHIM_IMRD));

        trace_adsp_dsp_shim_mask("IMRD", byt_region(SHIM_ISRD),
            byt_region(SHIM_IMRD), active);

        if (!active) {
            trace_adsp_dsp_shim_irq("irq: de-assert IPC IRQ to host");
            adsp_set_lvl1_irq(adsp, adsp->desc->ia_irq, 0);
        }

        break;
    case SHIM_CSR:

        /* set value via SHM */
        byt_region(SHIM_CSR) = val;
#ifdef ENABLE_SHM
        /* now send msg to HOST VM to notify register write */
        reg32.hdr.type = QEMU_IO_TYPE_REG;
        reg32.hdr.msg = QEMU_IO_MSG_REG32W;
        reg32.hdr.size = sizeof(reg32);
        reg32.reg = addr;
        reg32.val = val;
        qemu_io_send_msg(&reg32.hdr);
#endif
        break;
    case SHIM_PISR:
        /* write 1 to clear bits */
        /* set value via SHM */
        byt_region(SHIM_PISR) &= ~val;

        if (val & SHIM_PISR_EXTT) {
            trace_adsp_dsp_shim_irq("irq: de-assert ext timer IRQ");
            adsp_set_lvl1_irq(adsp, adsp->desc->ext_timer_irq, 0);
        }

        break;
    case SHIM_PIMR:

        /* set value via SHM */
        byt_region(SHIM_PIMR) = val;

        /* DSP IO interrupt mask */
        active = byt_region(SHIM_PISR) & ~(byt_region(SHIM_PIMR));

        trace_adsp_dsp_shim_mask("PIMR", byt_region(SHIM_PISR),
            byt_region(SHIM_PIMR), active);

        if (!(active & SHIM_PISR_EXTT)) {
            trace_adsp_dsp_shim_irq("irq: mask timer IRQ");
            adsp_set_lvl1_irq(adsp, adsp->desc->ext_timer_irq, 0);
        }
        break;
    case SHIM_EXT_TIMER_CNTLL:
        /* set the timer timeout value via SHM */
        byt_region(SHIM_EXT_TIMER_CNTLL) = val;

        if (byt_region(SHIM_EXT_TIMER_CNTLH) & SHIM_EXT_TIMER_RUN)
            rearm_ext_timer(adsp, info);
        break;
    case SHIM_EXT_TIMER_CNTLH:

        /* enable the timer ? */
        if (val & SHIM_EXT_TIMER_RUN &&
            !(byt_region(addr) & SHIM_EXT_TIMER_RUN)) {
                adsp->timer[0].start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
                rearm_ext_timer(adsp, info);
        }

        /* set value via SHM */
        byt_region(SHIM_EXT_TIMER_CNTLH) = val;

        /* set/clear ext timer */
        if (val & SHIM_EXT_TIMER_CLEAR)
            byt_region(SHIM_EXT_TIMER_STAT) = 0;

        break;
    case SHIM_EXT_TIMER_STAT:
        /* set status value via SHM - should not be written to ? */
        byt_region(SHIM_EXT_TIMER_STAT) = val;
        rearm_ext_timer(adsp, info);
        break;
    case SHIM_IPCLPESCH:
        /* DSP to SCU IPC command */

        /* set value via SHM */
        byt_region(SHIM_IPCLPESCH) = val;

        /* set/clear status bit */
        isrlpesc = byt_region(SHIM_ISRLPESC) &
            ~(SHIM_ISRLPESC_DONE | SHIM_ISRLPESC_BUSY);
        isrlpesc |= val & SHIM_IPCLPESCH_BUSY ? SHIM_ISRLPESC_BUSY : 0;
        isrlpesc |= val & SHIM_IPCLPESCH_DONE ? SHIM_ISRLPESC_DONE : 0;
        byt_region(SHIM_ISRLPESC) = isrlpesc;

        /* do we need to send an IRQ ? */
        if (val & SHIM_IPCLPESCH_BUSY) {

            adsp->pmc_cmd = val & 0xff;

            /* perform any action prior to IRQ */
            switch (adsp->pmc_cmd) {
            case PMC_SET_LPECLK:
                byt_region(SHIM_CLKCTL) |= SHIM_CLKCTL_FRCHNGGO;
                byt_region(SHIM_CLKCTL) &= ~SHIM_CLKCTL_FRCHNGACK;
                break;
            default:
                break;
            }

            /* launch PMC virtual processor for IPC */
            qemu_thread_create(&adsp->pmc_thread, "byt-pmc",
                pmc_work, info, QEMU_THREAD_DETACHED);
        }
        break;
    case SHIM_IMRLPESC:

        /* set value via SHM */
        byt_region(SHIM_IMRLPESC) = val;

        /* DSP IPC interrupt mask */
        active = byt_region(SHIM_ISRLPESC) & ~(byt_region(SHIM_IMRLPESC));

        trace_adsp_dsp_shim_mask("IMRLPESC", byt_region(SHIM_ISRLPESC),
            byt_region(SHIM_IMRLPESC), active);

        if (!active) {
            trace_adsp_dsp_shim_irq("irq: de-assert IPC IRQ to PMC");
            adsp_set_lvl1_irq(adsp, adsp->desc->pmc_irq, 0);
        }

        break;
    default:
        break;
    }
}

const MemoryRegionOps byt_shim_ops = {
    .read = shim_read,
    .write = shim_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void byt_shim_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    shim_reset(info);
    adsp->shim = info;
    adsp->timer[0].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &byt_ext_timer_cb, info);
    adsp->timer[0].clk_kHz = 2500;
    adsp->timer[0].ns_to_clock = (1000000 / adsp->timer[0].clk_kHz);
}

#ifdef ENABLE_SHM

/* 32 bit SHIM IO from host */
static void do_shim32(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    struct qemu_io_msg_reg32 *m = (struct qemu_io_msg_reg32 *)msg;

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

static void byt_irq_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    struct adsp_io_info *info = adsp->shim;
    uint32_t active;

    active = byt_region(SHIM_ISRD) & ~(byt_region(SHIM_IMRD));

    trace_adsp_dsp_host_irq("host", byt_region(SHIM_ISRD),
        byt_region(SHIM_IMRD), active, byt_region(SHIM_IPCX));

    if (active) {
        qemu_mutex_lock_iothread();
        adsp_set_lvl1_irq(adsp, adsp->desc->ia_irq, 1);
        qemu_mutex_unlock_iothread();
    }
}

static void byt_shim_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
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

    trace_adsp_dsp_host_msg("received type", msg->type);

    switch (msg->type) {
    case QEMU_IO_TYPE_REG:
        /* mostly handled by SHM, some exceptions */
        byt_shim_msg(adsp, msg);
        break;
    case QEMU_IO_TYPE_IRQ:
        byt_irq_msg(adsp, msg);
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
#endif

static void byt_irq_set(struct adsp_io_info *info, int irq, uint32_t mask)
{
     struct adsp_dev *adsp = info->adsp;
     adsp_set_lvl1_irq(adsp, irq, 1);
}

static void byt_irq_clear(struct adsp_io_info *info, int irq, uint32_t mask)
{
     struct adsp_dev *adsp = info->adsp;
     adsp_set_lvl1_irq(adsp, irq, 0);
}

static struct adsp_dev_ops byt_ops = {
    .irq_set = byt_irq_set,
    .irq_clear = byt_irq_clear,
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
    adsp->ops = &byt_ops;

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

static uint64_t mbox_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    uint64_t value;

    value = byt_region(addr);
    trace_adsp_dsp_mbox_read(addr, value);

    return value;
}

static void mbox_write(void *opaque, hwaddr addr,
        uint64_t value, unsigned size)
{
    struct adsp_io_info *info = opaque;

    byt_region(addr) = value;
    trace_adsp_dsp_mbox_write(addr, value);
}

static const MemoryRegionOps mbox_io_ops = {
    .read = mbox_read,
    .write = mbox_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void byt_mbox_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    adsp->mbox = info;
}

static struct adsp_mem_desc byt_mem[] = {
    {.name = "iram", .base = ADSP_BYT_DSP_IRAM_BASE,
        .size = ADSP_BYT_IRAM_SIZE},
    {.name = "dram", .base = ADSP_BYT_DSP_DRAM_BASE,
        .size = ADSP_BYT_DRAM_SIZE},
};

static struct adsp_reg_space byt_io[] = {
    { .name = "dmac0", .irq = IRQ_NUM_EXT_DMAC0,
        .init = &dw_dma_init_dev, .ops = &dw_dmac_ops,
        .desc = {.base = ADSP_BYT_DMA0_BASE, .size = ADSP_BYT_DMA0_SIZE},},
    { .name = "dmac1", .irq = IRQ_NUM_EXT_DMAC1,
        .init = &dw_dma_init_dev, .ops = &dw_dmac_ops,
        .desc = {.base = ADSP_BYT_DMA1_BASE, .size = ADSP_BYT_DMA1_SIZE},},
    { .name = "ssp0", .irq = IRQ_NUM_EXT_SSP0,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BYT_SSP0_BASE, .size = ADSP_BYT_SSP0_SIZE},},
    { .name = "ssp1", .irq = IRQ_NUM_EXT_SSP1,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BYT_SSP1_BASE, .size = ADSP_BYT_SSP1_SIZE},},
    { .name = "ssp2", .irq = IRQ_NUM_EXT_SSP2,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BYT_SSP2_BASE, .size = ADSP_BYT_SSP2_SIZE},},
    { .name = "shim", .init = &byt_shim_init, .ops = &byt_shim_ops,
        .desc = {.base = ADSP_BYT_DSP_SHIM_BASE, .size = ADSP_BYT_SHIM_SIZE},},
    { .name = "mbox", .init = &byt_mbox_init, .ops = &mbox_io_ops,
        .desc = {.base = ADSP_BYT_DSP_MAILBOX_BASE, .size = ADSP_MAILBOX_SIZE},},
};

/* hardware memory map */
static const struct adsp_desc byt_dsp_desc = {
    .ia_irq = IRQ_NUM_EXT_IA,
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,
    .pmc_irq = IRQ_NUM_EXT_PMC,

    .num_mem = ARRAY_SIZE(byt_mem),
    .mem_region = byt_mem,

    .num_io = ARRAY_SIZE(byt_io),
    .io_dev = byt_io,

    .mem_zones = {
        [SOF_FW_BLK_TYPE_IRAM] = {
            .base = ADSP_BYT_DSP_IRAM_BASE,
            .size = ADSP_BYT_IRAM_SIZE,
            .host_offset = ADSP_BYT_HOST_IRAM_OFFSET,
        },
        [SOF_FW_BLK_TYPE_DRAM] = {
            .base = ADSP_BYT_DSP_DRAM_BASE,
            .size = ADSP_BYT_DRAM_SIZE,
            .host_offset = ADSP_BYT_HOST_DRAM_OFFSET,
        },
    },
};

static struct adsp_reg_space cht_io[] = {
    { .name = "dmac0", .irq = IRQ_NUM_EXT_DMAC0,
        .init = &dw_dma_init_dev, .ops = &dw_dmac_ops,
        .desc = {.base = ADSP_BYT_DMA0_BASE, .size = ADSP_BYT_DMA0_SIZE},},
    { .name = "dmac1", .irq = IRQ_NUM_EXT_DMAC1,
        .init = &dw_dma_init_dev, .ops = &dw_dmac_ops,
        .desc = {.base = ADSP_BYT_DMA1_BASE, .size = ADSP_BYT_DMA1_SIZE},},
    { .name = "dmac2", .irq = IRQ_NUM_EXT_DMAC2,
        .init = &dw_dma_init_dev, .ops = &dw_dmac_ops,
        .desc = {.base = ADSP_BYT_DMA2_BASE, .size = ADSP_BYT_DMA2_SIZE},},
    { .name = "ssp0", .irq = IRQ_NUM_EXT_SSP0,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BYT_SSP0_BASE, .size = ADSP_BYT_SSP0_SIZE},},
    { .name = "ssp1", .irq = IRQ_NUM_EXT_SSP1,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BYT_SSP1_BASE, .size = ADSP_BYT_SSP1_SIZE},},
    { .name = "ssp2", .irq = IRQ_NUM_EXT_SSP2,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BYT_SSP2_BASE, .size = ADSP_BYT_SSP2_SIZE},},
    { .name = "ssp3", .irq = IRQ_NUM_EXT_SSP0,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BYT_SSP3_BASE, .size = ADSP_BYT_SSP3_SIZE},},
    { .name = "ssp4", .irq = IRQ_NUM_EXT_SSP1,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BYT_SSP4_BASE, .size = ADSP_BYT_SSP4_SIZE},},
    { .name = "ssp5", .irq = IRQ_NUM_EXT_SSP2,
        .init = &adsp_ssp_init, .ops = &ssp_ops,
        .desc = {.base = ADSP_BYT_SSP5_BASE, .size = ADSP_BYT_SSP5_SIZE},},
    { .name = "shim", .init = &byt_shim_init, .ops = &byt_shim_ops,
        .desc = {.base = ADSP_BYT_DSP_SHIM_BASE, .size = ADSP_BYT_SHIM_SIZE},},
    { .name = "mbox", .ops = &mbox_io_ops,
        .desc = {.base = ADSP_BYT_DSP_MAILBOX_BASE, .size = ADSP_MAILBOX_SIZE},},
};

/* hardware memory map */
static const struct adsp_desc cht_dsp_desc = {
    .ia_irq = IRQ_NUM_EXT_IA,
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,
    .pmc_irq = IRQ_NUM_EXT_PMC,

    .num_mem = ARRAY_SIZE(byt_mem),
    .mem_region = byt_mem,

    .num_io = ARRAY_SIZE(cht_io),
    .io_dev = cht_io,

   .mem_zones = {
        [SOF_FW_BLK_TYPE_IRAM] = {
                .base = ADSP_BYT_DSP_IRAM_BASE,
                .size = ADSP_BYT_IRAM_SIZE,
                .host_offset = ADSP_BYT_HOST_IRAM_OFFSET,
        },
        [SOF_FW_BLK_TYPE_DRAM] = {
                .base = ADSP_BYT_DSP_DRAM_BASE,
                .size = ADSP_BYT_DRAM_SIZE,
                .host_offset = ADSP_BYT_HOST_DRAM_OFFSET,
        },
    },
};

static void byt_adsp_init(MachineState *machine)
{
    adsp_init(&byt_dsp_desc, machine, ADSP_BYT_IRAM_SIZE + ADSP_BYT_DRAM_SIZE);
}

static void cht_adsp_init(MachineState *machine)
{
    adsp_init(&cht_dsp_desc, machine, ADSP_BYT_IRAM_SIZE + ADSP_BYT_DRAM_SIZE);
}

static void bsw_adsp_init(MachineState *machine)
{
    adsp_init(&cht_dsp_desc, machine, ADSP_BYT_IRAM_SIZE + ADSP_BYT_DRAM_SIZE);
}

static void xtensa_byt_machine_init(MachineClass *mc)
{
    mc->desc = "Baytrail HiFi2 Audio DSP";
    mc->is_default = false;
    mc->init = byt_adsp_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_byt", xtensa_byt_machine_init)

static void xtensa_cht_machine_init(MachineClass *mc)
{
    mc->desc = "Cherrytrail HiFi2 Audio DSP";
    mc->is_default = false;
    mc->init = cht_adsp_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_cht", xtensa_cht_machine_init)

static void xtensa_bsw_machine_init(MachineClass *mc)
{
    mc->desc = "Braswell HiFi2 Audio DSP";
    mc->is_default = false;
    mc->init = bsw_adsp_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_bsw", xtensa_bsw_machine_init)
