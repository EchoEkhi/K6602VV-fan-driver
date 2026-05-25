// SPDX-License-Identifier: GPL-2.0
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dmi.h>
#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#define ASUS_FAN_NAME "K6602VV_fan"
#define ASUS_FAN_COUNT 2

#define ASUS_EC_DATA_PORT 0x25c
#define ASUS_EC_CMD_PORT  0x25d
#define ASUS_EC_PORT_LEN  2

#define ASUS_EC_OBF BIT(0)
#define ASUS_EC_IBF BIT(1)

#define ASUS_EC_CMD_INIT    0xbb
#define ASUS_EC_CMD_HEALTHY 0xdd

#define ASUS_EC_STATUS_OK           0
#define ASUS_EC_STATUS_INVALID_DATA 2
#define ASUS_EC_STATUS_TIMEOUT      0x102

#define ASUS_EC_MAX_PAYLOAD 8
#define ASUS_EC_WAIT_US     20
#define ASUS_EC_TIMEOUT_US  100000

static DEFINE_MUTEX(asus_ec_lock);

static bool allow_unsupported;
module_param(allow_unsupported, bool, 0444);
MODULE_PARM_DESC(allow_unsupported,
		 "Allow loading on machines not present in the DMI allowlist");

struct asus_fan_data {
	struct platform_device *pdev;
	struct device *hwmon_dev;
	unsigned long manual_mask;
	u8 pwm[ASUS_FAN_COUNT];
};

static struct asus_fan_data asus_fan;

static const struct dmi_system_id asus_fan_dmi_table[] __initconst = {
	{
		.ident = "ASUS Vivobook K6602VV",
		.matches = {
			DMI_MATCH(DMI_SYS_VENDOR, "ASUSTeK COMPUTER INC."),
			DMI_MATCH(DMI_PRODUCT_NAME,
				  "Vivobook_ASUSLaptop K6602VV_K6602VV"),
			DMI_MATCH(DMI_BOARD_NAME, "K6602VV"),
		},
	},
	{ }
};
MODULE_DEVICE_TABLE(dmi, asus_fan_dmi_table);

static int asus_ec_wait_for(u8 mask, bool set)
{
	int waited;

	for (waited = 0; waited < ASUS_EC_TIMEOUT_US; waited += ASUS_EC_WAIT_US) {
		u8 status = inb(ASUS_EC_CMD_PORT);

		if (!!(status & mask) == set)
			return 0;

		usleep_range(ASUS_EC_WAIT_US, ASUS_EC_WAIT_US + 10);
	}

	return -ETIMEDOUT;
}

static void asus_ec_flush_output(void)
{
	int i;

	for (i = 0; i < 32; i++) {
		if (!(inb(ASUS_EC_CMD_PORT) & ASUS_EC_OBF))
			return;
		inb(ASUS_EC_DATA_PORT);
		udelay(ASUS_EC_WAIT_US);
	}
}

static int asus_ec_transaction_once(u8 command, const u8 *payload, u8 payload_len,
				    bool read, u8 *value)
{
	int i;
	int ret;

	if (payload_len > ASUS_EC_MAX_PAYLOAD)
		return -EINVAL;

	asus_ec_flush_output();

	ret = asus_ec_wait_for(ASUS_EC_IBF, false);
	if (ret)
		return ret;
	outb(0xff, ASUS_EC_CMD_PORT);

	ret = asus_ec_wait_for(ASUS_EC_IBF, false);
	if (ret)
		return ret;
	outb(command, ASUS_EC_CMD_PORT);

	for (i = 0; i < payload_len; i++) {
		ret = asus_ec_wait_for(ASUS_EC_IBF, false);
		if (ret)
			return ret;
		outb(payload[i], ASUS_EC_DATA_PORT);
	}

	ret = asus_ec_wait_for(ASUS_EC_IBF, false);
	if (ret)
		return ret;

	if (read) {
		ret = asus_ec_wait_for(ASUS_EC_OBF, true);
		if (ret)
			return ret;
		if (value)
			*value = inb(ASUS_EC_DATA_PORT);
	}

	return 0;
}

static int asus_ec_transaction(u8 command, const u8 *payload, u8 payload_len,
			       bool read, u8 *value)
{
	int ret;

	mutex_lock(&asus_ec_lock);
	ret = asus_ec_transaction_once(command, payload, payload_len, read, value);
	if (ret == -ETIMEDOUT)
		ret = asus_ec_transaction_once(command, payload, payload_len, read, value);
	mutex_unlock(&asus_ec_lock);

	return ret;
}

static int asus_ec_healthy_read(u8 op0, u8 op1, u8 *value)
{
	u8 payload[] = { op0, op1, 0x00 };

	return asus_ec_transaction(ASUS_EC_CMD_HEALTHY, payload, sizeof(payload),
				   true, value);
}

static int asus_ec_healthy_write(u8 op0, u8 op1, u8 value)
{
	u8 payload[] = { op0, op1, value };

	return asus_ec_transaction(ASUS_EC_CMD_HEALTHY, payload, sizeof(payload),
				   false, NULL);
}

static int asus_fan_require_table(void)
{
	u8 payload[] = { 0x50 };
	u8 version;
	int ret;

	ret = asus_ec_transaction(ASUS_EC_CMD_INIT, payload, sizeof(payload), true,
				  &version);
	if (ret)
		return ret;
	if (!version)
		return -ENODEV;

	return 0;
}

static int asus_fan_count(u8 *count)
{
	int ret;

	ret = asus_fan_require_table();
	if (ret)
		return ret;

	return asus_ec_healthy_read(0x02, 0x30, count);
}

static int asus_fan_select(u8 fan)
{
	if (fan >= ASUS_FAN_COUNT)
		return -EINVAL;

	return asus_ec_healthy_write(0x82, 0x32, fan);
}

static int asus_fan_rpm(u8 fan, u16 *rpm)
{
	u8 low;
	u8 high;
	int ret;

	ret = asus_fan_require_table();
	if (ret)
		return ret;

	ret = asus_fan_select(fan);
	if (ret)
		return ret;

	ret = asus_ec_healthy_read(0x02, 0x33, &low);
	if (ret)
		return ret;

	ret = asus_ec_healthy_read(0x02, 0x34, &high);
	if (ret)
		return ret;

	*rpm = ((u16)high << 8) | low;
	return 0;
}

static int asus_cpu_temp(u8 *temp)
{
	int ret;

	ret = asus_fan_require_table();
	if (ret)
		return ret;

	return asus_ec_healthy_read(0x02, 0x00, temp);
}

static int asus_fan_test_mode(u8 fan, bool enabled)
{
	int ret;

	ret = asus_fan_require_table();
	if (ret)
		return ret;

	ret = asus_fan_select(fan);
	if (ret)
		return ret;

	return asus_ec_healthy_write(0x82, 0x31, enabled ? 1 : 0);
}

static int asus_fan_duty(u8 fan, u8 duty)
{
	int ret;

	ret = asus_fan_require_table();
	if (ret)
		return ret;

	ret = asus_fan_select(fan);
	if (ret)
		return ret;

	return asus_ec_healthy_write(0x82, 0x35, duty);
}

static umode_t asus_fan_hwmon_is_visible(const void *drvdata,
					 enum hwmon_sensor_types type,
					 u32 attr, int channel)
{
	switch (type) {
	case hwmon_fan:
		if (channel < ASUS_FAN_COUNT && attr == hwmon_fan_input)
			return 0444;
		break;
	case hwmon_temp:
		if (channel == 0 && attr == hwmon_temp_input)
			return 0444;
		break;
	case hwmon_pwm:
		if (channel < ASUS_FAN_COUNT &&
		    (attr == hwmon_pwm_input ||
		     attr == hwmon_pwm_enable ||
		     attr == hwmon_pwm_auto_channels_temp))
			return 0644;
		break;
	default:
		break;
	}

	return 0;
}

static int asus_fan_hwmon_read(struct device *dev,
			       enum hwmon_sensor_types type, u32 attr,
			       int channel, long *val)
{
	struct asus_fan_data *data = dev_get_drvdata(dev);
	u16 rpm;
	u8 temp;
	int ret;

	switch (type) {
	case hwmon_fan:
		if (attr != hwmon_fan_input)
			return -EOPNOTSUPP;

		ret = asus_fan_rpm(channel, &rpm);
		if (ret)
			return ret;
		*val = rpm;
		return 0;

	case hwmon_temp:
		if (attr != hwmon_temp_input)
			return -EOPNOTSUPP;

		ret = asus_cpu_temp(&temp);
		if (ret)
			return ret;
		*val = temp * 1000;
		return 0;

	case hwmon_pwm:
		switch (attr) {
		case hwmon_pwm_input:
			*val = data->pwm[channel];
			return 0;
		case hwmon_pwm_enable:
			*val = test_bit(channel, &data->manual_mask) ? 1 : 2;
			return 0;
		case hwmon_pwm_auto_channels_temp:
			*val = 1;
			return 0;
		default:
			return -EOPNOTSUPP;
		}

	default:
		return -EOPNOTSUPP;
	}
}

static int asus_fan_hwmon_write(struct device *dev,
				enum hwmon_sensor_types type, u32 attr,
				int channel, long val)
{
	struct asus_fan_data *data = dev_get_drvdata(dev);
	u8 duty;
	int ret;

	if (type != hwmon_pwm)
		return -EOPNOTSUPP;

	switch (attr) {
	case hwmon_pwm_input:
		if (val < 0 || val > 255)
			return -EINVAL;

		if (!test_bit(channel, &data->manual_mask))
			return -EACCES;

		duty = (u8)val;
		ret = asus_fan_duty(channel, duty);
		if (ret)
			return ret;
		data->pwm[channel] = val;
		return 0;

	case hwmon_pwm_enable:
		if (val == 1) {
			ret = asus_fan_test_mode(channel, true);
			if (ret)
				return ret;
			set_bit(channel, &data->manual_mask);
			return 0;
		}

		if (val == 2) {
			ret = asus_fan_test_mode(channel, false);
			if (ret)
				return ret;
			clear_bit(channel, &data->manual_mask);
			return 0;
		}

		return -EINVAL;

	case hwmon_pwm_auto_channels_temp:
		if (val != 1)
			return -EINVAL;
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

static const struct hwmon_ops asus_fan_hwmon_ops = {
	.is_visible = asus_fan_hwmon_is_visible,
	.read = asus_fan_hwmon_read,
	.write = asus_fan_hwmon_write,
};

static const struct hwmon_channel_info *asus_fan_hwmon_info[] = {
	HWMON_CHANNEL_INFO(fan,
			   HWMON_F_INPUT,
			   HWMON_F_INPUT),
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT),
	HWMON_CHANNEL_INFO(pwm,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE |
			   HWMON_PWM_AUTO_CHANNELS_TEMP,
			   HWMON_PWM_INPUT | HWMON_PWM_ENABLE |
			   HWMON_PWM_AUTO_CHANNELS_TEMP),
	NULL,
};

static const struct hwmon_chip_info asus_fan_chip_info = {
	.ops = &asus_fan_hwmon_ops,
	.info = asus_fan_hwmon_info,
};

static int __init asus_fan_init(void)
{
	u8 count;
	int i;
	int ret;

	if (!allow_unsupported && !dmi_check_system(asus_fan_dmi_table)) {
		pr_err(ASUS_FAN_NAME
		       ": unsupported DMI system, refusing to touch EC ports\n");
		return -ENODEV;
	}

	if (!request_region(ASUS_EC_DATA_PORT, ASUS_EC_PORT_LEN, ASUS_FAN_NAME)) {
		pr_err(ASUS_FAN_NAME
		       ": EC ports 0x%x-0x%x already reserved by another driver\n",
		       ASUS_EC_DATA_PORT, ASUS_EC_CMD_PORT);
		return -EBUSY;
	}

	ret = asus_fan_count(&count);
	if (ret)
		goto err_release_region;

	asus_fan.pdev = platform_device_register_simple(ASUS_FAN_NAME,
							PLATFORM_DEVID_NONE,
							NULL, 0);
	if (IS_ERR(asus_fan.pdev)) {
		ret = PTR_ERR(asus_fan.pdev);
		asus_fan.pdev = NULL;
		pr_err(ASUS_FAN_NAME ": failed to register platform device: %d\n",
		       ret);
		goto err_release_region;
	}

	for (i = 0; i < ASUS_FAN_COUNT; i++)
		asus_fan.pwm[i] = 255;

	asus_fan.hwmon_dev =
		hwmon_device_register_with_info(&asus_fan.pdev->dev,
						ASUS_FAN_NAME, &asus_fan,
						&asus_fan_chip_info, NULL);
	if (IS_ERR(asus_fan.hwmon_dev)) {
		ret = PTR_ERR(asus_fan.hwmon_dev);
		asus_fan.hwmon_dev = NULL;
		pr_err(ASUS_FAN_NAME ": failed to register hwmon device: %d\n",
		       ret);
		goto err_unregister_platform;
	}

	pr_info(ASUS_FAN_NAME
		": loaded as hwmon with %u fan(s), EC reported %u, EC ports 0x%x-0x%x\n",
		ASUS_FAN_COUNT, count, ASUS_EC_DATA_PORT, ASUS_EC_CMD_PORT);
	return 0;

err_unregister_platform:
	platform_device_unregister(asus_fan.pdev);
	asus_fan.pdev = NULL;
err_release_region:
	release_region(ASUS_EC_DATA_PORT, ASUS_EC_PORT_LEN);
	return ret;
}

static void __exit asus_fan_exit(void)
{
	int fan;

	if (asus_fan.hwmon_dev)
		hwmon_device_unregister(asus_fan.hwmon_dev);

	for_each_set_bit(fan, &asus_fan.manual_mask, ASUS_FAN_COUNT) {
		if (asus_fan_test_mode(fan, false))
			pr_warn(ASUS_FAN_NAME
				": failed to restore automatic mode for fan%d\n",
				fan + 1);
	}

	if (asus_fan.pdev)
		platform_device_unregister(asus_fan.pdev);

	release_region(ASUS_EC_DATA_PORT, ASUS_EC_PORT_LEN);
	pr_info(ASUS_FAN_NAME ": unloaded\n");
}

module_init(asus_fan_init);
module_exit(asus_fan_exit);

MODULE_AUTHOR("EchoEkhi <echoekhi@gmail.com>");
MODULE_DESCRIPTION("ASUS EC hwmon fan driver for the Vivobook model K6602VV");
MODULE_LICENSE("GPL");
