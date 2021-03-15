/* Core IA host support for Baytrail audio DSP.
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
#include "hw/acpi/aml-build.h"
#include "hw/audio/adsp-host.h"
#include "hw/adsp/byt.h"
#include "hw/adsp/shim.h"
#include "qemu/io-bridge.h"

#if CONFIG_ADSP_INTEL_HOST_BYT
static void build_acpi_rt5640_device(Aml *table)
{
    Aml *method;
    Aml *scope = aml_scope("_SB");
    Aml *dev = aml_device("RTEK");
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);

    /* device info */
    aml_append(dev, aml_name_decl("_ADR", zero));
    aml_append(dev, aml_name_decl("_HID", aml_string("10EC5640")));
    aml_append(dev, aml_name_decl("_CID", aml_string("10EC5640")));
    aml_append(dev, aml_name_decl("_DDN",
        aml_string("RTEK Codec RT5640")));
    aml_append(dev, aml_name_decl("_UID", one));

    /* _STA method */
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0F)));
    aml_append(dev, method);

    aml_append(scope, dev);
    aml_append(table, scope);
}

static void build_acpi_rt5641_device(Aml *table)
{
    Aml *method;
    Aml *scope = aml_scope("_SB");
    Aml *dev = aml_device("RTEK");
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);

    /* device info */
    aml_append(dev, aml_name_decl("_ADR", zero));
    aml_append(dev, aml_name_decl("_HID", aml_string("10EC5651")));
    aml_append(dev, aml_name_decl("_CID", aml_string("10EC5651")));
    aml_append(dev, aml_name_decl("_DDN",
        aml_string("RTEK Codec RT5641")));
    aml_append(dev, aml_name_decl("_UID", one));

    /* _STA method */
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0F)));
    aml_append(dev, method);

    aml_append(scope, dev);
    aml_append(table, scope);
}

static void build_acpi_byt_device(Aml *table)
{
    Aml *crs;
    Aml *method;
    Aml *scope = aml_scope("_SB");
    Aml *dev = aml_device("LPEA");
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);

    /* device info */
    aml_append(dev, aml_name_decl("_ADR", zero));
    aml_append(dev, aml_name_decl("_HID", aml_string("80860F28")));
    aml_append(dev, aml_name_decl("_CID", aml_string("80860F28")));
    aml_append(dev, aml_name_decl("_SUB", aml_string("80867270")));
    aml_append(dev, aml_name_decl("_DDN",
        aml_string("Intel(R) Low Power Audio Controller - 80860F28")));
    aml_append(dev, aml_name_decl("_UID", one));

    /* _STA method */
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0F)));
    aml_append(dev, method);

    /* resources */
    crs = aml_resource_template();
    aml_append(crs,
    aml_memory32_fixed(ADSP_BYT_MMIO_BASE, ADSP_MMIO_SIZE, AML_READ_ONLY));
    aml_append(crs,
    aml_memory32_fixed(ADSP_BYT_PCI_BASE, ADSP_PCI_SIZE, AML_READ_ONLY));
    aml_append(crs, aml_memory32_fixed(0x55AA55AA, 0x00100000, AML_READ_ONLY));

    /* some BIOSes define 5 different IRQ resources for ADSP ? */
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb)); /* used by upstream driver */
    aml_append(dev, aml_name_decl("_CRS", crs));

    aml_append(scope, dev);
    aml_append(table, scope);
}

/* TODO: check with a CHT BIOS */
static void build_acpi_cht_device(Aml *table)
{
    Aml *crs;
    Aml *method;
    Aml *scope = aml_scope("_SB");
    Aml *dev = aml_device("LPEC");
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);

    /* device info */
    aml_append(dev, aml_name_decl("_ADR", zero));
    aml_append(dev, aml_name_decl("_HID", aml_string("808622A8")));
    aml_append(dev, aml_name_decl("_CID", aml_string("808622A8")));
    aml_append(dev, aml_name_decl("_SUB", aml_string("80867270")));
    aml_append(dev, aml_name_decl("_DDN",
        aml_string("Intel(R) Low Power Audio Controller - 808622A8")));
    aml_append(dev, aml_name_decl("_UID", one));

    /* _STA method */
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0F)));
    aml_append(dev, method);

    /* resources */
    crs = aml_resource_template();
    aml_append(crs,
    aml_memory32_fixed(ADSP_CHT_MMIO_BASE, ADSP_MMIO_SIZE, AML_READ_ONLY));
    aml_append(crs,
    aml_memory32_fixed(ADSP_CHT_PCI_BASE, ADSP_PCI_SIZE, AML_READ_ONLY));
    aml_append(crs, aml_memory32_fixed(0x55AA55AA, 0x00100000, AML_READ_ONLY));

    /* some BIOSes define 5 different IRQ resources for ADSP ? */
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(dev, aml_name_decl("_CRS", crs));

    aml_append(scope, dev);
    aml_append(table, scope);
}
#endif

#if CONFIG_ADSP_INTEL_HOST_HSW
static void build_acpi_rt286_device(Aml *table)
{
    Aml *method;
    Aml *scope = aml_scope("_SB");
    Aml *dev = aml_device("RTE1");
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);

    /* device info */
    aml_append(dev, aml_name_decl("_ADR", zero));
    aml_append(dev, aml_name_decl("_HID", aml_string("INT33CA")));
    aml_append(dev, aml_name_decl("_CID", aml_string("INT33CA")));
    aml_append(dev, aml_name_decl("_DDN",
        aml_string("RTEK Codec Controller")));
    aml_append(dev, aml_name_decl("_UID", one));

    /* _STA method */
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0F)));
    aml_append(dev, method);

    aml_append(scope, dev);
    aml_append(table, scope);
}

static void build_acpi_hsw_device(Aml *table)
{
    Aml *crs;
    Aml *method;
    Aml *scope = aml_scope("_SB");
    Aml *dev = aml_device("LPEB");
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);

    /* device info */
    aml_append(dev, aml_name_decl("_ADR", zero));
    aml_append(dev, aml_name_decl("_HID", aml_string("INT33C8")));
    aml_append(dev, aml_name_decl("_CID", aml_string("INT33C8")));
    aml_append(dev, aml_name_decl("_SUB", aml_string("80867270")));
    aml_append(dev, aml_name_decl("_DDN",
        aml_string("Intel(R) Low Power Audio Controller - INT33C8")));
    aml_append(dev, aml_name_decl("_UID", one));

    /* _STA method */
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0F)));
    aml_append(dev, method);

    /* resources */
    crs = aml_resource_template();
    aml_append(crs,
    aml_memory32_fixed(ADSP_HSW_MMIO_BASE, ADSP_MMIO_SIZE, AML_READ_ONLY));
    aml_append(crs,
    aml_memory32_fixed(ADSP_HSW_PCI_BASE, ADSP_PCI_SIZE, AML_READ_ONLY));
    aml_append(crs, aml_memory32_fixed(0x55AA55AA, 0x00100000, AML_READ_ONLY));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(dev, aml_name_decl("_CRS", crs));

    aml_append(scope, dev);
    aml_append(table, scope);
}

static void build_acpi_bdw_device(Aml *table)
{
    Aml *crs;
    Aml *method;
    Aml *scope = aml_scope("_SB");
    Aml *dev = aml_device("LPED");
    Aml *zero = aml_int(0);
    Aml *one = aml_int(1);

    /* device info */
    aml_append(dev, aml_name_decl("_ADR", zero));
    aml_append(dev, aml_name_decl("_HID", aml_string("INT3438")));
    aml_append(dev, aml_name_decl("_CID", aml_string("INT3438")));
    aml_append(dev, aml_name_decl("_SUB", aml_string("80867270")));
    aml_append(dev, aml_name_decl("_DDN",
        aml_string("Intel(R) Low Power Audio Controller - INT3438")));
    aml_append(dev, aml_name_decl("_UID", one));

    /* _STA method */
    method = aml_method("_STA", 0, AML_NOTSERIALIZED);
    aml_append(method, aml_return(aml_int(0x0F)));
    aml_append(dev, method);

    /* resources */
    crs = aml_resource_template();
    aml_append(crs,
    aml_memory32_fixed(ADSP_BDW_MMIO_BASE, ADSP_MMIO_SIZE, AML_READ_ONLY));
    aml_append(crs,
    aml_memory32_fixed(ADSP_BDW_PCI_BASE, ADSP_PCI_SIZE, AML_READ_ONLY));
    aml_append(crs, aml_memory32_fixed(0x55AA55AA, 0x00100000, AML_READ_ONLY));
    aml_append(crs, aml_irq_no_flags(0xb));
    aml_append(dev, aml_name_decl("_CRS", crs));

    aml_append(scope, dev);
    aml_append(table, scope);
}
#endif

#if CONFIG_ADSP_INTEL_HOST_CAVS
static void build_acpi_apl_rt286_device(Aml *table)
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
#endif

void adsp_create_acpi_devices(Aml *table)
{
#if CONFIG_ADSP_INTEL_HOST_BYT
    build_acpi_byt_device(table);
    build_acpi_rt5640_device(table);
    build_acpi_cht_device(table);
    build_acpi_rt5641_device(table);
#endif
#if CONFIG_ADSP_INTEL_HOST_HSW
    build_acpi_hsw_device(table);
    build_acpi_bdw_device(table);
    build_acpi_rt286_device(table);
#endif
#if CONFIG_ADSP_INTEL_HOST_CAVS
    build_acpi_apl_rt286_device(table);
#endif
}
