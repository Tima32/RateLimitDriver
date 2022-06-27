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

#include <asm/page.h>

#include "etn_fpga_mgr/etn_fpga.h"
#include "common_fpga/fpgafeat.h"


struct etn_rate_limiter
{
	struct stcmtk_common_fpga	*fpga;
	struct fpga_feature		    *fan; // пере.
	struct platform_device		*pdev;
};

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
	rl_dev->fan = stcmtk_fpga_get_feature(rl_dev->fpga, FPGA_FEATURE_FAN);
	if (!rl_dev->fan) {
		printk("Failed to get FPGA_FEATURE_FAN\n");
		goto err_put;
	}

	uint32_t num;
	// Читаем номер устройства
	if (!of_property_read_u32_array(np, "port-num", &num, 1))
	{
		printk("Failed to get port-num\n");
		goto err_num_port;
	}

	uint16_t cr = stcmtk_get_cr_base_on_port(rl_dev->fan, num);
	cr *= 2;

	struct stcmtk_common_fpga *fpga = rl_dev->fpga;
	uint32_t tmp;
	regmap_read(fpga->regmap, cr, &tmp);
	cr = !cr;
	regmap_write(fpga->regmap, cr, tmp);

err_num_port:
err_put:
	etn_fpga_ref_put(rl_dev->fpga); // Умньшить счетчик ссылок
err_ref:
	return -EFAULT;
}
static int etn_rate_limiter_remove(struct platform_device *pdev)
{
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
	if (platform_driver_register(&etn_rate_limiter_driver))
	{
		printk("Failed to probe ETN platform driver\n");
		return -ENXIO;
	}
	return 0;
}
static void __init rate_limiter_exit(void)
{
	//platform_driver_unregister(&etn_rate_limiter_driver);
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("STC Metrotek System Team <system@metrotek.ru>");
MODULE_DESCRIPTION("Beeper driver for PROBE devices");

module_init(rate_limiter_init);
module_exit(rate_limiter_exit);
