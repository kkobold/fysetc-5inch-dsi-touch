// SPDX-License-Identifier: GPL-2.0
/*
 * fysetc_sleep - panel controller driver (I2C 0x45) for the FYSETC Raspberry
 * Pi 5 inch DSI Touch Screen MIPI LCD V1.1.
 *
 * Modified copy of drivers/regulator/rpi-panel-attiny-regulator.c
 * (raspberrypi/linux rpi-6.12.y), changed by kkobold on 2026-10-21: renamed
 * to fysetc_sleep, the backlight made on/off, every Atmel write retried
 * (except on timeout), errors reported by enable and GPIO operations and
 * logged by disable, the cached port state updated on success (always to off
 * on disable), unused definitions removed, and builds on Linux 6.1 to 6.17+.
 *
 * The FYSETC MCU only acts on REG_PWM 0x00 (off) and 0xff (on). Brightness
 * keeps the 0..255 range: below FYSETC_BL_ON_PERCENT of the maximum is off,
 * anything else is on.
 *
 * Copyright (C) 2020 Marek Vasut <marex@denx.de>
 * Copyright (C) 2026 kkobold
 *
 * Based on rpi_touchscreen.c by Eric Anholt <eric@anholt.net>
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/gpio/driver.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>
#include <linux/version.h>

/* I2C registers of the Atmel microcontroller. */
#define REG_PORTA	0x81
#define REG_PORTB	0x82
#define REG_PORTC	0x83
#define REG_POWERON	0x85
#define REG_PWM		0x86
#define REG_ADDR_L	0x8c
#define REG_ADDR_H	0x8d
#define REG_WRITE_DATA_H	0x90
#define REG_WRITE_DATA_L	0x91

#define PA_LCD_LR		BIT(2)

#define PB_LCD_VCC_N		BIT(1)
#define PB_LCD_MAIN		BIT(7)

#define PC_LED_EN		BIT(0)
#define PC_RST_TP_N		BIT(1)
#define PC_RST_LCD_N		BIT(2)
#define PC_RST_BRIDGE_N		BIT(3)

/* REG_PWM values the FYSETC MCU acts on. */
#define FYSETC_PWM_OFF		0x00
#define FYSETC_PWM_ON		0xff

/* Brightness scale seen by user space, independent of the REG_PWM values. */
#define FYSETC_BL_MAX		255
#define FYSETC_BL_ON_PERCENT	30

/* The Atmel's I2C is fragile: every write gets this many attempts. */
#define ATTINY_WRITE_TRIES	10

enum gpio_signals {
	RST_BRIDGE_N,	/* TC358762 bridge reset */
	RST_TP_N,	/* Touch controller reset */
	NUM_GPIO
};

struct gpio_signal_mappings {
	unsigned int reg;
	unsigned int mask;
};

static const struct gpio_signal_mappings mappings[NUM_GPIO] = {
	[RST_BRIDGE_N] = { REG_PORTC, PC_RST_BRIDGE_N | PC_RST_LCD_N  },
	[RST_TP_N] = { REG_PORTC, PC_RST_TP_N },
};

struct attiny_lcd {
	/* lock to serialise overall accesses to the Atmel */
	struct mutex	lock;
	struct regmap	*regmap;
	u8 port_states[3];

	struct gpio_chip gc;
};

static const struct regmap_config attiny_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.disable_locking = 1,
	.max_register = REG_WRITE_DATA_L,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
	.cache_type = REGCACHE_RBTREE,
#else
	.cache_type = REGCACHE_MAPLE,
#endif
};

static int attiny_write(struct regmap *regmap, unsigned int reg, unsigned int val)
{
	int ret, i;

	/* A timeout means a hung bus: retrying would only hold the lock longer. */
	for (i = 0; i < ATTINY_WRITE_TRIES; i++) {
		ret = regmap_write(regmap, reg, val);
		if (!ret || ret == -ETIMEDOUT)
			break;
	}
	return ret;
}

/* The cached port state only changes when the write reached the Atmel (see disable). */
static int attiny_set_port_state(struct attiny_lcd *state, int reg, u8 val)
{
	int ret = attiny_write(state->regmap, reg, val);

	if (!ret)
		state->port_states[reg - REG_PORTA] = val;
	return ret;
}

static u8 attiny_get_port_state(struct attiny_lcd *state, int reg)
{
	return state->port_states[reg - REG_PORTA];
}

/* Stops at the first failed write: the panel is not powered half-configured. */
static int attiny_lcd_power_enable(struct regulator_dev *rdev)
{
	struct attiny_lcd *state = rdev_get_drvdata(rdev);
	int ret;

	mutex_lock(&state->lock);

	/* Ensure bridge, and tp stay in reset */
	ret = attiny_set_port_state(state, REG_PORTC, 0);
	if (ret)
		goto out;
	usleep_range(5000, 10000);

	/* Default to the same orientation as the closed source
	 * firmware used for the panel.  Runtime rotation
	 * configuration will be supported using VC4's plane
	 * orientation bits.
	 */
	ret = attiny_set_port_state(state, REG_PORTA, PA_LCD_LR);
	if (ret)
		goto out;
	usleep_range(5000, 10000);
	/* Main regulator on, and power to the panel (LCD_VCC_N) */
	ret = attiny_set_port_state(state, REG_PORTB, PB_LCD_MAIN);
	if (ret)
		goto out;
	usleep_range(5000, 10000);
	/* Bring controllers out of reset */
	ret = attiny_set_port_state(state, REG_PORTC, PC_LED_EN);
	if (ret)
		goto out;

	msleep(80);
out:
	mutex_unlock(&state->lock);

	return ret;
}

/*
 * Runs every step even after a failed write, records the off state and only
 * logs the first error. The regulator core keeps its use count when disable
 * fails, and skips enable while is_enabled() reports on: either way the next
 * enable would not run and the panel would stay off until reboot.
 */
static int attiny_lcd_power_disable(struct regulator_dev *rdev)
{
	static const u8 ports_off[][2] = {
		{ REG_PORTA, 0 },
		{ REG_PORTB, PB_LCD_VCC_N },
		{ REG_PORTC, 0 },
	};
	struct attiny_lcd *state = rdev_get_drvdata(rdev);
	unsigned int i;
	int ret, err;

	mutex_lock(&state->lock);

	ret = attiny_write(state->regmap, REG_PWM, FYSETC_PWM_OFF);
	for (i = 0; i < ARRAY_SIZE(ports_off); i++) {
		usleep_range(5000, 10000);
		err = attiny_write(state->regmap, ports_off[i][0], ports_off[i][1]);
		ret = ret ?: err;
		state->port_states[ports_off[i][0] - REG_PORTA] = ports_off[i][1];
	}
	msleep(30);

	mutex_unlock(&state->lock);

	if (ret)
		dev_err(rdev_get_dev(rdev), "panel power off incomplete: %d\n", ret);
	return 0;
}

static int attiny_lcd_power_is_enabled(struct regulator_dev *rdev)
{
	struct attiny_lcd *state = rdev_get_drvdata(rdev);

	return state->port_states[REG_PORTC - REG_PORTA] & PC_RST_BRIDGE_N;
}

static const struct regulator_init_data attiny_regulator_default = {
	.constraints = {
		.valid_ops_mask = REGULATOR_CHANGE_STATUS,
	},
};

static const struct regulator_ops attiny_regulator_ops = {
	.enable = attiny_lcd_power_enable,
	.disable = attiny_lcd_power_disable,
	.is_enabled = attiny_lcd_power_is_enabled,
};

static const struct regulator_desc attiny_regulator = {
	.name	= "tc358762-power",
	.ops	= &attiny_regulator_ops,
	.type	= REGULATOR_VOLTAGE,
	.owner	= THIS_MODULE,
};

/* Brightness from FYSETC_BL_ON_PERCENT of the maximum up is "on". */
static unsigned int fysetc_pwm(struct backlight_device *bl)
{
	int on_from = DIV_ROUND_UP(bl->props.max_brightness * FYSETC_BL_ON_PERCENT, 100);

	return backlight_get_brightness(bl) >= on_from ? FYSETC_PWM_ON : FYSETC_PWM_OFF;
}

static int attiny_update_status(struct backlight_device *bl)
{
	struct attiny_lcd *state = bl_get_data(bl);
	int ret;

	mutex_lock(&state->lock);
	ret = attiny_write(state->regmap, REG_PWM, fysetc_pwm(bl));
	mutex_unlock(&state->lock);

	return ret;
}

static const struct backlight_ops attiny_bl = {
	.update_status	= attiny_update_status,
};

static int attiny_gpio_get_direction(struct gpio_chip *gc, unsigned int off)
{
	return GPIO_LINE_DIRECTION_OUT;
}

/* Bridge register write through the Atmel, after the bridge leaves reset. */
static int attiny_bridge_init(struct attiny_lcd *state)
{
	static const u8 seq[][2] = {
		{ REG_ADDR_H, 0x04 },
		{ REG_ADDR_L, 0x7c },
		{ REG_WRITE_DATA_H, 0x00 },
		{ REG_WRITE_DATA_L, 0x00 },
	};
	unsigned int i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(seq); i++) {
		usleep_range(5000, 8000);
		ret = attiny_write(state->regmap, seq[i][0], seq[i][1]);
		if (ret)
			return ret;
	}

	msleep(100);
	return 0;
}

static int attiny_gpio_set(struct gpio_chip *gc, unsigned int off, int val)
{
	struct attiny_lcd *state = gpiochip_get_data(gc);
	u8 last_val;
	int ret;

	if (off >= NUM_GPIO)
		return -EINVAL;

	mutex_lock(&state->lock);

	last_val = attiny_get_port_state(state, mappings[off].reg);
	if (val)
		last_val |= mappings[off].mask;
	else
		last_val &= ~mappings[off].mask;

	ret = attiny_set_port_state(state, mappings[off].reg, last_val);
	if (!ret && off == RST_BRIDGE_N && val)
		ret = attiny_bridge_init(state);

	mutex_unlock(&state->lock);

	return ret;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 17, 0)
/* gpio_chip::set returns void before Linux 6.17. */
static void attiny_gpio_set_void(struct gpio_chip *gc, unsigned int off, int val)
{
	attiny_gpio_set(gc, off, val);
}
#endif

/*
 * I2C driver interface functions
 */
static int attiny_i2c_probe(struct i2c_client *i2c)
{
	struct backlight_properties props = { };
	struct regulator_config config = { };
	struct backlight_device *bl;
	struct regulator_dev *rdev;
	struct attiny_lcd *state;
	struct regmap *regmap;
	int ret;

	state = devm_kzalloc(&i2c->dev, sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	mutex_init(&state->lock);
	i2c_set_clientdata(i2c, state);

	regmap = devm_regmap_init_i2c(i2c, &attiny_regmap_config);
	if (IS_ERR(regmap)) {
		ret = PTR_ERR(regmap);
		dev_err(&i2c->dev, "Failed to allocate register map: %d\n",
			ret);
		goto error;
	}

	/* Best effort: failing probe would leave the panel without its regulator. */
	attiny_write(regmap, REG_POWERON, 0);
	msleep(30);
	attiny_write(regmap, REG_PWM, FYSETC_PWM_OFF);

	config.dev = &i2c->dev;
	config.regmap = regmap;
	config.of_node = i2c->dev.of_node;
	config.init_data = &attiny_regulator_default;
	config.driver_data = state;

	rdev = devm_regulator_register(&i2c->dev, &attiny_regulator, &config);
	if (IS_ERR(rdev)) {
		dev_err(&i2c->dev, "Failed to register ATTINY regulator\n");
		ret = PTR_ERR(rdev);
		goto error;
	}

	props.type = BACKLIGHT_RAW;
	props.max_brightness = FYSETC_BL_MAX;

	state->regmap = regmap;

	bl = devm_backlight_device_register(&i2c->dev, dev_name(&i2c->dev),
					    &i2c->dev, state, &attiny_bl,
					    &props);
	if (IS_ERR(bl)) {
		ret = PTR_ERR(bl);
		goto error;
	}

	bl->props.brightness = FYSETC_BL_MAX;

	state->gc.parent = &i2c->dev;
	state->gc.label = i2c->name;
	state->gc.owner = THIS_MODULE;
	state->gc.base = -1;
	state->gc.ngpio = NUM_GPIO;

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 17, 0)
	state->gc.set = attiny_gpio_set_void;
#else
	state->gc.set = attiny_gpio_set;
#endif
	state->gc.get_direction = attiny_gpio_get_direction;
	state->gc.can_sleep = true;

	ret = devm_gpiochip_add_data(&i2c->dev, &state->gc, state);
	if (ret) {
		dev_err(&i2c->dev, "Failed to create gpiochip: %d\n", ret);
		goto error;
	}

	return 0;

error:
	mutex_destroy(&state->lock);

	return ret;
}

static void attiny_i2c_remove(struct i2c_client *client)
{
	struct attiny_lcd *state = i2c_get_clientdata(client);

	mutex_destroy(&state->lock);
}

static void attiny_i2c_shutdown(struct i2c_client *client)
{
	struct attiny_lcd *state = i2c_get_clientdata(client);

	/* Best effort: the system is going down anyway. */
	attiny_write(state->regmap, REG_PWM, FYSETC_PWM_OFF);
	attiny_write(state->regmap, REG_POWERON, 0);
}

static const struct of_device_id attiny_dt_ids[] = {
	/* reg_display@45 created by vc4-kms-dsi-7inch; the stock driver is blacklisted. */
	{ .compatible = "raspberrypi,7inch-touchscreen-panel-regulator" },
	{},
};
MODULE_DEVICE_TABLE(of, attiny_dt_ids);

static struct i2c_driver attiny_regulator_driver = {
	.driver = {
		.name = "fysetc_sleep",
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = attiny_dt_ids,
	},
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 3, 0)
	.probe_new = attiny_i2c_probe,
#else
	.probe = attiny_i2c_probe,
#endif
	.remove	= attiny_i2c_remove,
	.shutdown = attiny_i2c_shutdown,
};

module_i2c_driver(attiny_regulator_driver);

MODULE_AUTHOR("Marek Vasut <marex@denx.de>");
MODULE_AUTHOR("kkobold");
MODULE_DESCRIPTION("FYSETC 5 inch DSI Touch Screen V1.1 panel controller (on/off backlight)");
MODULE_LICENSE("GPL v2");
