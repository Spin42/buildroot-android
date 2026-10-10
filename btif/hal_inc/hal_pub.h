#ifndef __HAL_PUB_H_
#define __HAL_PUB_H_

#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/kfifo.h>
#include <linux/vmalloc.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/timekeeping.h>

#include "btif_compat.h"

extern int mtk_btif_hal_get_log_lvl(void);

#define MTK_BTIF_MARK_UNUSED_API

typedef irq_handler_t mtk_btif_irq_handler;

#define MTK_BTIF_ENABLE_CLK_CTL 1
#define MTK_BTIF_ENABLE_CLK_REF_COUNTER 1

#define DBG_LOG_STR_SIZE 512

/*Log defination*/
static inline __printf(1, 2) void hal_log_print(const char *str, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, str);
	vaf.fmt = str;
	vaf.va = &args;
	printk(KERN_ERR "%pV", &vaf);
	va_end(args);
}

#define BTIF_LOG_LOUD    4
#define BTIF_LOG_DBG     3
#define BTIF_LOG_INFO    2
#define BTIF_LOG_WARN    1
#define BTIF_LOG_ERR     0

#ifndef DFT_TAG
#define DFT_TAG         "[BTIF-DFT]"
#endif

#define BTIF_LOUD_FUNC(fmt, arg ...) \
do { \
	if (mtk_btif_hal_get_log_lvl() >= BTIF_LOG_LOUD) \
		hal_log_print(DFT_TAG "[L]%s:"  fmt, \
		__func__, ## arg); \
} while (0)

#define BTIF_INFO_FUNC(fmt, arg ...) \
do { \
	if (mtk_btif_hal_get_log_lvl() >= BTIF_LOG_INFO)\
		hal_log_print(DFT_TAG "[I]%s:"  fmt, \
		__func__, ## arg); \
} while (0)

#define BTIF_WARN_FUNC(fmt, arg ...) \
do { \
	if (mtk_btif_hal_get_log_lvl() >= BTIF_LOG_WARN)\
		hal_log_print(DFT_TAG "[W]%s:"  fmt, \
		__func__, ## arg); \
} while (0)

#define BTIF_ERR_FUNC(fmt, arg ...)\
do {\
	if (mtk_btif_hal_get_log_lvl() >= BTIF_LOG_ERR)\
		hal_log_print(DFT_TAG "[E]%s(%d):"  fmt,\
		__func__, __LINE__, ## arg);\
} while (0)

#define BTIF_DBG_FUNC(fmt, arg ...) \
do { \
	if (mtk_btif_hal_get_log_lvl() >= BTIF_LOG_DBG) \
		hal_log_print(DFT_TAG "[D]%s:"  fmt, \
		__func__, ## arg); \
} while (0)

#define BTIF_TRC_FUNC(f) \
do { \
	if (mtk_btif_hal_get_log_lvl() >= BTIF_LOG_DBG) \
		hal_log_print(DFT_TAG "<%s> <%d>\n", \
		__func__, __LINE__); \
} while (0)

/*-----------------------------------Enum Defination--------------------------------*/
typedef enum _ENUM_CLOCK_CTRL_ {
	CLK_OUT_DISABLE = 0,
	CLK_OUT_ENABLE = CLK_OUT_DISABLE + 1,
	CLK_OUT_MAX
} ENUM_CLOCK_CTRL;

/*Error No. table */
typedef enum _ENUM_ERROR_CODE_ {
	ERR_NO_ERROR = 0,
	ERR_INVALID_PAR = ERR_NO_ERROR - 1,
	ERR_MAX = ERR_INVALID_PAR - 1,
} ENUM_ERROR_CODE;

typedef enum _ENUM_BTIF_DIR_ {
	BTIF_TX = 0,
	BTIF_RX = BTIF_TX + 1,
	BTIF_DIR_MAX,
} ENUM_BTIF_DIR;

typedef enum _ENUM_DMA_DIR_ {
	DMA_DIR_RX = 0,
	DMA_DIR_TX = DMA_DIR_RX + 1,
	DMA_DIR_BOTH,
} ENUM_DMA_DIR;

typedef enum _ENUM_BTIF_REG_ID_ {
	REG_IIR = 0,		/*Interrupt Identification Register */
	REG_LSR = 1,		/*Line Status Register */
	REG_FAKE_LCR = 2,	/*Fake Lcr Regiseter */
	REG_FIFO_CTRL = 3,	/*FIFO Control Register */
	REG_IER = 4,		/*Interrupt Enable Register */
	REG_SLEEP_EN = 5,	/*Sleep Enable Register */
	REG_RTO_COUNTER = 6,	/*Rx Timeout Counter Register */
	REG_DMA_EN = 7,		/*DMA Enalbe Register */
	REG_TRIG_LVL = 8,	/*Tx/Rx Trigger Level Register */
	REG_WAT_TIME = 9,	/*Async Wait Time Register */
	REG_HANDSHAKE = 10,	/*New HandShake Mode Register */
	REG_SLP_WAK = 11,	/*Sleep Wakeup Reigster */
	REG_BTIF_ALL = 12,	/*all btif controller's registers */
	REG_TX_DMA_ALL = 13,
	REG_RX_DMA_ALL = 14,
	REG_MAX
} ENUM_BTIF_REG_ID;

typedef enum _MTK_BTIF_PM_OPID_ {
	BTIF_PM_DPIDLE_EN,
	BTIF_PM_DPIDLE_DIS,
	BTIF_PM_SUSPEND,
	BTIF_PM_RESUME,
	BTIF_PM_RESTORE_NOIRQ,
} MTK_BTIF_PM_OPID;

#define BTIF_HAL_TX_FIFO_SIZE (1024 * 4)

/*-----------------------------------Enum Defination End--------------------------------*/

/*****************************structure definition***************************/
/*IRQ related information*/
typedef struct _MTK_BTIF_IRQ_STR_ {
	const char *name;
	bool is_irq_sup;
	unsigned int irq_id;	/*Linux virtual IRQ number */
	unsigned int irq_flags;	/*IRQF_TRIGGER_* taken from the device tree */
	bool reg_flag;
	irq_handler_t p_irq_handler;
} MTK_BTIF_IRQ_STR, *P_MTK_BTIF_IRQ_STR;

typedef struct _DMA_VFIFO_ {
	/*[Driver Access] vFIFO memory'svirtual address */
	unsigned char *p_vir_addr;
	/*[HW Access] dma handle , physically address, set to DMA's HW Register */
	dma_addr_t phy_addr;
	/*DMA's vFIFO size */
	unsigned int vfifo_size;
	/*DMA's threshold value */
	unsigned int thre;
} DMA_VFIFO, *P_DMA_VFIFO;

typedef unsigned int (*dma_rx_buf_write) (void *p_dma_info,
					  unsigned char *p_buf,
					  unsigned int buf_len);
typedef unsigned int (*btif_rx_buf_write) (void *p_btif_info,
					   unsigned char *p_buf,
					   unsigned int buf_len);

/*DMA related information*/
typedef struct _MTK_DMA_INFO_STR_ {
	unsigned long base;	/*virtual address of the channel's register block */
	ENUM_DMA_DIR dir;
	P_MTK_BTIF_IRQ_STR p_irq;
	dma_rx_buf_write rx_cb;
	P_DMA_VFIFO p_vfifo;
} MTK_DMA_INFO_STR, *P_MTK_DMA_INFO_STR;

/*DMA related information*/
typedef struct _MTK_BTIF_INFO_STR_ {
	unsigned long base;	/*virtual address of the BTIF register block */
	P_MTK_BTIF_IRQ_STR p_irq;	/*irq related information */

	unsigned int tx_fifo_size;	/*BTIF tx FIFO size */
	unsigned int rx_fifo_size;	/*BTIF rx FIFO size */

	unsigned int tx_tri_lvl;	/*BTIFtx trigger level in FIFO mode */
	unsigned int rx_tri_lvl;	/*BTIFrx trigger level in FIFO mode */

	unsigned int rx_data_len;	/*rx data length */

	btif_rx_buf_write rx_cb;

	struct clk *clk;	/*PERICFG BTIF clock gate, may be NULL */

	struct kfifo *p_tx_fifo;	/*tx fifo */
	spinlock_t tx_fifo_spinlock;	/*tx fifo spinlock */
} MTK_BTIF_INFO_STR, *P_MTK_BTIF_INFO_STR;

/**********End of Structure Definition***********/

/***********register operation***********/
/* Registers are addressed as (virtual base + offset) held in an unsigned long. */
#define BTIF_IOMEM(a)  ((void __iomem *)(unsigned long)(a))

/*byte write: write, then wait for the write to be issued */
#define btif_reg_sync_writeb(v, a)    do { writeb((v), BTIF_IOMEM(a)); wmb(); } while (0)
/*long write: write, then wait for the write to be issued */
#define btif_reg_sync_writel(v, a)    do { writel((v), BTIF_IOMEM(a)); wmb(); } while (0)

#define BTIF_READ8(REG)               readb(BTIF_IOMEM(REG))
#define BTIF_READ32(REG)              readl(BTIF_IOMEM(REG))

/* read-modify-write, the register keeps the bits not named */
#define BTIF_SET_BIT(REG, BITVAL)    do { \
	writel(readl(BTIF_IOMEM(REG)) | ((unsigned int)(BITVAL)), BTIF_IOMEM(REG)); \
	wmb(); \
} while (0)
#define BTIF_CLR_BIT(REG, BITVAL)    do { \
	writel(readl(BTIF_IOMEM(REG)) & ~((unsigned int)(BITVAL)), BTIF_IOMEM(REG)); \
	wmb(); \
} while (0)

/***********end of register operation *********/

#endif /*__HAL_PUB_H_*/
