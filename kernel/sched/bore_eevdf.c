/* SPDX-License-Identifier: GPL-2.0 */
/*
 * BORE apply path for EEVDF: burst score becomes CFS load.weight.
 */

#define BORE_PRIO_MAX 39

static u8 effective_prio_bore(struct task_struct *p)
{
	int prio = p->static_prio - MAX_RT_PRIO;

	prio += bore_apply_score(p);
	if (prio < 0)
		prio = 0;
	if (prio > BORE_PRIO_MAX)
		prio = BORE_PRIO_MAX;
	return prio;
}

static u64 bore_eevdf_slice(struct task_struct *p)
{
	u8 score = bore_apply_score(p);
	u64 slice = sysctl_sched_min_granularity;

	if (score)
		slice >>= score / 8;
	if (slice < 100000ULL)
		slice = 100000ULL;
	return slice;
}

static void bore_eevdf_commit(struct task_struct *p)
{
	if (p->bore.stop_update)
		return;
	if (p->sched_class != &fair_sched_class)
		return;
	if (!p->se.deadline)
		return;

	p->bore.stop_update = 1;
	reweight_task(p, effective_prio_bore(p));
	sched_eevdf_apply_slice(&p->se, bore_eevdf_slice(p));
	p->bore.stop_update = 0;
}
