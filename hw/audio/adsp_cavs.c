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
#include "hw/pci/pci.h"
#include "hw/pci/msi.h"
#include "hw/audio/adsp-host.h"
#include "hw/adsp/cavs.h"
#include "hw/adsp/shim.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/audio/soundhw.h"
#include "hw/audio/intel-hda.h"
#include "hw/audio/intel-hda-defs.h"
#include "migration/vmstate.h"
#include "qemu/error-report.h"
#include "qemu/io-bridge.h"

#define TYPE_INTEL_HDA_GENERIC "intel-hda-generic"

DECLARE_INSTANCE_CHECKER(IntelHDAState, INTEL_HDA,
                         TYPE_INTEL_HDA_GENERIC);

#define cavs_region(raddr) info->region[raddr >> 2]

static void intel_hda_set_adspcs(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{
    uint32_t pwr = (d->adspcs >> 16) & 0xff;

    d->adspcs &= 0x00ffffff;
    d->adspcs |= (pwr << 24);

    /* core 0 powered ON ? */
    if ((((old >> 16) & 1) == 0) && (((d->adspcs >> 16) & 1) == 1)) {
        /* reset ROM state */
        d->hipcie = HDA_DSP_REG_HIPCIE_DONE;
        d->fw_boot_count = 0;
        d->romsts = HDA_DSP_ROM_STATUS_INIT;
    }
}

static void intel_hda_set_adspic(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{

}

static void intel_hda_get_adspis(IntelHDAState *d, const IntelHDAReg *reg)
{

}

static void intel_hda_set_adspic2(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{

}

static void intel_hda_get_adspis2(IntelHDAState *d, const IntelHDAReg *reg)
{

}

static void intel_hda_set_hipct(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{

}

static void intel_hda_set_hipcte(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{

}

static void intel_hda_set_hipci(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{

}

static void intel_hda_set_hipcie(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{

}

static void intel_hda_set_hipcctl(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old)
{

}

static void intel_hda_get_rom_status(IntelHDAState *d, const IntelHDAReg *reg)
{
    if (d->fw_boot_count++ > 10)
        d->romsts = HDA_DSP_ROM_FW_ENTERED;
}

static uint32_t *intel_hda_reg_addr(IntelHDAState *d, const IntelHDAReg *reg)
{
    uint8_t *addr = (void*)d;

    addr += reg->offset;
    return (uint32_t*)addr;
}

static void intel_adsp_reg_write(IntelHDAState *d, const IntelHDAReg *reg, uint32_t val,
                                uint32_t wmask)
{
    uint32_t *addr;
    uint32_t old;

    if (!reg) {
        return;
    }
    if (!reg->wmask) {
        qemu_log_mask(LOG_GUEST_ERROR, "intel-hda: write to r/o reg %s\n",
                      reg->name);
        return;
    }

    if (d->debug) {
        time_t now = time(NULL);
        if (d->last_write && d->last_reg == reg && d->last_val == val) {
            d->repeat_count++;
            if (d->last_sec != now) {
                dprint(d, 2, "previous register op repeated %d times\n", d->repeat_count);
                d->last_sec = now;
                d->repeat_count = 0;
            }
        } else {
            if (d->repeat_count) {
                dprint(d, 2, "previous register op repeated %d times\n", d->repeat_count);
            }
            dprint(d, 2, "write %-16s: 0x%x (%x)\n", reg->name, val, wmask);
            d->last_write = 1;
            d->last_reg   = reg;
            d->last_val   = val;
            d->last_sec   = now;
            d->repeat_count = 0;
        }
    }

    assert(reg->offset != 0);

    addr = intel_hda_reg_addr(d, reg);
    old = *addr;

    if (reg->shift) {
        val <<= reg->shift;
        wmask <<= reg->shift;
    }
    wmask &= reg->wmask;
    *addr &= ~wmask;
    *addr |= wmask & val;
    *addr &= ~(val & reg->wclear);

    if (reg->whandler) {
        reg->whandler(d, reg, old);
    }
}

static uint32_t intel_hda_reg_read(IntelHDAState *d, const IntelHDAReg *reg,
                                   uint32_t rmask)
{
    uint32_t *addr, ret;

    if (!reg) {
        return 0;
    }

    if (reg->rhandler) {
        reg->rhandler(d, reg);
    }

    if (reg->offset == 0) {
        /* constant read-only register */
        ret = reg->reset;
    } else {
        addr = intel_hda_reg_addr(d, reg);
        ret = *addr;
        if (reg->shift) {
            ret >>= reg->shift;
        }
        ret &= rmask;
    }
    if (d->debug) {
        time_t now = time(NULL);
        if (!d->last_write && d->last_reg == reg && d->last_val == ret) {
            d->repeat_count++;
            if (d->last_sec != now) {
                dprint(d, 2, "previous register op repeated %d times\n", d->repeat_count);
                d->last_sec = now;
                d->repeat_count = 0;
            }
        } else {
            if (d->repeat_count) {
                dprint(d, 2, "previous register op repeated %d times\n", d->repeat_count);
            }
            dprint(d, 2, "read  %-16s: 0x%x (%x)\n", reg->name, ret, rmask);
            d->last_write = 0;
            d->last_reg   = reg;
            d->last_val   = ret;
            d->last_sec   = now;
            d->repeat_count = 0;
        }
    }

    return ret;
}

#define CL_REG(_n, _o) (HDA_ADSP_LOADER_BASE + (_o))

/* CAVS v1.5 */
static const struct IntelHDAReg regtabdsp[] = {
    /* global */
    [ ICH6_REG_GCAP ] = {
        .name     = "GCAP",
        .size     = 2,
        .reset    = 0x8801,
    },
    [ ICH6_REG_VMIN ] = {
        .name     = "VMIN",
        .size     = 1,
    },
    [ ICH6_REG_VMAJ ] = {
        .name     = "VMAJ",
        .size     = 1,
        .reset    = 1,
    },
    [ ICH6_REG_OUTPAY ] = {
        .name     = "OUTPAY",
        .size     = 2,
        .reset    = 0x3c,
        .offset   = offsetof(IntelHDAState, outpay),
    },
    [ ICH6_REG_INPAY ] = {
        .name     = "INPAY",
        .size     = 2,
        .reset    = 0x1d,
        .offset   = offsetof(IntelHDAState, inpay),
    },
    [ ICH6_REG_GCTL ] = {
        .name     = "GCTL",
        .size     = 4,
        .wmask    = 0x0103,
        .offset   = offsetof(IntelHDAState, g_ctl),
        .whandler = intel_hda_set_g_ctl,
    },
    [ ICH6_REG_WAKEEN ] = {
        .name     = "WAKEEN",
        .size     = 2,
        .wmask    = 0x7fff,
        .offset   = offsetof(IntelHDAState, wake_en),
        .whandler = intel_hda_set_wake_en,
    },
    [ ICH6_REG_STATESTS ] = {
        .name     = "STATESTS",
        .size     = 2,
        .wmask    = 0x7fff,
        .wclear   = 0x7fff,
        .offset   = offsetof(IntelHDAState, state_sts),
        .whandler = intel_hda_set_state_sts,
    },

    [ ICH6_REG_GSTS ] = {
        .name     = "GSTS",
        .size     = 2,
         .wmask    = 0x2,
        .offset   = offsetof(IntelHDAState, gsts),
    },

    [ ICH6_REG_LLCH ] = {
        .name     = "LLCH",
        .size     = 2,
        .reset    = ICH6_REG_ALLCH,
    },
    /* interrupts */
    [ ICH6_REG_INTCTL ] = {
        .name     = "INTCTL",
        .size     = 4,
        .wmask    = 0xc00000ff,
        .offset   = offsetof(IntelHDAState, int_ctl),
        .whandler = intel_hda_set_int_ctl,
    },
    [ ICH6_REG_INTSTS ] = {
        .name     = "INTSTS",
        .size     = 4,
        .wmask    = 0xc00000ff,
        .wclear   = 0xc00000ff,
        .offset   = offsetof(IntelHDAState, int_sts),
    },

    /* misc */
    [ ICH6_REG_WALLCLK ] = {
        .name     = "WALLCLK",
        .size     = 4,
        .offset   = offsetof(IntelHDAState, wall_clk),
        .rhandler = intel_hda_get_wall_clk,
    },

    /* dma engine */
    [ ICH6_REG_CORBLBASE ] = {
        .name     = "CORBLBASE",
        .size     = 4,
        .wmask    = 0xffffff80,
        .offset   = offsetof(IntelHDAState, corb_lbase),
    },
    [ ICH6_REG_CORBUBASE ] = {
        .name     = "CORBUBASE",
        .size     = 4,
        .wmask    = 0xffffffff,
        .offset   = offsetof(IntelHDAState, corb_ubase),
    },
    [ ICH6_REG_CORBWP ] = {
        .name     = "CORBWP",
        .size     = 2,
        .wmask    = 0xff,
        .offset   = offsetof(IntelHDAState, corb_wp),
        .whandler = intel_hda_set_corb_wp,
    },
    [ ICH6_REG_CORBRP ] = {
        .name     = "CORBRP",
        .size     = 2,
        .wmask    = 0x80ff,
        .offset   = offsetof(IntelHDAState, corb_rp),
    },
    [ ICH6_REG_CORBCTL ] = {
        .name     = "CORBCTL",
        .size     = 1,
        .wmask    = 0x03,
        .offset   = offsetof(IntelHDAState, corb_ctl),
        .whandler = intel_hda_set_corb_ctl,
    },
    [ ICH6_REG_CORBSTS ] = {
        .name     = "CORBSTS",
        .size     = 1,
        .wmask    = 0x01,
        .wclear   = 0x01,
        .offset   = offsetof(IntelHDAState, corb_sts),
    },
    [ ICH6_REG_CORBSIZE ] = {
        .name     = "CORBSIZE",
        .size     = 1,
        .reset    = 0x42,
        .offset   = offsetof(IntelHDAState, corb_size),
    },
    [ ICH6_REG_RIRBLBASE ] = {
        .name     = "RIRBLBASE",
        .size     = 4,
        .wmask    = 0xffffff80,
        .offset   = offsetof(IntelHDAState, rirb_lbase),
    },
    [ ICH6_REG_RIRBUBASE ] = {
        .name     = "RIRBUBASE",
        .size     = 4,
        .wmask    = 0xffffffff,
        .offset   = offsetof(IntelHDAState, rirb_ubase),
    },
    [ ICH6_REG_RIRBWP ] = {
        .name     = "RIRBWP",
        .size     = 2,
        .wmask    = 0x8000,
        .offset   = offsetof(IntelHDAState, rirb_wp),
        .whandler = intel_hda_set_rirb_wp,
    },
    [ ICH6_REG_RINTCNT ] = {
        .name     = "RINTCNT",
        .size     = 2,
        .wmask    = 0xff,
        .offset   = offsetof(IntelHDAState, rirb_cnt),
    },
    [ ICH6_REG_RIRBCTL ] = {
        .name     = "RIRBCTL",
        .size     = 1,
        .wmask    = 0x07,
        .offset   = offsetof(IntelHDAState, rirb_ctl),
    },
    [ ICH6_REG_RIRBSTS ] = {
        .name     = "RIRBSTS",
        .size     = 1,
        .wmask    = 0x05,
        .wclear   = 0x05,
        .offset   = offsetof(IntelHDAState, rirb_sts),
        .whandler = intel_hda_set_rirb_sts,
    },
    [ ICH6_REG_RIRBSIZE ] = {
        .name     = "RIRBSIZE",
        .size     = 1,
        .reset    = 0x42,
        .offset   = offsetof(IntelHDAState, rirb_size),
    },

    [ ICH6_REG_DPLBASE ] = {
        .name     = "DPLBASE",
        .size     = 4,
        .wmask    = 0xffffff81,
        .offset   = offsetof(IntelHDAState, dp_lbase),
    },
    [ ICH6_REG_DPUBASE ] = {
        .name     = "DPUBASE",
        .size     = 4,
        .wmask    = 0xffffffff,
        .offset   = offsetof(IntelHDAState, dp_ubase),
    },

    [ ICH6_REG_IC ] = {
        .name     = "ICW",
        .size     = 4,
        .wmask    = 0xffffffff,
        .offset   = offsetof(IntelHDAState, icw),
    },
    [ ICH6_REG_IR ] = {
        .name     = "IRR",
        .size     = 4,
        .offset   = offsetof(IntelHDAState, irr),
    },
    [ ICH6_REG_IRS ] = {
        .name     = "ICS",
        .size     = 2,
        .wmask    = 0x0003,
        .wclear   = 0x0002,
        .offset   = offsetof(IntelHDAState, ics),
        .whandler = intel_hda_set_ics,
    },

   /* Control and Status */
   [ HDA_DSP_REG_ADSPCS ] = {
        .name     = "ADSPCS",
        .size     = 4,
        .whandler = intel_hda_set_adspcs,
        .reset    = 0x0000ffff,
        .offset   = offsetof(IntelHDAState, adspcs),
        .wmask    = 0x00ffffff,
    },
   [ HDA_DSP_REG_ADSPIC ] = {
        .name     = "ADSPIC",
        .size     = 4,
        .whandler = intel_hda_set_adspic,
        .offset   = offsetof(IntelHDAState, adspic),
    },
   [ HDA_DSP_REG_ADSPIS ] = {
        .name     = "ADSPIS",
        .size     = 4,
        .rhandler = intel_hda_get_adspis,
        .offset   = offsetof(IntelHDAState, adspis),
    },
    [ HDA_DSP_REG_ADSPIC2 ] = {
        .name     = "ADSPIC2",
        .size     = 4,
        .whandler = intel_hda_set_adspic2,
        .offset   = offsetof(IntelHDAState, adspic2),
    },
   [ HDA_DSP_REG_ADSPIS2 ] = {
        .name     = "ADSPIS2",
        .size     = 4,
        .rhandler = intel_hda_get_adspis2,
        .offset   = offsetof(IntelHDAState, adspis2),
    },

    /* codeloader */
#define HDA_CL_STREAM(_t, _i)                                            \
    [ CL_REG(_i, ICH6_REG_SD_CTL) ] = {                               \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " CTL",                          \
        .size     = 4,                                                \
        .wmask    = 0x1cff001f,                                       \
        .offset   = offsetof(IntelHDAState, st[_i].ctl),              \
        .whandler = intel_hda_set_st_ctl,                             \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_CTL) + 2] = {                            \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " CTL(stnr)",                    \
        .size     = 1,                                                \
        .shift    = 16,                                               \
        .wmask    = 0x00ff0000,                                       \
        .offset   = offsetof(IntelHDAState, st[_i].ctl),              \
        .whandler = intel_hda_set_st_ctl,                             \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_STS)] = {                                \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " CTL(sts)",                     \
        .size     = 1,                                                \
        .shift    = 24,                                               \
        .wmask    = 0x1c000000,                                       \
        .wclear   = 0x1c000000,                                       \
        .offset   = offsetof(IntelHDAState, st[_i].ctl),              \
        .whandler = intel_hda_set_st_ctl,                             \
        .reset    = SD_STS_FIFO_READY << 24                           \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_LPIB) ] = {                              \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " LPIB",                         \
        .size     = 4,                                                \
        .offset   = offsetof(IntelHDAState, st[_i].lpib),             \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_LPIB) + 0x2000 ] = {                     \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " LPIB(alias)",                  \
        .size     = 4,                                                \
        .offset   = offsetof(IntelHDAState, st[_i].lpib),             \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_CBL) ] = {                               \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " CBL",                          \
        .size     = 4,                                                \
        .wmask    = 0xffffffff,                                       \
        .offset   = offsetof(IntelHDAState, st[_i].cbl),              \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_LVI) ] = {                               \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " LVI",                          \
        .size     = 2,                                                \
        .wmask    = 0x00ff,                                           \
        .offset   = offsetof(IntelHDAState, st[_i].lvi),              \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_FIFOSIZE) ] = {                          \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " FIFOS",                        \
        .size     = 2,                                                \
        .reset    = HDA_BUFFER_SIZE,                                  \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_FORMAT) ] = {                            \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " FMT",                          \
        .size     = 2,                                                \
        .wmask    = 0x7f7f,                                           \
        .offset   = offsetof(IntelHDAState, st[_i].fmt),              \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_BDLPL) ] = {                             \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " BDLPL",                        \
        .size     = 4,                                                \
        .wmask    = 0xffffff80,                                       \
        .offset   = offsetof(IntelHDAState, st[_i].bdlp_lbase),       \
    },                                                                \
    [ CL_REG(_i, ICH6_REG_SD_BDLPU) ] = {                             \
        .stream   = _i,                                               \
        .name     = _t stringify(_i) " BDLPU",                        \
        .size     = 4,                                                \
        .wmask    = 0xffffffff,                                       \
        .offset   = offsetof(IntelHDAState, st[_i].bdlp_ubase),       \
    },                                                                \

    HDA_CL_STREAM("CL", 15)

    /* IPC */
    [ HDA_DSP_REG_HIPCT ] = {
        .name     = "HIPCT",
        .size     = 4,
    .whandler = intel_hda_set_hipct,
        .offset   = offsetof(IntelHDAState, hipct),
    },
    [ HDA_DSP_REG_HIPCTE ] = {
        .name     = "HIPCTE",
        .size     = 4,
    .whandler = intel_hda_set_hipcte,
        .offset   = offsetof(IntelHDAState, hipcte),
    },
    [ HDA_DSP_REG_HIPCI ] = {
        .name     = "HIPCI",
        .size     = 4,
    .whandler = intel_hda_set_hipci,
        .offset   = offsetof(IntelHDAState, hipci),
    },
    [ HDA_DSP_REG_HIPCIE ] = {
        .name     = "HIPCIE",
        .size     = 4,
    .whandler = intel_hda_set_hipcie,
        .offset   = offsetof(IntelHDAState, hipcie),
    },
    [ HDA_DSP_REG_HIPCCTL ] = {
        .name     = "HIPCCTL",
        .size     = 4,
    .whandler = intel_hda_set_hipcctl,
        .offset   = offsetof(IntelHDAState, hipcctl),
    },

    /* rom regs */
    [ HDA_DSP_REG_ROM_STATUS ] = {
        .name    = "ROMSTS",
        .size    = 4,
        .reset   = HDA_DSP_ROM_STATUS_INIT,
        .rhandler = intel_hda_get_rom_status,
        .offset   = offsetof(IntelHDAState, romsts),
    },
    [ HDA_DSP_REG_ROM_ERROR ] = {
        .name    = "ROMERR",
        .size    = 4,
        .reset   = 0,
    },
    [ HDA_DSP_REG_ROM_END ] = {
        .name    = "ROMEND",
        .size    = 4,
        .reset   = 0,
    },
};

static const IntelHDAReg *intel_hda_dsp_reg_find(IntelHDAState *d, hwaddr addr)
{
    const IntelHDAReg *reg;

    if (addr >= ARRAY_SIZE(regtabdsp)) {
        goto noreg;
    }
    reg = regtabdsp + addr;
    if (reg->name == NULL) {
        goto noreg;
    }
    return reg;

noreg:
    dprint(d, 1, "unknown DSP register, addr 0x%x\n", (int) addr);
    return NULL;
}

static void intel_hda_dsp_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                                 unsigned size)
{
    IntelHDAState *d = opaque;
    const IntelHDAReg *reg = intel_hda_dsp_reg_find(d, addr);

    if (!reg)
        fprintf(stderr, "hda-dsp: no register to write at 0x%lx value 0x%lx size %d\n",
                addr, val, size);

    intel_adsp_reg_write(d, reg, val, MAKE_64BIT_MASK(0, size * 8));
}

static uint64_t intel_hda_dsp_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    IntelHDAState *d = opaque;
    const IntelHDAReg *reg = intel_hda_dsp_reg_find(d, addr);

    if (!reg)
        fprintf(stderr, "hda-dsp: no register to read at 0x%lx size %d\n", addr, size);

    return intel_hda_reg_read(d, reg, MAKE_64BIT_MASK(0, size * 8));
}

static const MemoryRegionOps intel_hda_dsp_mmio_ops = {
    .read = intel_hda_dsp_mmio_read,
    .write = intel_hda_dsp_mmio_write,
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void intel_hda_dsp_realize(PCIDevice *pci, Error **errp,
        int version, const char *name)
{
    IntelHDAState *d = INTEL_HDA(pci);
    uint8_t *conf = d->pci.config;
    IntelHDAStream *st = d->st + 8;
    Error *err = NULL;
    int ret;

    d->name = object_get_typename(OBJECT(d));

    pci_config_set_interrupt_pin(conf, 1);

    /* HDCTL off 0x40 bit 0 selects signaling mode (1-HDA, 0 - Ac97) 18.1.19 */
    conf[0x40] = 0x01;

    if (d->msi != ON_OFF_AUTO_OFF) {
        ret = msi_init(&d->pci, d->old_msi_addr ? 0x50 : 0x60,
                       1, true, false, &err);
        /* Any error other than -ENOTSUP(board's MSI support is broken)
         * is a programming error */
        assert(!ret || ret == -ENOTSUP);
        if (ret && d->msi == ON_OFF_AUTO_ON) {
            /* Can't satisfy user's explicit msi=on request, fail */
            error_append_hint(&err, "You have to use msi=auto (default) or "
                    "msi=off with this machine type.\n");
            error_propagate(errp, err);
            return;
        }
        assert(!err || d->msi == ON_OFF_AUTO_AUTO);
        /* With msi=auto, we fall back to MSI off silently */
        error_free(err);
    }

    /* codeloader is */
    st->is_codeloader = true;

    /* HDA Legacy BAR 0 */
    memory_region_init_io(&d->mmio, OBJECT(d), &intel_hda_dsp_mmio_ops, d,
                          "intel-hda", 0x4000);
    pci_register_bar(&d->pci, 0, 0, &d->mmio);

    /* DSP BAR 4 */
    memory_region_init_io(&d->mmio_dsp, OBJECT(d), &intel_hda_dsp_mmio_ops, d,
                          "intel-hda-dsp", 0x100000);
    pci_register_bar(&d->pci, 4, 0, &d->mmio_dsp);

    hda_codec_bus_init(DEVICE(pci), &d->codecs, sizeof(d->codecs),
                       intel_hda_response, intel_hda_xfer);
    adsp_hda_init(d, version,  name);
}

static void intel_hda_dsp_realize_bxt(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 15, "bxt");
}

static void intel_hda_dsp_realize_apl(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 15, "apl");
}

static void intel_hda_dsp_realize_glk(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 15, "glk");
}

static void intel_hda_dsp_realize_kbl(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 15, "kbl");
}

static void intel_hda_dsp_realize_skl(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 15, "skl");
}

static void intel_hda_dsp_realize_cnl(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 18, "cnl");
}

static void intel_hda_dsp_realize_cfl(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 18, "cfl");
}

static void intel_hda_dsp_realize_icl(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 18, "icl");
}

static void intel_hda_dsp_realize_tgl(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 25, "tgl");
}

static void intel_hda_dsp_realize_cml(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 15, "cml");
}

static void intel_hda_dsp_realize_ehl(PCIDevice *pci, Error **errp)
{
    intel_hda_dsp_realize(pci, errp, 18, "ehl");
}

static const VMStateDescription vmstate_intel_hda_stream = {
    .name = "intel-hda-stream",
    .version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32(ctl, IntelHDAStream),
        VMSTATE_UINT32(lpib, IntelHDAStream),
        VMSTATE_UINT32(cbl, IntelHDAStream),
        VMSTATE_UINT32(lvi, IntelHDAStream),
        VMSTATE_UINT32(fmt, IntelHDAStream),
        VMSTATE_UINT32(bdlp_lbase, IntelHDAStream),
        VMSTATE_UINT32(bdlp_ubase, IntelHDAStream),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_intel_dsp_hda = {
    .name = "intel-hda",
    .version_id = 1,
    .post_load = intel_hda_post_load,
    .fields = (VMStateField[]) {
        VMSTATE_PCI_DEVICE(pci, IntelHDAState),

        /* registers */
        VMSTATE_UINT32(g_ctl, IntelHDAState),
        VMSTATE_UINT32(wake_en, IntelHDAState),
        VMSTATE_UINT32(state_sts, IntelHDAState),
        VMSTATE_UINT32(int_ctl, IntelHDAState),
        VMSTATE_UINT32(int_sts, IntelHDAState),
        VMSTATE_UINT32(wall_clk, IntelHDAState),
        VMSTATE_UINT32(corb_lbase, IntelHDAState),
        VMSTATE_UINT32(corb_ubase, IntelHDAState),
        VMSTATE_UINT32(corb_rp, IntelHDAState),
        VMSTATE_UINT32(corb_wp, IntelHDAState),
        VMSTATE_UINT32(corb_ctl, IntelHDAState),
        VMSTATE_UINT32(corb_sts, IntelHDAState),
        VMSTATE_UINT32(corb_size, IntelHDAState),
        VMSTATE_UINT32(rirb_lbase, IntelHDAState),
        VMSTATE_UINT32(rirb_ubase, IntelHDAState),
        VMSTATE_UINT32(rirb_wp, IntelHDAState),
        VMSTATE_UINT32(rirb_cnt, IntelHDAState),
        VMSTATE_UINT32(rirb_ctl, IntelHDAState),
        VMSTATE_UINT32(rirb_sts, IntelHDAState),
        VMSTATE_UINT32(rirb_size, IntelHDAState),
        VMSTATE_UINT32(dp_lbase, IntelHDAState),
        VMSTATE_UINT32(dp_ubase, IntelHDAState),
        VMSTATE_UINT32(icw, IntelHDAState),
        VMSTATE_UINT32(irr, IntelHDAState),
        VMSTATE_UINT32(ics, IntelHDAState),
        VMSTATE_STRUCT_ARRAY(st, IntelHDAState, 16, 0,
                             vmstate_intel_hda_stream,
                             IntelHDAStream),

        /* additional state info */
        VMSTATE_UINT32(rirb_count, IntelHDAState),
        VMSTATE_INT64(wall_base_ns, IntelHDAState),

        VMSTATE_END_OF_LIST()
    }
};

static void intel_hda_class_init_dsp(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize = intel_hda_dsp_realize_bxt;
    k->exit = intel_hda_exit;
    k->vendor_id = PCI_VENDOR_ID_INTEL;
    k->class_id = PCI_CLASS_MULTIMEDIA_HD_DSP_AUDIO;
    dc->reset = intel_hda_reset;
    dc->vmsd = &vmstate_intel_dsp_hda;
}

static void intel_hda_class_init_apl(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x1a98;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Apollolake)";
    k->realize = intel_hda_dsp_realize_apl;
}

static void intel_hda_class_init_glk(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x3198;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Geminilake)";
    k->realize = intel_hda_dsp_realize_glk;
}

static void intel_hda_class_init_cnl(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x9dc8;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Cannonlake)";
    k->realize = intel_hda_dsp_realize_cnl;
}

static void intel_hda_class_init_cfl(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0xa348;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Coffeelake)";
    k->realize = intel_hda_dsp_realize_cfl;
}

static void intel_hda_class_init_kbl(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x9d71;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Kabylake)";
    k->realize = intel_hda_dsp_realize_kbl;
}

static void intel_hda_class_init_skl(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x9d70;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Skylake)";
    k->realize = intel_hda_dsp_realize_skl;
}

static void intel_hda_class_init_icl(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x34C8;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Icelake)";
    k->realize = intel_hda_dsp_realize_icl;
}

static void intel_hda_class_init_cml_lp(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x02c8;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Cometlake LP)";
    k->realize = intel_hda_dsp_realize_cml;
}

static void intel_hda_class_init_cml_h(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x06c8;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Cometlake H)";
    k->realize = intel_hda_dsp_realize_cml;
}

static void intel_hda_class_init_tgl(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0xa0c8;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Tigerlake)";
    k->realize = intel_hda_dsp_realize_tgl;
}

static void intel_hda_class_init_ehl(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->device_id = 0x4b55;
    k->revision = 3;
    set_bit(DEVICE_CATEGORY_SOUND, dc->categories);
    dc->desc = "Intel HD Audio Controller (Elkhartlake)";
    k->realize = intel_hda_dsp_realize_ehl;
}

static const TypeInfo intel_hda_info = {
    .name          = TYPE_INTEL_HDA_GENERIC,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(IntelHDAState),
    .class_init    = intel_hda_class_init_dsp,
    .abstract      = true,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static const TypeInfo intel_hda_info_apl = {
    .name          = "apl-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_apl,
};

static const TypeInfo intel_hda_info_glk = {
    .name          = "glk-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_glk,
};

static const TypeInfo intel_hda_info_cnl = {
    .name          = "cnl-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_cnl,
};

static const TypeInfo intel_hda_info_cfl = {
    .name          = "cfl-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_cfl,
};

static const TypeInfo intel_hda_info_kbl = {
    .name          = "kbl-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_kbl,
};

static const TypeInfo intel_hda_info_skl = {
    .name          = "skl-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_skl,
};

static const TypeInfo intel_hda_info_icl = {
    .name          = "icl-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_icl,
};

static const TypeInfo intel_hda_info_cml_lp = {
    .name          = "cml-lp-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_cml_lp,
};

static const TypeInfo intel_hda_info_cml_h = {
    .name          = "cml_h-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_cml_h,
};

static const TypeInfo intel_hda_info_tgl = {
    .name          = "tgl-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_tgl,
};

static const TypeInfo intel_hda_info_ehl = {
    .name          = "ehl-intel-hda",
    .parent        = TYPE_INTEL_HDA_GENERIC,
    .class_init    = intel_hda_class_init_ehl,
};

static void adsp_do_irq(IntelHDAState *d, struct qemu_io_msg *msg)
{
    //struct adsp_host *adsp = d->adsp;
    uint32_t active = 0;

#if 0
    active = adsp->shim_io[SHIM_ISRX >> 2] & ~(adsp->shim_io[SHIM_IMRX >> 2]);

    log_text(adsp->log, LOG_IRQ_ACTIVE,
        "DSP IRQ: status %x mask %x active %x cmd %x\n",
        adsp->shim_io[SHIM_ISRX >> 2],
        adsp->shim_io[SHIM_IMRX >> 2], active,
        adsp->shim_io[SHIM_IPCD >> 2]);
#endif
    if (active) {
        pci_set_irq(&d->pci, 1);
    }
}

static int adsp_bridge_cb(void *data, struct qemu_io_msg *msg)
{
    IntelHDAState *d = data;
    struct adsp_host *adsp = d->adsp;

    switch (msg->type) {
    case QEMU_IO_TYPE_REG:
        /* mostly handled by SHM, some exceptions */
        break;
    case QEMU_IO_TYPE_IRQ:
        adsp_do_irq(d, msg);
        break;
    case QEMU_IO_TYPE_DMA:
        adsp_host_do_dma(adsp, msg);
        break;
    case QEMU_IO_TYPE_PM:
    case QEMU_IO_TYPE_MEM:
    default:
        break;
    }

    return 0;
}

static struct adsp_mem_desc adsp_1_5_mem[] = {
    {.name = "l2-sram", .base = ADSP_CAVS_HOST_DSP_SRAM_BASE,
        .size = ADSP_CAVS_1_5_DSP_SRAM_SIZE},
    {.name = "hp-sram", .base = ADSP_CAVS_HOST_DSP_1_5_HP_SRAM_BASE,
        .size = ADSP_CAVS_1_5_DSP_HP_SRAM_SIZE,},
    {.name = "lp-sram", .base = ADSP_CAVS_HOST_DSP_1_5_LP_SRAM_BASE,
        .size = ADSP_CAVS_1_5_DSP_LP_SRAM_SIZE},
    {.name = "imr", .base = ADSP_CAVS_HOST_DSP_1_5_IMR_BASE,
        .size = ADSP_CAVS_1_5_DSP_IMR_SIZE},
    {.name = "rom", .base = ADSP_CAVS_HOST_1_5_ROM_BASE,
        .size = ADSP_CAVS_DSP_ROM_SIZE},
};

static struct adsp_mem_desc adsp_1_8_mem[] = {
    {.name = "l2-sram", .base = ADSP_CAVS_HOST_DSP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_SRAM_SIZE},
    {.name = "hp-sram", .base = ADSP_CAVS_HOST_DSP_1_8_HP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_HP_SRAM_SIZE,},
    {.name = "lp-sram", .base = ADSP_CAVS_HOST_DSP_1_8_LP_SRAM_BASE,
        .size = ADSP_CAVS_1_8_DSP_LP_SRAM_SIZE},
    {.name = "imr", .base = ADSP_CAVS_HOST_DSP_1_8_IMR_BASE,
        .size = ADSP_CAVS_1_8_DSP_IMR_SIZE},
    {.name = "rom", .base = ADSP_CAVS_HOST_1_8_ROM_BASE,
        .size = ADSP_CAVS_DSP_ROM_SIZE},
};

static struct adsp_reg_space adsp_1_5_io[] = {
    { .name = "hostwin0",
        .desc = {.base = ADSP_CAVS_HOST_1_5_SRAM_WND_BASE(0), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin1",
        .desc = {.base = ADSP_CAVS_HOST_1_5_SRAM_WND_BASE(1), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin2",
        .desc = {.base = ADSP_CAVS_HOST_1_5_SRAM_WND_BASE(2), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin3",
        .desc = {.base = ADSP_CAVS_HOST_1_5_SRAM_WND_BASE(3), .size = ADSP_CAVS_WND_SIZE},},
};

static struct adsp_reg_space adsp_1_8_io[] = {
    { .name = "hostwin0",
        .desc = {.base = ADSP_CAVS_HOST_1_8_SRAM_WND_BASE(0), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin1",
        .desc = {.base = ADSP_CAVS_HOST_1_8_SRAM_WND_BASE(1), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin2",
        .desc = {.base = ADSP_CAVS_HOST_1_8_SRAM_WND_BASE(2), .size = ADSP_CAVS_WND_SIZE},},
    { .name = "hostwin3",
        .desc = {.base = ADSP_CAVS_HOST_1_8_SRAM_WND_BASE(3), .size = ADSP_CAVS_WND_SIZE},},
};

static const struct adsp_desc adsp_1_5 = {
    .num_mem = ARRAY_SIZE(adsp_1_5_mem),
    .mem_region = adsp_1_5_mem,
    .num_io = ARRAY_SIZE(adsp_1_5_io),
    .io_dev = adsp_1_5_io,
};

static const struct adsp_desc adsp_1_8 = {
    .num_mem = ARRAY_SIZE(adsp_1_8_mem),
    .mem_region = adsp_1_8_mem,
    .num_io = ARRAY_SIZE(adsp_1_8_io),
    .io_dev = adsp_1_8_io,
};

void adsp_hda_init(IntelHDAState *d, int version, const char *name)
{
    struct adsp_host *adsp;

    adsp = calloc(1, sizeof(*adsp));
    if (!adsp) {
        fprintf(stderr, "cant alloc DSP\n");
        exit(0);
    }
    d->adsp = adsp;

    switch (version) {
    case 15:
        adsp->desc = &adsp_1_5;
        break;
    case 18:
    case 20:
    case 25:
        adsp->desc = &adsp_1_8;
        break;
    default:
        fprintf(stderr, "no such version %d\n", version);
        exit(0);
    }

    adsp->system_memory = get_system_memory();
    adsp->machine_opts = qemu_get_machine_opts();
    adsp->shm_idx = 0;

    adsp_create_host_memory_regions(adsp);

    adsp_create_host_io_devices(adsp, NULL);

    /* initialise bridge to x86 host driver */
    qemu_io_register_parent(name, &adsp_bridge_cb, (void*)d);
}

/*
 * create intel hda controller with codec attached to it,
 * so '-soundhw hda' works.
 */
static int intel_hda_and_codec_init(PCIBus *bus)
{
    DeviceState *controller;
    BusState *hdabus;
    DeviceState *codec;

    warn_report("'-soundhw hda' is deprecated, "
                "please use '-device intel-hda -device hda-duplex' instead");
    controller = DEVICE(pci_create_simple(bus, -1, "intel-hda"));
    hdabus = QLIST_FIRST(&controller->child_bus);
    codec = qdev_new("hda-duplex");
    qdev_realize_and_unref(codec, hdabus, &error_fatal);
    return 0;
}

static void intel_hda_register_types(void)
{
    type_register_static(&intel_hda_info);
    type_register_static(&intel_hda_info_apl);
    type_register_static(&intel_hda_info_glk);
    type_register_static(&intel_hda_info_cnl);
    type_register_static(&intel_hda_info_cfl);
    type_register_static(&intel_hda_info_skl);
    type_register_static(&intel_hda_info_kbl);
    type_register_static(&intel_hda_info_icl);
    type_register_static(&intel_hda_info_cml_lp);
    type_register_static(&intel_hda_info_cml_h);
    type_register_static(&intel_hda_info_tgl);
    type_register_static(&intel_hda_info_ehl);

 //   type_register_static(&hda_codec_device_type_info);
    pci_register_soundhw("hda", "Intel HD Audio", intel_hda_and_codec_init);
}

type_init(intel_hda_register_types)
