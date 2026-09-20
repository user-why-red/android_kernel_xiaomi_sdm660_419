/* SPDX-License-Identifier: GPL-2.0 */
/*
 * EEVDF accounting — 6.11 policy, included from fair.c.
 *
 * V is the weighted average of entity vruntimes:
 *
 *   avg_vruntime := \Sum (v_i - min_vruntime) * w_i
 *   avg_load     := \Sum w_i
 */

static inline s64 entity_key(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	return (s64)(se->vruntime - cfs_rq->min_vruntime);
}

static void avg_vruntime_add(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	unsigned long weight = scale_load_down(se->load.weight);
	s64 key = entity_key(cfs_rq, se);

	cfs_rq->avg_vruntime += key * weight;
	cfs_rq->avg_load += weight;
}

static void avg_vruntime_sub(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	unsigned long weight = scale_load_down(se->load.weight);
	s64 key = entity_key(cfs_rq, se);

	cfs_rq->avg_vruntime -= key * weight;
	cfs_rq->avg_load -= weight;
}

static inline void avg_vruntime_update(struct cfs_rq *cfs_rq, s64 delta)
{
	cfs_rq->avg_vruntime -= cfs_rq->avg_load * delta;
}

static u64 avg_vruntime(struct cfs_rq *cfs_rq)
{
	struct sched_entity *curr = cfs_rq->curr;
	s64 avg = cfs_rq->avg_vruntime;
	long load = cfs_rq->avg_load;

	if (curr && curr->on_rq) {
		unsigned long weight = scale_load_down(curr->load.weight);

		avg += entity_key(cfs_rq, curr) * weight;
		load += weight;
	}

	if (load) {
		if (avg < 0)
			avg -= (load - 1);
		avg = div_s64(avg, load);
	}

	return cfs_rq->min_vruntime + avg;
}

static int vruntime_eligible(struct cfs_rq *cfs_rq, u64 vruntime)
{
	struct sched_entity *curr = cfs_rq->curr;
	s64 avg = cfs_rq->avg_vruntime;
	long load = cfs_rq->avg_load;

	if (curr && curr->on_rq) {
		unsigned long weight = scale_load_down(curr->load.weight);

		avg += entity_key(cfs_rq, curr) * weight;
		load += weight;
	}

	return avg >= (s64)(vruntime - cfs_rq->min_vruntime) * load;
}

static int entity_eligible(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	return vruntime_eligible(cfs_rq, se->vruntime);
}

static s64 entity_lag(u64 avruntime, struct sched_entity *se)
{
	s64 vlag, limit;
	u64 slice = se->slice;

	if (!slice)
		slice = sysctl_sched_min_granularity;

	vlag = avruntime - se->vruntime;
	limit = calc_delta_fair(max_t(u64, 2 * slice, TICK_NSEC), se);

	return clamp(vlag, -limit, limit);
}

static void update_entity_lag(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	WARN_ON_ONCE(!se->on_rq);
	se->vlag = entity_lag(avg_vruntime(cfs_rq), se);
}

static void eevdf_place_entity(struct cfs_rq *cfs_rq, struct sched_entity *se,
			       int initial)
{
	u64 vruntime = avg_vruntime(cfs_rq);
	s64 lag = 0;

	if (!se->slice)
		se->slice = sysctl_sched_min_granularity;

	if (sched_feat(PLACE_LAG) && cfs_rq->nr_running) {
		struct sched_entity *curr = cfs_rq->curr;
		unsigned long load;

		lag = se->vlag;
		load = cfs_rq->avg_load;
		if (curr && curr->on_rq)
			load += scale_load_down(curr->load.weight);

		lag *= load + scale_load_down(se->load.weight);
		if (WARN_ON_ONCE(!load))
			load = 1;
		lag = div_s64(lag, load);
	}

	se->vruntime = vruntime - lag;
}
