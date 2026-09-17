// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2023-2024 Sultan Alsawaf <sultan@kerneltoast.com>.
 *
 * Backported to 4.19: cpus_allowed, arch_scale_cpu_capacity(sd, cpu),
 * select_task_rq extra args, rcu_read_lock around idle_get_state.
 */
/**
 * DOC: Capacity Aware Superset Scheduler (CASS) description
 *
 * The Capacity Aware Superset Scheduler (CASS) optimizes runqueue selection of
 * CFS tasks. By using CPU capacity as a basis for comparing the relative
 * utilization between different CPUs, CASS fairly balances load across CPUs of
 * varying capacities. This results in improved multi-core performance,
 * especially when CPUs are overutilized because CASS doesn't clip a CPU's
 * utilization when it eclipses the CPU's capacity.
 *
 * As a superset of capacity aware scheduling, CASS implements a hierarchy of
 * criteria to determine the better CPU to wake a task upon between CPUs that
 * have the same relative utilization. This way, single-core performance,
 * latency, and cache affinity are all optimized where possible.
 *
 * CASS doesn't feature explicit energy awareness but its basic load balancing
 * principle results in decreased overall energy, often better than what is
 * possible with explicit energy awareness. By fairly balancing load based on
 * relative utilization, all CPUs are kept at their lowest P-state necessary to
 * satisfy the overall load at any given moment.
 */

static __always_inline unsigned long cass_cap_orig(int cpu)
{
	return arch_scale_cpu_capacity(NULL, cpu);
}

static __always_inline unsigned long cass_thermal_load(struct rq *rq)
{
	int cpu = cpu_of(rq);
	unsigned long orig = cass_cap_orig(cpu);
	unsigned long scale = arch_scale_max_freq_capacity(NULL, cpu);
	unsigned long capped = orig * scale / SCHED_CAPACITY_SCALE;

	if (capped >= orig)
		return 0;
	return orig - capped;
}


#ifdef CONFIG_UCLAMP_TASK
static __always_inline unsigned long cass_uclamp_min(struct task_struct *p)
{
	/* Floor from uclamp.min, independent of boost_src. That sysctl
	 * only picks stune vs uclamp for *boost*, not the clamp itself.
	 */
	if (!uclamp_is_used())
		return 0;
	return uclamp_eff_value(p, UCLAMP_MIN);
}

static __always_inline unsigned long cass_uclamp_max(struct task_struct *p)
{
	if (!uclamp_is_used())
		return SCHED_CAPACITY_SCALE;
	return uclamp_eff_value(p, UCLAMP_MAX);
}
#else
static __always_inline unsigned long cass_uclamp_min(struct task_struct *p)
{
	return 0;
}

static __always_inline unsigned long cass_uclamp_max(struct task_struct *p)
{
	return SCHED_CAPACITY_SCALE;
}
#endif

struct cass_cpu_cand {
	int cpu;
	unsigned int exit_lat;
	unsigned long cap;
	unsigned long cap_max;
	unsigned long cap_no_therm;
	unsigned long cap_orig;
	unsigned long eff_util;
	unsigned long hard_util;
	unsigned long util;
};

static __always_inline
void cass_cpu_util(struct cass_cpu_cand *c, int this_cpu, bool sync)
{
	struct rq *rq = cpu_rq(c->cpu);
	struct cfs_rq *cfs_rq = &rq->cfs;
	unsigned long est;

	c->util = READ_ONCE(cfs_rq->avg.util_avg);
	if (sched_feat(UTIL_EST)) {
		est = READ_ONCE(cfs_rq->avg.util_est.enqueued);
		if (est > c->util) {
			sync = false;
			c->util = est;
		}
	}

	if (sync && c->cpu == this_cpu && !rt_task(current))
		c->util -= min(c->util, task_util(current));

	c->hard_util = cpu_util_rt(rq) + cpu_util_dl(rq) + cpu_util_irq(rq);
	c->cap = c->cap_max - min(c->hard_util, c->cap_max - 1);
	c->cap_no_therm = c->cap_orig - min(c->hard_util, c->cap_orig - 1);
}

static __always_inline
bool cass_prime_cpu(const struct cass_cpu_cand *c)
{
	if (nr_cpu_ids < 2)
		return false;

	return c->cpu == nr_cpu_ids - 1 &&
	       cass_cap_orig(nr_cpu_ids - 2) != SCHED_CAPACITY_SCALE;
}

static __always_inline bool cass_prefer_idle(struct task_struct *p)
{
	return wake_to_idle(p) || schedtune_prefer_idle(p);
}

static atomic_t cass_boost_count = ATOMIC_INIT(0);

static __always_inline bool cass_boosted(void)
{
	return atomic_read(&cass_boost_count) > 0;
}

static __always_inline bool cass_prefer_high_cap(struct task_struct *p)
{
	if (cass_boosted())
		return true;
	if (schedtune_prefer_high_cap(p))
		return true;
	return per_task_boost(p) > TASK_BOOST_NONE;
}

static __always_inline
bool cass_cpu_better(const struct cass_cpu_cand *a,
		     const struct cass_cpu_cand *b, unsigned long p_util,
		     int this_cpu, int prev_cpu, bool sync,
		     unsigned long uc_max, bool prefer_high_cap)
{
#define cass_cmp(a, b) ({ res = (long)(a) - (long)(b); })
#define cass_eq(a, b) ({ res = (a) == (b); })
	long res;

	/* uclamp.max: a CPU that fits the clamp beats one that does not.
	 * Before relative util so background stays on Silver even if Gold
	 * is idle. If neither CPU fits, fall through.
	 */
	if (uc_max < SCHED_CAPACITY_SCALE &&
	    cass_cmp(a->cap_orig <= uc_max, b->cap_orig <= uc_max))
		goto done;

	/* Unhinted and still fits: stay on the smaller cluster. Remaining
	 * cap used to wake Gold for every idle bg wakeup on 4+4. If the
	 * smaller CPU would go over capacity, skip and let relative util
	 * spill to Gold.
	 */
	if (!prefer_high_cap) {
		bool a_fits = fits_capacity(p_util, a->cap_max, 1280) &&
			      a->eff_util <= a->cap_max;
		bool b_fits = fits_capacity(p_util, b->cap_max, 1280) &&
			      b->eff_util <= b->cap_max;

		if (a_fits && b_fits && cass_cmp(b->cap_orig, a->cap_orig))
			goto done;
		if (a_fits != b_fits && cass_cmp(a_fits, b_fits))
			goto done;
	}

	/* Relative util, fixed-point. Integer div truncated this to 0/0
	 * until a CPU was over capacity, so the primary key never fired.
	 */
	if (cass_cmp(b->eff_util * SCHED_CAPACITY_SCALE / max(b->cap_max, 1UL),
		     a->eff_util * SCHED_CAPACITY_SCALE / max(a->cap_max, 1UL)))
		goto done;

	if (prefer_high_cap && cass_cmp(a->cap_orig, b->cap_orig))
		goto done;

	if (cass_cmp(fits_capacity(p_util, a->cap_max, 1280),
		     fits_capacity(p_util, b->cap_max, 1280)))
		goto done;

	if (cass_cmp(cass_prime_cpu(b), cass_prime_cpu(a)))
		goto done;

	if (cass_cmp(b->util, a->util))
		goto done;

	if (cass_cmp(!!a->exit_lat, !!b->exit_lat))
		goto done;

	if (sync && (cass_eq(a->cpu, this_cpu) || !cass_cmp(b->cpu, this_cpu)))
		goto done;

	if (cass_cmp(a->cap, b->cap))
		goto done;

	if (cass_cmp(b->exit_lat, a->exit_lat))
		goto done;

	if (cass_eq(a->cpu, prev_cpu) || !cass_cmp(b->cpu, prev_cpu))
		goto done;

	if (cass_cmp(cpus_share_cache(a->cpu, prev_cpu),
		     cpus_share_cache(b->cpu, prev_cpu)))
		goto done;
done:
	return res > 0;
}

static int cass_best_cpu(struct task_struct *p, int prev_cpu, bool sync)
{
	struct cass_cpu_cand cands[2], *best = cands;
	int this_cpu = raw_smp_processor_id();
	unsigned long p_util, uc_min, uc_max;
	bool has_idle = false, have_best = false;
	bool prefer_idle, prefer_high_cap;
	int cidx = 0, cpu;

	p_util = task_util_est(p);
	uc_min = cass_uclamp_min(p);
	uc_max = cass_uclamp_max(p);
	prefer_idle = cass_prefer_idle(p);
	prefer_high_cap = cass_prefer_high_cap(p);

	rcu_read_lock();
	for_each_cpu_and(cpu, &p->cpus_allowed, cpu_active_mask) {
		struct cass_cpu_cand *curr = &cands[cidx];
		struct cpuidle_state *idle_state;
		struct rq *rq = cpu_rq(cpu);

		if (cpu_isolated(cpu))
			continue;

		curr->cap_orig = cass_cap_orig(cpu);
		curr->cap_max = curr->cap_orig - min(cass_thermal_load(rq),
						     curr->cap_orig - 1);

		/*
		 * Skip thermally insufficient CPUs only after we have a
		 * real candidate. Comparing against an uninitialized
		 * best->cap_max could skip every CPU and return garbage.
		 */
		if (have_best && curr->cap_max < uc_min &&
		    curr->cap_max < best->cap_max)
			continue;

		curr->cpu = cpu;
		if ((sync && cpu == this_cpu && rq->nr_running == 1) ||
		    available_idle_cpu(cpu) || sched_idle_cpu(cpu)) {
			if (prefer_idle || (!uc_min && !cass_prime_cpu(curr)))
				has_idle = true;
			curr->exit_lat = 1;
			idle_state = idle_get_state(rq);
			if (idle_state)
				curr->exit_lat += idle_state->exit_latency;
		} else {
			if (has_idle)
				continue;
			curr->exit_lat = 0;
		}

		cass_cpu_util(curr, this_cpu, sync);

		if (cpu != task_cpu(p))
			curr->util += p_util;

		curr->eff_util = max(curr->util + curr->hard_util, uc_min);

		if (curr->util < uc_min)
			curr->util = uc_min;

		curr->util =
			curr->util * SCHED_CAPACITY_SCALE / curr->cap_no_therm;

		if (!have_best ||
		    cass_cpu_better(curr, best, max(p_util, uc_min),
				    this_cpu, prev_cpu, sync, uc_max,
				    prefer_high_cap)) {
			best = curr;
			cidx ^= 1;
			have_best = true;
		}
	}
	rcu_read_unlock();

	if (unlikely(!have_best)) {
		if (cpumask_test_cpu(prev_cpu, &p->cpus_allowed))
			return prev_cpu;
		return cpumask_first(&p->cpus_allowed);
	}

	return best->cpu;
}

static int cass_select_task_rq(struct task_struct *p, int prev_cpu,
			       int sd_flag, int wake_flags)
{
	bool sync;

	/* exec/fork are in sd_flag, not wake_flags. execve passes
	 * wake_flags=0 so this never fired, and WF_* can alias the bit.
	 */
	if (sd_flag & SD_BALANCE_EXEC)
		return prev_cpu;

	if (unlikely(!cpumask_intersects(&p->cpus_allowed, cpu_active_mask)))
		return cpumask_first(&p->cpus_allowed);

	if (!(sd_flag & SD_BALANCE_FORK))
		sync_entity_load_avg(&p->se);

	sync = (wake_flags & WF_SYNC) && !(current->flags & PF_EXITING);

	return cass_best_cpu(p, prev_cpu, sync);
}

static int cass_select_task_rq_fair(struct task_struct *p, int prev_cpu,
				    int sd_flag, int wake_flags,
				    int sibling_count_hint)
{
	return cass_select_task_rq(p, prev_cpu, sd_flag, wake_flags);
}

int sched_set_boost(int type)
{
	if (type < -3 || type > 3)
		return -EINVAL;

	if (type > 0)
		atomic_inc(&cass_boost_count);
	else if (type < 0)
		atomic_add_unless(&cass_boost_count, -1, 0);
	else
		atomic_set(&cass_boost_count, 0);

	return 0;
}
EXPORT_SYMBOL_GPL(sched_set_boost);

static bool cass_can_migrate_task(struct task_struct *p, int src_cpu,
				  int dst_cpu)
{
	unsigned long uc_max, src_orig, dst_orig;

	if (cpu_isolated(dst_cpu))
		return false;

	src_orig = cass_cap_orig(src_cpu);
	dst_orig = cass_cap_orig(dst_cpu);
	uc_max = cass_uclamp_max(p);

	/* Don't pull a clamped task onto a CPU wakeup would reject. */
	if (uc_max < SCHED_CAPACITY_SCALE &&
	    src_orig <= uc_max && dst_orig > uc_max)
		return false;

	/* Boosted / prefer_high_cap stays on the bigger CPU. */
	if (cass_prefer_high_cap(p) && dst_orig < src_orig)
		return false;

	return true;
}

