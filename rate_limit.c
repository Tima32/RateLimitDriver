//#define pr_fmt(fmt) "ETN_FAN: %s:%d: " fmt, __func__, __LINE__

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/fb.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/backlight.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/dma-mapping.h>
#include <linux/hwmon.h>
#include <linux/hwmon-sysfs.h>
#include <linux/regmap.h>

//char dev
#include <linux/fs.h>
#include <linux/cdev.h>

#include <asm/page.h>

#include "etn_fpga_mgr/etn_fpga.h"
#include "common_fpga/fpgafeat.h"

/* Variables for device and device class */
static struct class *class;

#define DRIVER_NAME "ratelimiter%d"
#define DRIVER_CLASS "RateLimiterClass"

#define SYSFS_FOLDER DRIVER_NAME

uint32_t clk = 250000;

struct etn_rate_limiter
{
	struct stcmtk_common_fpga	*fpga;
	struct fpga_feature		    *limiter; // пере.
	struct platform_device		*pdev;
	uint32_t cr;
	int32_t num;

	//char dev
	struct cdev device;
	dev_t device_nr;

	//sysfs
	struct kobject *kobj;
	struct kobj_attribute attr_status;
	struct kobj_attribute attr_rate;
};

struct file_package
{
	uint16_t enable;
	uint16_t rate;
};

// -- chardev -- //
static ssize_t driver_read(struct file *File, char *user_buffer, size_t count, loff_t *offs) {
	size_t to_copy, not_copied, delta;

	struct file_package buffer_package;
	char* buffer = (char*)&buffer_package;
	struct etn_rate_limiter* rl = File->private_data;

	uint32_t b;
	regmap_read(rl->fpga->regmap, rl->cr, &b);
	buffer_package.enable = b;
	regmap_read(rl->fpga->regmap, rl->cr+1, &b);
	buffer_package.rate = b;
	
	if (*offs >= sizeof(struct file_package))
		return 0;

	to_copy = sizeof(struct file_package) - *offs;
	to_copy = min(to_copy, count);

	not_copied = copy_to_user(user_buffer, buffer + *offs, to_copy);

	/* Calculate data */
	delta = to_copy - not_copied;
	*offs += delta;

	return delta;
}
static ssize_t driver_write(struct file *File, const char *user_buffer, size_t count, loff_t *offs) {
	struct file_package buffer;
	struct etn_rate_limiter* rl;
	size_t not_copied = 1;

	if (count < sizeof(struct file_package))
		return 0;

	if ((not_copied = copy_from_user(&buffer, user_buffer, sizeof(struct file_package))) != 0)
		return -EIO;
	rl = File->private_data;
	regmap_write(rl->fpga->regmap, rl->cr, buffer.enable);
	regmap_write(rl->fpga->regmap, rl->cr+1, buffer.rate);

	return sizeof(struct file_package);
}

static int driver_open(struct inode *device_file, struct file *instance) {
	instance->private_data = container_of(device_file->i_cdev, struct etn_rate_limiter, device);
	printk("dev_nr - open was called!\n");
	return 0;
}
static int driver_close(struct inode *device_file, struct file *instance) {
	printk("dev_nr - close was called!\n");
	return 0;
}

static struct file_operations fops = {
	.owner = THIS_MODULE,
	.open = driver_open,
	.release = driver_close,
	.read = driver_read,
	.write = driver_write
};

static int init_char_dev(struct etn_rate_limiter *rl_dev)
{
	char file_name[256] = {0};
	snprintf(file_name, 256, DRIVER_NAME, rl_dev->num);

	/* Allocate a device nr */
	if( alloc_chrdev_region(&rl_dev->device_nr, 0, 1, file_name) < 0) {
		printk("Device Nr. could not be allocated!\n");
		goto acr_err;
	}
	printk("read_write - Device Nr. Major: %d, Minor: %d was registered!\n", rl_dev->device_nr >> 20, rl_dev->device_nr && 0xfffff);

	/* create device file */
	if(device_create(class, NULL, rl_dev->device_nr, NULL, file_name) == NULL) {
		printk("Can not create device file!\n");
		goto dc_err;
	}

	/* Initialize device file */
	cdev_init(&rl_dev->device, &fops);

	/* Regisering device to kernel */
	if(cdev_add(&rl_dev->device, rl_dev->device_nr, 1) == -1) {
		printk("Registering of device to kernel failed!\n");
		goto add_err;
	}

	printk("Create char dev %s SUCCESS\n", file_name);
	return 0;
add_err:
	cdev_del(&rl_dev->device);
	device_destroy(class, rl_dev->device_nr);
dc_err:
	unregister_chrdev_region(rl_dev->device_nr, 1);
acr_err:
	return -1;
}
static void destrou_char_dev(struct etn_rate_limiter *rl_dev)
{
	cdev_del(&rl_dev->device);
	device_destroy(class, rl_dev->device_nr);
	unregister_chrdev_region(rl_dev->device_nr, 1);
	printk("Destroy char dev SUCCESS\n");
}
// -- chardev -- //

// -- sysfs -- //
static ssize_t dummy_status_show(struct kobject *kobj, struct kobj_attribute *attr, char *buffer) {
	struct etn_rate_limiter* rl_dev = container_of(attr, struct etn_rate_limiter, attr_status);
	uint32_t b;

	regmap_read(rl_dev->fpga->regmap, rl_dev->cr, &b);

	return snprintf(buffer, PAGE_SIZE, "%u", b);
}
static ssize_t dummy_status_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buffer, size_t count) {
	int err;
	struct etn_rate_limiter* rl_dev = container_of(attr, struct etn_rate_limiter, attr_status);
	uint16_t buf;

	if (buffer[count] != '\0')
		return -EINVAL;

	err = kstrtou16(buffer, 10, &buf);
	if (err)
		return -err;

	regmap_write(rl_dev->fpga->regmap, rl_dev->cr, buf);

	return count;
}
static ssize_t dummy_rate_show(struct kobject *kobj, struct kobj_attribute *attr, char *buffer) {
	struct etn_rate_limiter* rl_dev = container_of(attr, struct etn_rate_limiter, attr_rate);
	
	uint32_t b;
	regmap_read(rl_dev->fpga->regmap, rl_dev->cr+1, &b);
	b *= clk;

	return snprintf(buffer, PAGE_SIZE, "%u", b);
}
static ssize_t dummy_rate_store(struct kobject *kobj, struct kobj_attribute *attr, const char *buffer, size_t count) {
	struct etn_rate_limiter* rl_dev = container_of(attr, struct etn_rate_limiter, attr_rate);
	uint32_t buf;

	if (buffer[count] != '\0')
		return -1;

	if (kstrtou32(buffer, 10, &buf))
		return -1;

	buf /= clk;

	regmap_write(rl_dev->fpga->regmap, rl_dev->cr+1, buf);

	return count;
}

static struct kobj_attribute attr_status = __ATTR(status, 0660, dummy_status_show, dummy_status_store);
static struct kobj_attribute attr_rate = __ATTR(rate, 0660, dummy_rate_show, dummy_rate_store);

static int init_sysfs(struct etn_rate_limiter *rl_dev)
{
	char folder_name[256] = {0};

	/* Fill attribute */
	rl_dev->attr_status = attr_status;
	rl_dev->attr_rate =   attr_rate;

	snprintf(folder_name, 256, SYSFS_FOLDER, rl_dev->num);

	/* Creating the folder */
	rl_dev->kobj = kobject_create_and_add(folder_name, kernel_kobj);
	if(!rl_dev->kobj) {
		printk("sysfs - Error creating /sys/kernel/%s\n", folder_name);
		goto err_kobject_create;
	}

	/* Creade the sysfs file status */
	if(sysfs_create_file(rl_dev->kobj, &rl_dev->attr_status.attr)) {
		printk("sysfs_test - Error creating /sys/kernel/%s/status\n", folder_name);
		goto err_create_status;
	}

	/* Creade the sysfs file rate */
	if(sysfs_create_file(rl_dev->kobj, &rl_dev->attr_rate.attr)) {
		printk("sysfs_test - Error creating /sys/kernel/%s/rate\n", folder_name);
		goto err_create_rate;
	}

	printk("Create sysfs %s SUCCESS\n", folder_name);
	return 0;

	sysfs_remove_file(rl_dev->kobj, &rl_dev->attr_rate.attr);
err_create_rate:
	sysfs_remove_file(rl_dev->kobj, &rl_dev->attr_status.attr);
err_create_status:
	kobject_put(rl_dev->kobj);
err_kobject_create:
	return -1;
}
static void destroy_sysfs(struct etn_rate_limiter *rl_dev)
{
	sysfs_remove_file(rl_dev->kobj, &rl_dev->attr_rate.attr);
	sysfs_remove_file(rl_dev->kobj, &rl_dev->attr_status.attr);
	kobject_put(rl_dev->kobj);
}

// -- sysfs -- //

static int etn_rate_limiter_probe(struct platform_device *pdev)
{
	struct etn_rate_limiter *rl_dev;
	int ret = 1;
	uint32_t num; // Номер устройства
	uint32_t cr;

	//struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node; // Получение указателя на узел dev_tree моего устройства в памяти

	rl_dev = devm_kzalloc(&pdev->dev, sizeof(*rl_dev), GFP_KERNEL);
	if (!rl_dev)
		return -ENOMEM;

	rl_dev->pdev = pdev;

	rl_dev->fpga = stcmtk_get_fpga(np);
	if (!rl_dev->fpga) {
		printk("Failed to get target FPGA\n");
		return -ENODEV;
	}

	ret = etn_fpga_ref_get(rl_dev->fpga); // Увеличить счетчик ссылок
	if (ret) {
		printk("Failed to increment FPGA reference count\n");
		goto err_ref;
	}

	// Получение самого устройства
	rl_dev->limiter = stcmtk_fpga_get_feature(rl_dev->fpga, FPGA_FEATURE_RX_RATE_LIMIT);
	if (!rl_dev->limiter) {
		printk("Failed to get FPGA_FEATURE_RX_RATE_LIMIT\n");
		goto err_put;
	}

	// Читаем номер устройства
	ret = of_property_read_u32(np, "port-num", &num);
	if (ret)
	{
		printk("Failed to get port-num. Err: %d\n", ret);
		goto err_num_port;
	}
	rl_dev->num = num;

	platform_set_drvdata(pdev, rl_dev);

	cr = stcmtk_get_cr_base_on_port(rl_dev->limiter, num);
	rl_dev->cr = cr;
	printk("cr: %d\n", cr);

	if(init_char_dev(rl_dev))
		goto err_char_dev;

	if(init_sysfs(rl_dev))
		goto err_sysdev;

	return 0;

	destroy_sysfs(rl_dev);
err_sysdev:
	destrou_char_dev(rl_dev);
err_char_dev:
err_num_port:
err_put:
	etn_fpga_ref_put(rl_dev->fpga); // Умньшить счетчик ссылок
err_ref:
	return -ret;
}
static int etn_rate_limiter_remove(struct platform_device *pdev)
{
	struct etn_rate_limiter *rl_dev = platform_get_drvdata(pdev);

	destroy_sysfs(rl_dev);
	destrou_char_dev(rl_dev);

	if (rl_dev == NULL)
	{
		dev_info(&pdev->dev, "rl_dev is NULL. Line: %d\n", __LINE__);
		return 0;
	}

	etn_fpga_ref_put(rl_dev->fpga); // Умньшить счетчик ссылок
	return 0;
}

static const struct of_device_id etn_of_match[] = {
	{.compatible = "etn,rate-limiter",},
	{},
};
MODULE_DEVICE_TABLE(of, etn_of_match); // Автоматическая загрузка модуля при добавлении устройства

static struct platform_driver etn_rate_limiter_driver = {
	.probe = etn_rate_limiter_probe,
	.remove = etn_rate_limiter_remove,
	.driver = {
		.name = "etn-rate-limiter",
		.owner = THIS_MODULE,
		.of_match_table = of_match_ptr(etn_of_match),
	},
};

static int __init rate_limiter_init(void)
{
	printk("Hello world.\n");

	/* Create device class */
	if((class = class_create(THIS_MODULE, DRIVER_CLASS)) == NULL) {
		printk("Device class can not be created!\n");
		goto class_err;
	}

	//----------------//
	
	if (platform_driver_register(&etn_rate_limiter_driver))
	{
		printk("Failed to probe ETN platform driver\n");
		goto driver_register_err;
	}
	return 0;

driver_register_err:
	class_destroy(class);
class_err:
	return -EIO;
}
static void __exit rate_limiter_exit(void)
{
	platform_driver_unregister(&etn_rate_limiter_driver);
	class_destroy(class);
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Golovlev Timofei <4timonomit4@gmail.com>");
MODULE_DESCRIPTION("Beeper driver for PROBE devices");

module_init(rate_limiter_init);
module_exit(rate_limiter_exit);
