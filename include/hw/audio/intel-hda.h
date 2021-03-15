#ifndef HW_INTEL_HDA_H
#define HW_INTEL_HDA_H

#include "hw/qdev-core.h"

/* Intel HD Audio General DSP Registers */
#define HDA_DSP_GEN_BASE		0x0
#define HDA_DSP_REG_ADSPCS		(HDA_DSP_GEN_BASE + 0x04)
#define HDA_DSP_REG_ADSPIC		(HDA_DSP_GEN_BASE + 0x08)
#define HDA_DSP_REG_ADSPIS		(HDA_DSP_GEN_BASE + 0x0C)
#define HDA_DSP_REG_ADSPIC2		(HDA_DSP_GEN_BASE + 0x10)
#define HDA_DSP_REG_ADSPIS2		(HDA_DSP_GEN_BASE + 0x14)

/* Intel HD Audio Inter-Processor Communication Registers */
#define HDA_DSP_IPC_BASE		0x40
#define HDA_DSP_REG_HIPCT		(HDA_DSP_IPC_BASE + 0x00)
#define HDA_DSP_REG_HIPCTE		(HDA_DSP_IPC_BASE + 0x04)
#define HDA_DSP_REG_HIPCI		(HDA_DSP_IPC_BASE + 0x08)
#define HDA_DSP_REG_HIPCIE		(HDA_DSP_IPC_BASE + 0x0C)
#define HDA_DSP_REG_HIPCCTL		(HDA_DSP_IPC_BASE + 0x10)

/* Intel HD Audio Code Loader DMA Registers */
#define HDA_ADSP_LOADER_BASE		0x80
#define HDA_ADSP_DPLBASE			0x70
#define HDA_ADSP_DPUBASE			0x74

/* --------------------------------------------------------------------- */
/* hda bus                                                               */

#define TYPE_HDA_CODEC_DEVICE "hda-codec"
#define HDA_CODEC_DEVICE(obj) \
     OBJECT_CHECK(HDACodecDevice, (obj), TYPE_HDA_CODEC_DEVICE)
#define HDA_CODEC_DEVICE_CLASS(klass) \
     OBJECT_CLASS_CHECK(HDACodecDeviceClass, (klass), TYPE_HDA_CODEC_DEVICE)
#define HDA_CODEC_DEVICE_GET_CLASS(obj) \
     OBJECT_GET_CLASS(HDACodecDeviceClass, (obj), TYPE_HDA_CODEC_DEVICE)

#define TYPE_HDA_BUS "HDA"
#define HDA_BUS(obj) OBJECT_CHECK(HDACodecBus, (obj), TYPE_HDA_BUS)

typedef struct HDACodecBus HDACodecBus;
typedef struct HDACodecDevice HDACodecDevice;

typedef void (*hda_codec_response_func)(HDACodecDevice *dev,
                                        bool solicited, uint32_t response);
typedef bool (*hda_codec_xfer_func)(HDACodecDevice *dev,
                                    uint32_t stnr, bool output,
                                    uint8_t *buf, uint32_t len);

struct HDACodecBus {
    BusState qbus;
    uint32_t next_cad;
    hda_codec_response_func response;
    hda_codec_xfer_func xfer;
};

typedef struct HDACodecDeviceClass
{
    DeviceClass parent_class;

    int (*init)(HDACodecDevice *dev);
    void (*exit)(HDACodecDevice *dev);
    void (*command)(HDACodecDevice *dev, uint32_t nid, uint32_t data);
    void (*stream)(HDACodecDevice *dev, uint32_t stnr, bool running, bool output);
} HDACodecDeviceClass;

struct HDACodecDevice {
    DeviceState         qdev;
    uint32_t            cad;    /* codec address */
};

typedef struct IntelHDAStream IntelHDAStream;
typedef struct IntelHDAState IntelHDAState;
typedef struct IntelHDAReg IntelHDAReg;
struct adsp_host;

typedef struct bpl {
    uint64_t addr;
    uint32_t len;
    uint32_t flags;
} bpl;

struct IntelHDAStream {
    /* registers */
    uint32_t ctl;
    uint32_t lpib;
    uint32_t cbl;
    uint32_t lvi;
    uint32_t fmt;
    uint32_t bdlp_lbase;
    uint32_t bdlp_ubase;

    /* state */
    bpl      *bpl;
    uint32_t bentries;
    uint32_t bsize, be, bp;
    bool is_codeloader;
};

struct IntelHDASPIB {
    /* registers */
    uint32_t spib;
    uint32_t maxfifos;
};

struct IntelHDAState {
    PCIDevice pci;
    const char *name;
    HDACodecBus codecs;

    /* registers */
    uint32_t g_ctl;
    uint32_t wake_en;
    uint32_t state_sts;
    uint32_t int_ctl;
    uint32_t int_sts;
    uint32_t wall_clk;
    uint32_t gsts;

    uint32_t corb_lbase;
    uint32_t corb_ubase;
    uint32_t corb_rp;
    uint32_t corb_wp;
    uint32_t corb_ctl;
    uint32_t corb_sts;
    uint32_t corb_size;

    uint32_t rirb_lbase;
    uint32_t rirb_ubase;
    uint32_t rirb_wp;
    uint32_t rirb_cnt;
    uint32_t rirb_ctl;
    uint32_t rirb_sts;
    uint32_t rirb_size;

    uint32_t dp_lbase;
    uint32_t dp_ubase;

    uint32_t inpay;
    uint32_t outpay;

    uint32_t spib0;
    uint32_t sbpfcctl;
    uint32_t ppctl;
    uint32_t ppsts;

    uint32_t icw;
    uint32_t irr;
    uint32_t ics;

    uint32_t adspcs;
    uint32_t adspic;
    uint32_t adspis;
    uint32_t adspic2;
    uint32_t adspis2;

    uint32_t hipct;
    uint32_t hipcte;
    uint32_t hipci;
    uint32_t hipcie;
    uint32_t hipcctl;
    uint32_t em2;

    /* streams */
    IntelHDAStream st[16];
    struct IntelHDASPIB sd[16];

    /* state */
    MemoryRegion container;
    MemoryRegion mmio;
    MemoryRegion mmio_dsp;
    MemoryRegion alias;
    uint32_t rirb_count;
    int64_t wall_base_ns;

    /* debug logging */
    const IntelHDAReg *last_reg;
    uint32_t last_val;
    uint32_t last_write;
    uint32_t last_sec;
    uint32_t repeat_count;

    /* properties */
    uint32_t debug;
    OnOffAuto msi;
    bool old_msi_addr;

    /* DSP */
    struct adsp_host *adsp;
    int fw_boot_count;
    uint32_t romsts;
};

struct IntelHDAReg {
    const char *name;      /* register name */
    uint32_t   size;       /* size in bytes */
    uint32_t   reset;      /* reset value */
    uint32_t   wmask;      /* write mask */
    uint32_t   wclear;     /* write 1 to clear bits */
    uint32_t   offset;     /* location in IntelHDAState */
    uint32_t   shift;      /* byte access entries for dwords */
    uint32_t   stream;
    void       (*whandler)(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
    void       (*rhandler)(IntelHDAState *d, const IntelHDAReg *reg);
};

void hda_codec_bus_init(DeviceState *dev, HDACodecBus *bus, size_t bus_size,
                        hda_codec_response_func response,
                        hda_codec_xfer_func xfer);
HDACodecDevice *hda_codec_find(HDACodecBus *bus, uint32_t cad);

void hda_codec_response(HDACodecDevice *dev, bool solicited, uint32_t response);
bool hda_codec_xfer(HDACodecDevice *dev, uint32_t stnr, bool output,
                    uint8_t *buf, uint32_t len);

void intel_hda_set_st_ctl(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
void intel_hda_response(HDACodecDevice *dev, bool solicited, uint32_t response);
bool intel_hda_xfer(HDACodecDevice *dev, uint32_t stnr, bool output,
                           uint8_t *buf, uint32_t len);
void intel_hda_exit(PCIDevice *pci);
void intel_hda_reset(DeviceState *dev);
int intel_hda_post_load(void *opaque, int version);
void intel_hda_set_g_ctl(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
void intel_hda_set_wake_en(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
void intel_hda_set_state_sts(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
void intel_hda_set_int_ctl(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
void intel_hda_get_wall_clk(IntelHDAState *d, const IntelHDAReg *reg);
void intel_hda_set_corb_wp(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
void intel_hda_set_corb_ctl(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
void intel_hda_set_rirb_wp(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
void intel_hda_set_rirb_sts(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);
void intel_hda_set_ics(IntelHDAState *d, const IntelHDAReg *reg, uint32_t old);

/* --------------------------------------------------------------------- */

#define dprint(_dev, _level, _fmt, ...)                                 \
    do {                                                                \
        if (_dev->debug >= _level) {                                    \
            fprintf(stderr, "%s: ", _dev->name);                        \
            fprintf(stderr, _fmt, ## __VA_ARGS__);                      \
        }                                                               \
    } while (0)

/* --------------------------------------------------------------------- */

#endif
