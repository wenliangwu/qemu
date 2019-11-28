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
#include "hw/acpi/aml-build.h"
#include "hw/qdev-properties.h"
#include "hw/audio/adsp-host.h"
#include "hw/adsp/hsw.h"

static void build_acpi_rt286_device(Aml *table)
{
    Aml *method;
    Aml *scope = aml_scope("_SB");
    Aml *dev = aml_device("RTK2");
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);

    /* device info */
    aml_append(dev, aml_name_decl("_ADR", zero));
    aml_append(dev, aml_name_decl("_HID", aml_string("INT343A")));
    aml_append(dev, aml_name_decl("_CID", aml_string("INT343A")));
    aml_append(dev, aml_name_decl("_DDN",
        aml_string("RTEK Codec RT286")));
    aml_append(dev, aml_name_decl("_UID", one));

    /* _STA method */
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0F)));
    aml_append(dev, method);

    aml_append(scope, dev);
    aml_append(table, scope);
}

void build_acpi_cavs_devices(Aml *table)
{
	build_acpi_rt286_device(table);
}
