// SPDX-License-Identifier: GPL-2.0
// SPI driven IR LED device driver
//
// Copyright (c) 2016 Samsung Electronics Co., Ltd.
// Copyright (c) Andi Shyti <andi@etezian.org>

#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/kref.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <uapi/linux/lirc.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#include <linux/spi/spi.h>
#include <media/rc-core.h>

#define IR_SPI_DRIVER_NAME		"ir-spi"

#define IR_SPI_DEFAULT_FREQUENCY	38000
#define IR_SPI_MAX_BUFSIZE		32768

/* Xiaomi HAL sends an already modulated bitstream at 1.92 MHz. */
#define IR_SPI_LEGACY_FREQUENCY		1920000
#define IR_SPI_LEGACY_MAX_BUFSIZE	524288

struct ir_spi_data {
	u32 freq;
	bool negated;

	u16 tx_buf[IR_SPI_MAX_BUFSIZE];
	u16 pulse;
	u16 space;

	struct rc_dev *rc;
	struct spi_device *spi;
	struct regulator *regulator;
	struct miscdevice legacy;
	struct mutex lock;
	struct kref ref;
};

struct ir_spi_legacy_file {
	struct ir_spi_data *idata;
	u32 length;
};

static void ir_spi_free(struct kref *ref)
{
	struct ir_spi_data *idata = container_of(ref, struct ir_spi_data, ref);

	kfree(idata);
}

static void ir_spi_release_data(void *data)
{
	struct ir_spi_data *idata = data;

	kref_put(&idata->ref, ir_spi_free);
}

/* Both interfaces serialize transfers, including device removal. */
static int ir_spi_transfer(struct ir_spi_data *idata,
			   struct spi_transfer *xfer)
{
	int ret;

	if (!idata->spi)
		return -ENODEV;

	ret = regulator_enable(idata->regulator);
	if (ret)
		return ret;

	ret = spi_sync_transfer(idata->spi, xfer, 1);
	if (ret)
		dev_err(&idata->spi->dev, "unable to deliver the signal\n");

	regulator_disable(idata->regulator);
	return ret;
}

static int ir_spi_legacy_open(struct inode *inode, struct file *file)
{
	struct miscdevice *misc = file->private_data;
	struct ir_spi_data *idata = container_of(misc, struct ir_spi_data, legacy);
	struct ir_spi_legacy_file *ctx;
	int ret = 0;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	mutex_lock(&idata->lock);
	if (!idata->spi) {
		ret = -ENODEV;
	} else {
		kref_get(&idata->ref);
		ctx->idata = idata;
		file->private_data = ctx;
	}
	mutex_unlock(&idata->lock);

	if (ret)
		kfree(ctx);
	return ret;
}

static int ir_spi_legacy_close(struct inode *inode, struct file *file)
{
	struct ir_spi_legacy_file *ctx = file->private_data;

	kref_put(&ctx->idata->ref, ir_spi_free);
	kfree(ctx);
	return 0;
}

static long ir_spi_legacy_ioctl(struct file *file, unsigned int cmd,
			       unsigned long arg)
{
	struct ir_spi_legacy_file *ctx = file->private_data;
	u32 length;
	int ret = 0;

	/* The Xiaomi ABI uses SEND_MODE as a byte-buffer length. */
	if (cmd != LIRC_SET_SEND_MODE)
		return -EINVAL;
	if (get_user(length, (u32 __user *)arg))
		return -EFAULT;
	if (length > IR_SPI_LEGACY_MAX_BUFSIZE || length % sizeof(u32))
		return -EINVAL;

	mutex_lock(&ctx->idata->lock);
	if (!ctx->idata->spi)
		ret = -ENODEV;
	else
		ctx->length = length;
	mutex_unlock(&ctx->idata->lock);
	return ret;
}

static ssize_t ir_spi_legacy_write(struct file *file, const char __user *buf,
				  size_t length, loff_t *offset)
{
	struct ir_spi_legacy_file *ctx = file->private_data;
	struct spi_transfer xfer = {
		.speed_hz = IR_SPI_LEGACY_FREQUENCY,
		.bits_per_word = 32,
		.len = length,
	};
	void *data;
	int ret;

	if (!length || length > IR_SPI_LEGACY_MAX_BUFSIZE ||
	    length % sizeof(u32))
		return -EINVAL;

	data = memdup_user(buf, length);
	if (IS_ERR(data))
		return PTR_ERR(data);
	xfer.tx_buf = data;

	mutex_lock(&ctx->idata->lock);
	if (ctx->length && ctx->length != length)
		ret = -EINVAL;
	else
		ret = ir_spi_transfer(ctx->idata, &xfer);
	mutex_unlock(&ctx->idata->lock);
	kfree(data);

	return ret ? ret : length;
}

static const struct file_operations ir_spi_legacy_fops = {
	.owner = THIS_MODULE,
	.open = ir_spi_legacy_open,
	.release = ir_spi_legacy_close,
	.write = ir_spi_legacy_write,
	.unlocked_ioctl = ir_spi_legacy_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ir_spi_legacy_ioctl,
#endif
	.llseek = no_llseek,
};

static int ir_spi_tx(struct rc_dev *dev,
		     unsigned int *buffer, unsigned int count)
{
	int i;
	int ret;
	unsigned int len = 0;
	struct ir_spi_data *idata = dev->priv;
	struct spi_transfer xfer;

	mutex_lock(&idata->lock);

	/* convert the pulse/space signal to raw binary signal */
	for (i = 0; i < count; i++) {
		unsigned int periods;
		int j;
		u16 val;

		periods = DIV_ROUND_CLOSEST(buffer[i] * idata->freq, 1000000);

		if (len + periods >= IR_SPI_MAX_BUFSIZE) {
			ret = -EINVAL;
			goto out_unlock;
		}

		/*
		 * the first value in buffer is a pulse, so that 0, 2, 4, ...
		 * contain a pulse duration. On the contrary, 1, 3, 5, ...
		 * contain a space duration.
		 */
		val = (i % 2) ? idata->space : idata->pulse;
		for (j = 0; j < periods; j++)
			idata->tx_buf[len++] = val;
	}

	memset(&xfer, 0, sizeof(xfer));

	xfer.speed_hz = idata->freq * 16;
	xfer.len = len * sizeof(*idata->tx_buf);
	xfer.tx_buf = idata->tx_buf;

	ret = ir_spi_transfer(idata, &xfer);
out_unlock:
	mutex_unlock(&idata->lock);
	return ret ? ret : count;
}

static int ir_spi_set_tx_carrier(struct rc_dev *dev, u32 carrier)
{
	struct ir_spi_data *idata = dev->priv;

	if (!carrier)
		return -EINVAL;

	idata->freq = carrier;

	return 0;
}

static int ir_spi_set_duty_cycle(struct rc_dev *dev, u32 duty_cycle)
{
	struct ir_spi_data *idata = dev->priv;
	int bits = (duty_cycle * 15) / 100;

	idata->pulse = GENMASK(bits, 0);

	if (idata->negated) {
		idata->pulse = ~idata->pulse;
		idata->space = 0xffff;
	} else {
		idata->space = 0;
	}

	return 0;
}

static int ir_spi_probe(struct spi_device *spi)
{
	int ret;
	u8 dc;
	struct ir_spi_data *idata;

	idata = kzalloc(sizeof(*idata), GFP_KERNEL);
	if (!idata)
		return -ENOMEM;

	mutex_init(&idata->lock);
	kref_init(&idata->ref);
	ret = devm_add_action_or_reset(&spi->dev, ir_spi_release_data, idata);
	if (ret)
		return ret;

	idata->regulator = devm_regulator_get(&spi->dev, "irda_regulator");
	if (IS_ERR(idata->regulator))
		return PTR_ERR(idata->regulator);

	idata->rc = devm_rc_allocate_device(&spi->dev, RC_DRIVER_IR_RAW_TX);
	if (!idata->rc)
		return -ENOMEM;

	idata->rc->tx_ir           = ir_spi_tx;
	idata->rc->s_tx_carrier    = ir_spi_set_tx_carrier;
	idata->rc->s_tx_duty_cycle = ir_spi_set_duty_cycle;
	idata->rc->device_name	   = "IR SPI";
	idata->rc->driver_name     = IR_SPI_DRIVER_NAME;
	idata->rc->priv            = idata;
	idata->spi                 = spi;

	idata->negated = of_property_read_bool(spi->dev.of_node,
							"led-active-low");
	ret = of_property_read_u8(spi->dev.of_node, "duty-cycle", &dc);
	if (ret)
		dc = 50;

	/* ir_spi_set_duty_cycle cannot fail,
	 * it returns int to be compatible with the
	 * rc->s_tx_duty_cycle function
	 */
	ir_spi_set_duty_cycle(idata->rc, dc);

	idata->freq = IR_SPI_DEFAULT_FREQUENCY;

	idata->legacy.minor = MISC_DYNAMIC_MINOR;
	idata->legacy.name = "ir_spi";
	idata->legacy.fops = &ir_spi_legacy_fops;
	idata->legacy.parent = &spi->dev;
	ret = misc_register(&idata->legacy);
	if (ret)
		return ret;

	ret = devm_rc_register_device(&spi->dev, idata->rc);
	if (ret) {
		misc_deregister(&idata->legacy);
		mutex_lock(&idata->lock);
		idata->spi = NULL;
		mutex_unlock(&idata->lock);
		return ret;
	}

	spi_set_drvdata(spi, idata);
	return 0;
}

static int ir_spi_remove(struct spi_device *spi)
{
	struct ir_spi_data *idata = spi_get_drvdata(spi);

	misc_deregister(&idata->legacy);
	mutex_lock(&idata->lock);
	idata->spi = NULL;
	mutex_unlock(&idata->lock);
	return 0;
}

static const struct of_device_id ir_spi_of_match[] = {
	{ .compatible = "ir-spi-led" },
	{ .compatible = "ir-spi" },
	{},
};
MODULE_DEVICE_TABLE(of, ir_spi_of_match);

static struct spi_driver ir_spi_driver = {
	.probe = ir_spi_probe,
	.remove = ir_spi_remove,
	.driver = {
		.name = IR_SPI_DRIVER_NAME,
		.of_match_table = ir_spi_of_match,
	},
};

module_spi_driver(ir_spi_driver);

MODULE_AUTHOR("Andi Shyti <andi@etezian.org>");
MODULE_DESCRIPTION("SPI IR LED");
MODULE_LICENSE("GPL v2");
