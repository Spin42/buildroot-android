/* SPDX-License-Identifier: GPL-2.0 */
/* Staged capture prototype for the MT8127 camera pipeline: shared state and entry points. */
#ifndef CAM_PROTO_H
#define CAM_PROTO_H

#include <linux/atomic.h>
#include <linux/bitfield.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/regulator/consumer.h>

#include "cam_regs.h"

#define CAM_STATUS_BUF		8192

enum cam_win {
	CAM_WIN_IMGSYS,
	CAM_WIN_ISP,
	CAM_WIN_SENINF,
	CAM_WIN_MIPI_RX,
	CAM_WIN_MIPI_ANA,
	CAM_WIN_SPM,
	CAM_WIN_TOPCKGEN,
	CAM_WIN_GPIO,
	CAM_WIN_EMI,		/* optional */
	CAM_NUM_WIN
};

struct cam_regval {
	u8 reg;
	u8 val;
};

enum cam_slot {
	CAM_SLOT_MAIN,
	CAM_SLOT_SUB,
};

/* Static description of a fitted sensor; the register tables live in cam_sensor.c. */
struct cam_sensor {
	const char *name;
	enum cam_slot slot;
	u8 i2c_addr;
	bool yuv;
	u8 lanes;		/* data lanes the sensor drives */
	u16 width, height;	/* output size in pixels */
	u16 grab_x, grab_y;	/* TG grab start */
	u8 tg_pad, tg_src, tg_fmt;
	u8 pixid;		/* Bayer start colour for the ISP PIX_ID */
	u8 vf_delay;
	u8 ecc_order;
	u8 vsync_pol;
	u8 sync_a, sync_b;	/* setTg1SensorModeCfg arguments */
};

/* Values the user may change while the module is loaded (module parameters). */
struct cam_knobs {
	int irq_mode;		/* 1: frame done through the IRQ, 0: poll INT_STATUS */
	int port;		/* -1: main = CSI0, sub = CSI1; 0 / 1 force the port */
	int lanes;		/* 0: sensor default, 1..4 force the lane count */
	int csi2_ctrl;		/* -1: computed, otherwise written as is */
	int csi2_settle;
	int csi2_ecc_order;	/* -1: sensor default */
	int csi2_vsync;		/* -1: sensor default */
	int seninf_inten;
	int tg_pad, tg_src, tg_fmt;	/* -1: sensor default */
	int sync_a, sync_b;	/* -1: sensor default */
	int grab_x, grab_y;	/* -1: sensor default */
	int width, height;	/* 0: sensor default */
	int pixid;		/* -1: sensor default */
	int set_clr_mode;	/* 1: SET/CLR helper registers, 0: read-modify-write the base registers */
	int int_en;
	int dma_int_bits;
	int imgo_con, imgo_con2;
	int imgo_pb;		/* 0: auto (5 for RAW10, 8 for YUV422) */
	int imgo_stride_px;	/* 0: tight */
	int imgo_base_mode;	/* bit0: before VF start, bit1: rewrite at SOF1, bit2: rewrite at PASS1_DON */
	int fbc_mode;		/* 0: FBC off, 1: vendor init with one buffer */
	int vf_single;		/* 1: single-shot TG, 0: continuous */
	int vf_delay;		/* -1: sensor default */
	int done_mask;		/* INT_STATUS bits that finish a capture */
	int capture_timeout_ms;
	int frame_fill;		/* byte pattern the buffer is filled with before a capture */
	int mclk_pinmux;
	int io_drive;		/* GPIO_DRV_117_120 field value, -1 to leave it */
	int isp_domain_keep;	/* 1: leave the ISP power domain on at stage 0 */
	int test_pattern;	/* SP2508 colour bar */
	int stream_wait_ms;	/* delay between sensor start and status read in stage 2 */
	int run_while_error;	/* 1: continue stage 2 even if the PHY calibration reports not ready */
};

extern struct cam_knobs cam_knobs;

struct cam_proto {
	struct device *dev;
	struct mutex lock;

	void __iomem *base[CAM_NUM_WIN];
	resource_size_t size[CAM_NUM_WIN];
	int irq;

	struct regulator *vcama, *vcamio;
	struct gpio_desc *main_rst, *main_pdn, *sub_rst, *sub_pdn;
	struct i2c_adapter *adap;

	const struct cam_sensor *sensor;
	int stage;

	/* hardware state, used to unwind exactly what was done */
	bool domain_ours, clk_on, mclk_on, vcama_on, vcamio_on;
	bool seninf_on, isp_cfg, irq_req;
	u8 port;

	/* capture */
	struct completion frame_done;
	atomic_t int_acc, dma_acc;
	atomic_t irq_count;
	void *frame_cpu;
	dma_addr_t frame_dma;
	size_t frame_alloc;
	size_t frame_size;
	u32 frame_w, frame_h, frame_stride;
	const char *frame_fmt;
	bool frame_valid;
	int capture_ret;
	char frame_stats[160];

	struct dentry *dbg;
	char *snap;
	char reg_result[96];
};

static inline u32 cam_rd(struct cam_proto *c, enum cam_win w, u32 off)
{
	return readl(c->base[w] + off);
}

static inline void cam_wr(struct cam_proto *c, enum cam_win w, u32 off, u32 val)
{
	writel(val, c->base[w] + off);
}

static inline void cam_rmw(struct cam_proto *c, enum cam_win w, u32 off, u32 clr, u32 set)
{
	u32 v = cam_rd(c, w, off);

	cam_wr(c, w, off, (v & ~clr) | set);
}

/* cam_power.c */
int cam_power_on(struct cam_proto *c);
void cam_power_off(struct cam_proto *c);
void cam_mclk(struct cam_proto *c, bool on);
void cam_gpio_set(struct gpio_desc *d, int val);

/* cam_sensor.c */
extern const struct cam_sensor cam_sensor_sp2508, cam_sensor_sp0a20;
const struct cam_sensor *cam_sensor_by_name(const char *name);
int cam_sensor_identify(struct cam_proto *c);
int cam_sensor_program(struct cam_proto *c);

/* cam_seninf.c */
int cam_seninf_phy_on(struct cam_proto *c);
int cam_seninf_route(struct cam_proto *c);
void cam_seninf_report(struct cam_proto *c);
void cam_seninf_off(struct cam_proto *c);
int cam_seninf_lanes(const struct cam_proto *c);
void cam_seninf_dump(struct cam_proto *c, char *buf, size_t sz, size_t *n);

/* cam_isp.c */
int cam_isp_configure(struct cam_proto *c);
int cam_isp_capture(struct cam_proto *c);
void cam_isp_quiesce(struct cam_proto *c);
void cam_isp_free_frame(struct cam_proto *c);
int cam_isp_prepare_frame(struct cam_proto *c);
void cam_isp_dump(struct cam_proto *c, char *buf, size_t sz, size_t *n);
int cam_isp_path_write(struct cam_proto *c, const char *name, bool set, u32 val);
void cam_isp_path_show(struct cam_proto *c, char *buf, size_t sz, size_t *n);
irqreturn_t cam_isp_irq(int irq, void *data);
void cam_geometry(const struct cam_proto *c, u32 *w, u32 *h);

/* parameter-or-default helper: knob < 0 means "use the sensor value" */
#define CAM_KNOB(k, def)	((k) < 0 ? (def) : (k))

#define SNPR(buf, sz, n, ...)	do { if ((n) < (sz)) (n) += scnprintf((buf) + (n), (sz) - (n), __VA_ARGS__); } while (0)

#endif
