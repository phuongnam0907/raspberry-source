#include <stddef.h>
#include <linux/printk.h>   
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/module.h>
#include <linux/device.h>  
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/mutex.h>
#include<linux/uaccess.h>
#include <linux/kthread.h>
#include <linux/sched.h>  
#include <linux/kdev_t.h>
#include <linux/fs.h>
#include <linux/cdev.h>

#define PDEBUG(fmt,args...) printk(KERN_DEBUG"%s: "fmt,DRIVER_NAME, ##args)
#define PERR(fmt,args...) printk(KERN_ERR"%s: "fmt,DRIVER_NAME,##args)
#define PINFO(fmt,args...) printk(KERN_INFO"%s: "fmt,DRIVER_NAME, ##args)

#define DRIVER_NAME "reset-gpio"
#define BUFF_SIZE 5000
#define FIRST_MINOR 0

dev_t device_num ;
struct class * device_class;
struct device * device;
struct gpio_desc *reset;
static struct cdev cdev;
static struct task_struct *thread;

int res;
int count;
bool thread_run = false;

typedef struct privatedata {
    bool isp_mode ;
	struct mutex lock;
} private_data_t;
private_data_t *data;

static int driver_probe(struct platform_device *pdev);
static int driver_remove(struct platform_device *pdev);

static const struct of_device_id reset_dst[]={
    { .compatible = DRIVER_NAME },
    {}
};

static struct file_operations fops =
{
    .owner = THIS_MODULE,
};

MODULE_DEVICE_TABLE(of, reset_dst);	

static struct platform_driver reset_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .owner = THIS_MODULE,   
        .of_match_table = of_match_ptr (reset_dst)
    },
    .probe = driver_probe,
    .remove = driver_remove
};

/***********************************/
/********** sub-function ***********/
/***********************************/

int thread_function(void *pv)
{
    bool state = false;

    mutex_lock(&data->lock);

    while(!kthread_should_stop()) {
        state =! state;
        if (state == true) PINFO("Active... HIGH\n");
        else PINFO("Active... LOW\n");
        gpiod_set_value(reset, state);
        msleep(1000);
    }

    mutex_unlock(&data->lock);
    
    do_exit(0);
    return 0;
}

/***********************************/
/***** define device attribute *****/
/***********************************/

// create struct device_attribute variable

//=============================== ISP ==================================//
static ssize_t isp_store(struct device *dev, struct device_attribute *attr, const char *buff, size_t len)
{
    private_data_t *data = dev_get_drvdata(dev);
    if (!data)
        PERR("Can't get private data from device, pointer value: %p\n", data);
    else
        PINFO ("inside isp_store, store %d byte, first byte value: %c\n", len, buff[0]);
    if (buff[0] == '1' && (len == 2))
    {
        mutex_lock(&data->lock);
        gpiod_set_value(reset, 1); 

        msleep(100);

        gpiod_set_value(reset, 0);
        data->isp_mode = true;
        mutex_unlock(&data->lock);
    } else
        PINFO("wrong format \n");
    return len;
}
static ssize_t isp_show(struct device *dev, struct device_attribute *attr, char *buf)
{ 
    int res;
    private_data_t *data = dev_get_drvdata(dev);
    if (!data)
        PERR("Can't get private data from device, pointer value: %p\n", data);

    res = scnprintf(buf, PAGE_SIZE, "%s\n", data->isp_mode ? "isp mode" : "normal mode");

    return res;
}

static DEVICE_ATTR_RW(isp);

//============================== RESET =================================//
static ssize_t reset_store(struct device *dev, struct device_attribute *attr, const char *buff, size_t len)
{
    private_data_t *data = dev_get_drvdata(dev);
    if (!data)
        PERR("Can't get private data from device, pointer value: %p\n", data);
    else
        PINFO ("inside reset_store %d byte, first byte value: %c\n", len, buff[0]);

    if (buff[0] == '1' && (len == 2))
    {
       mutex_lock(&data->lock);
       gpiod_set_value(reset, 1); 

       msleep(100);

       gpiod_set_value(reset, 0);
       data->isp_mode = false;
       mutex_unlock(&data->lock);
    } else
        PINFO("wrong format \n");

    return len;
} 

static DEVICE_ATTR_WO(reset);

//============================== BLINKY =================================//
static ssize_t blink_store(struct device *dev, struct device_attribute *attr, const char *buff, size_t len)
{
    // size_t i;
    private_data_t *data = dev_get_drvdata(dev);
    if (!data)
        PERR("Can't get private data from device, pointer value: %p\n", data);
    else
        PINFO ("inside isp_store, store %d byte, string value: %s\n", len, buff);
    
    if (buff[0] == 't' && buff[1] == 'e' &&buff[2] == 's' &&buff[3] == 't' && (len == 5) &&  (thread_run == false))
    {
        // Start thread
        thread_run = true;
        thread = kthread_create(thread_function,NULL,"blinky_thread");
        if(thread) {
            PINFO("Kthread Created Successfully...\n");
            wake_up_process(thread);
        } else {
            PERR("Cannot create kthread\n");
            return -1;
        }
    } else
        // PINFO("wrong format \n");

    if (buff[0] == 's' && buff[1] == 't' &&buff[2] == 'o' &&buff[3] == 'p' && (len == 5) &&  (thread_run == true))
    {
        // Start thread
        thread_run = false;
        PINFO("Stop thread\n");
        kthread_stop(thread);
        
    } else
        PINFO("wrong format \n");
    return len;
}
static ssize_t blink_show(struct device *dev, struct device_attribute *attr, char *buf)
{ 
    int res;
    private_data_t *data = dev_get_drvdata(dev);
    if (!data)
        PERR("Can't get private data from device, pointer value: %p\n", data);

    res = scnprintf(buf, PAGE_SIZE, "%s\n", data->isp_mode ? "alpha mode" : "beta mode");

    return res;
}

static DEVICE_ATTR_RW(blink);

//========================== ADD ATTRIBUTE ==============================//
static struct attribute *device_attrs[] = {
        &dev_attr_reset.attr,
        &dev_attr_isp.attr,
        &dev_attr_blink.attr,
	    NULL
};
ATTRIBUTE_GROUPS(device);
//================================ END ==================================//

/***********************************/
/******** module init + exit *******/
/***********************************/

static int driver_probe (struct platform_device *pdev)
{
#if 0
    // DEBUG PIN
    struct device_node *pDN;
    pDN = of_find_node_by_name(NULL,pdev->name);

    if (!pDN)
        PERR("device node pointer is NULL\n");     
    else 
        pr_err("%s = %p\n", pdev->name, pDN);
    
    count = of_gpio_named_count(pDN, DRIVER_NAME);
    PINFO("gpio count is: %d \n", count);

    PINFO ("node name \"%s\"\n",pdev->dev.of_node->name );
#endif

    // register a device with major and minor number without create device file
    res = alloc_chrdev_region(&device_num, FIRST_MINOR, 250, DRIVER_NAME); 
    if (res){
        PERR("Can't register driver, error code: %d \n", res); 
        goto error;
    } else
        PINFO("success register driver with major is %d, minor is %d \n", MAJOR(device_num), MINOR(device_num));
    
    // Creating cdev structure
    cdev_init(&cdev,&fops);
 
    // Adding character device to the system
    if((cdev_add(&cdev,device_num,1)) < 0){
        PINFO("Cannot add the device to the system\n");
        goto error_class;
    }

    // create class 
    device_class = class_create(THIS_MODULE, DRIVER_NAME);
    if (IS_ERR(device_class))
    {
        PERR("Class create failed, error code: %p\n", device_class);
        goto error_class;
    } else
        PINFO("success create class of driver\n");

    // create private data
    data = (private_data_t*)kcalloc(1, sizeof(private_data_t), GFP_KERNEL);
    data->isp_mode = false;
    mutex_init(&data->lock);

    // create device and add attribute simultaneously
    device = device_create_with_groups(device_class, NULL, device_num, data, device_groups, DRIVER_NAME"s");
    if (IS_ERR(device))
    {
        PERR("device create fall, error code: %p\n", device);
        goto error_device;
    } else
        PINFO("success create device\n");

    reset =  gpiod_get(&pdev->dev, "reset", GPIOD_OUT_LOW);
    if (IS_ERR(reset))
    {
        PINFO ("can't get reset gpio, error code: %p\n", reset);
        goto error_reset_gpio;
    } else
        PINFO("get reset gpio pin\n");

    //init gpio
    res = gpiod_direction_output(reset, 0);
    if (res)
    {
        PERR ("reset gpio can't set as output, error code: %d", res); 
        goto error_gpio;   
    } else
        PINFO("set output active_low\n");
    
    return 0;

//error handle
error_gpio:
    gpiod_put(reset);
error_reset_gpio:
    device_destroy(device_class, device_num);
error_device:
    class_destroy(device_class);
error_class:
    unregister_chrdev_region(device_num, FIRST_MINOR); 
    cdev_del(&cdev);
error:
    return -1;
    
}

static int driver_remove(struct platform_device *pdev)
{
    if (thread_run == true) kthread_stop(thread);
    gpiod_put(reset);
    device_destroy(device_class, device_num);
    class_destroy(device_class);
    cdev_del(&cdev);
    unregister_chrdev_region(device_num, FIRST_MINOR); 
    kfree(data);
    PINFO("Device Driver Module Remove...Done!!\n");
    return 0;
}

/***********************************/
/********** module start ***********/
/***********************************/

module_platform_driver(reset_driver);

/***********************************/
/******* module information ********/
/***********************************/

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Le Phuong Nam <le.phuong.nam@styl.solutions>");
MODULE_DESCRIPTION("Test GPIO Device");
MODULE_VERSION("0.1");
MODULE_SUPPORTED_DEVICE("imx6ulevk");