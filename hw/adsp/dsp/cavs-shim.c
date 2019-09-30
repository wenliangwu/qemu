/* Core DSP SHIM support for Broxton audio DSP.
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

#include "qemu/io-bridge.h"
#include "hw/audio/adsp-dev.h"
#include "hw/adsp/shim.h"
#include "hw/adsp/log.h"
#include "hw/adsp/cavs.h"
#include "cavs.h"
#include "common.h"

static uint64_t ns2ticks(uint64_t ns, uint64_t clk_kHz)
{
    return ns * clk_kHz * 1000 / ( 1000 * 1000 * 1000 );
}

static uint64_t ticks2ns(uint64_t ticks, uint64_t clk_kHz)
{
    return ticks * 1000 * 1000 / clk_kHz;
}

static uint64_t cavs_set_time(struct adsp_dev *adsp, struct adsp_io_info *info)
{
	uint64_t time = (qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - adsp->timer[0].start);
	uint64_t ticks = ns2ticks(time, adsp->timer[0].clk_kHz);

	info->region[(SHIM_DSPWC + 4) >> 2] = (uint32_t)(ticks >> 32);
	info->region[(SHIM_DSPWC + 0) >> 2] = (uint32_t)(ticks & 0xffffffff);

	return time;
}

static void rearm_ext_timer0(struct adsp_dev *adsp,struct adsp_io_info *info)
{
    uint64_t wake = ((uint64_t)(info->region[(SHIM_DSPWCTT0C + 4)>> 2]) << 32) |
		info->region[(SHIM_DSPWCTT0C + 0)>> 2];

    cavs_set_time(adsp, info);

    /* never wakes ups if wake == 0 */
    if (wake == 0)
        wake = 10000000;

    timer_mod(adsp->timer[0].timer,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + wake * 100);
}

static void rearm_ext_timer1(struct adsp_dev *adsp,struct adsp_io_info *info)
{
     uint64_t wake = ((uint64_t)(info->region[SHIM_DSPWCTT1C >> 2]) << 32) | 
		info->region[(SHIM_DSPWCTT1C + 4)>> 2];

    cavs_set_time(adsp, info);

    /* never wakes ups if wake == 0 */
    if (wake == 0)
        wake = 10000000;

    timer_mod(adsp->timer[1].timer,
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + wake * 100);
}

void cavs_ext_timer_cb0(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

    /* clear T0A */
    info->region[SHIM_DSPWCTTCS >> 2] &= ~SHIM_DSPWCTTCS_T0A;
    /* set T0T */
    info->region[SHIM_DSPWCTTCS >> 2] |= SHIM_DSPWCTTCS_T0T;

    /* Interrupt may be generated if IL2MDx.FCT0 bit is set. */
    cavs_irq_set(adsp->timer[0].info, IRQ_DWCT0, 0);
}

void cavs_ext_timer_cb1(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;

     /* clear T0A */
    info->region[SHIM_DSPWCTTCS >> 2] &= ~SHIM_DSPWCTTCS_T1A;
    /* set T0T */
    info->region[SHIM_DSPWCTTCS >> 2] |= SHIM_DSPWCTTCS_T1T;

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
    struct adsp_reg_space *space = info->space;

    switch (addr) {
    case SHIM_DSPWC:
        cavs_set_time(adsp, info);
    case SHIM_DSPWC + 4:
        break;
    default:
        break;
    }

    log_read(adsp->log, space, addr, size,
        info->region[addr >> 2]);

    return info->region[addr >> 2];
}

/* SHIM IO from ADSP */
static void shim_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    log_write(adsp->log, space, addr, val, size,
        info->region[addr >> 2]);

    /* special case registers */
    switch (addr) {
    case SHIM_DSPWC:
        break;
    case SHIM_DSPWCTTCS:
        if ((val & SHIM_DSPWCTTCS_T0A) &&
            !(info->region[addr >> 2] & SHIM_DSPWCTTCS_T0A))
            rearm_ext_timer0(adsp, info);
        if ((val & SHIM_DSPWCTTCS_T1A) &&
            !(info->region[addr >> 2] & SHIM_DSPWCTTCS_T1A))
            rearm_ext_timer1(adsp, info);
        info->region[addr >> 2] = val;
        break;
    case SHIM_DSPWCTT0C:
        info->region[addr >> 2] = val;
        break;
    case SHIM_DSPWCTT0C + 4:
        info->region[addr >> 2] = val;
        break;
    case SHIM_DSPWCTT1C:
        info->region[addr >> 2] = val;
    case SHIM_DSPWCTT1C + 4:
        info->region[addr >> 2] = val;
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

            log_text(adsp->log, LOG_CPU_RESET, "cpu: reset\n");

            cpu_reset(CPU(adsp->xtensa[0]->cpu));
            //vm_stop(RUN_STATE_SHUTDOWN); TODO: fix, causes hang
            adsp->in_reset = 1;

        } else if (adsp->in_reset && !(m->val & SHIM_CSR_STALL)) {

            log_text(adsp->log, LOG_CPU_RESET, "cpu: running\n");

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

            log_text(adsp->log, LOG_CPU_RESET, "cpu: reset\n");

            cpu_reset(CPU(adsp->xtensa[0]->cpu));
            //vm_stop(RUN_STATE_SHUTDOWN); TODO: fix, causes hang
            adsp->in_reset = 1;

        } else if (adsp->in_reset && !(m->val & SHIM_CSR_STALL)) {

            log_text(adsp->log, LOG_CPU_RESET, "cpu: running\n");

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

void adsp_cavs_shim_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
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
        fprintf(stderr, "unknown register msg %d\n", msg->msg);
        break;
    }
}

const MemoryRegionOps cavs_shim_ops = {
    .read = shim_read,
    .write = shim_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void adsp_cavs_shim_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    shim_reset(info);
    adsp->shim = info;
    info->region[0x94 >> 2] = 0x00000080;
    adsp->timer[0].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb0, info);
    adsp->timer[0].clk_kHz = adsp->clk_kHz;
    adsp->timer[1].timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, &cavs_ext_timer_cb1, info);
    adsp->timer[1].clk_kHz = adsp->clk_kHz;
    adsp->timer[0].start = adsp->timer[1].start = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}
