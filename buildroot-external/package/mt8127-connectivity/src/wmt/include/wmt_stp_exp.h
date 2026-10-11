/* Build configuration shared by the WMT/STP headers. */
#ifndef _WMT_STP_EXP_H_
#define _WMT_STP_EXP_H_

#include <linux/version.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/sched.h>
#include <linux/poll.h>
#include <linux/proc_fs.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/time.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/vmalloc.h>
#include <linux/firmware.h>
#include <linux/kthread.h>
#include <linux/jiffies.h>
#include <linux/slab.h>
#include <mach/mtk_wcn_cmb_stub.h>
#include "osal_typedef.h"

#ifndef MTK_WCN_CMB_FOR_SDIO_1V_AUTOK
#define MTK_WCN_CMB_FOR_SDIO_1V_AUTOK 0
#endif

/* The LTE/IDC coexistence code is not part of this build. */
#ifdef CFG_WMT_LTE_COEX_HANDLING
#undef CFG_WMT_LTE_COEX_HANDLING
#endif
#define CFG_WMT_LTE_COEX_HANDLING 0

#endif /* _WMT_STP_EXP_H_ */
