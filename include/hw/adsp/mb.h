/* Mailbox support for mt8195 audio DSP.
 *
 * Copyright(c) 2021 Mediatek
 *
 * Author: Trevor Wu <trevor.wu@mediatek.com>
 */

#ifndef ADSP_MB_H
#define ADSP_MB_H

#define MTK_DSP_MBOX_IN_CMD  0x0
#define MTK_DSP_MBOX_IN_CMD_CLR 0x04
#define MTK_DSP_MBOX_OUT_CMD 0x1c
#define ADSP_IPI_OP_REQ BIT(0)
#define ADSP_IPI_OP_RSP BIT(1)
#define MTK_DSP_MBOX_OUT_CMD_CLR 0x20

#endif

