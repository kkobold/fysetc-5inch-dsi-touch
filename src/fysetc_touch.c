// SPDX-License-Identifier: GPL-2.0
/*
 * fysetc_touch - touch driver for the FYSETC Raspberry Pi 5 inch DSI Touch
 * Screen MIPI LCD V1.1 (I2C 0x38).
 *
 * Based on drivers/input/touchscreen/raspberrypi-ts.c (raspberrypi/linux),
 * changed by kkobold on 2026-10-05: the firmware buffer was replaced by I2C
 * reads at 0x38 that follow the FYSETC MCU's read ring (below). The input
 * setup, polling and point decoding follow raspberrypi-ts.c. Changed on
 * 2026-10-21: contacts are released when polls keep failing.
 *
 * The screen emulates the official 7" display: an MCU answers at 0x38 with
 * the FT5x06 register map, but multi-byte reads do not follow the standard:
 *
 *   - the register selects a record: 0x02 TD_STATUS, 0x03 + 6*i point i;
 *   - the first byte read is always the record's first field (TD_STATUS/XH);
 *   - the following bytes come from a 3-entry ring (XL, YH, YL) whose read
 *     pointer is GLOBAL, advances by (len - 1) per read and is never reset
 *     (not by STOP, not by a register change).
 *
 * So only 1-byte (TD_STATUS) and 4-byte (point) reads are issued:
 * 4 - 1 = 3 ≡ 0 (mod 3), which keeps the ring phase constant, so it only
 * has to be discovered, not tracked. The phase is validated on every frame
 * through YH (ID < 5, Y[11:8] in range) and corrected when exactly one
 * other phase is consistent.
 *
 * Native orientation matches the official display: both axes are inverted,
 * which the vc4-kms-dsi-7inch overlay already declares by default through
 * touchscreen-inverted-x/y. All transforms come from the DT properties.
 *
 * Copyright (C) 2015, 2017 Raspberry Pi
 * Copyright (C) 2018 Nicolas Saenz Julienne <nsaenzjulienne@suse.de>
 * Copyright (C) 2026 kkobold
 */
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/input/touchscreen.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/version.h>

#define FT_REG_TD_STATUS	0x02
#define FT_REG_P1		0x03
#define FT_POINT_STRIDE		6
#define FT_MAX_POINTS		5
#define FT_RING			3

#define FT_NATIVE_X		800
#define FT_NATIVE_Y		480
#define FT_POLL_MS		16
/*
 * Consecutive failed polls (read error or ambiguous frame, ~48 ms) after which
 * every contact is released.
 */
#define FT_MAX_FAILED_POLLS	3

struct fysetc_ts {
	struct i2c_client *client;
	struct input_dev *input;
	struct touchscreen_properties prop;
	unsigned int phase;
	unsigned int failed_polls;
};

struct ft_point {
	u8 xh, xl, yh, yl;
};

static int ft_read(struct fysetc_ts *ts, u8 reg, u8 *buf, u16 len)
{
	struct i2c_client *c = ts->client;
	struct i2c_msg msgs[2] = {
		{ .addr = c->addr, .flags = 0, .len = 1, .buf = &reg },
		{ .addr = c->addr, .flags = I2C_M_RD, .len = len, .buf = buf },
	};
	int ret;

	ret = i2c_transfer(c->adapter, msgs, 2);
	if (ret == 2)
		return 0;
	return ret < 0 ? ret : -EIO;
}

static void ft_decode(const u8 raw[4], unsigned int phase, struct ft_point *p)
{
	p->xh = raw[0];
	p->xl = raw[1 + phase % FT_RING];
	p->yh = raw[1 + (phase + 1) % FT_RING];
	p->yl = raw[1 + (phase + 2) % FT_RING];
}

static bool ft_point_valid(const struct ft_point *p)
{
	unsigned int x = ((p->xh & 0x0f) << 8) | p->xl;
	unsigned int y = ((p->yh & 0x0f) << 8) | p->yl;

	return (p->yh >> 4) < FT_MAX_POINTS && x < FT_NATIVE_X && y < FT_NATIVE_Y;
}

/* Phase consistent with every point read; -1 if none or more than one. */
static int ft_find_phase(u8 raw[][4], int n)
{
	struct ft_point p;
	int phase, i, found = -1;

	for (phase = 0; phase < FT_RING; phase++) {
		for (i = 0; i < n; i++) {
			ft_decode(raw[i], phase, &p);
			if (!ft_point_valid(&p))
				break;
		}
		if (i < n)
			continue;
		if (found >= 0)
			return -1;
		found = phase;
	}
	return found;
}

/*
 * A failed poll keeps the previous contacts; if polls keep failing, they are
 * released once (INPUT_MT_DROP_UNUSED drops every slot not reported in a frame).
 */
static void fysetc_ts_poll_failed(struct fysetc_ts *ts)
{
	if (ts->failed_polls >= FT_MAX_FAILED_POLLS)
		return;
	if (++ts->failed_polls < FT_MAX_FAILED_POLLS)
		return;

	input_mt_sync_frame(ts->input);
	input_sync(ts->input);
}

static void fysetc_ts_poll(struct input_dev *input)
{
	struct fysetc_ts *ts = input_get_drvdata(input);
	u8 raw[FT_MAX_POINTS][4];
	struct ft_point p;
	u8 status;
	int n, i, phase, error;

	error = ft_read(ts, FT_REG_TD_STATUS, &status, 1);
	if (error) {
		dev_err_ratelimited(&ts->client->dev, "TD_STATUS read: %d\n", error);
		fysetc_ts_poll_failed(ts);
		return;
	}

	n = min_t(int, status & 0x0f, FT_MAX_POINTS);

	for (i = 0; i < n; i++) {
		error = ft_read(ts, FT_REG_P1 + i * FT_POINT_STRIDE, raw[i], 4);
		if (error) {
			dev_err_ratelimited(&ts->client->dev, "point %d read: %d\n", i, error);
			fysetc_ts_poll_failed(ts);
			return;
		}
	}

	for (i = 0; i < n; i++) {
		ft_decode(raw[i], ts->phase, &p);
		if (!ft_point_valid(&p))
			break;
	}
	if (i < n) {
		phase = ft_find_phase(raw, n);
		if (phase < 0) {
			/* Ambiguous frame: drop it and keep the previous state. */
			fysetc_ts_poll_failed(ts);
			return;
		}
		dev_info_ratelimited(&ts->client->dev, "ring phase %u -> %d\n", ts->phase, phase);
		ts->phase = phase;
	}

	for (i = 0; i < n; i++) {
		unsigned int x, y;
		int slot;

		ft_decode(raw[i], ts->phase, &p);
		x = ((p.xh & 0x0f) << 8) | p.xl;
		y = ((p.yh & 0x0f) << 8) | p.yl;
		slot = p.yh >> 4;

		input_mt_slot(input, slot);
		input_mt_report_slot_state(input, MT_TOOL_FINGER, true);
		touchscreen_report_pos(input, &ts->prop, x, y, true);
	}

	input_mt_sync_frame(input);
	input_sync(input);
	ts->failed_polls = 0;
}

/* Initial phase from the records cached by the proxy (best effort). */
static void ft_initial_phase(struct fysetc_ts *ts)
{
	u8 raw[FT_MAX_POINTS][4];
	int i, n = 0, phase;

	for (i = 0; i < FT_MAX_POINTS; i++) {
		if (ft_read(ts, FT_REG_P1 + i * FT_POINT_STRIDE, raw[n], 4))
			continue;
		/* An empty record (XH 0x80, XL 0xff) tells nothing. */
		if (raw[n][1] == 0xff || raw[n][2] == 0xff || raw[n][3] == 0xff)
			continue;
		n++;
	}

	phase = n ? ft_find_phase(raw, n) : -1;
	ts->phase = phase < 0 ? 0 : phase;
	dev_info(&ts->client->dev, "initial ring phase: %u (%s)\n", ts->phase,
		 phase < 0 ? "default" : "detected");
}

static int fysetc_ts_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fysetc_ts *ts;
	struct input_dev *input;
	u8 status;
	int error;

	ts = devm_kzalloc(dev, sizeof(*ts), GFP_KERNEL);
	if (!ts)
		return -ENOMEM;
	ts->client = client;

	/* Short read: checks presence without moving the ring. */
	error = ft_read(ts, FT_REG_TD_STATUS, &status, 1);
	if (error)
		return dev_err_probe(dev, error, "no answer at 0x%02x\n", client->addr);

	input = devm_input_allocate_device(dev);
	if (!input)
		return -ENOMEM;
	ts->input = input;

	input->name = "FYSETC 5inch DSI Touch";
	input->id.bustype = BUS_I2C;
	input_set_drvdata(input, ts);

	input_set_abs_params(input, ABS_MT_POSITION_X, 0, FT_NATIVE_X - 1, 0, 0);
	input_set_abs_params(input, ABS_MT_POSITION_Y, 0, FT_NATIVE_Y - 1, 0, 0);
	touchscreen_parse_properties(input, true, &ts->prop);

	error = input_mt_init_slots(input, FT_MAX_POINTS,
				    INPUT_MT_DIRECT | INPUT_MT_DROP_UNUSED);
	if (error)
		return error;

	ft_initial_phase(ts);

	error = input_setup_polling(input, fysetc_ts_poll);
	if (error)
		return error;
	input_set_poll_interval(input, FT_POLL_MS);

	error = input_register_device(input);
	if (error)
		return error;

	i2c_set_clientdata(client, ts);
	return 0;
}

static const struct of_device_id fysetc_ts_of_match[] = {
	/* ts@38 node created by vc4-kms-dsi-7inch; edt_ft5x06 is blacklisted. */
	{ .compatible = "edt,edt-ft5506" },
	{ .compatible = "fysetc,5inch-dsi-touch" },
	{ }
};
MODULE_DEVICE_TABLE(of, fysetc_ts_of_match);

static struct i2c_driver fysetc_ts_driver = {
	.driver = {
		.name = "fysetc_touch",
		.of_match_table = fysetc_ts_of_match,
	},
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	.probe_new = fysetc_ts_probe,
#else
	.probe = fysetc_ts_probe,
#endif
};
module_i2c_driver(fysetc_ts_driver);

MODULE_AUTHOR("Gordon Hollingworth");
MODULE_AUTHOR("Nicolas Saenz Julienne <nsaenzjulienne@suse.de>");
MODULE_AUTHOR("kkobold");
MODULE_DESCRIPTION("FYSETC Raspberry Pi 5 inch DSI Touch Screen V1.1 touch driver");
MODULE_LICENSE("GPL v2");
