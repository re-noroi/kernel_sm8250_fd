// SPDX-License-Identifier: GPL-2.0
/*
 * Scheduler topology setup/handling methods
 */
#include "sched.h"

DEFINE_MUTEX(sched_domains_mutex);

/* Protected by sched_domains_mutex: */
cpumask_var_t sched_domains_tmpmask;
cpumask_var_t sched_domains_tmpmask2;

#ifdef CONFIG_SCHED_DEBUG

static int __init sched_debug_setup(char *str)
{
	sched_debug_enabled = true;

	return 0;
}
early_param("sched_debug", sched_debug_setup);

static inline bool sched_debug(void)
{
	return sched_debug_enabled;
}

static int sched_domain_debug_one(struct sched_domain *sd, int cpu, int level,
				  struct cpumask *groupmask)
{
	struct sched_group *group = sd->groups;

	cpumask_clear(groupmask);

	printk(KERN_DEBUG "%*s domain-%d: ", level, "", level);
	printk(KERN_CONT "span=%*pbl level=%s\n",
	       cpumask_pr_args(sched_domain_span(sd)), sd->name);

	if (!cpumask_test_cpu(cpu, sched_domain_span(sd))) {
		printk(KERN_ERR "ERROR: domain->span does not contain CPU%d\n", cpu);
	}
	if (group && !cpumask_test_cpu(cpu, sched_group_span(group))) {
		printk(KERN_ERR "ERROR: domain->groups does not contain CPU%d\n", cpu);
	}

	printk(KERN_DEBUG "%*s groups:", level + 1, "");
	do {
		if (!group) {
			printk("\n");
			printk(KERN_ERR "ERROR: group is NULL\n");
			break;
		}

		if (!cpumask_weight(sched_group_span(group))) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: empty group\n");
			break;
		}

		if (!(sd->flags & SD_OVERLAP) &&
		    cpumask_intersects(groupmask, sched_group_span(group))) {
			printk(KERN_CONT "\n");
			printk(KERN_ERR "ERROR: repeated CPUs\n");
			break;
		}

		cpumask_or(groupmask, groupmask, sched_group_span(group));

		printk(KERN_CONT " %d:{ span=%*pbl",
				group->sgc->id,
				cpumask_pr_args(sched_group_span(group)));

		if ((sd->flags & SD_OVERLAP) &&
		    !cpumask_equal(group_balance_mask(group), sched_group_span(group))) {
			printk(KERN_CONT " mask=%*pbl",
				cpumask_pr_args(group_balance_mask(group)));
		}

		if (group->sgc->capacity != SCHED_CAPACITY_SCALE)
			printk(KERN_CONT " cap=%lu", group->sgc->capacity);

		if (group == sd->groups && sd->child &&
		    !cpumask_equal(sched_domain_span(sd->child),
				   sched_group_span(group))) {
			printk(KERN_ERR "ERROR: domain->groups does not match domain->child\n");
		}

		printk(KERN_CONT " }");

		group = group->next;

		if (group != sd->groups)
			printk(KERN_CONT ",");

	} while (group != sd->groups);
	printk(KERN_CONT "\n");

	if (!cpumask_equal(sched_domain_span(sd), groupmask))
		printk(KERN_ERR "ERROR: groups don't span domain->span\n");

	if (sd->parent &&
	    !cpumask_subset(groupmask, sched_domain_span(sd->parent)))
		printk(KERN_ERR "ERROR: parent span is not a superset of domain->span\n");
	return 0;
}

static void sched_domain_debug(struct sched_domain *sd, int cpu)
{
	int level = 0;

	if (!sched_debug_enabled)
		return;

	if (!sd) {
		printk(KERN_DEBUG "CPU%d attaching NULL sched-domain.\n", cpu);
		return;
	}

	printk(KERN_DEBUG "CPU%d attaching sched-domain(s):\n", cpu);

	for (;;) {
		if (sched_domain_debug_one(sd, cpu, level, sched_domains_tmpmask))
			break;
		level++;
		sd = sd->parent;
		if (!sd)
			break;
	}
}
#else /* !CONFIG_SCHED_DEBUG */

# define sched_debug_enabled 0
# define sched_domain_debug(sd, cpu) do { } while (0)
static inline bool sched_debug(void)
{
	return false;
}
#endif /* CONFIG_SCHED_DEBUG */

static int sd_degenerate(struct sched_domain *sd)
{
	if (cpumask_weight(sched_domain_span(sd)) == 1)
		return 1;

	/* Following flags need at least 2 groups */
	if (sd->flags & (SD_BALANCE_NEWIDLE |
			 SD_BALANCE_FORK |
			 SD_BALANCE_EXEC |
			 SD_SHARE_CPUCAPACITY |
			 SD_ASYM_CPUCAPACITY |
			 SD_SHARE_PKG_RESOURCES |
			 SD_SHARE_POWERDOMAIN)) {
		if (sd->groups != sd->groups->next)
			return 0;
	}

	/* Following flags don't use groups */
	if (sd->flags & (SD_WAKE_AFFINE))
		return 0;

	return 1;
}

static int
sd_parent_degenerate(struct sched_domain *sd, struct sched_domain *parent)
{
	unsigned long cflags = sd->flags, pflags = parent->flags;

	if (sd_degenerate(parent))
		return 1;

	if (!cpumask_equal(sched_domain_span(sd), sched_domain_span(parent)))
		return 0;

	/* Flags needing groups don't count if only 1 group in parent */
	if (parent->groups == parent->groups->next) {
		pflags &= ~(SD_BALANCE_NEWIDLE |
			    SD_BALANCE_FORK |
			    SD_BALANCE_EXEC |
			    SD_ASYM_CPUCAPACITY |
			    SD_SHARE_CPUCAPACITY |
			    SD_SHARE_PKG_RESOURCES |
			    SD_PREFER_SIBLING |
			    SD_SHARE_POWERDOMAIN);
		if (nr_node_ids == 1)
			pflags &= ~SD_SERIALIZE;
	}
	if (~cflags & pflags)
		return 0;

	return 1;
}

#ifdef CONFIG_ENERGY_MODEL
DEFINE_STATIC_KEY_FALSE(sched_energy_present);
unsigned int sysctl_sched_energy_aware = 1;
static DEFINE_MUTEX(sched_energy_mutex);
static bool sched_energy_update;

#ifdef CONFIG_PROC_SYSCTL
int sched_energy_aware_handler(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret, state;

	if (write && !capable(CAP_SYS_ADMIN))
		return -EPERM;

	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (!ret && write) {
		state = static_branch_unlikely(&sched_energy_present);
		if (state != sysctl_sched_energy_aware) {
			mutex_lock(&sched_energy_mutex);
			sched_energy_update = 1;
			rebuild_sched_domains();
			sched_energy_update = 0;
			mutex_unlock(&sched_energy_mutex);
		}
	}

	return ret;
}
#endif

static void free_pd(struct perf_domain *pd)
{
	struct perf_domain *tmp;

	while (pd) {
		tmp = pd->next;
		kfree(pd);
		pd = tmp;
	}
}

static struct perf_domain *find_pd(struct perf_domain *pd, int cpu)
{
	while (pd) {
		if (cpumask_test_cpu(cpu, perf_domain_span(pd)))
			return pd;
		pd = pd->next;
	}

	return NULL;
}

static struct perf_domain *pd_init(int cpu)
{
	struct em_perf_domain *obj = em_cpu_get(cpu);
	struct perf_domain *pd;

	if (!obj) {
		if (sched_debug())
			pr_info("%s: no EM found for CPU%d\n", __func__, cpu);
		return NULL;
	}

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return NULL;
	pd->em_pd = obj;

	return pd;
}

static void perf_domain_debug(const struct cpumask *cpu_map,
						struct perf_domain *pd)
{
	if (!sched_debug() || !pd)
		return;

	printk(KERN_DEBUG "root_domain %*pbl:", cpumask_pr_args(cpu_map));

	while (pd) {
		printk(KERN_CONT " pd%d:{ cpus=%*pbl nr_cstate=%d }",
				cpumask_first(perf_domain_span(pd)),
				cpumask_pr_args(perf_domain_span(pd)),
				em_pd_nr_cap_states(pd->em_pd));
		pd = pd->next;
	}

	printk(KERN_CONT "\n");
}

static void destroy_perf_domain_rcu(struct rcu_head *rp)
{
	struct perf_domain *pd;

	pd = container_of(rp, struct perf_domain, rcu);
	free_pd(pd);
}

static void sched_energy_set(bool has_eas)
{
	if (!has_eas && static_branch_unlikely(&sched_energy_present)) {
		if (sched_debug())
			pr_info("%s: stopping EAS\n", __func__);
		static_branch_disable_cpuslocked(&sched_energy_present);
	} else if (has_eas && !static_branch_unlikely(&sched_energy_present)) {
		if (sched_debug())
			pr_info("%s: starting EAS\n", __func__);
		static_branch_enable_cpuslocked(&sched_energy_present);
	}
}

/*
 * EAS can be used on a root domain if it meets all the following conditions:
 *    1. an Energy Model (EM) is available;
 *    2. the SD_ASYM_CPUCAPACITY flag is set in the sched_domain hierarchy.
 *    3. the EM complexity is low enough to keep scheduling overheads low;
 *
 * The complexity of the Energy Model is defined as:
 *
 *              C = nr_pd * (nr_cpus + nr_cs)
 *
 * with parameters defined as:
 *  - nr_pd:    the number of performance domains
 *  - nr_cpus:  the number of CPUs
 *  - nr_cs:    the sum of the number of capacity states of all performance
 *              domains (for example, on a system with 2 performance domains,
 *              with 10 capacity states each, nr_cs = 2 * 10 = 20).
 *
 * It is generally not a good idea to use such a model in the wake-up path on
 * very complex platforms because of the associated scheduling overheads. The
 * arbitrary constraint below prevents that. It makes EAS usable up to 16 CPUs
 * with per-CPU DVFS and less than 8 capacity states each, for example.
 */
#define EM_MAX_COMPLEXITY 2048

static bool build_perf_domains(const struct cpumask *cpu_map)
{
	int i, nr_pd = 0, nr_cs = 0, nr_cpus = cpumask_weight(cpu_map);
	struct perf_domain *pd = NULL, *tmp;
	int cpu = cpumask_first(cpu_map);
	struct root_domain *rd = cpu_rq(cpu)->rd;
	unsigned long min_cap = ULONG_MAX;
	struct cpumask min_cpu_mask;

	if (!sysctl_sched_energy_aware)
		goto free;

	/* EAS is enabled for asymmetric CPU capacity topologies. */
	if (!per_cpu(sd_asym_cpucapacity, cpu)) {
		if (sched_debug()) {
			pr_info("rd %*pbl: CPUs do not have asymmetric capacities\n",
					cpumask_pr_args(cpu_map));
		}
		goto free;
	}

	for_each_cpu(i, cpu_map) {
		unsigned long scale_cpu = arch_scale_cpu_capacity(i);

		if (scale_cpu < min_cap) {
			cpumask_clear(&min_cpu_mask);
			min_cap = scale_cpu;
		}

		if (scale_cpu == min_cap)
			cpumask_set_cpu(i, &min_cpu_mask);

		/* Skip already covered CPUs. */
		if (find_pd(pd, i))
			continue;

		/* Create the new pd and add it to the local list. */
		tmp = pd_init(i);
		if (!tmp)
			goto free;
		tmp->next = pd;
		pd = tmp;

		/*
		 * Count performance domains and capacity states for the
		 * complexity check.
		 */
		nr_pd++;
		nr_cs += em_pd_nr_cap_states(pd->em_pd);
	}

	if (cpumask_weight(&min_cpu_mask))
		cpumask_copy(&min_cap_cpu_mask, &min_cpu_mask);

	/* Bail out if the Energy Model complexity is too high. */
	if (nr_pd * (nr_cs + nr_cpus) > EM_MAX_COMPLEXITY) {
		WARN(1, "rd %*pbl: Failed to start EAS, EM complexity is too high\n",
						cpumask_pr_args(cpu_map));
		goto free;
	}

	perf_domain_debug(cpu_map, pd);

	/* Attach the new list of performance domains to the root domain. */
	tmp = rd->pd;
	rcu_assign_pointer(rd->pd, pd);
	if (tmp)
		call_rcu(&tmp->rcu, destroy_perf_domain_rcu);

	return !!pd;

free:
	free_pd(pd);
	tmp = rd->pd;
	rcu_assign_pointer(rd->pd, NULL);
	if (tmp)
		call_rcu(&tmp->rcu, destroy_perf_domain_rcu);

	return false;
}
#else
static void free_pd(struct perf_domain *pd) { }
#endif /* CONFIG_ENERGY_MODEL */

static void free_rootdomain(struct rcu_head *rcu)
{
	struct root_domain *rd = container_of(rcu, struct root_domain, rcu);

	cpupri_cleanup(&rd->cpupri);
	cpudl_cleanup(&rd->cpudl);
	free_cpumask_var(rd->dlo_mask);
	free_cpumask_var(rd->rto_mask);
	free_cpumask_var(rd->online);
	free_cpumask_var(rd->span);
	free_pd(rd->pd);
	kfree(rd);
}

void rq_attach_root(struct rq *rq, struct root_domain *rd)
{
	struct root_domain *old_rd = NULL;
	struct rq_flags rf;

	rq_lock_irqsave(rq, &rf);

	if (rq->rd) {
		old_rd = rq->rd;

		if (cpumask_test_cpu(rq->cpu, old_rd->online))
			set_rq_offline(rq);

		cpumask_clear_cpu(rq->cpu, old_rd->span);

		/*
		 * If we dont want to free the old_rd yet then
		 * set old_rd to NULL to skip the freeing later
		 * in this function:
		 */
		if (!atomic_dec_and_test(&old_rd->refcount))
			old_rd = NULL;
	}

	atomic_inc(&rd->refcount);
	rq->rd = rd;

	cpumask_set_cpu(rq->cpu, rd->span);
	if (cpumask_test_cpu(rq->cpu, cpu_active_mask))
		set_rq_online(rq);

	rq_unlock_irqrestore(rq, &rf);

	if (old_rd)
		call_rcu_sched(&old_rd->rcu, free_rootdomain);
}

void sched_get_rd(struct root_domain *rd)
{
	atomic_inc(&rd->refcount);
}

void sched_put_rd(struct root_domain *rd)
{
	if (!atomic_dec_and_test(&rd->refcount))
		return;

	call_rcu_sched(&rd->rcu, free_rootdomain);
}

static int init_rootdomain(struct root_domain *rd)
{
	if (!zalloc_cpumask_var(&rd->span, GFP_KERNEL))
		goto out;
	if (!zalloc_cpumask_var(&rd->online, GFP_KERNEL))
		goto free_span;
	if (!zalloc_cpumask_var(&rd->dlo_mask, GFP_KERNEL))
		goto free_online;
	if (!zalloc_cpumask_var(&rd->rto_mask, GFP_KERNEL))
		goto free_dlo_mask;

#ifdef HAVE_RT_PUSH_IPI
	rd->rto_cpu = -1;
	raw_spin_lock_init(&rd->rto_lock);
	init_irq_work(&rd->rto_push_work, rto_push_irq_work_func);
#endif

	init_dl_bw(&rd->dl_bw);
	if (cpudl_init(&rd->cpudl) != 0)
		goto free_rto_mask;

	if (cpupri_init(&rd->cpupri) != 0)
		goto free_cpudl;

	init_max_cpu_capacity(&rd->max_cpu_capacity);

	return 0;

free_cpudl:
	cpudl_cleanup(&rd->cpudl);
free_rto_mask:
	free_cpumask_var(rd->rto_mask);
free_dlo_mask:
	free_cpumask_var(rd->dlo_mask);
free_online:
	free_cpumask_var(rd->online);
free_span:
	free_cpumask_var(rd->span);
out:
	return -ENOMEM;
}

/*
 * By default the system creates a single root-domain with all CPUs as
 * members (mimicking the global state we have today).
 */
struct root_domain def_root_domain;

void init_defrootdomain(void)
{
	init_rootdomain(&def_root_domain);

	atomic_set(&def_root_domain.refcount, 1);
}

static struct root_domain *alloc_rootdomain(void)
{
	struct root_domain *rd;

	rd = kzalloc(sizeof(*rd), GFP_KERNEL);
	if (!rd)
		return NULL;

	if (init_rootdomain(rd) != 0) {
		kfree(rd);
		return NULL;
	}

	return rd;
}

static void free_sched_groups(struct sched_group *sg, int free_sgc)
{
	struct sched_group *tmp, *first;

	if (!sg)
		return;

	first = sg;
	do {
		tmp = sg->next;

		if (free_sgc && atomic_dec_and_test(&sg->sgc->ref))
			kfree(sg->sgc);

		if (atomic_dec_and_test(&sg->ref))
			kfree(sg);
		sg = tmp;
	} while (sg != first);
}

static void destroy_sched_domain(struct sched_domain *sd)
{
	/*
	 * A normal sched domain may have multiple group references, an
	 * overlapping domain, having private groups, only one.  Iterate,
	 * dropping group/capacity references, freeing where none remain.
	 */
	free_sched_groups(sd->groups, 1);

	if (sd->shared && atomic_dec_and_test(&sd->shared->ref))
		kfree(sd->shared);
	kfree(sd);
}

static void destroy_sched_domains_rcu(struct rcu_head *rcu)
{
	struct sched_domain *sd = container_of(rcu, struct sched_domain, rcu);

	while (sd) {
		struct sched_domain *parent = sd->parent;
		destroy_sched_domain(sd);
		sd = parent;
	}
}

static void destroy_sched_domains(struct sched_domain *sd)
{
	if (sd)
		call_rcu(&sd->rcu, destroy_sched_domains_rcu);
}

/*
 * Keep a special pointer to the highest sched_domain that has
 * SD_SHARE_PKG_RESOURCE set (Last Level Cache Domain) for this
 * allows us to avoid some pointer chasing select_idle_sibling().
 *
 * Also keep a unique ID per domain (we use the first CPU number in
 * the cpumask of the domain), this allows us to quickly tell if
 * two CPUs are in the same cache domain, see cpus_share_cache().
 */
DEFINE_PER_CPU(struct sched_domain *, sd_llc);
DEFINE_PER_CPU(int, sd_llc_size);
DEFINE_PER_CPU(int, sd_llc_id);
DEFINE_PER_CPU(struct sched_domain_shared *, sd_llc_shared);
DEFINE_PER_CPU(struct sched_domain *, sd_numa);
DEFINE_PER_CPU(struct sched_domain *, sd_asym_packing);
DEFINE_PER_CPU(struct sched_domain *, sd_asym_cpucapacity);
DEFINE_STATIC_KEY_FALSE(sched_asym_cpucapacity);

static void update_top_cache_domain(int cpu)
{
	struct sched_domain_shared *sds = NULL;
	struct sched_domain *sd;
	int id = cpu;
	int size = 1;

	sd = highest_flag_domain(cpu, SD_SHARE_PKG_RESOURCES);
	if (sd) {
		id = cpumask_first(sched_domain_span(sd));
		size = cpumask_weight(sched_domain_span(sd));
		sds = sd->shared;
	}

	rcu_assign_pointer(per_cpu(sd_llc, cpu), sd);
	per_cpu(sd_llc_size, cpu) = size;
	per_cpu(sd_llc_id, cpu) = id;
	rcu_assign_pointer(per_cpu(sd_llc_shared, cpu), sds);

	sd = lowest_flag_domain(cpu, SD_NUMA);
	rcu_assign_pointer(per_cpu(sd_numa, cpu), sd);

	sd = highest_flag_domain(cpu, SD_ASYM_PACKING);
	rcu_assign_pointer(per_cpu(sd_asym_packing, cpu), sd);

	sd = lowest_flag_domain(cpu, SD_ASYM_CPUCAPACITY_FULL);
	rcu_assign_pointer(per_cpu(sd_asym_cpucapacity, cpu), sd);
}

/*
 * Attach the domain 'sd' to 'cpu' as its base domain. Callers must
 * hold the hotplug lock.
 */
static void
cpu_attach_domain(struct sched_domain *sd, struct root_domain *rd, int cpu)
{
	struct rq *rq = cpu_rq(cpu);
	struct sched_domain *tmp;

	/* Remove the sched domains which do not contribute to scheduling. */
	for (tmp = sd; tmp; ) {
		struct sched_domain *parent = tmp->parent;
		if (!parent)
			break;

		if (sd_parent_degenerate(tmp, parent)) {
			tmp->parent = parent->parent;
			if (parent->parent)
				parent->parent->child = tmp;
			/*
			 * Transfer SD_PREFER_SIBLING down in case of a
			 * degenerate parent; the spans match for this
			 * so the property transfers.
			 */
			if (parent->flags & SD_PREFER_SIBLING)
				tmp->flags |= SD_PREFER_SIBLING;
			destroy_sched_domain(parent);
		} else
			tmp = tmp->parent;
	}

	if (sd && sd_degenerate(sd)) {
		tmp = sd;
		sd = sd->parent;
		destroy_sched_domain(tmp);
		if (sd)
			sd->child = NULL;
	}

	sched_domain_debug(sd, cpu);

	rq_attach_root(rq, rd);
	tmp = rq->sd;
	rcu_assign_pointer(rq->sd, sd);
	dirty_sched_domain_sysctl(cpu);
	destroy_sched_domains(tmp);

	update_top_cache_domain(cpu);
}

struct s_data {
	struct sched_domain * __percpu *sd;
	struct root_domain	*rd;
};

enum s_alloc {
	sa_rootdomain,
	sa_sd,
	sa_sd_storage,
	sa_none,
};

/*
 * Return the canonical balance CPU for this group, this is the first CPU
 * of this group that's also in the balance mask.
 *
 * The balance mask are all those CPUs that could actually end up at this
 * group. See build_balance_mask().
 *
 * Also see should_we_balance().
 */
int group_balance_cpu(struct sched_group *sg)
{
	return cpumask_first(group_balance_mask(sg));
}


/*
 * NUMA topology (first read the regular topology blurb below)
 *
 * Given a node-distance table, for example:
 *
 *   node   0   1   2   3
 *     0:  10  20  30  20
 *     1:  20  10  20  30
 *     2:  30  20  10  20
 *     3:  20  30  20  10
 *
 * which represents a 4 node ring topology like:
 *
 *   0 ----- 1
 *   |       |
 *   |       |
 *   |       |
 *   3 ----- 2
 *
 * We want to construct domains and groups to represent this. The way we go
 * about doing this is to build the domains on 'hops'. For each NUMA level we
 * construct the mask of all nodes reachable in @level hops.
 *
 * For the above NUMA topology that gives 3 levels:
 *
 * NUMA-2	0-3		0-3		0-3		0-3
 *  groups:	{0-1,3},{1-3}	{0-2},{0,2-3}	{1-3},{0-1,3}	{0,2-3},{0-2}
 *
 * NUMA-1	0-1,3		0-2		1-3		0,2-3
 *  groups:	{0},{1},{3}	{0},{1},{2}	{1},{2},{3}	{0},{2},{3}
 *
 * NUMA-0	0		1		2		3
 *
 *
 * As can be seen; things don't nicely line up as with the regular topology.
 * When we iterate a domain in child domain chunks some nodes can be
 * represented multiple times -- hence the "overlap" naming for this part of
 * the topology.
 *
 * In order to minimize this overlap, we only build enough groups to cover the
 * domain. For instance Node-0 NUMA-2 would only get groups: 0-1,3 and 1-3.
 *
 * Because:
 *
 *  - the first group of each domain is its child domain; this
 *    gets us the first 0-1,3
 *  - the only uncovered node is 2, who's child domain is 1-3.
 *
 * However, because of the overlap, computing a unique CPU for each group is
 * more complicated. Consider for instance the groups of NODE-1 NUMA-2, both
 * groups include the CPUs of Node-0, while those CPUs would not in fact ever
 * end up at those groups (they would end up in group: 0-1,3).
 *
 * To correct this we have to introduce the group balance mask. This mask
 * will contain those CPUs in the group that can reach this group given the
 * (child) domain tree.
 *
 * With this we can once again compute balance_cpu and sched_group_capacity
 * relations.
 *
 * XXX include words on how balance_cpu is unique and therefore can be
 * used for sched_group_capacity links.
 *
 *
 * Another 'interesting' topology is:
 *
 *   node   0   1   2   3
 *     0:  10  20  20  30
 *     1:  20  10  20  20
 *     2:  20  20  10  20
 *     3:  30  20  20  10
 *
 * Which looks a little like:
 *
 *   0 ----- 1
 *   |     / |
 *   |   /   |
 *   | /     |
 *   2 ----- 3
 *
 * This topology is asymmetric, nodes 1,2 are fully connected, but nodes 0,3
 * are not.
 *
 * This leads to a few particularly weird cases where the sched_domain's are
 * not of the same number for each CPU. Consider:
 *
 * NUMA-2	0-3						0-3
 *  groups:	{0-2},{1-3}					{1-3},{0-2}
 *
 * NUMA-1	0-2		0-3		0-3		1-3
 *
 * NUMA-0	0		1		2		3
 *
 */


/*
 * Build the balance mask; it contains only those CPUs that can arrive at this
 * group and should be considered to continue balancing.
 *
 * We do this during the group creation pass, therefore the group information
 * isn't complete yet, however since each group represents a (child) domain we
 * can fully construct this using the sched_domain bits (which are already
 * complete).
 */
static void
build_balance_mask(struct sched_domain *sd, struct sched_group *sg, struct cpumask *mask)
{
	const struct cpumask *sg_span = sched_group_span(sg);
	struct sd_data *sdd = sd->private;
	struct sched_domain *sibling;
	int i;

	cpumask_clear(mask);

	for_each_cpu(i, sg_span) {
		sibling = *per_cpu_ptr(sdd->sd, i);

		/*
		 * Can happen in the asymmetric case, where these siblings are
		 * unused. The mask will not be empty because those CPUs that
		 * do have the top domain _should_ span the domain.
		 */
		if (!sibling->child)
			continue;

		/* If we would not end up here, we can't continue from here */
		if (!cpumask_equal(sg_span, sched_domain_span(sibling->child)))
			continue;

		cpumask_set_cpu(i, mask);
	}

	/* We must not have empty masks here */
	WARN_ON_ONCE(cpumask_empty(mask));
}

/*
 * XXX: This creates per-node group entries; since the load-balancer will
 * immediately access remote memory to construct this group's load-balance
 * statistics having the groups node local is of dubious benefit.
 */
static struct sched_group *
build_group_from_child_sched_domain(struct sched_domain *sd, int cpu)
{
	struct sched_group *sg;
	struct cpumask *sg_span;

	sg = kzalloc_node(sizeof(struct sched_group) + cpumask_size(),
			GFP_KERNEL, cpu_to_node(cpu));

	if (!sg)
		return NULL;

	sg_span = sched_group_span(sg);
	if (sd->child)
		cpumask_copy(sg_span, sched_domain_span(sd->child));
	else
		cpumask_copy(sg_span, sched_domain_span(sd));

	atomic_inc(&sg->ref);
	return sg;
}

static void init_overlap_sched_group(struct sched_domain *sd,
				     struct sched_group *sg)
{
	struct cpumask *mask = sched_domains_tmpmask2;
	struct sd_data *sdd = sd->private;
	struct cpumask *sg_span;
	int cpu;

	build_balance_mask(sd, sg, mask);
	cpu = cpumask_first_and(sched_group_span(sg), mask);

	sg->sgc = *per_cpu_ptr(sdd->sgc, cpu);
	if (atomic_inc_return(&sg->sgc->ref) == 1)
		cpumask_copy(group_balance_mask(sg), mask);
	else
		WARN_ON_ONCE(!cpumask_equal(group_balance_mask(sg), mask));

	/*
	 * Initialize sgc->capacity such that even if we mess up the
	 * domains and no possible iteration will get us here, we won't
	 * die on a /0 trap.
	 */
	sg_span = sched_group_span(sg);
	sg->sgc->capacity = SCHED_CAPACITY_SCALE * cpumask_weight(sg_span);
	sg->sgc->min_capacity = SCHED_CAPACITY_SCALE;
	sg->sgc->max_capacity = SCHED_CAPACITY_SCALE;
}

static int
build_overlap_sched_groups(struct sched_domain *sd, int cpu)
{
	struct sched_group *first = NULL, *last = NULL, *sg;
	const struct cpumask *span = sched_domain_span(sd);
	struct cpumask *covered = sched_domains_tmpmask;
	struct sd_data *sdd = sd->private;
	struct sched_domain *sibling;
	int i;

	cpumask_clear(covered);

	for_each_cpu_wrap(i, span, cpu) {
		struct cpumask *sg_span;

		if (cpumask_test_cpu(i, covered))
			continue;

		sibling = *per_cpu_ptr(sdd->sd, i);

		/*
		 * Asymmetric node setups can result in situations where the
		 * domain tree is of unequal depth, make sure to skip domains
		 * that already cover the entire range.
		 *
		 * In that case build_sched_domains() will have terminated the
		 * iteration early and our sibling sd spans will be empty.
		 * Domains should always include the CPU they're built on, so
		 * check that.
		 */
		if (!cpumask_test_cpu(i, sched_domain_span(sibling)))
			continue;

		sg = build_group_from_child_sched_domain(sibling, cpu);
		if (!sg)
			goto fail;

		sg_span = sched_group_span(sg);
		cpumask_or(covered, covered, sg_span);

		init_overlap_sched_group(sd, sg);

		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
		last->next = first;
	}
	sd->groups = first;

	return 0;

fail:
	free_sched_groups(first, 0);

	return -ENOMEM;
}


/*
 * Package topology (also see the load-balance blurb in fair.c)
 *
 * The scheduler builds a tree structure to represent a number of important
 * topology features. By default (default_topology[]) these include:
 *
 *  - Simultaneous multithreading (SMT)
 *  - Multi-Core Cache (MC)
 *  - Package (DIE)
 *
 * Where the last one more or less denotes everything up to a NUMA node.
 *
 * The tree consists of 3 primary data structures:
 *
 *	sched_domain -> sched_group -> sched_group_capacity
 *	    ^ ^             ^ ^
 *          `-'             `-'
 *
 * The sched_domains are per-CPU and have a two way link (parent & child) and
 * denote the ever growing mask of CPUs belonging to that level of topology.
 *
 * Each sched_domain has a circular (double) linked list of sched_group's, each
 * denoting the domains of the level below (or individual CPUs in case of the
 * first domain level). The sched_group linked by a sched_domain includes the
 * CPU of that sched_domain [*].
 *
 * Take for instance a 2 threaded, 2 core, 2 cache cluster part:
 *
 * CPU   0   1   2   3   4   5   6   7
 *
 * DIE  [                             ]
 * MC   [             ] [             ]
 * SMT  [     ] [     ] [     ] [     ]
 *
 *  - or -
 *
 * DIE  0-7 0-7 0-7 0-7 0-7 0-7 0-7 0-7
 * MC	0-3 0-3 0-3 0-3 4-7 4-7 4-7 4-7
 * SMT  0-1 0-1 2-3 2-3 4-5 4-5 6-7 6-7
 *
 * CPU   0   1   2   3   4   5   6   7
 *
 * One way to think about it is: sched_domain moves you up and down among these
 * topology levels, while sched_group moves you sideways through it, at child
 * domain granularity.
 *
 * sched_group_capacity ensures each unique sched_group has shared storage.
 *
 * There are two related construction problems, both require a CPU that
 * uniquely identify each group (for a given domain):
 *
 *  - The first is the balance_cpu (see should_we_balance() and the
 *    load-balance blub in fair.c); for each group we only want 1 CPU to
 *    continue balancing at a higher domain.
 *
 *  - The second is the sched_group_capacity; we want all identical groups
 *    to share a single sched_group_capacity.
 *
 * Since these topologies are exclusive by construction. That is, its
 * impossible for an SMT thread to belong to multiple cores, and cores to
 * be part of multiple caches. There is a very clear and unique location
 * for each CPU in the hierarchy.
 *
 * Therefore computing a unique CPU for each group is trivial (the iteration
 * mask is redundant and set all 1s; all CPUs in a group will end up at _that_
 * group), we can simply pick the first CPU in each group.
 *
 *
 * [*] in other words, the first group of each domain is its child domain.
 */

static struct sched_group *get_group(int cpu, struct sd_data *sdd)
{
	struct sched_domain *sd = *per_cpu_ptr(sdd->sd, cpu);
	struct sched_domain *child = sd->child;
	struct sched_group *sg;
	bool already_visited;

	if (child)
		cpu = cpumask_first(sched_domain_span(child));

	sg = *per_cpu_ptr(sdd->sg, cpu);
	sg->sgc = *per_cpu_ptr(sdd->sgc, cpu);

	/* Increase refcounts for claim_allocations: */
	already_visited = atomic_inc_return(&sg->ref) > 1;
	/* sgc visits should follow a similar trend as sg */
	WARN_ON(already_visited != (atomic_inc_return(&sg->sgc->ref) > 1));

	/* If we have already visited that group, it's already initialized. */
	if (already_visited)
		return sg;

	if (child) {
		cpumask_copy(sched_group_span(sg), sched_domain_span(child));
		cpumask_copy(group_balance_mask(sg), sched_group_span(sg));
	} else {
		cpumask_set_cpu(cpu, sched_group_span(sg));
		cpumask_set_cpu(cpu, group_balance_mask(sg));
	}

	sg->sgc->capacity = SCHED_CAPACITY_SCALE * cpumask_weight(sched_group_span(sg));
	sg->sgc->min_capacity = SCHED_CAPACITY_SCALE;
	sg->sgc->max_capacity = SCHED_CAPACITY_SCALE;

	return sg;
}

/*
 * build_sched_groups will build a circular linked list of the groups
 * covered by the given span, and will set each group's ->cpumask correctly,
 * and ->cpu_capacity to 0.
 *
 * Assumes the sched_domain tree is fully constructed
 */
static int
build_sched_groups(struct sched_domain *sd, int cpu)
{
	struct sched_group *first = NULL, *last = NULL;
	struct sd_data *sdd = sd->private;
	const struct cpumask *span = sched_domain_span(sd);
	struct cpumask *covered;
	int i;

	lockdep_assert_held(&sched_domains_mutex);
	covered = sched_domains_tmpmask;

	cpumask_clear(covered);

	for_each_cpu_wrap(i, span, cpu) {
		struct sched_group *sg;

		if (cpumask_test_cpu(i, covered))
			continue;

		sg = get_group(i, sdd);

		cpumask_or(covered, covered, sched_group_span(sg));

		if (!first)
			first = sg;
		if (last)
			last->next = sg;
		last = sg;
	}
	last->next = first;
	sd->groups = first;

	return 0;
}

/*
 * Initialize sched groups cpu_capacity.
 *
 * cpu_capacity indicates the capacity of sched group, which is used while
 * distributing the load between different sched groups in a sched domain.
 * Typically cpu_capacity for all the groups in a sched domain will be same
 * unless there are asymmetries in the topology. If there are asymmetries,
 * group having more cpu_capacity will pickup more load compared to the
 * group having less cpu_capacity.
 */
void init_sched_groups_capacity(int cpu, struct sched_domain *sd)
{
	struct sched_group *sg = sd->groups;
	WARN_ON(!sg);

	do {
		int cpu, max_cpu = -1;

		sg->group_weight = cpumask_weight(sched_group_span(sg));
		if (!(sd->flags & SD_ASYM_PACKING))
			goto next;

		for_each_cpu(cpu, sched_group_span(sg)) {
			if (max_cpu < 0)
				max_cpu = cpu;
			else if (sched_asym_prefer(cpu, max_cpu))
				max_cpu = cpu;
		}
		sg->asym_prefer_cpu = max_cpu;

next:
		sg = sg->next;
	} while (sg != sd->groups);

	if (cpu != group_balance_cpu(sg))
		return;

	update_group_capacity(sd, cpu);
}

/*
 * Set of available CPUs grouped by their corresponding capacities
 * Each list entry contains a CPU mask reflecting CPUs that share the same
 * capacity.
 * The lifespan of data is unlimited.
 */
LIST_HEAD(asym_cap_list);

/*
 * Verify whether there is any CPU capacity asymmetry in a given sched domain.
 * Provides sd_flags reflecting the asymmetry scope.
 */
static inline int
asym_cpu_capacity_classify(const struct cpumask *sd_span,
			   const struct cpumask *cpu_map)
{
	struct asym_cap_data *entry;
	int count = 0, miss = 0;

	/*
	 * Count how many unique CPU capacities this domain spans across
	 * (compare sched_domain CPUs mask with ones representing  available
	 * CPUs capacities). Take into account CPUs that might be offline:
	 * skip those.
	 */
	list_for_each_entry(entry, &asym_cap_list, link) {
		if (cpumask_intersects(sd_span, cpu_capacity_span(entry)))
			++count;
		else if (cpumask_intersects(cpu_map, cpu_capacity_span(entry)))
			++miss;
	}

	WARN_ON_ONCE(!count && !list_empty(&asym_cap_list));

	/* No asymmetry detected */
	if (count < 2)
		return 0;
	/* Some of the available CPU capacity values have not been detected */
	if (miss)
		return SD_ASYM_CPUCAPACITY;

	/* Full asymmetry */
	return SD_ASYM_CPUCAPACITY | SD_ASYM_CPUCAPACITY_FULL;

}

static void free_asym_cap_entry(struct rcu_head *head)
{
	struct asym_cap_data *entry = container_of(head, struct asym_cap_data, rcu);
	kfree(entry);
}

static inline void asym_cpu_capacity_update_data(int cpu)
{
	unsigned long capacity = arch_scale_cpu_capacity(cpu);
	struct asym_cap_data *insert_entry = NULL;
	struct asym_cap_data *entry;
 
	/*
	 * Search if capacity already exits. If not, track which the entry
	 * where we should insert to keep the list ordered descendingly.
	 */

	list_for_each_entry(entry, &asym_cap_list, link) {
		if (capacity == entry->capacity)
			goto done;
		else if (!insert_entry && capacity > entry->capacity)
			insert_entry = list_prev_entry(entry, link);
	}

	entry = kzalloc(sizeof(*entry) + cpumask_size(), GFP_KERNEL);
	if (WARN_ONCE(!entry, "Failed to allocate memory for asymmetry data\n"))
		return;
	entry->capacity = capacity;

	/* If NULL then the new capacity is the smallest, add last. */
	if (!insert_entry)
		list_add_tail_rcu(&entry->link, &asym_cap_list);
	else
		list_add_rcu(&entry->link, &insert_entry->link);
done:
	__cpumask_set_cpu(cpu, cpu_capacity_span(entry));
}

/*
 * Build-up/update list of CPUs grouped by their capacities
 * An update requires explicit request to rebuild sched domains
 * with state indicating CPU topology changes.
 */
static void asym_cpu_capacity_scan(void)
{
	struct asym_cap_data *entry, *next;
	int cpu;

	list_for_each_entry(entry, &asym_cap_list, link)
		cpumask_clear(cpu_capacity_span(entry));

	for_each_cpu_and(cpu, cpu_possible_mask, housekeeping_cpumask(HK_FLAG_DOMAIN))
		asym_cpu_capacity_update_data(cpu);

	list_for_each_entry_safe(entry, next, &asym_cap_list, link) {
		if (cpumask_empty(cpu_capacity_span(entry))) {
			list_del_rcu(&entry->link);
			call_rcu(&entry->rcu, free_asym_cap_entry);
		}
	}

	/*
	 * Only one capacity value has been detected i.e. this system is symmetric.
	 * No need to keep this data around.
	 */
	if (list_is_singular(&asym_cap_list)) {
		entry = list_first_entry(&asym_cap_list, typeof(*entry), link);
		list_del_rcu(&entry->link);
		call_rcu(&entry->rcu, free_asym_cap_entry);
	}
}

/*
 * Initializers for schedule domains
 * Non-inlined to reduce accumulated stack pressure in build_sched_domains()
 */

static int default_relax_domain_level = -1;
int sched_domain_level_max;

static int __init setup_relax_domain_level(char *str)
{
	if (kstrtoint(str, 0, &default_relax_domain_level))
		pr_warn("Unable to set relax_domain_level\n");

	return 1;
}
__setup("relax_domain_level=", setup_relax_domain_level);

static void set_domain_attribute(struct sched_domain *sd,
				 struct sched_domain_attr *attr)
{
	int request;

	if (!attr || attr->relax_domain_level < 0) {
		if (default_relax_domain_level < 0)
			return;
		request = default_relax_domain_level;
	} else
		request = attr->relax_domain_level;

	if (sd->level >= request) {
		/* Turn off idle balance on this domain: */
		sd->flags &= ~(SD_BALANCE_WAKE|SD_BALANCE_NEWIDLE);
	}
}

static void __sdt_free(const struct cpumask *cpu_map);
static int __sdt_alloc(const struct cpumask *cpu_map);

static void __free_domain_allocs(struct s_data *d, enum s_alloc what,
				 const struct cpumask *cpu_map)
{
	switch (what) {
	case sa_rootdomain:
		if (!atomic_read(&d->rd->refcount))
			free_rootdomain(&d->rd->rcu);
		/* Fall through */
	case sa_sd:
		free_percpu(d->sd);
		/* Fall through */
	case sa_sd_storage:
		__sdt_free(cpu_map);
		/* Fall through */
	case sa_none:
		break;
	}
}

static enum s_alloc
__visit_domain_allocation_hell(struct s_data *d, const struct cpumask *cpu_map)
{
	memset(d, 0, sizeof(*d));

	if (__sdt_alloc(cpu_map))
		return sa_sd_storage;
	d->sd = alloc_percpu(struct sched_domain *);
	if (!d->sd)
		return sa_sd_storage;
	d->rd = alloc_rootdomain();
	if (!d->rd)
		return sa_sd;

	return sa_rootdomain;
}

/*
 * NULL the sd_data elements we've used to build the sched_domain and
 * sched_group structure so that the subsequent __free_domain_allocs()
 * will not free the data we're using.
 */
static void claim_allocations(int cpu, struct sched_domain *sd)
{
	struct sd_data *sdd = sd->private;

	WARN_ON_ONCE(*per_cpu_ptr(sdd->sd, cpu) != sd);
	*per_cpu_ptr(sdd->sd, cpu) = NULL;

	if (atomic_read(&(*per_cpu_ptr(sdd->sds, cpu))->ref))
		*per_cpu_ptr(sdd->sds, cpu) = NULL;

	if (atomic_read(&(*per_cpu_ptr(sdd->sg, cpu))->ref))
		*per_cpu_ptr(sdd->sg, cpu) = NULL;

	if (atomic_read(&(*per_cpu_ptr(sdd->sgc, cpu))->ref))
		*per_cpu_ptr(sdd->sgc, cpu) = NULL;
}

#ifdef CONFIG_NUMA
enum numa_topology_type sched_numa_topology_type;

static int			sched_domains_numa_levels;
static int			sched_domains_curr_level;

int				sched_max_numa_distance;
static int			*sched_domains_numa_distance;
static struct cpumask		***sched_domains_numa_masks;
#endif

/*
 * SD_flags allowed in topology descriptions.
 *
 * These flags are purely descriptive of the topology and do not prescribe
 * behaviour. Behaviour is artificial and mapped in the below sd_init()
 * function:
 *
 *   SD_SHARE_CPUCAPACITY   - describes SMT topologies
 *   SD_SHARE_PKG_RESOURCES - describes shared caches
 *   SD_NUMA                - describes NUMA topologies
 *   SD_SHARE_POWERDOMAIN   - describes shared power domain
 *
 * Odd one out, which beside describing the topology has a quirk also
 * prescribes the desired behaviour that goes along with it:
 *
 *   SD_ASYM_PACKING        - describes SMT quirks
 */
#define TOPOLOGY_SD_FLAGS		\
	(SD_SHARE_CPUCAPACITY	|	\
	 SD_SHARE_PKG_RESOURCES |	\
	 SD_NUMA		|	\
	 SD_ASYM_PACKING	|	\
	 SD_SHARE_POWERDOMAIN)

static struct sched_domain *
sd_init(struct sched_domain_topology_level *tl,
	const struct cpumask *cpu_map,
	struct sched_domain *child, int cpu)
{
	struct sd_data *sdd = &tl->data;
	struct sched_domain *sd = *per_cpu_ptr(sdd->sd, cpu);
	int sd_id, sd_weight, sd_flags = 0;
	struct cpumask *sd_span;

#ifdef CONFIG_NUMA
	/*
	 * Ugly hack to pass state to sd_numa_mask()...
	 */
	sched_domains_curr_level = tl->numa_level;
#endif

	sd_weight = cpumask_weight(tl->mask(cpu));

	if (tl->sd_flags)
		sd_flags = (*tl->sd_flags)();
	if (WARN_ONCE(sd_flags & ~TOPOLOGY_SD_FLAGS,
			"wrong sd_flags in topology description\n"))
		sd_flags &= TOPOLOGY_SD_FLAGS;

	*sd = (struct sched_domain){
		.min_interval		= sd_weight,
		.max_interval		= 2*sd_weight,
		.busy_factor		= 16,
		.imbalance_pct		= 117,

		.cache_nice_tries	= 0,

		.flags			= 1*SD_BALANCE_NEWIDLE
					| 1*SD_BALANCE_EXEC
					| 1*SD_BALANCE_FORK
					| 0*SD_BALANCE_WAKE
					| 1*SD_WAKE_AFFINE
					| 0*SD_SHARE_CPUCAPACITY
					| 0*SD_SHARE_PKG_RESOURCES
					| 0*SD_SERIALIZE
					| 1*SD_PREFER_SIBLING
					| 0*SD_NUMA
					| sd_flags
					,

		.last_balance		= jiffies,
		.balance_interval	= sd_weight,
		.smt_gain		= 0,
		.max_newidle_lb_cost	= 0,
		.last_decay_max_lb_cost	= jiffies,
		.child			= child,
#ifdef CONFIG_SCHED_DEBUG
		.name			= tl->name,
#endif
	};

	sd_span = sched_domain_span(sd);
	cpumask_and(sd_span, cpu_map, tl->mask(cpu));
	sd_id = cpumask_first(sd_span);

	sd->flags |= asym_cpu_capacity_classify(sd_span, cpu_map);

	WARN_ONCE((sd->flags & (SD_SHARE_CPUCAPACITY | SD_ASYM_CPUCAPACITY)) ==
		  (SD_SHARE_CPUCAPACITY | SD_ASYM_CPUCAPACITY),
		  "CPU capacity asymmetry not supported on SMT\n");

	/*
	 * Convert topological properties into behaviour.
	 */
	/* Don't attempt to spread across CPUs of different capacities. */
	if ((sd->flags & SD_ASYM_CPUCAPACITY) && sd->child)
		sd->child->flags &= ~SD_PREFER_SIBLING;

	if (sd->flags & SD_SHARE_CPUCAPACITY) {
		sd->imbalance_pct = 110;
		sd->smt_gain = 1178; /* ~15% */

	} else if (sd->flags & SD_SHARE_PKG_RESOURCES) {
		sd->imbalance_pct = 117;
		sd->cache_nice_tries = 1;

#ifdef CONFIG_NUMA
	} else if (sd->flags & SD_NUMA) {
		sd->cache_nice_tries = 2;

		sd->flags &= ~SD_PREFER_SIBLING;
		sd->flags |= SD_SERIALIZE;
		if (sched_domains_numa_distance[tl->numa_level] > RECLAIM_DISTANCE) {
			sd->flags &= ~(SD_BALANCE_EXEC |
				       SD_BALANCE_FORK |
				       SD_WAKE_AFFINE);
		}

#endif
	} else {
		sd->cache_nice_tries = 1;
	}

	/*
	 * For all levels sharing cache; connect a sched_domain_shared
	 * instance.
	 */
	if (sd->flags & SD_SHARE_PKG_RESOURCES) {
		sd->shared = *per_cpu_ptr(sdd->sds, sd_id);
		atomic_inc(&sd->shared->ref);
		atomic_set(&sd->shared->nr_busy_cpus, sd_weight);
	}

	sd->private = sdd;

	return sd;
}

/*
 * Topology list, bottom-up.
 */
static struct sched_domain_topology_level default_topology[] = {
#ifdef CONFIG_SCHED_SMT
	{ cpu_smt_mask, cpu_smt_flags, SD_INIT_NAME(SMT) },
#endif
#ifdef CONFIG_SCHED_MC
	{ cpu_coregroup_mask, cpu_core_flags, SD_INIT_NAME(MC) },
#endif
	{ cpu_cpu_mask, SD_INIT_NAME(DIE) },
	{ NULL, },
};

static struct sched_domain_topology_level *sched_domain_topology =
	default_topology;

#define for_each_sd_topology(tl)			\
	for (tl = sched_domain_topology; tl->mask; tl++)

void set_sched_topology(struct sched_domain_topology_level *tl)
{
	if (WARN_ON_ONCE(sched_smp_initialized))
		return;

	sched_domain_topology = tl;
}

#ifdef CONFIG_NUMA

static const struct cpumask *sd_numa_mask(int cpu)
{
	return sched_domains_numa_masks[sched_domains_curr_level][cpu_to_node(cpu)];
}

static void sched_numa_warn(const char *str)
{
	static int done = false;
	int i,j;

	if (done)
		return;

	done = true;

	printk(KERN_WARNING "ERROR: %s\n\n", str);

	for (i = 0; i < nr_node_ids; i++) {
		printk(KERN_WARNING "  ");
		for (j = 0; j < nr_node_ids; j++)
			printk(KERN_CONT "%02d ", node_distance(i,j));
		printk(KERN_CONT "\n");
	}
	printk(KERN_WARNING "\n");
}

bool find_numa_distance(int distance)
{
	int i;

	if (distance == node_distance(0, 0))
		return true;

	for (i = 0; i < sched_domains_numa_levels; i++) {
		if (sched_domains_numa_distance[i] == distance)
			return true;
	}

	return false;
}

/*
 * A system can have three types of NUMA topology:
 * NUMA_DIRECT: all nodes are directly connected, or not a NUMA system
 * NUMA_GLUELESS_MESH: some nodes reachable through intermediary nodes
 * NUMA_BACKPLANE: nodes can reach other nodes through a backplane
 *
 * The difference between a glueless mesh topology and a backplane
 * topology lies in whether communication between not directly
 * connected nodes goes through intermediary nodes (where programs
 * could run), or through backplane controllers. This affects
 * placement of programs.
 *
 * The type of topology can be discerned with the following tests:
 * - If the maximum distance between any nodes is 1 hop, the system
 *   is directly connected.
 * - If for two nodes A and B, located N > 1 hops away from each other,
 *   there is an intermediary node C, which is < N hops away from both
 *   nodes A and B, the system is a glueless mesh.
 */
static void init_numa_topology_type(void)
{
	int a, b, c, n;

	n = sched_max_numa_distance;

	if (sched_domains_numa_levels <= 2) {
		sched_numa_topology_type = NUMA_DIRECT;
		return;
	}

	for_each_online_node(a) {
		for_each_online_node(b) {
			/* Find two nodes furthest removed from each other. */
			if (node_distance(a, b) < n)
				continue;

			/* Is there an intermediary node between a and b? */
			for_each_online_node(c) {
				if (node_distance(a, c) < n &&
				    node_distance(b, c) < n) {
					sched_numa_topology_type =
							NUMA_GLUELESS_MESH;
					return;
				}
			}

			sched_numa_topology_type = NUMA_BACKPLANE;
			return;
		}
	}
}


#define NR_DISTANCE_VALUES (1 << DISTANCE_BITS)

void sched_init_numa(void)
{
	struct sched_domain_topology_level *tl;
	unsigned long *distance_map;
	int nr_levels = 0;
	int i, j;

	/*
	 * O(nr_nodes^2) deduplicating selection sort -- in order to find the
	 * unique distances in the node_distance() table.
	 */
	distance_map = bitmap_alloc(NR_DISTANCE_VALUES, GFP_KERNEL);
	if (!distance_map)
		return;

	bitmap_zero(distance_map, NR_DISTANCE_VALUES);
	for (i = 0; i < nr_node_ids; i++) {
		for (j = 0; j < nr_node_ids; j++) {
			int distance = node_distance(i, j);

			if (distance < LOCAL_DISTANCE || distance >= NR_DISTANCE_VALUES) {
				sched_numa_warn("Invalid distance value range");
				return;
			}

			bitmap_set(distance_map, distance, 1);
		}
	}
	/*
	 * We can now figure out how many unique distance values there are and
	 * allocate memory accordingly.
	 */
	nr_levels = bitmap_weight(distance_map, NR_DISTANCE_VALUES);

	sched_domains_numa_distance = kcalloc(nr_levels, sizeof(int), GFP_KERNEL);
	if (!sched_domains_numa_distance) {
		bitmap_free(distance_map);
		return;
	}

	for (i = 0, j = 0; i < nr_levels; i++, j++) {
		j = find_next_bit(distance_map, NR_DISTANCE_VALUES, j);
		sched_domains_numa_distance[i] = j;
	}

	bitmap_free(distance_map);

	/*
	 * 'nr_levels' contains the number of unique distances
	 *
	 * The sched_domains_numa_distance[] array includes the actual distance
	 * numbers.
	 */

	/*
	 * Here, we should temporarily reset sched_domains_numa_levels to 0.
	 * If it fails to allocate memory for array sched_domains_numa_masks[][],
	 * the array will contain less then 'nr_levels' members. This could be
	 * dangerous when we use it to iterate array sched_domains_numa_masks[][]
	 * in other functions.
	 *
	 * We reset it to 'nr_levels' at the end of this function.
	 */
	sched_domains_numa_levels = 0;

	sched_domains_numa_masks = kzalloc(sizeof(void *) * nr_levels, GFP_KERNEL);
	if (!sched_domains_numa_masks)
		return;

	/*
	 * Now for each level, construct a mask per node which contains all
	 * CPUs of nodes that are that many hops away from us.
	 */
	for (i = 0; i < nr_levels; i++) {
		sched_domains_numa_masks[i] =
			kzalloc(nr_node_ids * sizeof(void *), GFP_KERNEL);
		if (!sched_domains_numa_masks[i])
			return;

		for (j = 0; j < nr_node_ids; j++) {
			struct cpumask *mask = kzalloc(cpumask_size(), GFP_KERNEL);
			int k;

			if (!mask)
				return;

			sched_domains_numa_masks[i][j] = mask;

			for_each_node(k) {
				if (sched_debug() && (node_distance(j, k) != node_distance(k, j)))
					sched_numa_warn("Node-distance not symmetric");

				if (node_distance(j, k) > sched_domains_numa_distance[i])
					continue;

				cpumask_or(mask, mask, cpumask_of_node(k));
			}
		}
	}

	/* Compute default topology size */
	for (i = 0; sched_domain_topology[i].mask; i++);

	tl = kzalloc((i + nr_levels + 1) *
			sizeof(struct sched_domain_topology_level), GFP_KERNEL);
	if (!tl)
		return;

	/*
	 * Copy the default topology bits..
	 */
	for (i = 0; sched_domain_topology[i].mask; i++)
		tl[i] = sched_domain_topology[i];

	/*
	 * Add the NUMA identity distance, aka single NODE.
	 */
	tl[i++] = (struct sched_domain_topology_level){
		.mask = sd_numa_mask,
		.numa_level = 0,
		SD_INIT_NAME(NODE)
	};

	/*
	 * .. and append 'j' levels of NUMA goodness.
	 */
	for (j = 1; j < nr_levels; i++, j++) {
		tl[i] = (struct sched_domain_topology_level){
			.mask = sd_numa_mask,
			.sd_flags = cpu_numa_flags,
			.flags = SDTL_OVERLAP,
			.numa_level = j,
			SD_INIT_NAME(NUMA)
		};
	}

	sched_domain_topology = tl;

	sched_domains_numa_levels = nr_levels;
	sched_max_numa_distance = sched_domains_numa_distance[nr_levels - 1];

	init_numa_topology_type();
}

void sched_domains_numa_masks_set(unsigned int cpu)
{
	int node = cpu_to_node(cpu);
	int i, j;

	for (i = 0; i < sched_domains_numa_levels; i++) {
		for (j = 0; j < nr_node_ids; j++) {
			if (node_distance(j, node) <= sched_domains_numa_distance[i])
				cpumask_set_cpu(cpu, sched_domains_numa_masks[i][j]);
		}
	}
}

void sched_domains_numa_masks_clear(unsigned int cpu)
{
	int i, j;

	for (i = 0; i < sched_domains_numa_levels; i++) {
		for (j = 0; j < nr_node_ids; j++)
			cpumask_clear_cpu(cpu, sched_domains_numa_masks[i][j]);
	}
}

#endif /* CONFIG_NUMA */

static int __sdt_alloc(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	int j;

	for_each_sd_topology(tl) {
		struct sd_data *sdd = &tl->data;

		sdd->sd = alloc_percpu(struct sched_domain *);
		if (!sdd->sd)
			return -ENOMEM;

		sdd->sds = alloc_percpu(struct sched_domain_shared *);
		if (!sdd->sds)
			return -ENOMEM;

		sdd->sg = alloc_percpu(struct sched_group *);
		if (!sdd->sg)
			return -ENOMEM;

		sdd->sgc = alloc_percpu(struct sched_group_capacity *);
		if (!sdd->sgc)
			return -ENOMEM;

		for_each_cpu(j, cpu_map) {
			struct sched_domain *sd;
			struct sched_domain_shared *sds;
			struct sched_group *sg;
			struct sched_group_capacity *sgc;

			sd = kzalloc_node(sizeof(struct sched_domain) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sd)
				return -ENOMEM;

			*per_cpu_ptr(sdd->sd, j) = sd;

			sds = kzalloc_node(sizeof(struct sched_domain_shared),
					GFP_KERNEL, cpu_to_node(j));
			if (!sds)
				return -ENOMEM;

			*per_cpu_ptr(sdd->sds, j) = sds;

			sg = kzalloc_node(sizeof(struct sched_group) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sg)
				return -ENOMEM;

			sg->next = sg;

			*per_cpu_ptr(sdd->sg, j) = sg;

			sgc = kzalloc_node(sizeof(struct sched_group_capacity) + cpumask_size(),
					GFP_KERNEL, cpu_to_node(j));
			if (!sgc)
				return -ENOMEM;

#ifdef CONFIG_SCHED_DEBUG
			sgc->id = j;
#endif

			*per_cpu_ptr(sdd->sgc, j) = sgc;
		}
	}

	return 0;
}

static void __sdt_free(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	int j;

	for_each_sd_topology(tl) {
		struct sd_data *sdd = &tl->data;

		for_each_cpu(j, cpu_map) {
			struct sched_domain *sd;

			if (sdd->sd) {
				sd = *per_cpu_ptr(sdd->sd, j);
				if (sd && (sd->flags & SD_OVERLAP))
					free_sched_groups(sd->groups, 0);
				kfree(*per_cpu_ptr(sdd->sd, j));
			}

			if (sdd->sds)
				kfree(*per_cpu_ptr(sdd->sds, j));
			if (sdd->sg)
				kfree(*per_cpu_ptr(sdd->sg, j));
			if (sdd->sgc)
				kfree(*per_cpu_ptr(sdd->sgc, j));
		}
		free_percpu(sdd->sd);
		sdd->sd = NULL;
		free_percpu(sdd->sds);
		sdd->sds = NULL;
		free_percpu(sdd->sg);
		sdd->sg = NULL;
		free_percpu(sdd->sgc);
		sdd->sgc = NULL;
	}
}

static struct sched_domain *build_sched_domain(struct sched_domain_topology_level *tl,
		const struct cpumask *cpu_map, struct sched_domain_attr *attr,
		struct sched_domain *child, int cpu)
{
	struct sched_domain *sd = sd_init(tl, cpu_map, child, cpu);

	if (child) {
		sd->level = child->level + 1;
		sched_domain_level_max = max(sched_domain_level_max, sd->level);
		child->parent = sd;

		if (!cpumask_subset(sched_domain_span(child),
				    sched_domain_span(sd))) {
			pr_err("BUG: arch topology borken\n");
#ifdef CONFIG_SCHED_DEBUG
			pr_err("     the %s domain not a subset of the %s domain\n",
					child->name, sd->name);
#endif
			/* Fixup, ensure @sd has at least @child CPUs. */
			cpumask_or(sched_domain_span(sd),
				   sched_domain_span(sd),
				   sched_domain_span(child));
		}

	}
	set_domain_attribute(sd, attr);

	return sd;
}

/*
 * Ensure topology masks are sane, i.e. there are no conflicts (overlaps) for
 * any two given CPUs on non-NUMA topology levels.
 */
static bool topology_span_sane(const struct cpumask *cpu_map)
{
	struct sched_domain_topology_level *tl;
	struct cpumask *covered, *id_seen;
	int cpu;

	lockdep_assert_held(&sched_domains_mutex);
	covered = sched_domains_tmpmask;
	id_seen = sched_domains_tmpmask2;

	for_each_sd_topology(tl) {

		/* NUMA levels are allowed to overlap */
		if (tl->flags & SDTL_OVERLAP)
			continue;

		cpumask_clear(covered);
		cpumask_clear(id_seen);

		/*
		 * Non-NUMA levels cannot partially overlap - they must be either
		 * completely equal or completely disjoint. Otherwise we can end up
		 * breaking the sched_group lists - i.e. a later get_group() pass
		 * breaks the linking done for an earlier span.
		 */
		for_each_cpu(cpu, cpu_map) {
			const struct cpumask *tl_cpu_mask = tl->mask(cpu);
			int id;

			/* lowest bit set in this mask is used as a unique id */
			id = cpumask_first(tl_cpu_mask);

			if (cpumask_test_cpu(id, id_seen)) {
				/* First CPU has already been seen, ensure identical spans */
				if (!cpumask_equal(tl->mask(id), tl_cpu_mask))
					return false;
			} else {
				/* First CPU hasn't been seen before, ensure it's a completely new span */
				if (cpumask_intersects(tl_cpu_mask, covered))
					return false;

				cpumask_or(covered, covered, tl_cpu_mask);
				cpumask_set_cpu(id, id_seen);
			}
		}
	}
	return true;
}

/*
 * Build sched domains for a given set of CPUs and attach the sched domains
 * to the individual CPUs
 */
static int
build_sched_domains(const struct cpumask *cpu_map, struct sched_domain_attr *attr)
{
	enum s_alloc alloc_state = sa_none;
	struct sched_domain *sd;
	struct s_data d;
	int i, ret = -ENOMEM;
	bool has_asym = false;

	if (WARN_ON(cpumask_empty(cpu_map)))
		goto error;

	alloc_state = __visit_domain_allocation_hell(&d, cpu_map);
	if (alloc_state != sa_rootdomain)
		goto error;

	/* Set up domains for CPUs specified by the cpu_map: */
	for_each_cpu(i, cpu_map) {
		struct sched_domain_topology_level *tl;

		sd = NULL;
		for_each_sd_topology(tl) {
			sd = build_sched_domain(tl, cpu_map, attr, sd, i);

			has_asym |= sd->flags & SD_ASYM_CPUCAPACITY;

			if (tl == sched_domain_topology)
				*per_cpu_ptr(d.sd, i) = sd;
			if (tl->flags & SDTL_OVERLAP)
				sd->flags |= SD_OVERLAP;
			if (cpumask_equal(cpu_map, sched_domain_span(sd)))
				break;
		}
	}

	if (WARN_ON(!topology_span_sane(cpu_map)))
		goto error;

	/* Build the groups for the domains */
	for_each_cpu(i, cpu_map) {
		for (sd = *per_cpu_ptr(d.sd, i); sd; sd = sd->parent) {
			sd->span_weight = cpumask_weight(sched_domain_span(sd));
			if (sd->flags & SD_OVERLAP) {
				if (build_overlap_sched_groups(sd, i))
					goto error;
			} else {
				if (build_sched_groups(sd, i))
					goto error;
			}
		}
	}

	/* Calculate CPU capacity for physical packages and nodes */
	for (i = nr_cpumask_bits-1; i >= 0; i--) {
		if (!cpumask_test_cpu(i, cpu_map))
			continue;

		for (sd = *per_cpu_ptr(d.sd, i); sd; sd = sd->parent) {
			claim_allocations(i, sd);
			init_sched_groups_capacity(i, sd);
		}
	}

	/* Attach the domains */
	rcu_read_lock();
	for_each_cpu(i, cpu_map) {
		sd = *per_cpu_ptr(d.sd, i);
		cpu_attach_domain(sd, d.rd, i);
	}

	rcu_read_unlock();

	if (has_asym)
		static_branch_inc_cpuslocked(&sched_asym_cpucapacity);

	ret = 0;
error:
	__free_domain_allocs(&d, alloc_state, cpu_map);

	return ret;
}

/* Current sched domains: */
static cpumask_var_t			*doms_cur;

/* Number of sched domains in 'doms_cur': */
static int				ndoms_cur;

/* Attribues of custom domains in 'doms_cur' */
static struct sched_domain_attr		*dattr_cur;

/*
 * Special case: If a kmalloc() of a doms_cur partition (array of
 * cpumask) fails, then fallback to a single sched domain,
 * as determined by the single cpumask fallback_doms.
 */
static cpumask_var_t			fallback_doms;

/*
 * arch_update_cpu_topology lets virtualized architectures update the
 * CPU core maps. It is supposed to return 1 if the topology changed
 * or 0 if it stayed the same.
 */
int __weak arch_update_cpu_topology(void)
{
	return 0;
}

cpumask_var_t *alloc_sched_domains(unsigned int ndoms)
{
	int i;
	cpumask_var_t *doms;

	doms = kmalloc_array(ndoms, sizeof(*doms), GFP_KERNEL);
	if (!doms)
		return NULL;
	for (i = 0; i < ndoms; i++) {
		if (!alloc_cpumask_var(&doms[i], GFP_KERNEL)) {
			free_sched_domains(doms, i);
			return NULL;
		}
	}
	return doms;
}

void free_sched_domains(cpumask_var_t doms[], unsigned int ndoms)
{
	unsigned int i;
	for (i = 0; i < ndoms; i++)
		free_cpumask_var(doms[i]);
	kfree(doms);
}

/*
 * Set up scheduler domains and groups. Callers must hold the hotplug lock.
 * For now this just excludes isolated CPUs, but could be used to
 * exclude other special cases in the future.
 */
int sched_init_domains(const struct cpumask *cpu_map)
{
	int err;

	zalloc_cpumask_var(&sched_domains_tmpmask, GFP_KERNEL);
	zalloc_cpumask_var(&sched_domains_tmpmask2, GFP_KERNEL);
	zalloc_cpumask_var(&fallback_doms, GFP_KERNEL);

	arch_update_cpu_topology();
	asym_cpu_capacity_scan();
	ndoms_cur = 1;
	doms_cur = alloc_sched_domains(ndoms_cur);
	if (!doms_cur)
		doms_cur = &fallback_doms;
	cpumask_and(doms_cur[0], cpu_map, housekeeping_cpumask(HK_FLAG_DOMAIN));
	err = build_sched_domains(doms_cur[0], NULL);
	register_sched_domain_sysctl();

	return err;
}

/*
 * Detach sched domains from a group of CPUs specified in cpu_map
 * These CPUs will now be attached to the NULL domain
 */
static void detach_destroy_domains(const struct cpumask *cpu_map)
{
	unsigned int cpu = cpumask_any(cpu_map);
	int i;

	if (rcu_access_pointer(per_cpu(sd_asym_cpucapacity, cpu)))
		static_branch_dec_cpuslocked(&sched_asym_cpucapacity);

	rcu_read_lock();
	for_each_cpu(i, cpu_map)
		cpu_attach_domain(NULL, &def_root_domain, i);
	rcu_read_unlock();
}

/* handle null as "default" */
static int dattrs_equal(struct sched_domain_attr *cur, int idx_cur,
			struct sched_domain_attr *new, int idx_new)
{
	struct sched_domain_attr tmp;

	/* Fast path: */
	if (!new && !cur)
		return 1;

	tmp = SD_ATTR_INIT;

	return !memcmp(cur ? (cur + idx_cur) : &tmp,
			new ? (new + idx_new) : &tmp,
			sizeof(struct sched_domain_attr));
}

/*
 * Partition sched domains as specified by the 'ndoms_new'
 * cpumasks in the array doms_new[] of cpumasks. This compares
 * doms_new[] to the current sched domain partitioning, doms_cur[].
 * It destroys each deleted domain and builds each new domain.
 *
 * 'doms_new' is an array of cpumask_var_t's of length 'ndoms_new'.
 * The masks don't intersect (don't overlap.) We should setup one
 * sched domain for each mask. CPUs not in any of the cpumasks will
 * not be load balanced. If the same cpumask appears both in the
 * current 'doms_cur' domains and in the new 'doms_new', we can leave
 * it as it is.
 *
 * The passed in 'doms_new' should be allocated using
 * alloc_sched_domains.  This routine takes ownership of it and will
 * free_sched_domains it when done with it. If the caller failed the
 * alloc call, then it can pass in doms_new == NULL && ndoms_new == 1,
 * and partition_sched_domains() will fallback to the single partition
 * 'fallback_doms', it also forces the domains to be rebuilt.
 *
 * If doms_new == NULL it will be replaced with cpu_online_mask.
 * ndoms_new == 0 is a special case for destroying existing domains,
 * and it will not create the default domain.
 *
 * Call with hotplug lock held
 */
void partition_sched_domains(int ndoms_new, cpumask_var_t doms_new[],
			     struct sched_domain_attr *dattr_new)
{
	bool __maybe_unused has_eas = false;
	int i, j, n;
	int new_topology;

	mutex_lock(&sched_domains_mutex);

	/* Always unregister in case we don't destroy any domains: */
	unregister_sched_domain_sysctl();

	/* Let the architecture update CPU core mappings: */
	new_topology = arch_update_cpu_topology();
	/* Trigger rebuilding CPU capacity asymmetry data */
	if (new_topology)
		asym_cpu_capacity_scan();

	if (!doms_new) {
		WARN_ON_ONCE(dattr_new);
		n = 0;
		doms_new = alloc_sched_domains(1);
		if (doms_new) {
			n = 1;
			cpumask_and(doms_new[0], cpu_active_mask,
				    housekeeping_cpumask(HK_FLAG_DOMAIN));
		}
	} else {
		n = ndoms_new;
	}

	/* Destroy deleted domains: */
	for (i = 0; i < ndoms_cur; i++) {
		for (j = 0; j < n && !new_topology; j++) {
			if (cpumask_equal(doms_cur[i], doms_new[j]) &&
			    dattrs_equal(dattr_cur, i, dattr_new, j))
				goto match1;
		}
		/* No match - a current sched domain not in new doms_new[] */
		detach_destroy_domains(doms_cur[i]);
match1:
		;
	}

	n = ndoms_cur;
	if (!doms_new) {
		n = 0;
		doms_new = &fallback_doms;
		cpumask_and(doms_new[0], cpu_active_mask,
			    housekeeping_cpumask(HK_FLAG_DOMAIN));
	}

	/* Build new domains: */
	for (i = 0; i < ndoms_new; i++) {
		for (j = 0; j < n && !new_topology; j++) {
			if (cpumask_equal(doms_new[i], doms_cur[j]) &&
			    dattrs_equal(dattr_new, i, dattr_cur, j))
				goto match2;
		}
		/* No match - add a new doms_new */
		build_sched_domains(doms_new[i], dattr_new ? dattr_new + i : NULL);
match2:
		;
	}

#ifdef CONFIG_ENERGY_MODEL
	/* Build perf. domains: */
	for (i = 0; i < ndoms_new; i++) {
		for (j = 0; j < n; j++) {
			if (cpumask_equal(doms_new[i], doms_cur[j]) &&
			    cpu_rq(cpumask_first(doms_cur[j]))->rd->pd) {
				has_eas = true;
				goto match3;
			}
		}
		/* No match - add perf. domains for a new rd */
		has_eas |= build_perf_domains(doms_new[i]);
match3:
		;
	}
	sched_energy_set(has_eas);
#endif

	/* Remember the new sched domains: */
	if (doms_cur != &fallback_doms)
		free_sched_domains(doms_cur, ndoms_cur);

	kfree(dattr_cur);
	doms_cur = doms_new;
	dattr_cur = dattr_new;
	ndoms_cur = ndoms_new;

	register_sched_domain_sysctl();

	mutex_unlock(&sched_domains_mutex);
}
