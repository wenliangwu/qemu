/* Virtualization support for SSP.
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
#include "qemu-common.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"

#include "qemu/io-bridge.h"
#include "hw/adsp/hw.h"
#include "hw/adsp/shim.h"
#include "hw/ssi/ssp.h"

#include "trace.h"

#define ssp_region(raddr) info->region[raddr]

static uint64_t ssp_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;

    trace_ssp_read(addr, ssp_region(addr));

    return ssp_region(addr);
}

static void ssp_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    uint32_t set, clear;

    trace_ssp_write(addr, val);

    switch (addr) {
    case SSCR1:
        set = val & ~ssp_region(addr);
        clear = ~val & ssp_region(addr);

        ssp_region(addr) = val;

        /* playback has been enabled */
        if (set & SSCR1_TSRE)
           trace_ssp_event("playback started");

        /* playback has finished */
        if (clear & SSCR1_TSRE)
            trace_ssp_event("playback stopped");

        /* open file if capture has been enabled */
        if (set & SSCR1_RSRE)
            trace_ssp_event("capture started");

        /* close file if capture has finished */
        if (clear & SSCR1_RSRE)
            trace_ssp_event("capture stooped");
        break;
    default:
        ssp_region(addr) = val;
        break;
    }
}

const MemoryRegionOps ssp_ops = {
    .read = ssp_read,
    .write = ssp_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

void adsp_ssp_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    struct adsp_reg_space *space = info->space;

    memset(info->region, 0, space->desc.size);
}
