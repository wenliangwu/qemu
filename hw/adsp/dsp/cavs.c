/* Core DSP support for Broxton audio DSP.
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
#include "qemu/timer.h"
#include "sysemu/sysemu.h"
#include "hw/boards.h"
#include "hw/loader.h"
#include "hw/hw.h"
#include "exec/memory.h"
#include "exec/address-spaces.h"
#include "hw/sysbus.h"
#include "qemu/error-report.h"
#include "qemu/io-bridge.h"

#include "hw/audio/adsp-dev.h"
#include "hw/adsp/shim.h"
#include "hw/adsp/log.h"
#include "hw/ssi/ssp.h"
#include "hw/dma/dw-dma.h"
#include "hw/adsp/cavs.h"
#include "mbox.h"
#include "cavs.h"
#include "common.h"
#include "manifest.h"

#define MAX_IMAGE_SIZE (1024 * 1024 *4)

static void adsp_reset(void *opaque)
{
}

static uint64_t io_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    log_read(adsp->log, space, addr, size,
        info->region[addr >> 2]);

    return info->region[addr >> 2];
}

/* SHIM IO from ADSP */
static void io_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    info->region[addr >> 2] = val;

    log_write(adsp->log, space, addr, val, size,
         info->region[addr >> 2]);
}

const MemoryRegionOps cavs_io_ops = {
    .read = io_read,
    .write = io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void adsp_pm_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
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
    case QEMU_IO_TYPE_PM:
        adsp_pm_msg(adsp, msg);
        break;
    case QEMU_IO_TYPE_DMA:
        dw_dma_msg(msg);
        break;
    case QEMU_IO_TYPE_MEM:
    default:
        break;
    }

    return 0;
}

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

    printf("found %d modules\n", hdr->num_module_entries);
    printf("using file offset 0x%x\n", board->file_offset);

    /* copy modules to SRAM */
    for (i = 0; i < hdr->num_module_entries; i++) {

        mod = &desc->module[i];
        printf("checking module %d : %s\n", i, mod->name);

        for (j = 0; j < 3; j++) {

            if (mod->segment[j].flags.r.load == 0)
                continue;

            foffset = mod->segment[j].file_offset;
            ssize = mod->segment[j].flags.r.length * 4096;

            mem = adsp_get_mem_space(adsp, mod->segment[j].v_base_addr);

            if (mem) {

                soffset = mod->segment[j].v_base_addr - mem->base;

                printf(" %s segment %d file offset 0x%lx MEM addr 0x%x offset 0x%lx size 0x%lx\n",
                    mem->name, j, foffset, mod->segment[j].v_base_addr, soffset, ssize);

                /* copy text to SRAM */
                memcpy(mem->ptr + soffset, (void*)base_ptr + foffset, ssize);

            } else {

            printf(" Unmatched segment %d file offset 0x%lx SRAM addr 0x%x offset 0x%lx size 0x%lx\n",
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
                 hdr->preload_page_count * 4096);
    printf("ROM loader: copy %d kernel pages to IMR\n", hdr->preload_page_count);
}

static struct adsp_dev *adsp_init(const struct adsp_desc *board,
    MachineState *machine, const char *name, int copy_modules,
    uint32_t exec_addr, uint32_t imr_addr)
{
    struct adsp_dev *adsp;
    struct adsp_mem_desc *mem;
    void *man_ptr, *desc_ptr;
    int n, skip = 0, size;
    void *rom;

    adsp = g_malloc(sizeof(*adsp));
    adsp->log = log_init(NULL);    /* TODO: add log name to cmd line */
    adsp->desc = board;
    adsp->shm_idx = 0;
    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->cpu_model = machine->cpu_model;
    adsp->kernel_filename = qemu_opt_get(adsp->machine_opts, "kernel");
    adsp->rom_filename = qemu_opt_get(adsp->machine_opts, "rom");

    /* initialise CPU */
    if (!adsp->cpu_model) {
        adsp->cpu_model = XTENSA_DEFAULT_CPU_MODEL;
    }

    for (n = 0; n < smp_cpus; n++) {

        adsp->xtensa[n] = g_malloc(sizeof(struct adsp_xtensa));
        adsp->xtensa[n]->cpu = XTENSA_CPU(cpu_create(machine->cpu_type));

        if (adsp->xtensa[n]->cpu == NULL) {
            error_report("unable to find CPU definition '%s'",
                adsp->cpu_model);
            exit(EXIT_FAILURE);
        }

        adsp->xtensa[n]->env = &adsp->xtensa[n]->cpu->env;
        adsp->xtensa[n]->env->sregs[PRID] = n;

        qemu_register_reset(adsp_reset, adsp->xtensa[n]->cpu);

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
    qemu_io_register_child(name, &bridge_cb, (void*)adsp);

    /* load binary file if one is specified on cmd line otherwise finish */
    if (adsp->kernel_filename == NULL) {
        printf(" ** CAVS Xtensa HiFi3 DSP initialised.\n"
            " ** Waiting for host to load firmware...\n");
        return adsp;
    }

    printf("now loading:\n kernel %s\n ROM %s\n",
        adsp->kernel_filename, adsp->rom_filename);

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
            return NULL;

        printf("copying exec manifest header 0x%x bytes to %s 0x%zx\n",
                size, mem->name, mem->base);
        memcpy(mem->ptr + board->imr_boot_ldr_offset, man_ptr, size);
        goto out;
    }

    /* Search for manifest ID = "$AEM" */
    desc_ptr = man_ptr;
    while (*((uint32_t*)desc_ptr) != 0x314d4124) {
        desc_ptr = desc_ptr + sizeof(uint32_t);
        skip += sizeof(uint32_t);
        if (skip >= size) {
            printf("error: failed to find FW manifest header $AM1\n");
            exit(0);
        }
    }

    printf("Header $AM1 found at offset 0x%x bytes\n", skip);

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

/* CAVS 1.5 IO devices */
static struct adsp_reg_space cavs_1_5_io[] = {
        { .name = "cmd", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_CMD_BASE, .size = ADSP_CAVS_1_5_DSP_CMD_SIZE},},
        { .name = "res", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_RES_BASE, .size = ADSP_CAVS_1_5_DSP_RES_SIZE},},
        { .name = "ipc", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IPC_HOST_BASE, .size = ADSP_CAVS_1_5_DSP_IPC_HOST_SIZE},},
        { .name = "idc0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IPC_DSP_BASE(0), .size = ADSP_CAVS_1_5_DSP_IPC_DSP_SIZE},},
        { .name = "idc1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IPC_DSP_BASE(1), .size = ADSP_CAVS_1_5_DSP_IPC_DSP_SIZE},},
        { .name = "hostwin0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(0), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(1), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(2), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin3", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_HOST_WIN_BASE(3), .size = ADSP_CAVS_1_5_DSP_HOST_WIN_SIZE},},
        { .name = "irq", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_IRQ_BASE, .size = ADSP_CAVS_1_5_DSP_IRQ_SIZE},},
        { .name = "timer", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_TIME_BASE, .size = ADSP_CAVS_1_5_DSP_TIME_SIZE},},
        { .name = "mn", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_MN_BASE, .size = ADSP_CAVS_1_5_DSP_MN_SIZE},},
        { .name = "l2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_5_DSP_L2_BASE, .size = ADSP_CAVS_1_5_DSP_L2_SIZE},},
        {.name = "ssp0", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(0), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp1", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(1), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp2", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(2), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp3", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(3), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp4", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(4), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "ssp5", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SSP_BASE(5), .size = ADSP_CAVS_1_5_DSP_SSP_SIZE},},
        {.name = "dmac0-lp", .reg_count = ARRAY_SIZE(adsp_gp_dma_map), .reg = adsp_gp_dma_map,
           .desc = {.base = ADSP_CAVS_1_5_DSP_LP_GP_DMA_LINK_BASE(0), .size = ADSP_CAVS_1_5_DSP_LP_GP_DMA_LINK_SIZE},},
        {.name = "dmac1-hp", .reg_count = ARRAY_SIZE(adsp_gp_dma_map), .reg = adsp_gp_dma_map,
           .desc = {.base = ADSP_CAVS_1_5_DSP_HP_GP_DMA_LINK_BASE(1), .size = ADSP_CAVS_1_5_DSP_HP_GP_DMA_LINK_SIZE},},
        {.name = "shim", .reg_count = ARRAY_SIZE(adsp_bxt_shim_map), .reg = adsp_bxt_shim_map,
           .desc = {.base = ADSP_CAVS_1_5_DSP_SHIM_BASE, .size = ADSP_CAVS_1_5_SHIM_SIZE},},
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

    .sram_base = ADSP_CAVS_1_5_DSP_HP_SRAM_BASE,
    .imr_base = ADSP_CAVS_1_5_DSP_IMR_BASE,
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

    .sram_base = ADSP_CAVS_1_5_DSP_HP_SRAM_BASE,
    .imr_base = ADSP_CAVS_1_5_DSP_IMR_BASE,
};

static void sue_ctrl_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
     info->region[0x40 >> 2] = 0x1;
}

static void cavs1_8_shim_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    info->region[0x94 >> 2] = 0x00000080;
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
    info->region[HSPGISTS0 >> 2] = 0x00ffffff;
    info->region[HSPGISTS1 >> 2] = 0x00ffffff;
}

static uint64_t cavs1_8_l2m_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    log_read(adsp->log, space, addr, size,
        info->region[addr >> 2]);

    return info->region[addr >> 2];
}

static void cavs1_8_l2m_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    switch (addr) {
    case HSPGCTL0:
         info->region[HSPGISTS0 >> 2] = val;
         break;
    case HSPGCTL1:
         info->region[HSPGISTS1 >> 2] = val;
         break;
    default:
	break;
    }

    info->region[addr >> 2] = val;

    log_write(adsp->log, space, addr, val, size,
         info->region[addr >> 2]);
}

const MemoryRegionOps cavs1_8_l2m_io_ops = {
    .read = cavs1_8_l2m_read,
    .write = cavs1_8_l2m_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

/* CAVS 1.8 IO devices */
static struct adsp_reg_space cavs_1_8_io[] = {
        { .name = "cap", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_CAP_BASE, .size = ADSP_CAVS_1_8_DSP_CAP_SIZE},},
        { .name = "hp-gpdma-shim", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_BASE, .size = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_SIZE},},
        { .name = "idc0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc3", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(3), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "hostwin0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(0), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(1), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(2), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin3", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(3), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "irq", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IRQ_BASE, .size = ADSP_CAVS_1_8_DSP_IRQ_SIZE},},
        { .name = "timer", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_TIME_BASE, .size = ADSP_CAVS_1_8_DSP_TIME_SIZE},},
        { .name = "mn", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_MN_BASE, .size = ADSP_CAVS_1_8_DSP_MN_SIZE},},
        { .name = "l2m", .reg_count = 0, .reg = NULL, .init = cavs1_8_l2m_init, .ops = &cavs1_8_l2m_io_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2M_BASE, .size = ADSP_CAVS_1_8_DSP_L2M_SIZE},},
        { .name = "l2c", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2C_BASE, .size = ADSP_CAVS_1_8_DSP_L2C_SIZE},},
        { .name = "res", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_RES_BASE, .size = ADSP_CAVS_1_8_DSP_RES_SIZE},},
        { .name = "cmd", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_CMD_BASE, .size = ADSP_CAVS_1_8_DSP_CMD_SIZE},},
        { .name = "dmic", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_DMIC_BASE, .size = ADSP_CAVS_1_8_DSP_DMIC_SIZE},},
        { .name = "ipc", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IPC_HOST_BASE, .size = ADSP_CAVS_1_8_DSP_IPC_HOST_SIZE},},
        { .name = "gtw-lout", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-lin", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_SIZE * 14},},
        { .name = "gtw-hout", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-hin", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_SIZE * 14},},
        { .name = "cl", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_BASE, .size = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_SIZE},},
        { .name = "lp-gpda-shim", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_SIZE * 4},},
        {.name = "ssp0", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp1", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp2", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp3", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(3), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp4", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(4), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp5", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(5), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        { .name = "dmac0", .reg_count = ARRAY_SIZE(adsp_gp_dma_map), .reg = adsp_gp_dma_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},},
        { .name = "dmac1", .reg_count = ARRAY_SIZE(adsp_gp_dma_map), .reg = adsp_gp_dma_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(1), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},},
        { .name = "shim", .reg_count = ARRAY_SIZE(adsp_bxt_shim_map), .reg = adsp_bxt_shim_map,
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

    .sram_base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE,
    .imr_base = ADSP_CAVS_1_8_DSP_IMR_BASE,
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
        { .name = "cap", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_CAP_BASE, .size = ADSP_CAVS_1_8_DSP_CAP_SIZE},},
        { .name = "hp-gpdma-shim", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_BASE, .size = ADSP_CAVS_1_8_DSP_HP_GPDMA_SHIM_SIZE},},
        { .name = "idc0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "idc3", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IDC_DSP_BASE(3), .size = ADSP_CAVS_1_8_DSP_IDC_DSP_SIZE},},
        { .name = "hostwin0", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(0), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin1", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(1), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin2", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(2), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "hostwin3", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_HOST_WIN_BASE(3), .size = ADSP_CAVS_1_8_DSP_HOST_WIN_SIZE},},
        { .name = "irq", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IRQ_BASE, .size = ADSP_CAVS_1_8_DSP_IRQ_SIZE},},
        { .name = "timer", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_TIME_BASE, .size = ADSP_CAVS_1_8_DSP_TIME_SIZE},},
        { .name = "mn", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_MN_BASE, .size = ADSP_CAVS_1_8_DSP_MN_SIZE},},
        { .name = "l2m", .reg_count = 0, .reg = NULL, .init = cavs1_8_l2m_init, .ops = &cavs1_8_l2m_io_ops,
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2M_BASE, .size = ADSP_CAVS_1_8_DSP_L2M_SIZE},},
        { .name = "l2c", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_L2C_BASE, .size = ADSP_CAVS_1_8_DSP_L2C_SIZE},},
        { .name = "res", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_RES_BASE, .size = ADSP_CAVS_1_8_DSP_RES_SIZE},},
        { .name = "cmd", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_CMD_BASE, .size = ADSP_CAVS_1_8_DSP_CMD_SIZE},},
        { .name = "dmic", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_DMIC_BASE, .size = ADSP_CAVS_1_8_DSP_DMIC_SIZE},},
        { .name = "ipc", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_IPC_HOST_BASE, .size = ADSP_CAVS_1_8_DSP_IPC_HOST_SIZE},},
        { .name = "gtw-lout", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-lin", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_LINK_IN_STREAM_SIZE * 14},},
        { .name = "gtw-hout", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_OUT_STREAM_SIZE * 14},},
        { .name = "gtw-hin", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_BASE(0), .size = ADSP_CAVS_1_8_DSP_GTW_HOST_IN_STREAM_SIZE * 14},},
        { .name = "cl", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_BASE, .size = ADSP_CAVS_1_8_DSP_GTW_CODE_LDR_SIZE},},
        { .name = "lp-gpda-shim", .reg_count = 0, .reg = NULL,
            .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GPDMA_SHIM_SIZE * 4},},
        { .name = "shim", .reg_count = ARRAY_SIZE(adsp_bxt_shim_map), .reg = adsp_bxt_shim_map, .init = cavs1_8_shim_init,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SHIM_BASE, .size = ADSP_CAVS_1_8_DSP_SHIM_SIZE},},
        { .name = "dmac0", .reg_count = ARRAY_SIZE(adsp_gp_dma_map), .reg = adsp_gp_dma_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(0), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},},
        { .name = "dmac1", .reg_count = ARRAY_SIZE(adsp_gp_dma_map), .reg = adsp_gp_dma_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_BASE(1), .size = ADSP_CAVS_1_8_DSP_LP_GP_DMA_LINK_SIZE},},
        { .name = "spi-slave", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_SPIS_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_SPIS_SIZE},},
        { .name = "i2c", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_I2C_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_I2C_SIZE},},
        { .name = "uart", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_UART_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_UART_SIZE},},
        { .name = "gpio", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_GPIO_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_GPIO_SIZE},},
        { .name = "timer-ext", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_TIMER_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_TIMER_SIZE},},
        { .name = "wdt", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_WDT_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_WDT_SIZE},},
        { .name = "irq-ext", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_IRQ_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_IRQ_SIZE},},
        { .name = "ctrl", .reg_count = 0, .reg = NULL, .init = sue_ctrl_init,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_CTRL_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_CTRL_SIZE},},
        { .name = "usb", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_USB_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_USB_SIZE},},
        { .name = "spi-master", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_SPIM_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_SPIM_SIZE},},
        { .name = "mem-ctrl", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_PMEMCTRL_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_PMEMCTRL_SIZE},},
        { .name = "gna", .reg_count = 0, .reg = NULL,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SUE_GNA_BASE, .size = ADSP_CAVS_1_8_DSP_SUE_GNA_SIZE},},
        {.name = "ssp0", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(0), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp1", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(1), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp2", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(2), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp3", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(3), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp4", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
           .desc = {.base = ADSP_CAVS_1_8_DSP_SSP_BASE(4), .size = ADSP_CAVS_1_8_DSP_SSP_SIZE},},
        {.name = "ssp5", .reg_count = ARRAY_SIZE(adsp_ssp_map), .reg = adsp_ssp_map,
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

    .sram_base = ADSP_CAVS_1_8_DSP_HP_SRAM_BASE,
    .imr_base = ADSP_CAVS_1_8_DSP_IMR_BASE,
};

static void bxt_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_5p_dsp_desc, machine, "bxt", 0, 0, ADSP_CAVS_1_8_DSP_IMR_BASE);
}

static void xtensa_bxt_machine_init(MachineClass *mc)
{
    mc->desc = "Broxton HiFi3";
    mc->is_default = true;
    mc->init = bxt_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_bxt", xtensa_bxt_machine_init)

static void skl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_5_dsp_desc, machine, "skl", 1, 0, ADSP_CAVS_1_8_DSP_IMR_BASE);
}

static void xtensa_skl_machine_init(MachineClass *mc)
{
    mc->desc = "Skylake HiFi3";
    mc->is_default = true;
    mc->init = skl_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_skl", xtensa_skl_machine_init)

static void kbl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_5_dsp_desc, machine, "kbl", 1, 0, ADSP_CAVS_1_8_DSP_IMR_BASE);
}

static void xtensa_kbl_machine_init(MachineClass *mc)
{
    mc->desc = "Kabylake HiFi3";
    mc->is_default = true;
    mc->init = kbl_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_kbl", xtensa_kbl_machine_init)

static void sue_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_8_sue_dsp_desc, machine, "sue", 0,
        ADSP_CAVS_1_8_DSP_HP_SRAM_BASE, ADSP_CAVS_1_8_DSP_IMR_BASE);
}

static void xtensa_sue_machine_init(MachineClass *mc)
{
    mc->desc = "Sue HiFi3";
    mc->is_default = true;
    mc->init = sue_adsp_init;
    mc->max_cpus = 2;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_sue", xtensa_sue_machine_init)

static void cnl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_8_dsp_desc, machine, "cnl", 0, 0, ADSP_CAVS_1_8_DSP_IMR_BASE);
}

static void xtensa_cnl_machine_init(MachineClass *mc)
{
    mc->desc = "Cannonlake HiFi3";
    mc->is_default = true;
    mc->init = cnl_adsp_init;
    mc->max_cpus = 4;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_cnl", xtensa_cnl_machine_init)

static void icl_adsp_init(MachineState *machine)
{
    adsp_init(&cavs_1_8_dsp_desc, machine, "icl", 0, 0, ADSP_CAVS_1_8_DSP_IMR_BASE);
}

static void xtensa_icl_machine_init(MachineClass *mc)
{
    mc->desc = "Icelake HiFi3";
    mc->is_default = true;
    mc->init = icl_adsp_init;
    mc->max_cpus = 4;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_icl", xtensa_icl_machine_init)
