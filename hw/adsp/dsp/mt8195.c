/* Core DSP support for mt8195 audio DSP.
 *
 * Copyright(c) 2021 Mediatek
 *
 * Author: Trevor Wu <trevor.wu@mediatek.com>
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
#include "migration/vmstate.h"
#include "qemu/main-loop.h"
#include "sysemu/runstate.h"
#include "sysemu/reset.h"

#include "hw/audio/adsp-dev.h"
#include "hw/adsp/log.h"
#include "hw/adsp/mt8195.h"
#include "hw/adsp/dsp/mbox.h"
#include "mt8195.h"
#include "common.h"
#include "hw/adsp/fw.h"

static void adsp_reset(void *opaque)
{

}

static void adsp_pm_msg(struct adsp_dev *adsp, struct qemu_io_msg *msg)
{
}

static int bridge_cb(void *data, struct qemu_io_msg *msg)
{
    struct adsp_dev *adsp = (struct adsp_dev *)data;

    log_text(adsp->log, LOG_MSGQ,
            "msg: id %d msg %d size %d type %d\n",
            msg->id, msg->msg, msg->size, msg->type);

    switch (msg->type) {
    case QEMU_IO_TYPE_REG:
        break;
    case QEMU_IO_TYPE_IRQ:
        break;
    case QEMU_IO_TYPE_PM:
        adsp_pm_msg(adsp, msg);
        break;
    case QEMU_IO_TYPE_DMA:
        break;
    case QEMU_IO_TYPE_MEM:
    default:
        break;
    }

    return 0;
}

static struct adsp_dev *adsp_init(const struct adsp_desc *board,
    MachineState *machine, const char *name)
{
    struct adsp_dev *adsp;
    uint8_t *ldata;
    size_t lsize;
    int n;

    adsp = g_malloc(sizeof(*adsp));
    adsp->log = log_init(NULL, NULL);    /* TODO: add log name to cmd line */
    adsp->desc = board;
    adsp->shm_idx = 0;
    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->kernel_filename = qemu_opt_get(adsp->machine_opts, "kernel");

    /* initialise CPU */
    if (!adsp->cpu_model) {
        adsp->cpu_model = XTENSA_DEFAULT_CPU_MODEL;
    }

    for (n = 0; n < machine->smp.cpus; n++) {

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
    adsp_create_io_devices(adsp, NULL);

    /* reset all devices to init state */
    qemu_devices_reset();

    /* initialise bridge to x86 host driver */
    qemu_io_register_child(name, &bridge_cb, (void *)adsp);

    /* load binary file if one is specified on cmd line otherwise finish */
    if (adsp->kernel_filename == NULL) {
        printf(" ** MT8195 HiFi4 initialised.\n"
            " ** Waiting for host to load firmware...\n");
        return adsp;
    }
    /* load the binary image and copy to IRAM */
    ldata = g_malloc(ADSP_MT8195_DSP_DRAM0_SIZE + ADSP_MT8195_DSP_SRAM_SIZE);
    lsize = load_image_size(adsp->kernel_filename, ldata,
        ADSP_MT8195_DSP_DRAM0_SIZE + ADSP_MT8195_DSP_SRAM_SIZE);

    adsp_load_modules(adsp, ldata, lsize);

    return adsp;
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

/* MBOX IO from ADSP */
static void io_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    info->region[addr >> 2] = val;

    /* omit 0 writes as it fills mbox log */
    if (val == 0) {
        return;
    }

    log_write(adsp->log, space, addr, val, size,
         info->region[addr >> 2]);
}

static const MemoryRegionOps mbox_io_ops = {
    .read = io_read,
    .write = io_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

const struct adsp_reg_desc adsp_mt8195_dspcfg_map[] = {
    {.name = "dspcfg", .enable = LOG_DSPCFG,
        .offset = 0x00000000, .size = ADSP_MT8195_DSP_REG_SIZE},
};

static void dspcfg_reset(void *opaque)
{
    struct adsp_io_info *info = opaque;
    struct adsp_reg_space *space = info->space;
    memset(info->region, 0, space->desc.size);
}

/* dspcfg IO from ADSP */
static uint64_t dspcfg_read(void *opaque, hwaddr addr,
        unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    log_read(adsp->log, space, addr, size,
        info->region[addr >> 2]);

    return info->region[addr >> 2];
}

static void dspcfg_write(void *opaque, hwaddr addr,
        uint64_t val, unsigned size)
{
    struct adsp_io_info *info = opaque;
    struct adsp_dev *adsp = info->adsp;
    struct adsp_reg_space *space = info->space;

    log_write(adsp->log, space, addr, val, size,
        info->region[addr >> 2]);

    /* set value via SHM */
    info->region[addr >> 2] = val;
}

const MemoryRegionOps mt8195_dspcfg_io_ops = {
    .read = dspcfg_read,
    .write = dspcfg_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void adsp_mt8195_dspcfg_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info)
{
    dspcfg_reset(info);
}

static struct adsp_mem_desc mt8195_mem[] = {
    {.name = "iram", .base = ADSP_MT8195_DSP_SRAM_BASE,
        .size = ADSP_MT8195_DSP_SRAM_SIZE},
    {.name = "sram", .base = ADSP_MT8195_DSP_DRAM0_BASE,
        .size = ADSP_MT8195_DSP_DRAM0_SIZE},
};

static struct adsp_reg_space mt8195_io[] = {
    /* mbox map is the same as imx8, use it directly*/
    { .name = "mbox", .reg_count = ARRAY_SIZE(adsp_imx8_mbox_map),
        .reg = adsp_imx8_mbox_map, .init = &adsp_mbox_init, .ops = &mbox_io_ops,
        .desc = {.base = ADSP_MT8195_DSP_MAILBOX_BASE,
        .size = ADSP_MT8195_DSP_MAILBOX_SIZE},},
    { .name = "mb0", .reg_count = ARRAY_SIZE(adsp_mt8195_mb_map),
        .reg = adsp_mt8195_mb_map,
        .init = &adsp_mt8195_mb_init, .ops = &mt8195_mb_ops,
        .desc = {.base = MTK_DSP_MBOX0_REG_BASE,
        .size = MTK_DSP_MBOX0_REG_SIZE},},
    { .name = "mb1", .reg_count = ARRAY_SIZE(adsp_mt8195_mb_map),
        .reg = adsp_mt8195_mb_map,
        .init = &adsp_mt8195_mb_init, .ops = &mt8195_mb_ops,
        .desc = {.base = MTK_DSP_MBOX1_REG_BASE,
        .size = MTK_DSP_MBOX1_REG_SIZE},},
    { .name = "mb2", .reg_count = ARRAY_SIZE(adsp_mt8195_mb_map),
        .reg = adsp_mt8195_mb_map,
        .init = &adsp_mt8195_mb_init, .ops = &mt8195_mb_ops,
        .desc = {.base = MTK_DSP_MBOX2_REG_BASE,
        .size = MTK_DSP_MBOX2_REG_SIZE},},
    { .name = "dspcfg", .reg_count = ARRAY_SIZE(adsp_mt8195_dspcfg_map),
        .reg = adsp_mt8195_dspcfg_map,
        .init = &adsp_mt8195_dspcfg_init, .ops = &mt8195_dspcfg_io_ops,
        .desc = {.base = ADSP_MT8195_DSP_REG_BASE,
        .size = ADSP_MT8195_DSP_REG_SIZE},},
};

/* hardware memory map */
static const struct adsp_desc mt8195_dsp_desc = {
    .num_mem = ARRAY_SIZE(mt8195_mem),
    .mem_region = mt8195_mem,
    .num_io = ARRAY_SIZE(mt8195_io),
    .io_dev = mt8195_io,

    .mem_zones = {
                [SOF_FW_BLK_TYPE_IRAM] = {
                        .base = ADSP_MT8195_DSP_SRAM_BASE,
                        .size = ADSP_MT8195_DSP_SRAM_SIZE,
                        .host_offset = 0,
                },
                [SOF_FW_BLK_TYPE_SRAM] = {
                        .base = ADSP_MT8195_DSP_DRAM0_BASE,
                        .size = ADSP_MT8195_DSP_DRAM0_SIZE,
                        .host_offset = 0,
                },
        },
};

static void mt8195_adsp_init(MachineState *machine)
{
    adsp_init(&mt8195_dsp_desc, machine, "mt8195");
}

static void xtensa_mt8195_machine_init(MachineClass *mc)
{
    mc->desc = "MT8195 HiFi4";
    mc->is_default = true;
    mc->init = mt8195_adsp_init;
    mc->max_cpus = 1;
    mc->default_cpu_type = XTENSA_DEFAULT_CPU_TYPE;
}

DEFINE_MACHINE("adsp_mt8195", xtensa_mt8195_machine_init)
