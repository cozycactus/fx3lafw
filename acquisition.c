#include <bsp/gpif.h>
#include <bsp/cache.h>
#include <bsp/dma.h>
#include <bsp/gpio.h>
#include <bsp/usb.h>
#include <bsp/uart.h>
#include <bsp/regaccess.h>
#include <bsp/util.h>
#include <rdb/dma.h>
#include <rdb/gpio.h>
#include <rdb/gpif.h>
#include <rdb/pib.h>
#include <rdb/uib.h>
#include <stdio.h>

#include "command.h"

#ifdef FX3_ULPI_SNIFFER
#define NUM_DMA_BUFFERS 16
#define DMA_BUFFER_SIZE 24576
#else
#define NUM_DMA_BUFFERS 16
#define DMA_BUFFER_SIZE 24576 /* Needs to be divisable by 12 */
#endif

#ifdef FX3_ULPI_SNIFFER
#define ULPI_DIR_GPIO_MASK (1UL << 19)
#define ULPI_SAMPLE_GPIO_FIRST 8
#define ULPI_SAMPLE_GPIO_LAST 15
#endif
#ifdef FX3_ULPI_SNIFFER
#define ACQUISITION_SAMPLE_ALPHA \
  (FX3_GPIF_ALPHA_SAMPLE_DIN | FX3_GPIF_ALPHA_UPDATE_DOUT)
#else
#define ACQUISITION_SAMPLE_ALPHA FX3_GPIF_ALPHA_SAMPLE_DIN
#endif

static const uint16_t functions[]  = {
  [0] = 0U,  /* Constant 0 */
  [1] = (uint16_t)~0U, /* Constant 1 */
  [2] = FX3_GPIF_FUNCTION_Fa, /* Fa */
  [3] = (uint16_t)~FX3_GPIF_FUNCTION_Fa, /* !Fa */
  [4] = FX3_GPIF_FUNCTION_Fb, /* Fb */
  [5] = (uint16_t)~FX3_GPIF_FUNCTION_Fb, /* !Fb */
  [6] = (uint16_t)(~FX3_GPIF_FUNCTION_Fa | FX3_GPIF_FUNCTION_Fc),
  [7] = FX3_GPIF_FUNCTION_Fa & FX3_GPIF_FUNCTION_Fb,
  [8] = FX3_GPIF_FUNCTION_Fa & (uint16_t)~FX3_GPIF_FUNCTION_Fb,
  /* !DMA_RDY & !ADDR_HIT: mid-buffer stall without stealing the
   * buffer-boundary thread switch (ULPI dual-producer lockstep). */
  [9] = (uint16_t)~FX3_GPIF_FUNCTION_Fa & (uint16_t)~FX3_GPIF_FUNCTION_Fb,
};

static const Fx3GpifWaveform_t waveforms[] = {
  [0] = { GPIF_START_STATE(0), .left=13 },
  [1] = { GPIF_START_STATE(1), .left=8 },
  [2] = { GPIF_START_STATE(2), .left=3 },

  /* Delay >= 2 */

  [3] = { GPIF_STATE(3, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 2, 0,
		     0, 0,
		     FX3_GPIF_BETA_THREAD_0 |
		     FX3_GPIF_BETA_LD_DATA_COUNT |
		     FX3_GPIF_BETA_LD_ADDR_COUNT, 0, 0), .left = 4 },

  /* Thread 0 */

  [4] = { GPIF_STATE(4, FX3_GPIF_LAMBDA_INDEX_ADDR_CNT_HIT /* Fa */,
		     FX3_GPIF_LAMBDA_INDEX_DATA_CNT_HIT /* Fb */,
		     0, 0, 4, 2,
			     ACQUISITION_SAMPLE_ALPHA, 0,
		     FX3_GPIF_BETA_THREAD_0 |
		     FX3_GPIF_BETA_COUNT_DATA, 0, 0), .left = 5, .right = 6 },
  [5] = { GPIF_STATE(5, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1,
		     0, 0,
		     FX3_GPIF_BETA_THREAD_0 |
		     FX3_GPIF_BETA_WQ_PUSH |
		     FX3_GPIF_BETA_COUNT_DATA |
		     FX3_GPIF_BETA_COUNT_ADDR,
		     0, 0), .left = 16, .right=4 },

  /* Thread 1 */

  [6] = { GPIF_STATE(6, FX3_GPIF_LAMBDA_INDEX_ADDR_CNT_HIT /* Fa */,
		     FX3_GPIF_LAMBDA_INDEX_DATA_CNT_HIT /* Fb */,
		     0, 0, 4, 2,
			     ACQUISITION_SAMPLE_ALPHA, 0,
		     FX3_GPIF_BETA_THREAD_1 |
		     FX3_GPIF_BETA_COUNT_DATA, 0, 0), .left = 7, .right = 4 },
  [7] = { GPIF_STATE(7, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1,
		     0, 0,
		     FX3_GPIF_BETA_THREAD_1 |
		     FX3_GPIF_BETA_WQ_PUSH |
		     FX3_GPIF_BETA_COUNT_DATA |
		     FX3_GPIF_BETA_COUNT_ADDR,
		     0, 0), .left = 17, .right=6 },

  /* Delay == 1 */

  [8] = { GPIF_STATE(8, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 2, 0,
		     0, 0,
		     FX3_GPIF_BETA_THREAD_0 |
		     FX3_GPIF_BETA_LD_ADDR_COUNT, 0, 0), .left = 9 },

  /* Thread 0 */

  [9] = { GPIF_STATE(9, FX3_GPIF_LAMBDA_INDEX_ADDR_CNT_HIT, 0, 0, 0, 2, 1,
			     ACQUISITION_SAMPLE_ALPHA, 0,
		     FX3_GPIF_BETA_THREAD_0, 0, 0), .left = 12, .right = 10 },
  [10] = { GPIF_STATE(10, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1,
		      0, 0,
		      FX3_GPIF_BETA_THREAD_0 |
		      FX3_GPIF_BETA_WQ_PUSH |
		      FX3_GPIF_BETA_COUNT_ADDR,
		      0, 0), .left = 18, .right=9 },

  /* Thread 1 */

  [11] = { GPIF_STATE(11, FX3_GPIF_LAMBDA_INDEX_ADDR_CNT_HIT, 0, 0, 0, 2, 1,
			      ACQUISITION_SAMPLE_ALPHA, 0,
		      FX3_GPIF_BETA_THREAD_1, 0, 0), .left = 9, .right = 12 },
  [12] = { GPIF_STATE(12, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1,
		      0, 0,
		      FX3_GPIF_BETA_THREAD_1 |
		      FX3_GPIF_BETA_WQ_PUSH |
		      FX3_GPIF_BETA_COUNT_ADDR,
		      0, 0), .left = 19, .right=11 },

  /* Delay == 0 */

  [13] = { GPIF_STATE(13, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 2, 0,
			      ACQUISITION_SAMPLE_ALPHA, 0,
		      FX3_GPIF_BETA_THREAD_0 |
		      FX3_GPIF_BETA_LD_ADDR_COUNT, 0, 0), .left = 14 },

  /* Thread 0 */

  [14] = { GPIF_STATE(14, FX3_GPIF_LAMBDA_INDEX_DMA_RDY /* Fa */,
		      FX3_GPIF_LAMBDA_INDEX_ADDR_CNT_HIT /* Fb */,
		      0, 0, 3, 4,
			      0, ACQUISITION_SAMPLE_ALPHA,
		      FX3_GPIF_BETA_THREAD_0 |
		      FX3_GPIF_BETA_WQ_PUSH |
		      FX3_GPIF_BETA_COUNT_ADDR, 0, 0), .left = 20, .right = 15 },

  /* Thread 1 */

  [15] = { GPIF_STATE(15, FX3_GPIF_LAMBDA_INDEX_DMA_RDY /* Fa */,
		      FX3_GPIF_LAMBDA_INDEX_ADDR_CNT_HIT /* Fb */,
		      0, 0, 3, 4,
			      0, ACQUISITION_SAMPLE_ALPHA,
		      FX3_GPIF_BETA_THREAD_1 |
		      FX3_GPIF_BETA_WQ_PUSH |
		      FX3_GPIF_BETA_COUNT_ADDR, 0, 0), .left = 21, .right = 14 },

  /* Wait for DMA readiness without repeating WQ_PUSH or sample/count side effects. */

  [16] = { GPIF_STATE(16, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1, 0, 0,
		      FX3_GPIF_BETA_THREAD_0, 0, 0), .left = 16, .right = 5 },
  [17] = { GPIF_STATE(17, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1, 0, 0,
		      FX3_GPIF_BETA_THREAD_1, 0, 0), .left = 17, .right = 7 },
  [18] = { GPIF_STATE(18, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1, 0, 0,
		      FX3_GPIF_BETA_THREAD_0, 0, 0), .left = 18, .right = 10 },
  [19] = { GPIF_STATE(19, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1, 0, 0,
		      FX3_GPIF_BETA_THREAD_1, 0, 0), .left = 19, .right = 12 },
  [20] = { GPIF_STATE(20, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1, 0, 0,
		      FX3_GPIF_BETA_THREAD_0, 0, 0), .left = 20, .right = 14 },
  [21] = { GPIF_STATE(21, FX3_GPIF_LAMBDA_INDEX_DMA_RDY, 0, 0, 0, 3, 1, 0, 0,
		      FX3_GPIF_BETA_THREAD_1, 0, 0), .left = 21, .right = 15 },

  /* Done */

  [22] = { GPIF_STATE(22, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0), .left = 0 },
};

#ifdef FX3_ULPI_SNIFFER
/*
 * Reuse the stock delay==0 acquisition waveform. It switches GPIF threads
 * on ADDR_CNT_HIT, exactly after one complete DMA buffer, and has already
 * been validated by the normal logic-analyzer path.
 */
enum ulpi_acquisition_state {
  ULPI_ACQ_START = 13,
  ULPI_ACQ_SAMPLE_0 = 14,
  ULPI_ACQ_SAMPLE_1 = 15,
  ULPI_ACQ_WAIT_0 = 20,
  ULPI_ACQ_WAIT_1 = 21,
};

#define ulpi_acquisition_waveforms waveforms
#endif

static Fx3GpifRegisters_t registers = {
  .config = (FX3_GPIF_CONFIG_ENABLE |
	     FX3_GPIF_CONFIG_THREAD_IN_STATE |
	     FX3_GPIF_CONFIG_CLK_SOURCE),
#ifdef FX3_ULPI_SNIFFER
  /*
   * CTL2/DIR gates the lower data drivers: drive ULPI Idle while DIR=0,
   * release DATA immediately while DIR=1. DQ8..15 remain GPIO inputs.
   * The unused data counter stays zero in the delay==0 waveform.
   */
  .bus_config = FX3_GPIF_BUS_CONFIG_OE_PRESENT,
  .bus_config2 = 0,
  .ad_config = (FX3_GPIF_OEN_CFG_INPUT <<
                  FX3_GPIF_AD_CONFIG_A_OEN_CFG_SHIFT) |
               (FX3_GPIF_OEN_CFG_OUTPUT <<
                  FX3_GPIF_AD_CONFIG_DQ_OEN_CFG_SHIFT) |
               FX3_GPIF_AD_CONFIG_DOUT_SELECT,
#else
  .ad_config = (FX3_GPIF_OEN_CFG_INPUT << FX3_GPIF_AD_CONFIG_A_OEN_CFG_SHIFT) |
               (FX3_GPIF_OEN_CFG_INPUT << FX3_GPIF_AD_CONFIG_DQ_OEN_CFG_SHIFT),
#endif
#ifdef FX3_ULPI_SNIFFER
  .ctrl_bus_direction =
    (FX3_GPIF_CTRL_BUS_DIRECTION_OUTPUT << (3 * 2)),
  .ctrl_bus_polarity = 1UL << 2,
  .ctrl_bus_select[3] = FX3_GPIF_OMEGA_INDEX_ALPHA4,
#endif
  .data_count_config = (1UL << FX3_GPIF_DATA_COUNT_CONFIG_INCREMENT_SHIFT) |
                       FX3_GPIF_DATA_COUNT_CONFIG_DOWN_UP |
                       FX3_GPIF_DATA_COUNT_CONFIG_RELOAD |
                       FX3_GPIF_DATA_COUNT_CONFIG_ENABLE,
  .addr_count_config = (1UL << FX3_GPIF_ADDR_COUNT_CONFIG_INCREMENT_SHIFT) |
                       FX3_GPIF_ADDR_COUNT_CONFIG_DOWN_UP |
                       FX3_GPIF_ADDR_COUNT_CONFIG_RELOAD |
                       FX3_GPIF_ADDR_COUNT_CONFIG_ENABLE,
  .thread_config[0] = (FX3_GPIF_THREAD_CONFIG_ENABLE |
		       (1UL << FX3_GPIF_THREAD_CONFIG_WATERMARK_SHIFT) |
		       (4UL << FX3_GPIF_THREAD_CONFIG_BURST_SIZE_SHIFT) |
		       (0UL << FX3_GPIF_THREAD_CONFIG_THREAD_SOCK_SHIFT)),
  .thread_config[1] = (FX3_GPIF_THREAD_CONFIG_ENABLE |
		       (1UL << FX3_GPIF_THREAD_CONFIG_WATERMARK_SHIFT) |
		       (4UL << FX3_GPIF_THREAD_CONFIG_BURST_SIZE_SHIFT) |
		       (1UL << FX3_GPIF_THREAD_CONFIG_THREAD_SOCK_SHIFT)),
  .waveform_switch = ((22UL << FX3_GPIF_WAVEFORM_SWITCH_DONE_STATE_SHIFT) |
			      FX3_GPIF_WAVEFORM_SWITCH_DONE_ENABLE),
  .beta_deassert = 0xFFFFFFC1UL,
};

static volatile uint8_t gpif_buf[NUM_DMA_BUFFERS][DMA_BUFFER_SIZE] __attribute__((aligned(32)));
static uint32_t dma_buffer_descriptor[NUM_DMA_BUFFERS];
static uint32_t pause_count;
#ifdef FX3_ULPI_SNIFFER
/*
 * Mid-acq DMA rebuild starved UsbPoll and wedged the SS endpoint. On
 * overflow, stop cleanly so control/EP2 stay alive for a fast host abort.
 */
#endif
static uint8_t pause_gpif_stat;
static uint8_t pause_gpif_state;
static uint16_t pause_pib_error;
static uint32_t pause_gpif_status;
static uint32_t pause_pib_sck0_status;
static uint32_t pause_pib_sck0_dscr;
static uint32_t pause_pib_sck0_count;
static uint32_t pause_pib_sck1_status;
static uint32_t pause_pib_sck1_dscr;
static uint32_t pause_pib_sck1_count;
static uint32_t pause_uib_sck2_status;
static uint32_t pause_uib_sck2_dscr;
static uint32_t pause_uib_sck2_count;

#ifdef FX3_ULPI_SNIFFER
static struct ulpi_acquisition_status ulpi_acquisition_status;
static uint8_t ulpi_acquisition_active;
static uint8_t ulpi_dma_armed;
static uint16_t ulpi_dll_setting;
static uint8_t ulpi_dll_config;

static void ulpi_override_sample_gpio(uint8_t enable)
{
  uint8_t gpio;

  for (gpio = ULPI_SAMPLE_GPIO_FIRST;
       gpio <= ULPI_SAMPLE_GPIO_LAST; gpio++) {
    if (enable)
      Fx3GpioSetupSimple(gpio,
                         FX3_GPIO_SIMPLE_ENABLE |
                         FX3_GPIO_SIMPLE_INPUT_EN);
    else
      Fx3GpioReleaseSimple(gpio);
  }
}

static void ulpi_apply_core_phase(void)
{
  uint32_t control;

  Fx3ClearReg32(FX3_PIB_DLL_CTRL, FX3_PIB_DLL_CTRL_ENABLE);
  Fx3UtilDelayUs(1);
  if (ulpi_dll_config == ULPI_DLL_CONFIG_PHASE &&
      ulpi_dll_setting == ULPI_CORE_PHASE_DLL_OFF)
    return;

  control = Fx3ReadReg32(FX3_PIB_DLL_CTRL) &
            FX3_PIB_DLL_CTRL_HIGH_FREQ;
  if (ulpi_dll_config == ULPI_DLL_CONFIG_FIXED_DELAY) {
    control |=
      ((uint32_t)ulpi_dll_setting <<
        FX3_PIB_DLL_CTRL_DLL_SLAVE_DLY_SHIFT) |
      FX3_PIB_DLL_CTRL_DLL_MODE |
      FX3_PIB_DLL_CTRL_DLL_DFT_MODE_MASK;
  } else {
    control |=
      (uint32_t)ulpi_dll_setting <<
        FX3_PIB_DLL_CTRL_CORE_PHASE_SELECT_SHIFT;
  }
  control |= FX3_PIB_DLL_CTRL_ENABLE;
  Fx3WriteReg32(FX3_PIB_DLL_CTRL, control);
  Fx3UtilDelayUs(1);
  Fx3ClearReg32(FX3_PIB_DLL_CTRL, FX3_PIB_DLL_CTRL_DLL_RESET_N);
  Fx3UtilDelayUs(1);
  Fx3SetReg32(FX3_PIB_DLL_CTRL, FX3_PIB_DLL_CTRL_DLL_RESET_N);
  Fx3UtilDelayUs(1);
  if (ulpi_dll_config == ULPI_DLL_CONFIG_PHASE) {
    while (!(Fx3ReadReg32(FX3_PIB_DLL_CTRL) &
             FX3_PIB_DLL_CTRL_DLL_STAT))
      ;
  }
}
#endif

static void uart_tx_u8_dec(uint8_t value)
{
  if (value >= 100)
    Fx3UartTxChar('0' + value / 100);
  if (value >= 10)
    Fx3UartTxChar('0' + (value / 10) % 10);
  Fx3UartTxChar('0' + value % 10);
}

void stop_acquisition(void)
{
#ifdef FX3_ULPI_SNIFFER
  uint8_t dma_armed = ulpi_dma_armed;

  ulpi_acquisition_active = 0;
  Fx3GpifStop();
  if (dma_armed) {
    Fx3DmaAbortSocket(FX3_PIB_DMA_SCK(0));
    Fx3DmaAbortSocket(FX3_PIB_DMA_SCK(1));
    Fx3DmaAbortSocket(FX3_UIB_DMA_SCK(2));
    ulpi_dma_armed = 0;
  }
#else
  /* Quiesce USB before flushing DMA/EPM; a packet may still be in flight. */
  Fx3GpifStop();
  Fx3GpifInvalidate();
  Fx3UsbSetInEndpointNak(2, 1);
  Fx3UtilDelayUs(125);
  Fx3DmaAbortSocket(FX3_PIB_DMA_SCK(0));
  Fx3DmaAbortSocket(FX3_PIB_DMA_SCK(1));
  Fx3DmaAbortSocket(FX3_UIB_DMA_SCK(2));
#endif
  Fx3GpifPibStop();
#ifdef FX3_ULPI_SNIFFER
  ulpi_override_sample_gpio(1);
  if (dma_armed)
    Fx3UsbFlushInEndpoint(2);
#else
  Fx3UsbFlushInEndpoint(2);
#endif
}

static void setup_descriptors(void)
{
  unsigned i;
  for(i=0; i<NUM_DMA_BUFFERS; i++) {
    unsigned next = (i==NUM_DMA_BUFFERS-1? 0 : i+1);
    unsigned nextnext = (next==NUM_DMA_BUFFERS-1? 0 : next+1);
    Fx3DmaFillDescriptorThrough(FX3_PIB_DMA_SCK(i&1), FX3_UIB_DMA_SCK(2),
				dma_buffer_descriptor[i], gpif_buf[i],
				DMA_BUFFER_SIZE,
				dma_buffer_descriptor[nextnext],
				dma_buffer_descriptor[next]);
  }
}

void start_acquisition(uint8_t bits, uint32_t delay, uint16_t clock_divisor_x2,
		       uint8_t external_clock, uint8_t invert_clock)
{
  registers.config &= ~(FX3_GPIF_CONFIG_CLK_SOURCE |
			      FX3_GPIF_CONFIG_CLK_INVERT |
			      FX3_GPIF_CONFIG_CLK_OUT |
			      FX3_GPIF_CONFIG_SYNC
#ifdef FX3_ULPI_SNIFFER
			      | FX3_GPIF_CONFIG_SYNC_SPEED
#endif
			      );
  if (external_clock) {
    registers.config |= FX3_GPIF_CONFIG_SYNC;
  } else {
    registers.config |= FX3_GPIF_CONFIG_CLK_SOURCE;
  }
  if (invert_clock)
    registers.config |= FX3_GPIF_CONFIG_CLK_INVERT;

  registers.bus_config &= ~FX3_GPIF_BUS_CONFIG_BUS_WIDTH_MASK;
  registers.bus_config |= ((bits >> 3) - 1) << FX3_GPIF_BUS_CONFIG_BUS_WIDTH_SHIFT;

  uint32_t samples_per_buffer =  DMA_BUFFER_SIZE * 8 / bits;
  if (!delay)
    --samples_per_buffer;
  registers.data_count_limit = delay;
  registers.addr_count_limit = samples_per_buffer;

#ifndef FX3_ULPI_SNIFFER
  stop_acquisition();
#else
  /*
   * CMD_STOP already reset the DMA sockets and endpoint. The cached ULPI
   * mode command leaves the pin-monitor waveform running, so stop only GPIF
   * here before replacing it with the acquisition waveform.
   */
  Fx3GpifStop();
  Fx3GpifPibStop();
#endif
  pause_count = 0;
  pause_pib_error = 0;
  setup_descriptors();

#ifdef FX3_ULPI_SNIFFER
  /*
   * Never drive the mirrored PHY controls when the 16-bit GPIF data output
   * is enabled. GPIO override disables output drivers on DQ8..15 only.
   */
  ulpi_override_sample_gpio(1);
#endif

  /*
   * With an external interface clock, clock_divisor_x2 only describes the
   * incoming pin clock; it must not throttle the PIB core. Deriving the core
   * clock from it put the socket/DMA side at SYS_CLK/6.5 = 59 MHz behind a
   * 60 MHz pin clock, i.e. the DMA sink was slower than the GPIF source and
   * TH*_WR_OVERFLOW was only a matter of time. Run the core at SYS_CLK/2 as
   * Cypress' own GPIF designs do.
   */
  Fx3GpifPibStartEx(external_clock ? 4 : clock_divisor_x2, clock_divisor_x2);
#ifdef FX3_ULPI_SNIFFER
  ulpi_apply_core_phase();
  Fx3GpifConfigure(ulpi_acquisition_waveforms,
                   sizeof(ulpi_acquisition_waveforms) /
                     sizeof(ulpi_acquisition_waveforms[0]),
                   functions, sizeof(functions) / sizeof(functions[0]),
                   &registers);
#else
  Fx3GpifConfigure(waveforms,
		   sizeof(waveforms)/sizeof(waveforms[0]),
		   functions, sizeof(functions)/sizeof(functions[0]),
		   &registers);
#endif
#ifdef FX3_ULPI_SNIFFER
  ulpi_acquisition_status = (struct ulpi_acquisition_status){0};
  ulpi_acquisition_status.bus_config =
    Fx3ReadReg32(FX3_GPIF_BUS_CONFIG);
  ulpi_acquisition_status.bus_config2 =
    Fx3ReadReg32(FX3_GPIF_BUS_CONFIG2);
  ulpi_acquisition_status.ad_config = Fx3ReadReg32(FX3_GPIF_AD_CONFIG);
  ulpi_acquisition_status.ctrl_bus_direction =
    Fx3ReadReg32(FX3_GPIF_CTRL_BUS_DIRECTION);
  ulpi_acquisition_status.ctrl_bus_polarity =
    Fx3ReadReg32(FX3_GPIF_CTRL_BUS_POLARITY);
  ulpi_acquisition_status.sample_base_word1 =
    Fx3ReadReg32(FX3_GPIF_RIGHT_WAVEFORM +
                 ULPI_ACQ_SAMPLE_0 * 16 + 4);
  ulpi_acquisition_status.sample_mirror_word1 =
    Fx3ReadReg32(FX3_GPIF_RIGHT_WAVEFORM +
                 ULPI_ACQ_SAMPLE_1 * 16 + 4);
  ulpi_acquisition_status.gpif_config =
    Fx3ReadReg32(FX3_GPIF_CONFIG);
  ulpi_acquisition_status.pib_dll_ctrl =
    Fx3ReadReg32(FX3_PIB_DLL_CTRL);
  ulpi_acquisition_status.core_phase = ulpi_dll_setting;
  ulpi_acquisition_status.dll_config = ulpi_dll_config;
#endif

#ifdef FX3_ULPI_SNIFFER
  /*
   * Arm the complete many-to-one DMA path before allowing the external
   * clocked GPIF state machine to push its first word. This matches the FX3
   * SDK gpiftousbmulti sequence and avoids a startup window where GPIF sees
   * an uninitialised producer/consumer path.
   */
  Fx3DmaStartProducer(FX3_PIB_DMA_SCK(1), dma_buffer_descriptor[1], 0, 0);
  Fx3DmaStartProducer(FX3_PIB_DMA_SCK(0), dma_buffer_descriptor[0], 0, 0);
  Fx3DmaStartConsumer(FX3_UIB_DMA_SCK(2),
		      dma_buffer_descriptor[0], 0, 0);
  ulpi_dma_armed = 1;
  Fx3GpifStart(ULPI_ACQ_START, 0);
  ulpi_acquisition_active = 1;
#else
  Fx3DmaStartProducer(FX3_PIB_DMA_SCK(1), dma_buffer_descriptor[1], 0, 0);
  Fx3DmaStartProducer(FX3_PIB_DMA_SCK(0), dma_buffer_descriptor[0], 0, 0);
  Fx3DmaStartConsumer(FX3_UIB_DMA_SCK(2), dma_buffer_descriptor[0], 0, 0);
  Fx3UsbSetInEndpointNak(2, 0);
  /* Arm the entire DMA path before GPIF can write its first sample. */
  Fx3GpifStart((delay < 2? delay : 2), 0);
#endif
}

void setup_acquisition(void)
{
  unsigned i;
  for(i=0; i<NUM_DMA_BUFFERS; i++)
    dma_buffer_descriptor[i] = Fx3DmaAllocateDescriptor();
}

void get_acquisition_status(volatile struct acquisition_status *status)
{
  uint8_t state;

  status->gpif_stat = Fx3GpifGetStat(&state);
  status->gpif_state = state;
  status->reserved = 0;
#ifdef FX3_ULPI_SNIFFER
  for (unsigned i = 0; i < NUM_DMA_BUFFERS; i++) {
    volatile struct Fx3DmaDescriptor *descriptor =
      FX3_DMA_DESCRIPTOR(dma_buffer_descriptor[i]);

    Fx3CacheInvalidateDCacheEntry(descriptor);
    if (descriptor->dscr_size & FX3_DSCR_SIZE_BUFFER_OCCUPIED)
      status->reserved |= (uint16_t)(1U << (i + 1));
    if (descriptor->dscr_size & FX3_DSCR_SIZE_BUFFER_ERROR)
      status->reserved |= 0x8000U;
  }
#endif
  status->gpif_status = Fx3ReadReg32(FX3_GPIF_STATUS);
  status->gpif_intr = Fx3ReadReg32(FX3_GPIF_INTR);
  status->pib_intr = Fx3ReadReg32(FX3_PIB_INTR);
  status->pib_error = Fx3ReadReg32(FX3_PIB_ERROR);
  status->pib_sck0_status = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_STATUS);
  status->pib_sck0_intr = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_INTR);
  status->pib_sck0_dscr = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_DSCR);
  status->pib_sck0_count = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_COUNT);
  status->pib_sck1_status = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_STATUS);
  status->pib_sck1_intr = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_INTR);
  status->pib_sck1_dscr = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_DSCR);
  status->pib_sck1_count = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_COUNT);
  status->uib_sck2_status = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_STATUS);
  status->uib_sck2_intr = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_INTR);
  status->uib_sck2_dscr = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_DSCR);
  status->uib_sck2_count = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_COUNT);
  status->eepm_cs = Fx3ReadReg32(FX3_EEPM_CS);
  status->eepm_endpoint2 = Fx3ReadReg32(FX3_EEPM_ENDPOINT + (2 << 2));
  status->prot_epi_cs1 = Fx3ReadReg32(FX3_PROT_EPI_CS1 + (2 << 2));
  status->pause_count = pause_count;
  status->pause_gpif_stat = pause_gpif_stat;
  status->pause_gpif_state = pause_gpif_state;
  status->pause_reserved = pause_pib_error;
  status->pause_gpif_status = pause_gpif_status;
  status->pause_pib_sck0_status = pause_pib_sck0_status;
  status->pause_pib_sck0_dscr = pause_pib_sck0_dscr;
  status->pause_pib_sck0_count = pause_pib_sck0_count;
  status->pause_pib_sck1_status = pause_pib_sck1_status;
  status->pause_pib_sck1_dscr = pause_pib_sck1_dscr;
  status->pause_pib_sck1_count = pause_pib_sck1_count;
  status->pause_uib_sck2_status = pause_uib_sck2_status;
  status->pause_uib_sck2_dscr = pause_uib_sck2_dscr;
  status->pause_uib_sck2_count = pause_uib_sck2_count;
}

#ifdef FX3_ULPI_SNIFFER
void get_ulpi_acquisition_status(
  volatile struct ulpi_acquisition_status *status)
{
  *status = ulpi_acquisition_status;
}

void set_ulpi_acquisition_timing(uint16_t value, uint8_t config)
{
  ulpi_dll_setting = value;
  ulpi_dll_config = config;
}
#endif

void poll_acquisition(void)
{
  uint8_t state;
  uint8_t gpif_stat = Fx3GpifGetStat(&state);
#ifdef FX3_ULPI_SNIFFER
  if (ulpi_acquisition_active) {
    uint32_t alpha = Fx3ReadReg32(FX3_GPIF_ALPHA_STAT);
    uint32_t gpio = Fx3ReadReg32(FX3_GPIO_INVALUE0);
    uint32_t ingress = Fx3ReadReg32(FX3_GPIF_INGRESS_DATA) & 0xff;
    uint32_t waveform = Fx3ReadReg32(FX3_GPIF_WAVEFORM_CTRL_STAT);
    uint8_t dir = !!(gpio & ULPI_DIR_GPIO_MASK);
    uint8_t sampling =
      state == ULPI_ACQ_SAMPLE_0 || state == ULPI_ACQ_SAMPLE_1;
    uint8_t waiting =
      state == ULPI_ACQ_WAIT_0 || state == ULPI_ACQ_WAIT_1;

    ulpi_acquisition_status.poll_count++;
    if (dir)
      ulpi_acquisition_status.dir_high_count++;
    if (sampling)
      ulpi_acquisition_status.sampling_state_count++;
    if (waiting)
      ulpi_acquisition_status.wait_state_count++;
    if (dir && (gpio & 0xff))
      ulpi_acquisition_status.dir_data_nonzero_count++;
    if (dir && ingress)
      ulpi_acquisition_status.dir_ingress_nonzero_count++;
    if (dir && (alpha & FX3_GPIF_ALPHA_DQ_OEN))
      ulpi_acquisition_status.alpha_dq_oen_while_dir_count++;
    if (ingress)
      ulpi_acquisition_status.ingress_nonzero_count++;
    ulpi_acquisition_status.last_gpio_invalue0 = gpio;
    ulpi_acquisition_status.last_alpha_stat = alpha;
    ulpi_acquisition_status.last_waveform_ctrl_stat = waveform;
    ulpi_acquisition_status.last_ingress_data = ingress;
  }
#endif
  if (gpif_stat == FX3_GPIF_PAUSED) {
#ifdef FX3_ULPI_SNIFFER
    /*
     * Error ISR already paused GPIF. Do NOT call stop_acquisition() here:
     * AbortSocket + UsbFlushInEndpoint while the host still has bulk IN
     * URBs outstanding wedges macOS SuperSpeed (strings/control time out
     * until a bus reset). Just mark inactive and clear the sticky error;
     * CMD_STOP from the host does the full teardown later.
     */
    if (ulpi_acquisition_active && state != 22) {
      pause_count++;
      pause_gpif_stat = gpif_stat;
      pause_gpif_state = state;
      pause_pib_error = Fx3ReadReg32(FX3_PIB_ERROR);
      pause_gpif_status = Fx3ReadReg32(FX3_GPIF_STATUS);
      pause_pib_sck0_status = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_STATUS);
      pause_pib_sck0_dscr = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_DSCR);
      pause_pib_sck0_count = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_COUNT);
      pause_pib_sck1_status = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_STATUS);
      pause_pib_sck1_dscr = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_DSCR);
      pause_pib_sck1_count = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_COUNT);
      pause_uib_sck2_status = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_STATUS);
      pause_uib_sck2_dscr = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_DSCR);
      pause_uib_sck2_count = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_COUNT);
      ulpi_acquisition_active = 0;
      Fx3WriteReg32(FX3_PIB_ERROR, 0);
      return;
    }
#endif
    if (state != 22)
      return;

    pause_count++;
    pause_gpif_stat = gpif_stat;
    pause_gpif_state = state;
    pause_pib_error = Fx3ReadReg32(FX3_PIB_ERROR);
    pause_gpif_status = Fx3ReadReg32(FX3_GPIF_STATUS);
    pause_pib_sck0_status = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_STATUS);
    pause_pib_sck0_dscr = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_DSCR);
    pause_pib_sck0_count = Fx3ReadReg32(FX3_PIB_DMA_SCK(0) + FX3_SCK_COUNT);
    pause_pib_sck1_status = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_STATUS);
    pause_pib_sck1_dscr = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_DSCR);
    pause_pib_sck1_count = Fx3ReadReg32(FX3_PIB_DMA_SCK(1) + FX3_SCK_COUNT);
    pause_uib_sck2_status = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_STATUS);
    pause_uib_sck2_dscr = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_DSCR);
    pause_uib_sck2_count = Fx3ReadReg32(FX3_UIB_DMA_SCK(2) + FX3_SCK_COUNT);
    Fx3UartTxString("GPIF paused state=");
    uart_tx_u8_dec(state);
    Fx3UartTxString(", stopping DMA\n");
    stop_acquisition();
  }
}
