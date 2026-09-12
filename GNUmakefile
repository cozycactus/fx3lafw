

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
ULPI_OBJS = ulpi-main.o ulpi-usb.o ulpi-gpif.o gctl.o gpio.o uart.o util.o ulpi-dma.o irq.o cache.o vectors.o ulpi-descriptors.o ulpi-acquisition.o ulpi.o

all : fx3lafw-cypress-fx3.fw
ulpi : fx3ulpifw-cypress-fx3.fw
host-tools : tools/fx3lafw-load tools/fx3lafw-reset tools/fx3lafw-status \
	tools/fx3lafw-ulpi-status tools/ulpi-capture-check

check-host: tools/fx3lafw-load-test tools/ulpi-capture-check-test
	./tools/fx3lafw-load-test
	./tools/ulpi-capture-check-test

fx3lafw-cypress-fx3.fw : fx3lafw.elf
	python3 elf2img.py $< $@

fx3ulpifw-cypress-fx3.fw : fx3ulpifw.elf
	python3 elf2img.py $< $@

clean :
	rm -f fx3lafw.img fx3lafw.elf fx3lafw.map fx3ulpifw.elf fx3ulpifw.map \
		fx3ulpifw-cypress-fx3.fw tools/fx3lafw-reset tools/fx3lafw-status \
		tools/fx3lafw-load tools/fx3lafw-load-test tools/fx3lafw-ulpi-status \
		tools/ulpi-capture-check tools/ulpi-capture-check-test \
		$(OBJS) $(OBJS:.o=.d) ulpi-main.o ulpi-main.d \
		ulpi-usb.o ulpi-usb.d \
		ulpi-descriptors.o ulpi-descriptors.d \
		ulpi-acquisition.o ulpi-acquisition.d ulpi-dma.o ulpi-dma.d \
		ulpi-gpif.o ulpi-gpif.d \
		ulpi.o ulpi.d

fx3lafw.elf : $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

fx3ulpifw.elf : $(ULPI_OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

ulpi-main.o : main.c
	$(CC) $(CFLAGS) -DFX3_ULPI_SNIFFER -c -o $@ $<

ulpi-usb.o : usb.c
	$(CC) $(CFLAGS) -DFX3_ULPI_SNIFFER -c -o $@ $<

ulpi-descriptors.o : descriptors.c
	$(CC) $(CFLAGS) -DFX3_ULPI_SNIFFER -c -o $@ $<

ulpi-acquisition.o : acquisition.c
	$(CC) $(CFLAGS) -DFX3_ULPI_SNIFFER -c -o $@ $<

ulpi-dma.o : dma.c
	$(CC) $(CFLAGS) -DFX3_ULPI_SNIFFER -c -o $@ $<

ulpi-gpif.o : gpif.c
	$(CC) $(CFLAGS) -DFX3_ULPI_SNIFFER -c -o $@ $<

tools/fx3lafw-load : tools/fx3lafw-load.c command.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS)

tools/fx3lafw-load-test : tools/fx3lafw-load-test.c tools/fx3lafw-load.c command.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS)

tools/fx3lafw-reset : tools/fx3lafw-reset.c command.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS)

tools/fx3lafw-status : tools/fx3lafw-status.c command.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS)

tools/fx3lafw-ulpi-status : tools/fx3lafw-ulpi-status.c command.h
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror $(LIBUSB_CFLAGS) -o $@ $< $(LIBUSB_LIBS)

tools/ulpi-capture-check : tools/ulpi-capture-check.c
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $<

tools/ulpi-capture-check-test : tools/ulpi-capture-check-test.c tools/ulpi-capture-check.c
	$(HOST_CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $<

-include $(OBJS:.o=.d) ulpi-main.d ulpi-usb.d ulpi-descriptors.d \
	ulpi-acquisition.d ulpi-dma.d ulpi-gpif.d ulpi.d

.PHONY: all ulpi clean host-tools check-host
