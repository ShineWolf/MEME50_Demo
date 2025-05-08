obj-m += ads1115_overlay.o
obj-m += meme-ws2812.o

DIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

all:
	$(MAKE) -C $(DIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(DIR) M=$(PWD) clean
