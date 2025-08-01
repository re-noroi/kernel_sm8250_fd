// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2014-2020, The Linux Foundation. All rights reserved.
 */

#define pr_fmt(fmt)	"core_ctl: " fmt

#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/cpufreq.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/syscore_ops.h>
#include <uapi/linux/sched/types.h>
#include <linux/sched/core_ctl.h>

#include <trace/events/sched.h>
#include "sched.h"

struct cluster_data {
	bool inited;
	unsigned int min_cpus;
	unsigned int max_cpus;
	unsigned int offline_delay_ms;
	unsigned int busy_up_thres[MAX_CPUS_PER_CLUSTER];
	unsigned int busy_down_thres[MAX_CPUS_PER_CLUSTER];
	unsigned int active_cpus;
	unsigned int num_cpus;
	unsigned int nr_isolated_cpus;
	unsigned int nr_not_preferred_cpus;
	cpumask_t cpu_mask;
	unsigned int need_cpus;
	unsigned int task_thres;
	unsigned int max_nr;
	unsigned int nr_prev_assist;
	unsigned int nr_prev_assist_thresh;
	s64 need_ts;
	struct list_head lru;
	bool pending;
	spinlock_t pending_lock;
	bool enable;
	int nrrun;
	struct task_struct *core_ctl_thread;
	unsigned int first_cpu;
	unsigned int boost;
	struct kobject kobj;
	unsigned int strict_nrrun;
};

struct cpu_data {
	bool is_busy;
	unsigned int busy;
	unsigned int cpu;
	bool not_preferred;
	struct cluster_data *cluster;
	struct list_head sib;
	bool isolated_by_us;
};

static DEFINE_PER_CPU(struct cpu_data, cpu_state);
static struct cluster_data cluster_state[MAX_CLUSTERS];
static unsigned int num_clusters;

#define for_each_cluster(cluster, idx) \
	for (; (idx) < num_clusters && ((cluster) = &cluster_state[idx]);\
		(idx)++)

static DEFINE_SPINLOCK(state_lock);
static void apply_need(struct cluster_data *state);
static void wake_up_core_ctl_thread(struct cluster_data *state);
static bool initialized;

ATOMIC_NOTIFIER_HEAD(core_ctl_notifier);
static unsigned int last_nr_big;

static unsigned int get_active_cpu_count(const struct cluster_data *cluster);

/* ========================= sysfs interface =========================== */

static ssize_t store_min_cpus(struct cluster_data *state,
				const char *buf, size_t count)
{
	unsigned int val;

	if (sscanf(buf, "%u\n", &val) != 1)
		return -EINVAL;

	state->min_cpus = min(val, state->num_cpus);
	wake_up_core_ctl_thread(state);

	return count;
}

static ssize_t show_min_cpus(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->min_cpus);
}

static ssize_t store_max_cpus(struct cluster_data *state,
				const char *buf, size_t count)
{
	unsigned int val;

	if (sscanf(buf, "%u\n", &val) != 1)
		return -EINVAL;

	val = min(val, state->num_cpus);
	state->max_cpus = val;
	wake_up_core_ctl_thread(state);

	return count;
}

static ssize_t show_max_cpus(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->max_cpus);
}

static ssize_t store_offline_delay_ms(struct cluster_data *state,
					const char *buf, size_t count)
{
	unsigned int val;

	if (sscanf(buf, "%u\n", &val) != 1)
		return -EINVAL;

	state->offline_delay_ms = val;
	apply_need(state);

	return count;
}

static ssize_t show_task_thres(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->task_thres);
}

static ssize_t store_task_thres(struct cluster_data *state,
				const char *buf, size_t count)
{
	unsigned int val;

	if (sscanf(buf, "%u\n", &val) != 1)
		return -EINVAL;

	if (val < state->num_cpus)
		return -EINVAL;

	state->task_thres = val;
	apply_need(state);

	return count;
}

static ssize_t show_nr_prev_assist_thresh(const struct cluster_data *state,
								char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->nr_prev_assist_thresh);
}

static ssize_t store_nr_prev_assist_thresh(struct cluster_data *state,
				const char *buf, size_t count)
{
	unsigned int val;

	if (sscanf(buf, "%u\n", &val) != 1)
		return -EINVAL;

	state->nr_prev_assist_thresh = val;
	apply_need(state);

	return count;
}

static ssize_t show_offline_delay_ms(const struct cluster_data *state,
				     char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->offline_delay_ms);
}

static ssize_t store_busy_up_thres(struct cluster_data *state,
					const char *buf, size_t count)
{
	unsigned int val[MAX_CPUS_PER_CLUSTER];
	int ret, i;

	ret = sscanf(buf, "%u %u %u %u %u %u\n",
			&val[0], &val[1], &val[2], &val[3],
			&val[4], &val[5]);
	if (ret != 1 && ret != state->num_cpus)
		return -EINVAL;

	if (ret == 1) {
		for (i = 0; i < state->num_cpus; i++)
			state->busy_up_thres[i] = val[0];
	} else {
		for (i = 0; i < state->num_cpus; i++)
			state->busy_up_thres[i] = val[i];
	}
	apply_need(state);
	return count;
}

static ssize_t show_busy_up_thres(const struct cluster_data *state, char *buf)
{
	int i, count = 0;

	for (i = 0; i < state->num_cpus; i++)
		count += snprintf(buf + count, PAGE_SIZE - count, "%u ",
				  state->busy_up_thres[i]);

	count += snprintf(buf + count, PAGE_SIZE - count, "\n");
	return count;
}

static ssize_t store_busy_down_thres(struct cluster_data *state,
					const char *buf, size_t count)
{
	unsigned int val[MAX_CPUS_PER_CLUSTER];
	int ret, i;

	ret = sscanf(buf, "%u %u %u %u %u %u\n",
			&val[0], &val[1], &val[2], &val[3],
			&val[4], &val[5]);
	if (ret != 1 && ret != state->num_cpus)
		return -EINVAL;

	if (ret == 1) {
		for (i = 0; i < state->num_cpus; i++)
			state->busy_down_thres[i] = val[0];
	} else {
		for (i = 0; i < state->num_cpus; i++)
			state->busy_down_thres[i] = val[i];
	}
	apply_need(state);
	return count;
}

static ssize_t show_busy_down_thres(const struct cluster_data *state, char *buf)
{
	int i, count = 0;

	for (i = 0; i < state->num_cpus; i++)
		count += snprintf(buf + count, PAGE_SIZE - count, "%u ",
				  state->busy_down_thres[i]);

	count += snprintf(buf + count, PAGE_SIZE - count, "\n");
	return count;
}

static ssize_t store_enable(struct cluster_data *state,
				const char *buf, size_t count)
{
	unsigned int val;
	bool bval;

	if (sscanf(buf, "%u\n", &val) != 1)
		return -EINVAL;

	bval = !!val;
	if (bval != state->enable) {
		state->enable = bval;
		apply_need(state);
	}

	return count;
}

static ssize_t show_enable(const struct cluster_data *state, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%u\n", state->enable);
}

static ssize_t show_need_cpus(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->need_cpus);
}

static ssize_t show_active_cpus(const struct cluster_data *state, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", state->active_cpus);
}

static ssize_t show_global_state(const struct cluster_data *state, char *buf)
{
	struct cpu_data *c;
	struct cluster_data *cluster;
	ssize_t count = 0;
	unsigned int cpu;

	spin_lock_irq(&state_lock);
	for_each_possible_cpu(cpu) {
		c = &per_cpu(cpu_state, cpu);
		cluster = c->cluster;
		if (!cluster || !cluster->inited)
			continue;

		count += snprintf(buf + count, PAGE_SIZE - count,
					"CPU%u\n", cpu);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tCPU: %u\n", c->cpu);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tOnline: %u\n",
					cpu_online(c->cpu));
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tIsolated: %u\n",
					cpu_isolated(c->cpu));
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tFirst CPU: %u\n",
						cluster->first_cpu);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tBusy%%: %u\n", c->busy);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tIs busy: %u\n", c->is_busy);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tNot preferred: %u\n",
						c->not_preferred);
		count += snprintf(buf + count, PAGE_SIZE - count,
					"\tNr running: %u\n", cluster->nrrun);
		count += snprintf(buf + count, PAGE_SIZE - count,
			"\tActive CPUs: %u\n", get_active_cpu_count(cluster));
		count += snprintf(buf + count, PAGE_SIZE - count,
				"\tNeed CPUs: %u\n", cluster->need_cpus);
		count += snprintf(buf + count, PAGE_SIZE - count,
				"\tNr isolated CPUs: %u\n",
						cluster->nr_isolated_cpus);
		count += snprintf(buf + count, PAGE_SIZE - count,
				"\tBoost: %u\n", (unsigned int) cluster->boost);
	}
	spin_unlock_irq(&state_lock);

	return count;
}

static ssize_t store_not_preferred(struct cluster_data *state,
				   const char *buf, size_t count)
{
	struct cpu_data *c;
	unsigned int i;
	unsigned int val[MAX_CPUS_PER_CLUSTER];
	unsigned long flags;
	int ret;
	int not_preferred_count = 0;

	ret = sscanf(buf, "%u %u %u %u %u %u\n",
			&val[0], &val[1], &val[2], &val[3],
			&val[4], &val[5]);
	if (ret != state->num_cpus)
		return -EINVAL;

	spin_lock_irqsave(&state_lock, flags);
	for (i = 0; i < state->num_cpus; i++) {
		c = &per_cpu(cpu_state, i + state->first_cpu);
		c->not_preferred = val[i];
		not_preferred_count += !!val[i];
	}
	state->nr_not_preferred_cpus = not_preferred_count;
	spin_unlock_irqrestore(&state_lock, flags);

	return count;
}

static ssize_t show_not_preferred(const struct cluster_data *state, char *buf)
{
	struct cpu_data *c;
	ssize_t count = 0;
	unsigned long flags;
	int i;

	spin_lock_irqsave(&state_lock, flags);
	for (i = 0; i < state->num_cpus; i++) {
		c = &per_cpu(cpu_state, i + state->first_cpu);
		count += scnprintf(buf + count, PAGE_SIZE - count,
				"CPU#%d: %u\n", c->cpu, c->not_preferred);
	}
	spin_unlock_irqrestore(&state_lock, flags);

	return count;
}


struct core_ctl_attr {
	struct attribute attr;
	ssize_t (*show)(const struct cluster_data *, char *);
	ssize_t (*store)(struct cluster_data *, const char *, size_t count);
};

#define core_ctl_attr_ro(_name)		\
static struct core_ctl_attr _name =	\
__ATTR(_name, 0444, show_##_name, NULL)

#define core_ctl_attr_rw(_name)			\
static struct core_ctl_attr _name =		\
__ATTR(_name, 0644, show_##_name, store_##_name)

core_ctl_attr_rw(min_cpus);
core_ctl_attr_rw(max_cpus);
core_ctl_attr_rw(offline_delay_ms);
core_ctl_attr_rw(busy_up_thres);
core_ctl_attr_rw(busy_down_thres);
core_ctl_attr_rw(task_thres);
core_ctl_attr_rw(nr_prev_assist_thresh);
core_ctl_attr_ro(need_cpus);
core_ctl_attr_ro(active_cpus);
core_ctl_attr_ro(global_state);
core_ctl_attr_rw(not_preferred);
core_ctl_attr_rw(enable);

static struct attribute *default_attrs[] = {
	&min_cpus.attr,
	&max_cpus.attr,
	&offline_delay_ms.attr,
	&busy_up_thres.attr,
	&busy_down_thres.attr,
	&task_thres.attr,
	&nr_prev_assist_thresh.attr,
	&enable.attr,
	&need_cpus.attr,
	&active_cpus.attr,
	&global_state.attr,
	&not_preferred.attr,
	NULL
};

#define to_cluster_data(k) container_of(k, struct cluster_data, kobj)
#define to_attr(a) container_of(a, struct core_ctl_attr, attr)
static ssize_t show(struct kobject *kobj, struct attribute *attr, char *buf)
{
	struct cluster_data *data = to_cluster_data(kobj);
	struct core_ctl_attr *cattr = to_attr(attr);
	ssize_t ret = -EIO;

	if (cattr->show)
		ret = cattr->show(data, buf);

	return ret;
}

static ssize_t store(struct kobject *kobj, struct attribute *attr,
		     const char *buf, size_t count)
{
	struct cluster_data *data = to_cluster_data(kobj);
	struct core_ctl_attr *cattr = to_attr(attr);
	ssize_t ret = -EIO;

	if (cattr->store)
		ret = cattr->store(data, buf, count);

	return ret;
}

static const struct sysfs_ops sysfs_ops = {
	.show	= show,
	.store	= store,
};

static struct kobj_type ktype_core_ctl = {
	.sysfs_ops	= &sysfs_ops,
	.default_attrs	= default_attrs,
};

/* ==================== runqueue based core count =================== */

static struct sched_avg_stats nr_stats[NR_CPUS];

/*
 * nr_need:
 *   Number of tasks running on this cluster plus
 *   tasks running on higher capacity clusters.
 *   To find out CPUs needed from this cluster.
 *
 * For example:
 *   On dual cluster system with 4 min capacity
 *   CPUs and 4 max capacity CPUs, if there are
 *   4 small tasks running on min capacity CPUs
 *   and 2 big tasks running on 2 max capacity
 *   CPUs, nr_need has to be 6 for min capacity
 *   cluster and 2 for max capacity cluster.
 *   This is because, min capacity cluster has to
 *   account for tasks running on max capacity
 *   cluster, so that, the min capacity cluster
 *   can be ready to accommodate tasks running on max
 *   capacity CPUs if the demand of tasks goes down.
 */
static int compute_cluster_nr_need(int index)
{
	int cpu;
	struct cluster_data *cluster;
	int nr_need = 0;

	for_each_cluster(cluster, index) {
		for_each_cpu(cpu, &cluster->cpu_mask)
			nr_need += nr_stats[cpu].nr;
	}

	return nr_need;
}

/*
 * prev_misfit_need:
 *   Tasks running on smaller capacity cluster which
 *   needs to be migrated to higher capacity cluster.
 *   To find out how many tasks need higher capacity CPUs.
 *
 * For example:
 *   On dual cluster system with 4 min capacity
 *   CPUs and 4 max capacity CPUs, if there are
 *   2 small tasks and 2 big tasks running on
 *   min capacity CPUs and no tasks running on
 *   max cpacity, prev_misfit_need of min capacity
 *   cluster will be 0 and prev_misfit_need of
 *   max capacity cluster will be 2.
 */
static int compute_prev_cluster_misfit_need(int index)
{
	int cpu;
	struct cluster_data *prev_cluster;
	int prev_misfit_need = 0;

	/*
	 * Lowest capacity cluster does not have to
	 * accommodate any misfit tasks.
	 */
	if (index == 0)
		return 0;

	prev_cluster = &cluster_state[index - 1];

	for_each_cpu(cpu, &prev_cluster->cpu_mask)
		prev_misfit_need += nr_stats[cpu].nr_misfit;

	return prev_misfit_need;
}

static int compute_cluster_max_nr(int index)
{
	int cpu;
	struct cluster_data *cluster = &cluster_state[index];
	int max_nr = 0;

	for_each_cpu(cpu, &cluster->cpu_mask)
		max_nr = max(max_nr, nr_stats[cpu].nr_max);

	return max_nr;
}

static int cluster_real_big_tasks(int index)
{
	int nr_big = 0;
	int cpu;
	struct cluster_data *cluster = &cluster_state[index];

	if (index == 0) {
		for_each_cpu(cpu, &cluster->cpu_mask)
			nr_big += nr_stats[cpu].nr_misfit;
	} else {
		for_each_cpu(cpu, &cluster->cpu_mask)
			nr_big += nr_stats[cpu].nr;
	}

	return nr_big;
}

/*
 * prev_nr_need_assist:
 *   Tasks that are eligible to run on the previous
 *   cluster but cannot run because of insufficient
 *   CPUs there. prev_nr_need_assist is indicative
 *   of number of CPUs in this cluster that should
 *   assist its previous cluster to makeup for
 *   insufficient CPUs there.
 *
 * For example:
 *   On tri-cluster system with 4 min capacity
 *   CPUs, 3 intermediate capacity CPUs and 1
 *   max capacity CPU, if there are 4 small
 *   tasks running on min capacity CPUs, 4 big
 *   tasks running on intermediate capacity CPUs
 *   and no tasks running on max capacity CPU,
 *   prev_nr_need_assist for min & max capacity
 *   clusters will be 0, but, for intermediate
 *   capacity cluster prev_nr_need_assist will
 *   be 1 as it has 3 CPUs, but, there are 4 big
 *   tasks to be served.
 */
static int prev_cluster_nr_need_assist(int index)
{
	int need = 0;
	int cpu;
	struct cluster_data *prev_cluster;

	if (index == 0)
		return 0;

	index--;
	prev_cluster = &cluster_state[index];

	/*
	 * Next cluster should not assist, while there are isolated cpus
	 * in this cluster.
	 */
	if (prev_cluster->nr_isolated_cpus)
		return 0;

	for_each_cpu(cpu, &prev_cluster->cpu_mask)
		need += nr_stats[cpu].nr;

	need += compute_prev_cluster_misfit_need(index);

	if (need > prev_cluster->active_cpus)
		need = need - prev_cluster->active_cpus;
	else
		need = 0;

	return need;
}

/*
 * This is only implemented for min capacity cluster.
 *
 * Bringing a little CPU out of isolation and using it
 * more does not hurt power as much as bringing big CPUs.
 *
 * little cluster provides help needed for the other clusters.
 * we take nr_scaled (which gives better resolution) and find
 * the total nr in the system. Then take out the active higher
 * capacity CPUs from the nr and consider the remaining nr as
 * strict and consider that many little CPUs are needed.
 */
static int compute_cluster_nr_strict_need(int index)
{
	int cpu;
	struct cluster_data *cluster;
	int nr_strict_need = 0;

	if (index != 0)
		return 0;

	for_each_cluster(cluster, index) {
		int nr_scaled = 0;
		int active_cpus = cluster->active_cpus;

		for_each_cpu(cpu, &cluster->cpu_mask)
			nr_scaled += nr_stats[cpu].nr_scaled;

		nr_scaled /= 100;

		/*
		 * For little cluster, nr_scaled becomes the nr_strict,
		 * for other cluster, overflow is counted towards
		 * the little cluster need.
		 */
		if (index == 0)
			nr_strict_need += nr_scaled;
		else
			nr_strict_need += max(0, nr_scaled - active_cpus);
	}

	return nr_strict_need;
}
static void update_running_avg(void)
{
	struct cluster_data *cluster;
	unsigned int index = 0;
	unsigned long flags;
	int big_avg = 0;

	sched_get_nr_running_avg(nr_stats);

	spin_lock_irqsave(&state_lock, flags);
	for_each_cluster(cluster, index) {
		int nr_need, prev_misfit_need;

		if (!cluster->inited)
			continue;

		nr_need = compute_cluster_nr_need(index);
		prev_misfit_need = compute_prev_cluster_misfit_need(index);


		cluster->nrrun = nr_need + prev_misfit_need;
		cluster->max_nr = compute_cluster_max_nr(index);
		cluster->nr_prev_assist = prev_cluster_nr_need_assist(index);

		cluster->strict_nrrun = compute_cluster_nr_strict_need(index);

		trace_core_ctl_update_nr_need(cluster->first_cpu, nr_need,
					prev_misfit_need,
					cluster->nrrun, cluster->max_nr,
					cluster->nr_prev_assist);

		big_avg += cluster_real_big_tasks(index);
	}
	spin_unlock_irqrestore(&state_lock, flags);

	last_nr_big = big_avg;

}

#define MAX_NR_THRESHOLD	4
/* adjust needed CPUs based on current runqueue information */
static unsigned int apply_task_need(const struct cluster_data *cluster,
				    unsigned int new_need)
{
	/* unisolate all cores if there are enough tasks */
	if (cluster->nrrun >= cluster->task_thres)
		return cluster->num_cpus;

	/*
	 * unisolate as many cores as the previous cluster
	 * needs assistance with.
	 */
	if (cluster->nr_prev_assist >= cluster->nr_prev_assist_thresh)
		new_need = new_need + cluster->nr_prev_assist;

	/* only unisolate more cores if there are tasks to run */
	if (cluster->nrrun > new_need)
		new_need = new_need + 1;

	/*
	 * We don't want tasks to be overcrowded in a cluster.
	 * If any CPU has more than MAX_NR_THRESHOLD in the last
	 * window, bring another CPU to help out.
	 */
	if (cluster->max_nr > MAX_NR_THRESHOLD)
		new_need = new_need + 1;

	/*
	 * For little cluster, we use a bit more relaxed approach
	 * and impose the strict nr condition. Because all tasks can
	 * spill onto little if big cluster is crowded.
	 */
	if (new_need < cluster->strict_nrrun)
		new_need = cluster->strict_nrrun;

	return new_need;
}

/* ======================= load based core count  ====================== */

static unsigned int apply_limits(const struct cluster_data *cluster,
				 unsigned int need_cpus)
{
	return min(max(cluster->min_cpus, need_cpus), cluster->max_cpus);
}

static unsigned int get_active_cpu_count(const struct cluster_data *cluster)
{
	return cluster->num_cpus -
				sched_isolate_count(&cluster->cpu_mask, true);
}

static bool is_active(const struct cpu_data *state)
{
	return cpu_online(state->cpu) && !cpu_isolated(state->cpu);
}

static bool adjustment_possible(const struct cluster_data *cluster,
							unsigned int need)
{
	return (need < cluster->active_cpus || (need > cluster->active_cpus &&
						cluster->nr_isolated_cpus));
}

static bool need_all_cpus(const struct cluster_data *cluster)
{

	return (is_min_capacity_cpu(cluster->first_cpu) &&
		sched_ravg_window < DEFAULT_SCHED_RAVG_WINDOW);
}

static bool eval_need(struct cluster_data *cluster)
{
	unsigned long flags;
	struct cpu_data *c;
	unsigned int need_cpus = 0, last_need, thres_idx;
	int ret = 0;
	bool need_flag = false;
	unsigned int new_need;
	s64 now, elapsed;

	if (unlikely(!cluster->inited))
		return 0;

	spin_lock_irqsave(&state_lock, flags);

	if (cluster->boost || !cluster->enable || need_all_cpus(cluster)) {
		need_cpus = cluster->max_cpus;
	} else {
		cluster->active_cpus = get_active_cpu_count(cluster);
		thres_idx = cluster->active_cpus ? cluster->active_cpus - 1 : 0;
		list_for_each_entry(c, &cluster->lru, sib) {
			bool old_is_busy = c->is_busy;

			if (c->busy >= cluster->busy_up_thres[thres_idx] ||
			    sched_cpu_high_irqload(c->cpu))
				c->is_busy = true;
			else if (c->busy < cluster->busy_down_thres[thres_idx])
				c->is_busy = false;

			trace_core_ctl_set_busy(c->cpu, c->busy, old_is_busy,
						c->is_busy);
			need_cpus += c->is_busy;
		}
		need_cpus = apply_task_need(cluster, need_cpus);
	}
	new_need = apply_limits(cluster, need_cpus);
	need_flag = adjustment_possible(cluster, new_need);

	last_need = cluster->need_cpus;
	now = ktime_to_ms(ktime_get());

	if (new_need > cluster->active_cpus) {
		ret = 1;
	} else {
		/*
		 * When there is no change in need and there are no more
		 * active CPUs than currently needed, just update the
		 * need time stamp and return.
		 */
		if (new_need == last_need && new_need == cluster->active_cpus) {
			cluster->need_ts = now;
			spin_unlock_irqrestore(&state_lock, flags);
			return 0;
		}

		elapsed =  now - cluster->need_ts;
		ret = elapsed >= cluster->offline_delay_ms;
	}

	if (ret) {
		cluster->need_ts = now;
		cluster->need_cpus = new_need;
	}
	trace_core_ctl_eval_need(cluster->first_cpu, last_need, new_need,
				 ret && need_flag);
	spin_unlock_irqrestore(&state_lock, flags);

	return ret && need_flag;
}

static void apply_need(struct cluster_data *cluster)
{
	if (eval_need(cluster))
		wake_up_core_ctl_thread(cluster);
}

/* ========================= core count enforcement ==================== */

static void wake_up_core_ctl_thread(struct cluster_data *cluster)
{
	unsigned long flags;

	spin_lock_irqsave(&cluster->pending_lock, flags);
	cluster->pending = true;
	spin_unlock_irqrestore(&cluster->pending_lock, flags);

	wake_up_process(cluster->core_ctl_thread);
}

static u64 core_ctl_check_timestamp;

int core_ctl_set_boost(bool boost)
{
	unsigned int index = 0;
	struct cluster_data *cluster = NULL;
	unsigned long flags;
	int ret = 0;
	bool boost_state_changed = false;

	if (unlikely(!initialized))
		return 0;

	spin_lock_irqsave(&state_lock, flags);
	for_each_cluster(cluster, index) {
		if (boost) {
			boost_state_changed = !cluster->boost;
			++cluster->boost;
		} else {
			if (!cluster->boost) {
				ret = -EINVAL;
				break;
			}
			--cluster->boost;
			boost_state_changed = !cluster->boost;
		}
	}
	spin_unlock_irqrestore(&state_lock, flags);

	if (boost_state_changed) {
		index = 0;
		for_each_cluster(cluster, index)
			apply_need(cluster);
	}

	if (cluster)
		trace_core_ctl_set_boost(cluster->boost, ret);

	return ret;
}
EXPORT_SYMBOL(core_ctl_set_boost);

void core_ctl_notifier_register(struct notifier_block *n)
{
	atomic_notifier_chain_register(&core_ctl_notifier, n);
}

void core_ctl_notifier_unregister(struct notifier_block *n)
{
	atomic_notifier_chain_unregister(&core_ctl_notifier, n);
}

static void core_ctl_call_notifier(void)
{
	struct core_ctl_notif_data ndata = {0};
	struct notifier_block *nb;

	/*
	 * Don't bother querying the stats when the notifier
	 * chain is empty.
	 */
	rcu_read_lock();
	nb = rcu_dereference_raw(core_ctl_notifier.head);
	rcu_read_unlock();

	if (!nb)
		return;

	ndata.nr_big = last_nr_big;
	trace_core_ctl_notif_data(ndata.nr_big, ndata.coloc_load_pct,
			ndata.ta_util_pct, ndata.cur_cap_pct);

	atomic_notifier_call_chain(&core_ctl_notifier, 0, &ndata);
}

void core_ctl_check(u64 window_start)
{
	int cpu;
	struct cpu_data *c;
	struct cluster_data *cluster;
	unsigned int index = 0;
	unsigned long flags;

	if (unlikely(!initialized))
		return;

	if (window_start == core_ctl_check_timestamp)
		return;

	core_ctl_check_timestamp = window_start;

	spin_lock_irqsave(&state_lock, flags);
	for_each_possible_cpu(cpu) {

		c = &per_cpu(cpu_state, cpu);
		cluster = c->cluster;

		if (!cluster || !cluster->inited)
			continue;

		c->busy = sched_get_cpu_util(cpu);
	}
	spin_unlock_irqrestore(&state_lock, flags);

	update_running_avg();

	for_each_cluster(cluster, index) {
		if (eval_need(cluster))
			wake_up_core_ctl_thread(cluster);
	}

	core_ctl_call_notifier();
}

static void move_cpu_lru(struct cpu_data *cpu_data)
{
	unsigned long flags;

	spin_lock_irqsave(&state_lock, flags);
	list_del(&cpu_data->sib);
	list_add_tail(&cpu_data->sib, &cpu_data->cluster->lru);
	spin_unlock_irqrestore(&state_lock, flags);
}

static bool should_we_isolate(int cpu, struct cluster_data *cluster)
{
	return true;
}

static void try_to_isolate(struct cluster_data *cluster, unsigned int need)
{
	struct cpu_data *c, *tmp;
	unsigned long flags;
	unsigned int num_cpus = cluster->num_cpus;
	unsigned int nr_isolated = 0;
	bool first_pass = cluster->nr_not_preferred_cpus;

	/*
	 * Protect against entry being removed (and added at tail) by other
	 * thread (hotplug).
	 */
	spin_lock_irqsave(&state_lock, flags);
	list_for_each_entry_safe(c, tmp, &cluster->lru, sib) {
		if (!num_cpus--)
			break;

		if (!is_active(c))
			continue;
		if (cluster->active_cpus == need)
			break;
		/* Don't isolate busy CPUs. */
		if (c->is_busy)
			continue;

		/*
		 * We isolate only the not_preferred CPUs. If none
		 * of the CPUs are selected as not_preferred, then
		 * all CPUs are eligible for isolation.
		 */
		if (cluster->nr_not_preferred_cpus && !c->not_preferred)
			continue;

		if (!should_we_isolate(c->cpu, cluster))
			continue;

		spin_unlock_irqrestore(&state_lock, flags);

		pr_debug("Trying to isolate CPU%u\n", c->cpu);
		if (!sched_isolate_cpu(c->cpu)) {
			c->isolated_by_us = true;
			move_cpu_lru(c);
			nr_isolated++;
		} else {
			pr_debug("Unable to isolate CPU%u\n", c->cpu);
		}
		cluster->active_cpus = get_active_cpu_count(cluster);
		spin_lock_irqsave(&state_lock, flags);
	}
	cluster->nr_isolated_cpus += nr_isolated;
	spin_unlock_irqrestore(&state_lock, flags);

again:
	/*
	 * If the number of active CPUs is within the limits, then
	 * don't force isolation of any busy CPUs.
	 */
	if (cluster->active_cpus <= cluster->max_cpus)
		return;

	nr_isolated = 0;
	num_cpus = cluster->num_cpus;
	spin_lock_irqsave(&state_lock, flags);
	list_for_each_entry_safe(c, tmp, &cluster->lru, sib) {
		if (!num_cpus--)
			break;

		if (!is_active(c))
			continue;
		if (cluster->active_cpus <= cluster->max_cpus)
			break;

		if (first_pass && !c->not_preferred)
			continue;

		spin_unlock_irqrestore(&state_lock, flags);

		pr_debug("Trying to isolate CPU%u\n", c->cpu);
		if (!sched_isolate_cpu(c->cpu)) {
			c->isolated_by_us = true;
			move_cpu_lru(c);
			nr_isolated++;
		} else {
			pr_debug("Unable to isolate CPU%u\n", c->cpu);
		}
		cluster->active_cpus = get_active_cpu_count(cluster);
		spin_lock_irqsave(&state_lock, flags);
	}
	cluster->nr_isolated_cpus += nr_isolated;
	spin_unlock_irqrestore(&state_lock, flags);

	if (first_pass && cluster->active_cpus > cluster->max_cpus) {
		first_pass = false;
		goto again;
	}
}

static void __try_to_unisolate(struct cluster_data *cluster,
			       unsigned int need, bool force)
{
	struct cpu_data *c, *tmp;
	unsigned long flags;
	unsigned int num_cpus = cluster->num_cpus;
	unsigned int nr_unisolated = 0;

	/*
	 * Protect against entry being removed (and added at tail) by other
	 * thread (hotplug).
	 */
	spin_lock_irqsave(&state_lock, flags);
	list_for_each_entry_safe(c, tmp, &cluster->lru, sib) {
		if (!num_cpus--)
			break;

		if (!c->isolated_by_us)
			continue;
		if ((cpu_online(c->cpu) && !cpu_isolated(c->cpu)) ||
			(!force && c->not_preferred))
			continue;
		if (cluster->active_cpus == need)
			break;

		spin_unlock_irqrestore(&state_lock, flags);

		pr_debug("Trying to unisolate CPU%u\n", c->cpu);
		if (!sched_unisolate_cpu(c->cpu)) {
			c->isolated_by_us = false;
			move_cpu_lru(c);
			nr_unisolated++;
		} else {
			pr_debug("Unable to unisolate CPU%u\n", c->cpu);
		}
		cluster->active_cpus = get_active_cpu_count(cluster);
		spin_lock_irqsave(&state_lock, flags);
	}
	cluster->nr_isolated_cpus -= nr_unisolated;
	spin_unlock_irqrestore(&state_lock, flags);
}

static void try_to_unisolate(struct cluster_data *cluster, unsigned int need)
{
	bool force_use_non_preferred = false;

	__try_to_unisolate(cluster, need, force_use_non_preferred);

	if (cluster->active_cpus == need)
		return;

	force_use_non_preferred = true;
	__try_to_unisolate(cluster, need, force_use_non_preferred);
}

static void __ref do_core_ctl(struct cluster_data *cluster)
{
	unsigned int need;

	need = apply_limits(cluster, cluster->need_cpus);

	if (adjustment_possible(cluster, need)) {
		pr_debug("Trying to adjust group %u from %u to %u\n",
				cluster->first_cpu, cluster->active_cpus, need);

		if (cluster->active_cpus > need)
			try_to_isolate(cluster, need);
		else if (cluster->active_cpus < need)
			try_to_unisolate(cluster, need);
	}
}

static int __ref try_core_ctl(void *data)
{
	struct cluster_data *cluster = data;
	unsigned long flags;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		spin_lock_irqsave(&cluster->pending_lock, flags);
		if (!cluster->pending) {
			spin_unlock_irqrestore(&cluster->pending_lock, flags);
			schedule();
			if (kthread_should_stop())
				break;
			spin_lock_irqsave(&cluster->pending_lock, flags);
		}
		set_current_state(TASK_RUNNING);
		cluster->pending = false;
		spin_unlock_irqrestore(&cluster->pending_lock, flags);

		do_core_ctl(cluster);
	}

	return 0;
}

static int isolation_cpuhp_state(unsigned int cpu,  bool online)
{
	struct cpu_data *state = &per_cpu(cpu_state, cpu);
	struct cluster_data *cluster = state->cluster;
	unsigned int need;
	bool do_wakeup = false, unisolated = false;
	unsigned long flags;

	if (unlikely(!cluster || !cluster->inited))
		return 0;

	if (online) {
		cluster->active_cpus = get_active_cpu_count(cluster);

		/*
		 * Moving to the end of the list should only happen in
		 * CPU_ONLINE and not on CPU_UP_PREPARE to prevent an
		 * infinite list traversal when thermal (or other entities)
		 * reject trying to online CPUs.
		 */
		move_cpu_lru(state);
	} else {
		/*
		 * We don't want to have a CPU both offline and isolated.
		 * So unisolate a CPU that went down if it was isolated by us.
		 */
		if (state->isolated_by_us) {
			sched_unisolate_cpu_unlocked(cpu);
			state->isolated_by_us = false;
			unisolated = true;
		}

		/* Move a CPU to the end of the LRU when it goes offline. */
		move_cpu_lru(state);

		state->busy = 0;
		cluster->active_cpus = get_active_cpu_count(cluster);
	}

	need = apply_limits(cluster, cluster->need_cpus);
	spin_lock_irqsave(&state_lock, flags);
	if (unisolated)
		cluster->nr_isolated_cpus--;
	do_wakeup = adjustment_possible(cluster, need);
	spin_unlock_irqrestore(&state_lock, flags);
	if (do_wakeup)
		wake_up_core_ctl_thread(cluster);

	return 0;
}

static int core_ctl_isolation_online_cpu(unsigned int cpu)
{
	return isolation_cpuhp_state(cpu, true);
}

static int core_ctl_isolation_dead_cpu(unsigned int cpu)
{
	return isolation_cpuhp_state(cpu, false);
}

/* ============================ init code ============================== */

static struct cluster_data *find_cluster_by_first_cpu(unsigned int first_cpu)
{
	unsigned int i;

	for (i = 0; i < num_clusters; ++i) {
		if (cluster_state[i].first_cpu == first_cpu)
			return &cluster_state[i];
	}

	return NULL;
}

static int cluster_init(const struct cpumask *mask)
{
	struct device *dev;
	unsigned int first_cpu = cpumask_first(mask);
	struct cluster_data *cluster;
	struct cpu_data *state;
	unsigned int cpu;
	struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

	if (find_cluster_by_first_cpu(first_cpu))
		return 0;

	dev = get_cpu_device(first_cpu);
	if (!dev)
		return -ENODEV;

	pr_info("Creating CPU group %d\n", first_cpu);

	if (num_clusters == MAX_CLUSTERS) {
		pr_err("Unsupported number of clusters. Only %u supported\n",
								MAX_CLUSTERS);
		return -EINVAL;
	}
	cluster = &cluster_state[num_clusters];
	++num_clusters;

	cpumask_copy(&cluster->cpu_mask, mask);
	cluster->num_cpus = cpumask_weight(mask);
	if (cluster->num_cpus > MAX_CPUS_PER_CLUSTER) {
		pr_err("HW configuration not supported\n");
		return -EINVAL;
	}
	cluster->first_cpu = first_cpu;
	cluster->min_cpus = 1;
	cluster->max_cpus = cluster->num_cpus;
	cluster->need_cpus = cluster->num_cpus;
	cluster->offline_delay_ms = 100;
	cluster->task_thres = UINT_MAX;
	cluster->nr_prev_assist_thresh = UINT_MAX;
	cluster->nrrun = cluster->num_cpus;
	cluster->enable = true;
	cluster->nr_not_preferred_cpus = 0;
	cluster->strict_nrrun = 0;
	INIT_LIST_HEAD(&cluster->lru);
	spin_lock_init(&cluster->pending_lock);

	for_each_cpu(cpu, mask) {
		pr_info("Init CPU%u state\n", cpu);

		state = &per_cpu(cpu_state, cpu);
		state->cluster = cluster;
		state->cpu = cpu;
		list_add_tail(&state->sib, &cluster->lru);
	}
	cluster->active_cpus = get_active_cpu_count(cluster);

	cluster->core_ctl_thread = kthread_run(try_core_ctl, (void *) cluster,
					"core_ctl/%d", first_cpu);
	if (IS_ERR(cluster->core_ctl_thread))
		return PTR_ERR(cluster->core_ctl_thread);

	sched_setscheduler_nocheck(cluster->core_ctl_thread, SCHED_FIFO,
				   &param);

	cluster->inited = true;

	kobject_init(&cluster->kobj, &ktype_core_ctl);
	return kobject_add(&cluster->kobj, &dev->kobj, "core_ctl");
}

static int __init core_ctl_init(void)
{
	struct sched_cluster *cluster;
	int ret;

	cpuhp_setup_state_nocalls(CPUHP_AP_ONLINE_DYN,
			"core_ctl/isolation:online",
			core_ctl_isolation_online_cpu, NULL);

	cpuhp_setup_state_nocalls(CPUHP_CORE_CTL_ISOLATION_DEAD,
			"core_ctl/isolation:dead",
			NULL, core_ctl_isolation_dead_cpu);

	for_each_sched_cluster(cluster) {
		ret = cluster_init(&cluster->cpus);
		if (ret)
			pr_warn("unable to create core ctl group: %d\n", ret);
	}

	initialized = true;
	return 0;
}

late_initcall(core_ctl_init);
