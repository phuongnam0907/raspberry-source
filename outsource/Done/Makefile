obj-m += mod.o

KDIR := /home/lenam-styl084/rpi3/outsource/linux/
PWD := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD)

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
