/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_SCHED_BORE_H
#define _KERNEL_SCHED_BORE_H

#ifdef CONFIG_SCHED_BORE
extern u32 sched_bore;
extern u32 sched_burst_smoothness;
extern u32 sched_burst_penalty_offset;
extern u32 sched_burst_penalty_scale;

void sched_init_bore(void);
void reset_task_bore(struct task_struct *p);
void update_curr_bore(struct task_struct *p, u64 delta_exec);
void restart_burst_bore(struct task_struct *p);

static inline u8 bore_score(struct task_struct *p)
{
	return p->bore.penalty >> 8;
}
#else
extern u32 sched_bore;
extern u32 sched_burst_smoothness;
extern u32 sched_burst_penalty_offset;
extern u32 sched_burst_penalty_scale;
static inline void sched_init_bore(void) { }
static inline void reset_task_bore(struct task_struct *p) { }
static inline void update_curr_bore(struct task_struct *p, u64 delta_exec) { }
static inline void restart_burst_bore(struct task_struct *p) { }
static inline u8 bore_score(struct task_struct *p) { return 0; }
#endif

#endif
