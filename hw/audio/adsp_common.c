/* Core common support for audio DSP.
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
#include "migration/vmstate.h"

#define ENABLE_SHM
#ifdef ENABLE_SHM
#include "qemu/io-bridge.h"
#endif

#include "hw/audio/adsp-dev.h"
#include "hw/audio/adsp-host.h"
#include "hw/adsp/shim.h"

static int create_io_devices(const struct adsp_desc *board,
                              MemoryRegion *parent, void *adsp, int idx,
                              const MemoryRegionOps *ops)
{
    struct adsp_io_info *info;
    struct adsp_reg_space *space;
#ifdef ENABLE_SHM
    char shm_name[32];
    int err;
#endif
    int i;

    for (i = 0; i < board->num_io; i++) {

        info = g_malloc(sizeof(*info));
        space = &board->io_dev[i];
        idx++;
#ifdef ENABLE_SHM
        sprintf(shm_name, "%s-io", space->name);
        err = qemu_io_register_shm(shm_name, idx,
                space->desc.size, &space->desc.ptr);
        if (err < 0)
            fprintf(stderr, "error: cant alloc IO %s:%d SHM %d\n", shm_name,
                    idx,err);
#endif
        info->region = space->desc.ptr;
        info->adsp = adsp;
        info->io_dev = i;
        info->space = space;

        if (space->init)
            space->init(adsp, parent, info);

        memory_region_init_io(&info->io, NULL,
                              space->ops ? space->ops : ops, info,
                              board->io_dev[i].name, space->desc.size);
        memory_region_add_subregion(parent, space->desc.base, &info->io);
    }

    return idx;
}

static int create_memory_regions(const struct adsp_desc *board,
                                  MemoryRegion *parent, void *adsp, int idx)
{
    MemoryRegion *region;
    struct adsp_mem_desc *desc;
    char name[32];
#ifdef ENABLE_SHM
    int err;
#endif
    int i;

    for (i = 0; i < board->num_mem; i++) {

        desc = &board->mem_region[i];
        idx++;
#ifdef ENABLE_SHM
        /* shared via SHM (not shared on real HW) */
        sprintf(name, "%s-mem", desc->name);
        err = qemu_io_register_shm(name, idx,
            desc->size, &desc->ptr);
        if (err < 0)
            fprintf(stderr, "error: cant alloc %s:%d SHM %d\n", name,
                    idx,err);
#endif
        region = g_malloc(sizeof(*region));
        memory_region_init_ram_ptr(region, NULL, desc->name,
            desc->size, desc->ptr);
        vmstate_register_ram_global(region);
        memory_region_add_subregion(parent, desc->base, region);

        if (desc->alias) {
            sprintf(name, "%s-mem-alias", desc->name);

            region = g_malloc(sizeof(*region));
            memory_region_init_ram_ptr(region, NULL, name,
                                       desc->size, desc->ptr);
            vmstate_register_ram_global(region);
            memory_region_add_subregion(parent, desc->alias, region);
         }
    }

    return idx;
}

void adsp_create_io_devices(struct adsp_dev *adsp, const MemoryRegionOps *ops)
{
    const struct adsp_desc *board = adsp->desc;
    MemoryRegion *parent = adsp->system_memory;

    adsp->shm_idx = create_io_devices(board, parent, adsp, adsp->shm_idx, ops);
}

void adsp_create_memory_regions(struct adsp_dev *adsp)
{
    const struct adsp_desc *board = adsp->desc;
    MemoryRegion *parent = adsp->system_memory;

    adsp->shm_idx = create_memory_regions(board, parent, adsp, adsp->shm_idx);
}

void adsp_create_host_io_devices(struct adsp_host *adsp,
                                 const MemoryRegionOps *ops)
{
    const struct adsp_desc *board = adsp->desc;
    MemoryRegion *parent = adsp->system_memory;

    adsp->shm_idx = create_io_devices(board, parent, adsp, adsp->shm_idx, ops);
}

void adsp_create_host_memory_regions(struct adsp_host *adsp)
{
    const struct adsp_desc *board = adsp->desc;
    MemoryRegion *parent = adsp->system_memory;

    adsp->shm_idx = create_memory_regions(board, parent, adsp, adsp->shm_idx);
}

struct adsp_mem_desc *adsp_get_mem_space(struct adsp_dev *adsp, hwaddr addr)
{
    const struct adsp_desc *board = adsp->desc;
    struct adsp_mem_desc *desc = NULL;
    int i;

    for (i = 0; i < board->num_mem; i++) {

        desc = &board->mem_region[i];

        if (addr >= desc->base &&
            addr < desc->base + desc->size)
                return desc;

        if (!desc->alias)
            continue;

        if (addr >= desc->alias &&
            addr < desc->alias + desc->size)
                return desc;
    }

    return NULL;
}

struct adsp_reg_space *adsp_get_io_space(struct adsp_dev *adsp, hwaddr addr)
{
    const struct adsp_desc *board = adsp->desc;
    struct adsp_reg_space *space = NULL;
    int i;

    for (i = 0; i < board->num_io; i++) {

        space = &board->io_dev[i];

        if (addr >= space->desc.base &&
            addr < space->desc.base + space->desc.size)
                return space;
    }

    return NULL;
}
