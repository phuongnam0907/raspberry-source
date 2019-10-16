/*
 * SRF05: ultrasonic sensor for distance measuring by using GPIOs
 *
 * Copyright (c) 2017 Andreas Klinger <ak@it-klinger.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For details about the device see:
 * http://www.robot-electronics.co.uk/htm/srf05tech.htm
 *
 * the measurement cycle as timing diagram looks like:
 *
 *          +---+
 * GPIO     |   |
 * trig:  --+   +------------------------------------------------------
 *          ^   ^
 *          |<->|
 *         udelay(10)
 *
 * ultra           +-+ +-+ +-+
 * sonic           | | | | | |
 * burst: ---------+ +-+ +-+ +-----------------------------------------
 *                           .
 * ultra                     .              +-+ +-+ +-+
 * sonic                     .              | | | | | |
 * echo:  ----------------------------------+ +-+ +-+ +----------------
 *                           .                        .
 *                           +------------------------+
 * GPIO                      |                        |
 * echo:  -------------------+                        +---------------
 *                           ^                        ^
 *                           interrupt                interrupt
 *                           (ts_rising)              (ts_falling)
 *                           |<---------------------->|
 *                              pulse time measured
 *                              --> one round trip of ultra sonic waves
 */
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/delay.h>
#include <linux/iio/iio.h>
#include <linux/iio/sysfs.h>
#include <linux/ioctl.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/kdev_t.h>

#define IOCTL_APP_TYPE 71
#define READ_VALUE _IOR(IOCTL_APP_TYPE,2,int32_t*)

#define MAX_DEV 1
#define FIRST_MINOR 0

dev_t dev_num_t;
static struct cdev cdev;
static struct class *srf05_class;

struct srf05_data {
	struct device		*dev;
	struct gpio_desc	*gpiod_trig;
	struct gpio_desc	*gpiod_echo;
	struct mutex		lock;
	int			irqnr;
	ktime_t			ts_rising;
	ktime_t			ts_falling;
	struct completion	rising;
	struct completion	falling;
};

int32_t value = 0;
int rets = 0;
int rer;
int p_val;
int val2;

struct iio_dev *indio_dev;
struct iio_chan_spec const *channel;

static int srf05_open(struct inode *inode, struct file *file);
static int srf05_release(struct inode *inode, struct file *file);
static long srf05_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open       = srf05_open,
    .release    = srf05_release,
    .unlocked_ioctl = srf05_ioctl,
};

static irqreturn_t srf05_handle_irq(int irq, void *dev_id)
{
	struct iio_dev *indio_dev = dev_id;
	struct srf05_data *data = iio_priv(indio_dev);
	ktime_t now = ktime_get();

	if (gpiod_get_value(data->gpiod_echo)) {
		data->ts_rising = now;
		complete(&data->rising);
	} else {
		data->ts_falling = now;
		complete(&data->falling);
	}

	return IRQ_HANDLED;
}

static int srf05_read(struct srf05_data *data)
{
	int ret;
	ktime_t ktime_dt;
	u64 dt_ns;
	u32 time_ns, distance_mm;

	/*
	 * just one read-echo-cycle can take place at a time
	 * ==> lock against concurrent reading calls
	 */
	mutex_lock(&data->lock);

	reinit_completion(&data->rising);
	reinit_completion(&data->falling);

	gpiod_set_value(data->gpiod_trig, 1);
	udelay(10);
	gpiod_set_value(data->gpiod_trig, 0);

	/* it cannot take more than 30 ms */
	ret = wait_for_completion_killable_timeout(&data->rising, HZ/33.35);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	} else if (ret == 0) {
		mutex_unlock(&data->lock);
		return -ETIMEDOUT;
	}

	ret = wait_for_completion_killable_timeout(&data->falling, HZ/33.35);
	if (ret < 0) {
		mutex_unlock(&data->lock);
		return ret;
	} else if (ret == 0) {
		mutex_unlock(&data->lock);
		return -ETIMEDOUT;
	}

	ktime_dt = ktime_sub(data->ts_falling, data->ts_rising);

	mutex_unlock(&data->lock);

	dt_ns = ktime_to_ns(ktime_dt);
	/*
	 * measuring more than 4 meters is beyond the capabilities of
	 * the sensor
	 * ==> filter out invalid results for not measuring echos of
	 *     another us sensor
	 *
	 * formula:
	 *         distance       4 m
	 * time = ---------- = --------- = 12539185 ns
	 *          speed       319 m/s
	 *
	 * using a minimum speed at -20 °C of 319 m/s
	 */
	if (dt_ns > 12539185)
		return -EIO;

	time_ns = dt_ns;

	/*
	 * the speed as function of the temperature is approximately:
	 *
	 * speed = 331,5 + 0,6 * Temp
	 *   with Temp in °C
	 *   and speed in m/s
	 *
	 * use 343 m/s as ultrasonic speed at 27 °C here in absence of the
	 * temperature
	 *
	 * therefore:
	 *             time     347
	 * distance = ------ * -----
	 *             10^6       2
	 *   with time in ns
	 *   and distance in mm (one way)
	 *
	 * because we limit to 4 meters the multiplication with 347 just
	 * fits into 32 bit
	 */
	distance_mm = time_ns * 347 / 2000000;

	return distance_mm;
}

static int srf05_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *channel, int *val,
			    int *val2, long info)
{
	struct srf05_data *data = iio_priv(indio_dev);
	int ret;

	if (channel->type != IIO_DISTANCE)
		return -EINVAL;
	switch (info) {
	case IIO_CHAN_INFO_RAW:
		val2 = NULL;
		val = NULL;
		ret = srf05_read(data);
		if (ret < 0) 
			return -1;
		else
			return ret;
	case IIO_CHAN_INFO_SCALE:
		/*
		 * theoretical maximum resolution is 3 mm
		 * 1 LSB is 1 mm
		 */
		*val = 0;
		*val2 = 1000;
		return IIO_VAL_INT_PLUS_MICRO;
	default:
		return -EINVAL;
	}
}

static const struct iio_info srf05_iio_info = {
	.read_raw		= srf05_read_raw,
};

static const struct iio_chan_spec srf05_chan_spec[] = {
	{
		.type = IIO_DISTANCE,
		.info_mask_separate =
				BIT(IIO_CHAN_INFO_RAW) |
				BIT(IIO_CHAN_INFO_SCALE),
	},
};

static int srf05_open(struct inode *inode, struct file *file)
{
    printk("SRF05: Device open\n");
    return 0;
}

static int srf05_release(struct inode *inode, struct file *file)
{
    printk("SRF05: Device close\n");
    return 0;
}

static long srf05_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    // printk("SRF05: Device ioctl\n");
	rets = srf05_read_raw(indio_dev,srf05_chan_spec,&p_val,&val2,IIO_CHAN_INFO_RAW);
	rer = copy_to_user((int32_t*) arg, &rets, sizeof(rets));
    return rets;
}

static int srf05_uevent(struct device *dev, struct kobj_uevent_env *env)
{
    add_uevent_var(env, "DEVMODE=%#o", 0666);
    return 0;
}

static int srf05_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct srf05_data *data;
	// struct iio_dev *indio_dev;
	int ret;

	indio_dev = devm_iio_device_alloc(dev, sizeof(struct srf05_data));
	if (!indio_dev) {
		dev_err(dev, "failed to allocate IIO device\n");
		return -ENOMEM;
	}

	data = iio_priv(indio_dev);
	data->dev = dev;

	/*Allocating Major number*/
	ret = alloc_chrdev_region(&dev_num_t, FIRST_MINOR, MAX_DEV, "srf05");
	if (ret){
        dev_err(dev, "Can't register driver, error code: %d \n", ret); 
        return -1;
    } else printk(KERN_INFO "SRF05: Major = %d Minor = %d \n", MAJOR(dev_num_t), MINOR(dev_num_t));

	/*Creating cdev structure*/
	cdev_init(&cdev, &fops);
	cdev.owner = THIS_MODULE;

	/*Adding character device to the system*/
	ret = cdev_add(&cdev, dev_num_t, 1);
	if(ret)
	{
		printk(KERN_INFO "Cannot add the device to the system\n");
		goto r_class;
	}
	
	/*Creating struct class*/
	if((srf05_class = class_create(THIS_MODULE,"srf05")) == NULL)
	{
		printk(KERN_INFO "Cannot create the struct class\n");
		goto r_class;
	}
	srf05_class->dev_uevent = srf05_uevent;

	/*Creating device*/
	if((device_create(srf05_class,NULL,dev_num_t,NULL,"srf05")) == NULL){
		printk(KERN_INFO "Cannot create the Device srf05\n");
		goto r_device;
	} else printk(KERN_INFO "SRF05: Device Driver Insert...Done!!!\n");


	mutex_init(&data->lock);
	init_completion(&data->rising);
	init_completion(&data->falling);

	data->gpiod_trig = devm_gpiod_get(dev, "trig", GPIOD_OUT_LOW);
	if (IS_ERR(data->gpiod_trig)) {
		dev_err(dev, "failed to get trig-gpios: err=%ld\n",
					PTR_ERR(data->gpiod_trig));
		return PTR_ERR(data->gpiod_trig);
	}

	data->gpiod_echo = devm_gpiod_get(dev, "echo", GPIOD_IN);
	if (IS_ERR(data->gpiod_echo)) {
		dev_err(dev, "failed to get echo-gpios: err=%ld\n",
					PTR_ERR(data->gpiod_echo));
		return PTR_ERR(data->gpiod_echo);
	}

	if (gpiod_cansleep(data->gpiod_echo)) {
		dev_err(data->dev, "cansleep-GPIOs not supported\n");
		return -ENODEV;
	}

	data->irqnr = gpiod_to_irq(data->gpiod_echo);
	if (data->irqnr < 0) {
		dev_err(data->dev, "gpiod_to_irq: %d\n", data->irqnr);
		return data->irqnr;
	}

	ret = devm_request_irq(dev, data->irqnr, srf05_handle_irq,
			IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING,
			pdev->name, indio_dev);
	if (ret < 0) {
		dev_err(data->dev, "request_irq: %d\n", ret);
		return ret;
	}

	platform_set_drvdata(pdev, indio_dev);

	indio_dev->name = "srf05";
	indio_dev->dev.parent = &pdev->dev;
	indio_dev->info = &srf05_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = srf05_chan_spec;
	indio_dev->num_channels = ARRAY_SIZE(srf05_chan_spec);

	return devm_iio_device_register(dev, indio_dev);

r_device:
        class_destroy(srf05_class);
r_class:
        unregister_chrdev_region(dev_num_t,1);
        return -1;
}

static int srf05_remove(struct platform_device *pdev)
{
	struct srf05_data *data = platform_get_drvdata(pdev);
	gpiod_put(data->gpiod_echo);
	gpiod_put(data->gpiod_trig);
    device_destroy(srf05_class, dev_num_t);
    class_unregister(srf05_class);
    class_destroy(srf05_class);
	cdev_del(&cdev);
	kfree(data);
    unregister_chrdev_region(dev_num_t, MINORMASK);

	return 0;
}

static const struct of_device_id of_srf05_match[] = {
	{ .compatible = "devantech,srf05", },
	{},
};

MODULE_DEVICE_TABLE(of, of_srf05_match);

static struct platform_driver srf05_driver = {
	.probe		= srf05_probe,
	.remove		= srf05_remove,
	.driver		= {
		.name		= "srf05-gpio",
		.of_match_table	= of_srf05_match,
	},
};

module_platform_driver(srf05_driver);

MODULE_AUTHOR("Le Phuong Nam <le.phuong.nam@styl.solutions>");
MODULE_DESCRIPTION("SRF05 ultrasonic sensor for distance measuring using GPIOs - IOCTL");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:srf05");
