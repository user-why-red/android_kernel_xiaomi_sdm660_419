// SPDX-License-Identifier: GPL-2.0
/*
 * Burst-Oriented Response Enhancer — CFS vruntime scoring.
 * Scoring from firelzrd BORE 6.8; applied as vruntime only on 4.19 CFS.
 */
#include "sched.h"

#ifdef CONFIG_SCHED_BORE

u32 __read_mostly sched_bore = 1;
DEFINE_STATIC_KEY_TRUE(sched_bore_enabled);
u32 __read_mostly sched_burst_inherit_type = 2;
u32 __read_mostly sched_burst_smoothness = 1;
u32 __read_mostly sched_burst_penalty_offset = 24;
u32 __read_mostly sched_burst_penalty_scale = 1280;
u32 __read_mostly sched_burst_cache_lifetime = 60000000;

#define BORE_MAX_PENALTY ((40U << 8) - 1)

static u32 bore_log2p1(u64 v)
{
	int clz, exp;
	u32 mant;

	if (!v)
		return 0;
	clz = __builtin_clzll(v);
	exp = 64 - clz;
	mant = (u32)((v << clz) << 1 >> (64 - 8));
	return (exp << 8) | mant;
}

static u32 calc_burst_penalty(u64 burst_time)
{
	u32 greed = bore_log2p1(burst_time);
	u32 tolerance = sched_burst_penalty_offset << 8;
	s32 diff = (s32)greed - (s32)tolerance;
	u32 scaled;

	if (diff < 0)
		diff = 0;
	scaled = (u32)diff * sched_burst_penalty_scale >> 10;
	if (scaled > BORE_MAX_PENALTY)
		scaled = BORE_MAX_PENALTY;
	return scaled;
}

static u32 binary_smooth(u32 new, u32 old)
{
	s32 inc = (s32)new - (s32)old;

	if (inc >= 0)
		return old + ((u32)inc >> sched_burst_smoothness);
	return new;
}

static void bore_set_penalty(struct task_struct *p)
{
	u16 pen;

	if (p->flags & PF_KTHREAD) {
		p->bore.penalty = 0;
		return;
	}
	pen = p->bore.curr_penalty;
	if (p->bore.prev_penalty > pen)
		pen = p->bore.prev_penalty;
	p->bore.penalty = pen;
}

void update_curr_bore(struct task_struct *p, u64 delta_exec)
{
	u32 curr;

	if (!bore_enabled())
		return;

	p->bore.burst_time += delta_exec;
	curr = calc_burst_penalty(p->bore.burst_time);
	p->bore.curr_penalty = curr;
	if (curr <= p->bore.prev_penalty)
		return;
	bore_set_penalty(p);
}

void restart_burst_bore(struct task_struct *p)
{
	u32 smoothed;

	if (!bore_enabled())
		return;

	smoothed = binary_smooth(p->bore.curr_penalty, p->bore.prev_penalty);
	p->bore.prev_penalty = smoothed;
	p->bore.curr_penalty = 0;
	p->bore.burst_time = 0;
	bore_set_penalty(p);
}

#define BORE_CACHE_SAMPLE 63
#define BORE_CACHE_SCAN (BORE_CACHE_SAMPLE * 2)

u8 bore_apply_score(struct task_struct *p)
{
	if (!bore_enabled())
		return 0;
	if (p->flags & PF_KTHREAD)
		return 0;
	if (p->policy == SCHED_BATCH || p->policy == SCHED_IDLE)
		return 0;
	if ((p->flags & PF_WAKE_UP_IDLE) || schedtune_prefer_idle(p))
		return 0;
#ifdef CONFIG_UCLAMP_TASK
	if (uclamp_is_used()) {
		if (uclamp_eff_value(p, UCLAMP_MAX) < SCHED_CAPACITY_SCALE / 5)
			return 0;
		if (uclamp_eff_value(p, UCLAMP_MIN))
			return 0;
	}
#endif
	return bore_score(p);
}

static bool bore_inheritable(struct task_struct *p)
{
	return p->sched_class == &fair_sched_class &&
	       !p->exit_state && !(p->flags & PF_KTHREAD);
}

static u32 count_children_upto2(struct task_struct *p)
{
	struct list_head *h = &p->children;

	if (h->next == h)
		return 0;
	if (h->next->next == h)
		return 1;
	return 2;
}

static void update_child_burst_cache(struct task_struct *p, u64 now)
{
	struct task_struct *child;
	u32 cnt = 0, sum = 0, scan = 0, avg;

	list_for_each_entry(child, &p->children, sibling) {
		if (scan++ >= BORE_CACHE_SCAN)
			break;
		if (!bore_inheritable(child))
			continue;
		cnt++;
		sum += child->bore.penalty;
		if (cnt >= BORE_CACHE_SAMPLE)
			break;
	}
	avg = cnt ? sum / cnt : 0;
	if (avg < p->bore.penalty)
		avg = p->bore.penalty;
	p->bore.child_burst = avg;
	p->bore.child_burst_cnt = cnt;
	p->bore.child_burst_cached = now;
}

static u32 inherit_from_parent(struct task_struct *parent, u64 now)
{
	if (now - parent->bore.child_burst_cached > sched_burst_cache_lifetime)
		update_child_burst_cache(parent, now);
	return parent->bore.child_burst;
}

static u32 inherit_from_ancestor(struct task_struct *parent, u64 now)
{
	struct task_struct *anc = parent;

	while (anc->real_parent != anc && count_children_upto2(anc) == 1)
		anc = anc->real_parent;
	return inherit_from_parent(anc, now);
}

static u32 inherit_from_thread_group(struct task_struct *p, u64 now)
{
	struct task_struct *leader = p->group_leader;
	struct task_struct *t;
	u32 cnt = 0, sum = 0, scan = 0, avg;

	if (now - leader->bore.group_burst_cached <= sched_burst_cache_lifetime)
		return leader->bore.group_burst;

	for_each_thread(leader, t) {
		if (scan++ >= BORE_CACHE_SCAN)
			break;
		if (!bore_inheritable(t))
			continue;
		cnt++;
		sum += t->bore.penalty;
		if (cnt >= BORE_CACHE_SAMPLE)
			break;
	}
	avg = cnt ? sum / cnt : 0;
	if (avg < leader->bore.penalty)
		avg = leader->bore.penalty;
	leader->bore.group_burst = avg;
	leader->bore.group_burst_cached = now;
	return avg;
}

void task_fork_bore(struct task_struct *p)
{
	u64 now;
	u32 inherited = 0;

	if (!bore_enabled() || !bore_inheritable(p))
		return;

	now = ktime_get_ns();
	read_lock(&tasklist_lock);
	if (!thread_group_leader(p))
		inherited = inherit_from_thread_group(p, now);
	else if (sched_burst_inherit_type == 2)
		inherited = inherit_from_ancestor(p->real_parent, now);
	else if (sched_burst_inherit_type == 1)
		inherited = inherit_from_parent(p->real_parent, now);
	read_unlock(&tasklist_lock);

	if (p->bore.prev_penalty < inherited)
		p->bore.prev_penalty = inherited;
	p->bore.curr_penalty = 0;
	p->bore.burst_time = 0;
	bore_set_penalty(p);
}

void reset_task_bore(struct task_struct *p)
{
	memset(&p->bore, 0, sizeof(p->bore));
}

void __init sched_init_bore(void)
{
	reset_task_bore(&init_task);
	if (sched_bore)
		static_branch_enable(&sched_bore_enabled);
	else
		static_branch_disable(&sched_bore_enabled);
	pr_info("BORE: CFS burst scoring\n");
}

#ifdef CONFIG_SYSCTL
static int bore_zero;
static int bore_one = 1;
static int bore_two = 2;

static int sched_bore_sysctl(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int ret;

	ret = proc_dointvec_minmax(table, write, buffer, lenp, ppos);
	if (ret || !write)
		return ret;
	if (sched_bore)
		static_branch_enable(&sched_bore_enabled);
	else
		static_branch_disable(&sched_bore_enabled);
	return 0;
}

static int bore_three = 3;
static int bore_sixfour = 64;
static int bore_12bit = 4095;

static struct ctl_table sched_bore_table[] = {
	{
		.procname	= "sched_bore",
		.data		= &sched_bore,
		.maxlen		= sizeof(u32),
		.mode		= 0644,
		.proc_handler	= sched_bore_sysctl,
		.extra1		= &bore_zero,
		.extra2		= &bore_one,
	},
	{
		.procname	= "sched_burst_inherit_type",
		.data		= &sched_burst_inherit_type,
		.maxlen		= sizeof(u32),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &bore_two,
	},
	{
		.procname	= "sched_burst_smoothness",
		.data		= &sched_burst_smoothness,
		.maxlen		= sizeof(u32),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &bore_three,
	},
	{
		.procname	= "sched_burst_penalty_offset",
		.data		= &sched_burst_penalty_offset,
		.maxlen		= sizeof(u32),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &bore_sixfour,
	},
	{
		.procname	= "sched_burst_penalty_scale",
		.data		= &sched_burst_penalty_scale,
		.maxlen		= sizeof(u32),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &bore_zero,
		.extra2		= &bore_12bit,
	},
	{
		.procname	= "sched_burst_cache_lifetime",
		.data		= &sched_burst_cache_lifetime,
		.maxlen		= sizeof(u32),
		.mode		= 0644,
		.proc_handler	= proc_dointvec,
	},
	{ }
};

static int __init sched_bore_sysctl_init(void)
{
	register_sysctl("kernel", sched_bore_table);
	return 0;
}
late_initcall(sched_bore_sysctl_init);
#endif

#endif /* CONFIG_SCHED_BORE */
