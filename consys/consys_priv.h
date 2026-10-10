/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _CONSYS_PRIV_H_
#define _CONSYS_PRIV_H_

#include <linux/types.h>

/* Provided by consys_hw.c to wmt_plat.c. */
u32 consys_hw_read_cpupcr(void);
int consys_hw_pulse_ap2conn_wakeup(void);
int consys_hw_bgf_irq(void);

#endif
