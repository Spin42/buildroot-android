/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __BTIF_COMPAT_H_
#define __BTIF_COMPAT_H_

#include <linux/timekeeping.h>
#include <linux/ktime.h>

/* Wall-clock timestamp used to tag logged BTIF packets and IRQ storms. */
typedef struct timespec64 btif_timestamp;

static inline void btif_timestamp_now(btif_timestamp *ts)
{
	ktime_get_real_ts64(ts);
}

#define btif_timestamp_usec(ts) ((int)((ts)->tv_nsec / NSEC_PER_USEC))

#endif /* __BTIF_COMPAT_H_ */
