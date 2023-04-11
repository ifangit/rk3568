#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/uaccess.h>
#include <linux/spi/spi.h>
#include <linux/spi/spidev.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/delay.h>
#include <linux/ide.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_gpio.h>
#include <asm/io.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/fb.h>

#include <linux/kthread.h>

#define LCD_W 240
#define LCD_H 240

//画笔颜色
#define WHITE        0xFFFF
#define BLACK        0x0000   
#define BLUE         0x001F  
#define BRED         0XF81F
#define GRED         0XFFE0
#define GBLUE        0X07FF
#define RED          0xF800
#define MAGENTA      0xF81F
#define GREEN        0x07E0
#define CYAN         0x7FFF
#define YELLOW       0xFFE0
#define BROWN        0XBC40 //棕色
#define BRRED        0XFC07 //棕红色
#define GRAY         0X8430 //灰色

/*------------------字符设备内容----------------------*/
#define DEV_NAME "spi_lcd"
#define DEV_CNT (1)


struct ipslcd_par
{
	struct spi_device *spi;
	struct fb_info *info;
	int res_gpios;	            //复位所使用的GPIO编号
	int dc_gpios;				//命令所使用的GPIO编号
    int blk_gpios;	//
	u32 width;
	u32 height;
    struct task_struct *thread;
	/* data */
};

// struct isplcd_par isplcdpar;


/*帧缓冲文件操作集合*/
static struct fb_ops ipslcd_fbops = {
	.owner		= THIS_MODULE,
	//.fb_blank	= oledfb_blank,
};

static int ipslcd_blk(struct ipslcd_par *par, int status)
{
    int ret;
    if(status)
        ret = gpio_direction_output(par->blk_gpios,1);
    else
        ret = gpio_direction_output(par->blk_gpios,0);
    return ret == 0 ? 0 : -1;
}

int spi_write_chunked(struct spi_device *spi, const void *buf, unsigned int len, unsigned int chunk_size)
{
    int ret = 0;
    unsigned int pos = 0;

    while (pos < len) {
        unsigned int remaining = len - pos;
        unsigned int current_chunk = remaining < chunk_size ? remaining : chunk_size;
        ret = spi_write(spi, buf + pos, current_chunk);
        if (ret < 0) {
            return ret;
        }
        pos += current_chunk;
    }

    return pos;
}


#define BUF_SIZE 0xffff

static int ipslcd_wr_regs(struct ipslcd_par *par, const void *buf, unsigned int len)
{
    int ret = 0;
    unsigned int pos = 0;
    unsigned int chunk_size = BUF_SIZE;
    struct spi_device *spi = (struct spi_device *)par->spi;

    while (pos < len) {
        if (len - pos < chunk_size)
            chunk_size = len - pos;

        ret = spi_write_chunked(spi, buf + pos, chunk_size, chunk_size);
        if (ret < 0) {
            dev_err(&spi->dev, "SPI write error\n");
            return ret;
        }

        pos += chunk_size;
    }

    return 0;
}

/* 写一个命令command */
void ipslcd_wr_cmd(struct ipslcd_par *par, u8 cmd)
{
    gpio_set_value(par->dc_gpios, 0);   //写命令 低电平
    ipslcd_wr_regs(par, &cmd, 1);
    gpio_set_value(par->dc_gpios, 1);   //一般很少写命令，写完命令，就切换为写数据
}

/* 写一个数据8位data */
void ipslcd_wr_data8(struct ipslcd_par *par, u8 data)
{
    ipslcd_wr_regs(par, &data, 1);
}

/* 分两次写一个数据16位data */
void ipslcd_wr_data16(struct ipslcd_par *par, u16 data)
{
    u8 data_h8,data_l8;
   // printk(KERN_EMERG " ipslcd wr data16\n");
    //获取高8位和低8位
    data_h8 = data>>8;
    data_l8 = (u8)data;
    //发送2字节 人家设备就是一次只发8bit,要是一下发16bit就不好了
    ipslcd_wr_regs(par, &data_h8, 1);
    ipslcd_wr_regs(par, &data_l8, 1);
}//画笔颜色


/* ipslcd设置起始地址和结束地址 和横竖屏设置有关，具体查阅源码*/
void ipslcd_adr_set(struct ipslcd_par *par, u16 x1, u16 y1, u16 x2, u16 y2)
{
    ipslcd_wr_cmd(par, 0x2a);    //列地址设置
    ipslcd_wr_data16(par, x1);
    ipslcd_wr_data16(par, x2);
    ipslcd_wr_cmd(par, 0x2b);    //行地址设置
    ipslcd_wr_data16(par, y1);
    ipslcd_wr_data16(par, y2);
    ipslcd_wr_cmd(par, 0x2c);    //存储器写
}

/* framebuffer线程刷屏函数 fbi显存的数据给spi发送*/
void show_fb(struct ipslcd_par *par)
{
    u8 *p = (u8 *)par->info->screen_base;
   // printk(KERN_EMERG " show fb \n");
    //创建窗口，
    //0x2c存储器可以一下收很多数据，而设置窗口时
    //列地址和行地址的设置只能一次收8bit数据
    ipslcd_adr_set(par, 0, 0, LCD_W-1, LCD_H-1);
	//发送窗口数据时，可以无顾虑的发，一次spi发好多好多
    ipslcd_wr_regs(par, p, LCD_W*LCD_H*2);

}

/*---------------------------------------------------------------------------------------------------*/
/* spi刷屏幕填充函数 一次spi就发一个像素*/
void ipslcd_fill(struct ipslcd_par *par, u16 color){
    u16 i,j;
   // printk(KERN_EMERG " ipslcd fill \n");
    ipslcd_adr_set(par, 0, 0, LCD_W-1, LCD_H-1);
    for(i=0; i<240; i++)
    {
        for(j=0; j<240; j++)
        {
            ipslcd_wr_data16(par, color);
        }
    }
}


/* ipslcd初始化函数 照着厂家给的51驱动移植*/
void ipslcd_dev_init(struct ipslcd_par *par)
{
    printk(KERN_EMERG " match ipslcd init \n");
    ipslcd_blk(par, 1);

    gpio_set_value(par->res_gpios, 0);
    mdelay(100);
    gpio_set_value(par->res_gpios, 1);
    mdelay(120);

    /************* Start Initial Sequence **********/
    //设置横竖屏显示 可在此查阅源码修改
    ipslcd_wr_cmd(par, 0x11);
    mdelay(120);
    ipslcd_wr_cmd(par, 0x36);
    ipslcd_wr_data8(par, 0x00);

    ipslcd_wr_cmd(par, 0x3A);
    ipslcd_wr_data8(par, 0x05);

    ipslcd_wr_cmd(par, 0xB2);
    ipslcd_wr_data8(par, 0x0C);
    ipslcd_wr_data8(par, 0x0C);
    ipslcd_wr_data8(par, 0x00);
    ipslcd_wr_data8(par, 0x33);
    ipslcd_wr_data8(par, 0x33);

    ipslcd_wr_cmd(par, 0xB7);
    ipslcd_wr_data8(par, 0x72);

    ipslcd_wr_cmd(par, 0xBB);
    ipslcd_wr_data8(par, 0x3D);

    ipslcd_wr_cmd(par, 0xC0);
    ipslcd_wr_data8(par, 0x2C);

    ipslcd_wr_cmd(par, 0xC2);
    ipslcd_wr_data8(par, 0x01);

    ipslcd_wr_cmd(par, 0xC3);
    ipslcd_wr_data8(par, 0x12);

    ipslcd_wr_cmd(par, 0xC4);
    ipslcd_wr_data8(par, 0x20);

    ipslcd_wr_cmd(par, 0xC6);
    ipslcd_wr_data8(par, 0x01);

    ipslcd_wr_cmd(par, 0xD0);
    ipslcd_wr_data8(par, 0xA4);
    ipslcd_wr_data8(par, 0xA1);

    ipslcd_wr_cmd(par, 0xE0);
    ipslcd_wr_data8(par, 0xD0);
    ipslcd_wr_data8(par, 0x04);
    ipslcd_wr_data8(par, 0x0D);
    ipslcd_wr_data8(par, 0x11);
    ipslcd_wr_data8(par, 0x13);
    ipslcd_wr_data8(par, 0x2B);
    ipslcd_wr_data8(par, 0x3F);
    ipslcd_wr_data8(par, 0x54);
    ipslcd_wr_data8(par, 0x4C);
    ipslcd_wr_data8(par, 0x18);
    ipslcd_wr_data8(par, 0x0D);
    ipslcd_wr_data8(par, 0x0B);
    ipslcd_wr_data8(par, 0x1F);
    ipslcd_wr_data8(par, 0x23);

    ipslcd_wr_cmd(par, 0xE1);
    ipslcd_wr_data8(par, 0xD0);
    ipslcd_wr_data8(par, 0x04);
    ipslcd_wr_data8(par, 0x0C);
    ipslcd_wr_data8(par, 0x11);
    ipslcd_wr_data8(par, 0x13);
    ipslcd_wr_data8(par, 0x2C);
    ipslcd_wr_data8(par, 0x3F);
    ipslcd_wr_data8(par, 0x44);
    ipslcd_wr_data8(par, 0x51);
    ipslcd_wr_data8(par, 0x2F);
    ipslcd_wr_data8(par, 0x1F);
    ipslcd_wr_data8(par, 0x1F);
    ipslcd_wr_data8(par, 0x20);
    ipslcd_wr_data8(par, 0x23);

    ipslcd_wr_cmd(par, 0x21);

    ipslcd_wr_cmd(par, 0x29);

    printk(KERN_EMERG"ipslcd init finish \t\n");
    //测试刷屏
    ipslcd_fill(par, YELLOW);   //红

}

static const struct fb_fix_screeninfo st7789fb_fix = {
    .id = "ips st7789",
    //.smem_start= p_addr//物理地址
    .smem_len = LCD_H*LCD_W*4,//屏幕缓冲区大小
    .line_length = LCD_W*4,//一行的字节数
    .type = FB_TYPE_PACKED_PIXELS,
    .visual = FB_VISUAL_TRUECOLOR,
};

static const struct fb_var_screeninfo st7789fb_var = {
    .xres = LCD_W,
    .yres = LCD_H,
    .xres_virtual = LCD_W,
    .yres_virtual = LCD_H,
    .bits_per_pixel = 16,//每个像素16bit=5+6+5  2字节
    .red = 
    {
        .offset = 11,
        .length = 5,
    },
    .green =
    {
        .offset = 5,
        .length = 6,
    },
    .blue =
    {
        .offset = 0,
        .length = 5,
    },
};



/* fb设备清屏函数 直接往显存里放数据 非常接近framebuffer思想*/
void ipslcd_fb_fill2(struct ipslcd_par *par, u16 color)
{
    u8 *p = (u8 *)par->info->screen_base;
    u16 i;

    //ipslcd_address_set(spi, 0, 0, LCD_W-1, LCD_H-1);
    for(i = 0; i < LCD_W * LCD_H; i++)
    {
        p[2*i] = color>>8;      //高8位
        p[2*i+1] = (u8)color;   //低8位
    }
}


struct fb_info *fbtft_framebuffer_alloc(struct spi_device *spi)
{
    struct fb_info *info;
    u8 *vmem = NULL;
    int vmem_size;

    //分配空间
    info = framebuffer_alloc(sizeof(struct ipslcd_par), &spi->dev);

    
    // 参数初始化
    vmem_size =  LCD_W*LCD_H*2;
    vmem = vzalloc(vmem_size);
    if (!vmem) {
        printk(KERN_EMERG "dma_alloc_coherent %d bytes fail!\n", info->fix.smem_len);
    }
    info->screen_base = (u8 __force __iomem *)vmem;
    if (!info->screen_base) {
        printk(KERN_EMERG "dma_alloc_coherent %d bytes fail!\n", info->fix.smem_len);
    }
    info->screen_size = vmem_size;
    info->fix.smem_start = __pa(vmem);
    info->fix.smem_len = vmem_size;

    info->fix = st7789fb_fix;//
    info->var = st7789fb_var;
    info->fbops = &ipslcd_fbops;

    register_framebuffer(info);//注册framebuffer

    return info;

}

static int gpio_init(struct ipslcd_par *par)
{
    struct device_node *node = par->spi->dev.of_node;
    if (!node) {
		dev_err(&par->spi->dev, "No device tree data found!\n");
		return -EINVAL;
	}
    par->res_gpios = of_get_named_gpio(node, "res-gpios", 0);
    par->dc_gpios = of_get_named_gpio(node, "dc-gpios", 0);
    par->blk_gpios = of_get_named_gpio(node, "blk-gpios", 0);
    gpio_request(par->dc_gpios, "spi-lcd-dc-gpio");
    gpio_request(par->res_gpios, "spi-lcd-res-gpio");
    gpio_request(par->blk_gpios, "spi-lcd-blk-gpio");
    gpio_direction_output(par->res_gpios, 1);
    gpio_direction_output(par->dc_gpios, 1);
    gpio_direction_output(par->blk_gpios, 1);
    printk(KERN_INFO "res-gpios:%d, dc-gpios:%d, blk-gpios:%d \n", par->res_gpios, par->dc_gpios, par->blk_gpios);
    return 0;
}

int st7789_refresh_kthread_func(void *data)
{   
    struct ipslcd_par *par = (struct ipslcd_par *)data;
    while (1) {
        if (kthread_should_stop())
            break;
        
        //列地址和行地址的设置只能一次收8bit数据
        ipslcd_adr_set(par, 0, 0, LCD_W-1, LCD_H-1);
        //发送窗口数据时，可以无顾虑的发，一次spi发好多好多
        ipslcd_wr_regs(par, par->info->screen_base, LCD_W*LCD_H*2);
        //FPS@30
        msleep(33);
    }

    return 0;
}

/*----------------平台驱动函数集-----------------*/
static int ipslcd_probe(struct spi_device *spi)
{
	struct ipslcd_par *par;
    struct fb_info *info ;
    printk(KERN_EMERG " match successed \n");	
    //分配空间
    info = fbtft_framebuffer_alloc(spi);
    par = info->par;
    par->info = info;
    par->spi = spi;
    spi_set_drvdata(spi, info);
    spi->mode = SPI_MODE_3;
    spi->max_speed_hz = 24000000;
    spi_setup(spi);

    gpio_init(par);
    ipslcd_blk(par, 0);
    ipslcd_dev_init(par);
    par->thread = kthread_run(st7789_refresh_kthread_func, par, spi->modalias);

	return 0;
}

static int ipslcd_remove(struct spi_device *spi)
{
	struct fb_info *info = spi_get_drvdata(spi);
    struct ipslcd_par *par = (struct ipslcd_par *)info->par;
	unregister_framebuffer(info);
    vfree(info->screen_base);
    framebuffer_release(info);
    gpio_free(par->res_gpios);
    gpio_free(par->dc_gpios);
    gpio_free(par->blk_gpios);

	return 0;
}

/*定义ID 匹配表*/
static const struct spi_device_id ipslcd_device_id[] = {
	{"ifan,spi_lcd", 0},
	{}
};
MODULE_DEVICE_TABLE(spi, ipslcd_device_id);

/*定义设备树匹配表*/
static const struct of_device_id ipslcd_of_match_table[] = {
	{.compatible = "ifan,spi_lcd"},
	{/* sentinel */}
};
MODULE_DEVICE_TABLE(of, ipslcd_of_match_table);

/*定义i2c总线设备结构体*/
struct spi_driver ipslcd_driver = {
	.probe = ipslcd_probe,
	.remove = ipslcd_remove,
	.id_table = ipslcd_device_id,
	.driver = {
		.name = "ifan,spi_lcd",
		.owner = THIS_MODULE,
		.of_match_table = ipslcd_of_match_table,
	},
};

module_spi_driver(ipslcd_driver);

MODULE_DESCRIPTION("FB driver for the ifan SSD1307 OLED controller");
MODULE_LICENSE("GPL");
MODULE_AUTHOR("ifan");
