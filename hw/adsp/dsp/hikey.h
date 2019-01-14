/* Core DSP SHIM support for Broadwell audio DSP.
 *
 * Copyright (C) 2018 John Gunn
 *
 * Author: John Gunn jgunn0262@gmail.com
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

#ifndef __ADSP_HIKEY_H__
#define __ADSP_HIKEY_H__

/* IRQ numbers - TODO: To be completed and confirmed */
/* IRQ numbers  - lots of external IRQs missing due to lack of docs */
#define IRQ_NUM_EXT_IPC         3       /* Level 3 */
#define IRQ_NUM_SOFTWARE1       1       /* Level 3 */
#define IRQ_NUM_TIMER1          5       /* Level 3 */
#define IRQ_NUM_TIMER2          6       /* Level 4 */
#define IRQ_NUM_PROFILE         19      /* Level 3 */
#define IRQ_NUM_WRITE_ERR       29      /* Level 3 */
#define IRQ_NUM_NMI             0       /* Level 6 */

/* IRQ Masks */
#define IRQ_MASK_EXT_IPC        (1 << IRQ_NUM_EXT_IPC)
#define IRQ_MASK_TIMER1         (1 << IRQ_NUM_TIMER1)
#define IRQ_MASK_SOFTWARE1      (1 << IRQ_NUM_SOFTWARE1)
#define IRQ_MASK_TIMER2         (1 << IRQ_NUM_TIMER2)
#define IRQ_MASK_PROFILE        (1 << IRQ_NUM_PROFILE)
#define IRQ_MASK_WRITE_ERR      (1 << IRQ_NUM_WRITE_ERR)


struct adsp_dev;

#endif
