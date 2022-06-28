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

/* Buffer for data */
static char buffer[255];
static int buffer_pointer = 0;

struct etn_rate_limiter
{
	struct stcmtk_common_fpga	*fpga;
	struct fpga_feature		    *fan; // пере.
	struct platform_device		*pdev;
	uint32_t cr;
	int32_t num;

	//char dev
	struct cdev device;
	dev_t device_nr;
};

struct file_package
{
	uint16_t enable;
	uint16_t rate;
};

static ssize_t driver_read(struct file *File, char *user_buffer, size_t count, loff_t *offs) {
	long long to_copy, not_copied, delta;

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
	
	if (count < sizeof(struct file_package))
		return 0;

	struct file_package buffer;

	copy_from_user(&buffer, user_buffer, sizeof(struct file_package));
	struct etn_rate_limiter* rl = File->private_data;
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

	printk("Create char dev SUCCESS\n");
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

static int etn_rate_limiter_probe(struct platform_device *pdev)
{
	struct etn_rate_limiter *rl_dev;
	int ret = 0;

	//struct device *dev = &pdev->dev;
	struct device_node *np = pdev->dev.of_node; // Получение указателя на узел dev_tree моего устройства в памяти

	rl_dev = devm_kzalloc(&pdev->dev, sizeof(*rl_dev), GFP_KERNEL);
	if (!rl_dev)
		return -ENOMEM;

	rl_dev->pdev = pdev; //??????

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
	rl_dev->fan = stcmtk_fpga_get_feature(rl_dev->fpga, FPGA_FEATURE_RX_RATE_LIMIT);
	if (!rl_dev->fan) {
		printk("Failed to get FPGA_FEATURE_RX_RATE_LIMIT\n");
		goto err_put;
	}

	uint32_t num;
	// Читаем номер устройства
	int err = of_property_read_u32(np, "port-num", &num);
	if (err)
	{
		printk("Failed to get port-num. Err: %d\n", err);
		goto err_num_port;
	}
	rl_dev->num = num;

	platform_set_drvdata(pdev, rl_dev);

	uint32_t cr = stcmtk_get_cr_base_on_port(rl_dev->fan, num);
	rl_dev->cr = cr;
	printk("cr: %d\n", cr);

	struct stcmtk_common_fpga *fpga = rl_dev->fpga;
	uint32_t tmp = 0xFF;
	int r_err = regmap_read(fpga->regmap, cr, &tmp);
	printk("Reg: %d, e_err: %d\n", tmp, r_err);
	tmp = !tmp;
	int w_err = regmap_write(fpga->regmap, cr, tmp);
	printk("Reg!: %d, w_wrr: %d\n", tmp, w_err);

	int cd_err = init_char_dev(rl_dev);

	return 0;

err_num_port:
err_put:
	etn_fpga_ref_put(rl_dev->fpga); // Умньшить счетчик ссылок
err_ref:
	return -EFAULT;
}
static int etn_rate_limiter_remove(struct platform_device *pdev)
{
	struct etn_rate_limiter *rl_dev = platform_get_drvdata(pdev);

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
	int retval;

	printk("Hello world.\n");

	/* Create device class */
	if((class = class_create(THIS_MODULE, DRIVER_CLASS)) == NULL) {
		printk("Device class can not be created!\n");
		goto class_err;
	}
	printk("create class: SUCCESS\n");

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
	return -1;
}
static void __exit rate_limiter_exit(void)
{
	platform_driver_unregister(&etn_rate_limiter_driver);
	class_destroy(class);
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("STC Metrotek System Team <system@metrotek.ru>");
MODULE_DESCRIPTION("Beeper driver for PROBE devices");

module_init(rate_limiter_init);
module_exit(rate_limiter_exit);
