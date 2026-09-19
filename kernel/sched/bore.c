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
