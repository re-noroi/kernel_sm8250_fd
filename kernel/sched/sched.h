/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Scheduler internal types and methods:
 */
#include <linux/sched.h>

#include <linux/sched/autogroup.h>
#include <linux/sched/clock.h>
#include <linux/sched/coredump.h>
#include <linux/sched/cpufreq.h>
#include <linux/sched/cputime.h>
#include <linux/sched/deadline.h>
#include <linux/sched/debug.h>
#include <linux/sched/hotplug.h>
#include <linux/sched/idle.h>
#include <linux/sched/init.h>
#include <linux/sched/isolation.h>
#include <linux/sched/jobctl.h>
#include <linux/sched/loadavg.h>
#include <linux/sched/mm.h>
#include <linux/sched/nohz.h>
#include <linux/sched/numa_balancing.h>
#include <linux/sched/prio.h>
#include <linux/sched/rt.h>
#include <linux/sched/signal.h>
#include <linux/sched/smt.h>
#include <linux/sched/stat.h>
#include <linux/sched/sysctl.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/topology.h>
#include <linux/sched/user.h>
#include <linux/sched/wake_q.h>
#include <linux/sched/xacct.h>

#include <uapi/linux/sched/types.h>

#include <linux/binfmts.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/compat.h>
#include <linux/context_tracking.h>
#include <linux/cpufreq.h>
#include <linux/cpuidle.h>
#include <linux/cpuset.h>
#include <linux/ctype.h>
#include <linux/debugfs.h>
#include <linux/delayacct.h>
#include <linux/energy_model.h>
#include <linux/init_task.h>
#include <linux/kprobes.h>
#include <linux/kthread.h>
#include <linux/membarrier.h>
#include <linux/migrate.h>
#include <linux/mmu_context.h>
#include <linux/nmi.h>
#include <linux/proc_fs.h>
#include <linux/prefetch.h>
#include <linux/profile.h>
#include <linux/psi.h>
#include <linux/rcupdate_wait.h>
#include <linux/security.h>
#include <linux/stackprotector.h>
#include <linux/stop_machine.h>
#include <linux/suspend.h>
#include <linux/swait.h>
#include <linux/syscalls.h>
#include <linux/task_work.h>
#include <linux/tsacct_kern.h>
#include <linux/delayacct.h>

#include <asm/tlb.h>

#ifdef CONFIG_PARAVIRT
# include <asm/paravirt.h>
#endif

#include "cpupri.h"
#include "cpudeadline.h"
#include "features.h"

#ifdef CONFIG_SCHED_DEBUG
# define SCHED_WARN_ON(x)	WARN_ONCE(x, #x)
#else
# define SCHED_WARN_ON(x)	({ (void)(x), 0; })
#endif

struct rq;
struct cpuidle_state;

#define CPU_NR		8

extern __read_mostly bool sched_predl;
extern unsigned int sched_capacity_margin_up[CPU_NR];
extern unsigned int sched_capacity_margin_down[CPU_NR];

/* task_struct::on_rq states: */
#define TASK_ON_RQ_QUEUED	1
#define TASK_ON_RQ_MIGRATING	2

extern __read_mostly int scheduler_running;

extern unsigned long calc_load_update;
extern atomic_long_t calc_load_tasks;

extern void calc_global_load_tick(struct rq *this_rq);
extern long calc_load_fold_active(struct rq *this_rq, long adjust);

/*
 * Asymmetric CPU capacity bits
 */
struct asym_cap_data {
	struct list_head link;
	struct rcu_head rcu;
	unsigned long capacity;
	unsigned long cpus[];
};

extern struct list_head asym_cap_list;

#define cpu_capacity_span(asym_data) to_cpumask((asym_data)->cpus)

/*
 * Helpers for converting nanosecond timing to jiffy resolution
 */
#define NS_TO_JIFFIES(TIME)	((unsigned long)(TIME) / (NSEC_PER_SEC / HZ))

/*
 * Increase resolution of nice-level calculations for 64-bit architectures.
 * The extra resolution improves shares distribution and load balancing of
 * low-weight task groups (eg. nice +19 on an autogroup), deeper taskgroup
 * hierarchies, especially on larger systems. This is not a user-visible change
 * and does not change the user-interface for setting shares/weights.
 *
 * We increase resolution only if we have enough bits to allow this increased
 * resolution (i.e. 64-bit). The costs for increasing resolution when 32-bit
 * are pretty high and the returns do not justify the increased costs.
 *
 * Really only required when CONFIG_FAIR_GROUP_SCHED=y is also set, but to
 * increase coverage and consistency always enable it on 64-bit platforms.
 */
#ifdef CONFIG_64BIT
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT + SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		((w) << SCHED_FIXEDPOINT_SHIFT)
# define scale_load_down(w) \
({ \
	unsigned long __w = (w); \
	if (__w) \
		__w = max(2UL, __w >> SCHED_FIXEDPOINT_SHIFT); \
	__w; \
})
#else
# define NICE_0_LOAD_SHIFT	(SCHED_FIXEDPOINT_SHIFT)
# define scale_load(w)		(w)
# define scale_load_down(w)	(w)
#endif

/*
 * Task weight (visible to users) and its load (invisible to users) have
 * independent resolution, but they should be well calibrated. We use
 * scale_load() and scale_load_down(w) to convert between them. The
 * following must be true:
 *
 *  scale_load(sched_prio_to_weight[NICE_TO_PRIO(0)-MAX_RT_PRIO]) == NICE_0_LOAD
 *
 */
#define NICE_0_LOAD		(1L << NICE_0_LOAD_SHIFT)

/*
 * Single value that decides SCHED_DEADLINE internal math precision.
 * 10 -> just above 1us
 * 9  -> just above 0.5us
 */
#define DL_SCALE		10

/*
 * Single value that denotes runtime == period, ie unlimited time.
 */
#define RUNTIME_INF		((u64)~0ULL)

static inline int idle_policy(int policy)
{
	return policy == SCHED_IDLE;
}
static inline int fair_policy(int policy)
{
	return policy == SCHED_NORMAL || policy == SCHED_BATCH;
}

static inline int rt_policy(int policy)
{
	return policy == SCHED_FIFO || policy == SCHED_RR;
}

static inline int dl_policy(int policy)
{
	return policy == SCHED_DEADLINE;
}
static inline bool valid_policy(int policy)
{
	return idle_policy(policy) || fair_policy(policy) ||
		rt_policy(policy) || dl_policy(policy);
}

static inline int task_has_idle_policy(struct task_struct *p)
{
	return idle_policy(p->policy);
}

static inline int task_has_rt_policy(struct task_struct *p)
{
	return rt_policy(p->policy);
}

static inline int task_has_dl_policy(struct task_struct *p)
{
	return dl_policy(p->policy);
}

#define cap_scale(v, s) ((v)*(s) >> SCHED_CAPACITY_SHIFT)

static inline void update_avg(u64 *avg, u64 sample)
{
	s64 diff = sample - *avg;
	*avg += diff / 8;
}

/*
 * Shifting a value by an exponent greater *or equal* to the size of said value
 * is UB; cap at size-1.
 */
#define shr_bound(val, shift)							\
	(val >> min_t(typeof(shift), shift, BITS_PER_TYPE(typeof(val)) - 1))

/*
 * !! For sched_setattr_nocheck() (kernel) only !!
 *
 * This is actually gross. :(
 *
 * It is used to make schedutil kworker(s) higher priority than SCHED_DEADLINE
 * tasks, but still be able to sleep. We need this on platforms that cannot
 * atomically change clock frequency. Remove once fast switching will be
 * available on such platforms.
 *
 * SUGOV stands for SchedUtil GOVernor.
 */
#define SCHED_FLAG_SUGOV	0x10000000

#define SCHED_DL_FLAGS (SCHED_FLAG_RECLAIM | SCHED_FLAG_DL_OVERRUN | SCHED_FLAG_SUGOV)

static inline bool dl_entity_is_special(struct sched_dl_entity *dl_se)
{
#ifdef CONFIG_CPU_FREQ_GOV_SCHEDUTIL
	return unlikely(dl_se->flags & SCHED_FLAG_SUGOV);
#else
	return false;
#endif
}

/*
 * Tells if entity @a should preempt entity @b.
 */
static inline bool
dl_entity_preempt(struct sched_dl_entity *a, struct sched_dl_entity *b)
{
	return dl_entity_is_special(a) ||
	       dl_time_before(a->deadline, b->deadline);
}

/*
 * This is the priority-queue data structure of the RT scheduling class:
 */
struct rt_prio_array {
	DECLARE_BITMAP(bitmap, MAX_RT_PRIO+1); /* include 1 bit for delimiter */
	struct list_head queue[MAX_RT_PRIO];
};

struct rt_bandwidth {
	/* nests inside the rq lock: */
	raw_spinlock_t		rt_runtime_lock;
	ktime_t			rt_period;
	u64			rt_runtime;
	struct hrtimer		rt_period_timer;
	unsigned int		rt_period_active;
};

void __dl_clear_params(struct task_struct *p);

/*
 * To keep the bandwidth of -deadline tasks and groups under control
 * we need some place where:
 *  - store the maximum -deadline bandwidth of the system (the group);
 *  - cache the fraction of that bandwidth that is currently allocated.
 *
 * This is all done in the data structure below. It is similar to the
 * one used for RT-throttling (rt_bandwidth), with the main difference
 * that, since here we are only interested in admission control, we
 * do not decrease any runtime while the group "executes", neither we
 * need a timer to replenish it.
 *
 * With respect to SMP, the bandwidth is given on a per-CPU basis,
 * meaning that:
 *  - dl_bw (< 100%) is the bandwidth of the system (group) on each CPU;
 *  - dl_total_bw array contains, in the i-eth element, the currently
 *    allocated bandwidth on the i-eth CPU.
 * Moreover, groups consume bandwidth on each CPU, while tasks only
 * consume bandwidth on the CPU they're running on.
 * Finally, dl_total_bw_cpu is used to cache the index of dl_total_bw
 * that will be shown the next time the proc or cgroup controls will
 * be red. It on its turn can be changed by writing on its own
 * control.
 */
struct dl_bandwidth {
	raw_spinlock_t		dl_runtime_lock;
	u64			dl_runtime;
	u64			dl_period;
};

static inline int dl_bandwidth_enabled(void)
{
	return sysctl_sched_rt_runtime >= 0;
}

struct dl_bw {
	raw_spinlock_t		lock;
	u64			bw;
	u64			total_bw;
};

extern void dl_change_utilization(struct task_struct *p, u64 new_bw);
extern void init_dl_bw(struct dl_bw *dl_b);
extern int  sched_dl_global_validate(void);
extern void sched_dl_do_global(void);
extern int  sched_dl_overflow(struct task_struct *p, int policy, const struct sched_attr *attr);
extern void __setparam_dl(struct task_struct *p, const struct sched_attr *attr);
extern void __getparam_dl(struct task_struct *p, struct sched_attr *attr);
extern bool __checkparam_dl(const struct sched_attr *attr);
extern bool dl_param_changed(struct task_struct *p, const struct sched_attr *attr);
extern int  dl_cpuset_cpumask_can_shrink(const struct cpumask *cur, const struct cpumask *trial);
extern int  dl_cpu_busy(int cpu, struct task_struct *p);

#ifdef CONFIG_CGROUP_SCHED

#include <linux/cgroup.h>
#include <linux/psi.h>

struct cfs_rq;
struct rt_rq;

extern struct list_head task_groups;

struct cfs_bandwidth {
#ifdef CONFIG_CFS_BANDWIDTH
	raw_spinlock_t		lock;
	ktime_t			period;
	u64			quota;
	u64			runtime;
	s64			hierarchical_quota;

	short			idle;
	short			period_active;
	struct hrtimer		period_timer;
	struct hrtimer		slack_timer;
	struct list_head	throttled_cfs_rq;

	/* Statistics: */
	int			nr_periods;
	int			nr_throttled;
	u64			throttled_time;

	bool                    distribute_running;
#endif
};

/* Task group related information */
struct task_group {
	struct cgroup_subsys_state css;

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* schedulable entities of this group on each CPU */
	struct sched_entity	**se;
	/* runqueue "owned" by this group on each CPU */
	struct cfs_rq		**cfs_rq;
	unsigned long		shares;

#ifdef	CONFIG_SMP
	/*
	 * load_avg can be heavily contended at clock tick time, so put
	 * it in its own cacheline separated from the fields above which
	 * will also be accessed at each tick.
	 */
	atomic_long_t		load_avg ____cacheline_aligned;
#endif
#endif

#ifdef CONFIG_RT_GROUP_SCHED
	struct sched_rt_entity	**rt_se;
	struct rt_rq		**rt_rq;

	struct rt_bandwidth	rt_bandwidth;
#endif

	struct rcu_head		rcu;
	struct list_head	list;

	struct task_group	*parent;
	struct list_head	siblings;
	struct list_head	children;

#ifdef CONFIG_SCHED_AUTOGROUP
	struct autogroup	*autogroup;
#endif

	struct cfs_bandwidth	cfs_bandwidth;

#ifdef CONFIG_UCLAMP_TASK_GROUP
	/* The two decimal precision [%] value requested from user-space */
	unsigned int		uclamp_pct[UCLAMP_CNT];
	/* Clamp values requested for a task group */
	struct uclamp_se	uclamp_req[UCLAMP_CNT];
	/* Effective clamp values used for a task group */
	struct uclamp_se	uclamp[UCLAMP_CNT];
	/* Latency-sensitive flag used for a task group */
	unsigned int		latency_sensitive;
	/* Boosted flag for a task group */
	unsigned int 		boosted;
#endif

};

#ifdef CONFIG_FAIR_GROUP_SCHED
#define ROOT_TASK_GROUP_LOAD	NICE_0_LOAD

/*
 * A weight of 0 or 1 can cause arithmetics problems.
 * A weight of a cfs_rq is the sum of weights of which entities
 * are queued on this cfs_rq, so a weight of a entity should not be
 * too large, so as the shares value of a task group.
 * (The default weight is 1024 - so there's no practical
 *  limitation from this.)
 */
#define MIN_SHARES		(1UL <<  1)
#define MAX_SHARES		(1UL << 18)
#endif

typedef int (*tg_visitor)(struct task_group *, void *);

extern int walk_tg_tree_from(struct task_group *from,
			     tg_visitor down, tg_visitor up, void *data);

/*
 * Iterate the full tree, calling @down when first entering a node and @up when
 * leaving it for the final time.
 *
 * Caller must hold rcu_lock or sufficient equivalent.
 */
static inline int walk_tg_tree(tg_visitor down, tg_visitor up, void *data)
{
	return walk_tg_tree_from(&root_task_group, down, up, data);
}

extern int tg_nop(struct task_group *tg, void *data);

extern void free_fair_sched_group(struct task_group *tg);
extern int alloc_fair_sched_group(struct task_group *tg, struct task_group *parent);
extern void online_fair_sched_group(struct task_group *tg);
extern void unregister_fair_sched_group(struct task_group *tg);
extern void init_tg_cfs_entry(struct task_group *tg, struct cfs_rq *cfs_rq,
			struct sched_entity *se, int cpu,
			struct sched_entity *parent);
extern void init_cfs_bandwidth(struct cfs_bandwidth *cfs_b);

extern void __refill_cfs_bandwidth_runtime(struct cfs_bandwidth *cfs_b);
extern void start_cfs_bandwidth(struct cfs_bandwidth *cfs_b);
extern void unthrottle_cfs_rq(struct cfs_rq *cfs_rq);

extern void unregister_rt_sched_group(struct task_group *tg);
extern void free_rt_sched_group(struct task_group *tg);
extern int alloc_rt_sched_group(struct task_group *tg, struct task_group *parent);
extern void init_tg_rt_entry(struct task_group *tg, struct rt_rq *rt_rq,
		struct sched_rt_entity *rt_se, int cpu,
		struct sched_rt_entity *parent);
extern int sched_group_set_rt_runtime(struct task_group *tg, long rt_runtime_us);
extern int sched_group_set_rt_period(struct task_group *tg, u64 rt_period_us);
extern long sched_group_rt_runtime(struct task_group *tg);
extern long sched_group_rt_period(struct task_group *tg);
extern int sched_rt_can_attach(struct task_group *tg, struct task_struct *tsk);

extern struct task_group *sched_create_group(struct task_group *parent);
extern void sched_online_group(struct task_group *tg,
			       struct task_group *parent);
extern void sched_destroy_group(struct task_group *tg);
extern void sched_release_group(struct task_group *tg);

extern void sched_move_task(struct task_struct *tsk);

#ifdef CONFIG_FAIR_GROUP_SCHED
extern int sched_group_set_shares(struct task_group *tg, unsigned long shares);

#ifdef CONFIG_SMP
extern void set_task_rq_fair(struct sched_entity *se,
			     struct cfs_rq *prev, struct cfs_rq *next);
#else /* !CONFIG_SMP */
static inline void set_task_rq_fair(struct sched_entity *se,
			     struct cfs_rq *prev, struct cfs_rq *next) { }
#endif /* CONFIG_SMP */
#endif /* CONFIG_FAIR_GROUP_SCHED */

#else /* CONFIG_CGROUP_SCHED */

struct cfs_bandwidth { };

#endif	/* CONFIG_CGROUP_SCHED */

/*
 * u64_u32_load/u64_u32_store
 *
 * Use a copy of a u64 value to protect against data race. This is only
 * applicable for 32-bits architectures.
 */
#ifdef CONFIG_64BIT
# define u64_u32_load_copy(var, copy)       var
# define u64_u32_store_copy(var, copy, val) (var = val)
#else
# define u64_u32_load_copy(var, copy)					\
({									\
	u64 __val, __val_copy;						\
	do {								\
		__val_copy = copy;					\
		/*							\
		 * paired with u64_u32_store_copy(), ordering access	\
		 * to var and copy.					\
		 */							\
		smp_rmb();						\
		__val = var;						\
	} while (__val != __val_copy);					\
	__val;								\
})
# define u64_u32_store_copy(var, copy, val)				\
do {									\
	typeof(val) __val = (val);					\
	var = __val;							\
	/*								\
	 * paired with u64_u32_load_copy(), ordering access to var and	\
	 * copy.							\
	 */								\
	smp_wmb();							\
	copy = __val;							\
} while (0)
#endif
# define u64_u32_load(var)      u64_u32_load_copy(var, var##_copy)
# define u64_u32_store(var, val) u64_u32_store_copy(var, var##_copy, val)

/* CFS-related fields in a runqueue */
struct cfs_rq {
	struct load_weight	load;
	unsigned int		nr_running;
	unsigned int		h_nr_running;      /* SCHED_{NORMAL,BATCH,IDLE} */
	unsigned int		h_nr_runnable;     /* SCHED_{NORMAL,BATCH,IDLE} */
	unsigned int		idle_h_nr_running; /* SCHED_IDLE */

	s64			avg_vruntime;
	u64			avg_load;

	u64			min_vruntime;

	struct rb_root_cached	tasks_timeline;

	/*
	 * 'curr' points to currently running entity on this cfs_rq.
	 * It is set to NULL otherwise (i.e when none are currently running).
	 */
	struct sched_entity	*curr;
	struct sched_entity	*next;

#ifdef CONFIG_SMP
	/*
	 * CFS load tracking
	 */
	struct sched_avg	avg;
#ifndef CONFIG_64BIT
	u64			last_update_time_copy;
#endif
	struct {
		raw_spinlock_t	lock ____cacheline_aligned;
		int		nr;
		unsigned long	load_avg;
		unsigned long	util_avg;
		unsigned long	runnable_avg;
	} removed;

#ifdef CONFIG_FAIR_GROUP_SCHED
	unsigned long		tg_load_avg_contrib;
	long			propagate;
	long			prop_runnable_sum;

	/*
	 *   h_load = weight * f(tg)
	 *
	 * Where f(tg) is the recursive weight fraction assigned to
	 * this group.
	 */
	unsigned long		h_load;
	u64			last_h_load_update;
	struct sched_entity	*h_load_next;
#endif /* CONFIG_FAIR_GROUP_SCHED */
#endif /* CONFIG_SMP */

#ifdef CONFIG_FAIR_GROUP_SCHED
	struct rq		*rq;	/* CPU runqueue to which this cfs_rq is attached */

	/*
	 * leaf cfs_rqs are those that hold tasks (lowest schedulable entity in
	 * a hierarchy). Non-leaf lrqs hold other higher schedulable entities
	 * (like users, containers etc.)
	 *
	 * leaf_cfs_rq_list ties together list of leaf cfs_rq's in a CPU.
	 * This list is used during load balance.
	 */
	int			on_list;
	struct list_head	leaf_cfs_rq_list;
	struct task_group	*tg;	/* group that "owns" this runqueue */

#ifdef CONFIG_CFS_BANDWIDTH
	int			runtime_enabled;
	s64			runtime_remaining;

	u64			throttled_pelt_idle;
#ifndef CONFIG_64BIT
	u64                     throttled_pelt_idle_copy;
#endif
	u64			throttled_clock;
	u64			throttled_clock_pelt;
	u64			throttled_clock_pelt_time;
	int			throttled;
	int			throttle_count;
	struct list_head	throttled_list;
#endif /* CONFIG_CFS_BANDWIDTH */
#endif /* CONFIG_FAIR_GROUP_SCHED */
};

static inline int rt_bandwidth_enabled(void)
{
	return sysctl_sched_rt_runtime >= 0;
}

/* RT IPI pull logic requires IRQ_WORK */
#if defined(CONFIG_IRQ_WORK) && defined(CONFIG_SMP)
# define HAVE_RT_PUSH_IPI
#endif

/* Real-Time classes' related field in a runqueue: */
struct rt_rq {
	struct rt_prio_array	active;
	unsigned int		rt_nr_running;
	unsigned int		rr_nr_running;
#if defined CONFIG_SMP || defined CONFIG_RT_GROUP_SCHED
	struct {
		int		curr; /* highest queued rt task prio */
#ifdef CONFIG_SMP
		int		next; /* next highest */
#endif
	} highest_prio;
#endif
#ifdef CONFIG_SMP
	unsigned int		rt_nr_migratory;
	unsigned int		rt_nr_total;
	int			overloaded;
	struct plist_head	pushable_tasks;

#endif /* CONFIG_SMP */
	int			rt_queued;

	int			rt_throttled;
	u64			rt_time;
	u64			rt_runtime;
	/* Nests inside the rq lock: */
	raw_spinlock_t		rt_runtime_lock;

#ifdef CONFIG_RT_GROUP_SCHED
	unsigned int		rt_nr_boosted;

	struct rq		*rq;
	struct task_group	*tg;
#endif
};

static inline bool rt_rq_is_runnable(struct rt_rq *rt_rq)
{
	return rt_rq->rt_queued && rt_rq->rt_nr_running;
}

/* Deadline class' related fields in a runqueue */
struct dl_rq {
	/* runqueue is an rbtree, ordered by deadline */
	struct rb_root_cached	root;

	unsigned int		dl_nr_running;

#ifdef CONFIG_SMP
	/*
	 * Deadline values of the currently executing and the
	 * earliest ready task on this rq. Caching these facilitates
	 * the decision wether or not a ready but not running task
	 * should migrate somewhere else.
	 */
	struct {
		u64		curr;
		u64		next;
	} earliest_dl;

	unsigned int		dl_nr_migratory;
	int			overloaded;

	/*
	 * Tasks on this rq that can be pushed away. They are kept in
	 * an rb-tree, ordered by tasks' deadlines, with caching
	 * of the leftmost (earliest deadline) element.
	 */
	struct rb_root_cached	pushable_dl_tasks_root;
#else
	struct dl_bw		dl_bw;
#endif
	/*
	 * "Active utilization" for this runqueue: increased when a
	 * task wakes up (becomes TASK_RUNNING) and decreased when a
	 * task blocks
	 */
	u64			running_bw;

	/*
	 * Utilization of the tasks "assigned" to this runqueue (including
	 * the tasks that are in runqueue and the tasks that executed on this
	 * CPU and blocked). Increased when a task moves to this runqueue, and
	 * decreased when the task moves away (migrates, changes scheduling
	 * policy, or terminates).
	 * This is needed to compute the "inactive utilization" for the
	 * runqueue (inactive utilization = this_bw - running_bw).
	 */
	u64			this_bw;
	u64			extra_bw;

	/*
	 * Inverse of the fraction of CPU utilization that can be reclaimed
	 * by the GRUB algorithm.
	 */
	u64			bw_ratio;
};

#ifdef CONFIG_FAIR_GROUP_SCHED
/* An entity is a task if it doesn't "own" a runqueue */
#define entity_is_task(se)	(!se->my_q)

static inline void se_update_runnable(struct sched_entity *se)
{
	if (!entity_is_task(se))
		se->runnable_weight = se->my_q->h_nr_runnable;
}

static inline long se_runnable(struct sched_entity *se)
{
	if (se->sched_delayed)
		return false;

	if (entity_is_task(se))
		return !!se->on_rq;
	else
		return se->runnable_weight;
}

#else
#define entity_is_task(se)	1

static inline void se_update_runnable(struct sched_entity *se) {}

static inline long se_runnable(struct sched_entity *se)
{
	if (se->sched_delayed)
		return false;

	return !!se->on_rq;
}
#endif

#ifdef CONFIG_RT_GROUP_SCHED
#define rt_entity_is_task(rt_se) (!(rt_se)->my_q)
#else /* CONFIG_RT_GROUP_SCHED */
#define rt_entity_is_task(rt_se) (1)
#endif /* CONFIG_RT_GROUP_SCHED */

#ifdef CONFIG_SMP
/*
 * XXX we want to get rid of these helpers and use the full load resolution.
 */
static inline long se_weight(struct sched_entity *se)
{
	return scale_load_down(se->load.weight);
}


static inline bool sched_asym_prefer(int a, int b)
{
	return arch_asym_cpu_priority(a) > arch_asym_cpu_priority(b);
}

struct perf_domain {
	struct em_perf_domain *em_pd;
	struct perf_domain *next;
	struct rcu_head rcu;
};

struct max_cpu_capacity {
	raw_spinlock_t lock;
	unsigned long val;
	int cpu;
};

/* Scheduling group status flags */
#define SG_OVERLOAD		0x1 /* More than one runnable task on a CPU. */
#define SG_OVERUTILIZED		0x2 /* One or more CPUs are over-utilized. */

/*
 * We add the notion of a root-domain which will be used to define per-domain
 * variables. Each exclusive cpuset essentially defines an island domain by
 * fully partitioning the member CPUs from any other cpuset. Whenever a new
 * exclusive cpuset is created, we also create and attach a new root-domain
 * object.
 *
 */
struct root_domain {
	atomic_t		refcount;
	atomic_t		rto_count;
	struct rcu_head		rcu;
	cpumask_var_t		span;
	cpumask_var_t		online;

	/*
	 * Indicate pullable load on at least one CPU, e.g:
	 * - More than one runnable task
	 * - Running task is misfit
	 */
	int			overload;

	int			overutilized;

	/*
	 * The bit corresponding to a CPU gets set here if such CPU has more
	 * than one runnable -deadline task (as it is below for RT tasks).
	 */
	cpumask_var_t		dlo_mask;
	atomic_t		dlo_count;
	struct dl_bw		dl_bw;
	struct cpudl		cpudl;

#ifdef HAVE_RT_PUSH_IPI
	/*
	 * For IPI pull requests, loop across the rto_mask.
	 */
	struct irq_work		rto_push_work;
	raw_spinlock_t		rto_lock;
	/* These are only updated and read within rto_lock */
	int			rto_loop;
	int			rto_cpu;
	/* These atomics are updated outside of a lock */
	atomic_t		rto_loop_next;
	atomic_t		rto_loop_start;
#endif
	/*
	 * The "RT overload" flag: it gets set if a CPU has more than
	 * one runnable RT task.
	 */
	cpumask_var_t		rto_mask;
	struct cpupri		cpupri;

	/* Maximum cpu capacity in the system. */
	struct max_cpu_capacity max_cpu_capacity;

	/*
	 * NULL-terminated list of performance domains intersecting with the
	 * CPUs of the rd. Protected by RCU.
	 */
	struct perf_domain	*pd;

};

extern struct root_domain def_root_domain;
extern struct mutex sched_domains_mutex;

extern void init_defrootdomain(void);
extern void init_max_cpu_capacity(struct max_cpu_capacity *mcc);
extern int sched_init_domains(const struct cpumask *cpu_map);
extern void rq_attach_root(struct rq *rq, struct root_domain *rd);
extern void sched_get_rd(struct root_domain *rd);
extern void sched_put_rd(struct root_domain *rd);

static inline int get_rd_overload(struct root_domain *rd)
{
	return READ_ONCE(rd->overload);
}

static inline void set_rd_overload(struct root_domain *rd, int status)
{
	if (get_rd_overload(rd) != status)
		WRITE_ONCE(rd->overload, status);
}

#ifdef HAVE_RT_PUSH_IPI
extern void rto_push_irq_work_func(struct irq_work *work);
#endif
#endif /* CONFIG_SMP */

#ifdef CONFIG_UCLAMP_TASK
/*
 * struct uclamp_bucket - Utilization clamp bucket
 * @value: utilization clamp value for tasks on this clamp bucket
 * @tasks: number of RUNNABLE tasks on this clamp bucket
 *
 * Keep track of how many tasks are RUNNABLE for a given utilization
 * clamp value.
 */
struct uclamp_bucket {
	unsigned long value : bits_per(SCHED_CAPACITY_SCALE);
	unsigned long tasks : BITS_PER_LONG - bits_per(SCHED_CAPACITY_SCALE);
};

/*
 * struct uclamp_rq - rq's utilization clamp
 * @value: currently active clamp values for a rq
 * @bucket: utilization clamp buckets affecting a rq
 *
 * Keep track of RUNNABLE tasks on a rq to aggregate their clamp values.
 * A clamp value is affecting a rq when there is at least one task RUNNABLE
 * (or actually running) with that value.
 *
 * There are up to UCLAMP_CNT possible different clamp values, currently there
 * are only two: minimum utilization and maximum utilization.
 *
 * All utilization clamping values are MAX aggregated, since:
 * - for util_min: we want to run the CPU at least at the max of the minimum
 *   utilization required by its currently RUNNABLE tasks.
 * - for util_max: we want to allow the CPU to run up to the max of the
 *   maximum utilization allowed by its currently RUNNABLE tasks.
 *
 * Since on each system we expect only a limited number of different
 * utilization clamp values (UCLAMP_BUCKETS), use a simple array to track
 * the metrics required to compute all the per-rq utilization clamp values.
 */
struct uclamp_rq {
	unsigned int value;
	struct uclamp_bucket bucket[UCLAMP_BUCKETS];
};

DECLARE_STATIC_KEY_FALSE(sched_uclamp_used);
#endif /* CONFIG_UCLAMP_TASK */

/*
 * This is the main, per-CPU runqueue data structure.
 *
 * Locking rule: those places that want to lock multiple runqueues
 * (such as the load balancing or the thread migration code), lock
 * acquire operations must be ordered by ascending &runqueue.
 */
struct rq {
	/* runqueue lock: */
	raw_spinlock_t		lock;

	/*
	 * nr_running and cpu_load should be in the same cacheline because
	 * remote CPUs use both these fields when doing load calculation.
	 */
	unsigned int		nr_running;
#ifdef CONFIG_NUMA_BALANCING
	unsigned int		nr_numa_running;
	unsigned int		nr_preferred_running;
	unsigned int		numa_migrate_on;
#endif
#ifdef CONFIG_NO_HZ_COMMON
#ifdef CONFIG_SMP
	unsigned long		last_blocked_load_update_tick;
	unsigned int		has_blocked_load;
	call_single_data_t	nohz_csd;
#endif /* CONFIG_SMP */
	unsigned int		nohz_tick_stopped;
	atomic_t		nohz_flags;
#endif /* CONFIG_NO_HZ_COMMON */

#ifdef CONFIG_SMP
	unsigned int		ttwu_pending;
#endif
	u64			nr_switches;

#ifdef CONFIG_UCLAMP_TASK
	/* Utilization clamp values based on CPU's RUNNABLE tasks */
	struct uclamp_rq	uclamp[UCLAMP_CNT] ____cacheline_aligned;
	unsigned int		uclamp_flags;
#define UCLAMP_FLAG_IDLE 0x01
#endif

	struct cfs_rq		cfs;
	struct rt_rq		rt;
	struct dl_rq		dl;

#ifdef CONFIG_FAIR_GROUP_SCHED
	/* list of leaf cfs_rq on this CPU: */
	struct list_head	leaf_cfs_rq_list;
	struct list_head	*tmp_alone_branch;
#endif /* CONFIG_FAIR_GROUP_SCHED */

	/*
	 * This is part of a global counter where only the total sum
	 * over all CPUs matters. A task can increase this counter on
	 * one CPU and if it got migrated afterwards it may decrease
	 * it on another CPU. Always updated under the runqueue lock:
	 */
	unsigned long		nr_uninterruptible;

	struct task_struct	*curr;
	struct task_struct	*idle;
	struct task_struct	*stop;
	unsigned long		next_balance;
	struct mm_struct	*prev_mm;

	unsigned int		clock_update_flags;
	u64			clock;
	/* Ensure that all clocks are in the same cache line */
	u64			clock_task ____cacheline_aligned;
	u64			clock_pelt;
	unsigned long		lost_idle_time;
	u64			clock_pelt_idle;
	u64			clock_idle;
#ifndef CONFIG_64BIT
	u64			clock_pelt_idle_copy;
	u64			clock_idle_copy;
#endif

	atomic_t		nr_iowait;

#ifdef CONFIG_SMP
	struct root_domain	*rd;
	struct sched_domain	*sd;

	unsigned long		cpu_capacity;
	unsigned long		cpu_capacity_orig;

	struct callback_head	*balance_callback;

	unsigned char		nohz_idle_balance;
	unsigned char		idle_balance;

	unsigned long		misfit_task_load;

	/* For active balancing */
	int			active_balance;
	int			push_cpu;
	struct cpu_stop_work	active_balance_work;

	/* CPU of this runqueue: */
	int			cpu;
	int			online;

	struct list_head cfs_tasks;

	struct sched_avg	avg_rt;
	struct sched_avg	avg_dl;
#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
	struct sched_avg	avg_irq;
#endif
#ifdef CONFIG_SCHED_THERMAL_PRESSURE
	struct sched_avg	avg_thermal;
#endif
	u64			idle_stamp;
	u64			avg_idle;

	unsigned long		wake_stamp;
	u64			wake_avg_idle;

	/* This is used to determine avg_idle's max value */
	u64			max_idle_balance_cost;
#endif /* CONFIG_SMP */

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
	u64			prev_irq_time;
#endif
#ifdef CONFIG_PARAVIRT
	u64			prev_steal_time;
#endif
#ifdef CONFIG_PARAVIRT_TIME_ACCOUNTING
	u64			prev_steal_time_rq;
#endif

	/* calc_load related fields */
	unsigned long		calc_load_update;
	long			calc_load_active;

#ifdef CONFIG_SCHED_HRTICK
#ifdef CONFIG_SMP
	int			hrtick_csd_pending;
	call_single_data_t	hrtick_csd;
#endif
	struct hrtimer		hrtick_timer;
	ktime_t 		hrtick_time;
#endif

#ifdef CONFIG_SCHEDSTATS
	/* latency stats */
	struct sched_info	rq_sched_info;
	unsigned long long	rq_cpu_time;

	/* sys_sched_yield() stats */
	unsigned int		yld_count;

	/* schedule() stats */
	unsigned int		sched_count;
	unsigned int		sched_goidle;

	/* try_to_wake_up() stats */
	unsigned int		ttwu_count;
	unsigned int		ttwu_local;
#endif

#ifdef CONFIG_CPU_IDLE
	/* Must be inspected within a rcu lock section */
	struct cpuidle_state	*idle_state;
#endif
};

#ifdef CONFIG_FAIR_GROUP_SCHED

/* CPU runqueue to which this cfs_rq is attached */
static inline struct rq *rq_of(struct cfs_rq *cfs_rq)
{
	return cfs_rq->rq;
}

#else

static inline struct rq *rq_of(struct cfs_rq *cfs_rq)
{
	return container_of(cfs_rq, struct rq, cfs);
}
#endif

static inline int cpu_of(struct rq *rq)
{
#ifdef CONFIG_SMP
	return rq->cpu;
#else
	return 0;
#endif
}

struct sched_group;
#ifdef CONFIG_SCHED_CORE
static inline struct cpumask *sched_group_span(struct sched_group *sg);
/*
 * Helpers to check if the CPU's core cookie matches with the task's cookie
 * when core scheduling is enabled.
 * A special case is that the task's cookie always matches with CPU's core
 * cookie if the CPU is in an idle core.
 */
static inline bool sched_cpu_cookie_match(struct rq *rq, struct task_struct *p)
{
	/* Ignore cookie match if core scheduler is not enabled on the CPU. */
	if (!sched_core_enabled(rq))
		return true;

	return rq->core->core_cookie == p->core_cookie;
}

static inline bool sched_core_cookie_match(struct rq *rq, struct task_struct *p)
{
	bool idle_core = true;
	int cpu;

	/* Ignore cookie match if core scheduler is not enabled on the CPU. */
	if (!sched_core_enabled(rq))
		return true;

	for_each_cpu(cpu, cpu_smt_mask(cpu_of(rq))) {
		if (!available_idle_cpu(cpu)) {
			idle_core = false;
			break;
		}
	}

	/*
	 * A CPU in an idle core is always the best choice for tasks with
	 * cookies.
	 */
	return idle_core || rq->core->core_cookie == p->core_cookie;
}

static inline bool sched_group_cookie_match(struct rq *rq,
					    struct task_struct *p,
					    struct sched_group *group)
{
	int cpu;

	/* Ignore cookie match if core scheduler is not enabled on the CPU. */
	if (!sched_core_enabled(rq))
		return true;

	for_each_cpu_and(cpu, sched_group_span(group), p->cpus_ptr) {
		if (sched_core_cookie_match(rq, p))
			return true;
	}
	return false;
}
#else /* !CONFIG_SCHED_CORE */
static inline bool sched_cpu_cookie_match(struct rq *rq, struct task_struct *p)
{
	return true;
}

static inline bool sched_core_cookie_match(struct rq *rq, struct task_struct *p)
{
	return true;
}

static inline bool sched_group_cookie_match(struct rq *rq,
					    struct task_struct *p,
					    struct sched_group *group)
{
	return true;
}
#endif /* CONFIG_SCHED_CORE */

#ifdef CONFIG_SCHED_SMT
extern void __update_idle_core(struct rq *rq);

static inline void update_idle_core(struct rq *rq)
{
	if (static_branch_unlikely(&sched_smt_present))
		__update_idle_core(rq);
}

#else
static inline void update_idle_core(struct rq *rq) { }
#endif

DECLARE_PER_CPU_SHARED_ALIGNED(struct rq, runqueues);

#define cpu_rq(cpu)		(&per_cpu(runqueues, (cpu)))
#define this_rq()		this_cpu_ptr(&runqueues)
#define task_rq(p)		cpu_rq(task_cpu(p))
#define cpu_curr(cpu)		(cpu_rq(cpu)->curr)
#define raw_rq()		raw_cpu_ptr(&runqueues)

extern void update_rq_clock(struct rq *rq);

/*
 * rq::clock_update_flags bits
 *
 * %RQCF_REQ_SKIP - will request skipping of clock update on the next
 *  call to __schedule(). This is an optimisation to avoid
 *  neighbouring rq clock updates.
 *
 * %RQCF_ACT_SKIP - is set from inside of __schedule() when skipping is
 *  in effect and calls to update_rq_clock() are being ignored.
 *
 * %RQCF_UPDATED - is a debug flag that indicates whether a call has been
 *  made to update_rq_clock() since the last time rq::lock was pinned.
 *
 * If inside of __schedule(), clock_update_flags will have been
 * shifted left (a left shift is a cheap operation for the fast path
 * to promote %RQCF_REQ_SKIP to %RQCF_ACT_SKIP), so you must use,
 *
 *	if (rq-clock_update_flags >= RQCF_UPDATED)
 *
 * to check if %RQCF_UPADTED is set. It'll never be shifted more than
 * one position though, because the next rq_unpin_lock() will shift it
 * back.
 */
#define RQCF_REQ_SKIP		0x01
#define RQCF_ACT_SKIP		0x02
#define RQCF_UPDATED		0x04

static inline void assert_clock_updated(struct rq *rq)
{
	/*
	 * The only reason for not seeing a clock update since the
	 * last rq_pin_lock() is if we're currently skipping updates.
	 */
	SCHED_WARN_ON(rq->clock_update_flags < RQCF_ACT_SKIP);
}

static inline u64 rq_clock(struct rq *rq)
{
	lockdep_assert_held(&rq->lock);
	assert_clock_updated(rq);

	return rq->clock;
}

static inline u64 rq_clock_task(struct rq *rq)
{
	lockdep_assert_held(&rq->lock);
	assert_clock_updated(rq);

	return rq->clock_task;
}

/**
 * By default the decay is the default pelt decay period.
 * The decay shift can change the decay period in
 * multiples of 32.
 *  Decay shift		Decay period(ms)
 *	0			32
 *	1			64
 *	2			128
 *	3			256
 *	4			512
 */
extern int sched_thermal_decay_shift;

static inline u64 rq_clock_thermal(struct rq *rq)
{
	return rq_clock_task(rq) >> sched_thermal_decay_shift;
}

static inline void rq_clock_skip_update(struct rq *rq)
{
	lockdep_assert_held(&rq->lock);
	rq->clock_update_flags |= RQCF_REQ_SKIP;
}

/*
 * See rt task throttling, which is the only time a skip
 * request is cancelled.
 */
static inline void rq_clock_cancel_skipupdate(struct rq *rq)
{
	lockdep_assert_held(&rq->lock);
	rq->clock_update_flags &= ~RQCF_REQ_SKIP;
}

struct rq_flags {
	unsigned long flags;
	struct pin_cookie cookie;
#ifdef CONFIG_SCHED_DEBUG
	/*
	 * A copy of (rq::clock_update_flags & RQCF_UPDATED) for the
	 * current pin context is stashed here in case it needs to be
	 * restored in rq_repin_lock().
	 */
	unsigned int clock_update_flags;
#endif
};

static inline void rq_pin_lock(struct rq *rq, struct rq_flags *rf)
{
	rf->cookie = lockdep_pin_lock(&rq->lock);

#ifdef CONFIG_SCHED_DEBUG
	rq->clock_update_flags &= (RQCF_REQ_SKIP|RQCF_ACT_SKIP);
	rf->clock_update_flags = 0;
#endif
#ifdef CONFIG_SMP
	SCHED_WARN_ON(rq->balance_callback);
#endif
}

static inline void rq_unpin_lock(struct rq *rq, struct rq_flags *rf)
{
#ifdef CONFIG_SCHED_DEBUG
	if (rq->clock_update_flags > RQCF_ACT_SKIP)
		rf->clock_update_flags = RQCF_UPDATED;
#endif

	lockdep_unpin_lock(&rq->lock, rf->cookie);
}

static inline void rq_repin_lock(struct rq *rq, struct rq_flags *rf)
{
	lockdep_repin_lock(&rq->lock, rf->cookie);

#ifdef CONFIG_SCHED_DEBUG
	/*
	 * Restore the value we stashed in @rf for this pin context.
	 */
	rq->clock_update_flags |= rf->clock_update_flags;
#endif
}

struct rq *__task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(rq->lock);

struct rq *task_rq_lock(struct task_struct *p, struct rq_flags *rf)
	__acquires(p->pi_lock)
	__acquires(rq->lock);

static inline void __task_rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_unlock(&rq->lock);
}

static inline void
task_rq_unlock(struct rq *rq, struct task_struct *p, struct rq_flags *rf)
	__releases(rq->lock)
	__releases(p->pi_lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_unlock(&rq->lock);
	raw_spin_unlock_irqrestore(&p->pi_lock, rf->flags);
}

static inline void
rq_lock_irqsave(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock_irqsave(&rq->lock, rf->flags);
	rq_pin_lock(rq, rf);
}

static inline void
rq_lock_irq(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock_irq(&rq->lock);
	rq_pin_lock(rq, rf);
}

static inline void
rq_lock(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock(&rq->lock);
	rq_pin_lock(rq, rf);
}

static inline void
rq_relock(struct rq *rq, struct rq_flags *rf)
	__acquires(rq->lock)
{
	raw_spin_lock(&rq->lock);
	rq_repin_lock(rq, rf);
}

static inline void
rq_unlock_irqrestore(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_unlock_irqrestore(&rq->lock, rf->flags);
}

static inline void
rq_unlock_irq(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_unlock_irq(&rq->lock);
}

static inline void
rq_unlock(struct rq *rq, struct rq_flags *rf)
	__releases(rq->lock)
{
	rq_unpin_lock(rq, rf);
	raw_spin_unlock(&rq->lock);
}

static inline struct rq *
this_rq_lock_irq(struct rq_flags *rf)
	__acquires(rq->lock)
{
	struct rq *rq;

	local_irq_disable();
	rq = this_rq();
	rq_lock(rq, rf);
	return rq;
}

#ifdef CONFIG_NUMA
enum numa_topology_type {
	NUMA_DIRECT,
	NUMA_GLUELESS_MESH,
	NUMA_BACKPLANE,
};
extern enum numa_topology_type sched_numa_topology_type;
extern int sched_max_numa_distance;
extern bool find_numa_distance(int distance);
#endif

#ifdef CONFIG_NUMA
extern void sched_init_numa(void);
extern void sched_domains_numa_masks_set(unsigned int cpu);
extern void sched_domains_numa_masks_clear(unsigned int cpu);
#else
static inline void sched_init_numa(void) { }
static inline void sched_domains_numa_masks_set(unsigned int cpu) { }
static inline void sched_domains_numa_masks_clear(unsigned int cpu) { }
#endif

#ifdef CONFIG_NUMA_BALANCING
/* The regions in numa_faults array from task_struct */
enum numa_faults_stats {
	NUMA_MEM = 0,
	NUMA_CPU,
	NUMA_MEMBUF,
	NUMA_CPUBUF
};
extern void sched_setnuma(struct task_struct *p, int node);
extern int migrate_task_to(struct task_struct *p, int cpu);
extern void init_numa_balancing(unsigned long clone_flags, struct task_struct *p);
#else
static inline void
init_numa_balancing(unsigned long clone_flags, struct task_struct *p)
{
}
#endif /* CONFIG_NUMA_BALANCING */

extern int migrate_swap(struct task_struct *p, struct task_struct *t,
			int cpu, int scpu);

#ifdef CONFIG_SMP

static inline void
queue_balance_callback(struct rq *rq,
		       struct callback_head *head,
		       void (*func)(struct rq *rq))
{
	lockdep_assert_held(&rq->lock);

	if (unlikely(head->next))
		return;

	head->func = (void (*)(struct callback_head *))func;
	head->next = rq->balance_callback;
	rq->balance_callback = head;
}

#define rcu_dereference_check_sched_domain(p) \
	rcu_dereference_check((p), \
			      lockdep_is_held(&sched_domains_mutex))

/*
 * The domain tree (rq->sd) is protected by RCU's quiescent state transition.
 * See detach_destroy_domains: synchronize_sched for details.
 *
 * The domain tree of any CPU may only be accessed from within
 * preempt-disabled sections.
 */
#define for_each_domain(cpu, __sd) \
	for (__sd = rcu_dereference_check_sched_domain(cpu_rq(cpu)->sd); \
			__sd; __sd = __sd->parent)

/**
 * highest_flag_domain - Return highest sched_domain containing flag.
 * @cpu:	The CPU whose highest level of sched domain is to
 *		be returned.
 * @flag:	The flag to check for the highest sched_domain
 *		for the given CPU.
 *
 * Returns the highest sched_domain of a CPU which contains the given flag.
 */
static inline struct sched_domain *highest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd, *hsd = NULL;

	for_each_domain(cpu, sd) {
		if (!(sd->flags & flag))
			break;
		hsd = sd;
	}

	return hsd;
}

static inline struct sched_domain *lowest_flag_domain(int cpu, int flag)
{
	struct sched_domain *sd;

	for_each_domain(cpu, sd) {
		if (sd->flags & flag)
			break;
	}

	return sd;
}

DECLARE_PER_CPU(struct sched_domain *, sd_llc);
DECLARE_PER_CPU(int, sd_llc_size);
DECLARE_PER_CPU(int, sd_llc_id);
DECLARE_PER_CPU(struct sched_domain_shared *, sd_llc_shared);
DECLARE_PER_CPU(struct sched_domain *, sd_numa);
DECLARE_PER_CPU(struct sched_domain *, sd_asym_packing);
DECLARE_PER_CPU(struct sched_domain *, sd_asym_cpucapacity);
extern struct static_key_false sched_asym_cpucapacity;

static __always_inline bool sched_asym_cpucap_active(void)
{
	return static_branch_unlikely(&sched_asym_cpucapacity);
}

struct sched_group_capacity {
	atomic_t		ref;
	/*
	 * CPU capacity of this group, SCHED_CAPACITY_SCALE being max capacity
	 * for a single CPU.
	 */
	unsigned long		capacity;
	unsigned long		min_capacity;		/* Min per-CPU capacity in group */
	unsigned long		max_capacity;		/* Max per-CPU capacity in group */
	int			imbalance;		/* XXX unrelated to capacity but shared group state */

#ifdef CONFIG_SCHED_DEBUG
	int			id;
#endif

	unsigned long		cpumask[0];		/* Balance mask */
};

struct sched_group {
	struct sched_group	*next;			/* Must be a circular list */
	atomic_t		ref;

	unsigned int		group_weight;
	struct sched_group_capacity *sgc;
	int			asym_prefer_cpu;	/* CPU of highest priority in group */

	/*
	 * The CPUs this group covers.
	 *
	 * NOTE: this field is variable length. (Allocated dynamically
	 * by attaching extra space to the end of the structure,
	 * depending on how many CPUs the kernel has booted up with)
	 */
	unsigned long		cpumask[0];
};

static inline struct cpumask *sched_group_span(struct sched_group *sg)
{
	return to_cpumask(sg->cpumask);
}

/*
 * See build_balance_mask().
 */
static inline struct cpumask *group_balance_mask(struct sched_group *sg)
{
	return to_cpumask(sg->sgc->cpumask);
}

/**
 * group_first_cpu - Returns the first CPU in the cpumask of a sched_group.
 * @group: The group whose first CPU is to be returned.
 */
static inline unsigned int group_first_cpu(struct sched_group *group)
{
	return cpumask_first(sched_group_span(group));
}

extern int group_balance_cpu(struct sched_group *sg);

#if defined(CONFIG_SCHED_DEBUG) && defined(CONFIG_SYSCTL)
void register_sched_domain_sysctl(void);
void dirty_sched_domain_sysctl(int cpu);
void unregister_sched_domain_sysctl(void);
#else
static inline void register_sched_domain_sysctl(void)
{
}
static inline void dirty_sched_domain_sysctl(int cpu)
{
}
static inline void unregister_sched_domain_sysctl(void)
{
}
#endif

extern void flush_smp_call_function_from_idle(void);

#else /* !CONFIG_SMP: */
static inline void flush_smp_call_function_from_idle(void) { }
#endif

#include "stats.h"
#include "autogroup.h"

#ifdef CONFIG_CGROUP_SCHED

/*
 * Return the group to which this tasks belongs.
 *
 * We cannot use task_css() and friends because the cgroup subsystem
 * changes that value before the cgroup_subsys::attach() method is called,
 * therefore we cannot pin it and might observe the wrong value.
 *
 * The same is true for autogroup's p->signal->autogroup->tg, the autogroup
 * core changes this before calling sched_move_task().
 *
 * Instead we use a 'copy' which is updated from sched_move_task() while
 * holding both task_struct::pi_lock and rq::lock.
 */
static inline struct task_group *task_group(struct task_struct *p)
{
	return p->sched_task_group;
}

/* Change a task's cfs_rq and parent entity if it moves across CPUs/groups */
static inline void set_task_rq(struct task_struct *p, unsigned int cpu)
{
#if defined(CONFIG_FAIR_GROUP_SCHED) || defined(CONFIG_RT_GROUP_SCHED)
	struct task_group *tg = task_group(p);
#endif

#ifdef CONFIG_FAIR_GROUP_SCHED
	set_task_rq_fair(&p->se, p->se.cfs_rq, tg->cfs_rq[cpu]);
	p->se.cfs_rq = tg->cfs_rq[cpu];
	p->se.parent = tg->se[cpu];
	p->se.depth = tg->se[cpu] ? tg->se[cpu]->depth + 1 : 0;
#endif

#ifdef CONFIG_RT_GROUP_SCHED
	p->rt.rt_rq  = tg->rt_rq[cpu];
	p->rt.parent = tg->rt_se[cpu];
#endif
}

#else /* CONFIG_CGROUP_SCHED */

static inline void set_task_rq(struct task_struct *p, unsigned int cpu) { }
static inline struct task_group *task_group(struct task_struct *p)
{
	return NULL;
}

#endif /* CONFIG_CGROUP_SCHED */

static inline void __set_task_cpu(struct task_struct *p, unsigned int cpu)
{
	set_task_rq(p, cpu);
#ifdef CONFIG_SMP
	/*
	 * After ->cpu is set up to a new value, task_rq_lock(p, ...) can be
	 * successfuly executed on another CPU. We must ensure that updates of
	 * per-task data have been completed by this moment.
	 */
	smp_wmb();
#ifdef CONFIG_THREAD_INFO_IN_TASK
	WRITE_ONCE(p->cpu, cpu);
#else
	WRITE_ONCE(task_thread_info(p)->cpu, cpu);
#endif
	p->wake_cpu = cpu;
#endif
}

/*
 * Tunables that become constants when CONFIG_SCHED_DEBUG is off:
 */
#ifdef CONFIG_SCHED_DEBUG
# include <linux/static_key.h>
# define const_debug __read_mostly
#else
# define const_debug const
#endif

#define sched_feat(x) SCHED_FEAT_##x

extern struct static_key_false sched_numa_balancing;
extern struct static_key_false sched_schedstats;

static inline u64 global_rt_period(void)
{
	return (u64)sysctl_sched_rt_period * NSEC_PER_USEC;
}

static inline u64 global_rt_runtime(void)
{
	if (sysctl_sched_rt_runtime < 0)
		return RUNTIME_INF;

	return (u64)sysctl_sched_rt_runtime * NSEC_PER_USEC;
}

static inline int task_current(struct rq *rq, struct task_struct *p)
{
	return rq->curr == p;
}

static inline int task_running(struct rq *rq, struct task_struct *p)
{
#ifdef CONFIG_SMP
	return p->on_cpu;
#else
	return task_current(rq, p);
#endif
}

static inline int task_on_rq_queued(struct task_struct *p)
{
	return READ_ONCE(p->on_rq) == TASK_ON_RQ_QUEUED;
}

static inline int task_on_rq_migrating(struct task_struct *p)
{
	return READ_ONCE(p->on_rq) == TASK_ON_RQ_MIGRATING;
}

/* Wake flags. The first three directly map to some SD flag value */
#define WF_EXEC     0x02 /* Wakeup after exec; maps to SD_BALANCE_EXEC */
#define WF_FORK     0x04 /* Wakeup after fork; maps to SD_BALANCE_FORK */
#define WF_TTWU     0x08 /* Wakeup;            maps to SD_BALANCE_WAKE */

#define WF_SYNC     0x10 /* Waker goes to sleep after wakeup */
#define WF_MIGRATED 0x20 /* Internal use, task got migrated */
#define WF_ON_CPU   0x40 /* Wakee is on_cpu */

#ifdef CONFIG_SMP
enum {
    __wf_exec_mismatch = BUILD_BUG_ON_ZERO(WF_EXEC  != SD_BALANCE_EXEC),
    __wf_fork_mismatch = BUILD_BUG_ON_ZERO(WF_FORK  != SD_BALANCE_FORK),
    __wf_ttwu_mismatch = BUILD_BUG_ON_ZERO(WF_TTWU  != SD_BALANCE_WAKE),
};
#endif
/*
 * To aid in avoiding the subversion of "niceness" due to uneven distribution
 * of tasks with abnormal "nice" values across CPUs the contribution that
 * each task makes to its run queue's load is weighted according to its
 * scheduling class and "nice" value. For SCHED_NORMAL tasks this is just a
 * scaled version of the new time slice allocation that they receive on time
 * slice expiry etc.
 */

#define WEIGHT_IDLEPRIO		3
#define WMULT_IDLEPRIO		1431655765

extern const int		sched_prio_to_weight[40];
extern const u32		sched_prio_to_wmult[40];

/*
 * {de,en}queue flags:
 *
 * DEQUEUE_SLEEP  - task is no longer runnable
 * ENQUEUE_WAKEUP - task just became runnable
 *
 * SAVE/RESTORE - an otherwise spurious dequeue/enqueue, done to ensure tasks
 *                are in a known state which allows modification. Such pairs
 *                should preserve as much state as possible.
 *
 * MOVE - paired with SAVE/RESTORE, explicitly does not preserve the location
 *        in the runqueue.
 *
 * ENQUEUE_HEAD      - place at front of runqueue (tail if not specified)
 * ENQUEUE_REPLENISH - CBS (replenish runtime and postpone deadline)
 * ENQUEUE_MIGRATED  - the task was migrated during wakeup
 *
 */

#define DEQUEUE_SLEEP		0x01 /* Matches ENQUEUE_WAKEUP */
#define DEQUEUE_SAVE		0x02 /* Matches ENQUEUE_RESTORE */
#define DEQUEUE_MOVE		0x04 /* Matches ENQUEUE_MOVE */
#define DEQUEUE_NOCLOCK		0x08 /* Matches ENQUEUE_NOCLOCK */
#define DEQUEUE_SPECIAL		0x10
#define DEQUEUE_DELAYED		0x200 /* Matches ENQUEUE_DELAYED */

#define ENQUEUE_WAKEUP		0x01
#define ENQUEUE_RESTORE		0x02
#define ENQUEUE_MOVE		0x04
#define ENQUEUE_NOCLOCK		0x08

#define ENQUEUE_HEAD		0x10
#define ENQUEUE_REPLENISH	0x20
#ifdef CONFIG_SMP
#define ENQUEUE_MIGRATED	0x40
#else
#define ENQUEUE_MIGRATED	0x00
#endif

#define ENQUEUE_WAKEUP_SYNC	0x80
#define ENQUEUE_INITIAL		0x100
#define ENQUEUE_DELAYED		0x200

#define RETRY_TASK		((void *)-1UL)

extern s64 update_curr_common(struct rq *rq);

struct sched_class {
	const struct sched_class *next;

#ifdef CONFIG_UCLAMP_TASK
	int uclamp_enabled;
#endif

	void (*enqueue_task) (struct rq *rq, struct task_struct *p, int flags);
	bool (*dequeue_task) (struct rq *rq, struct task_struct *p, int flags);
	void (*yield_task)   (struct rq *rq);
	bool (*yield_to_task)(struct rq *rq, struct task_struct *p);

	void (*check_preempt_curr)(struct rq *rq, struct task_struct *p, int flags);

	struct task_struct *(*pick_next_task)(struct rq *rq);

	void (*put_prev_task)(struct rq *rq, struct task_struct *p);
	void (*set_next_task)(struct rq *rq, struct task_struct *p, bool first);

#ifdef CONFIG_SMP
	int (*balance)(struct rq *rq, struct task_struct *prev, struct rq_flags *rf);
	int  (*select_task_rq)(struct task_struct *p, int task_cpu, int flags);
	struct task_struct * (*pick_task)(struct rq *rq);

	void (*migrate_task_rq)(struct task_struct *p, int new_cpu);

	void (*task_woken)(struct rq *this_rq, struct task_struct *task);

	void (*set_cpus_allowed)(struct task_struct *p,
				 const struct cpumask *newmask);

	void (*rq_online)(struct rq *rq);
	void (*rq_offline)(struct rq *rq);
#endif

	void (*task_tick)(struct rq *rq, struct task_struct *p, int queued);
	void (*task_fork)(struct task_struct *p);
	void (*task_dead)(struct task_struct *p);

	/*
	 * The switched_from() call is allowed to drop rq->lock, therefore we
	 * cannot assume the switched_from/switched_to pair is serliazed by
	 * rq->lock. They are however serialized by p->pi_lock.
	 */
	void (*switched_from)(struct rq *this_rq, struct task_struct *task);
	void (*switched_to)  (struct rq *this_rq, struct task_struct *task);
	void (*prio_changed) (struct rq *this_rq, struct task_struct *task,
			      int oldprio);

	unsigned int (*get_rr_interval)(struct rq *rq,
					struct task_struct *task);

	void (*update_curr)(struct rq *rq);

#ifdef CONFIG_FAIR_GROUP_SCHED
	void (*task_change_group)(struct task_struct *p);
#endif

};


static inline void put_prev_task(struct rq *rq, struct task_struct *prev)
{
	WARN_ON_ONCE(rq->curr != prev);
	prev->sched_class->put_prev_task(rq, prev);
}

static inline void set_next_task(struct rq *rq, struct task_struct *next)
{
	WARN_ON_ONCE(rq->curr != next);
	next->sched_class->set_next_task(rq, next, false);
}

#ifdef CONFIG_SMP
#define sched_class_highest (&stop_sched_class)
#else
#define sched_class_highest (&dl_sched_class)
#endif

#define for_class_range(class, _from, _to) \
	for (class = (_from); class != (_to); class = class->next)

#define for_each_class(class) \
	for_class_range(class, sched_class_highest, NULL)

extern const struct sched_class stop_sched_class;
extern const struct sched_class dl_sched_class;
extern const struct sched_class rt_sched_class;
extern const struct sched_class fair_sched_class;
extern const struct sched_class idle_sched_class;

static inline bool sched_stop_runnable(struct rq *rq)
{
	return rq->stop && task_on_rq_queued(rq->stop);
}

static inline bool sched_dl_runnable(struct rq *rq)
{
	return rq->dl.dl_nr_running > 0;
}

static inline bool sched_rt_runnable(struct rq *rq)
{
	return rq->rt.rt_queued > 0;
}

static inline bool sched_fair_runnable(struct rq *rq)
{
	return rq->cfs.nr_running > 0;
}

extern struct task_struct *pick_next_task_fair(struct rq *rq, struct task_struct *prev, struct rq_flags *rf);
extern struct task_struct *pick_next_task_idle(struct rq *rq);

#ifdef CONFIG_SMP

extern void update_group_capacity(struct sched_domain *sd, int cpu);

extern void trigger_load_balance(struct rq *rq);

extern void set_cpus_allowed_common(struct task_struct *p, const struct cpumask *new_mask);

bool __cpu_overutilized(int cpu, int delta);
bool cpu_overutilized(int cpu);

#endif

#ifdef CONFIG_CPU_IDLE
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
	rq->idle_state = idle_state;
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	SCHED_WARN_ON(!rcu_read_lock_sched_held());

	return rq->idle_state;
}

#else
static inline void idle_set_state(struct rq *rq,
				  struct cpuidle_state *idle_state)
{
}

static inline struct cpuidle_state *idle_get_state(struct rq *rq)
{
	return NULL;
}

#endif

extern void schedule_idle(void);

extern void sysrq_sched_debug_show(void);
extern void sched_init_granularity(void);
extern void update_max_interval(void);

extern void init_sched_dl_class(void);
extern void init_sched_rt_class(void);
extern void init_sched_fair_class(void);

extern void reweight_task(struct task_struct *p, const struct load_weight *lw);

extern void resched_curr(struct rq *rq);
extern void resched_cpu(int cpu);

extern struct rt_bandwidth def_rt_bandwidth;
extern void init_rt_bandwidth(struct rt_bandwidth *rt_b, u64 period, u64 runtime);

extern void init_dl_bandwidth(struct dl_bandwidth *dl_b, u64 period, u64 runtime);
extern void init_dl_task_timer(struct sched_dl_entity *dl_se);
extern void init_dl_inactive_task_timer(struct sched_dl_entity *dl_se);
extern void init_dl_rq_bw_ratio(struct dl_rq *dl_rq);

#define BW_SHIFT		20
#define BW_UNIT			(1 << BW_SHIFT)
#define RATIO_SHIFT		8
unsigned long to_ratio(u64 period, u64 runtime);

extern void init_entity_runnable_average(struct sched_entity *se);
extern void post_init_entity_util_avg(struct task_struct *p);

#ifdef CONFIG_NO_HZ_FULL
extern bool sched_can_stop_tick(struct rq *rq);
extern int __init sched_tick_offload_init(void);

/*
 * Tick may be needed by tasks in the runqueue depending on their policy and
 * requirements. If tick is needed, lets send the target an IPI to kick it out of
 * nohz mode if necessary.
 */
static inline void sched_update_tick_dependency(struct rq *rq)
{
	int cpu;

	if (!tick_nohz_full_enabled())
		return;

	cpu = cpu_of(rq);

	if (!tick_nohz_full_cpu(cpu))
		return;

	if (sched_can_stop_tick(rq))
		tick_nohz_dep_clear_cpu(cpu, TICK_DEP_BIT_SCHED);
	else
		tick_nohz_dep_set_cpu(cpu, TICK_DEP_BIT_SCHED);
}
#else
static inline int sched_tick_offload_init(void) { return 0; }
static inline void sched_update_tick_dependency(struct rq *rq) { }
#endif

static inline void add_nr_running(struct rq *rq, unsigned count)
{
	unsigned prev_nr = rq->nr_running;

	rq->nr_running = prev_nr + count;

	if (prev_nr < 2 && rq->nr_running >= 2) {
#ifdef CONFIG_SMP
		set_rd_overload(rq->rd, SG_OVERLOAD);
#endif
	}

	sched_update_tick_dependency(rq);
}

static inline void sub_nr_running(struct rq *rq, unsigned count)
{
	rq->nr_running -= count;
	/* Check if we still need preemption */
	sched_update_tick_dependency(rq);
}

static inline void __block_task(struct rq *rq, struct task_struct *p)
{
	if (p->sched_contributes_to_load)
		rq->nr_uninterruptible++;

	if (p->in_iowait) {
		atomic_inc(&rq->nr_iowait);
		delayacct_blkio_start();
	}

	/*
	 * The moment this write goes through, ttwu() can swoop in and migrate
	 * this task, rendering our rq->__lock ineffective.
	 *
	 * __schedule()				try_to_wake_up()
	 *   LOCK rq->__lock			  LOCK p->pi_lock
	 *   pick_next_task()
	 *     pick_next_task_fair()
	 *       pick_next_entity()
	 *         dequeue_entities()
	 *           __block_task()
	 *             RELEASE p->on_rq = 0	  if (p->on_rq && ...)
	 *					    break;
	 *
	 *					  ACQUIRE (after ctrl-dep)
	 *
	 *					  cpu = select_task_rq();
	 *					  set_task_cpu(p, cpu);
	 *					  ttwu_queue()
	 *					    ttwu_do_activate()
	 *					      LOCK rq->__lock
	 *					      activate_task()
	 *					        STORE p->on_rq = 1
	 *   UNLOCK rq->__lock
	 *
	 * Callers must ensure to not reference @p after this -- we no longer
	 * own it.
	 */
	smp_store_release(&p->on_rq, 0);
}

extern void activate_task(struct rq *rq, struct task_struct *p, int flags);
extern void deactivate_task(struct rq *rq, struct task_struct *p, int flags);

extern void check_preempt_curr(struct rq *rq, struct task_struct *p, int flags);

#ifdef CONFIG_PREEMPT_RT
#define SCHED_NR_MIGRATE_BREAK 8
#else
#define SCHED_NR_MIGRATE_BREAK 32
#endif

extern const_debug unsigned int sysctl_sched_nr_migrate;
extern const_debug unsigned int sysctl_sched_migration_cost;

extern unsigned int sysctl_sched_base_slice;

extern unsigned int sysctl_boost_lpmask;
extern unsigned int sysctl_boost_bpmask;
extern unsigned int sysctl_boost_prime;
extern unsigned int sysctl_manual_boost;
extern unsigned int sysctl_suppress_boost;

#ifdef CONFIG_SCHED_HRTICK

/*
 * Use hrtick when:
 *  - enabled by features
 *  - hrtimer is actually high res
 */
static inline int hrtick_enabled(struct rq *rq)
{
	if (!cpu_active(cpu_of(rq)))
		return 0;
	return hrtimer_is_hres_active(&rq->hrtick_timer);
}

static inline int hrtick_enabled_fair(struct rq *rq)
{
	if (!sched_feat(HRTICK))
		return 0;
	return hrtick_enabled(rq);
}

static inline int hrtick_enabled_dl(struct rq *rq)
{
	if (!sched_feat(HRTICK_DL))
		return 0;
	return hrtick_enabled(rq);
}

void hrtick_start(struct rq *rq, u64 delay);

#else

static inline int hrtick_enabled_fair(struct rq *rq)
{
	return 0;
}

static inline int hrtick_enabled_dl(struct rq *rq)
{
	return 0;
}

static inline int hrtick_enabled(struct rq *rq)
{
	return 0;
}

#endif /* CONFIG_SCHED_HRTICK */

#define sched_ravg_window TICK_NSEC
#define sched_ktime_clock ktime_get_ns

#ifndef arch_scale_freq_capacity
static __always_inline
unsigned long arch_scale_freq_capacity(int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

#ifndef arch_scale_max_freq_capacity
struct sched_domain;
static __always_inline
unsigned long arch_scale_max_freq_capacity(struct sched_domain *sd, int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

#ifdef CONFIG_SMP
static inline unsigned long capacity_of(int cpu)
{
	return cpu_rq(cpu)->cpu_capacity;
}

static inline unsigned long capacity_orig_of(int cpu)
{
	return cpu_rq(cpu)->cpu_capacity_orig;
}

static inline unsigned long task_util(struct task_struct *p)
{
	return READ_ONCE(p->se.avg.util_avg);
}

/**
 * Amount of capacity of a CPU that is (estimated to be) used by CFS tasks
 * @cpu: the CPU to get the utilization of
 *
 * The unit of the return value must be the one of capacity so we can compare
 * the utilization with the capacity of the CPU that is available for CFS task
 * (ie cpu_capacity).
 *
 * cfs_rq.avg.util_avg is the sum of running time of runnable tasks plus the
 * recent utilization of currently non-runnable tasks on a CPU. It represents
 * the amount of utilization of a CPU in the range [0..capacity_orig] where
 * capacity_orig is the cpu_capacity available at the highest frequency
 * (arch_scale_freq_capacity()).
 * The utilization of a CPU converges towards a sum equal to or less than the
 * current capacity (capacity_curr <= capacity_orig) of the CPU because it is
 * the running time on this CPU scaled by capacity_curr.
 *
 * The estimated utilization of a CPU is defined to be the maximum between its
 * cfs_rq.avg.util_avg and the sum of the estimated utilization of the tasks
 * currently RUNNABLE on that CPU.
 * This allows to properly represent the expected utilization of a CPU which
 * has just got a big task running since a long sleep period. At the same time
 * however it preserves the benefits of the "blocked utilization" in
 * describing the potential for other tasks waking up on the same CPU.
 *
 * Nevertheless, cfs_rq.avg.util_avg can be higher than capacity_curr or even
 * higher than capacity_orig because of unfortunate rounding in
 * cfs.avg.util_avg or just after migrating tasks and new task wakeups until
 * the average stabilizes with the new running time. We need to check that the
 * utilization stays within the range of [0..capacity_orig] and cap it if
 * necessary. Without utilization capping, a group could be seen as overloaded
 * (CPU0 utilization at 121% + CPU1 utilization at 80%) whereas CPU1 has 20% of
 * available capacity. We allow utilization to overshoot capacity_curr (but not
 * capacity_orig) as it useful for predicting the capacity required after task
 * migrations (scheduler-driven DVFS).
 *
 * Return: the (estimated) utilization for the specified CPU
 */

static inline unsigned long cpu_util(int cpu);
static inline unsigned long __cpu_util(int cpu)
{
	struct cfs_rq *cfs_rq;
	unsigned int util;

	cfs_rq = &cpu_rq(cpu)->cfs;
	util = READ_ONCE(cfs_rq->avg.util_avg);

	if (sched_feat(UTIL_EST))
		util = max(util, READ_ONCE(cfs_rq->avg.util_est));

	return min_t(unsigned long, util, capacity_orig_of(cpu));
}

static inline unsigned long cpu_util_cum(int cpu, int delta)
{
	u64 util = cpu_rq(cpu)->cfs.avg.util_avg;
	unsigned long capacity = capacity_orig_of(cpu);

	delta += util;
	if (delta < 0)
		return 0;

	return (delta >= capacity) ? capacity : delta;
}

extern unsigned int capacity_margin_freq;

static inline unsigned long
add_capacity_margin(unsigned long cpu_capacity, int cpu)
{
	cpu_capacity  = cpu_capacity * capacity_margin_freq *
			(100 + per_cpu(sched_load_boost, cpu));
	cpu_capacity /= 100;
	cpu_capacity /= SCHED_CAPACITY_SCALE;
	return cpu_capacity;
}

#endif

#ifdef CONFIG_SMP
#ifdef CONFIG_PREEMPT

static inline void double_rq_lock(struct rq *rq1, struct rq *rq2);

/*
 * fair double_lock_balance: Safely acquires both rq->locks in a fair
 * way at the expense of forcing extra atomic operations in all
 * invocations.  This assures that the double_lock is acquired using the
 * same underlying policy as the spinlock_t on this architecture, which
 * reduces latency compared to the unfair variant below.  However, it
 * also adds more overhead and therefore may reduce throughput.
 */
static inline int _double_lock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(this_rq->lock)
	__acquires(busiest->lock)
	__acquires(this_rq->lock)
{
	raw_spin_unlock(&this_rq->lock);
	double_rq_lock(this_rq, busiest);

	return 1;
}

#else
/*
 * Unfair double_lock_balance: Optimizes throughput at the expense of
 * latency by eliminating extra atomic operations when the locks are
 * already in proper order on entry.  This favors lower CPU-ids and will
 * grant the double lock to lower CPUs over higher ids under contention,
 * regardless of entry order into the function.
 */
static inline int _double_lock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(this_rq->lock)
	__acquires(busiest->lock)
	__acquires(this_rq->lock)
{
	int ret = 0;

	if (unlikely(!raw_spin_trylock(&busiest->lock))) {
		if (busiest < this_rq) {
			raw_spin_unlock(&this_rq->lock);
			raw_spin_lock(&busiest->lock);
			raw_spin_lock_nested(&this_rq->lock,
					      SINGLE_DEPTH_NESTING);
			ret = 1;
		} else
			raw_spin_lock_nested(&busiest->lock,
					      SINGLE_DEPTH_NESTING);
	}
	return ret;
}

#endif /* CONFIG_PREEMPT */

/*
 * double_lock_balance - lock the busiest runqueue, this_rq is locked already.
 */
static inline int double_lock_balance(struct rq *this_rq, struct rq *busiest)
{
	if (unlikely(!irqs_disabled())) {
		/* printk() doesn't work well under rq->lock */
		raw_spin_unlock(&this_rq->lock);
		BUG_ON(1);
	}

	return _double_lock_balance(this_rq, busiest);
}

static inline void double_unlock_balance(struct rq *this_rq, struct rq *busiest)
	__releases(busiest->lock)
{
	raw_spin_unlock(&busiest->lock);
	lock_set_subclass(&this_rq->lock.dep_map, 0, _RET_IP_);
}

static inline void double_lock(spinlock_t *l1, spinlock_t *l2)
{
	if (l1 > l2)
		swap(l1, l2);

	spin_lock(l1);
	spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

static inline void double_lock_irq(spinlock_t *l1, spinlock_t *l2)
{
	if (l1 > l2)
		swap(l1, l2);

	spin_lock_irq(l1);
	spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

static inline void double_raw_lock(raw_spinlock_t *l1, raw_spinlock_t *l2)
{
	if (l1 > l2)
		swap(l1, l2);

	raw_spin_lock(l1);
	raw_spin_lock_nested(l2, SINGLE_DEPTH_NESTING);
}

/*
 * double_rq_lock - safely lock two runqueues
 *
 * Note this does not disable interrupts like task_rq_lock,
 * you need to do so manually before calling.
 */
static inline void double_rq_lock(struct rq *rq1, struct rq *rq2)
	__acquires(rq1->lock)
	__acquires(rq2->lock)
{
	WARN_ON_ONCE(!irqs_disabled());
	if (rq1 == rq2) {
		raw_spin_lock(&rq1->lock);
		__acquire(rq2->lock);	/* Fake it out ;) */
	} else {
		if (rq1 < rq2) {
			raw_spin_lock(&rq1->lock);
			raw_spin_lock_nested(&rq2->lock, SINGLE_DEPTH_NESTING);
		} else {
			raw_spin_lock(&rq2->lock);
			raw_spin_lock_nested(&rq1->lock, SINGLE_DEPTH_NESTING);
		}
	}
}

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
static inline void double_rq_unlock(struct rq *rq1, struct rq *rq2)
	__releases(rq1->lock)
	__releases(rq2->lock)
{
	raw_spin_unlock(&rq1->lock);
	if (rq1 != rq2)
		raw_spin_unlock(&rq2->lock);
	else
		__release(rq2->lock);
}

extern void set_rq_online (struct rq *rq);
extern void set_rq_offline(struct rq *rq);
extern bool sched_smp_initialized;

#else /* CONFIG_SMP */

/*
 * double_rq_lock - safely lock two runqueues
 *
 * Note this does not disable interrupts like task_rq_lock,
 * you need to do so manually before calling.
 */
static inline void double_rq_lock(struct rq *rq1, struct rq *rq2)
	__acquires(rq1->lock)
	__acquires(rq2->lock)
{
	WARN_ON_ONCE(!irqs_disabled());
	WARN_ON_ONCE(rq1 != rq2);
	raw_spin_lock(&rq1->lock);
	__acquire(rq2->lock);	/* Fake it out ;) */
}

/*
 * double_rq_unlock - safely unlock two runqueues
 *
 * Note this does not restore interrupts like task_rq_unlock,
 * you need to do so manually after calling.
 */
static inline void double_rq_unlock(struct rq *rq1, struct rq *rq2)
	__releases(rq1->lock)
	__releases(rq2->lock)
{
	WARN_ON_ONCE(rq1 != rq2);
	raw_spin_unlock(&rq1->lock);
	__release(rq2->lock);
}

#endif

extern struct sched_entity *__pick_root_entity(struct cfs_rq *cfs_rq);
extern struct sched_entity *__pick_first_entity(struct cfs_rq *cfs_rq);
extern struct sched_entity *__pick_last_entity(struct cfs_rq *cfs_rq);

#ifdef	CONFIG_SCHED_DEBUG
extern bool sched_debug_enabled;

extern void print_cfs_stats(struct seq_file *m, int cpu);
extern void print_rt_stats(struct seq_file *m, int cpu);
extern void print_dl_stats(struct seq_file *m, int cpu);
extern void print_cfs_rq(struct seq_file *m, int cpu, struct cfs_rq *cfs_rq);
extern void print_rt_rq(struct seq_file *m, int cpu, struct rt_rq *rt_rq);
extern void print_dl_rq(struct seq_file *m, int cpu, struct dl_rq *dl_rq);
#ifdef CONFIG_NUMA_BALANCING
extern void
show_numa_stats(struct task_struct *p, struct seq_file *m);
extern void
print_numa_stats(struct seq_file *m, int node, unsigned long tsf,
	unsigned long tpf, unsigned long gsf, unsigned long gpf);
#endif /* CONFIG_NUMA_BALANCING */
#endif /* CONFIG_SCHED_DEBUG */

extern void init_cfs_rq(struct cfs_rq *cfs_rq);
extern void init_rt_rq(struct rt_rq *rt_rq);
extern void init_dl_rq(struct dl_rq *dl_rq);

extern void cfs_bandwidth_usage_inc(void);
extern void cfs_bandwidth_usage_dec(void);

#ifdef CONFIG_NO_HZ_COMMON
#define NOHZ_BALANCE_KICK_BIT	0
#define NOHZ_STATS_KICK_BIT	1
#define NOHZ_NEWILB_KICK_BIT	2
#define NOHZ_NEXT_KICK_BIT	3

/* Run rebalance_domains() */
#define NOHZ_BALANCE_KICK	BIT(NOHZ_BALANCE_KICK_BIT)
/* Update blocked load */
#define NOHZ_STATS_KICK		BIT(NOHZ_STATS_KICK_BIT)
/* Update blocked load when entering idle */
#define NOHZ_NEWILB_KICK	BIT(NOHZ_NEWILB_KICK_BIT)
/* Update nohz.next_balance */
#define NOHZ_NEXT_KICK		BIT(NOHZ_NEXT_KICK_BIT)

#define NOHZ_KICK_MASK	(NOHZ_BALANCE_KICK | NOHZ_STATS_KICK | NOHZ_NEXT_KICK)

#define nohz_flags(cpu)	(&cpu_rq(cpu)->nohz_flags)

extern void nohz_balance_exit_idle(struct rq *rq);
#else
static inline void nohz_balance_exit_idle(struct rq *rq) { }
#endif

#if defined(CONFIG_SMP) && defined(CONFIG_NO_HZ_COMMON)
extern void nohz_run_idle_balance(int cpu);
#else
static inline void nohz_run_idle_balance(int cpu) { }
#endif

#ifdef CONFIG_IRQ_TIME_ACCOUNTING
struct irqtime {
	u64			total;
	u64			tick_delta;
	u64			irq_start_time;
	struct u64_stats_sync	sync;
};

DECLARE_PER_CPU(struct irqtime, cpu_irqtime);
extern int sched_clock_irqtime;

static inline int irqtime_enabled(void)
{
	return sched_clock_irqtime;
}

/*
 * Returns the irqtime minus the softirq time computed by ksoftirqd.
 * Otherwise ksoftirqd's sum_exec_runtime is substracted its own runtime
 * and never move forward.
 */
static inline u64 irq_time_read(int cpu)
{
	struct irqtime *irqtime = &per_cpu(cpu_irqtime, cpu);
	unsigned int seq;
	u64 total;

	do {
		seq = __u64_stats_fetch_begin(&irqtime->sync);
		total = irqtime->total;
	} while (__u64_stats_fetch_retry(&irqtime->sync, seq));

	return total;
}
#else

static inline int irqtime_enabled(void)
{
	return 0;
}

#endif /* CONFIG_IRQ_TIME_ACCOUNTING */

#ifdef CONFIG_CPU_FREQ
DECLARE_PER_CPU(struct update_util_data *, cpufreq_update_util_data);

/**
 * cpufreq_update_util - Take a note about CPU utilization changes.
 * @rq: Runqueue to carry out the update for.
 * @flags: Update reason flags.
 *
 * This function is called by the scheduler on the CPU whose utilization is
 * being updated.
 *
 * It can only be called from RCU-sched read-side critical sections.
 *
 * The way cpufreq is currently arranged requires it to evaluate the CPU
 * performance state (frequency/voltage) on a regular basis to prevent it from
 * being stuck in a completely inadequate performance level for too long.
 * That is not guaranteed to happen if the updates are only triggered from CFS
 * and DL, though, because they may not be coming in if only RT tasks are
 * active all the time (or there are RT tasks only).
 *
 * As a workaround for that issue, this function is called periodically by the
 * RT sched class to trigger extra cpufreq updates to prevent it from stalling,
 * but that really is a band-aid.  Going forward it should be replaced with
 * solutions targeted more specifically at RT tasks.
 */
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags)
{
	struct update_util_data *data;
	u64 clock;

	clock = rq_clock(rq);

	data = rcu_dereference_sched(*per_cpu_ptr(&cpufreq_update_util_data,
					cpu_of(rq)));
	if (data)
		data->func(data, rq_clock(rq), flags);
}
#else
static inline void cpufreq_update_util(struct rq *rq, unsigned int flags) {}
#endif /* CONFIG_CPU_FREQ */

#ifdef CONFIG_UCLAMP_TASK
unsigned long uclamp_eff_value(struct task_struct *p, enum uclamp_id clamp_id);

/*
 * When uclamp is compiled in, the aggregation at rq level is 'turned off'
 * by default in the fast path and only gets turned on once userspace performs
 * an operation that requires it.
 *
 * Returns true if userspace opted-in to use uclamp and aggregation at rq level
 * hence is active.
 */
static inline bool uclamp_is_used(void)
{
	return static_branch_likely(&sched_uclamp_used);
}

/*
 * Enabling static branches would get the cpus_read_lock(),
 * check whether uclamp_is_used before enable it to avoid always
 * calling cpus_read_lock(). Because we never disable this
 * static key once enable it.
 */
static inline void sched_uclamp_enable(void)
{
	if (!uclamp_is_used())
		static_branch_enable(&sched_uclamp_used);
}

static inline unsigned long uclamp_rq_get(struct rq *rq,
					  enum uclamp_id clamp_id)
{
	return READ_ONCE(rq->uclamp[clamp_id].value);
}

static inline void uclamp_rq_set(struct rq *rq, enum uclamp_id clamp_id,
				 unsigned int value)
{
	WRITE_ONCE(rq->uclamp[clamp_id].value, value);
}

static inline bool uclamp_rq_is_idle(struct rq *rq)
{
	return rq->uclamp_flags & UCLAMP_FLAG_IDLE;
}

/**
 * uclamp_rq_util_with - clamp @util with @rq and @p effective uclamp values.
 * @rq:		The rq to clamp against. Must not be NULL.
 * @util:	The util value to clamp.
 * @p:		The task to clamp against. Can be NULL if you want to clamp
 *		against @rq only.
 *
 * Clamps the passed @util to the max(@rq, @p) effective uclamp values.
 *
 * If sched_uclamp_used static key is disabled, then just return the util
 * without any clamping since uclamp aggregation at the rq level in the fast
 * path is disabled, rendering this operation a NOP.
 *
 * Use uclamp_eff_value() if you don't care about uclamp values at rq level. It
 * will return the correct effective uclamp value of the task even if the
 * static key is disabled.
 */
static __always_inline
unsigned long uclamp_rq_util_with(struct rq *rq, unsigned long util,
				  struct task_struct *p)
{
	unsigned long min_util = 0;
	unsigned long max_util = 0;

	if (!uclamp_is_used())
		return util;

	if (p) {
		min_util = uclamp_eff_value(p, UCLAMP_MIN);
		max_util = uclamp_eff_value(p, UCLAMP_MAX);

		/*
		 * Ignore last runnable task's max clamp, as this task will
		 * reset it. Similarly, no need to read the rq's min clamp.
		 */
		if (uclamp_rq_is_idle(rq))
			goto out;
	}

	min_util = max_t(unsigned long, min_util, uclamp_rq_get(rq, UCLAMP_MIN));
	max_util = max_t(unsigned long, max_util, uclamp_rq_get(rq, UCLAMP_MAX));
out:
	/*
	 * Since CPU's {min,max}_util clamps are MAX aggregated considering
	 * RUNNABLE tasks with _different_ clamps, we can end up with an
	 * inversion. Fix it now when the clamps are applied.
	 */
	if (unlikely(min_util >= max_util))
		return min_util;

	return clamp(util, min_util, max_util);
}

#else /* CONFIG_UCLAMP_TASK */
static inline unsigned long uclamp_eff_value(struct task_struct *p,
					     enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;

	return SCHED_CAPACITY_SCALE;
}

static inline
unsigned long uclamp_rq_util_with(struct rq *rq, unsigned long util,
				  struct task_struct *p)
{
	return util;
}

static inline bool uclamp_is_used(void)
{
	return false;
}

static inline void sched_uclamp_enable(void) {}

static inline unsigned long uclamp_rq_get(struct rq *rq,
					  enum uclamp_id clamp_id)
{
	if (clamp_id == UCLAMP_MIN)
		return 0;

	return SCHED_CAPACITY_SCALE;
}

static inline void uclamp_rq_set(struct rq *rq, enum uclamp_id clamp_id,
				 unsigned int value)
{
}

static inline bool uclamp_rq_is_idle(struct rq *rq)
{
	return false;
}
#endif /* CONFIG_UCLAMP_TASK */

unsigned long task_util_est(struct task_struct *p);
unsigned int uclamp_task(struct task_struct *p);
bool uclamp_latency_sensitive(struct task_struct *p);
bool uclamp_boosted(struct task_struct *p);

#ifdef arch_scale_freq_capacity
# ifndef arch_scale_freq_invariant
#  define arch_scale_freq_invariant()	true
# endif
#else
# define arch_scale_freq_invariant()	false
#endif

extern struct cpumask min_cap_cpu_mask;
#ifdef CONFIG_SMP
static inline unsigned long cpu_util_cfs(struct rq *rq)
{
	unsigned long util = READ_ONCE(rq->cfs.avg.util_avg);

	if (sched_feat(UTIL_EST)) {
		util = max_t(unsigned long, util,
			     READ_ONCE(rq->cfs.avg.util_est));
	}

	return util;
}
#endif

#ifdef CONFIG_CPU_FREQ_GOV_SCHEDUTIL

unsigned long schedutil_cpu_util(int cpu, unsigned long util_cfs,
				 unsigned long *min,
				 unsigned long *max);

unsigned long sugov_effective_cpu_perf(int cpu, unsigned long actual,
				 unsigned long min,
				 unsigned long max);

/*
 * Verify the fitness of task @p to run on @cpu taking into account the
 * CPU original capacity and the runtime/deadline ratio of the task.
 *
 * The function will return true if the original capacity of @cpu is
 * greater than or equal to task's deadline density right shifted by
 * (BW_SHIFT - SCHED_CAPACITY_SHIFT) and false otherwise.
 */
static inline bool dl_task_fits_capacity(struct task_struct *p, int cpu)
{
	unsigned long cap = arch_scale_cpu_capacity(cpu);

	return cap >= p->dl.dl_density >> (BW_SHIFT - SCHED_CAPACITY_SHIFT);
}

static inline unsigned long cpu_bw_dl(struct rq *rq)
{
	return (rq->dl.running_bw * SCHED_CAPACITY_SCALE) >> BW_SHIFT;
}

static inline unsigned long cpu_util_dl(struct rq *rq)
{
	return READ_ONCE(rq->avg_dl.util_avg);
}

static inline unsigned long cpu_util_rt(struct rq *rq)
{
	return READ_ONCE(rq->avg_rt.util_avg);
}

#else /* CONFIG_CPU_FREQ_GOV_SCHEDUTIL */
static inline unsigned long schedutil_cpu_util(int cpu, unsigned long util_cfs,
				 unsigned long max, enum schedutil_type type,
				 struct task_struct *p)
{
	return 0;
}

static inline unsigned long cpu_util_rt(struct rq *rq)
{
	return 0;
}
#endif /* CONFIG_CPU_FREQ_GOV_SCHEDUTIL */

#ifdef CONFIG_SMP
static inline unsigned long cpu_util(int cpu)
{
	return min(__cpu_util(cpu) + cpu_util_rt(cpu_rq(cpu)),
		   capacity_orig_of(cpu));
}
#endif

#ifdef CONFIG_HAVE_SCHED_AVG_IRQ
static inline unsigned long cpu_util_irq(struct rq *rq)
{
	return rq->avg_irq.util_avg;
}

static inline
unsigned long scale_irq_capacity(unsigned long util, unsigned long irq, unsigned long max)
{
	util *= (max - irq);
	util /= max;

	return util;

}
#else
static inline unsigned long cpu_util_irq(struct rq *rq)
{
	return 0;
}

static inline
unsigned long scale_irq_capacity(unsigned long util, unsigned long irq, unsigned long max)
{
	return util;
}
#endif

#ifdef CONFIG_ENERGY_MODEL
#define perf_domain_span(pd) (to_cpumask(((pd)->em_pd->cpus)))
DECLARE_STATIC_KEY_FALSE(sched_energy_present);

static inline bool sched_energy_enabled(void)
{
	return static_branch_unlikely(&sched_energy_present);
}

#else
#define perf_domain_span(pd) NULL
static inline bool sched_energy_enabled(void) { return false; }
#endif

enum sched_boost_policy {
	SCHED_BOOST_NONE,
	SCHED_BOOST_ON_BIG,
	SCHED_BOOST_ON_ALL,
};

struct related_thread_group;
struct sched_cluster;

static inline bool task_sched_boost(struct task_struct *p)
{
	return false;
}

static inline bool task_placement_boost_enabled(struct task_struct *p)
{
	return false;
}

static inline void check_for_migration(struct rq *rq, struct task_struct *p) { }

static inline int sched_boost(void)
{
	return 0;
}

static inline bool rt_boost_on_big(void)
{
	return false;
}

static inline bool is_full_throttle_boost(void)
{
	return false;
}

static inline enum sched_boost_policy task_boost_policy(struct task_struct *p)
{
	return SCHED_BOOST_NONE;
}

static inline bool
task_in_cum_window_demand(struct rq *rq, struct task_struct *p)
{
	return false;
}

static inline bool hmp_capable(void) { return false; }
static inline bool is_min_capacity_cpu(int cpu) { return true; }
static inline bool is_max_capacity_cpu(int cpu) { return true; }

static inline int asym_cap_siblings(int cpu1, int cpu2) { return 0; }

static inline bool is_asym_cap_cpu(int cpu) { return false; }

static inline void set_preferred_cluster(struct related_thread_group *grp) { }

static inline bool task_in_related_thread_group(struct task_struct *p)
{
	return false;
}

static inline
struct related_thread_group *task_related_thread_group(struct task_struct *p)
{
	return NULL;
}

static inline bool task_rtg_high_prio(struct task_struct *p)
{
	return false;
}

static inline u32 task_load(struct task_struct *p) { return 0; }
static inline u32 task_pl(struct task_struct *p) { return 0; }

static inline int update_preferred_cluster(struct related_thread_group *grp,
			 struct task_struct *p, u32 old_load, bool from_tick)
{
	return 0;
}

static inline void add_new_task_to_grp(struct task_struct *new) {}

static inline int mark_reserved(int cpu)
{
	return 0;
}

static inline void clear_reserved(int cpu) { }
static inline int alloc_related_thread_groups(void) { return 0; }

#ifdef CONFIG_SMP
static inline unsigned long thermal_cap(int cpu)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

static inline int is_reserved(int cpu)
{
	return 0;
}

static inline enum sched_boost_policy sched_boost_policy(void)
{
	return SCHED_BOOST_NONE;
}

static inline void sched_boost_parse_dt(void) { }

static inline void clear_ed_task(struct task_struct *p, struct rq *rq) { }

static inline bool early_detection_notify(struct rq *rq, u64 wallclock)
{
	return 0;
}

static inline void note_task_waking(struct task_struct *p, u64 wallclock) { }

struct sched_avg_stats {
	int nr;
	int nr_misfit;
	int nr_max;
	int nr_scaled;
};
extern void sched_get_nr_running_avg(struct sched_avg_stats *stats);

extern u64 avg_vruntime(struct cfs_rq *cfs_rq);
extern bool dequeue_task(struct rq *rq, struct task_struct *p, int flags);
