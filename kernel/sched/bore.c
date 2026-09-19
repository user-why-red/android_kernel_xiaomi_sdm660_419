// SPDX-License-Identifier: GPL-2.0
/*
 * Burst-Oriented Response Enhancer — CFS vruntime scoring.
 * Scoring from firelzrd BORE 6.8; applied as vruntime only on 4.19 CFS.
 */
#include "sched.h"

#ifdef CONFIG_SCHED_BORE

u32 __read_mostly sched_bore = 1;
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

	if (!sched_bore)
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

	if (!sched_bore)
		return;

	smoothed = binary_smooth(p->bore.curr_penalty, p->bore.prev_penalty);
	p->bore.prev_penalty = smoothed;
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
	pr_info("BORE: CFS burst scoring\n");
}

#ifdef CONFIG_SYSCTL
static int bore_zero;
static int bore_one = 1;
static int bore_two = 2;
static int bore_three = 3;
static int bore_sixfour = 64;
static int bore_12bit = 4095;

static struct ctl_table sched_bore_table[] = {
	{
		.procname	= "sched_bore",
		.data		= &sched_bore,
		.maxlen		= sizeof(u32),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
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
