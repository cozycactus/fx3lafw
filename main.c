
#include <bsp/gctl.h>
#include <bsp/gpio.h>
#include <bsp/uart.h>
#include <bsp/usb.h>
#include <bsp/irq.h>
#include <bsp/cache.h>
#include <bsp/util.h>

#include <string.h>
#include <stdio.h>

#include "descriptors.h"
#include "acquisition.h"
#include "command.h"
#ifdef FX3_ULPI_SNIFFER
#include "ulpi.h"
#endif

#define CLOCK_DIVISOR_X2_FOR(sys_clk, f) (((sys_clk) + (f)/4) / ((f)/2))
#define FX3LAFW_89MHZ_PLL_FBDIV 21
#define FX3LAFW_89MHZ_SYS_CLK FX3_GCTL_SYS_CLK_FOR_PLL_FBDIV(FX3LAFW_89MHZ_PLL_FBDIV)

static volatile uint8_t DmaBuf[128] __attribute__((aligned(32)));

#ifdef FX3_ULPI_SNIFFER
enum DeferredVendorCommand {
  DEFERRED_VENDOR_COMMAND_NONE = 0,
  DEFERRED_VENDOR_COMMAND_STOP,
};

static volatile enum DeferredVendorCommand DeferredCommand;
static volatile uint8_t DeferredCommandReady;

static void UsbStatusStage(Fx3UsbSpeed_t s)
{
  (void)s;

  if (DeferredCommand != DEFERRED_VENDOR_COMMAND_NONE)
    DeferredCommandReady = 1;
}

static void PollDeferredVendorCommand(void)
{
  if (!DeferredCommandReady)
    return;

  DeferredCommandReady = 0;
  switch (DeferredCommand) {
  case DEFERRED_VENDOR_COMMAND_STOP:
    Fx3UartTxString("CMD_STOP_APPLY\n");
    stop_acquisition();
    Fx3GctlSetPllFbDiv(PLL_FBDIV);
    ulpi_start_idle();
    break;
  case DEFERRED_VENDOR_COMMAND_NONE:
    break;
  }
  DeferredCommand = DEFERRED_VENDOR_COMMAND_NONE;
}
#endif

static void VendorCommand(uint8_t request_type, uint8_t request, uint16_t value,
			  uint16_t index, uint16_t length, Fx3UsbSpeed_t s)
{
  switch(request) {
#ifdef FX3_ULPI_SNIFFER
  case CMD_SET_ULPI_PHASE:
    if (request_type !=
	(FX3_USB_REQTYPE_OUT | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (length != 0 ||
	(index == ULPI_DLL_CONFIG_PHASE &&
	 value > ULPI_CORE_PHASE_DLL_OFF) ||
	(index == ULPI_DLL_CONFIG_FIXED_DELAY &&
	 value > ULPI_DLL_FIXED_DELAY_MAX) ||
	index > ULPI_DLL_CONFIG_FIXED_DELAY)
      goto stall;
    Fx3UartTxString("CMD_SET_ULPI_PHASE\n");
    set_ulpi_acquisition_timing(value, (uint8_t)index);
    Fx3UsbUnstallEp0(s);
    return;
  case CMD_GET_ULPI_ACQ_STATUS:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 ||
        length != sizeof(struct ulpi_acquisition_status))
      goto stall;
    get_ulpi_acquisition_status(
      (volatile struct ulpi_acquisition_status *)DmaBuf);
    Fx3CacheCleanDCacheEntry(DmaBuf);
    Fx3CacheCleanDCacheEntry(DmaBuf + 32);
    Fx3CacheCleanDCacheEntry(DmaBuf + 64);
    Fx3UsbUnstallEp0(s);
    Fx3UsbDmaDataIn(0, DmaBuf, sizeof(struct ulpi_acquisition_status));
    return;
  case CMD_GET_ULPI_PIN_STATUS:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index > 1 || length != sizeof(struct ulpi_pin_status))
      goto stall;
    Fx3UartTxString("CMD_GET_ULPI_PIN_STATUS\n");
    ulpi_get_pin_status((volatile struct ulpi_pin_status *)DmaBuf, index != 0);
	    Fx3CacheCleanDCacheEntry(DmaBuf);
	    Fx3CacheCleanDCacheEntry(DmaBuf + 32);
	    Fx3CacheCleanDCacheEntry(DmaBuf + 64);
	    Fx3CacheCleanDCacheEntry(DmaBuf + 96);
    Fx3UsbUnstallEp0(s);
    Fx3UsbDmaDataIn(0, DmaBuf, sizeof(struct ulpi_pin_status));
    return;
  case CMD_GET_ULPI_STATUS:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != sizeof(struct ulpi_status))
      goto stall;
    Fx3UartTxString("CMD_GET_ULPI_STATUS\n");
    ulpi_get_status((volatile struct ulpi_status *)DmaBuf);
    Fx3CacheCleanDCacheEntry(DmaBuf);
    Fx3CacheCleanDCacheEntry(DmaBuf + 32);
    Fx3CacheCleanDCacheEntry(DmaBuf + 64);
    Fx3UsbUnstallEp0(s);
    Fx3UsbDmaDataIn(0, DmaBuf, sizeof(struct ulpi_status));
    return;
  case CMD_SET_ULPI_MODE:
    if (request_type !=
	(FX3_USB_REQTYPE_OUT | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value > ULPI_SPEED_LOW || index > ULPI_MODE_CONFIG_FORCE || length != 0)
      goto stall;
    Fx3UartTxString("CMD_SET_ULPI_MODE\n");
    if (index == ULPI_MODE_CONFIG_FORCE) {
      if (!ulpi_force_configure_passive((enum ulpi_speed)value))
	goto stall;
    } else if (!ulpi_configure_passive((enum ulpi_speed)value)) {
      goto stall;
    }
    Fx3UsbUnstallEp0(s);
    return;
#endif
  case CMD_STOP:
    if (request_type !=
	(FX3_USB_REQTYPE_OUT | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != 0)
      goto stall;
    Fx3UartTxString("CMD_STOP\n");
#ifdef FX3_ULPI_SNIFFER
    if (DeferredCommand != DEFERRED_VENDOR_COMMAND_NONE)
      goto stall;
    DeferredCommandReady = 0;
    DeferredCommand = DEFERRED_VENDOR_COMMAND_STOP;
    Fx3UsbUnstallEp0(s);
    return;
#else
    stop_acquisition();
    Fx3GctlSetPllFbDiv(PLL_FBDIV);
    Fx3UsbUnstallEp0(s);
    return;
#endif
  case CMD_RESET:
    if (request_type !=
	(FX3_USB_REQTYPE_OUT | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != 0)
      goto stall;
    Fx3UartTxString("CMD_RESET\n");
    stop_acquisition();
    Fx3UsbUnstallEp0(s);
    Fx3UartTxFlush();
    Fx3UtilDelayUs(100000);
    Fx3GctlHardReset();
    return;
  case CMD_START:
    if (request_type !=
	(FX3_USB_REQTYPE_OUT | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != sizeof(struct cmd_start_acquisition))
      goto stall;
    Fx3UartTxString("CMD_START\n");
    Fx3UsbUnstallEp0(s);
    Fx3UsbDmaDataOut(0, DmaBuf, sizeof(struct cmd_start_acquisition));
    Fx3CacheInvalidateDCacheEntry(DmaBuf);
    volatile struct cmd_start_acquisition *cmd = (volatile struct cmd_start_acquisition *)DmaBuf;
    {
      char buf[64];
      snprintf(buf, sizeof(buf), "flags=%02x, sample_delay_h=%02x, sample_delay_l=%02x\n",
	       (unsigned)cmd->flags, (unsigned)cmd->sample_delay_h, (unsigned)cmd->sample_delay_l);
      Fx3UartTxString(buf);
    }

    uint8_t flags = cmd->flags;
#ifdef FX3_ULPI_SNIFFER
    Fx3GctlSetPllFbDiv(PLL_FBDIV);
    start_acquisition(16, 0,
		      CLOCK_DIVISOR_X2_FOR(SYS_CLK, 60000000),
		      1,
		      (flags & CMD_START_FLAGS_CLK_INVERT) != 0);
#else
    uint16_t sample_delay = (cmd->sample_delay_h<<8)|cmd->sample_delay_l;
    uint8_t bits = 8;
    if (flags & (1<<CMD_START_FLAGS_WIDE_POS))
      bits = 16;
    if (flags & (1<<CMD_START_FLAGS_SUPERWIDE_POS))
      bits += 16;
    uint16_t clock_divisor_x2;
    uint8_t pll_fbdiv = PLL_FBDIV;
    uint32_t sys_clk = SYS_CLK;
    switch (flags & (CMD_START_FLAGS_CLK_CTL2 | CMD_START_FLAGS_CLK_SRC_MASK)) {
    case CMD_START_FLAGS_CLK_89MHZ:
      pll_fbdiv = FX3LAFW_89MHZ_PLL_FBDIV;
      sys_clk = FX3LAFW_89MHZ_SYS_CLK;
      clock_divisor_x2 = CLOCK_DIVISOR_X2_FOR(sys_clk, 89600000); /* 89.6 MHz */
      break;
    case CMD_START_FLAGS_CLK_192MHZ:
      clock_divisor_x2 = CLOCK_DIVISOR_X2_FOR(sys_clk, 192000000); /* 192 MHz */
      break;
    case CMD_START_FLAGS_CLK_80MHZ:
      clock_divisor_x2 = CLOCK_DIVISOR_X2_FOR(sys_clk, 80000000); /* 80 MHz */
      break;
    case CMD_START_FLAGS_CLK_48MHZ:
      clock_divisor_x2 = CLOCK_DIVISOR_X2_FOR(sys_clk, 48000000); /* 48 MHz */
      break;
    default:
      clock_divisor_x2 = CLOCK_DIVISOR_X2_FOR(sys_clk, 30000000); /* 30 MHz */
      break;
    }

    stop_acquisition();
    Fx3GctlSetPllFbDiv(pll_fbdiv);
    start_acquisition(bits, sample_delay, clock_divisor_x2,
		      (flags & CMD_START_FLAGS_EXT_CLOCK) != 0,
		      (flags & CMD_START_FLAGS_CLK_INVERT) != 0);
#endif

    return;
  case CMD_GET_FW_VERSION:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != sizeof(struct version_info))
      goto stall;
    Fx3UartTxString("CMD_GET_FW_VERSION\n");
    volatile struct version_info *vinfo = (volatile struct version_info *)DmaBuf;
    vinfo->major = 1;
    vinfo->minor = 23;
    Fx3CacheCleanDCacheEntry(DmaBuf);
    Fx3UsbUnstallEp0(s);
    Fx3UsbDmaDataIn(0, DmaBuf, sizeof(struct version_info));
    return;
  case CMD_GET_ACQ_STATUS:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != sizeof(struct acquisition_status))
      goto stall;
    Fx3UartTxString("CMD_GET_ACQ_STATUS\n");
    volatile struct acquisition_status *status =
      (volatile struct acquisition_status *)DmaBuf;
    get_acquisition_status(status);
#ifdef FX3_ULPI_SNIFFER
    if (DeferredCommand != DEFERRED_VENDOR_COMMAND_NONE)
      status->reserved |= ACQ_STATUS_FLAGS_STOP_PENDING;
#endif
    Fx3CacheCleanDCacheEntry(DmaBuf);
    Fx3CacheCleanDCacheEntry(DmaBuf + 32);
    Fx3CacheCleanDCacheEntry(DmaBuf + 64);
    Fx3CacheCleanDCacheEntry(DmaBuf + 96);
    Fx3UsbUnstallEp0(s);
    Fx3UsbDmaDataIn(0, DmaBuf, sizeof(struct acquisition_status));
    return;
  case CMD_GET_REVID_VERSION:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_VENDOR | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 0 || index != 0 || length != 1)
      goto stall;
    Fx3UartTxString("CMD_GET_REVID_VERSION\n");
    DmaBuf[0] = 1;
    Fx3CacheCleanDCacheEntry(DmaBuf);
    Fx3UsbUnstallEp0(s);
    Fx3UsbDmaDataIn(0, DmaBuf, 1);
    return;
  }
 stall:
  Fx3UsbStallEp0(s);
}

static void SetupData(uint8_t request_type, uint8_t request, uint16_t value,
		      uint16_t index, uint16_t length, Fx3UsbSpeed_t s)
{
  char buf[64];
  snprintf(buf, sizeof(buf),
	   "req: %02x %02x value: %04x index: %04x length: %04x\n",
	   (unsigned)request_type, (unsigned)request,
	   (unsigned)value, (unsigned)index, (unsigned)length);
  Fx3UartTxString(buf);

  if ((request_type & FX3_USB_REQTYPE_TYPE_MASK) == FX3_USB_REQTYPE_TYPE_VENDOR) {
    VendorCommand(request_type, request, value, index, length, s);
    return;
  }

  if ((request_type & FX3_USB_REQTYPE_TYPE_MASK) != FX3_USB_REQTYPE_TYPE_STD)
    goto stall;

  switch(request) {
  case FX3_USB_STD_REQUEST_CLEAR_FEATURE:
    if (request_type != (FX3_USB_REQTYPE_OUT | FX3_USB_REQTYPE_TYPE_STD |
			 FX3_USB_REQTYPE_TGT_EP) || value != 0 ||
	index != 0x82 || length != 0)
      goto stall;
    /* WinUSB may clear EP_HALT when cancelling outstanding bulk reads. */
    stop_acquisition();
    if (!Fx3UsbClearInEndpointHalt(2, s))
      goto stall;
    Fx3UartTxString("CLEAR_HALT EP82 complete\n");
    Fx3UsbUnstallEp0(s);
    return;

#ifdef FX3_ULPI_SNIFFER
  case FX3_USB_STD_REQUEST_GET_STATUS:
    if ((request_type & (FX3_USB_REQTYPE_DIR_MASK |
			 FX3_USB_REQTYPE_TYPE_MASK)) !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_STD))
      goto stall;
    if (value != 0 || length != 2 || (index & 0xff00) != 0)
      goto stall;
    switch (request_type & FX3_USB_REQTYPE_TGT_MASK) {
    case FX3_USB_REQTYPE_TGT_DEVICE:
      if (index != 0)
	goto stall;
      break;
    case FX3_USB_REQTYPE_TGT_INTERFC:
      if (index != 0)
	goto stall;
      break;
    case FX3_USB_REQTYPE_TGT_EP:
      if (index != 0x00 && index != 0x80 && index != 0x82)
	goto stall;
      break;
    default:
      goto stall;
    }
    DmaBuf[0] = 0;
    DmaBuf[1] = 0;
    Fx3CacheCleanDCacheEntry(DmaBuf);
    Fx3UsbUnstallEp0(s);
    Fx3UsbDmaDataIn(0, DmaBuf, 2);
    return;
#endif
  case FX3_USB_STD_REQUEST_GET_DESCRIPTOR:
    if (request_type !=
	(FX3_USB_REQTYPE_IN | FX3_USB_REQTYPE_TYPE_STD | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    const uint8_t *descr = GetDescriptor(value>>8, value&0xff, s);
    if (!descr) goto stall;
    uint8_t descr_type = descr[1];
    uint16_t len = (descr_type == FX3_USB_DESCRIPTOR_CONFIGURATION ||
		    descr_type == FX3_USB_DESCRIPTOR_BOS?
		    *(const uint16_t *)(descr+2) : *descr);
    if (len < length)
      length = len;
    Fx3UsbUnstallEp0(s);
    Fx3UsbDmaDataIn(0, descr, length);
    return;

  case FX3_USB_STD_REQUEST_SET_CONFIGURATION:
    if (request_type !=
	(FX3_USB_REQTYPE_OUT | FX3_USB_REQTYPE_TYPE_STD | FX3_USB_REQTYPE_TGT_DEVICE))
      goto stall;
    if (value != 1)
      goto stall;

    Fx3UsbEnableInEndpoint(2, FX3_USB_EP_BULK, s == FX3_USB_HIGH_SPEED ? 512 : 1024);
    Fx3UsbUnstallEp0(s);
    return;
  }

 stall:
  Fx3UsbStallEp0(s);
}



int main(void)
{
  static const struct Fx3UsbCallbacks callbacks = {
    .sutok = SetupData,
#ifdef FX3_ULPI_SNIFFER
    .status_stage = UsbStatusStage,
#endif
  };
#ifndef FX3_ULPI_SNIFFER
  uint32_t blink_half_us;
#endif

  Fx3CacheEnableCaches();
  Fx3IrqInit();

  Fx3GctlInitClock();
  *(volatile uint32_t *)(void *)0x400020e8 = 0;
  Fx3GctlInitIoMatrix(FX3_GCTL_ALTFUNC_GPIF32BIT_UART_I2S);

  /*
   * Bring the console up before anything that can stall. Boot runs entirely
   * before Fx3UsbConnect(), so without this a hang anywhere below is
   * indistinguishable from a dead board.
   */
  Fx3UartInit(115200, FX3_UART_NO_PARITY, FX3_UART_1_STOP_BIT);
  Fx3UartTxString("\nGood moaning!\n");
  Fx3UartTxFlush();

#ifdef FX3_ULPI_SNIFFER
  Fx3UartTxString("boot: ulpi_init\n");
  ulpi_init();
#endif

  Fx3UartTxString("boot: gpio\n");
  Fx3GpioInitClock();
#ifndef FX3_ULPI_SNIFFER
  Fx3GpioSetupSimple(45,
		     FX3_GPIO_SIMPLE_ENABLE |
		     FX3_GPIO_SIMPLE_INPUT_EN);
#endif
  Fx3GpioSetupSimple(54,
		     FX3_GPIO_SIMPLE_ENABLE |
		     FX3_GPIO_SIMPLE_DRIVE_HI_EN |
		     FX3_GPIO_SIMPLE_DRIVE_LO_EN);

  // Divide the GPIO slow clock into a 1kHz clock output for reference
  Fx3GpioSetupComplex(50,
		      FX3_PIN_STATUS_ENABLE |
		      (FX3_GPIO_TIMER_MODE_SLOW_CLK << FX3_PIN_STATUS_TIMER_MODE_SHIFT) |
		      (FX3_GPIO_PIN_MODE_PWM << FX3_PIN_STATUS_MODE_SHIFT) |
		      FX3_PIN_STATUS_DRIVE_HI_EN |
		      FX3_PIN_STATUS_DRIVE_LO_EN,
		      0, GPIO_SLOW_CLK/1000-1, GPIO_SLOW_CLK/2000);

  Fx3UartTxString("boot: irq\n");
  Fx3IrqEnableInterrupts();

  Fx3UartTxString("boot: setup_acquisition\n");
  setup_acquisition();

  Fx3UartTxString("boot: usb_init\n");
  Fx3UsbInit(&callbacks);
  Fx3UartTxString("boot: usb_connect\n");
  Fx3UsbConnect();
  Fx3UartTxString("boot: running\n");
  Fx3UartTxFlush();

  /*
   * The console is not reachable on every board, so encode whether the USB
   * PHY was actually brought up in the LED blink rate: a device that reaches
   * this point but never enumerates is otherwise indistinguishable from a
   * dead one. Slow (1 Hz) = VBUS seen and PHY enabled, fast (5 Hz) = no VBUS.
   */
#ifndef FX3_ULPI_SNIFFER
  blink_half_us = Fx3UsbVbusSeen ? 500000 : 100000;
#endif

#ifdef FX3_ULPI_SNIFFER
  for (;;) {
    Fx3UsbPoll();
    Fx3UsbLinkCheck();
    PollDeferredVendorCommand();
    poll_acquisition();
  }
#else
  for (;;) {
    poll_acquisition();
    Fx3GpioSetOutputValueSimple(54, 1);
    Fx3UtilDelayUs(blink_half_us);
    Fx3UsbLinkCheck();
    if (!Fx3GpioGetInputValueSimple(45)) {
      Fx3UartTxString("BUTTON\n");
      Fx3UartTxFlush();
      Fx3GctlHardReset();
    }
    Fx3GpioSetOutputValueSimple(54, 0);
    Fx3UtilDelayUs(blink_half_us);
  }
#endif
}

/* Referenced by newlib */
FILE __sf[3];

/* Workaround for handling newlib being compiled with ssp */
uintptr_t __stack_chk_guard = 0x00000aff;
void __stack_chk_fail(void)
{
  for(;;);
}

/* Newlib's assert() calls this function if the assertion fails */
void
__assert_func (const char *file,
        int line,
        const char *func,
        const char *failedexpr)
{
  if (file != NULL) {
    char linestrbuf[16], *linestr = &linestrbuf[sizeof(linestrbuf)];
    Fx3UartTxString(file);
    Fx3UartTxChar(':');
    /* Avoid using newlib functions like itoa so as not to trigger
       a recursive assert... */
    *--linestr = '\0';
    while (line >= 10 && linestr != &linestrbuf[1]) {
      *--linestr = '0' + (line % 10);
      line /= 10;
    }
    *--linestr = '0' + line;
    Fx3UartTxString(linestr);
    Fx3UartTxString(": ");
  }
  if (func != NULL) {
    Fx3UartTxString(func);
    Fx3UartTxString(": ");
  }
  Fx3UartTxString("Assertion ");
  if (failedexpr != NULL) {
    Fx3UartTxChar('`');
    Fx3UartTxString(failedexpr);
    Fx3UartTxString("' ");
  }
  Fx3UartTxString("failed.\n");
  for(;;)
    ;
}
