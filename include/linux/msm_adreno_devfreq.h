/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2016-2020, The Linux Foundation. All rights reserved.
 */

#ifndef MSM_ADRENO_DEVFREQ_H
#define MSM_ADRENO_DEVFREQ_H

#include <linux/devfreq.h>
#include <linux/notifier.h>

#define ADRENO_DEVFREQ_NOTIFY_SUBMIT	1
#define ADRENO_DEVFREQ_NOTIFY_RETIRE	2
#define ADRENO_DEVFREQ_NOTIFY_IDLE	3

#define DEVFREQ_FLAG_WAKEUP_MAXFREQ	0x2
/* Flags used to send bus modifier hint from busmon governer to driver */
#define BUSMON_FLAG_FAST_HINT		BIT(0)
#define BUSMON_FLAG_SUPER_FAST_HINT	BIT(1)
#define BUSMON_FLAG_SLOW_HINT		BIT(2)

struct device;

int kgsl_devfreq_add_notifier(struct device *device,
	struct notifier_block *block);

int kgsl_devfreq_del_notifier(struct device *device,
	struct notifier_block *block);

/* same as KGSL_MAX_PWRLEVELS */
#define MSM_ADRENO_MAX_PWRLEVELS 10

struct xstats {
	u64 ram_time;
	u64 ram_wait;
	int mod;
	unsigned long gpu_minfreq;
};

struct devfreq_msm_adreno_tz_data {
	struct notifier_block nb;
	struct {
		s64 total_time;
		s64 busy_time;
		u32 ctxt_aware_target_pwrlevel;
		u32 ctxt_aware_busy_penalty;
	} bin;
	struct {
		u64 total_time;
		u64 ram_time;
		u64 ram_wait;
		u64 gpu_time;
		u32 num;
		u32 max;
		u32 width;
		u32 *up;
		u32 *down;
		s32 *p_up;
		s32 *p_down;
		unsigned int *index;
		uint64_t *ib;
		bool floating;
	} bus;
	unsigned int device_id;
	bool is_64;
	bool disable_busy_time_burst;
	bool ctxt_aware_enable;
};

struct msm_adreno_extended_profile {
	struct devfreq_msm_adreno_tz_data *private_data;
	struct devfreq *bus_devfreq;
	struct workqueue_struct *partner_wq;
	struct work_struct partner_start_event_ws;
	struct work_struct partner_stop_event_ws;
	struct work_struct partner_suspend_event_ws;
	struct work_struct partner_resume_event_ws;
	struct devfreq_dev_profile profile;
};

struct msm_busmon_extended_profile {
	u32 flag;
	u32 sampling_ms;
	unsigned long percent_ab;
	unsigned long ab_mbytes;
	struct devfreq_msm_adreno_tz_data *private_data;
	struct devfreq_dev_profile profile;
};

typedef void(*getbw_func)(unsigned long *, unsigned long *, void *);

int devfreq_vbif_update_bw(void);
void devfreq_vbif_register_callback(getbw_func func, void *data);

#endif
