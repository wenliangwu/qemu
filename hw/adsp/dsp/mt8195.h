/* Core DSP support for mt8195 audio DSP.
 *
 * Copyright(c) 2021 Mediatek
 *
 * Author: Trevor Wu <trevor.wu@mediatek.com>
 */

#ifndef __ADSP_MT8195_H__
#define __ADSP_MT8195_H__

struct adsp_dev;

void adsp_mt8195_mb_init(struct adsp_dev *adsp, MemoryRegion *parent,
        struct adsp_io_info *info);
extern const MemoryRegionOps mt8195_mb_ops;

#endif
