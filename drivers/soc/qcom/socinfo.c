// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2009-2021, The Linux Foundation. All rights reserved.
 * Copyright (C) 2021 XiaoMi, Inc.
 */

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/export.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sys_soc.h>
#include <linux/slab.h>
#include <linux/stat.h>
#include <linux/string.h>
#include <linux/types.h>

#include <asm/system_misc.h>

#include <soc/qcom/socinfo.h>
#include <linux/soc/qcom/smem.h>
#include <soc/qcom/boot_stats.h>
#include <asm/unaligned.h>

#define BUILD_ID_LENGTH 32
#define CHIP_ID_LENGTH 32
#define SMEM_IMAGE_VERSION_BLOCKS_COUNT 32
#define SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE 128
#define SMEM_IMAGE_VERSION_SIZE 4096
#define SMEM_IMAGE_VERSION_NAME_SIZE 75
#define SMEM_IMAGE_VERSION_VARIANT_SIZE 20
#define SMEM_IMAGE_VERSION_VARIANT_OFFSET 75
#define SMEM_IMAGE_VERSION_OEM_SIZE 33
#define SMEM_IMAGE_VERSION_OEM_OFFSET 95
#define SMEM_IMAGE_VERSION_PARTITION_APPS 10

static DECLARE_RWSEM(current_image_rwsem);
enum {
	HW_PLATFORM_UNKNOWN = 0,
	HW_PLATFORM_SURF    = 1,
	HW_PLATFORM_FFA     = 2,
	HW_PLATFORM_FLUID   = 3,
	HW_PLATFORM_SVLTE_FFA	= 4,
	HW_PLATFORM_SVLTE_SURF	= 5,
	HW_PLATFORM_MTP_MDM = 7,
	HW_PLATFORM_MTP  = 8,
	HW_PLATFORM_LIQUID  = 9,
	/* Dragonboard platform id is assigned as 10 in CDT */
	HW_PLATFORM_DRAGON	= 10,
	HW_PLATFORM_QRD	= 11,
	HW_PLATFORM_HRD	= 13,
	HW_PLATFORM_DTV	= 14,
	HW_PLATFORM_RCM	= 21,
	HW_PLATFORM_STP = 23,
	HW_PLATFORM_SBC = 24,

	HW_PLATFORM_J1  = 36,
	HW_PLATFORM_J11 = 37,
	HW_PLATFORM_J1S = 41,
	HW_PLATFORM_J3S = 42,
	HW_PLATFORM_J2  = 43,
	HW_PLATFORM_K11A = 44,
	HW_PLATFORM_J2S = 45,
	HW_PLATFORM_K81 = 46,
	HW_PLATFORM_K81A = 47,
	HW_PLATFORM_HDK = 31,
	HW_PLATFORM_IDP = 34,
	HW_PLATFORM_INVALID
};

const char *hw_platform[] = {
	[HW_PLATFORM_UNKNOWN] = "Unknown",
	[HW_PLATFORM_SURF] = "Surf",
	[HW_PLATFORM_FFA] = "FFA",
	[HW_PLATFORM_FLUID] = "Fluid",
	[HW_PLATFORM_SVLTE_FFA] = "SVLTE_FFA",
	[HW_PLATFORM_SVLTE_SURF] = "SLVTE_SURF",
	[HW_PLATFORM_MTP_MDM] = "MDM_MTP_NO_DISPLAY",
	[HW_PLATFORM_MTP] = "MTP",
	[HW_PLATFORM_RCM] = "RCM",
	[HW_PLATFORM_LIQUID] = "Liquid",
	[HW_PLATFORM_DRAGON] = "Dragon",
	[HW_PLATFORM_QRD] = "QRD",
	[HW_PLATFORM_HRD] = "HRD",
	[HW_PLATFORM_DTV] = "DTV",
	[HW_PLATFORM_STP] = "STP",
	[HW_PLATFORM_SBC] = "SBC",
	[HW_PLATFORM_J2] = "UMI",
	[HW_PLATFORM_J1] = "CMI",
	[HW_PLATFORM_J11] = "LMI",
	[HW_PLATFORM_J1S] = "CAS",
	[HW_PLATFORM_J3S] = "APOLLO",
	[HW_PLATFORM_K11A] = "ALIOTH",
	[HW_PLATFORM_J2S] = "THYME",
	[HW_PLATFORM_K81] = "ENUMA",
	[HW_PLATFORM_K81A] = "ELISH",
	[HW_PLATFORM_HDK] = "HDK",
	[HW_PLATFORM_IDP] = "IDP"
};

enum {
	ACCESSORY_CHIP_UNKNOWN = 0,
	ACCESSORY_CHIP_CHARM = 58,
};

enum {
	PLATFORM_SUBTYPE_QRD = 0x0,
	PLATFORM_SUBTYPE_SKUAA = 0x1,
	PLATFORM_SUBTYPE_SKUF = 0x2,
	PLATFORM_SUBTYPE_SKUAB = 0x3,
	PLATFORM_SUBTYPE_SKUG = 0x5,
	PLATFORM_SUBTYPE_QRD_INVALID,
};

const char *qrd_hw_platform_subtype[] = {
	[PLATFORM_SUBTYPE_QRD] = "QRD",
	[PLATFORM_SUBTYPE_SKUAA] = "SKUAA",
	[PLATFORM_SUBTYPE_SKUF] = "SKUF",
	[PLATFORM_SUBTYPE_SKUAB] = "SKUAB",
	[PLATFORM_SUBTYPE_SKUG] = "SKUG",
	[PLATFORM_SUBTYPE_QRD_INVALID] = "INVALID",
};

enum {
	PLATFORM_SUBTYPE_UNKNOWN = 0x0,
	PLATFORM_SUBTYPE_CHARM = 0x1,
	PLATFORM_SUBTYPE_STRANGE = 0x2,
	PLATFORM_SUBTYPE_STRANGE_2A = 0x3,
	PLATFORM_SUBTYPE_INVALID,
};

const char *hw_platform_subtype[] = {
	[PLATFORM_SUBTYPE_UNKNOWN] = "Unknown",
	[PLATFORM_SUBTYPE_CHARM] = "charm",
	[PLATFORM_SUBTYPE_STRANGE] = "strange",
	[PLATFORM_SUBTYPE_STRANGE_2A] = "strange_2a",
	[PLATFORM_SUBTYPE_INVALID] = "Invalid",
};

/* Used to parse shared memory.  Must match the modem. */
struct socinfo_v0_1 {
	uint32_t format;
	uint32_t id;
	uint32_t version;
	char build_id[BUILD_ID_LENGTH];
};

struct socinfo_v0_2 {
	struct socinfo_v0_1 v0_1;
	uint32_t raw_id;
	uint32_t raw_version;
};

struct socinfo_v0_3 {
	struct socinfo_v0_2 v0_2;
	uint32_t hw_platform;
};

struct socinfo_v0_4 {
	struct socinfo_v0_3 v0_3;
	uint32_t platform_version;
};

struct socinfo_v0_5 {
	struct socinfo_v0_4 v0_4;
	uint32_t accessory_chip;
};

struct socinfo_v0_6 {
	struct socinfo_v0_5 v0_5;
	uint32_t hw_platform_subtype;
};

struct socinfo_v0_7 {
	struct socinfo_v0_6 v0_6;
	uint32_t pmic_model;
	uint32_t pmic_die_revision;
};

struct socinfo_v0_8 {
	struct socinfo_v0_7 v0_7;
	uint32_t pmic_model_1;
	uint32_t pmic_die_revision_1;
	uint32_t pmic_model_2;
	uint32_t pmic_die_revision_2;
};

struct socinfo_v0_9 {
	struct socinfo_v0_8 v0_8;
	uint32_t foundry_id;
};

struct socinfo_v0_10 {
	struct socinfo_v0_9 v0_9;
	uint32_t serial_number;
};

struct socinfo_v0_11 {
	struct socinfo_v0_10 v0_10;
	uint32_t num_pmics;
	uint32_t pmic_array_offset;
};

struct socinfo_v0_12 {
	struct socinfo_v0_11 v0_11;
	uint32_t chip_family;
	uint32_t raw_device_family;
	uint32_t raw_device_number;
};

struct socinfo_v0_13 {
	struct socinfo_v0_12 v0_12;
	uint32_t nproduct_id;
	char chip_name[CHIP_ID_LENGTH];
};

struct socinfo_v0_14 {
	struct socinfo_v0_13 v0_13;
	uint32_t num_clusters;
	uint32_t ncluster_array_offset;
	uint32_t num_subset_parts;
	uint32_t nsubset_parts_array_offset;
};

struct socinfo_v0_15 {
	struct socinfo_v0_14 v0_14;
	uint32_t nmodem_supported;
};

struct socinfo_v0_16 {
	struct socinfo_v0_15 v0_15;
	__le32  feature_code;
	__le32  pcode;
	__le32  npartnamemap_offset;
	__le32  nnum_partname_mapping;
};

static union {
	struct socinfo_v0_1 v0_1;
	struct socinfo_v0_2 v0_2;
	struct socinfo_v0_3 v0_3;
	struct socinfo_v0_4 v0_4;
	struct socinfo_v0_5 v0_5;
	struct socinfo_v0_6 v0_6;
	struct socinfo_v0_7 v0_7;
	struct socinfo_v0_8 v0_8;
	struct socinfo_v0_9 v0_9;
	struct socinfo_v0_10 v0_10;
	struct socinfo_v0_11 v0_11;
	struct socinfo_v0_12 v0_12;
	struct socinfo_v0_13 v0_13;
	struct socinfo_v0_14 v0_14;
	struct socinfo_v0_15 v0_15;
	struct socinfo_v0_16 v0_16;
} *socinfo;

#define PART_NAME_MAX		32
struct socinfo_partinfo {
	__le32 part_type;
	char part_name[PART_NAME_MAX];
	__le32 part_name_len;
};
struct socinfo_partinfo partinfo[SOCINFO_PART_MAX_PARTTYPE];

/* max socinfo format version supported */
#define MAX_SOCINFO_FORMAT SOCINFO_VERSION(0, 16)

static const char * const hw_platform_feature_code[] = {
	[SOCINFO_FC_UNKNOWN] = "Unknown",
	[SOCINFO_FC_AA] = "AA",
	[SOCINFO_FC_AB] = "AB",
	[SOCINFO_FC_AC] = "AC",
	[SOCINFO_FC_AD] = "AD",
	[SOCINFO_FC_AE] = "AE",
	[SOCINFO_FC_AF] = "AF",
	[SOCINFO_FC_AG] = "AG",
	[SOCINFO_FC_AH] = "AH",
};

#define SOCINFO_FC_INT_MASK 0x0f
static const char * const hw_platform_ifeature_code[] = {
	[SOCINFO_FC_Y0 - SOCINFO_FC_Y0] = "Y0",
	[SOCINFO_FC_Y1 - SOCINFO_FC_Y0] = "Y1",
	[SOCINFO_FC_Y2 - SOCINFO_FC_Y0] = "Y2",
	[SOCINFO_FC_Y3 - SOCINFO_FC_Y0] = "Y3",
	[SOCINFO_FC_Y4 - SOCINFO_FC_Y0] = "Y4",
	[SOCINFO_FC_Y5 - SOCINFO_FC_Y0] = "Y5",
	[SOCINFO_FC_Y6 - SOCINFO_FC_Y0] = "Y6",
	[SOCINFO_FC_Y7 - SOCINFO_FC_Y0] = "Y7",
};

static struct msm_soc_info cpu_of_id[] = {
	[0]  = {MSM_CPU_UNKNOWN, "Unknown CPU"},
	/* 8960 IDs */
	[87] = {MSM_CPU_8960, "MSM8960"},

	/* 8064 IDs */
	[109] = {MSM_CPU_8064, "APQ8064"},
	[122] = {MSM_CPU_8960, "MSM8960"},

	/* 8260A ID */
	[123] = {MSM_CPU_8960, "MSM8960"},

	/* 8060A ID */
	[124] = {MSM_CPU_8960, "MSM8960"},

	/* 8064 MPQ ID */
	[130] = {MSM_CPU_8064, "APQ8064"},

	/* 8974 IDs */
	[126] = {MSM_CPU_8974, "MSM8974"},
	[184] = {MSM_CPU_8974, "MSM8974"},
	[185] = {MSM_CPU_8974, "MSM8974"},
	[186] = {MSM_CPU_8974, "MSM8974"},

	/* 8974AA IDs */
	[208] = {MSM_CPU_8974PRO_AA, "MSM8974PRO-AA"},
	[211] = {MSM_CPU_8974PRO_AA, "MSM8974PRO-AA"},
	[214] = {MSM_CPU_8974PRO_AA, "MSM8974PRO-AA"},
	[217] = {MSM_CPU_8974PRO_AA, "MSM8974PRO-AA"},

	/* 8974AB IDs */
	[209] = {MSM_CPU_8974PRO_AB, "MSM8974PRO-AB"},
	[212] = {MSM_CPU_8974PRO_AB, "MSM8974PRO-AB"},
	[215] = {MSM_CPU_8974PRO_AB, "MSM8974PRO-AB"},
	[218] = {MSM_CPU_8974PRO_AB, "MSM8974PRO-AB"},

	/* 8974AC IDs */
	[194] = {MSM_CPU_8974PRO_AC, "MSM8974PRO-AC"},
	[210] = {MSM_CPU_8974PRO_AC, "MSM8974PRO-AC"},
	[213] = {MSM_CPU_8974PRO_AC, "MSM8974PRO-AC"},
	[216] = {MSM_CPU_8974PRO_AC, "MSM8974PRO-AC"},

	/* 8960AB IDs */
	[138] = {MSM_CPU_8960AB, "MSM8960AB"},
	[139] = {MSM_CPU_8960AB, "MSM8960AB"},
	[140] = {MSM_CPU_8960AB, "MSM8960AB"},
	[141] = {MSM_CPU_8960AB, "MSM8960AB"},

	/* 8084 IDs */
	[178] = {MSM_CPU_8084, "APQ8084"},

	/* 8916 IDs */
	[206] = {MSM_CPU_8916, "MSM8916"},
	[247] = {MSM_CPU_8916, "APQ8016"},
	[248] = {MSM_CPU_8916, "MSM8216"},
	[249] = {MSM_CPU_8916, "MSM8116"},
	[250] = {MSM_CPU_8916, "MSM8616"},

	/* 8996 IDs */
	[246] = {MSM_CPU_8996, "MSM8996"},
	[310] = {MSM_CPU_8996, "MSM8996"},
	[311] = {MSM_CPU_8996, "APQ8096"},
	[291] = {MSM_CPU_8996, "APQ8096"},
	[305] = {MSM_CPU_8996, "MSM8996pro"},
	[312] = {MSM_CPU_8996, "APQ8096pro"},

	/* SDM660 ID */
	[317] = {MSM_CPU_SDM660, "SDM660"},

	/* sm8150 ID */
	[339] = {MSM_CPU_SM8150, "SM8150"},

	/* sa8150 ID */
	[362] = {MSM_CPU_SA8150, "SA8150"},

	/* sdmshrike ID */
	[340] = {MSM_CPU_SDMSHRIKE, "SDMSHRIKE"},

	/* sm6150 ID */
	[355] = {MSM_CPU_SM6150, "SM6150"},

	/* qcs405 ID */
	[352] = {MSM_CPU_QCS405, "QCS405"},

	/* sdxprairie ID */
	[357] = {SDX_CPU_SDXPRAIRIE, "SDXPRAIRIE"},

	/* sdmmagpie ID */
	[365] = {MSM_CPU_SDMMAGPIE, "SDMMAGPIE"},

	/* kona ID */
	[356] = {MSM_CPU_KONA, "SM8250"},
	[455] = {MSM_CPU_KONA, "KONA"},
	[496] = {MSM_CPU_KONA, "KONA"},

	/* kona-7230-iot ID */
	[548] = {MSM_CPU_KONA_IOT, "KONA-7230-IOT"},

	/* Lito ID */
	[400] = {MSM_CPU_LITO, "LITO"},
	[440] = {MSM_CPU_LITO, "LITO"},

	/* Orchid ID */
	[476] = {MSM_CPU_ORCHID, "ORCHID"},

	/* Bengal ID */
	[417] = {MSM_CPU_BENGAL, "BENGAL"},
	[444] = {MSM_CPU_BENGAL, "BENGAL"},

	/* Khaje ID */
	[518] = {MSM_CPU_KHAJE, "KHAJE"},
	[586] = {MSM_CPU_KHAJE, "KHAJE"},

	/* Khajep ID */
	[561] = {MSM_CPU_KHAJEP, "KHAJEP"},

	/* Khajeq ID */
	[562] = {MSM_CPU_KHAJEQ, "KHAJEQ"},

	/* Khajeg ID */
	[585] = {MSM_CPU_KHAJEG, "KAHJEG"},

	/* Lagoon ID */
	[434] = {MSM_CPU_LAGOON, "LAGOON"},
	[459] = {MSM_CPU_LAGOON, "LAGOON"},

	/* Bengalp ID */
	[445] = {MSM_CPU_BENGALP, "BENGALP"},
	[420] = {MSM_CPU_BENGALP, "BENGALP"},

	/* Scuba ID */
	[441] = {MSM_CPU_SCUBA, "SCUBA"},
	[471] = {MSM_CPU_SCUBA, "SCUBA"},

	/* Scuba IIOT  ID */
	[473] = {MSM_CPU_SCUBAIOT, "SCUBAIIOT"},
	[474] = {MSM_CPU_SCUBAPIOT, "SCUBAPIIOT"},

	/* BENGAL-IOT ID */
	[469] = {MSM_CPU_BENGAL_IOT, "BENGAL-IOT"},

	/* BENGALP-IOT ID */
	[470] = {MSM_CPU_BENGALP_IOT, "BENGALP-IOT"},

	/* MSM8937 ID */
	[294] = {MSM_CPU_8937, "MSM8937"},
	[295] = {MSM_CPU_8937, "APQ8937"},

	/* MSM8917 IDs */
	[303] = {MSM_CPU_8917, "MSM8917"},
	[307] = {MSM_CPU_8917, "APQ8017"},
	[308] = {MSM_CPU_8917, "MSM8217"},
	[309] = {MSM_CPU_8917, "MSM8617"},

	/* SDM429 and SDM439 ID */
	[353] = {MSM_CPU_SDM439, "SDM439"},
	[354] = {MSM_CPU_SDM429, "SDM429"},


	/* QM215 ID */
	[386] = {MSM_CPU_QM215, "QM215"},

	/* QCM2150 ID */
	[436] = {MSM_CPU_QCM2150, "QCM2150"},

	/* 8953 ID */
	[293] = {MSM_CPU_8953, "MSM8953"},
	[304] = {MSM_CPU_8953, "APQ8053"},

	/* SDM450 ID */
	[338] = {MSM_CPU_SDM450, "SDM450"},

	/* Uninitialized IDs are not known to run Linux.
	 * MSM_CPU_UNKNOWN is set to 0 to ensure these IDs are
	 * considered as unknown CPU.
	 */
};

static enum msm_cpu cur_cpu;
static int current_image;
static uint32_t socinfo_format;
static const char *sku;

static struct socinfo_v0_1 dummy_socinfo = {
	.format = SOCINFO_VERSION(0, 1),
	.version = 1,
};

uint32_t socinfo_get_id(void)
{
	return (socinfo) ? socinfo->v0_1.id : 0;
}
EXPORT_SYMBOL(socinfo_get_id);

char *socinfo_get_id_string(void)
{
	return (socinfo) ? cpu_of_id[socinfo->v0_1.id].soc_id_string : NULL;
}
EXPORT_SYMBOL(socinfo_get_id_string);

uint32_t socinfo_get_version(void)
{
	return (socinfo) ? socinfo->v0_1.version : 0;
}

char *socinfo_get_build_id(void)
{
	return (socinfo) ? socinfo->v0_1.build_id : NULL;
}

static char *msm_read_hardware_id(void)
{
	static char msm_soc_str[256] = "Qualcomm Technologies, Inc ";
	static bool string_generated;
	int ret = 0;

	if (string_generated)
		return msm_soc_str;
	if (!socinfo)
		goto err_path;
	if (!cpu_of_id[socinfo->v0_1.id].soc_id_string)
		goto err_path;

	ret = strlcat(msm_soc_str, cpu_of_id[socinfo->v0_1.id].soc_id_string,
			sizeof(msm_soc_str));
	if (ret > sizeof(msm_soc_str))
		goto err_path;

	string_generated = true;
	return msm_soc_str;
err_path:
	return "UNKNOWN SOC TYPE";
}

const char * __init arch_read_machine_name(void)
{
	static char msm_machine_name[256] = "Qualcomm Technologies, Inc. ";
	static bool string_generated;
	u32 len = 0;
	const char *name;

	if (string_generated)
		return msm_machine_name;

	len = strlen(msm_machine_name);
	name = of_get_flat_dt_prop(of_get_flat_dt_root(),
				"qcom,msm-name", NULL);
	if (name)
		len += snprintf(msm_machine_name + len,
					sizeof(msm_machine_name) - len,
					"%s", name);
	else
		goto no_prop_path;

	name = of_get_flat_dt_prop(of_get_flat_dt_root(),
				"qcom,pmic-name", NULL);
	if (name) {
		len += snprintf(msm_machine_name + len,
					sizeof(msm_machine_name) - len,
					"%s", " ");
		len += snprintf(msm_machine_name + len,
					sizeof(msm_machine_name) - len,
					"%s", name);
	} else
		goto no_prop_path;

	name = of_flat_dt_get_machine_name();
	if (name) {
		len += snprintf(msm_machine_name + len,
					sizeof(msm_machine_name) - len,
					"%s", " ");
		len += snprintf(msm_machine_name + len,
					sizeof(msm_machine_name) - len,
					"%s", name);
	} else
		goto no_prop_path;

	string_generated = true;
	return msm_machine_name;
no_prop_path:
	return of_flat_dt_get_machine_name();
}

uint32_t socinfo_get_raw_id(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 2) ?
			socinfo->v0_2.raw_id : 0)
		: 0;
}

uint32_t socinfo_get_raw_version(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 2) ?
			socinfo->v0_2.raw_version : 0)
		: 0;
}

uint32_t socinfo_get_platform_type(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 3) ?
			socinfo->v0_3.hw_platform : 0)
		: 0;
}


uint32_t socinfo_get_platform_version(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 4) ?
			socinfo->v0_4.platform_version : 0)
		: 0;
}

/* This information is directly encoded by the machine id */
/* Thus no external callers rely on this information at the moment */
static uint32_t socinfo_get_accessory_chip(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 5) ?
			socinfo->v0_5.accessory_chip : 0)
		: 0;
}

uint32_t socinfo_get_platform_subtype(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 6) ?
			socinfo->v0_6.hw_platform_subtype : 0)
		: 0;
}

static uint32_t socinfo_get_foundry_id(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 9) ?
			socinfo->v0_9.foundry_id : 0)
		: 0;
}

uint32_t socinfo_get_serial_number(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 10) ?
			socinfo->v0_10.serial_number : 0)
		: 0;
}
EXPORT_SYMBOL(socinfo_get_serial_number);

static uint32_t socinfo_get_chip_family(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 12) ?
			socinfo->v0_12.chip_family : 0)
		: 0;
}

static uint32_t socinfo_get_raw_device_family(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 12) ?
			socinfo->v0_12.raw_device_family : 0)
		: 0;
}

static uint32_t socinfo_get_raw_device_number(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 12) ?
			socinfo->v0_12.raw_device_number : 0)
		: 0;
}

static char *socinfo_get_chip_name(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 13) ?
			socinfo->v0_13.chip_name : "N/A")
		: "N/A";
}

static uint32_t socinfo_get_nproduct_id(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 13) ?
			socinfo->v0_13.nproduct_id : 0)
		: 0;
}

static uint32_t socinfo_get_num_clusters(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 14) ?
			socinfo->v0_14.num_clusters : 0)
		: 0;
}

static uint32_t socinfo_get_ncluster_array_offset(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 14) ?
			socinfo->v0_14.ncluster_array_offset : 0)
		: 0;
}

static uint32_t socinfo_get_num_subset_parts(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 14) ?
			socinfo->v0_14.num_subset_parts : 0)
		: 0;
}

static uint32_t socinfo_get_nsubset_parts_array_offset(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 14) ?
			socinfo->v0_14.nsubset_parts_array_offset : 0)
		: 0;
}

static uint32_t socinfo_get_nmodem_supported(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 15) ?
			socinfo->v0_15.nmodem_supported : 0)
		: 0;
}

static uint32_t socinfo_get_feature_code_id(void)
{
	uint32_t fc_id;

	if (!socinfo || socinfo_format < SOCINFO_VERSION(0, 16))
		return SOCINFO_FC_UNKNOWN;

	fc_id = le32_to_cpu(socinfo->v0_16.feature_code);
	if (fc_id <= SOCINFO_FC_UNKNOWN || fc_id >= SOCINFO_FC_INT_RESERVE)
		return SOCINFO_FC_UNKNOWN;

	return fc_id;
}

static const char *socinfo_get_feature_code_mapping(void)
{
	uint32_t id = socinfo_get_feature_code_id();

	if (id > SOCINFO_FC_UNKNOWN && id < SOCINFO_FC_EXT_RESERVE)
		return hw_platform_feature_code[id];
	else if (id >= SOCINFO_FC_Y0 && id < SOCINFO_FC_INT_RESERVE)
		return hw_platform_ifeature_code[id & SOCINFO_FC_INT_MASK];

	return NULL;
}

static uint32_t socinfo_get_pcode_id(void)
{
	uint32_t pcode;

	if (!socinfo || socinfo_format < SOCINFO_VERSION(0, 16))
		return SOCINFO_PCODE_RESERVE;

	pcode = le32_to_cpu(socinfo->v0_16.pcode);
	if (pcode <= SOCINFO_PCODE_UNKNOWN || pcode >= SOCINFO_PCODE_RESERVE)
		return SOCINFO_PCODE_UNKNOWN;

	return pcode;
}

int socinfo_get_feature_code(void)
{
	if (socinfo_format < SOCINFO_VERSION(0, 16)) {
		pr_warn("socinfo: Feature code is not supported by bootloaders\n");
		return -EINVAL;
	}

	return socinfo_get_feature_code_id();
}
EXPORT_SYMBOL(socinfo_get_feature_code);

int socinfo_get_pcode(void)
{
	if (socinfo_format < SOCINFO_VERSION(0, 16)) {
		pr_warn("socinfo: pcode is not supported by bootloaders\n");
		return -EINVAL;
	}

	return socinfo_get_pcode_id();
}
EXPORT_SYMBOL(socinfo_get_pcode);

char *socinfo_get_partinfo_details(unsigned int part_id)
{
	if (socinfo_format < SOCINFO_VERSION(0, 16) ||
			part_id > SOCINFO_PART_MAX_PARTTYPE)
		return NULL;

	return partinfo[part_id].part_name;
}
EXPORT_SYMBOL(socinfo_get_partinfo_details);

void socinfo_enumerate_partinfo_details(void)
{
	unsigned int partinfo_array_offset;
	unsigned int nnum_partname_mapping;
	void *ptr = socinfo;
	int i, part_type, part_name_len;

	if (socinfo_format < SOCINFO_VERSION(0, 16))
		return;

	partinfo_array_offset =
		le32_to_cpu(socinfo->v0_16.npartnamemap_offset);
	nnum_partname_mapping =
		le32_to_cpu(socinfo->v0_16.nnum_partname_mapping);

	if (nnum_partname_mapping >  SOCINFO_PART_MAX_PARTTYPE) {
		pr_warn("socinfo: Mismatch between bootloaders and hlos\n");
		return;
	}

	ptr += partinfo_array_offset;
	for (i = 0; i < nnum_partname_mapping; i++) {
		part_type = get_unaligned_le32(ptr);
		if (part_type > SOCINFO_PART_MAX_PARTTYPE)
			pr_warn("socinfo: part type mismatch\n");

		partinfo[part_type].part_type = part_type;
		ptr += sizeof(u32);
		strscpy(partinfo[part_type].part_name, ptr, PART_NAME_MAX);
		part_name_len = strlen(partinfo[part_type].part_name);
		ptr += PART_NAME_MAX;
		if (part_name_len != get_unaligned_le32(ptr))
			pr_warn("socinfo: part info string length mismatch\n");

		partinfo[part_type].part_name_len = part_name_len;
		ptr += sizeof(u32);
	}
}

enum pmic_model socinfo_get_pmic_model(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 7) ?
			socinfo->v0_7.pmic_model : PMIC_MODEL_UNKNOWN)
		: PMIC_MODEL_UNKNOWN;
}

uint32_t socinfo_get_pmic_die_revision(void)
{
	return socinfo ?
		(socinfo_format >= SOCINFO_VERSION(0, 7) ?
			socinfo->v0_7.pmic_die_revision : 0)
		: 0;
}

static char *socinfo_get_image_version_base_address(void)
{
	size_t size;

	return qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_IMAGE_VERSION_TABLE,
			&size);
}

enum msm_cpu socinfo_get_msm_cpu(void)
{
	return cur_cpu;
}
EXPORT_SYMBOL(socinfo_get_msm_cpu);

static ssize_t
msm_get_vendor(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "Qualcomm Technologies, Inc\n");
}

static ssize_t
msm_get_raw_id(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_raw_id());
}

static ssize_t
msm_get_raw_version(struct device *dev,
		     struct device_attribute *attr,
		     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_raw_version());
}

static ssize_t
msm_get_build_id(struct device *dev,
		   struct device_attribute *attr,
		   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%-.32s\n",
			socinfo_get_build_id());
}

static ssize_t
msm_get_hw_platform(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	uint32_t hw_type;

	hw_type = socinfo_get_platform_type();

	return snprintf(buf, PAGE_SIZE, "%-.32s\n",
			hw_platform[hw_type]);
}

static ssize_t
msm_get_platform_version(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_platform_version());
}

static ssize_t
msm_get_accessory_chip(struct device *dev,
				struct device_attribute *attr,
				char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_accessory_chip());
}

static ssize_t
msm_get_platform_subtype(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	uint32_t hw_subtype;

	hw_subtype = socinfo_get_platform_subtype();
	if (socinfo_get_platform_type() == HW_PLATFORM_QRD) {
		if (hw_subtype >= PLATFORM_SUBTYPE_QRD_INVALID) {
			pr_err("Invalid hardware platform sub type for qrd found\n");
			hw_subtype = PLATFORM_SUBTYPE_QRD_INVALID;
		}
		return snprintf(buf, PAGE_SIZE, "%-.32s\n",
					qrd_hw_platform_subtype[hw_subtype]);
	} else {
		if (hw_subtype >= PLATFORM_SUBTYPE_INVALID) {
			pr_err("Invalid hardware platform subtype\n");
			hw_subtype = PLATFORM_SUBTYPE_INVALID;
		}
		return snprintf(buf, PAGE_SIZE, "%-.32s\n",
			hw_platform_subtype[hw_subtype]);
	}
}

static ssize_t
msm_get_platform_subtype_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	uint32_t hw_subtype;

	hw_subtype = socinfo_get_platform_subtype();
	return snprintf(buf, PAGE_SIZE, "%u\n",
		hw_subtype);
}

static ssize_t
msm_get_foundry_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_foundry_id());
}

static ssize_t
msm_get_serial_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_serial_number());
}

static ssize_t
msm_get_chip_family(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_chip_family());
}

static ssize_t
msm_get_raw_device_family(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_raw_device_family());
}

static ssize_t
msm_get_raw_device_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_raw_device_number());
}

static ssize_t
msm_get_chip_name(struct device *dev,
		   struct device_attribute *attr,
		   char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%-.32s\n",
			socinfo_get_chip_name());
}

static ssize_t
msm_get_nproduct_id(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_nproduct_id());
}

static ssize_t
msm_get_num_clusters(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_num_clusters());
}

static ssize_t
msm_get_ncluster_array_offset(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_ncluster_array_offset());
}

uint32_t
socinfo_get_cluster_info(enum subset_cluster_type cluster)
{
	uint32_t sub_cluster, num_cluster, offset;
	void *cluster_val;
	void *info = socinfo;

	if (cluster >= NUM_CLUSTERS_MAX) {
		pr_err("Bad cluster\n");
		return -EINVAL;
	}

	num_cluster = socinfo_get_num_clusters();
	offset = socinfo_get_ncluster_array_offset();

	if (!num_cluster || !offset)
		return -EINVAL;

	info += offset;
	cluster_val = info + (sizeof(uint32_t) * cluster);
	sub_cluster = get_unaligned_le32(cluster_val);

	return sub_cluster;
}
EXPORT_SYMBOL(socinfo_get_cluster_info);

static ssize_t
msm_get_subset_cores(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	uint32_t sub_cluster = socinfo_get_cluster_info(CLUSTER_CPUSS);

	return scnprintf(buf, PAGE_SIZE, "%x\n", sub_cluster);
}

static ssize_t
msm_get_num_subset_parts(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_num_subset_parts());
}

static ssize_t
msm_get_nsubset_parts_array_offset(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_nsubset_parts_array_offset());
}

static uint32_t
socinfo_get_subset_parts(void)
{
	uint32_t num_parts = socinfo_get_num_subset_parts();
	uint32_t offset = socinfo_get_nsubset_parts_array_offset();
	uint32_t sub_parts = 0;
	void *info = socinfo;
	uint32_t part_entry;
	int i;

	if (!num_parts || !offset)
		return -EINVAL;

	info += offset;
	for (i = 0; i < num_parts; i++) {
		part_entry = get_unaligned_le32(info);
		if (part_entry)
			sub_parts |= BIT(i);
		info += sizeof(uint32_t);
	}

	return sub_parts;
}

bool
socinfo_get_part_info(enum subset_part_type part)
{
	uint32_t partinfo;

	if (part >= NUM_PARTS_MAX) {
		pr_err("Bad part number\n");
		return false;
	}

	partinfo = socinfo_get_subset_parts();
	if (partinfo < 0) {
		pr_err("Failed to get part information\n");
		return false;
	}

	return (partinfo & BIT(part));
}
EXPORT_SYMBOL(socinfo_get_part_info);

static ssize_t
msm_get_subset_parts(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	uint32_t sub_parts = socinfo_get_subset_parts();

	return scnprintf(buf, PAGE_SIZE, "%x\n", sub_parts);
}

static ssize_t
msm_get_nmodem_supported(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "0x%x\n",
		socinfo_get_nmodem_supported());
}

static ssize_t
msm_get_sku(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n",
		sku ? sku : "Unknown");
}

static ssize_t
msm_get_pcode(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "0x%x\n", socinfo_get_pcode_id());
}

static ssize_t
msm_get_feature_code(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	const char *feature_code = socinfo_get_feature_code_mapping();

	return scnprintf(buf, PAGE_SIZE, "%s\n",
		feature_code ? feature_code : "Unknown");
}

static ssize_t
msm_get_pmic_model(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
		socinfo_get_pmic_model());
}

static ssize_t
msm_get_pmic_die_revision(struct device *dev,
			       struct device_attribute *attr,
			       char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n",
			 socinfo_get_pmic_die_revision());
}

static ssize_t
msm_get_image_version(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	char *string_address;

	string_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(string_address)) {
		pr_err("Failed to get image version base address");
		return snprintf(buf, SMEM_IMAGE_VERSION_NAME_SIZE, "Unknown");
	}
	down_read(&current_image_rwsem);
	string_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	up_read(&current_image_rwsem);
	return snprintf(buf, SMEM_IMAGE_VERSION_NAME_SIZE, "%-.75s\n",
			string_address);
}

static ssize_t
msm_set_image_version(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *store_address;

	down_read(&current_image_rwsem);
	if (current_image != SMEM_IMAGE_VERSION_PARTITION_APPS) {
		up_read(&current_image_rwsem);
		return count;
	}
	store_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(store_address)) {
		pr_err("Failed to get image version base address");
		up_read(&current_image_rwsem);
		return count;
	}
	store_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	up_read(&current_image_rwsem);
	snprintf(store_address, SMEM_IMAGE_VERSION_NAME_SIZE, "%-.75s", buf);
	return count;
}

static ssize_t
msm_get_image_variant(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	char *string_address;

	string_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(string_address)) {
		pr_err("Failed to get image version base address");
		return snprintf(buf, SMEM_IMAGE_VERSION_VARIANT_SIZE,
		"Unknown");
	}
	down_read(&current_image_rwsem);
	string_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	up_read(&current_image_rwsem);
	string_address += SMEM_IMAGE_VERSION_VARIANT_OFFSET;
	return snprintf(buf, SMEM_IMAGE_VERSION_VARIANT_SIZE, "%-.20s\n",
			string_address);
}

static ssize_t
msm_set_image_variant(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *store_address;

	down_read(&current_image_rwsem);
	if (current_image != SMEM_IMAGE_VERSION_PARTITION_APPS) {
		up_read(&current_image_rwsem);
		return count;
	}
	store_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(store_address)) {
		pr_err("Failed to get image version base address");
		up_read(&current_image_rwsem);
		return count;
	}
	store_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	up_read(&current_image_rwsem);
	store_address += SMEM_IMAGE_VERSION_VARIANT_OFFSET;
	snprintf(store_address, SMEM_IMAGE_VERSION_VARIANT_SIZE, "%-.20s", buf);
	return count;
}

static ssize_t
msm_get_image_crm_version(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	char *string_address;

	string_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(string_address)) {
		pr_err("Failed to get image version base address");
		return snprintf(buf, SMEM_IMAGE_VERSION_OEM_SIZE, "Unknown");
	}
	down_read(&current_image_rwsem);
	string_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	up_read(&current_image_rwsem);
	string_address += SMEM_IMAGE_VERSION_OEM_OFFSET;
	return snprintf(buf, SMEM_IMAGE_VERSION_OEM_SIZE, "%-.33s\n",
			string_address);
}

static ssize_t
msm_set_image_crm_version(struct device *dev,
			struct device_attribute *attr,
			const char *buf,
			size_t count)
{
	char *store_address;

	down_read(&current_image_rwsem);
	if (current_image != SMEM_IMAGE_VERSION_PARTITION_APPS) {
		up_read(&current_image_rwsem);
		return count;
	}
	store_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(store_address)) {
		pr_err("Failed to get image version base address");
		up_read(&current_image_rwsem);
		return count;
	}
	store_address += current_image * SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	up_read(&current_image_rwsem);
	store_address += SMEM_IMAGE_VERSION_OEM_OFFSET;
	snprintf(store_address, SMEM_IMAGE_VERSION_OEM_SIZE, "%-.33s", buf);
	return count;
}

static ssize_t
msm_get_image_number(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	int ret;

	down_read(&current_image_rwsem);
	ret = snprintf(buf, PAGE_SIZE, "%d\n",
			current_image);
	up_read(&current_image_rwsem);
	return ret;

}

static ssize_t
msm_select_image(struct device *dev, struct device_attribute *attr,
			const char *buf, size_t count)
{
	int ret, digit;

	ret = kstrtoint(buf, 10, &digit);
	if (ret)
		return ret;
	down_write(&current_image_rwsem);
	if (digit >= 0 && digit < SMEM_IMAGE_VERSION_BLOCKS_COUNT)
		current_image = digit;
	else
		current_image = 0;
	up_write(&current_image_rwsem);
	return count;
}

static ssize_t
msm_get_images(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	int pos = 0;
	int image;
	char *image_address;

	image_address = socinfo_get_image_version_base_address();
	if (IS_ERR_OR_NULL(image_address))
		return snprintf(buf, PAGE_SIZE, "Unavailable\n");

	*buf = '\0';
	for (image = 0; image < SMEM_IMAGE_VERSION_BLOCKS_COUNT; image++) {
		if (*image_address == '\0') {
			image_address += SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
			continue;
		}

		pos += snprintf(buf + pos, PAGE_SIZE - pos, "%d:\n",
			image);
		pos += snprintf(buf + pos, PAGE_SIZE - pos,
			"\tCRM:\t\t%-.75s\n", image_address);
		pos += snprintf(buf + pos, PAGE_SIZE - pos,
			"\tVariant:\t%-.20s\n",
			image_address + SMEM_IMAGE_VERSION_VARIANT_OFFSET);
		pos += snprintf(buf + pos, PAGE_SIZE - pos,
			"\tVersion:\t%-.33s\n",
			image_address + SMEM_IMAGE_VERSION_OEM_OFFSET);

		image_address += SMEM_IMAGE_VERSION_SINGLE_BLOCK_SIZE;
	}

	return pos;
}

static struct device_attribute msm_soc_attr_raw_version =
	__ATTR(raw_version, 0444, msm_get_raw_version,  NULL);

static struct device_attribute msm_soc_attr_raw_id =
	__ATTR(raw_id, 0444, msm_get_raw_id,  NULL);

static struct device_attribute msm_soc_attr_vendor =
	__ATTR(vendor, 0444, msm_get_vendor,  NULL);

static struct device_attribute msm_soc_attr_build_id =
	__ATTR(build_id, 0444, msm_get_build_id, NULL);

static struct device_attribute msm_soc_attr_hw_platform =
	__ATTR(hw_platform, 0444, msm_get_hw_platform, NULL);


static struct device_attribute msm_soc_attr_platform_version =
	__ATTR(platform_version, 0444,
			msm_get_platform_version, NULL);

static struct device_attribute msm_soc_attr_accessory_chip =
	__ATTR(accessory_chip, 0444,
			msm_get_accessory_chip, NULL);

static struct device_attribute msm_soc_attr_platform_subtype =
	__ATTR(platform_subtype, 0444,
			msm_get_platform_subtype, NULL);

/* Platform Subtype String is being deprecated. Use Platform
 * Subtype ID instead.
 */
static struct device_attribute msm_soc_attr_platform_subtype_id =
	__ATTR(platform_subtype_id, 0444,
			msm_get_platform_subtype_id, NULL);

static struct device_attribute msm_soc_attr_foundry_id =
	__ATTR(foundry_id, 0444,
			msm_get_foundry_id, NULL);

static struct device_attribute msm_soc_attr_serial_number =
	__ATTR(serial_number, 0444,
			msm_get_serial_number, NULL);

static struct device_attribute msm_soc_attr_chip_family =
	__ATTR(chip_family, 0444,
			msm_get_chip_family, NULL);

static struct device_attribute msm_soc_attr_raw_device_family =
	__ATTR(raw_device_family, 0444,
			msm_get_raw_device_family, NULL);

static struct device_attribute msm_soc_attr_raw_device_number =
	__ATTR(raw_device_number, 0444,
			msm_get_raw_device_number, NULL);

static struct device_attribute msm_soc_attr_chip_name =
	__ATTR(chip_name, 0444,
			msm_get_chip_name, NULL);

static struct device_attribute msm_soc_attr_nproduct_id =
	__ATTR(nproduct_id, 0444,
			msm_get_nproduct_id, NULL);

static struct device_attribute msm_soc_attr_num_clusters =
	__ATTR(num_clusters, 0444,
			msm_get_num_clusters, NULL);

static struct device_attribute msm_soc_attr_ncluster_array_offset =
	__ATTR(ncluster_array_offset, 0444,
			msm_get_ncluster_array_offset, NULL);

static struct device_attribute msm_soc_attr_subset_cores =
	__ATTR(subset_cores, 0444,
			msm_get_subset_cores, NULL);

static struct device_attribute msm_soc_attr_num_subset_parts =
	__ATTR(num_subset_parts, 0444,
			msm_get_num_subset_parts, NULL);

static struct device_attribute msm_soc_attr_nsubset_parts_array_offset =
	__ATTR(nsubset_parts_array_offset, 0444,
			msm_get_nsubset_parts_array_offset, NULL);

static struct device_attribute msm_soc_attr_subset_parts =
	__ATTR(subset_parts, 0444,
			msm_get_subset_parts, NULL);

static struct device_attribute msm_soc_attr_nmodem_supported =
	__ATTR(nmodem_supported, 0444,
			msm_get_nmodem_supported, NULL);

static struct device_attribute msm_soc_attr_sku =
	__ATTR(sku, 0444, msm_get_sku, NULL);

static struct device_attribute msm_soc_attr_feature_code =
	__ATTR(feature_code, 0444, msm_get_feature_code, NULL);

static struct device_attribute msm_soc_attr_pcode =
	__ATTR(pcode, 0444, msm_get_pcode, NULL);

static struct device_attribute msm_soc_attr_pmic_model =
	__ATTR(pmic_model, 0444,
			msm_get_pmic_model, NULL);

static struct device_attribute msm_soc_attr_pmic_die_revision =
	__ATTR(pmic_die_revision, 0444,
			msm_get_pmic_die_revision, NULL);

static struct device_attribute image_version =
	__ATTR(image_version, 0644,
			msm_get_image_version, msm_set_image_version);

static struct device_attribute image_variant =
	__ATTR(image_variant, 0644,
			msm_get_image_variant, msm_set_image_variant);

static struct device_attribute image_crm_version =
	__ATTR(image_crm_version, 0644,
			msm_get_image_crm_version, msm_set_image_crm_version);

static struct device_attribute select_image =
	__ATTR(select_image, 0644,
			msm_get_image_number, msm_select_image);

static struct device_attribute images =
	__ATTR(images, 0444, msm_get_images, NULL);

static void * __init setup_dummy_socinfo(void)
{
	if (early_machine_is_apq8084()) {
		dummy_socinfo.id = 178;
		strlcpy(dummy_socinfo.build_id, "apq8084 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8916()) {
		dummy_socinfo.id = 206;
		strlcpy(dummy_socinfo.build_id, "msm8916 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8996()) {
		dummy_socinfo.id = 246;
		strlcpy(dummy_socinfo.build_id, "msm8996 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8996_auto()) {
		dummy_socinfo.id = 310;
		strlcpy(dummy_socinfo.build_id, "msm8996-auto - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sdm660()) {
		dummy_socinfo.id = 317;
		strlcpy(dummy_socinfo.build_id, "sdm660 - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sm8150()) {
		dummy_socinfo.id = 339;
		strlcpy(dummy_socinfo.build_id, "sm8150 - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sa8150()) {
		dummy_socinfo.id = 362;
		strlcpy(dummy_socinfo.build_id, "sa8150 - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_kona()) {
		dummy_socinfo.id = 356;
		strlcpy(dummy_socinfo.build_id, "kona - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_lito()) {
		dummy_socinfo.id = 400;
		strlcpy(dummy_socinfo.build_id, "lito - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_orchid()) {
		dummy_socinfo.id = 476;
		strlcpy(dummy_socinfo.build_id, "orchid - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_bengal()) {
		dummy_socinfo.id = 417;
		strlcpy(dummy_socinfo.build_id, "bengal - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_khaje()) {
		dummy_socinfo.id = 518;
		strlcpy(dummy_socinfo.build_id, "khaje - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_khajep()) {
		dummy_socinfo.id = 561;
		strlcpy(dummy_socinfo.build_id, "khajep - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_khajeq()) {
		dummy_socinfo.id = 562;
		strlcpy(dummy_socinfo.build_id, "khajeq - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_khajeg()) {
		dummy_socinfo.id = 585;
		strlcpy(dummy_socinfo.build_id, "khajeg - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_bengalp()) {
		dummy_socinfo.id = 445;
		strlcpy(dummy_socinfo.build_id, "bengalp - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_lagoon()) {
		dummy_socinfo.id = 434;
		strlcpy(dummy_socinfo.build_id, "lagoon - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_scuba()) {
		dummy_socinfo.id = 441;
		strlcpy(dummy_socinfo.build_id, "scuba - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_scubaiot()) {
		dummy_socinfo.id = 473;
		strlcpy(dummy_socinfo.build_id, "scubaiot - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_scubapiot()) {
		dummy_socinfo.id = 474;
		strlcpy(dummy_socinfo.build_id, "scubapiot - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sdmshrike()) {
		dummy_socinfo.id = 340;
		strlcpy(dummy_socinfo.build_id, "sdmshrike - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sm6150()) {
		dummy_socinfo.id = 355;
		strlcpy(dummy_socinfo.build_id, "sm6150 - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_qcs405()) {
		dummy_socinfo.id = 352;
		strlcpy(dummy_socinfo.build_id, "qcs405 - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sdxprairie()) {
		dummy_socinfo.id = 357;
		strlcpy(dummy_socinfo.build_id, "sdxprairie - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sdmmagpie()) {
		dummy_socinfo.id = 365;
		strlcpy(dummy_socinfo.build_id, "sdmmagpie - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_bengal_iot()) {
		dummy_socinfo.id = 469;
		strlcpy(dummy_socinfo.build_id, "bengal-iot - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_bengalp_iot()) {
		dummy_socinfo.id = 470;
		strlcpy(dummy_socinfo.build_id, "bengalp-iot - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8937()) {
		dummy_socinfo.id = 294;
		strlcpy(dummy_socinfo.build_id, "msm8937 - ",
		sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8917()) {
		dummy_socinfo.id = 303;
		strlcpy(dummy_socinfo.build_id, "msm8917 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sdm439()) {
		dummy_socinfo.id = 353;
		strlcpy(dummy_socinfo.build_id, "sdm439 - ",
				sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sdm429()) {
		dummy_socinfo.id = 354;
		strlcpy(dummy_socinfo.build_id, "sdm429 - ",
				sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_qm215()) {
		dummy_socinfo.id = 386;
		strlcpy(dummy_socinfo.build_id, "qm215 - ",
				sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_qcm2150()) {
		dummy_socinfo.id = 436;
		strlcpy(dummy_socinfo.build_id, "qcm2150 - ",
				sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_msm8953()) {
		dummy_socinfo.id = 293;
		strlcpy(dummy_socinfo.build_id, "msm8953 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_sdm450()) {
		dummy_socinfo.id = 338;
		strlcpy(dummy_socinfo.build_id, "sdm450 - ",
			sizeof(dummy_socinfo.build_id));
	} else if (early_machine_is_kona_7230_iot()) {
		dummy_socinfo.id = 548;
		strlcpy(dummy_socinfo.build_id, "kona-7230-iot - ",
		sizeof(dummy_socinfo.build_id));
	} else
		strlcat(dummy_socinfo.build_id, "Dummy socinfo",
			sizeof(dummy_socinfo.build_id));

	return (void *) &dummy_socinfo;
}

static void __init populate_soc_sysfs_files(struct device *msm_soc_device)
{
	device_create_file(msm_soc_device, &msm_soc_attr_vendor);
	device_create_file(msm_soc_device, &image_version);
	device_create_file(msm_soc_device, &image_variant);
	device_create_file(msm_soc_device, &image_crm_version);
	device_create_file(msm_soc_device, &select_image);
	device_create_file(msm_soc_device, &images);

	switch (socinfo_format) {
	case SOCINFO_VERSION(0, 16):
		device_create_file(msm_soc_device,
					&msm_soc_attr_sku);
		device_create_file(msm_soc_device,
					&msm_soc_attr_feature_code);
		device_create_file(msm_soc_device,
					&msm_soc_attr_pcode);
	case SOCINFO_VERSION(0, 15):
		device_create_file(msm_soc_device,
					&msm_soc_attr_nmodem_supported);
	case SOCINFO_VERSION(0, 14):
		device_create_file(msm_soc_device,
					&msm_soc_attr_num_clusters);
		device_create_file(msm_soc_device,
					&msm_soc_attr_ncluster_array_offset);
		device_create_file(msm_soc_device,
					&msm_soc_attr_subset_cores);
		device_create_file(msm_soc_device,
					&msm_soc_attr_num_subset_parts);
		device_create_file(msm_soc_device,
				&msm_soc_attr_nsubset_parts_array_offset);
		device_create_file(msm_soc_device,
					&msm_soc_attr_subset_parts);
	case SOCINFO_VERSION(0, 13):
		 device_create_file(msm_soc_device,
					&msm_soc_attr_nproduct_id);
		 device_create_file(msm_soc_device,
					&msm_soc_attr_chip_name);
	case SOCINFO_VERSION(0, 12):
		device_create_file(msm_soc_device,
					&msm_soc_attr_chip_family);
		device_create_file(msm_soc_device,
					&msm_soc_attr_raw_device_family);
		device_create_file(msm_soc_device,
					&msm_soc_attr_raw_device_number);
	case SOCINFO_VERSION(0, 11):
	case SOCINFO_VERSION(0, 10):
		 device_create_file(msm_soc_device,
					&msm_soc_attr_serial_number);
	case SOCINFO_VERSION(0, 9):
		 device_create_file(msm_soc_device,
					&msm_soc_attr_foundry_id);
	case SOCINFO_VERSION(0, 8):
	case SOCINFO_VERSION(0, 7):
		device_create_file(msm_soc_device,
					&msm_soc_attr_pmic_model);
		device_create_file(msm_soc_device,
					&msm_soc_attr_pmic_die_revision);
	case SOCINFO_VERSION(0, 6):
		device_create_file(msm_soc_device,
					&msm_soc_attr_platform_subtype);
		device_create_file(msm_soc_device,
					&msm_soc_attr_platform_subtype_id);
	case SOCINFO_VERSION(0, 5):
		device_create_file(msm_soc_device,
					&msm_soc_attr_accessory_chip);
	case SOCINFO_VERSION(0, 4):
		device_create_file(msm_soc_device,
					&msm_soc_attr_platform_version);
	case SOCINFO_VERSION(0, 3):
		device_create_file(msm_soc_device,
					&msm_soc_attr_hw_platform);
	case SOCINFO_VERSION(0, 2):
		device_create_file(msm_soc_device,
					&msm_soc_attr_raw_id);
		device_create_file(msm_soc_device,
					&msm_soc_attr_raw_version);
	case SOCINFO_VERSION(0, 1):
		device_create_file(msm_soc_device,
					&msm_soc_attr_build_id);
		break;
	default:
		pr_err("Unknown socinfo format: v%u.%u\n",
				SOCINFO_VERSION_MAJOR(socinfo_format),
				SOCINFO_VERSION_MINOR(socinfo_format));
		break;
	}

}

static void  __init soc_info_populate(struct soc_device_attribute *soc_dev_attr)
{
	uint32_t soc_version = socinfo_get_version();

	soc_dev_attr->soc_id   = kasprintf(GFP_KERNEL, "%d", socinfo_get_id());
	soc_dev_attr->family  =  "Snapdragon";
	soc_dev_attr->machine  = socinfo_get_id_string();
	soc_dev_attr->revision = kasprintf(GFP_KERNEL, "%u.%u",
			SOCINFO_VERSION_MAJOR(soc_version),
			SOCINFO_VERSION_MINOR(soc_version));
	return;

}

static int __init socinfo_init_sysfs(void)
{
	struct device *msm_soc_device;
	struct soc_device *soc_dev;
	struct soc_device_attribute *soc_dev_attr;

	if (!socinfo) {
		pr_err("No socinfo found!\n");
		return -ENODEV;
	}

	soc_dev_attr = kzalloc(sizeof(*soc_dev_attr), GFP_KERNEL);
	if (!soc_dev_attr)
		return -ENOMEM;

	soc_info_populate(soc_dev_attr);
	soc_dev = soc_device_register(soc_dev_attr);
	if (IS_ERR_OR_NULL(soc_dev)) {
		kfree(soc_dev_attr);
		pr_err("Soc device register failed\n");
		return -EIO;
	}

	msm_soc_device = soc_device_to_device(soc_dev);
	populate_soc_sysfs_files(msm_soc_device);
	return 0;
}

late_initcall(socinfo_init_sysfs);

static void socinfo_print(void)
{
	uint32_t f_maj = SOCINFO_VERSION_MAJOR(socinfo_format);
	uint32_t f_min = SOCINFO_VERSION_MINOR(socinfo_format);
	uint32_t v_maj = SOCINFO_VERSION_MAJOR(socinfo->v0_1.version);
	uint32_t v_min = SOCINFO_VERSION_MINOR(socinfo->v0_1.version);

	switch (socinfo_format) {
	case SOCINFO_VERSION(0, 1):
		pr_info("v%u.%u, id=%u, ver=%u.%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min);
		break;
	case SOCINFO_VERSION(0, 2):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version);
		break;
	case SOCINFO_VERSION(0, 3):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform);
		break;
	case SOCINFO_VERSION(0, 4):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version);
		break;
	case SOCINFO_VERSION(0, 5):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip);
		break;
	case SOCINFO_VERSION(0, 6):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u hw_plat_subtype=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype);
		break;
	case SOCINFO_VERSION(0, 7):
	case SOCINFO_VERSION(0, 8):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision);
		break;
	case SOCINFO_VERSION(0, 9):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id);
		break;
	case SOCINFO_VERSION(0, 10):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u serial_number=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number);
		break;
	case SOCINFO_VERSION(0, 11):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u serial_number=%u num_pmics=%u\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number,
			socinfo->v0_11.num_pmics);
		break;
	case SOCINFO_VERSION(0, 12):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u serial_number=%u num_pmics=%u chip_family=0x%x raw_device_family=0x%x raw_device_number=0x%x\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number,
			socinfo->v0_11.num_pmics,
			socinfo->v0_12.chip_family,
			socinfo->v0_12.raw_device_family,
			socinfo->v0_12.raw_device_number);
		break;
	case SOCINFO_VERSION(0, 13):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u serial_number=%u num_pmics=%u chip_family=0x%x raw_device_family=0x%x raw_device_number=0x%x nproduct_id=0x%x\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number,
			socinfo->v0_11.num_pmics,
			socinfo->v0_12.chip_family,
			socinfo->v0_12.raw_device_family,
			socinfo->v0_12.raw_device_number,
			socinfo->v0_13.nproduct_id);
		break;

	case SOCINFO_VERSION(0, 14):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u serial_number=%u num_pmics=%u chip_family=0x%x raw_device_family=0x%x raw_device_number=0x%x nproduct_id=0x%x num_clusters=0x%x ncluster_array_offset=0x%x num_subset_parts=0x%x nsubset_parts_array_offset=0x%x\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number,
			socinfo->v0_11.num_pmics,
			socinfo->v0_12.chip_family,
			socinfo->v0_12.raw_device_family,
			socinfo->v0_12.raw_device_number,
			socinfo->v0_13.nproduct_id,
			socinfo->v0_14.num_clusters,
			socinfo->v0_14.ncluster_array_offset,
			socinfo->v0_14.num_subset_parts,
			socinfo->v0_14.nsubset_parts_array_offset);
		break;

	case SOCINFO_VERSION(0, 15):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u serial_number=%u num_pmics=%u chip_family=0x%x raw_device_family=0x%x raw_device_number=0x%x nproduct_id=0x%x num_clusters=0x%x ncluster_array_offset=0x%x num_subset_parts=0x%x nsubset_parts_array_offset=0x%x nmodem_supported=0x%x\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number,
			socinfo->v0_11.num_pmics,
			socinfo->v0_12.chip_family,
			socinfo->v0_12.raw_device_family,
			socinfo->v0_12.raw_device_number,
			socinfo->v0_13.nproduct_id,
			socinfo->v0_14.num_clusters,
			socinfo->v0_14.ncluster_array_offset,
			socinfo->v0_14.num_subset_parts,
			socinfo->v0_14.nsubset_parts_array_offset,
			socinfo->v0_15.nmodem_supported);
		break;

	case SOCINFO_VERSION(0, 16):
		pr_info("v%u.%u, id=%u, ver=%u.%u, raw_id=%u, raw_ver=%u, hw_plat=%u, hw_plat_ver=%u\n accessory_chip=%u, hw_plat_subtype=%u, pmic_model=%u, pmic_die_revision=%u foundry_id=%u serial_number=%u num_pmics=%u chip_family=0x%x raw_device_family=0x%x raw_device_number=0x%x nproduct_id=0x%x num_clusters=0x%x ncluster_array_offset=0x%x num_subset_parts=0x%x nsubset_parts_array_offset=0x%x nmodem_supported=0x%x feature_code=0x%x pcode=0x%x sku=%s\n",
			f_maj, f_min, socinfo->v0_1.id, v_maj, v_min,
			socinfo->v0_2.raw_id, socinfo->v0_2.raw_version,
			socinfo->v0_3.hw_platform,
			socinfo->v0_4.platform_version,
			socinfo->v0_5.accessory_chip,
			socinfo->v0_6.hw_platform_subtype,
			socinfo->v0_7.pmic_model,
			socinfo->v0_7.pmic_die_revision,
			socinfo->v0_9.foundry_id,
			socinfo->v0_10.serial_number,
			socinfo->v0_11.num_pmics,
			socinfo->v0_12.chip_family,
			socinfo->v0_12.raw_device_family,
			socinfo->v0_12.raw_device_number,
			socinfo->v0_13.nproduct_id,
			socinfo->v0_14.num_clusters,
			socinfo->v0_14.ncluster_array_offset,
			socinfo->v0_14.num_subset_parts,
			socinfo->v0_14.nsubset_parts_array_offset,
			socinfo->v0_15.nmodem_supported,
			socinfo->v0_16.feature_code,
			socinfo->v0_16.pcode,
			sku ? sku : "Unknown");
		break;

	default:
		pr_err("Unknown format found: v%u.%u\n", f_maj, f_min);
		break;
	}
}

static void __init socinfo_select_format(void)
{
	uint32_t f_maj = SOCINFO_VERSION_MAJOR(socinfo->v0_1.format);
	uint32_t f_min = SOCINFO_VERSION_MINOR(socinfo->v0_1.format);

	if (f_maj != 0) {
		pr_err("Unsupported format v%u.%u. Falling back to dummy values.\n",
			f_maj, f_min);
		socinfo = setup_dummy_socinfo();
	}

	if (socinfo->v0_1.format > MAX_SOCINFO_FORMAT) {
		pr_warn("Unsupported format v%u.%u. Falling back to v%u.%u.\n",
			f_maj, f_min, SOCINFO_VERSION_MAJOR(MAX_SOCINFO_FORMAT),
			SOCINFO_VERSION_MINOR(MAX_SOCINFO_FORMAT));
		socinfo_format = MAX_SOCINFO_FORMAT;
	} else {
		socinfo_format = socinfo->v0_1.format;
	}
}

const char *product_name_get(void)
{
	char *product_name = NULL;
	size_t size;
	uint32_t hw_type;

	hw_type = socinfo_get_platform_type();

	product_name = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_ID_VENDOR1, &size);
	if (IS_ERR_OR_NULL(product_name)) {
		pr_warn("Can't find SMEM_ID_VENDOR1; falling back on dummy values.\n");
		return hw_platform[hw_type];
	}

	return product_name;
}

EXPORT_SYMBOL(product_name_get);

uint32_t get_hw_country_version(void)
{
	uint32_t version = socinfo_get_platform_version();
	return (version & HW_COUNTRY_VERSION_MASK) >> HW_COUNTRY_VERSION_SHIFT;
}

EXPORT_SYMBOL(get_hw_country_version);

uint32_t get_hw_version_platform(void)
{
	uint32_t hw_type = socinfo_get_platform_type();
	if (hw_type == HW_PLATFORM_J2)
		return HARDWARE_PLATFORM_UMI;
	if (hw_type == HW_PLATFORM_J1)
		return HARDWARE_PLATFORM_CMI;
	if (hw_type == HW_PLATFORM_J11)
		return HARDWARE_PLATFORM_LMI;
	if (hw_type == HW_PLATFORM_J1S)
		return HARDWARE_PLATFORM_CAS;
	if (hw_type == HW_PLATFORM_J3S)
		return HARDWARE_PLATFORM_APOLLO;
	if (hw_type == HW_PLATFORM_K11A)
		return HARDWARE_PLATFORM_ALIOTH;
	if (hw_type == HW_PLATFORM_K81)
		return HARDWARE_PLATFORM_ENUMA;
	if (hw_type == HW_PLATFORM_K81A)
		return HARDWARE_PLATFORM_ELISH;
        if (hw_type == HW_PLATFORM_J2S)
                return HARDWARE_PLATFORM_THYME;
	else
		return HARDWARE_PLATFORM_UNKNOWN;
}
EXPORT_SYMBOL(get_hw_version_platform);

uint32_t get_hw_version_major(void)
{
	uint32_t version = socinfo_get_platform_version();
	return (version & HW_MAJOR_VERSION_MASK) >> HW_MAJOR_VERSION_SHIFT;
}
EXPORT_SYMBOL(get_hw_version_major);

uint32_t get_hw_version_minor(void)
{
	uint32_t version = socinfo_get_platform_version();
	return (version & HW_MINOR_VERSION_MASK) >> HW_MINOR_VERSION_SHIFT;
}
EXPORT_SYMBOL(get_hw_version_minor);

uint32_t get_hw_version_build(void)
{
	uint32_t version = socinfo_get_platform_version();
	return (version & HW_BUILD_VERSION_MASK) >> HW_BUILD_VERSION_SHIFT;
}
EXPORT_SYMBOL(get_hw_version_build);

int __init socinfo_init(void)
{
	static bool socinfo_init_done;
	size_t size;
	uint32_t soc_info_id;
	const char *machine, *fc;

	if (socinfo_init_done)
		return 0;

	socinfo = qcom_smem_get(QCOM_SMEM_HOST_ANY, SMEM_HW_SW_BUILD_ID, &size);
	if (IS_ERR_OR_NULL(socinfo)) {
		pr_warn("Can't find SMEM_HW_SW_BUILD_ID; falling back on dummy values.\n");
		socinfo = setup_dummy_socinfo();
	}

	socinfo_select_format();

	WARN(!socinfo_get_id(), "Unknown SOC ID!\n");

	soc_info_id = socinfo_get_id();
	if ((soc_info_id >= ARRAY_SIZE(cpu_of_id)) ||
			(!cpu_of_id[soc_info_id].soc_id_string))
		pr_warn("New IDs added! ID => CPU mapping needs an update.\n");

	if (socinfo_format >= SOCINFO_VERSION(0, 16)) {
		socinfo_enumerate_partinfo_details();
		machine = socinfo_get_id_string();
		fc = socinfo_get_feature_code_mapping();
		sku = kasprintf(GFP_KERNEL, "%s-%u-%s",
			machine, socinfo_get_pcode(), fc);
	}

	cur_cpu = cpu_of_id[socinfo->v0_1.id].generic_soc_type;
	boot_stats_init();
	socinfo_print();
	arch_read_hardware_id = msm_read_hardware_id;
	socinfo_init_done = true;

	return 0;
}
subsys_initcall(socinfo_init);
