/*
 * FPC1020 Fingerprint sensor device driver
 *
 * This driver will control the platform resources that the FPC fingerprint
 * sensor needs to operate. The major things are probing the sensor to check
 * that it is actually connected and let the Kernel know this and with that also
 * enabling and disabling of regulators, controlling GPIOs such as sensor reset
 * line, sensor IRQ line.
 *
 * The driver will expose most of its available functionality in sysfs which
 * enables dynamic control of these features from eg. a user space process.
 *
 * The sensor's IRQ events will be pushed to Kernel's event handling system and
 * are exposed in the drivers event node.
 *
 * This driver will NOT send any commands to the sensor it only controls the
 * electrical parts.
 *
 *
 * Copyright (c) 2015 Fingerprint Cards AB <tech@fingerprints.com>
 * Copyright (C) 2019 XiaoMi, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License Version 2
 * as published by the Free Software Foundation.
 */
#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/notifier.h>
#include <linux/fb.h>

#ifdef CONFIG_TOUCHSCREEN_COMMON
#include <linux/input.h>
#include <linux/input/tp_common.h>
#endif

#define FPC1020_NAME "fpc1020"

#define FPC_TTW_HOLD_TIME		400
#define RESET_LOW_SLEEP_MIN_US		5000
#define RESET_LOW_SLEEP_MAX_US		(RESET_LOW_SLEEP_MIN_US + 100)
#define RESET_HIGH_SLEEP1_MIN_US	100
#define RESET_HIGH_SLEEP1_MAX_US	(RESET_HIGH_SLEEP1_MIN_US + 100)
#define RESET_HIGH_SLEEP2_MIN_US	5000
#define RESET_HIGH_SLEEP2_MAX_US	(RESET_HIGH_SLEEP2_MIN_US + 100)
#define PWR_ON_SLEEP_MIN_US		100
#define PWR_ON_SLEEP_MAX_US		(PWR_ON_SLEEP_MIN_US + 900)

#define NUM_PARAMS_REG_ENABLE_SET 2

#ifdef CONFIG_MACH_LONGCHEER
extern int fpsensor;
#endif

static const char *const pctl_names[] = {
	"fpc1020_reset_reset",
	"fpc1020_reset_active",
	"fpc1020_irq_active",
};

typedef enum {
	VDD_ANA = 0,
	VCC_SPI = 0,
	VDD_IO = 0,
	FPC_VREG_MAX,
} fpc_rails_t;

struct vreg_config {
	char *name;
	unsigned long vmin;
	unsigned long vmax;
	int ua_load;
};

static const struct vreg_config vreg_conf[] = {
	{ "vdd_ana", 1800000UL, 1800000UL, 6000 },
	{ "vcc_spi", 1800000UL, 1800000UL, 10 },
	{ "vdd_io", 1800000UL, 1800000UL, 6000 },
};

struct fpc1020_data {
	struct device *dev;
	struct pinctrl *fingerprint_pinctrl;
	struct pinctrl_state *pinctrl_state[ARRAY_SIZE(pctl_names)];
	struct regulator *vreg[ARRAY_SIZE(vreg_conf)];
	struct wakeup_source *ttw_ws;
	struct mutex lock; /* To set/get exported values in sysfs */
	struct notifier_block fb_notifier;
#ifdef CONFIG_TOUCHSCREEN_COMMON
	struct input_handler input_handler;
#endif
	int irq_gpio;
	int rst_gpio;
	bool prepared;
	bool fb_black;
	bool wait_finger_down;
	bool proximity_state; /* 0:far 1:near */
	atomic_t wakeup_enabled; /* Used both in ISR and non-ISR */
};

static struct kernfs_node *soc_symlink = NULL;

static inline int vreg_setup(struct fpc1020_data *fpc1020, fpc_rails_t fpc_rail,
			     bool enable)
{
	int rc;
	struct regulator *vreg = fpc1020->vreg[fpc_rail];
	struct device *dev = fpc1020->dev;

	if (!vreg)
		return -EINVAL;

	if (enable) {
		if (regulator_count_voltages(vreg) > 0) {
			rc = regulator_set_voltage(vreg,
					vreg_conf[fpc_rail].vmin,
					vreg_conf[fpc_rail].vmax);
			if (rc)
				dev_err(dev,
					"Unable to set voltage on %s, %d\n",
					vreg_conf[fpc_rail].name, rc);
		}

		rc = regulator_set_load(vreg, vreg_conf[fpc_rail].ua_load);
		if (rc < 0)
			dev_err(dev, "Unable to set current on %s, %d\n",
					vreg_conf[fpc_rail].name, rc);

		rc = regulator_enable(vreg);
		if (rc) {
			dev_err(dev, "error enabling %s: %d\n",
				vreg_conf[fpc_rail].name, rc);
		}
	} else {
		if (regulator_is_enabled(vreg))
			regulator_disable(vreg);
		rc = 0;
	}

	return rc;
}

/*
 * sysfs node for controlling clocks.
 *
 * This is disabled in platform variant of this driver but kept for
 * backwards compatibility. Only prints a debug print that it is
 * disabled.
 */
static inline ssize_t clk_enable_store(struct device *dev,
				       struct device_attribute *attr, const char *buf,
				       size_t count)
{
	return count;
}
static DEVICE_ATTR_WO(clk_enable);

static inline ssize_t fingerdown_wait_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	if (!strcmp(buf, "enable"))
		fpc1020->wait_finger_down = true;
	else if (!strcmp(buf, "disable"))
		fpc1020->wait_finger_down = false;
	else
		return -EINVAL;

	return count;
}
static DEVICE_ATTR_WO(fingerdown_wait);

/*
 * Will try to select the set of pins (GPIOS) defined in a pin control node of
 * the device tree named @p name.
 *
 * The node can contain several eg. GPIOs that is controlled when selecting it.
 * The node may activate or deactivate the pins it contains, the action is
 * defined in the device tree node itself and not here. The states used
 * internally is fetched at probe time.
 *
 * @see pctl_names
 * @see fpc1020_probe
 */
static inline int select_pin_ctl(struct fpc1020_data *fpc1020, const char *name)
{
	size_t i;
	int rc;
	struct device *dev = fpc1020->dev;

	for (i = 0; i < ARRAY_SIZE(pctl_names); i++) {
		const char *n = pctl_names[i];
		if (!strcmp(n, name)) {
			rc = pinctrl_select_state(fpc1020->fingerprint_pinctrl,
					fpc1020->pinctrl_state[i]);
			if (rc)
				dev_err(dev, "cannot select '%s'\n", name);
			else
				dev_dbg(dev, "Selected '%s'\n", name);

			return rc;
		}
	}

	dev_err(dev, "%s:'%s' not found\n", __func__, name);
	return -EINVAL;
}

static inline ssize_t pinctl_set_store(struct device *dev,
				       struct device_attribute *attr, const char *buf,
				       size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc;

	mutex_lock(&fpc1020->lock);
	rc = select_pin_ctl(fpc1020, buf);
	mutex_unlock(&fpc1020->lock);

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(pinctl_set);

static inline fpc_rails_t name_to_fpc_rail(const char *name)
{
	if (!strcmp(name, "vdd_ana"))
		return VDD_ANA;
	else if (!strcmp(name, "vcc_spi"))
		return VCC_SPI;
	else if (!strcmp(name, "vdd_io"))
		return VDD_IO;
	else
		return -EINVAL;
}

static inline ssize_t regulator_enable_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	char op;
	char name[16];
	int rc;
	bool enable;
	fpc_rails_t fpc_rail;

	if (sscanf(buf, "%15[^,],%c", name, &op) != NUM_PARAMS_REG_ENABLE_SET)
		return -EINVAL;
	if (op == 'e')
		enable = true;
	else if (op == 'd')
		enable = false;
	else
		return -EINVAL;

	fpc_rail = name_to_fpc_rail(name);

	mutex_lock(&fpc1020->lock);
	rc = vreg_setup(fpc1020, fpc_rail, enable);
	mutex_unlock(&fpc1020->lock);

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(regulator_enable);

static inline void hw_reset(struct fpc1020_data *fpc1020)
{
	(void)gpio_get_value(fpc1020->irq_gpio);

	select_pin_ctl(fpc1020, "fpc1020_reset_active");

	usleep_range(RESET_HIGH_SLEEP1_MIN_US, RESET_HIGH_SLEEP1_MAX_US);

	select_pin_ctl(fpc1020, "fpc1020_reset_reset");

	usleep_range(RESET_LOW_SLEEP_MIN_US, RESET_LOW_SLEEP_MAX_US);

	select_pin_ctl(fpc1020, "fpc1020_reset_active");

	usleep_range(RESET_HIGH_SLEEP2_MIN_US, RESET_HIGH_SLEEP2_MAX_US);

	(void)gpio_get_value(fpc1020->irq_gpio);
}

static inline ssize_t hw_reset_store(struct device *dev, struct device_attribute *attr,
				     const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strcmp(buf, "reset")) {
		mutex_lock(&fpc1020->lock);
		hw_reset(fpc1020);
		mutex_unlock(&fpc1020->lock);
		return count;
	}

	return -EINVAL;
}
static DEVICE_ATTR_WO(hw_reset);

static inline void config_irq(struct fpc1020_data *fpc1020, bool enabled)
{
	static bool irq_enabled = true;

	mutex_lock(&fpc1020->lock);
	if (enabled != irq_enabled) {
		if (enabled)
			enable_irq(gpio_to_irq(fpc1020->irq_gpio));
		else
			disable_irq(gpio_to_irq(fpc1020->irq_gpio));

		irq_enabled = enabled;
	}
	mutex_unlock(&fpc1020->lock);
}

/*
 * Will setup GPIOs, and regulators to correctly initialize the touch sensor to
 * be ready for work.
 *
 * In the correct order according to the sensor spec this function will
 * enable/disable regulators, and reset line, all to set the sensor in a
 * correct power on or off state "electrical" wise.
 *
 * @see  device_prepare_set
 * @note This function will not send any commands to the sensor it will only
 *       control it "electrically".
 */
static inline void device_prepare(struct fpc1020_data *fpc1020, bool enable)
{
	mutex_lock(&fpc1020->lock);
	if (enable && !fpc1020->prepared) {
		fpc1020->prepared = true;
		select_pin_ctl(fpc1020, "fpc1020_reset_reset");

		vreg_setup(fpc1020, VCC_SPI, true);
		vreg_setup(fpc1020, VDD_IO, true);
		vreg_setup(fpc1020, VDD_ANA, true);

		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);

		/* As we can't control chip, select here the other part of the
		 * sensor driver eg. the TEE driver needs to do a _SOFT_ reset
		 * on the sensor after power up to be sure that the sensor is
		 * in a good state after power up. Acked by ASIC. */
		select_pin_ctl(fpc1020, "fpc1020_reset_active");
	} else if (!enable && fpc1020->prepared) {
		select_pin_ctl(fpc1020, "fpc1020_reset_reset");

		usleep_range(PWR_ON_SLEEP_MIN_US, PWR_ON_SLEEP_MAX_US);

		vreg_setup(fpc1020, VDD_ANA, false);
		vreg_setup(fpc1020, VDD_IO, false);
		vreg_setup(fpc1020, VCC_SPI, false);

		fpc1020->prepared = false;
	}
	mutex_unlock(&fpc1020->lock);
}

/*
 * sysfs node to enable/disable (power up/power down) the touch sensor
 *
 * @see device_prepare
 */
static inline ssize_t device_prepare_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	int rc = 0;
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);

	if (!strcmp(buf, "enable"))
		device_prepare(fpc1020, true);
	else if (!strcmp(buf, "disable"))
		device_prepare(fpc1020, false);
	else
		rc = -EINVAL;

	return rc ? rc : count;
}
static DEVICE_ATTR_WO(device_prepare);

/*
 * sysfs node for controlling whether the driver is allowed
 * to wake up the platform on interrupt.
 */
static inline ssize_t wakeup_enable_store(struct device *dev,
					  struct device_attribute *attr,
					  const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	ssize_t ret = count;

	mutex_lock(&fpc1020->lock);
	if (!strcmp(buf, "enable"))
		atomic_set(&fpc1020->wakeup_enabled, 1);
	else if (!strcmp(buf, "disable"))
		atomic_set(&fpc1020->wakeup_enabled, 0);
	else
		ret = -EINVAL;
	mutex_unlock(&fpc1020->lock);

	return ret;
}
static DEVICE_ATTR_WO(wakeup_enable);

/*
 * sysfs node to check the interrupt status of the sensor, the interrupt
 * handler should perform sysf_notify to allow userland to poll the node.
 */
static inline ssize_t irq_get(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int irq = gpio_get_value(fpc1020->irq_gpio);

	return scnprintf(buf, PAGE_SIZE, "%i\n", irq);
}

/*
 * writing to the irq node will just drop a printk message
 * and return success, used for latency measurement.
 */
static inline ssize_t irq_ack(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	return count;
}
static DEVICE_ATTR(irq, 0600, irq_get, irq_ack);

static inline ssize_t proximity_state_store(struct device *dev,
					    struct device_attribute *attr,
					    const char *buf, size_t count)
{
	struct fpc1020_data *fpc1020 = dev_get_drvdata(dev);
	int rc;
	int val;

	rc = kstrtoint(buf, 10, &val);
	if (rc)
		return -EINVAL;

	fpc1020->proximity_state = !!val;

	if (fpc1020->fb_black) {
		if (fpc1020->proximity_state)
			/* Disable IRQ when screen is off and proximity sensor is covered */
			config_irq(fpc1020, false);
		else
			/* Enable IRQ when screen is off and proximity sensor is uncovered */
			config_irq(fpc1020, true);
	}
	return count;
}
static DEVICE_ATTR_WO(proximity_state);

static struct attribute *attributes[] = {
	&dev_attr_pinctl_set.attr,
	&dev_attr_device_prepare.attr,
	&dev_attr_regulator_enable.attr,
	&dev_attr_hw_reset.attr,
	&dev_attr_wakeup_enable.attr,
	&dev_attr_clk_enable.attr,
	&dev_attr_irq.attr,
	&dev_attr_fingerdown_wait.attr,
	&dev_attr_proximity_state.attr,
	NULL
};

static const struct attribute_group attribute_group = {
	.attrs = attributes,
};

static __always_inline irqreturn_t fpc1020_irq_handler(int irq, void *handle)
{
	struct fpc1020_data *fpc1020 = handle;

	__pm_wakeup_event(fpc1020->ttw_ws, FPC_TTW_HOLD_TIME);

	sysfs_notify(&fpc1020->dev->kobj, NULL, dev_attr_irq.attr.name);

	return IRQ_HANDLED;
}

static inline int fpc1020_request_named_gpio(struct fpc1020_data *fpc1020,
					     const char *label, int *gpio)
{
	struct device *dev = fpc1020->dev;
	struct device_node *np = dev->of_node;
	int rc;

	rc = of_get_named_gpio(np, label, 0);

	if (rc < 0)
		return rc;

	*gpio = rc;

	rc = devm_gpio_request(dev, *gpio, label);
	if (rc)
		return rc;

	return 0;
}

static __always_inline int fpc_fb_notif_callback(struct notifier_block *nb, unsigned long val,
						 void *data)
{
	struct fpc1020_data *fpc1020 =
		container_of(nb, struct fpc1020_data, fb_notifier);
	struct fb_event *evdata = data;
	unsigned int blank;

	if (!fpc1020 || val != FB_EVENT_BLANK)
		return 0;

	if (evdata && evdata->data && val == FB_EVENT_BLANK) {
		blank = *(int *)(evdata->data);
		switch (blank) {
		case FB_BLANK_POWERDOWN:
			fpc1020->fb_black = true;
			/*
			 * Disable IRQ when screen turns off,
			 * if proximity sensor is covered
			 */
			if (fpc1020->proximity_state)
				config_irq(fpc1020, false);
			break;
		case FB_BLANK_UNBLANK:
		case FB_BLANK_NORMAL:
			fpc1020->fb_black = false;
			/* Unconditionally enable IRQ when screen turns on */
			config_irq(fpc1020, true);
			break;
		default:
			break;
		}
	}

	return NOTIFY_OK;
}

static struct notifier_block fpc_notif_block = {
	.notifier_call = fpc_fb_notif_callback,
};

#ifdef CONFIG_TOUCHSCREEN_COMMON
static inline int input_connect(struct input_handler *handler, struct input_dev *dev,
				const struct input_device_id *id)
{
	int rc;
	struct input_handle *handle;
	struct fpc1020_data *fpc1020 =
		container_of(handler, struct fpc1020_data, input_handler);

	if (!strstr(dev->name, "uinput-fpc"))
		return -ENODEV;

	handle = kzalloc(sizeof(struct input_handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = FPC1020_NAME;
	handle->private = fpc1020;

	rc = input_register_handle(handle);
	if (rc)
		goto err_input_register_handle;

	rc = input_open_device(handle);
	if (rc)
		goto err_input_open_device;

	return 0;

err_input_open_device:
	input_unregister_handle(handle);
err_input_register_handle:
	kfree(handle);
	return rc;
}

static inline bool input_filter(struct input_handle *handle, unsigned int type,
				unsigned int code, int value)
{
	if (code == KEY_HOME)
		return !capacitive_keys_enabled;

	return false;
}

static inline void input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id ids[] = {
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{},
};
#endif

static inline int fpc1020_get_regulators(struct fpc1020_data *fpc1020)
{
	struct device *dev = fpc1020->dev;
	unsigned short i;

	for (i = 0; i < FPC_VREG_MAX; i++) {
		fpc1020->vreg[i] = devm_regulator_get(dev, vreg_conf[i].name);

		if (IS_ERR_OR_NULL(fpc1020->vreg[i])) {
			fpc1020->vreg[i] = NULL;
			dev_err(dev, "CRITICAL: Cannot get %s regulator.\n",
				vreg_conf[i].name);
			return -EINVAL;
		}
	}

	return 0;
}

static inline int fpc1020_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device *platform_dev;
	struct kobject *soc_kobj;
	struct kernfs_node *devices_node, *soc_node;
	struct device_node *np = dev->of_node;
	struct fpc1020_data *fpc1020 =
		devm_kzalloc(dev, sizeof(*fpc1020), GFP_KERNEL);
	int rc = 0;
	size_t i;

	if (!fpc1020) {
		dev_err(dev,
			"failed to allocate memory for struct fpc1020_data\n");
		return -ENOMEM;
	}

#ifdef CONFIG_MACH_LONGCHEER
	if (fpsensor != 1) {
		pr_err("Macle fpc1020_probe failed as fpsensor=%d(1=fp)\n",
		       fpsensor);

		return -1;
	}
#endif

	fpc1020->dev = dev;
	platform_set_drvdata(pdev, fpc1020);

	if (!np) {
		dev_err(dev, "no of node found\n");
		return -EINVAL;
	}

	rc = fpc1020_get_regulators(fpc1020);
	if (rc)
		return rc;

	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_irq",
					&fpc1020->irq_gpio);
	if (rc)
		return -EINVAL;

	rc = fpc1020_request_named_gpio(fpc1020, "fpc,gpio_rst",
					&fpc1020->rst_gpio);
	if (rc)
		return -EINVAL;

	fpc1020->fingerprint_pinctrl = devm_pinctrl_get(dev);
	if (IS_ERR(fpc1020->fingerprint_pinctrl)) {
		if (PTR_ERR(fpc1020->fingerprint_pinctrl) == -EPROBE_DEFER) {
			dev_info(dev, "pinctrl is not ready\n");
			return -EPROBE_DEFER;
		}
		dev_err(dev, "Target does not use pinctrl\n");
		fpc1020->fingerprint_pinctrl = NULL;
		return -EINVAL;
	}

	for (i = 0; i < ARRAY_SIZE(pctl_names); i++) {
		const char *n = pctl_names[i];
		struct pinctrl_state *state =
			pinctrl_lookup_state(fpc1020->fingerprint_pinctrl, n);
		if (IS_ERR(state)) {
			dev_err(dev, "cannot find '%s'\n", n);
			return -EINVAL;
		}
		dev_info(dev, "found pin control %s\n", n);
		fpc1020->pinctrl_state[i] = state;
	}

	select_pin_ctl(fpc1020, "fpc1020_reset_reset");
	select_pin_ctl(fpc1020, "fpc1020_irq_active");

	atomic_set(&fpc1020->wakeup_enabled, 0);

	if (of_property_read_bool(dev->of_node, "fpc,enable-wakeup"))
		device_init_wakeup(dev, 1);

	mutex_init(&fpc1020->lock);
	rc = devm_request_threaded_irq(dev, gpio_to_irq(fpc1020->irq_gpio),
		NULL, fpc1020_irq_handler,
		IRQF_TRIGGER_RISING | IRQF_ONESHOT,
		dev_name(dev), fpc1020);
	if (rc)
		return rc;

	/* Request that the interrupt should be wakeable */
	enable_irq_wake(gpio_to_irq(fpc1020->irq_gpio));

	fpc1020->ttw_ws = wakeup_source_register(NULL, "fpc_ttw_ws");

#ifdef CONFIG_TOUCHSCREEN_COMMON
	fpc1020->input_handler.filter = input_filter;
	fpc1020->input_handler.connect = input_connect;
	fpc1020->input_handler.disconnect = input_disconnect;
	fpc1020->input_handler.name = FPC1020_NAME;
	fpc1020->input_handler.id_table = ids;
	rc = input_register_handler(&fpc1020->input_handler);
	if (rc)
		return rc;
#endif

	rc = sysfs_create_group(&dev->kobj, &attribute_group);
	if (rc)
		return rc;

	if (of_property_read_bool(dev->of_node, "fpc,enable-on-boot"))
		(void)device_prepare(fpc1020, true);

	if (!dev->parent || !dev->parent->parent)
		return rc;

	platform_dev = dev->parent->parent;
	if (strcmp(kobject_name(&platform_dev->kobj), "platform"))
		return rc;

	devices_node = platform_dev->kobj.sd->parent;
	soc_kobj = &dev->parent->kobj;
	soc_node = soc_kobj->sd;
	kernfs_get(soc_node);
	soc_symlink = kernfs_create_link(devices_node, kobject_name(soc_kobj),
					 soc_node);
	kernfs_put(soc_node);

	if (IS_ERR(soc_symlink))
		dev_warn(dev, "Unable to create soc symlink\n");

	hw_reset(fpc1020);

	dev_info(dev, "%s: ok\n", __func__);
	fpc1020->fb_black = false;
	fpc1020->wait_finger_down = false;
	fpc1020->fb_notifier = fpc_notif_block;
	fb_register_client(&fpc1020->fb_notifier);

	return 0;
}

static inline int fpc1020_remove(struct platform_device *pdev)
{
	struct fpc1020_data *fpc1020 = platform_get_drvdata(pdev);

	if (!IS_ERR(soc_symlink))
		kernfs_remove_by_name(soc_symlink->parent, soc_symlink->name);
	fb_unregister_client(&fpc1020->fb_notifier);
	sysfs_remove_group(&pdev->dev.kobj, &attribute_group);
	mutex_destroy(&fpc1020->lock);
	wakeup_source_unregister(fpc1020->ttw_ws);
	vreg_setup(fpc1020, VDD_ANA, false);
	vreg_setup(fpc1020, VDD_IO, false);
	vreg_setup(fpc1020, VCC_SPI, false);

	return 0;
}

static const struct of_device_id fpc1020_of_match[] = {
	{ .compatible = "fpc,fpc1020", },
	{},
};
MODULE_DEVICE_TABLE(of, fpc1020_of_match);

static struct platform_driver fpc1020_driver = {
	.driver = {
		.name	= FPC1020_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = fpc1020_of_match,
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
	},
	.probe	= fpc1020_probe,
	.remove	= fpc1020_remove,
};

module_platform_driver(fpc1020_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Aleksej Makarov");
MODULE_AUTHOR("Henrik Tillman <henrik.tillman@fingerprints.com>");
MODULE_DESCRIPTION("FPC1020 Fingerprint sensor device driver.");
