// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2024 Sultan Alsawaf <sultan@kerneltoast.com>.
 */

#ifdef CONFIG_SCHED_CASS
/*
 * CASS is CFS-only. RT wakeup stays on find_lowest_rq / cpupri so
 * RT_SOFTINT_OPTIMIZATION and rt_task_fits_capacity still run.
 */
#endif /* CONFIG_SCHED_CASS */
