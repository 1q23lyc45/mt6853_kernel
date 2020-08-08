/*
 * Copyright (C) Fourier Semiconductor Inc. 2016-2020. All rights reserved.
 */
#include "fsm_public.h"
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/version.h>
#include <linux/fs.h>
#ifdef CONFIG_OF
#include <linux/of.h>
#include <linux/of_gpio.h>
#endif
#if defined(CONFIG_REGULATOR)
#include <linux/regulator/consumer.h>
static struct regulator *g_fsm_vdd = NULL;
#endif
#include <fsm_ext.h>
static DEFINE_MUTEX(g_fsm_mutex);
static struct device *g_fsm_pdev = NULL;

/* customize configrature */
#include "fsm_firmware.c"
// #include "fsm_proc.c"
#include "fsm_class.c"
#include "fsm_misc.c"
#include "fsm_codec.c"
// #include "fsm_regmap.c"

void fsm_mutex_lock()
{
	mutex_lock(&g_fsm_mutex);
}

void fsm_mutex_unlock()
{
	mutex_unlock(&g_fsm_mutex);
}

int fsm_i2c_reg_read(fsm_dev_t *fsm_dev, uint8_t reg, uint16_t *pVal)
{
	struct i2c_msg msgs[2];
	uint8_t retries = 0;
	uint8_t buffer[2];
	int ret;

	if (!fsm_dev || !fsm_dev->i2c || !pVal) {
		return -EINVAL;
	}

	// write register address.
	msgs[0].addr = fsm_dev->i2c->addr;
	msgs[0].flags = 0;
	msgs[0].len = 1;
	msgs[0].buf = &reg;
	// read register buffer.
	msgs[1].addr = fsm_dev->i2c->addr;
	msgs[1].flags = I2C_M_RD;
	msgs[1].len = 2;
	msgs[1].buf = &buffer[0];

	do {
		mutex_lock(&fsm_dev->i2c_lock);
		ret = i2c_transfer(fsm_dev->i2c->adapter, &msgs[0], ARRAY_SIZE(msgs));
		mutex_unlock(&fsm_dev->i2c_lock);
		if (ret != ARRAY_SIZE(msgs)) {
			fsm_delay_ms(5);
			retries++;
		}
	} while (ret != ARRAY_SIZE(msgs) && retries < FSM_I2C_RETRY);

	if (ret != ARRAY_SIZE(msgs)) {
		pr_err("read %02x transfer error: %d", reg, ret);
		return -EIO;
	}

	*pVal = ((buffer[0] << 8) | buffer[1]);

	return 0;
}

int fsm_i2c_reg_write(fsm_dev_t *fsm_dev, uint8_t reg, uint16_t val)
{
	struct i2c_msg msgs[1];
	uint8_t retries = 0;
	uint8_t buffer[3];
	int ret;

	if (!fsm_dev || !fsm_dev->i2c) {
		return -EINVAL;
	}

	buffer[0] = reg;
	buffer[1] = (val >> 8) & 0x00ff;
	buffer[2] = val & 0x00ff;
	msgs[0].addr = fsm_dev->i2c->addr;
	msgs[0].flags = 0;
	msgs[0].len = sizeof(buffer);
	msgs[0].buf = &buffer[0];

	do {
		mutex_lock(&fsm_dev->i2c_lock);
		ret = i2c_transfer(fsm_dev->i2c->adapter, &msgs[0], ARRAY_SIZE(msgs));
		mutex_unlock(&fsm_dev->i2c_lock);
		if (ret != ARRAY_SIZE(msgs)) {
			fsm_delay_ms(5);
			retries++;
		}
	} while (ret != ARRAY_SIZE(msgs) && retries < FSM_I2C_RETRY);

	if (ret != ARRAY_SIZE(msgs)) {
		pr_err("write %02x transfer error: %d", reg, ret);
		return -EIO;
	}

	return 0;
}

int fsm_i2c_bulkwrite(fsm_dev_t *fsm_dev, uint8_t reg,
				uint8_t *data, int len)
{
	uint8_t retries = 0;
	uint8_t *buf;
	int size;
	int ret;

	if (!fsm_dev || !fsm_dev->i2c || !data) {
		return -EINVAL;
	}

	size = sizeof(uint8_t) + len;
	buf = (uint8_t *)fsm_alloc_mem(size);
	if (!buf) {
		pr_err("alloc memery failed");
		return -ENOMEM;
	}

	buf[0] = reg;
	memcpy(&buf[1], data, len);
	do {
		mutex_lock(&fsm_dev->i2c_lock);
		ret = i2c_master_send(fsm_dev->i2c, buf, size);
		mutex_unlock(&fsm_dev->i2c_lock);
		if (ret < 0) {
			fsm_delay_ms(5);
			retries++;
		} else if (ret == size) {
			break;
		}
	} while (ret != size && retries < FSM_I2C_RETRY);

	fsm_free_mem(buf);

	if (ret != size) {
		pr_err("write %02x transfer error: %d", reg, ret);
		return -EIO;
	}

	return 0;
}

bool fsm_set_pdev(struct device *dev)
{
	if (g_fsm_pdev == NULL || dev == NULL) {
		g_fsm_pdev = dev;
		pr_debug("dev_name: %s", dev_name(dev));
		return true;
	}
	return false; // already got device
}

struct device *fsm_get_pdev(void)
{
	return g_fsm_pdev;
}

static int fsm_check_re25_valid(struct fsm_calib_v2 *data,
				char *buf, int buf_len)
{
	struct fsm_cal_data *cal_data;
	int len = 0;
	int idx;

	if (!data || !buf) {
		return -EINVAL;
	}
	for (idx = 0; idx < data->dev_count; idx++) {
		cal_data = &data->cal_data[idx];
		if (cal_data->cal_re >= cal_data->re_min
					&& cal_data->cal_re <= cal_data->re_max) {
			cal_data->calib_pass = true;
			pr_info("chn:%X, tempr:%d, re25:%d, calibrate success!",
					cal_data->channel, cal_data->cal_tempr, cal_data->cal_re);
			len += snprintf(buf + len, STRING_LEN_MAX, "[%d,%d,%d]",
					cal_data->channel, cal_data->cal_re, cal_data->cal_tempr);
			if (len >= buf_len) {
				pr_info("length of buffer limited:%d", buf_len);
				break;
			}
		}
		else {
			cal_data->calib_pass = false;
			pr_err("chn:%X, tempr:%d, re25:%d out of range",
					cal_data->channel, cal_data->cal_tempr, cal_data->cal_re);
		}
	}

	return len;
}

int fsm_read_efsdata(struct fsm_calib_v2 *data)
{
	char *fname = FSM_CALIB_FILE;
	char buf[STRING_LEN_MAX] = {0};
	struct file *fp;
	// mm_segment_t fs;
	// loff_t pos = 0;
	int result;
	int len = 0;

	if (data == NULL) {
		return -EINVAL;
	}
	if (data->dev_count > 0) {
		return 0;
	}
	// fs = get_fs();
	// set_fs(get_ds());
	fp = filp_open(fname, O_RDONLY, 0644);
	if (IS_ERR(fp)) {
		pr_err("open %s failed", fname);
		// set_fs(fs);
		return PTR_ERR(fp);
	}
	// vfs_read(fp, buf, STRING_LEN_MAX - 1, &pos);
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0))
	result = kernel_read(fp, 0, buf, STRING_LEN_MAX - 1);
#else
	result = kernel_read(fp, buf, STRING_LEN_MAX - 1, 0);
#endif
	if (result <= 0) {
		pr_err("read read fail:%d", result);
		return result;
	}
	data->dev_count = 0;
	do {
		len = sscanf(buf + len, "[%d,%d,%d]",
				&data->cal_data[data->dev_count].channel,
				&data->cal_data[data->dev_count].cal_re,
				&data->cal_data[data->dev_count].cal_tempr);
		if (len <= 0) {
			break;
		}
		pr_info("chn:%X, re:%d, t:%d",
				data->cal_data[data->dev_count].channel,
				data->cal_data[data->dev_count].cal_re,
				data->cal_data[data->dev_count].cal_tempr);
		data->dev_count++;
	} while (1);
	filp_close(fp, NULL);
	// set_fs(fs);

	return 0;
}

int fsm_write_efsdata(struct fsm_calib_v2 *data)
{
	char *fname = FSM_CALIB_FILE;
	char buf[STRING_LEN_MAX] = {0};
	struct file *fp;
	// mm_segment_t fs;
	// loff_t pos = 0;
	int result;
	int len = 0;

	if (!data) {
		return -EINVAL;
	}
	if (data->dev_count <= 0) {
		return 0;
	}
	// fs = get_fs();
	// set_fs(get_ds());
	fp = filp_open(fname, O_RDWR | O_CREAT, 0666);
	if (IS_ERR(fp)) {
		pr_err("open %s failed", fname);
		// set_fs(fs);
		return PTR_ERR(fp);
	}
	len = fsm_check_re25_valid(data, buf, sizeof(buf));
	if (len <= 0) {
		pr_err("re25 is invalid, ret: %d", len);
		return -EINVAL;
	}
	pr_info("save file:%s", fname);
	// vfs_write(fp, buf, len, &pos);
	result = kernel_write(fp, buf, len, 0);
	if (result != len) {
		pr_err("write file fail:%d, len:%d", result, len);
		return -ENOMEM;
	}
	filp_close(fp, NULL);
	// set_fs(fs);

	return 0;
}

int fsm_i2c_save_re25(struct fsm_calib_v2 *calib_data)
{
	fsm_config_t *cfg = fsm_get_config();
	struct fsm_cal_data *dev_cal;
	fsm_dev_t *fsm_dev;
	int temp_val;
	int dev;

	if (!calib_data) {
		return -EINVAL;
	}
	for (dev = 0; dev < cfg->dev_count; dev++) {
		dev_cal = &calib_data->cal_data[dev];
		fsm_dev = fsm_get_fsm_dev_by_id(dev);
		if (fsm_dev == NULL) continue;
		dev_cal->channel = fsm_dev->pos_mask;
		dev_cal->cal_re = fsm_dev->re25;
		dev_cal->cal_tempr = cfg->amb_tempr;
		temp_val = FSM_MAGNIF(fsm_dev->spkr);
		if (fsm_dev->spkr <= 10) { // 10 ohm
			dev_cal->re_min = temp_val * (100 - FSM_SPKR_ALLOWANCE) / 100;
			dev_cal->re_max = temp_val * (100 + FSM_SPKR_ALLOWANCE) / 100;
		} else {
			dev_cal->re_min = temp_val * (100 - FSM_RCVR_ALLOWANCE) / 100;
			dev_cal->re_max = temp_val * (100 + FSM_RCVR_ALLOWANCE) / 100;
		}
		pr_addr(info, "spkr:%d, min:%d, max:%d",
				fsm_dev->spkr, dev_cal->re_min, dev_cal->re_max);
	}
	calib_data->dev_count = cfg->dev_count;
	return fsm_write_efsdata(calib_data);
}

int fsm_vddd_on(struct device *dev)
{
	fsm_config_t *cfg = fsm_get_config();
	int ret = 0;

	if (!cfg || cfg->vddd_on) {
		return 0;
	}
#if defined(CONFIG_REGULATOR)
	g_fsm_vdd = regulator_get(dev, "fsm_vddd");
	if (IS_ERR(g_fsm_vdd) != 0) {
		pr_err("error getting fsm_vddd regulator");
		ret = PTR_ERR(g_fsm_vdd);
		g_fsm_vdd = NULL;
		return ret;
	}
	pr_info("enable regulator");
	regulator_set_voltage(g_fsm_vdd, 1800000, 1800000);
	ret = regulator_enable(g_fsm_vdd);
	if (ret < 0) {
		pr_err("enabling fsm_vddd failed: %d", ret);
	}
#endif
	cfg->vddd_on = 1;
	fsm_delay_ms(10);

	return ret;
}

void fsm_vddd_off(void)
{
	fsm_config_t *cfg = fsm_get_config();

	if (!cfg || !cfg->vddd_on || cfg->dev_count > 0) {
		return;
	}
#if defined(CONFIG_REGULATOR)
	if (g_fsm_vdd) {
		pr_info("disable regulator");
		regulator_disable(g_fsm_vdd);
		regulator_put(g_fsm_vdd);
		g_fsm_vdd = NULL;
	}
#endif
	cfg->vddd_on = 0;
}

int fsm_get_amb_tempr(void)
{
	union power_supply_propval psp = { 0 };
	struct power_supply *psy;
	int tempr = FSM_DFT_AMB_TEMPR;
	int vbat = FSM_DFT_AMB_VBAT;

	psy = power_supply_get_by_name("battery");
#if (LINUX_VERSION_CODE < KERNEL_VERSION(4, 4, 0))
	if (psy && psy->get_property) {
		// battery temperatrue
		psy->get_property(psy, POWER_SUPPLY_PROP_TEMP, &psp);
		tempr = DIV_ROUND_CLOSEST(psp.intval, 10);
		psy->get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &psp);
		vbat = DIV_ROUND_CLOSEST(psp.intval, 1000);
	}
#else
	if (psy && psy->desc && psy->desc->get_property) {
		// battery temperatrue
		psy->desc->get_property(psy, POWER_SUPPLY_PROP_TEMP, &psp);
		tempr = DIV_ROUND_CLOSEST(psp.intval, 10);
		psy->desc->get_property(psy, POWER_SUPPLY_PROP_VOLTAGE_NOW, &psp);
		vbat = DIV_ROUND_CLOSEST(psp.intval, 1000);
	}
#endif
	pr_info("vbat:%d, tempr:%d", vbat, tempr);

	return tempr;
}

void *fsm_devm_kstrdup(struct device *dev, void *buf, size_t size)
{
	char *devm_buf = devm_kzalloc(dev, size + 1, GFP_KERNEL);

	if (!devm_buf) {
		return devm_buf;
	}
	memcpy(devm_buf, buf, size);

	return devm_buf;
}

static int fsm_set_irq(fsm_dev_t *fsm_dev, bool enable)
{
	if (!fsm_dev || fsm_dev->irq_id <= 0) {
		return -EINVAL;
	}
	if (enable)
		enable_irq(fsm_dev->irq_id);
	else
		disable_irq(fsm_dev->irq_id);

	return 0;
}

int fsm_set_monitor(fsm_dev_t *fsm_dev, bool enable)
{
	if (!fsm_dev || !fsm_dev->fsm_wq) {
		return -EINVAL;
	}
	if (fsm_dev->use_irq) {
		return fsm_set_irq(fsm_dev, enable);
	}
	if (enable) {
		queue_delayed_work(fsm_dev->fsm_wq,
				&fsm_dev->monitor_work, 2*HZ);
	}
	else {
		if (delayed_work_pending(&fsm_dev->monitor_work)) {
			cancel_delayed_work_sync(&fsm_dev->monitor_work);
		}
	}

	return 0;
}

static int fsm_ext_reset(fsm_dev_t *fsm_dev)
{
	fsm_config_t *cfg = fsm_get_config();

	if (cfg && cfg->reset_chip) {
		return 0;
	}
	if (fsm_dev && gpio_is_valid(fsm_dev->rst_gpio)) {
		gpio_set_value_cansleep(fsm_dev->rst_gpio, 0);
		fsm_delay_ms(10); // mdelay
		gpio_set_value_cansleep(fsm_dev->rst_gpio, 1);
		fsm_delay_ms(1); // mdelay
		cfg->reset_chip = true;
	}

	return 0;
}

static irqreturn_t fsm_irq_hander(int irq, void *data)
{
	fsm_dev_t *fsm_dev = data;

	queue_delayed_work(fsm_dev->fsm_wq, &fsm_dev->interrupt_work, 0);

	return IRQ_HANDLED;
}

static void fsm_work_monitor(struct work_struct *work)
{
	fsm_config_t *cfg = fsm_get_config();
	fsm_dev_t *fsm_dev;
	int ret;

	fsm_dev = container_of(work, struct fsm_dev, monitor_work.work);
	if (!cfg || cfg->skip_monitor || !fsm_dev) {
		return;
	}
	fsm_mutex_lock();
	ret = fsm_dev_recover(fsm_dev);
	fsm_get_spkr_tempr(fsm_dev);
	fsm_mutex_unlock();
	if (fsm_dev->rec_count >= 5) { // 5 time max
		pr_addr(warning, "recover max time, stop it");
		return;
	}
	/* reschedule */
	queue_delayed_work(fsm_dev->fsm_wq, &fsm_dev->monitor_work,
			2*HZ);

}

static void fsm_work_interrupt(struct work_struct *work)
{
	fsm_config_t *cfg = fsm_get_config();
	fsm_dev_t *fsm_dev;
	int ret;

	fsm_dev = container_of(work, struct fsm_dev, interrupt_work.work);
	if (!cfg || cfg->skip_monitor || !fsm_dev) {
		return;
	}
	fsm_mutex_lock();
	ret = fsm_dev_recover(fsm_dev);
	fsm_get_spkr_tempr(fsm_dev);

	fsm_mutex_unlock();
}

static int fsm_request_irq(fsm_dev_t *fsm_dev)
{
	struct i2c_client *i2c;
	int irq_flags;
	int ret;

	if (fsm_dev == NULL || fsm_dev->i2c == NULL) {
		return -EINVAL;
	}
	fsm_dev->irq_id = -1;
	if (!fsm_dev->use_irq || !gpio_is_valid(fsm_dev->irq_gpio)) {
		pr_addr(info, "skip to request irq");
		return 0;
	}
	i2c = fsm_dev->i2c;
	/* register irq handler */
	fsm_dev->irq_id = gpio_to_irq(fsm_dev->irq_gpio);
	if (fsm_dev->irq_id <= 0) {
		dev_err(&i2c->dev, "invalid irq %d\n", fsm_dev->irq_id);
		return -EINVAL;
	}
	irq_flags = IRQF_TRIGGER_FALLING | IRQF_ONESHOT;
	ret = devm_request_threaded_irq(&i2c->dev, fsm_dev->irq_id,
				NULL, fsm_irq_hander, irq_flags, "fs16xx", fsm_dev);
	if (ret) {
		dev_err(&i2c->dev, "failed to request IRQ %d: %d\n",
				fsm_dev->irq_id, ret);
		return ret;
	}
	disable_irq(fsm_dev->irq_id);

	return 0;
}

#ifdef CONFIG_OF
static int fsm_parse_dts(struct i2c_client *i2c, fsm_dev_t *fsm_dev)
{
	struct device_node *np = i2c->dev.of_node;
	char const *position;
	int ret;

	if (fsm_dev == NULL || np == NULL) {
		return -EINVAL;
	}

	fsm_dev->rst_gpio = of_get_named_gpio(np, "fsm,rst-gpio", 0);
	if (gpio_is_valid(fsm_dev->rst_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, fsm_dev->rst_gpio,
			GPIOF_OUT_INIT_LOW, "FS16XX_RST");
		if (ret)
			return ret;
	}
	fsm_dev->irq_gpio = of_get_named_gpio(np, "fsm,irq-gpio", 0);
	if (gpio_is_valid(fsm_dev->irq_gpio)) {
		ret = devm_gpio_request_one(&i2c->dev, fsm_dev->irq_gpio,
			GPIOF_OUT_INIT_LOW, "FS16XX_IRQ");
		if (ret)
			return ret;
	}

	if (of_property_read_string(np, "fsm,position", &position)) {
		fsm_dev->pos_mask = FSM_POS_MONO; // mono
		return 0;
	}
	if (!strcmp(position, "LTOP")) {
		fsm_dev->pos_mask = FSM_POS_LTOP;
	}
	else if (!strcmp(position, "RBTM")) {
		fsm_dev->pos_mask = FSM_POS_RBTM;
	}
	else if (!strcmp(position, "LBTM")) {
		fsm_dev->pos_mask = FSM_POS_LBTM;
	}
	else if (!strcmp(position, "RTOP")) {
		fsm_dev->pos_mask = FSM_POS_RTOP;
	}
	else {
		fsm_dev->pos_mask = FSM_POS_MONO;
	}

	return 0;
}
/*
static struct of_device_id fsm_match_tbl[] =
{
	{ .compatible = "foursemi,fs16xx" },
	{},
};
MODULE_DEVICE_TABLE(of, fsm_match_tbl);
*/
#endif

int fsm_i2c_probe(struct i2c_client *i2c,
			const struct i2c_device_id *id)
{
	fsm_config_t *cfg = fsm_get_config();
	fsm_dev_t *fsm_dev;
	int ret;

	pr_debug("enter");
	if (!i2c_check_functionality(i2c->adapter, I2C_FUNC_I2C)) {
		dev_err(&i2c->dev, "check I2C_FUNC_I2C failed");
		return -EIO;
	}

	fsm_dev = devm_kzalloc(&i2c->dev, sizeof(fsm_dev_t), GFP_KERNEL);
	if (fsm_dev == NULL) {
		dev_err(&i2c->dev, "alloc memory fialed");
		return -ENOMEM;
	}

	memset(fsm_dev, 0, sizeof(fsm_dev_t));
	mutex_init(&fsm_dev->i2c_lock);
	fsm_dev->i2c = i2c;

#ifdef CONFIG_OF
	ret = fsm_parse_dts(i2c, fsm_dev);
	if (ret) {
		dev_err(&i2c->dev, "failed to parse DTS node");
	}
#endif
#if defined(CONFIG_FSM_REGMAP)
	fsm_dev->regmap = fsm_regmap_i2c_init(i2c);
	if (fsm_dev->regmap == NULL) {
		devm_kfree(&i2c->dev, fsm_dev);
		dev_err(&i2c->dev, "regmap init fialed");
		return -EINVAL;
	}
#endif

	fsm_vddd_on(&i2c->dev);
	fsm_ext_reset(fsm_dev);
	ret = fsm_probe(fsm_dev, i2c->addr);
	if (ret) {
		dev_err(&i2c->dev, "detect device failed");
#if defined(CONFIG_FSM_REGMAP)
		fsm_regmap_i2c_deinit(fsm_dev->regmap);
#endif
		devm_kfree(&i2c->dev, fsm_dev);
		return ret;
	}
	fsm_dev->id = cfg->dev_count - 1;
	i2c_set_clientdata(i2c, fsm_dev);
	pr_addr(info, "index:%d", fsm_dev->id);
	fsm_dev->fsm_wq = create_singlethread_workqueue("fs16xx");
	INIT_DELAYED_WORK(&fsm_dev->monitor_work, fsm_work_monitor);
	INIT_DELAYED_WORK(&fsm_dev->interrupt_work, fsm_work_interrupt);
	fsm_request_irq(fsm_dev);

	if(fsm_dev->id == 0) {
		// reigster only in the first device
		fsm_set_pdev(&i2c->dev);
		fsm_misc_init();
		fsm_proc_init();
		fsm_sysfs_init(&i2c->dev);
		fsm_codec_register(&i2c->dev, fsm_dev->id);
	}

	dev_info(&i2c->dev, "i2c probe completed");

	return 0;
}

int fsm_i2c_remove(struct i2c_client *i2c)
{
	fsm_dev_t *fsm_dev = i2c_get_clientdata(i2c);

	pr_debug("enter");
	if (fsm_dev->fsm_wq) {
		cancel_delayed_work_sync(&fsm_dev->interrupt_work);
		cancel_delayed_work_sync(&fsm_dev->monitor_work);
		destroy_workqueue(fsm_dev->fsm_wq);
	}
#if defined(CONFIG_FSM_REGMAP)
	fsm_regmap_i2c_deinit(fsm_dev->regmap);
#endif
	if (fsm_dev->id == 0) {
		fsm_codec_unregister(&i2c->dev);
		fsm_sysfs_deinit(&i2c->dev);
		fsm_proc_deinit();
		fsm_misc_deinit();
		fsm_set_pdev(NULL);
	}
	fsm_remove(fsm_dev);
	fsm_vddd_off();
	if (gpio_is_valid(fsm_dev->irq_gpio)) {
		devm_gpio_free(&i2c->dev, fsm_dev->irq_gpio);
	}
	if (gpio_is_valid(fsm_dev->rst_gpio)) {
		devm_gpio_free(&i2c->dev, fsm_dev->rst_gpio);
	}
	if (fsm_dev) {
		devm_kfree(&i2c->dev, fsm_dev);
	}
	dev_info(&i2c->dev, "i2c removed");

	return 0;
}
/*
static const struct i2c_device_id fsm_i2c_id[] =
{
	{ "fs16xx", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, fsm_i2c_id);

static struct i2c_driver fsm_i2c_driver =
{
	.driver = {
		.name  = FSM_DRV_NAME,
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = of_match_ptr(fsm_match_tbl),
#endif
	},
	.probe    = fsm_i2c_probe,
	.remove   = fsm_i2c_remove,
	.id_table = fsm_i2c_id,
};

int exfsm_i2c_probe(struct i2c_client *i2c,
			const struct i2c_device_id *id)
{
	return fsm_i2c_probe(i2c, id);
}
EXPORT_SYMBOL(exfsm_i2c_probe);

int exfsm_i2c_remove(struct i2c_client *i2c)
{
	return fsm_i2c_remove(i2c);
}
EXPORT_SYMBOL(exfsm_i2c_remove);

int fsm_i2c_init(void)
{
	return i2c_add_driver(&fsm_i2c_driver);
}

void fsm_i2c_exit(void)
{
	pr_info("enter");
	i2c_del_driver(&fsm_i2c_driver);
}
*/
#ifdef CONFIG_FSM_STUB
static int fsm_plat_probe(struct platform_device *pdev)
{
	int ret;

	if (0) { //(pdev->dev.of_node) {
		dev_set_name(&pdev->dev, "%s", "fsm-codec-stub");
	}
	pr_info("dev_name: %s", dev_name(&pdev->dev));
	fsm_vddd_on(&pdev->dev);
	fsm_set_pdev(&pdev->dev);
	ret = fsm_codec_register(&pdev->dev, 0);
//	ret = fsm_i2c_init();
	if (ret) {
		pr_err("i2c init failed: %d", ret);
		fsm_codec_unregister(&pdev->dev);
		return ret;
	}

	return 0;
}

static int fsm_plat_remove(struct platform_device *pdev)
{
	pr_debug("enter");
	fsm_codec_unregister(&pdev->dev);
//	fsm_i2c_exit();
	fsm_vddd_off();
	dev_info(&pdev->dev, "platform removed");

	return 0;
}

#ifdef CONFIG_OF
static struct of_device_id fsm_codec_stub_dt_match[] = {
	{ .compatible = "foursemi,fsm-codec-stub" },
	{},
};
MODULE_DEVICE_TABLE(of, fsm_codec_stub_dt_match);
#else
static struct platform_device *soc_fsm_device;
#endif

static struct platform_driver soc_fsm_driver = {
	.driver = {
		.name = "fsm-codec-stub",
		.owner = THIS_MODULE,
#ifdef CONFIG_OF
		.of_match_table = fsm_codec_stub_dt_match,
#endif
	},
	.probe = fsm_plat_probe,
	.remove = fsm_plat_remove,
};

static int fsm_stub_init(void)
{
	int ret;

#ifndef CONFIG_OF
	// soc_fsm_device = platform_device_alloc("fsm-codec-stub", -1);
	soc_fsm_device = platform_device_register_simple("fsm-codec-stub",
				-1, NULL, 0);
	if (IS_ERR(soc_fsm_device)) {
		pr_err("register device failed");
		// return -ENOMEM;
		return PTR_ERR(soc_fsm_device);
	}

	ret = platform_device_add(soc_fsm_device);
	if (ret != 0) {
		platform_device_put(soc_fsm_device);
		return ret;
	}
#endif
	ret = platform_driver_register(&soc_fsm_driver);
	if (ret) {
		pr_err("register driver failed: %d", ret);
	}

	return ret;
}

static void fsm_stub_exit(void)
{
#ifndef CONFIG_OF
	if (!IS_ERR(soc_fsm_device)) {
		platform_device_unregister(soc_fsm_device);
	}
#endif
	platform_driver_unregister(&soc_fsm_driver);
}
#endif // CONFIG_FSM_STUB

static int __init fsm_mod_init(void)
{
#if 0
	int ret;

#ifdef CONFIG_FSM_STUB
	ret = fsm_stub_init();
#else
	ret = fsm_i2c_init();
#endif
	if (ret) {
		pr_err("init fail: %d", ret);
		return ret;
	}
#endif
	return 0;
}

static void __exit fsm_mod_exit(void)
{
#ifdef CONFIG_FSM_STUB
	fsm_stub_exit();
#else
//	fsm_i2c_exit();
#endif
}

//module_i2c_driver(fsm_i2c_driver);
module_init(fsm_mod_init);
module_exit(fsm_mod_exit);

MODULE_AUTHOR("FourSemi SW <support@foursemi.com>");
MODULE_DESCRIPTION("FourSemi Smart PA Driver");
MODULE_VERSION(FSM_CODE_VERSION);
MODULE_ALIAS("foursemi:"FSM_DRV_NAME);
MODULE_LICENSE("GPL");

