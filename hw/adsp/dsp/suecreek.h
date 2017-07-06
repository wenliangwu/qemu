
#ifndef __MANIFEST_SUE_H__
#define __MANIFEST_SUE_H__

#include <stdint.h>
#include "css.h"
#include "cse.h"
#include "plat_auth.h"

#define MAN_PAGE_SIZE	4096

/* start offset for base FW module */
#define FILE_TEXT_OFFSET		0x6000

/*
 * CSE values for BXT
 */
#define MAN_CSE_PARTS			3

/*
 * CSS values for SUE
 */
#define MAN_CSS_MOD_TYPE		6
#define MAN_CSS_HDR_SIZE		\
	(sizeof(struct css_header) + 2 * MAN_CSS_MOD_SIZE + MAN_CSS_EXP_SIZE)
#define MAN_CSS_HDR_VERSION		0x10000
#define MAN_CSS_MOD_VENDOR		0x8086

#define MAN_CSS_KEY_SIZE		(MAN_RSA_KEY_MODULUS_LEN >> 2)
#define MAN_CSS_MOD_SIZE		(MAN_RSA_KEY_MODULUS_LEN >> 2)
#define MAN_CSS_EXP_SIZE		(MAN_RSA_KEY_EXPONENT_LEN >> 2)
#define MAN_CSS_MAN_SIZE		\
	(sizeof(struct fw_image_manifest) >> 2)

/*
 * Manifest values for BXT
 */

#define MAN_DESC_OFFSET			0x2000

/* start offset for base FW module */
#define MAN_MOD_OFFSET		0x2000
#define MAN_EXT_PADDING			0x20
#define MAN_CSE_PADDING_SIZE		0x30

#define MAN_CSE_HDR_OFFSET	0

#define MAN_CSS_HDR_OFFSET \
	(MAN_CSE_HDR_OFFSET + \
	sizeof(struct CsePartitionDirHeader) + \
	MAN_CSE_PARTS * sizeof(struct CsePartitionDirEntry))

#define MAN_SIG_PKG_OFFSET \
	(MAN_CSS_HDR_OFFSET + \
	sizeof(struct css_header))

#define MAN_PART_INFO_OFFSET \
	(MAN_SIG_PKG_OFFSET + \
	sizeof(struct signed_pkg_info_ext))

#define MAN_META_EXT_OFFSET \
	(MAN_SIG_PKG_OFFSET + \
	sizeof(struct signed_pkg_info_ext) + \
	sizeof(struct partition_info_ext) + \
	MAN_CSE_PADDING_SIZE)

#define MAN_FW_DESC_OFFSET \
	(MAN_META_EXT_OFFSET + \
	sizeof(struct adsp_meta_file_ext) + \
	MAN_EXT_PADDING)

#define MAN_DESC_PADDING_SIZE	\
	(MAN_DESC_OFFSET - MAN_FW_DESC_OFFSET)

/*
 * FW modules.
 *
 */

/* module type load type */
#define MAN_MOD_TYPE_LOAD_BUILTIN	0
#define MAN_MOD_TYPE_LOAD_MODULE 	1

struct module_type {
	uint32_t load_type:4;	/* MAN_MOD_TYPE_LOAD_ */
	uint32_t auto_start:1;
	uint32_t domain_ll:1;
	uint32_t domain_dp:1;
	uint32_t rsvd_:25;
};

/* segment flags.type */
#define MAN_SEGMENT_TEXT		0
#define MAN_SEGMENT_RODATA		1
#define MAN_SEGMENT_BSS 		2

union segment_flags
{
	uint32_t ul;
	struct {
		uint32_t contents:1;
		uint32_t alloc:1;
		uint32_t load:1;
		uint32_t readonly:1;
		uint32_t code:1;
		uint32_t data:1;
		uint32_t _rsvd0:2;
		uint32_t type:4;	/* MAN_SEGMENT_ */
		uint32_t _rsvd1:4;
		uint32_t length:16;	/* of segment in pages */
	} r;
} __attribute__((packed));

/*
 * Module segment descriptor.
 */
struct segment_desc {
	union segment_flags flags;
	uint32_t v_base_addr;
	uint32_t file_offset;
} __attribute__((packed));

/*
 * The firmware binary can be split into several modules. We will only support
 * one module (the base firmware) at the moment.
 */

#define MAN_MODULE_NAME_LEN		 8
#define MAN_MODULE_SHA256_LEN		32

#define MAN_MODULE_ID			{'$','A','M','E'}

#define MAN_MODULE_BASE_NAME		"BASEFW"
#define MAN_MODULE_BASE_UUID		\
	{0x383b9be2, 0x4db03518, 0x47b19188, 0xf814890a}
#define MAN_MODULE_BASE_TYPE		0x21
#define MAN_MODULE_BASE_ENTRY		0xbe00a000
#define MAN_MODULE_BASE_CFG_OFFSET	0x0
#define MAN_MODULE_BASE_CFG_COUNT	0x0
#define MAN_MODULE_BASE_AFFINITY	0x3	/* all 4 cores */
#define MAN_MODULE_BASE_INST_COUNT	0x1
#define MAN_MODULE_BASE_INST_BSS	0x23

/*
 * Each module has an entry in the FW header.
 */
struct module {
	uint8_t struct_id[4];			/* MAN_MODULE_ID */
	uint8_t name[MAN_MODULE_NAME_LEN];
	uint32_t uuid[4];
	struct module_type type;
	uint8_t hash[MAN_MODULE_SHA256_LEN];
	uint32_t entry_point;
	uint16_t cfg_offset;
	uint16_t cfg_count;
	uint32_t affinity_mask;
	uint16_t instance_max_count;	/* max number of instances */
	uint16_t instance_bss_size;	/* instance (pages) */
	struct segment_desc segment[3];
} __attribute__((packed));

/*
 * Each module has a configuration in the FW header.
 */
struct mod_config {
	uint32_t par[4];	/* module parameters */
	uint32_t is_pages;	/* actual size of instance .bss (pages) */
	uint32_t cps;		/* cycles per second */
	uint32_t ibs;		/* input buffer size (bytes) */
	uint32_t obs;		/* output buffer size (bytes) */
	uint32_t module_flags;	/* flags, reserved for future use */
	uint32_t cpc;		/* cycles per single run */
	uint32_t obls;		/* output block size, reserved for future use */
} __attribute__((packed));

#define MAN_FW_HDR_FW_NAME_LEN		8

#define MAN_FW_HDR_ID			{'$','A','M','1'}
#define MAN_FW_HDR_NAME			"ADSPFW"
#define MAN_FW_HDR_FLAGS		0x0
#define MAN_FW_HDR_FEATURES		0x1f

/* hard coded atm - will pass this in from cmd line and git */
#define MAN_FW_HDR_VERSION_MAJOR	0xa
#define MAN_FW_HDR_VERSION_MINOR	0x246e
#define MAN_FW_HDR_VERSION_HOTFIX	0x0
#define MAN_FW_HDR_VERSION_BUILD	0x940

#define MAN_FW_HDR_HW_BUF_ADDR		0x0
#define MAN_FW_HDR_HW_BUF_SIZE		0x0

/*
 * The firmware has a standard header that is checked by the ROM on firmware
 * loading.  Members from header_id to num_module entries must be unchanged
 * to be backward compatible with SPT-LP ROM.
 *
 * preload_page_count is used by DMA code loader and is entire image size on
 * SUE. i.e. SUE: total size of the binary’s .text and .rodata
 */
struct adsp_fw_header {
	/* ROM immutable start */
	uint8_t header_id[4];
	uint32_t header_len; /* sizeof(this) in bytes (0x34) */
	uint8_t name[MAN_FW_HDR_FW_NAME_LEN];
	uint32_t preload_page_count; /* number of pages of preloaded image 0x39 */
	uint32_t fw_image_flags;
	uint32_t feature_mask;
	uint16_t major_version;
	uint16_t minor_version;
	uint16_t hotfix_version;
	uint16_t build_version;
	uint32_t num_module_entries;
	/* ROM immutable end - members after this point may be modified */
	uint32_t hw_buf_base_addr;
	uint32_t hw_buf_length;
	/* load_addr required for cAVS 1.5/Broxton and loadable libraries 0x0 */
	uint32_t load_addr;
} __attribute__((packed));

/* just base module atm */
#define MAN_SUE_NUM_MODULES		1

struct adsp_fw_desc {
	struct adsp_fw_header header;
	struct module module[MAN_SUE_NUM_MODULES];
	struct mod_config config[MAN_SUE_NUM_MODULES];
} __attribute__((packed));

struct component_desc {
	uint32_t reserved[2]; //all 0
	uint32_t version;
	//sha2 of entire partition minus manifest
	uint8_t hash[MAN_MODULE_SHA256_LEN];
	uint32_t base_offset; //base offset of IMR contents
	/* Limit offset address of the IMR contents.
	Set to base_offset + total binary size (part that is loaded into IMR 
	and hashed into hash field, meaning the Extended Manifest is not included). */
	uint32_t limit_offset; //limit offset of IMR contents
	uint32_t attributes[4]; //aDSP attributes of component
} __attribute__((packed));

struct adsp_meta_file_ext {
	uint32_t ext_type; /* always 17 for ADSP extension */
	/* ext_len Equals 32+64*n for this version where n=1 for ADSP. */
	uint32_t ext_len; //28 + 68 * n, n is module number
	uint32_t imr_type; // 1,2 -reserved for iUnit; 3-aDSP base image; 4-aDSP ext image
	uint8_t reserved[16]; //all 0
	struct component_desc comp_desc[MAN_SUE_NUM_MODULES];
} __attribute__((packed));

struct fw_image_manifest {
	struct adsp_fw_desc desc; // at offset 0x284
} __attribute__((packed));

#endif
