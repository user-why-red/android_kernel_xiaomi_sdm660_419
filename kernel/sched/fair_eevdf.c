/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/rbtree_augmented.h>
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

	if (!se->slice)
		se->slice = sysctl_sched_min_granularity;
	{
		u64 vslice = calc_delta_fair(se->slice, se);

		if (sched_feat(PLACE_DEADLINE_INITIAL) && initial)
			vslice /= 2;
		se->deadline = se->vruntime + vslice;
	}
}

static void clear_buddies(struct cfs_rq *cfs_rq, struct sched_entity *se);

static void update_deadline(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	if ((s64)(se->vruntime - se->deadline) < 0)
		return;

	if (!se->slice)
		se->slice = sysctl_sched_min_granularity;

	se->deadline = se->vruntime + calc_delta_fair(se->slice, se);

	if (cfs_rq->nr_running > 1) {
		resched_curr(rq_of(cfs_rq));
		clear_buddies(cfs_rq, se);
	}
}

#define __node_2_se(node) rb_entry((node), struct sched_entity, run_node)

static inline u64 compute_min_vruntime(struct sched_entity *se)
{
	struct rb_node *node = &se->run_node;
	u64 min_v = se->vruntime;

	if (node->rb_left)
		min_v = min_vruntime(min_v,
			__node_2_se(node->rb_left)->min_vruntime);
	if (node->rb_right)
		min_v = min_vruntime(min_v,
			__node_2_se(node->rb_right)->min_vruntime);
	return min_v;
}

RB_DECLARE_CALLBACKS(static, min_vruntime_cb, struct sched_entity,
		     run_node, u64, min_vruntime, compute_min_vruntime);

static void eevdf_enqueue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	struct rb_node **link = &cfs_rq->tasks_timeline.rb_root.rb_node;
	struct rb_node *parent = NULL;
	struct sched_entity *entry;
	bool leftmost = true;

	avg_vruntime_add(cfs_rq, se);
	se->min_vruntime = se->vruntime;

	while (*link) {
		parent = *link;
		entry = rb_entry(parent, struct sched_entity, run_node);
		if ((s64)(se->deadline - entry->deadline) < 0) {
			link = &parent->rb_left;
		} else {
			link = &parent->rb_right;
			leftmost = false;
		}
	}

	rb_link_node(&se->run_node, parent, link);
	rb_insert_augmented_cached(&se->run_node, &cfs_rq->tasks_timeline,
				   leftmost, &min_vruntime_cb);
}

static void eevdf_dequeue_entity(struct cfs_rq *cfs_rq, struct sched_entity *se)
{
	rb_erase_augmented_cached(&se->run_node, &cfs_rq->tasks_timeline,
				  &min_vruntime_cb);
	avg_vruntime_sub(cfs_rq, se);
}

static struct sched_entity *pick_eevdf(struct cfs_rq *cfs_rq)
{
	struct rb_node *node = cfs_rq->tasks_timeline.rb_root.rb_node;
	struct sched_entity *se = __pick_first_entity(cfs_rq);
	struct sched_entity *curr = cfs_rq->curr;
	struct sched_entity *best = NULL;

	if (cfs_rq->nr_running == 1)
		return curr && curr->on_rq ? curr : se;

	if (curr && (!curr->on_rq || !entity_eligible(cfs_rq, curr)))
		curr = NULL;

	if (sched_feat(RUN_TO_PARITY) && curr && curr->vlag == curr->deadline)
		return curr;

	if (se && entity_eligible(cfs_rq, se)) {
		best = se;
		goto found;
	}

	while (node) {
		struct rb_node *left = node->rb_left;

		if (left && vruntime_eligible(cfs_rq,
				__node_2_se(left)->min_vruntime)) {
			node = left;
			continue;
		}

		se = __node_2_se(node);
		if (entity_eligible(cfs_rq, se)) {
			best = se;
			break;
		}
		node = node->rb_right;
	}
found:
	if (!best || (curr && (s64)(curr->deadline - best->deadline) < 0))
		best = curr;

	return best;
}

static void reweight_eevdf(struct sched_entity *se, u64 avruntime,
			   unsigned long weight)
{
	unsigned long old_weight = se->load.weight;
	s64 vlag, vslice;

	if (!weight)
		return;

	if (avruntime != se->vruntime) {
		vlag = entity_lag(avruntime, se);
		vlag = div_s64(vlag * old_weight, weight);
		se->vruntime = avruntime - vlag;
	}

	vslice = (s64)(se->deadline - avruntime);
	vslice = div_s64(vslice * old_weight, weight);
	se->deadline = avruntime + vslice;
}
