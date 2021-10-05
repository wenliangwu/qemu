/* Core Audio DSP
 *
 * Copyright(c) 2021 Mediatek
 *
 * Author: Trevor Wu <trevor.wu@mediatek.com>
 *
 */

#ifndef __HW_ADSP_MT8195_H__
#define __HW_ADSP_MT8195_H__

#define ADSP_MT8195_DSP_MAILBOX_BASE      0x60800000
#define ADSP_MT8195_HOST_IRAM_OFFSET      0x10000

/* Mailbox configuration */
#define ADSP_SRAM_OUTBOX_BASE       ADSP_MT8195_DSP_MAILBOX_BASE
#define ADSP_SRAM_OUTBOX_SIZE       0x1000
#define ADSP_SRAM_OUTBOX_OFFSET     0

#define ADSP_SRAM_INBOX_BASE        (ADSP_SRAM_OUTBOX_BASE \
                                    + ADSP_SRAM_OUTBOX_SIZE)
#define ADSP_SRAM_INBOX_SIZE        0x1000
#define ADSP_SRAM_INBOX_OFFSET      ADSP_SRAM_OUTBOX_SIZE

#define ADSP_SRAM_DEBUG_BASE        (ADSP_SRAM_INBOX_BASE \
                                    + ADSP_SRAM_INBOX_SIZE)
#define ADSP_SRAM_DEBUG_SIZE        0x800
#define ADSP_SRAM_DEBUG_OFFSET      (ADSP_SRAM_INBOX_OFFSET \
                                    + ADSP_SRAM_INBOX_SIZE)

#define ADSP_SRAM_EXCEPT_BASE       (ADSP_SRAM_DEBUG_BASE \
                                    + ADSP_SRAM_DEBUG_SIZE)
#define ADSP_SRAM_EXCEPT_SIZE       0x800
#define ADSP_SRAM_EXCEPT_OFFSET     (ADSP_SRAM_DEBUG_OFFSET \
                                    + ADSP_SRAM_DEBUG_SIZE)

#define ADSP_SRAM_STREAM_BASE       (ADSP_SRAM_EXCEPT_BASE \
                                    + ADSP_SRAM_EXCEPT_SIZE)
#define ADSP_SRAM_STREAM_SIZE       0x1000
#define ADSP_SRAM_STREAM_OFFSET     (ADSP_SRAM_EXCEPT_OFFSET \
                                    + ADSP_SRAM_EXCEPT_SIZE)

#define ADSP_SRAM_TRACE_BASE        (ADSP_SRAM_STREAM_BASE \
                                    + ADSP_SRAM_STREAM_SIZE)
#define ADSP_SRAM_TRACE_SIZE        0x1000
#define ADSP_SRAM_TRACE_OFFSET      (ADSP_SRAM_STREAM_OFFSET \
                                    + ADSP_SRAM_STREAM_SIZE)

#define ADSP_MT8195_DSP_MAILBOX_SIZE  (ADSP_SRAM_INBOX_SIZE \
                                    + ADSP_SRAM_OUTBOX_SIZE \
                                    + ADSP_SRAM_DEBUG_SIZE \
                                    + ADSP_SRAM_EXCEPT_SIZE \
                                    + ADSP_SRAM_STREAM_SIZE \
                                    + ADSP_SRAM_TRACE_SIZE)

#define ADSP_MT8195_DSP_REG_BASE     0x10803000
#define ADSP_MT8195_DSP_REG_SIZE     0x600

#define ADSP_MT8195_DSP_DRAM0_BASE     0x60000000
#define ADSP_MT8195_DSP_DRAM0_SIZE     0x1000000

#define ADSP_MT8195_DSP_SRAM_BASE     0x40000000
#define ADSP_MT8195_DSP_SRAM_SIZE     0x40000

#define MTK_DSP_MBOX0_REG_BASE (0x10816000)
#define MTK_DSP_MBOX0_REG_SIZE (0x1000)
#define MTK_DSP_MBOX1_REG_BASE (0x10817000)
#define MTK_DSP_MBOX1_REG_SIZE (0x1000)
#define MTK_DSP_MBOX2_REG_BASE (0x10818000)
#define MTK_DSP_MBOX2_REG_SIZE (0x1000)

#endif
