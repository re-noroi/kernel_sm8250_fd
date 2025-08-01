/* SPDX-License-Identifier: GPL-2.0 */

#define SCHED_FEAT_ENFORCE_ELIGIBILITY 0
/*
 * Using the avg_vruntime, do the right thing and preserve lag across
 * sleep+wake cycles. EEVDF placement strategy #1, #2 if disabled.
 */
#define SCHED_FEAT_PLACE_LAG 0
/*
 * Give new tasks half a slice to ease into the competition.
 */
#define SCHED_FEAT_PLACE_DEADLINE_INITIAL 0
/*
 * Preserve relative virtual deadline on 'migration'.
 */
#define SCHED_FEAT_PLACE_REL_DEADLINE 0
/*
 * Inhibit (wakeup) preemption until the current task has either matched the
 * 0-lag point or until is has exhausted it's slice.
 */
#define SCHED_FEAT_RUN_TO_PARITY 0
/*
 * Allow wakeup of tasks with a shorter slice to cancel RESPECT_SLICE for
 * current.
 */
#define SCHED_FEAT_PREEMPT_SHORT 1
/*
 * Prefer to schedule the task we woke last (assuming it failed
 * wakeup-preemption), since its likely going to consume data we
 * touched, increases cache locality.
 */
#define SCHED_FEAT_NEXT_BUDDY 0

/*
 * Allow completely ignoring cfs_rq->next; which can be set from various
 * places:
 *   - NEXT_BUDDY (wakeup preemption)
 *   - yield_to_task()
 *   - cgroup dequeue / pick
 */
#define SCHED_FEAT_PICK_BUDDY 1

/*
 * Consider buddies to be cache hot, decreases the likeliness of a
 * cache buddy being migrated away, increases cache locality.
 */
#define SCHED_FEAT_CACHE_HOT_BUDDY 0

/*
 * Delay dequeueing tasks until they get selected or woken.
 *
 * By delaying the dequeue for non-eligible tasks, they remain in the
 * competition and can burn off their negative lag. When they get selected
 * they'll have positive lag by definition.
 *
 * DELAY_ZERO clips the lag on dequeue (or wakeup) to 0.
 */
#define SCHED_FEAT_DELAY_DEQUEUE 1
#define SCHED_FEAT_DELAY_ZERO 1

/*
 * Allow wakeup-time preemption of the current task:
 */
#define SCHED_FEAT_WAKEUP_PREEMPTION 1

#define SCHED_FEAT_HRTICK 0
#define SCHED_FEAT_HRTICK_DL 0

/*
 * Decrement CPU capacity based on time not spent running tasks
 */
#define SCHED_FEAT_NONTASK_CAPACITY 1

/*
 * Queue remote wakeups on the target CPU and process them
 * using the scheduler IPI. Reduces rq->lock contention/bounces.
 */
#define SCHED_FEAT_TTWU_QUEUE 0

/*
 * When doing wakeups, attempt to limit superfluous scans of the LLC domain.
 */
#define SCHED_FEAT_SIS_PROP 0
#define SCHED_FEAT_SIS_UTIL 1

/*
 * Issue a WARN when we do multiple update_rq_clock() calls
 * in a single rq->lock section. Default disabled because the
 * annotations are not complete.
 */
#define SCHED_FEAT_WARN_DOUBLE_CLOCK 0

#if defined(CONFIG_IRQ_WORK) && defined(CONFIG_SMP)
/*
 * In order to avoid a thundering herd attack of CPUs that are
 * lowering their priorities at the same time, and there being
 * a single CPU that has an RT task that can migrate and is waiting
 * to run, where the other CPUs will try to take that CPUs
 * rq lock and possibly create a large contention, sending an
 * IPI to that CPU and let that CPU push the RT task to where
 * it should go may be a better scenario.
 */
#define SCHED_FEAT_RT_PUSH_IPI 1
#else
#define SCHED_FEAT_RT_PUSH_IPI 0
#endif

#define SCHED_FEAT_RT_RUNTIME_SHARE 0
#define SCHED_FEAT_LB_MIN 0
#define SCHED_FEAT_ATTACH_AGE_LOAD 1

#define SCHED_FEAT_WA_IDLE 1
#define SCHED_FEAT_WA_WEIGHT 1
#define SCHED_FEAT_WA_BIAS 1

/*
 * UtilEstimation. Use estimated CPU utilization.
 */
#define SCHED_FEAT_UTIL_EST 1

/*
 * Energy aware scheduling algorithm choices:
 * EAS_PREFER_IDLE
 *   Direct tasks in a schedtune.prefer_idle=1 group through
 *   the EAS path for wakeup task placement. Otherwise, put
 *   those tasks through the mainline slow path.
 */
#define SCHED_FEAT_EAS_PREFER_IDLE 1

/*
 * Request max frequency from schedutil whenever a RT task is running.
 */
#define SCHED_FEAT_SUGOV_RT_MAX_FREQ 0
