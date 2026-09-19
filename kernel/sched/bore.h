/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _KERNEL_SCHED_BORE_H
#define _KERNEL_SCHED_BORE_H

#ifdef CONFIG_SCHED_BORE
void sched_init_bore(void);
void reset_task_bore(struct task_struct *p);

/* Filled in by later fair.c hooks; no-ops until then. */
static inline void update_curr_bore(struct task_struct *p, u64 delta_exec) { }
static inline void restart_burst_bore(struct task_struct *p) { }
#else
static inline void sched_init_bore(void) { }
static inline void reset_task_bore(struct task_struct *p) { }
static inline void update_curr_bore(struct task_struct *p, u64 delta_exec) { }
static inline void restart_burst_bore(struct task_struct *p) { }
#endif

#endif
