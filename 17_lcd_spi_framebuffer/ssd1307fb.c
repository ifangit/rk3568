/*
 * Driver for the Solomon SSD1307 OLED controller
 *
 * Copyright 2012 Free Electrons
 *
 * Licensed under the GPLv2 or later.
 */

#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/fb.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/pwm.h>
#include <linux/uaccess.h>
#include <linux/regulator/consumer.h>

#define SSD1307FB_DATA			0x40//写数据
#define SSD1307FB_COMMAND		0x00//写命令

#define SSD1307FB_SET_ADDRESS_MODE	0x20
#define SSD1307FB_SET_ADDRESS_MODE_HORIZONTAL	(0x00)
#define SSD1307FB_SET_ADDRESS_MODE_VERTICAL	(0x01)
#define SSD1307FB_SET_ADDRESS_MODE_PAGE		(0x02)
#define SSD1307FB_SET_COL_RANGE		0x21
#define SSD1307FB_SET_PAGE_RANGE	0x22
#define SSD1307FB_CONTRAST		0x81//设置对比度
#define	SSD1307FB_CHARGE_PUMP		0x8d//电荷泵
#define SSD1307FB_SEG_REMAP_ON		0xa1
#define SSD1307FB_DISPLAY_OFF		0xae//关闭oled
#define SSD1307FB_SET_MULTIPLEX_RATIO	0xa8
#define SSD1307FB_DISPLAY_ON		0xaf
#define SSD1307FB_START_PAGE_ADDRESS	0xb0
#define SSD1307FB_SET_DISPLAY_OFFSET	0xd3// 设置显示偏移 -- set display offset (0x00~0x3F)
#define	SSD1307FB_SET_CLOCK_FREQ	0xd5//设置显示时钟分频因子/振荡器频率
#define	SSD1307FB_SET_PRECHARGE_PERIOD	0xd9
#define	SSD1307FB_SET_COM_PINS_CONFIG	0xda// 设置列引脚硬件配置 -- set com pins hardware configuration
#define	SSD1307FB_SET_VCOMH		0xdb

#define MAX_CONTRAST 255

#define REFRESHRATE 1

static u_int refreshrate = REFRESHRATE;
module_param(refreshrate, uint, 0);

struct ssd1307fb_par;

struct ssd1307fb_deviceinfo {
	u32 default_vcomh;
	u32 default_dclk_div;//分频因子
	u32 default_dclk_frq;//震荡频率
	int need_pwm;
	int need_chargepump;//电荷泵
};

struct ssd1307fb_par {
	u32 com_invdir;
	u32 com_lrremap;
	u32 com_offset;//设置显示偏移 -- set display offset (0x00~0x3F)
	u32 com_seq;
	u32 contrast;//对比度
	u32 dclk_div;//分频因子
	u32 dclk_frq;//震荡频率
	const struct ssd1307fb_deviceinfo *device_info;
	struct i2c_client *client;
	u32 width;
	u32 height;
	struct fb_info *info;
	u32 page_offset;
	u32 prechargep1;
	u32 prechargep2;
	struct pwm_device *pwm;
	u32 pwm_period;
	struct gpio_desc *reset;
	struct regulator *vbat_reg;
	u32 seg_remap;//左右反置
	u32 vcomh;
	
};

struct ssd1307fb_array {
	u8	type;
	u8	data[0];
};

static const struct fb_fix_screeninfo ssd1307fb_fix = {
	.id		= "Solomon SSD1306",
	
};

static const struct fb_var_screeninfo ssd1307fb_var = {
	.bits_per_pixel	= 1,
};

static struct ssd1307fb_array *ssd1307fb_alloc_array(u32 len, u8 type)
{
	struct ssd1307fb_array *array;

	array = kzalloc(sizeof(struct ssd1307fb_array) + len, GFP_KERNEL);
	if (!array)
		return NULL;

	array->type = type;

	return array;
}

static int ssd1307fb_write_array(struct i2c_client *client,
				 struct ssd1307fb_array *array, u32 len)
{
	int ret;

	len += sizeof(struct ssd1307fb_array);

	ret = i2c_master_send(client, (u8 *)array, len);
	if (ret != len) {
		dev_err(&client->dev, "Couldn't send I2C command.\n");
		return ret;
	}

	return 0;
}

static inline int ssd1307fb_write_cmd(struct i2c_client *client, u8 cmd)
{
	struct ssd1307fb_array *array;
	int ret;

	array = ssd1307fb_alloc_array(1, SSD1307FB_COMMAND);
	if (!array)
		return -ENOMEM;

	array->data[0] = cmd;

	ret = ssd1307fb_write_array(client, array, 1);
	kfree(array);

	return ret;
}

static void ssd1307fb_update_display(struct ssd1307fb_par *par)
{
	struct ssd1307fb_array *array;
	u8 *vmem = par->info->screen_base;
	int i, j, k;

	array = ssd1307fb_alloc_array(par->width * par->height / 8,
				      SSD1307FB_DATA);
	if (!array)
		return;

	for (i = 0; i < (par->height / 8); i++) {
		for (j = 0; j < par->width; j++) {
			u32 array_idx = i * par->width + j;
			array->data[array_idx] = 0;
			for (k = 0; k < 8; k++) {
				u32 page_length = par->width * i;
				u32 index = page_length + (par->width * k + j) / 8;
				u8 byte = *(vmem + index);
				u8 bit = byte & (1 << (j % 8));
				bit = bit >> (j % 8);
				array->data[array_idx] |= bit << k;
			}
		}
	}
	ssd1307fb_write_array(par->client, array, par->width * par->height / 8);
	kfree(array);
}


static ssize_t ssd1307fb_write(struct fb_info *info, const char __user *buf,
		size_t count, loff_t *ppos)
{
	struct ssd1307fb_par *par = info->par;
	unsigned long total_size;
	unsigned long p = *ppos;
	u8 __iomem *dst;

	total_size = info->fix.smem_len;

	if (p > total_size)
		return -EINVAL;

	if (count + p > total_size)
		count = total_size - p;

	if (!count)
		return -EINVAL;

	dst = (void __force *) (info->screen_base + p);

	if (copy_from_user(dst, buf, count))
		return -EFAULT;

	ssd1307fb_update_display(par);

	*ppos += count;

	return count;
}

static int ssd1307fb_blank(int blank_mode, struct fb_info *info)
{
	struct ssd1307fb_par *par = info->par;

	if (blank_mode != FB_BLANK_UNBLANK)
		return ssd1307fb_write_cmd(par->client, SSD1307FB_DISPLAY_OFF);
	else
		return ssd1307fb_write_cmd(par->client, SSD1307FB_DISPLAY_ON);
}



static struct fb_ops ssd1307fb_ops = {
	.owner		= THIS_MODULE,
	.fb_read	= fb_sys_read,
	.fb_write	= ssd1307fb_write,
	.fb_blank	= ssd1307fb_blank,
};

static void ssd1307fb_deferred_io(struct fb_info *info,
				struct list_head *pagelist)
{
	ssd1307fb_update_display(info->par);
}

static int ssd1307fb_init(struct ssd1307fb_par *par)
{
	int ret;
	u32 precharge, dclk, com_invdir, compins;

	/* Set initial contrast */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_CONTRAST);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client, par->contrast);
	if (ret < 0)
		return ret;

	/* Set segment re-map */
	if (par->seg_remap) {
		ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SEG_REMAP_ON);
		if (ret < 0)
			return ret;
	};

	/* Set COM direction */
	com_invdir = 0xc0 | (par->com_invdir & 0x1) << 3;
	ret = ssd1307fb_write_cmd(par->client,  com_invdir);
	if (ret < 0)
		return ret;

	/* Set multiplex ratio value */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SET_MULTIPLEX_RATIO);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client, par->height - 1);
	if (ret < 0)
		return ret;

	/* set display offset value */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SET_DISPLAY_OFFSET);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client, par->com_offset);
	if (ret < 0)
		return ret;

	/* Set clock frequency */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SET_CLOCK_FREQ);
	if (ret < 0)
		return ret;

	dclk = ((par->dclk_div - 1) & 0xf) | (par->dclk_frq & 0xf) << 4;
	ret = ssd1307fb_write_cmd(par->client, dclk);
	if (ret < 0)
		return ret;

	/* Set precharge period in number of ticks from the internal clock */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SET_PRECHARGE_PERIOD);
	if (ret < 0)
		return ret;

	precharge = (par->prechargep1 & 0xf) | (par->prechargep2 & 0xf) << 4;
	ret = ssd1307fb_write_cmd(par->client, precharge);
	if (ret < 0)
		return ret;

	/* Set COM pins configuration */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SET_COM_PINS_CONFIG);
	if (ret < 0)
		return ret;

	compins = 0x02 | !(par->com_seq & 0x1) << 4
				   | (par->com_lrremap & 0x1) << 5;
	ret = ssd1307fb_write_cmd(par->client, compins);
	if (ret < 0)
		return ret;

	/* Set VCOMH */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SET_VCOMH);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client, par->vcomh);
	if (ret < 0)
		return ret;

	/* Turn on the DC-DC Charge Pump */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_CHARGE_PUMP);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client,
		BIT(4) | (par->device_info->need_chargepump ? BIT(2) : 0));
	if (ret < 0)
		return ret;

	/* Switch to horizontal addressing mode */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SET_ADDRESS_MODE);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client,
				  SSD1307FB_SET_ADDRESS_MODE_HORIZONTAL);
	if (ret < 0)
		return ret;

	/* Set column range */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SET_COL_RANGE);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client, 0x0);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client, par->width - 1);
	if (ret < 0)
		return ret;

	/* Set page range */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_SET_PAGE_RANGE);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client, par->page_offset);
	if (ret < 0)
		return ret;

	ret = ssd1307fb_write_cmd(par->client,
				  par->page_offset + (par->height / 8) - 1);
	if (ret < 0)
		return ret;

	/* Clear the screen */
	ssd1307fb_update_display(par);

	/* Turn on the display */
	ret = ssd1307fb_write_cmd(par->client, SSD1307FB_DISPLAY_ON);
	if (ret < 0)
		return ret;

	return 0;
}


static struct ssd1307fb_deviceinfo ssd1307fb_ssd1306_deviceinfo = {
	.default_vcomh = 0x20,
	.default_dclk_div = 1,
	.default_dclk_frq = 8,
	.need_chargepump = 1,
};

static const struct of_device_id ssd1307fb_of_match[] = {
	{
		.compatible = "solomon,ssd1306fb-i2c",
		.data = (void *)&ssd1307fb_ssd1306_deviceinfo,
	},
	{},
};
MODULE_DEVICE_TABLE(of, ssd1307fb_of_match);

static int ssd1307fb_probe(struct i2c_client *client,
			   const struct i2c_device_id *id)
{
	struct fb_info *info;
	struct device_node *node = client->dev.of_node;
	struct fb_deferred_io *ssd1307fb_defio;
	u32 vmem_size;
	struct ssd1307fb_par *par;
	u8 *vmem;
	int ret;

	if (!node) {
		dev_err(&client->dev, "No device tree data found!\n");
		return -EINVAL;
	}

	info = framebuffer_alloc(sizeof(struct ssd1307fb_par), &client->dev);
	if (!info) {
		dev_err(&client->dev, "Couldn't allocate framebuffer.\n");
		return -ENOMEM;
	}

	par = info->par;
	par->info = info;
	par->client = client;

	par->device_info = of_device_get_match_data(&client->dev);

	if (of_property_read_u32(node, "solomon,width", &par->width))
		par->width = 128;

	if (of_property_read_u32(node, "solomon,height", &par->height))
		par->height = 64;


	// par->seg_remap = !of_property_read_bool(node, "solomon,segment-no-remap");
	// par->com_seq = of_property_read_bool(node, "solomon,com-seq");
	// par->com_lrremap = of_property_read_bool(node, "solomon,com-lrremap");
	// par->com_invdir = of_property_read_bool(node, "solomon,com-invdir");
	par->seg_remap = 1;//左右对置
	par->com_invdir = 1;//上下对置
	par->contrast = 127;//对比度
	par->vcomh = par->device_info->default_vcomh;

	/* Setup display timing */
	par->dclk_div = par->device_info->default_dclk_div;
	par->dclk_frq = par->device_info->default_dclk_frq;

	vmem_size = par->width * par->height / 8;

	vmem = (void *)__get_free_pages(GFP_KERNEL | __GFP_ZERO,
					get_order(vmem_size));
	if (!vmem) {
		dev_err(&client->dev, "Couldn't allocate graphical memory.\n");
		ret = -ENOMEM;
		goto fb_alloc_error;
	}

	ssd1307fb_defio = devm_kzalloc(&client->dev, sizeof(*ssd1307fb_defio),
				       GFP_KERNEL);
	if (!ssd1307fb_defio) {
		dev_err(&client->dev, "Couldn't allocate deferred io.\n");
		ret = -ENOMEM;
		goto fb_alloc_error;
	}

	ssd1307fb_defio->delay = HZ / refreshrate;
	ssd1307fb_defio->deferred_io = ssd1307fb_deferred_io;

	info->fbops = &ssd1307fb_ops;
	info->fix = ssd1307fb_fix;
	info->fix.line_length = par->width / 8;
	info->fbdefio = ssd1307fb_defio;

	info->var = ssd1307fb_var;
	info->var.xres = par->width;
	// info->var.xres_virtual = par->width;
	info->var.yres = par->height;
	// info->var.yres_virtual = par->height;


	info->screen_base = (u8 __force __iomem *)vmem;
	info->fix.smem_start = __pa(vmem);
	info->fix.smem_len = vmem_size;

	fb_deferred_io_init(info);

	i2c_set_clientdata(client, info);


	ret = ssd1307fb_init(par);
	if (ret)
		goto regulator_enable_error;

	ret = register_framebuffer(info);
	if (ret) {
		dev_err(&client->dev, "Couldn't register the framebuffer\n");
		goto panel_init_error;
	}


	dev_info(&client->dev, "fb%d: %s framebuffer device registered, using %d bytes of video memory\n", info->node, info->fix.id, vmem_size);

	return 0;

	unregister_framebuffer(info);
panel_init_error:
	if (par->device_info->need_pwm) {
		pwm_disable(par->pwm);
		pwm_put(par->pwm);
	};
regulator_enable_error:
	fb_deferred_io_cleanup(info);
fb_alloc_error:
	framebuffer_release(info);
	return ret;

}

static int ssd1307fb_remove(struct i2c_client *client)
{
	struct fb_info *info = i2c_get_clientdata(client);
	struct ssd1307fb_par *par = info->par;

	ssd1307fb_write_cmd(par->client, SSD1307FB_DISPLAY_OFF);

	unregister_framebuffer(info);

	fb_deferred_io_cleanup(info);
	__free_pages(__va(info->fix.smem_start), get_order(info->fix.smem_len));
	framebuffer_release(info);

	return 0;
}

static const struct i2c_device_id ssd1307fb_i2c_id[] = {
	{ "ssd1306fb", 0 },
	{ }
};
MODULE_DEVICE_TABLE(i2c, ssd1307fb_i2c_id);

static struct i2c_driver ssd1307fb_driver = {
	.probe = ssd1307fb_probe,
	.remove = ssd1307fb_remove,
	.id_table = ssd1307fb_i2c_id,
	.driver = {
		.name = "ssd1307fb",
		.of_match_table = ssd1307fb_of_match,
	},
};

module_i2c_driver(ssd1307fb_driver);

MODULE_DESCRIPTION("FB driver for the Solomon SSD1307 OLED controller");
MODULE_AUTHOR("Maxime Ripard <maxime.ripard@free-electrons.com>");
MODULE_LICENSE("GPL");
