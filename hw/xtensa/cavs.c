/* Core DSP support for Intel CAVS audio DSPs.
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
#include "hw/adsp/cavs.h"
#include "hw/ssi/ssp.h"
#include "hw/dma/dw-dma.h"
#include "hw/dma/hda-dma.h"
#include "common.h"
#include "manifest.h"
#include "hw/adsp/fw.h"

#define ENABLE_SHM
#ifdef ENABLE_SHM
#include "qemu/io-bridge.h"
#endif

#include "trace.h"

#define MAX_IMAGE_SIZE (1024 * 1024 *4)

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

#define cavs_region(raddr)    info->region[raddr >> 2]

static inline uint64_t ns2ticks(uint64_t ns, uint64_t clk_kHz)
{
    return ns * clk_kHz * 1000 / ( 1000 * 1000 * 1000 );
}

static inline uint64_t ticks2ns(uint64_t ticks, uint64_t clk_kHz)
{
    return ticks * 1000 * 1000 / clk_kHz;
}

static uint64_t cavs_set_time(struct adsp_dev *adsp, struct adsp_io_info *info)
{
    uint64_t time = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - adsp->timer[0].start);
    uint64_t ticks = ns2ticks(time, adsp->timer[0].clk_kHz);

    cavs_region((SHIM_DSPWC + 4)) = (uint32_t)(ticks >> 32);
    cavs_region((SHIM_DSPWC + 0)) = (uint32_t)(ticks & 0xffffffff);

    return time;
}

static void rearm_ext_timer0(struct adsp_dev *adsp,struct adsp_io_info *info)
{
    uint64_t waketicks = ((uint64_t)(cavs_region((SHIM_DSPWCTT0C + 4))) << 32) |
        cavs_region((SHIM_DSPWCTT0C + 0));

    cavs_set_time(adsp, info);


    uint64_t waketime = ticks2ns(waketicks, adsp->timer[0].clk_kHz) + adsp->timer[0].start;

    if (waketime < qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))
        timer_mod(adsp->timer[0].timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10000);
    else {
        timer_mod(adsp->timer[0].timer, waketime);
    }
}

static void rearm_ext_timer1(struct adsp_dev *adsp,struct adsp_io_info *info)
{
    uint64_t waketicks = ((uint64_t)(cavs_region((SHIM_DSPWCTT1C + 4))) << 32) |
        cavs_region((SHIM_DSPWCTT1C + 0));

    cavs_set_time(adsp, info);

    uint64_t waketime = ticks2ns(waketicks, adsp->timer[0].clk_kHz) + adsp->timer[0].start;

    if (waketime < qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))
        timer_mod(adsp->timer[1].timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 10);
    else {
        timer_mod(adsp->timer[1].timer, waketime);
    }
}

void cavs_ext_timer_cb0(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

    /* clear T0A */
    cavs_region(SHIM_DSPWCTTCS) &= ~SHIM_DSPWCTTCS_T0A;
    /* set T0T */
    cavs_region(SHIM_DSPWCTTCS) |= SHIM_DSPWCTTCS_T0T;

    /* Interrupt may be generated if IL2MDx.FCT0 bit is set. */
    cavs_irq_set(adsp->timer[0].info, IRQ_DWCT0, 0);
}

void cavs_ext_timer_cb1(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

     /* clear T0A */
    cavs_region(SHIM_DSPWCTTCS) &= ~SHIM_DSPWCTTCS_T1A;
    /* set T0T */
    cavs_region(SHIM_DSPWCTTCS) |= SHIM_DSPWCTTCS_T1T;

    /* Interrupt may be generated if IL2MDx.FCT1 bit is set. */
    cavs_irq_set(adsp->timer[1].info, IRQ_DWCT1, 0);
}

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
    struct adsp_dev *adsp = info->adsp;

    switch (addr) {
    case SHIM_DSPWC:
        cavs_set_time(adsp, info);
        break;
    default:
        break;
    }

    trace_adsp_dsp_shim_read(addr, cavs_region(addr));

    return cavs_region(addr);
}

/* SHIM IO from ADSP */
static void shim_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    uint64_t waketicks;

    waketicks = ((uint64_t)(cavs_region((SHIM_DSPWCTT0C + 4))) << 32);
    waketicks |= cavs_region((SHIM_DSPWCTT0C + 0));

    trace_adsp_dsp_shim_write(addr, val);

    /* special case registers */
    switch (addr) {
    case SHIM_DSPWC:
        break;
    case SHIM_DSPWCTTCS:
    /* set timer only in valid */
        if ((val & SHIM_DSPWCTTCS_T0A) &&
            waketicks ) {
            rearm_ext_timer0(adsp, info);
            cavs_region(addr) |= val;
    }
        if ((val & SHIM_DSPWCTTCS_T1A) &&
            waketicks ) {
            rearm_ext_timer1(adsp, info);
            cavs_region(addr) |= val;
    }
    /* clear IRQ */
        if ((val & SHIM_DSPWCTTCS_T0T) &&
            (cavs_region(addr) & SHIM_DSPWCTTCS_T0T)) {
            cavs_irq_clear(adsp->timer[0].info, IRQ_DWCT0, 0);
            cavs_region(addr) &= ~val;
    }
        if ((val & SHIM_DSPWCTTCS_T1T) &&
            (cavs_region(addr) & SHIM_DSPWCTTCS_T1T)) {
            cavs_irq_clear(adsp->timer[1].info, IRQ_DWCT1, 0);
            cavs_region(addr) &= ~val;
    }
        break;
    case SHIM_DSPWCTT0C:
        cavs_region(addr) = val;
        break;
    case SHIM_DSPWCTT0C + 4:
        cavs_region(addr) = val;
        break;
    case SHIM_DSPWCTT1C:
        cavs_region(addr) = val;
    case SHIM_DSPWCTT1C + 4:
        cavs_region(addr) = val;
        break;
    case SHIM_CLKCTL:
    cavs_region(addr) = val;
    cavs_region(SHIM_CLKSTS) = val;
    break;
    default:
        break;
    }
}
#if 0
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
#endif
static void adsp_cavs_shim_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    trace_adsp_dsp_host_msg("shim", msg->msg);

    switch (msg->msg) {
    case QEMU_IO_MSG_REG32W:
       // do_shim32(adsp, msg);
        break;
    case QEMU_IO_MSG_REG32R:
        break;
    case QEMU_IO_MSG_REG64W:
       // do_shim64(adsp, msg);
        break;
    case QEMU_IO_MSG_REG64R:
        break;
    default:
        break;
    }
}

const MemoryRegionOps cavs_shim_ops = {
    .read = shim_read,
    .write = shim_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void adsp_cavs_shim_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    shim_reset(info);
    adsp->shim = info;
    cavs_region(0x94) = 0x00000080;
    adsp->timer[0].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb0, info);
    adsp->timer[0].clk_kHz = adsp->clk_kHz;
    adsp->timer[1].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb1, info);
    adsp->timer[1].clk_kHz = adsp->clk_kHz;
    adsp->timer[0].start = adsp->timer[1].start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static uint64_t io_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_shim_read(addr, cavs_region(addr));

    return cavs_region(addr);
}

/* SHIM IO from ADSP */
static void io_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;

    cavs_region(addr) = val;

    trace_adsp_dsp_shim_write(addr, val);
}

static const MemoryRegionOps cavs_io_ops = {
    .read = io_read,
    .write = io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* SKL, KBL, APL */
static const struct cavs_irq_desc cavs_1_5_irqs[] = {
    {IRQ_HPGPDMA, 2, 0xff000000, 24},
    {IRQ_DWCT1, 2, 0x00800000},
    {IRQ_DWCT0, 2, 0x00400000},
    {IRQ_L2ME, 2, 0x00200000},
    {IRQ_DTS, 2, 0x00100000},
    {IRQ_IDC, 2, 0x00000080},
    {IRQ_IPC, 2, 0x00000040},

    {IRQ_DSPGCL, 3, 0x80000000},
    {IRQ_DSPGHOS, 3, 0x7fff0000, 16},
    {IRQ_HPGPDMA0, 3, 0x00008000},
    {IRQ_DSPGHIS, 3, 0x00007fff, 0},

    {IRQ_LPGPDMA1, 4, 0x80000000},
    {IRQ_DSPGLOS, 4, 0x7fff0000, 16},
    {IRQ_LPGPDMA0, 4, 0x00008000},
    {IRQ_DSPGLIS, 4, 0x00007fff, 0},

    {IRQ_LPGPDMA1, 5, 0xff000000, 24},
    {IRQ_LPGPDMA0, 5, 0x00ff0000, 16},
    {IRQ_DMIC0, 5, 0x00000040},
    {IRQ_SSP5, 5, 0x00000020},
    {IRQ_SSP4, 5, 0x00000010},
    {IRQ_SSP3, 5, 0x00000008},
    {IRQ_SSP2, 5, 0x00000004},
    {IRQ_SSP1, 5, 0x00000002},
    {IRQ_SSP0, 5, 0x00000001},
};

/* CNL, ICL, SUE, levels 2,3, 4 the same as above */
static const struct cavs_irq_desc cavs_1_8_irqs[] = {
    {IRQ_HPGPDMA, 2, 0xff000000, 24},
    {IRQ_DWCT1, 2, 0x00800000},
    {IRQ_DWCT0, 2, 0x00400000},
    {IRQ_L2ME, 2, 0x00200000},
    {IRQ_DTS, 2, 0x00100000},
    {IRQ_IDC, 2, 0x00000080},
    {IRQ_IPC, 2, 0x00000040},

    {IRQ_DSPGCL, 3, 0x80000000},
    {IRQ_DSPGHOS, 3, 0x7fff0000, 16},
    {IRQ_HPGPDMA0, 3, 0x00008000},
    {IRQ_DSPGHIS, 3, 0x00007fff, 0},

    {IRQ_LPGPDMA1, 4, 0x80000000},
    {IRQ_DSPGLOS, 4, 0x7fff0000, 16},
    {IRQ_LPGPDMA0, 4, 0x00008000},
    {IRQ_DSPGLIS, 4, 0x00007fff, 0},

    {IRQ_LPGPDMA, 5, 0x00010000},
    {IRQ_DWCT1, 5, 0x00008000},
    {IRQ_DWCT0, 5, 0x00004000},
    {IRQ_SNDW, 5, 0x00000800},
    {IRQ_DMIC0, 5, 0x00000080},
    {IRQ_SSP5, 5, 0x00000020},
    {IRQ_SSP4, 5, 0x00000010},
    {IRQ_SSP3, 5, 0x00000008},
    {IRQ_SSP2, 5, 0x00000004},
    {IRQ_SSP1, 5, 0x00000002},
    {IRQ_SSP0, 5, 0x00000001},
};

static const struct cavs_irq_map irq_map[] = {
    {2, IRQ_NUM_EXT_LEVEL2},
    {3, IRQ_NUM_EXT_LEVEL3},
    {4, IRQ_NUM_EXT_LEVEL4},
    {5, IRQ_NUM_EXT_LEVEL5},
};

/* mask values copied as is */
static void cavs_do_set_irq(struct adsp_io_info *info,
    const struct cavs_irq_desc *irq_desc, uint32_t mask)
{
    if (irq_desc->shift) {
        cavs_region(ILRSD(irq_desc->level)) &= ~irq_desc->mask;
        cavs_region(ILRSD(irq_desc->level)) |= (mask << irq_desc->shift);
    } else {
        cavs_region(ILRSD(irq_desc->level)) |= irq_desc->mask;
    }

    cavs_region(ILSC(irq_desc->level)) =
        cavs_region(ILRSD(irq_desc->level)) &
        (~cavs_region(ILMC(irq_desc->level)));

    adsp_set_lvl1_irq(info->adsp, irq_map[irq_desc->level - 2].irq, 1);
}

/* mask values copied as is */
static void cavs_do_clear_irq(struct adsp_io_info *info,
    const struct cavs_irq_desc *irq_desc, uint32_t mask)
{
    if (irq_desc->shift) {
       cavs_region(ILRSD(irq_desc->level)) &= ~irq_desc->mask;
       cavs_region(ILRSD(irq_desc->level)) |= (mask << irq_desc->shift);
    } else {
        cavs_region(ILRSD(irq_desc->level)) &= ~irq_desc->mask;
    }
     cavs_region(ILSC(irq_desc->level)) =
        cavs_region(ILRSD(irq_desc->level)) &
        (~cavs_region(ILMC(irq_desc->level)));

    if (!cavs_region(ILSC(irq_desc->level)))
        adsp_set_lvl1_irq(info->adsp, irq_map[irq_desc->level - 2].irq, 0);
}

static void cavs_irq_1_5_set(struct adsp_io_info *info, int irq, uint32_t mask)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(cavs_1_5_irqs); i++) {
        if (irq == cavs_1_5_irqs[i].id)
            cavs_do_set_irq(info, &cavs_1_5_irqs[i], mask);
    }
}

static void cavs_irq_1_5_clear(struct adsp_io_info *info, int irq, uint32_t mask)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(cavs_1_5_irqs); i++) {
        if (irq == cavs_1_5_irqs[i].id)
            cavs_do_clear_irq(info, &cavs_1_5_irqs[i], mask);
    }
}

static const struct adsp_dev_ops cavs_1_5_ops = {
    .irq_set = cavs_irq_1_5_set,
    .irq_clear = cavs_irq_1_5_clear,
};

static void cavs_irq_1_8_set(struct adsp_io_info *info, int irq, uint32_t mask)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(cavs_1_8_irqs); i++) {
        if (irq == cavs_1_8_irqs[i].id)
            cavs_do_set_irq(info, &cavs_1_8_irqs[i], mask);
    }
}

static void cavs_irq_1_8_clear(struct adsp_io_info *info, int irq, uint32_t mask)
{
    int i;

    for (i = 0; i < ARRAY_SIZE(cavs_1_8_irqs); i++) {
        if (irq == cavs_1_8_irqs[i].id)
            cavs_do_clear_irq(info, &cavs_1_8_irqs[i], mask);
    }
}

static const struct adsp_dev_ops cavs_1_8_ops = {
    .irq_set = cavs_irq_1_8_set,
    .irq_clear = cavs_irq_1_8_clear,
};

void cavs_irq_set(struct adsp_io_info *info, int irq, uint32_t mask)
{
    struct adsp_dev *adsp = info->adsp;
    adsp->ops->irq_set(info, irq, mask);
}

void cavs_irq_clear(struct adsp_io_info *info, int irq, uint32_t mask)
{
     struct adsp_dev *adsp = info->adsp;
     adsp->ops->irq_clear(info, irq, mask);
}

static void adsp_cavs_irq_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
    qemu_mutex_lock_iothread();
    adsp_set_lvl1_irq(adsp, IRQ_IPC, 1);
    qemu_mutex_unlock_iothread();
}

static int bridge_cb(void *data, struct qemu_io_msg *msg)
{
    struct adsp_dev *adsp = (struct adsp_dev *)data;

    switch (msg->type) {
    case QEMU_IO_TYPE_REG:
        /* mostly handled by SHM, some exceptions */
        adsp_cavs_shim_msg(adsp, msg);
        break;
    case QEMU_IO_TYPE_IRQ:
        adsp_cavs_irq_msg(adsp, msg);
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

#define NUM_SEGMENTS    3
#define PAGE_SIZE   4096

static void copy_man_modules(const struct adsp_desc *board, struct adsp_dev *adsp,
    struct adsp_fw_desc *desc)
{
    struct module *mod;
    struct adsp_mem_desc *mem;
    struct adsp_fw_header *hdr = &desc->header;
    unsigned long foffset, soffset, ssize;
    void *base_ptr = desc;
    int i, j;

    base_ptr -= board->file_offset;

    qemu_log("found %d modules\n", hdr->num_module_entries);
    qemu_log("using file offset 0x%x\n", board->file_offset);

    /* copy modules to SRAM */
    for (i = 0; i < hdr->num_module_entries; i++) {

        mod = &desc->module[i];
        qemu_log("checking module %d : %s\n", i, mod->name);

        for (j = 0; j < NUM_SEGMENTS; j++) {

            if (mod->segment[j].flags.r.load == 0)
                continue;

            foffset = mod->segment[j].file_offset;
            ssize = mod->segment[j].flags.r.length * PAGE_SIZE;

            mem = adsp_get_mem_space(adsp, mod->segment[j].v_base_addr);

            if (mem) {

                soffset = mod->segment[j].v_base_addr - mem->base;

                qemu_log(" %s segment %d file offset 0x%lx MEM addr 0x%x offset 0x%lx size 0x%lx\n",
                    mem->name, j, foffset, mod->segment[j].v_base_addr, soffset, ssize);

                /* copy text to SRAM */
                memcpy(mem->ptr + soffset, (void*)base_ptr + foffset, ssize);

            } else {

                soffset = mod->segment[j].v_base_addr;

                qemu_log(" Unmatched segment %d file offset 0x%lx SRAM addr 0x%x offset 0x%lx size 0x%lx\n",
                    j, foffset, mod->segment[j].v_base_addr, soffset, ssize);
            }
        }
    }
}

static void copy_man_to_imr(const struct adsp_desc *board, struct adsp_dev *adsp,
    struct adsp_fw_desc *desc, uint32_t imr_addr)
{
    struct adsp_fw_header *hdr = &desc->header;
    struct adsp_mem_desc *mem;

    mem = adsp_get_mem_space(adsp, imr_addr);
    if (!mem)
        return;

    /* copy manifest to IMR */
    memcpy(mem->ptr + board->imr_boot_ldr_offset, (void*)hdr,
                 hdr->preload_page_count * PAGE_SIZE);

    qemu_log("ROM loader: copy %d kernel pages to IMR\n",
        hdr->preload_page_count);
}

#define HEADER_MAGIC    0x314d4124

static struct adsp_dev *adsp_init(const struct adsp_desc *board,
    MachineState *machine, const char *name, int copy_modules,
    uint32_t exec_addr, uint32_t imr_addr, uint32_t clk_kHz)
{
    struct adsp_dev *adsp;
    struct adsp_mem_desc *mem;
    void *man_ptr, *desc_ptr;
    int n, skip, size;
    void *rom;

    adsp = g_malloc(sizeof(*adsp));
    adsp->desc = board;
    adsp->shm_idx = 0;
    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->kernel_filename = qemu_opt_get(adsp->machine_opts, "kernel");
    adsp->rom_filename = qemu_opt_get(adsp->machine_opts, "rom");
    adsp->ops = board->ops;
    adsp->clk_kHz = clk_kHz;

    for (n = 0; n < machine->smp.cpus; n++) {

        adsp->xtensa[n] = g_malloc(sizeof(struct adsp_xtensa));
        adsp->xtensa[n]->cpu = XTENSA_CPU(cpu_create(machine->cpu_type));

        if (adsp->xtensa[n]->cpu == NULL) {
            error_report("unable to find CPU definition '%s'", machine->cpu_type);
            exit(EXIT_FAILURE);
        }

        adsp->xtensa[n]->env = &adsp->xtensa[n]->cpu->env;
        adsp->xtensa[n]->env->sregs[PRID] = n;

        /* Need MMU initialized prior to ELF loading,
        * so that ELF gets loaded into virtual addresses
        */
        cpu_reset(CPU(adsp->xtensa[n]->cpu));
    }

    adsp_create_memory_regions(adsp);
    adsp_create_io_devices(adsp, &cavs_io_ops);

    /* reset all devices to init state */
    qemu_devices_reset();

    /* initialise bridge to x86 host driver */
    qemu_io_register_child(machine->cpu_type, &bridge_cb, (void*)adsp);

    /* load binary file if one is specified on cmd line otherwise finish */
    if (adsp->kernel_filename == NULL) {
        qemu_log_mask(CPU_LOG_RESET,"%s initialised, waiting for FW load",
                   board->name);
        return adsp;
    }

    if (adsp->rom_filename != NULL) {

        /* get ROM */
        mem = adsp_get_mem_space(adsp, ADSP_CAVS_DSP_ROM_BASE);
        if (!mem)
            return NULL;

        /* load ROM image and copy to ROM */
        rom = g_malloc(ADSP_CAVS_DSP_ROM_SIZE);
        load_image_size(adsp->rom_filename, rom,
            ADSP_CAVS_DSP_ROM_SIZE);

        memcpy(mem->ptr, rom, ADSP_CAVS_DSP_ROM_SIZE);
    }

    /* load the binary image and copy to SRAM */
    man_ptr = g_malloc(MAX_IMAGE_SIZE);
    size = load_image_size(adsp->kernel_filename, man_ptr,
        MAX_IMAGE_SIZE);

    /* executable manifest header */
    if (exec_addr) {

        /* get ROM */
        mem = adsp_get_mem_space(adsp, exec_addr);
        if (!mem)
            exit(EXIT_FAILURE);

        qemu_log("copying exec manifest header 0x%x bytes to %s 0x%zx\n",
                size, mem->name, mem->base);
        memcpy(mem->ptr + board->imr_boot_ldr_offset, man_ptr, size);
        goto out;
    }

    skip = adsp_get_ext_man_size(man_ptr);

    /* Search for manifest ID = "$AEM" */
    desc_ptr = (uint8_t *)man_ptr + skip;
    while (*((uint32_t*)desc_ptr) != HEADER_MAGIC) {
        desc_ptr = desc_ptr + sizeof(uint32_t);
        skip += sizeof(uint32_t);
        if (skip >= size) {
            error_report("error: failed to find FW manifest header $AM1\n");
            exit(EXIT_FAILURE);
        }
    }

    qemu_log("Header $AM1 found at offset 0x%x bytes\n", skip);

    /* does ROM or VM load manifest */
    if (adsp->rom_filename != NULL && !copy_modules) {

         /* copy whole manifest if required */
         copy_man_to_imr(board, adsp, desc_ptr, imr_addr);

    } else {

        /* copy manifest modules if required */
        copy_man_modules(board, adsp, desc_ptr);
    }

out:
    return adsp;
}

static void cavs_irq_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    adsp->timer[0].info = info;
    adsp->timer[1].info = info;
}

static uint64_t cavs_irq_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_shim_read(addr, cavs_region(addr));

    return cavs_region(addr);
}

static void cavs_irq_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

    trace_adsp_dsp_shim_write(addr, val);

    switch (addr) {
    case ILMSD(2):
        /* mask set - level 2 */
        if (!(cavs_region(ILMC(2)) & val)) {
            cavs_region(ILMC(2)) |= val;

            cavs_region(ILSC(2)) =
                (~cavs_region(ILMC(2))) & cavs_region(ILRSD(2));

            /* clear IRQ if no other IRQ left */
            if (!cavs_region(ILSC(2)))
                 adsp_set_lvl1_irq(adsp, IRQ_NUM_EXT_LEVEL2, 0);
        }
        break;
    case ILMSD(3):
         /* mask set - level 3 */
        if (!(cavs_region(ILMC(3)) & val)) {
            cavs_region(ILMC(3)) |= val;

            cavs_region(ILSC(3)) =
                (~cavs_region(ILMC(3))) & cavs_region(ILRSD(3));

            /* clear IRQ if no other IRQ left */
            if (!cavs_region(ILSC(3)))
                 adsp_set_lvl1_irq(adsp, IRQ_NUM_EXT_LEVEL3, 0);
        }
        break;
    case ILMSD(4):
        /* mask set - level 4 */
        if (!(cavs_region(ILMC(4)) & val)) {
            cavs_region(ILMC(4)) |= val;

            cavs_region(ILSC(4)) =
                (~cavs_region(ILMC(4))) & cavs_region(ILRSD(4));

            /* clear IRQ if no other IRQ left */
            if (!cavs_region(ILSC(4)))
                 adsp_set_lvl1_irq(adsp, IRQ_NUM_EXT_LEVEL4, 0);
        }
        break;
    case ILMSD(5):
        /* mask set - level 5 */
        if (!(cavs_region(ILMC(5)) & val)) {
            cavs_region(ILMC(5)) |= val;

            cavs_region(ILSC(5)) =
                (~cavs_region(ILMC(5))) & cavs_region(ILRSD(5));

            /* clear IRQ if no other IRQ left */
            if (!cavs_region(ILSC(5)))
                 adsp_set_lvl1_irq(adsp, IRQ_NUM_EXT_LEVEL5, 0);
        }
        break;
    case ILMCD(2):
         /* mask clear - level 2 */
         if (cavs_region(ILMC(2)) & val) {
             cavs_region(ILMC(2)) &= ~val;

            cavs_region(ILSC(2)) =
                (~cavs_region(ILMC(2))) & cavs_region(ILRSD(2));

            /* generate an IRQ if it was masked */
            if (cavs_region(ILSC(2)))
                 adsp_set_lvl1_irq(adsp, IRQ_NUM_EXT_LEVEL2, 1);
        }
        break;
     case ILMCD(3):
         /* mask clear - level 3 */
         if (cavs_region(ILMC(3)) & val) {
             cavs_region(ILMC(3)) &= ~val;

            cavs_region(ILSC(3)) =
                (~cavs_region(ILMC(3))) & cavs_region(ILRSD(3));

            /* generate an IRQ if it was masked */
            if (cavs_region(ILSC(3)))
                 adsp_set_lvl1_irq(adsp, IRQ_NUM_EXT_LEVEL3, 1);
        }
        break;
     case ILMCD(4):
         /* mask clear - level 4 */
         if (cavs_region(ILMC(4)) & val) {
             cavs_region(ILMC(4)) &= ~val;

            cavs_region(ILSC(4)) =
                (~cavs_region(ILMC(4))) & cavs_region(ILRSD(4));

            /* generate an IRQ if it was masked */
            if (cavs_region(ILSC(4)))
                 adsp_set_lvl1_irq(adsp, IRQ_NUM_EXT_LEVEL4, 1);
        }
        break;

     case ILMCD(5):
         /* mask clear - level 5 */
         if (cavs_region(ILMC(5)) & val) {
             cavs_region(ILMC(5)) &= ~val;

            cavs_region(ILSC(5)) =
                (~cavs_region(ILMC(5))) & cavs_region(ILRSD(5));

            /* generate an IRQ if it was masked */
            if (cavs_region(ILSC(5)))
                 adsp_set_lvl1_irq(adsp, IRQ_NUM_EXT_LEVEL5, 1);
        }
        break;
    default:
    break;
    }
}

static const MemoryRegionOps cavs_irq_io_ops = {
    .read = cavs_irq_read,
    .write = cavs_irq_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint64_t cavs_ipc_1_5_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_shim_read(addr, cavs_region(addr));

    return cavs_region(addr);
}

/* SHIM IO from ADSP */
static void cavs_ipc_1_5_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct qemu_io_msg_irq irq;

    trace_adsp_dsp_shim_write(addr, val);

    /* special case registers */
    switch (addr) {
    case IPC_DIPCT:
        /* host to DSP */
        if (val & IPC_DIPCT_DSPLRST)
            cavs_region(addr) &= ~IPC_DIPCT_DSPLRST;
    break;
    case IPC_DIPCI:
        /* DSP to host */
        cavs_region(addr) = val;

        if (val & IPC_DIPCI_DSPRST) {
            trace_adsp_dsp_shim_event("irq: send BUSY interrupt to host", val);

            /* send IRQ to parent */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
        }
    break;
        case IPC_DIPCIE:
            cavs_region(addr) = val & ~(0x1 << 31);
            if (val & IPC_DIPCIE_DONE)
                cavs_region(addr) &= ~IPC_DIPCIE_DONE;
        break;
        case IPC_DIPCCTL5:
             /* assume interrupts are not masked atm */
             cavs_region(addr) = val;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps cavs_ipc_v1_5_io_ops = {
    .read = cavs_ipc_1_5_read,
    .write = cavs_ipc_1_5_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* CAVS 1.5 IO devices */
static struct adsp_reg_space cavs_1_5_io[] = {
        { .name = "cmd",
            .desc = {.base = ADSP_CAVS_1_5_DSP_CMD_BASE, .size = ADSP_CAVS_1_5_DSP_CMD_SIZE},},
        { .name = "res",
            .desc = {.base = ADSP_CAVS_1_5_DSP_RES_BASE, .size = ADSP_CAVS_1_5_DSP_RES_SIZE},},
        { .name = "ipc-dsp",  .ops = &cavs_ipc_v1_5_io_ops,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IPC_HOST_BASE, .size = ADSP_CAVS_1_5_DSP_IPC_HOST_SIZE},},
        { .name = "idc0",
            .desc = {.base = ADSP_CAVS_1_5_DSP_IPC_DSP_BASE(0), .size = ADSP_CAVS_1_5_DSP_IPC_DSP_SIZE},},
        { .name = "idc1",
            .desc = {.base = ADSP_CAVS_1_5_DSP_IPC_DSP_BASE(1), .size = ADSP_CAVS_1_5_DSP_IPC_DSP_SIZE},},
        { .name = "hostwin0",
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(0), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin1",
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(1), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin2",
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(2), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin3",
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(3), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "irq",
            .init = cavs_irq_init, .ops = &cavs_irq_io_ops,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IRQ_BASE, .size = ADSP_CAVS_1_5_DSP_IRQ_SIZE},},
        { .name = "timer",  .irq = IRQ_IPC,
            .desc = {.base = ADSP_CAVS_1_5_DSP_TIME_BASE, .size = ADSP_CAVS_1_5_DSP_TIME_SIZE},},
        { .name = "mn",
            .desc = {.base = ADSP_CAVS_1_5_DSP_MN_BASE, .size = ADSP_CAVS_1_5_DSP_MN_SIZE},},
        { .name = "l2",
            .desc = {.base = ADSP_CAVS_1_5_DSP_L2_BASE, .size = ADSP_CAVS_1_5_DSP_L2_SIZE},},
        {.name = "ssp0", .irq = IRQ_SSP0,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(0), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp1", .irq = IRQ_SSP1,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(1), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp2", .irq = IRQ_SSP2,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(2), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp3", .irq = IRQ_SSP3,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(3), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp4", .irq = IRQ_SSP4,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(4), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp5", .irq = IRQ_SSP5,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(5), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "dmac0-lp", .irq = IRQ_LPGPDMA0,
           .desc = {.base = ADSP_CAVS_1_5_DSP_LP_GP_DMA_LINK_BASE(0), .size = ADSP_CAVS_1_5_DSP_LP_GP_DMA_LINK_SIZE},},
        {.name = "dmac1-hp", .irq = IRQ_LPGPDMA1,
           .desc = {.base = ADSP_CAVS_1_5_DSP_HP_GP_DMA_LINK_BASE(1), .size = ADSP_CAVS_1_5_DSP_HP_GP_DMA_LINK_SIZE},},
        {.name = "shim",
           .init = &adsp_cavs_shim_init, .ops = &cavs_shim_ops,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SHIM_BASE, .size = ADSP_CAVS_1_5_SHIM_SIZE},},
        { .name = "gtw-lout",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_5_DSP_GTW_LINK_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_5_DSP_GTW_LINK_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-lin",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_5_DSP_GTW_LINK_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_5_DSP_GTW_LINK_IN_STREAM_SIZE * 14},},
        { .name = "gtw-hout",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_5_DSP_GTW_HOST_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_5_DSP_GTW_HOST_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-hin",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_5_DSP_GTW_HOST_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_5_DSP_GTW_HOST_IN_STREAM_SIZE * 14},},
};

static struct adsp_mem_desc cavs_1_5_mem[] = {
    {.name = "l2-sram", .base = ADSP_CAVS_1_5_DSP_SRAM_BASE,
        .size = ADSP_CAVS_1_5_DSP_SRAM_SIZE},
    {.name = "hp-sram", .base = ADSP_CAVS_1_5_DSP_HP_SRAM_BASE,
        .size = ADSP_CAVS_1_5_DSP_HP_SRAM_SIZE, .alias = ADSP_CAVS_1_5_DSP_UNCACHE_BASE},
    {.name = "lp-sram", .base = ADSP_CAVS_1_5_DSP_LP_SRAM_BASE,
        .size = ADSP_CAVS_1_5_DSP_LP_SRAM_SIZE},
    {.name = "imr", .base = ADSP_CAVS_1_5_DSP_IMR_BASE,
        .size = ADSP_CAVS_1_5_DSP_IMR_SIZE},
    {.name = "rom", .base = ADSP_CAVS_DSP_ROM_BASE,
        .size = ADSP_CAVS_DSP_ROM_SIZE},
};

/* hardware memory map for APL */
static const struct adsp_desc cavs_1_5p_dsp_desc = {
    .ia_irq = IRQ_NUM_EXT_IA,
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,

    .imr_boot_ldr_offset = ADSP_CAVS_1_5P_DSP_IMR_MAN_OFFSET,

    .num_mem = ARRAY_SIZE(cavs_1_5_mem),
    .mem_region = cavs_1_5_mem,

    .num_io = ARRAY_SIZE(cavs_1_5_io),
    .io_dev = cavs_1_5_io,

    .mem_zones = {
        [SOF_FW_BLK_TYPE_IMR] = {
            .base = ADSP_CAVS_1_8_DSP_IMR_BASE,
        },
        [SOF_FW_BLK_TYPE_SRAM] = {
            .base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE,
        },
    },

    .ops = &cavs_1_5_ops,
};

/* hardware memory map for SKL, KBL */
static const struct adsp_desc cavs_1_5_dsp_desc = {
    .ia_irq = IRQ_NUM_EXT_IA,
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,

    .imr_boot_ldr_offset = ADSP_CAVS_1_5_DSP_IMR_MAN_OFFSET,
    .file_offset = sizeof(struct fw_image_manifest_v1_5) - sizeof(struct adsp_fw_desc),

    .num_mem = ARRAY_SIZE(cavs_1_5_mem),
    .mem_region = cavs_1_5_mem,

    .num_io = ARRAY_SIZE(cavs_1_5_io),
    .io_dev = cavs_1_5_io,

    .mem_zones = {
        [SOF_FW_BLK_TYPE_IMR] = {
            .base = ADSP_CAVS_1_5_DSP_IMR_BASE,
        },
        [SOF_FW_BLK_TYPE_SRAM] = {
            .base = ADSP_CAVS_1_5_DSP_HP_SRAM_BASE,
        },
     },

    .ops = &cavs_1_5_ops,
};

static void sue_ctrl_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
     cavs_region(0x40) = 0x1;
}

#define L2LMCAP			0x00
#define L2MPAT			0x04

#define HSPGCTL0		0x10
#define HSRMCTL0		0x14
#define HSPGISTS0		0x18

#define HSPGCTL1		0x20
#define HSRMCTL1		0x24
#define HSPGISTS1		0x28

#define LSPGCTL			0x50
#define LSRMCTL			0x54
#define LSPGISTS		0x58

static void cavs1_8_l2m_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    cavs_region(HSPGISTS0) = 0x00ffffff;
    cavs_region(HSPGISTS1) = 0x00ffffff;
}

static uint64_t cavs1_8_l2m_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_shim_read(addr, cavs_region(addr));

    return cavs_region(addr);
}

static void cavs1_8_l2m_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_shim_write(addr, val);

    switch (addr) {
    case HSPGCTL0:
         cavs_region(HSPGISTS0) = val;
         break;
    case HSPGCTL1:
         cavs_region(HSPGISTS1) = val;
         break;
    default:
    break;
    }

    cavs_region(addr) = val;
}

const MemoryRegionOps cavs1_8_l2m_io_ops = {
    .read = cavs1_8_l2m_read,
    .write = cavs1_8_l2m_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static uint64_t cavs_ipc_1_8_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_adsp_dsp_shim_read(addr, cavs_region(addr));

    return cavs_region(addr);
}

/* SHIM IO from ADSP */
static void cavs_ipc_1_8_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct qemu_io_msg_irq irq;

    trace_adsp_dsp_shim_write(addr, val);

    /* special case registers */
    switch (addr) {
    case IPC_DIPCTDR:
        /* host to DSP */
        if (val & IPC_DIPCT_DSPLRST)
            cavs_region(addr) &= ~IPC_DIPCT_DSPLRST;
        break;
    case IPC_DIPCTDA:
        /* DSP to host */
        cavs_region(addr) = val;

        if (val & IPC_DIPCI_DSPRST) {
            trace_adsp_dsp_shim_event("irq: send DONE interrupt to host", val);

            /* send IRQ to parent */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
        }
    break;
    case IPC_DIPCTDD:
    case IPC_DIPCIDD:
    case IPC_DIPCCST:
        cavs_region(addr) = val;
        break;
    case IPC_DIPCIDR:
       /* DSP to host */
        cavs_region(addr) = val;

        if (val & IPC_DIPCI_DSPRST) {
            trace_adsp_dsp_shim_event("irq: send BUSY interrupt to host", val);

            /* send IRQ to parent */
            irq.hdr.type = QEMU_IO_TYPE_IRQ;
            irq.hdr.msg = QEMU_IO_MSG_IRQ;
            irq.hdr.size = sizeof(irq);
            irq.irq = 0;

            qemu_io_send_msg(&irq.hdr);
        }
    break;
   case IPC_DIPCIDA:
            cavs_region(addr) = val & ~(0x1 << 31);
            if (val & IPC_DIPCIE_DONE)
                cavs_region(addr) &= ~IPC_DIPCIE_DONE;
        break;
    case IPC_DIPCCTL8:
             /* assume interrupts are not masked atm */
             cavs_region(addr) = val;
        break;
    default:
        break;
    }
}

const MemoryRegionOps cavs_ipc_v1_8_io_ops = {
    .read = cavs_ipc_1_8_read,
    .write = cavs_ipc_1_8_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* CAVS 1.8 IO devices */
static struct adsp_reg_space cavs_1_8_io[] = {
        { .name = "cap",
            .desc = {.base = ADSP_CAVS_1_8_DSP_CAP_BASE, .size = ADSP_CAVS_1_8_DSP_CAP_SIZE},},
        { .name = "hp-gpdma-shim",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_BASE, .size = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_SIZE},},
        { .name = "idc0",
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc1",
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc2",
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc3",
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(3), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "hostwin0",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(0), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin1",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(1), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin2",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(2), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin3",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(3), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "irq",
            .init = cavs_irq_init, .ops = &cavs_irq_io_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IRQ_BASE, .size = ADSP_CAVS_1_8_DSP_IRQ_SIZE},},
        { .name = "timer",
            .desc = {.base = ADSP_CAVS_1_8_DSP_TIME_BASE, .size = ADSP_CAVS_1_8_DSP_TIME_SIZE},},
        { .name = "mn",
            .desc = {.base = ADSP_CAVS_1_8_DSP_MN_BASE, .size = ADSP_CAVS_1_8_DSP_MN_SIZE},},
        { .name = "l2m",  .init = cavs1_8_l2m_init, .ops = &cavs1_8_l2m_io_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2M_BASE, .size = ADSP_CAVS_1_8_DSP_L2M_SIZE},},
        { .name = "l2c",
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2C_BASE, .size = ADSP_CAVS_1_8_DSP_L2C_SIZE},},
        { .name = "res",
            .desc = {.base = ADSP_CAVS_1_8_DSP_RES_BASE, .size = ADSP_CAVS_1_8_DSP_RES_SIZE},},
        { .name = "cmd",
            .desc = {.base = ADSP_CAVS_1_8_DSP_CMD_BASE, .size = ADSP_CAVS_1_8_DSP_CMD_SIZE},},
        { .name = "dmic",
            .desc = {.base = ADSP_CAVS_1_8_DSP_DMIC_BASE, .size = ADSP_CAVS_1_8_DSP_DMIC_SIZE},},
        { .name = "ipc-dsp",  .ops = &cavs_ipc_v1_8_io_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IPC_HOST_BASE, .size = ADSP_CAVS_1_8_DSP_IPC_HOST_SIZE},},
        { .name = "gtw-lout",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-lin",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_SIZE * 14},},
        { .name = "gtw-hout",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-hin",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_SIZE * 14},},
        { .name = "cl",
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_BASE, .size = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_SIZE},},
        { .name = "lp-gpda-shim",
            .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_SIZE * 4},},
        {.name = "ssp0", .irq = IRQ_SSP0,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp1", .irq = IRQ_SSP1,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp2", .irq = IRQ_SSP2,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp3", .irq = IRQ_SSP3,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(3), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp4", .irq = IRQ_SSP4,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(4), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp5", .irq = IRQ_SSP5,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(5), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        { .name = "dmac0", .irq = IRQ_LPGPDMA,
           .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},},
        { .name = "dmac1", .irq = IRQ_LPGPDMA,
           .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(1), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},},
        { .name = "shim",
           .init = &adsp_cavs_shim_init, .ops = &cavs_shim_ops,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SHIM_BASE, .size = ADSP_CAVS_1_8_DSP_SHIM_SIZE},},
};

static struct adsp_mem_desc cavs_1_8_mem[] = {
    {.name = "l2-sram", .base = ADSP_CAVS_1_8_DSP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_SRAM_SIZE},
    {.name = "hp-sram", .base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_HP_SRAM_SIZE, .alias = ADSP_CAVS_1_5_DSP_UNCACHE_BASE},
    {.name = "lp-sram", .base = ADSP_CAVS_1_8_DSP_LP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_LP_SRAM_SIZE},
    {.name = "imr", .base = ADSP_CAVS_1_8_DSP_IMR_BASE,
        .size = ADSP_CAVS_1_8_DSP_IMR_SIZE},
    {.name = "rom", .base = ADSP_CAVS_DSP_ROM_BASE,
        .size = ADSP_CAVS_DSP_ROM_SIZE},
};

/* CNL */
static const struct adsp_desc cavs_1_8_dsp_desc = {
    .ia_irq = IRQ_NUM_EXT_IA,
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,

    .imr_boot_ldr_offset = ADSP_CAVS_1_8_DSP_IMR_MAN_OFFSET,

    .num_mem = ARRAY_SIZE(cavs_1_8_mem),
    .mem_region = cavs_1_8_mem,

    .num_io = ARRAY_SIZE(cavs_1_8_io),
    .io_dev = cavs_1_8_io,

    .mem_zones = {
        [SOF_FW_BLK_TYPE_IMR] = {
            .base = ADSP_CAVS_1_8_DSP_IMR_BASE,
        },
        [SOF_FW_BLK_TYPE_SRAM] = {
            .base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE,
        },
    },

    .ops = &cavs_1_8_ops,
};

/* Tigerlake */
static struct adsp_mem_desc cavs_1_8_mem_tgl[] = {
    {.name = "l2-sram", .base = ADSP_CAVS_1_8_DSP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_SRAM_SIZE},
    {.name = "hp-sram", .base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_HP_SRAM_SIZE, .alias = ADSP_CAVS_1_5_DSP_UNCACHE_BASE},
    {.name = "lp-sram", .base = ADSP_CAVS_1_8_DSP_LP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_LP_SRAM_SIZE},
    {.name = "imr", .base = ADSP_CAVS_1_8_DSP_IMR_BASE,
        .size = ADSP_CAVS_1_8_DSP_IMR_SIZE},
    {.name = "rom", .base = ADSP_CAVS_TGL_DSP_ROM_BASE,
        .size = ADSP_CAVS_DSP_ROM_SIZE},
};

static const struct adsp_desc cavs_1_8_dsp_tgl_desc = {
    .ia_irq = IRQ_NUM_EXT_IA,
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,

    .imr_boot_ldr_offset = ADSP_CAVS_1_8_DSP_IMR_MAN_OFFSET,

    .num_mem = ARRAY_SIZE(cavs_1_8_mem_tgl),
    .mem_region = cavs_1_8_mem_tgl,

    .num_io = ARRAY_SIZE(cavs_1_8_io),
    .io_dev = cavs_1_8_io,

    .mem_zones = {
        [SOF_FW_BLK_TYPE_IMR] = {
            .base = ADSP_CAVS_1_8_DSP_IMR_BASE,
        },
        [SOF_FW_BLK_TYPE_SRAM] = {
            .base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE,
        },
    },

    .ops = &cavs_1_8_ops,
};

/* Sue creek */
static struct adsp_mem_desc cavs_1_8_sue_mem[] = {
    {.name = "l2-sram", .base = ADSP_CAVS_1_8_DSP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_SRAM_SIZE},
    {.name = "hp-sram", .base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_HP_SRAM_SIZE, .alias = ADSP_CAVS_1_5_DSP_UNCACHE_BASE},
    {.name = "lp-sram", .base = ADSP_CAVS_1_8_DSP_LP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_LP_SRAM_SIZE, .alias = ADSP_CAVS_1_8_DSP_LP_UNCACHE_BASE},
    {.name = "imr", .base = ADSP_CAVS_1_8_DSP_IMR_BASE,
        .size = ADSP_CAVS_1_8_DSP_IMR_SIZE},
    {.name = "rom", .base = ADSP_CAVS_DSP_ROM_BASE,
        .size = ADSP_CAVS_DSP_ROM_SIZE},
    {.name = "spi-xip", .base = ADSP_CAVS_1_8_DSP_SUE_SPIMEM_CACHE_BASE,
        .size = ADSP_CAVS_1_8_DSP_SUE_SPIMEML_SIZE, .alias = ADSP_CAVS_1_8_DSP_SUE_SPIMEM_UNCACHE_BASE},
    {.name = "parallel", .base = ADSP_CAVS_1_8_DSP_SUE_PARMEM_CACHE_BASE,
        .size = ADSP_CAVS_1_8_DSP_SUE_PARMEML_SIZE, .alias = ADSP_CAVS_1_8_DSP_SUE_PARMEM_UNCACHE_BASE},
};

/* CAVS 1.8 IO devices */
static struct adsp_reg_space cavs_1_8_sue_io[] = {
        { .name = "cap",
            .desc = {.base = ADSP_CAVS_1_8_DSP_CAP_BASE, .size = ADSP_CAVS_1_8_DSP_CAP_SIZE},},
        { .name = "hp-gpdma-shim",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_BASE, .size = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_SIZE},},
        { .name = "idc0",
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc1",
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc2",
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc3",
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(3), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "hostwin0",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(0), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin1",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(1), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin2",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(2), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin3",
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(3), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "irq",
            .init = cavs_irq_init, .ops = &cavs_irq_io_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IRQ_BASE, .size = ADSP_CAVS_1_8_DSP_IRQ_SIZE},},
        { .name = "timer",
            .desc = {.base = ADSP_CAVS_1_8_DSP_TIME_BASE, .size = ADSP_CAVS_1_8_DSP_TIME_SIZE},},
        { .name = "mn",
            .desc = {.base = ADSP_CAVS_1_8_DSP_MN_BASE, .size = ADSP_CAVS_1_8_DSP_MN_SIZE},},
        { .name = "l2m",  .init = cavs1_8_l2m_init, .ops = &cavs1_8_l2m_io_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2M_BASE, .size = ADSP_CAVS_1_8_DSP_L2M_SIZE},},
        { .name = "l2c",
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2C_BASE, .size = ADSP_CAVS_1_8_DSP_L2C_SIZE},},
        { .name = "res",
            .desc = {.base = ADSP_CAVS_1_8_DSP_RES_BASE, .size = ADSP_CAVS_1_8_DSP_RES_SIZE},},
        { .name = "cmd",
            .desc = {.base = ADSP_CAVS_1_8_DSP_CMD_BASE, .size = ADSP_CAVS_1_8_DSP_CMD_SIZE},},
        { .name = "dmic",
            .desc = {.base = ADSP_CAVS_1_8_DSP_DMIC_BASE, .size = ADSP_CAVS_1_8_DSP_DMIC_SIZE},},
        { .name = "ipc",  .ops = &cavs_ipc_v1_8_io_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IPC_HOST_BASE, .size = ADSP_CAVS_1_8_DSP_IPC_HOST_SIZE},},
        { .name = "gtw-lout",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-lin",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_SIZE * 14},},
        { .name = "gtw-hout",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-hin",  .init = hda_dma_init_dev, .ops = &hda_dmac_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_SIZE * 14},},
        { .name = "cl",
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_BASE, .size = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_SIZE},},
        { .name = "lp-gpda-shim",
            .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_SIZE * 4},},
        { .name = "shim",
            .init = &adsp_cavs_shim_init, .ops = &cavs_shim_ops,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SHIM_BASE, .size = ADSP_CAVS_1_8_DSP_SHIM_SIZE},},
        { .name = "dmac0",
           .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},},
        { .name = "dmac1",
           .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(1), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},},
        { .name = "spi-slave",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_SPIS_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_SPIS_SIZE},},
        { .name = "i2c",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_I2C_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_I2C_SIZE},},
        { .name = "uart",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_UART_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_UART_SIZE},},
        { .name = "gpio",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_GPIO_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_GPIO_SIZE},},
        { .name = "timer-ext",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_TIMER_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_TIMER_SIZE},},
        { .name = "wdt",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_WDT_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_WDT_SIZE},},
        { .name = "irq-ext",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_IRQ_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_IRQ_SIZE},},
        { .name = "ctrl",  .init = sue_ctrl_init,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_CTRL_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_CTRL_SIZE},},
        { .name = "usb",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_USB_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_USB_SIZE},},
        { .name = "spi-master",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_SPIM_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_SPIM_SIZE},},
        { .name = "mem-ctrl",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_PMEMCTRL_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_PMEMCTRL_SIZE},},
        { .name = "gna",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_GNA_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_GNA_SIZE},},
        {.name = "ssp0",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp1",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp2",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp3",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(3), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp4",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(4), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp5",
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(5), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
};

/* SUE */
static const struct adsp_desc cavs_1_8_sue_dsp_desc = {
    .ext_timer_irq = IRQ_NUM_EXT_TIMER,

    .imr_boot_ldr_offset = ADSP_CAVS_1_8_DSP_IMR_MAN_OFFSET,

    .num_mem = ARRAY_SIZE(cavs_1_8_sue_mem),
    .mem_region = cavs_1_8_sue_mem,

    .num_io = ARRAY_SIZE(cavs_1_8_sue_io),
    .io_dev = cavs_1_8_sue_io,

    .mem_zones = {
        [SOF_FW_BLK_TYPE_IMR] = {
            .base = ADSP_CAVS_1_8_DSP_IMR_BASE,
        },
        [SOF_FW_BLK_TYPE_SRAM] = {
            .base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE,
        },
    },
    .ops = &cavs_1_8_ops,
};

static void apl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_5p_dsp_desc, machine, "bxt", 0, 0, ADSP_CAVS_1_8_DSP_IMR_BASE, 19200);
}

static void xtensa_apl_machine_init(MachineClass *mc)
{
    mc->desc = "Apollolake HiFi3";
    mc->is_default = false;
    mc->init = apl_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_bxt", xtensa_apl_machine_init)

static void skl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_5_dsp_desc, machine, "skl", 1, 0, ADSP_CAVS_1_8_DSP_IMR_BASE, 19200);
}

static void xtensa_skl_machine_init(MachineClass *mc)
{
    mc->desc = "Skylake HiFi3";
    mc->is_default = false;
    mc->init = skl_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_skl", xtensa_skl_machine_init)

static void kbl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_5_dsp_desc, machine, "kbl", 1, 0, ADSP_CAVS_1_8_DSP_IMR_BASE, 19200);
}

static void xtensa_kbl_machine_init(MachineClass *mc)
{
    mc->desc = "Kabylake HiFi3";
    mc->is_default = false;
    mc->init = kbl_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_kbl", xtensa_kbl_machine_init)

static void sue_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_8_sue_dsp_desc, machine, "sue", 0,
        ADSP_CAVS_1_8_DSP_HP_SRAM_BASE, ADSP_CAVS_1_8_DSP_IMR_BASE, 19200);
}

static void xtensa_sue_machine_init(MachineClass *mc)
{
    mc->desc = "Sue HiFi3";
    mc->is_default = false;
    mc->init = sue_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_sue", xtensa_sue_machine_init)

static void cnl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_8_dsp_desc, machine, "cnl", 0, 0, ADSP_CAVS_1_8_DSP_IMR_BASE, 24000);
}

static void xtensa_cnl_machine_init(MachineClass *mc)
{
    mc->desc = "Cannonlake HiFi3";
    mc->is_default = false;
    mc->init = cnl_adsp_init;
    mc->max_cpus = 4;
    mc->default_cpus = 4;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_cnl", xtensa_cnl_machine_init)

static void icl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_8_dsp_desc, machine, "icl", 0, 0, ADSP_CAVS_1_8_DSP_IMR_BASE, 38400);
}

static void xtensa_icl_machine_init(MachineClass *mc)
{
    mc->desc = "Icelake HiFi3";
    mc->is_default = false;
    mc->init = icl_adsp_init;
    mc->max_cpus = 4;
    mc->default_cpus = 4;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_icl", xtensa_icl_machine_init)

static void tgl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_8_dsp_tgl_desc, machine, "tgl", 0, 0, ADSP_CAVS_1_8_DSP_IMR_BASE, 38400);
}

static void xtensa_tgl_machine_init(MachineClass *mc)
{
    mc->desc = "Tigerlake HiFi3";
    mc->is_default = false;
    mc->init = tgl_adsp_init;
    mc->max_cpus = 4;
    mc->default_cpus = 4;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_NOMMU_TYPE;
}

DEFINE_MACHINE("adsp_tgl", xtensa_tgl_machine_init)
