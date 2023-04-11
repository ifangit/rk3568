KERNEL_DIR=/home/ifan/Lubancat/linux/kernel

ARCH=arm64
CROSS_COMPILE=aarch64-linux-gnu-
out = testApp
export  ARCH  CROSS_COMPILE

obj-m := i2c_oled.o
all:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) modules
	$(CROSS_COMPILE)gcc -o $(out) test_app.c
	
dtbo:
	$(MAKE) -C $(KERNEL_DIR) -j4  dtbs
	
cp:
	cp *.ko *App /run/user/1000/gvfs/sftp:host=10.42.0.207,user=cat/home/cat/
	
	
cpdtbo:
	cp /home/ifan/Lubancat/linux/kernel/arch/arm64/boot/dts/rockchip/overlay/lubancat-ipslcd-overlay.dtbo	/run/user/1000/gvfs/sftp:host=10.42.0.207,user=cat/home/cat/

# dtbo:
# 	cd $(KERNEL_DIR)
# 	make ARCH=arm64 -j4 CROSS_COMPILE=aarch64-linux-gnu- dtbs

.PHONE:clean

clean:
	$(MAKE) -C $(KERNEL_DIR) M=$(CURDIR) clean	
