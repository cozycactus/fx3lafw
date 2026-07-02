

CC = arm-none-eabi-gcc
HOST_CC ?= cc
PKG_CONFIG ?= pkg-config

CGENFLAGS = -mcpu=arm926ej-s -mthumb-interwork -fno-pie
WARN = -Wall -Wextra -Werror
OPTIMIZE = -g -Os
INCLUDE = -I.
GENDEP = -MMD -MP
DEFS = -DPLL_FBDIV=20

CFLAGS = -std=c11 $(CGENFLAGS) $(WARN) $(OPTIMIZE) $(INCLUDE) $(GENDEP) $(DEFS) -fno-stack-protector
LDFLAGS = -static -nostartfiles -T bsp/fx3.ld -Wl,-z,max-page-size=4096,-Map,$(basename $@).map
LIBUSB_CFLAGS = $(shell $(PKG_CONFIG) --cflags libusb-1.0)
LIBUSB_LIBS = $(shell $(PKG_CONFIG) --libs libusb-1.0)

VPATH = bsp

OBJS = main.o usb.o gpif.o gctl.o gpio.o uart.o util.o dma.o irq.o cache.o vectors.o descriptors.o acquisition.o

all : fx3lafw-cypress-fx3.fw
host-tools : tools/fx3lafw-reset tools/fx3lafw-status

fx3lafw-cypress-fx3.fw : fx3lafw.elf
	python3 elf2img.py $< $@

clean :
	rm -f fx3lafw.img fx3lafw.elf fx3lafw.map tools/fx3lafw-reset tools/fx3lafw-status $(OBJS) $(OBJS:.o=.d)

fx3lafw.elf : $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

tools/fx3lafw-reset : tools/fx3lafw-reset.c command.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS)

tools/fx3lafw-status : tools/fx3lafw-status.c command.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS)

-include $(OBJS:.o=.d)
