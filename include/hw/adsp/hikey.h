/* Core DSP support HiKey960.
 *
 * Copyright (C) 2018 John Gunn
 *
 * Author: John Gunn <jgunn0262@gmail.com>
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

#ifndef __HW_ADSP_HIKEY_H__
#define __HW_ADSP_HIKEY_H__

#include <sys/time.h>
#include <stdio.h>
#include <stdint.h>
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "exec/hwaddr.h"
#include "hw.h"

/*
 * Host Side
 */


#define ADSP_HIKEY_HOST_RUN_SIZE                   0x600000
#define ADSP_HIKEY_HOST_IMAGE_TCMBAK_SIZE          0x34000
#define ADSP_HIKEY_HOST_IMAGE_SIZE                 0x31C000
#define ADSP_HIKEY_HOST_RUN_ITCM_BASE              0xe8080000
#define ADSP_HIKEY_HOST_RUN_ITCM_SIZE              0x9000
#define ADSP_HIKEY_HOST_RUN_DTCM_BASE              0xe8058000
#define ADSP_HIKEY_HOST_RUN_DTCM_SIZE              0x28000

#define ADSP_HIKEY_HOST_PHYMEM_BASE 0x89200000
#define ADSP_HIKEY_HOST_PHYMEM_SIZE 0x980000

#define ADSP_HIKEY_HOST_OCRAM_BACK_BASE            0xE8000000
#define ADSP_HIKEY_HOST_TCM_BACK_BASE              0xE8058000
#define ADSP_HIKEY_HOST_HIFI_RUN_BASE         0xC0000000



/*
 * DSP Side - Non Secure 3.5M
 */

#define ADSP_HIKEY_MUSIC_DATA_BASE           0x8B300000
#define ADSP_HIKEY_MUSIC_DATA_SIZE           0x00132000

#define ADSP_HIKEY_PCM_DATA_BASE           0x8B432000
#define ADSP_HIKEY_PCM_DATA_SIZE           0x00100000

#define ADSP_HIKEY_HIFI_UART_BASE           0x8B532000
#define ADSP_HIKEY_HIFI_UART_SIZE           0x0007F000

#define ADSP_HIKEY_PANIC_STACK_BASE           0x8B5B1000
#define ADSP_HIKEY_PANIC_STACK_SIZE           0x00001000

#define ADSP_HIKEY_ICC_DEBUG_BASE           0x8B5B2000
#define ADSP_HIKEY_ICC_DEBUG_SIZE           0x00013000

#define ADSP_HIKEY_FLAG_DATA_BASE           0x8B5C5000
#define ADSP_HIKEY_FLAG_DATA_SIZE           0x00001000

#define ADSP_HIKEY_DDR_SEC_HEAD_BASE           0x8B5C6000
#define ADSP_HIKEY_DDR_SEC_HEAD_SIZE           0x00001000

#define ADSP_HIKEY_AP_NV_BASE           0x8B5C7000
#define ADSP_HIKEY_AP_NV_SIZE           0x00032800

#define ADSP_HIKEY_AP_HIFIMB_BASE           0x8B5F9800
#define ADSP_HIKEY_AP_HIFIMB_SIZE           0x00010000

#define ADSP_HIKEY_CODEC_DMA_BUF_BASE           0x8B609800
#define ADSP_HIKEY_CODEC_DMA_BUF_SIZE           0x0000f000

#define ADSP_HIKEY_CODEC_DMA_CONF_BASE           0x8B618800
#define ADSP_HIKEY_CODEC_DMA_CONF_SIZE           0x00000080

#define ADSP_HIKEY_SOUND_TRIGGER_BASE           0x8B618880
#define ADSP_HIKEY_SOUND_TRIGGER_SIZE           0x0000f000

#define ADSP_HIKEY_PCM_UPLOAD_BASE           0x8B627880
#define ADSP_HIKEY_PCM_UPLOAD_SIZE           0x00002000

#define ADSP_HIKEY_SHARE_BASE           0x8B629880
#define ADSP_HIKEY_SHARE_SIZE           0x00003000

#define ADSP_HIKEY_UNSEC_RSVD_BASE           0x8B62C880
#define ADSP_HIKEY_UNSEC_RSVD_SIZE           0x00053780

/*
 * DSP Side - Secure 9.5M
 */

#define ADSP_HIKEY_HIFI_RUN_BASE           0x89200000
#define ADSP_HIKEY_HIFI_RUN_SIZE           0x00600000

#define ADSP_HIKEY_OCRAM_BACK_BASE           0x89800000
#define ADSP_HIKEY_OCRAM_BACK_SIZE           0x00030000

#define ADSP_HIKEY_TCM_BACK_BASE           0x89830000
#define ADSP_HIKEY_TCM_BACK_SIZE           0x00034000

#define ADSP_HIKEY_IMG_BACK_BASE           0x89864000
#define ADSP_HIKEY_IMG_BACK_SIZE           0x0031C000

/* mailbox */
#define ADSP_HIKEY_DSP_MAILBOX_SIZE      0x4000


#endif
