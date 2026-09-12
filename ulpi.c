#include <bsp/gpio.h>
#include <bsp/gpif.h>
#include <bsp/regaccess.h>
#include <bsp/uart.h>
#include <bsp/util.h>
#include <rdb/gctl.h>
#include <rdb/gpio.h>
#include <rdb/gpif.h>
#include <rdb/pib.h>

#include <stdio.h>
#include <string.h>

#include "command.h"
#include "ulpi.h"

#define ULPI_REG_VENDOR_ID_LOW 0x00
#define ULPI_REG_VENDOR_ID_HIGH 0x01
#define ULPI_REG_PRODUCT_ID_LOW 0x02
#define ULPI_REG_PRODUCT_ID_HIGH 0x03
#define ULPI_REG_FUNCTION_CONTROL 0x04
#define ULPI_REG_INTERFACE_CONTROL 0x07
#define ULPI_REG_OTG_CONTROL 0x0a
#define ULPI_REG_DEBUG 0x15
#define ULPI_REG_SCRATCH 0x16

#define ULPI_FUNCTION_OPMODE_NON_DRIVING (1U << 3)
#define ULPI_FUNCTION_SUSPEND_M (1U << 6)

#define ULPI_WRITE_COMMAND(address) (0x80U | (address))
#define ULPI_READ_COMMAND(address) (0xc0U | (address))
#define ULPI_GPIF_CLOCK_DIVISOR_X2 13
#define ULPI_TRANSACTION_TIMEOUT_US 1000
#define ULPI_TRANSACTION_ATTEMPTS 8
#define ULPI_RETRY_DELAY_US 25
#define ULPI_DIR_LAMBDA FX3_GPIF_LAMBDA_INDEX_CTL2
#define ULPI_NXT_LAMBDA FX3_GPIF_LAMBDA_INDEX_CTL0
#define ULPI_READ_THREAD 2
#define ULPI_NXT_ACK_FUNCTION 5
#define ULPI_PIN_DIAGNOSTIC_SAMPLES 131072UL
#define ULPI_DIRECTION_MIRROR_BIT 0x80
#define ULPI_WAVEFORM_ALPHA_DQ_OEN \
  ((FX3_GPIF_ALPHA_DQ_OEN << 6) | (FX3_GPIF_ALPHA_DQ_OEN << 14))
#define ULPI_SAMPLE_DQ_FIRST 8
#define ULPI_SAMPLE_DQ_LAST 15

#define ULPI_PIN_DQ8_MASK (1UL << 8)
#define ULPI_PIN_DQ9_MASK (1UL << 9)
#define ULPI_PIN_DQ10_MASK (1UL << 10)
#define ULPI_PIN_CTL0_MASK (1UL << 17)
#define ULPI_PIN_CTL2_MASK (1UL << 19)
#define ULPI_PIN_CTL3_MASK (1UL << 20)

#define ULPI_STP_ALPHA (1UL << 4)
#define ULPI_DRIVE_HOLD_ALPHA FX3_GPIF_ALPHA_DQ_OEN
#define ULPI_DRIVE_DATA_ALPHA \
  (FX3_GPIF_ALPHA_DQ_OEN | FX3_GPIF_ALPHA_UPDATE_DOUT)

enum ulpi_write_state {
  ULPI_WRITE_START = 0,
  ULPI_WRITE_WAIT_DIR,
  ULPI_WRITE_TURNAROUND,
  ULPI_WRITE_COMMAND,
  ULPI_WRITE_DATA,
  ULPI_WRITE_DATA_HOLD,
  ULPI_WRITE_DATA_OUTPUT,
  ULPI_WRITE_IDLE_SELECT,
  ULPI_WRITE_IDLE_SELECT_HOLD,
  ULPI_WRITE_IDLE_STP,
  ULPI_WRITE_STP_HOLD,
  ULPI_WRITE_IDLE,
  ULPI_WRITE_DONE,
};

enum ulpi_read_state {
  ULPI_READ_START = 0,
  ULPI_READ_WAIT_DIR,
  ULPI_READ_LINK_TURNAROUND,
  ULPI_READ_COMMAND,
  ULPI_READ_WAIT_PHY,
  ULPI_READ_PHY_TURNAROUND,
  ULPI_READ_SAMPLE,
  ULPI_READ_PUSH,
  ULPI_READ_WAIT_RELEASE,
  ULPI_READ_LINK_IDLE_TURNAROUND,
  ULPI_READ_LINK_IDLE,
  ULPI_READ_RELEASE,
  ULPI_READ_DONE,
};

enum ulpi_pin_state {
  ULPI_PIN_START = 0,
  ULPI_PIN_DRIVE,
  ULPI_PIN_RECEIVE,
};

static const uint16_t functions[] = {
  [0] = 0U,
  [1] = (uint16_t)~0U,
  /*
   * CTL2 is active-low for the dedicated OE function, so its lambda value is
   * the inverse of the physical DIR level used by the ULPI state machines.
   */
  [2] = (uint16_t)~FX3_GPIF_FUNCTION_Fa,
  [3] = FX3_GPIF_FUNCTION_Fa,
  [4] = FX3_GPIF_FUNCTION_Fb,
  [ULPI_NXT_ACK_FUNCTION] =
    (uint16_t)(FX3_GPIF_FUNCTION_Fb & FX3_GPIF_FUNCTION_Fa),
};

static const Fx3GpifWaveform_t waveforms[] = {
  [ULPI_WRITE_START] = {
    GPIF_START_STATE(ULPI_WRITE_START),
    .left = ULPI_WRITE_WAIT_DIR,
  },
  [ULPI_WRITE_WAIT_DIR] = {
    GPIF_STATE(ULPI_WRITE_WAIT_DIR,
               ULPI_DIR_LAMBDA, 0, 0, 0, 3, 0,
               0, 0, 0, 0, 0),
    .left = ULPI_WRITE_TURNAROUND,
    .right = ULPI_WRITE_WAIT_DIR,
  },
  [ULPI_WRITE_TURNAROUND] = {
    GPIF_STATE(ULPI_WRITE_TURNAROUND,
               ULPI_DIR_LAMBDA, 0, 0, 0, 2, 1,
               0, ULPI_DRIVE_DATA_ALPHA, 0, 0, 0),
    .left = ULPI_WRITE_WAIT_DIR,
    .right = ULPI_WRITE_COMMAND,
  },
  [ULPI_WRITE_COMMAND] = {
    GPIF_STATE(ULPI_WRITE_COMMAND,
               ULPI_DIR_LAMBDA, ULPI_NXT_LAMBDA, 0, 0,
               2, ULPI_NXT_ACK_FUNCTION,
               0, ULPI_DRIVE_DATA_ALPHA,
               FX3_GPIF_BETA_THREAD_0 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_WRITE_WAIT_DIR,
    .right = ULPI_WRITE_DATA,
  },
  [ULPI_WRITE_DATA] = {
    GPIF_STATE(ULPI_WRITE_DATA, 0, 0, 0, 0, 1, 0,
               ULPI_DRIVE_HOLD_ALPHA, 0,
               FX3_GPIF_BETA_THREAD_1 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_WRITE_DATA_HOLD,
  },
  [ULPI_WRITE_DATA_HOLD] = {
    GPIF_STATE(ULPI_WRITE_DATA_HOLD, 0, 0, 0, 0, 1, 0,
               ULPI_DRIVE_HOLD_ALPHA, 0,
               FX3_GPIF_BETA_THREAD_1 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_WRITE_DATA_OUTPUT,
  },
  [ULPI_WRITE_DATA_OUTPUT] = {
    GPIF_STATE(ULPI_WRITE_DATA_OUTPUT, 0, 0, 0, 0, 1, 0,
               ULPI_DRIVE_DATA_ALPHA, 0,
               FX3_GPIF_BETA_THREAD_1 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_WRITE_IDLE_SELECT,
  },
  [ULPI_WRITE_IDLE_SELECT] = {
    GPIF_STATE(ULPI_WRITE_IDLE_SELECT, 0, 0, 0, 0, 1, 0,
               ULPI_DRIVE_HOLD_ALPHA, 0,
               FX3_GPIF_BETA_THREAD_2 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_WRITE_IDLE_SELECT_HOLD,
  },
  [ULPI_WRITE_IDLE_SELECT_HOLD] = {
    GPIF_STATE(ULPI_WRITE_IDLE_SELECT_HOLD, 0, 0, 0, 0, 1, 0,
               ULPI_DRIVE_HOLD_ALPHA, 0,
               FX3_GPIF_BETA_THREAD_2 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_WRITE_IDLE_STP,
  },
  [ULPI_WRITE_IDLE_STP] = {
    GPIF_STATE(ULPI_WRITE_IDLE_STP, 0, 0, 0, 0, 1, 0,
               ULPI_DRIVE_DATA_ALPHA | ULPI_STP_ALPHA, 0,
               FX3_GPIF_BETA_THREAD_2 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_WRITE_STP_HOLD,
  },
  [ULPI_WRITE_STP_HOLD] = {
    GPIF_STATE(ULPI_WRITE_STP_HOLD,
               0, ULPI_NXT_LAMBDA, 0, 0, 4, 1,
               ULPI_DRIVE_HOLD_ALPHA | ULPI_STP_ALPHA, 0,
               FX3_GPIF_BETA_THREAD_2 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_WRITE_STP_HOLD,
    .right = ULPI_WRITE_IDLE,
  },
  [ULPI_WRITE_IDLE] = {
    GPIF_STATE(ULPI_WRITE_IDLE, 0, 0, 0, 0, 1, 0,
               ULPI_DRIVE_HOLD_ALPHA, 0,
               FX3_GPIF_BETA_THREAD_2 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_WRITE_DONE,
  },
  [ULPI_WRITE_DONE] = {
    GPIF_STATE(ULPI_WRITE_DONE, 0, 0, 0, 0, 0, 0,
               ULPI_DRIVE_HOLD_ALPHA, 0,
               FX3_GPIF_BETA_THREAD_2 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
  },
};

static const Fx3GpifWaveform_t read_waveforms[] = {
  [ULPI_READ_START] = {
    GPIF_START_STATE(ULPI_READ_START),
    .left = ULPI_READ_WAIT_DIR,
  },
  [ULPI_READ_WAIT_DIR] = {
    GPIF_STATE(ULPI_READ_WAIT_DIR,
               ULPI_DIR_LAMBDA, 0, 0, 0, 3, 0,
               0, 0, 0, 0, 0),
    .left = ULPI_READ_LINK_TURNAROUND,
    .right = ULPI_READ_WAIT_DIR,
  },
  [ULPI_READ_LINK_TURNAROUND] = {
    GPIF_STATE(ULPI_READ_LINK_TURNAROUND,
               ULPI_DIR_LAMBDA, 0, 0, 0, 2, 1,
               0, ULPI_DRIVE_DATA_ALPHA, 0, 0, 0),
    .left = ULPI_READ_WAIT_DIR,
    .right = ULPI_READ_COMMAND,
  },
  [ULPI_READ_COMMAND] = {
    GPIF_STATE(ULPI_READ_COMMAND,
               ULPI_DIR_LAMBDA, ULPI_NXT_LAMBDA, 0, 0,
               2, ULPI_NXT_ACK_FUNCTION,
               0, 0,
               FX3_GPIF_BETA_THREAD_0 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_READ_WAIT_DIR,
    .right = ULPI_READ_WAIT_PHY,
  },
  [ULPI_READ_WAIT_PHY] = {
    GPIF_STATE(ULPI_READ_WAIT_PHY,
               ULPI_DIR_LAMBDA, 0, 0, 0, 2, 0,
               0, 0, 0, 0, 0),
    .left = ULPI_READ_PHY_TURNAROUND,
  },
  [ULPI_READ_PHY_TURNAROUND] = {
    GPIF_STATE(ULPI_READ_PHY_TURNAROUND, 0, 0, 0, 0, 1, 0,
               0, 0, 0, 0, 0),
    .left = ULPI_READ_SAMPLE,
  },
  [ULPI_READ_SAMPLE] = {
    GPIF_STATE(ULPI_READ_SAMPLE, 0, 0, 0, 0, 1, 0,
               FX3_GPIF_ALPHA_SAMPLE_DIN, 0,
               FX3_GPIF_BETA_THREAD_2, 0, 0),
    .left = ULPI_READ_PUSH,
  },
  [ULPI_READ_PUSH] = {
    GPIF_STATE(ULPI_READ_PUSH, 0, 0, 0, 0, 1, 0,
               0, 0,
               FX3_GPIF_BETA_THREAD_2 |
                 FX3_GPIF_BETA_WQ_PUSH |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_READ_WAIT_RELEASE,
  },
  [ULPI_READ_WAIT_RELEASE] = {
    GPIF_STATE(ULPI_READ_WAIT_RELEASE,
               ULPI_DIR_LAMBDA, 0, 0, 0, 3, 0,
               0, 0, 0, 0, 0),
    .left = ULPI_READ_LINK_IDLE_TURNAROUND,
    .right = ULPI_READ_WAIT_RELEASE,
  },
  [ULPI_READ_LINK_IDLE_TURNAROUND] = {
    GPIF_STATE(ULPI_READ_LINK_IDLE_TURNAROUND,
               ULPI_DIR_LAMBDA, 0, 0, 0, 2, 1,
               0, ULPI_DRIVE_DATA_ALPHA,
               FX3_GPIF_BETA_THREAD_1 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_READ_WAIT_RELEASE,
    .right = ULPI_READ_LINK_IDLE,
  },
  [ULPI_READ_LINK_IDLE] = {
    GPIF_STATE(ULPI_READ_LINK_IDLE, 0, 0, 0, 0, 1, 0,
               0, 0,
               FX3_GPIF_BETA_THREAD_1 |
                 FX3_GPIF_BETA_REGISTER_ACCESS, 0, 0),
    .left = ULPI_READ_RELEASE,
  },
  [ULPI_READ_RELEASE] = {
    GPIF_STATE(ULPI_READ_RELEASE, 0, 0, 0, 0, 1, 0,
               0, 0, 0, 0, 0),
    .left = ULPI_READ_DONE,
  },
  [ULPI_READ_DONE] = {
    GPIF_STATE(ULPI_READ_DONE, 0, 0, 0, 0, 0, 0,
               0, 0, 0, 0, 0),
  },
};

static const Fx3GpifWaveform_t pin_waveforms[] = {
  [ULPI_PIN_START] = {
    GPIF_START_STATE(ULPI_PIN_START),
    .left = ULPI_PIN_DRIVE,
  },
  [ULPI_PIN_DRIVE] = {
    GPIF_STATE(ULPI_PIN_DRIVE,
               ULPI_DIR_LAMBDA, 0, 0, 0, 3, 1,
               ULPI_DRIVE_HOLD_ALPHA, 0, 0, 0, 0),
    .left = ULPI_PIN_DRIVE,
    .right = ULPI_PIN_RECEIVE,
  },
  [ULPI_PIN_RECEIVE] = {
    GPIF_STATE(ULPI_PIN_RECEIVE,
               ULPI_DIR_LAMBDA, 0, 0, 0, 2, 1,
               FX3_GPIF_ALPHA_SAMPLE_DIN, ULPI_DRIVE_HOLD_ALPHA,
               0, 0, 0),
    .left = ULPI_PIN_RECEIVE,
    .right = ULPI_PIN_DRIVE,
  },
};

static const Fx3GpifWaveform_t mirror_pin_waveforms[] = {
  [0] = {
    GPIF_START_STATE(0),
    .left = 1,
  },
  [1] = {
    GPIF_STATE(1, 0, 0, 0, 0, 1, 0,
               FX3_GPIF_ALPHA_DQ_OEN, FX3_GPIF_ALPHA_DQ_OEN,
               0, 0, 0),
    .left = 1,
    .right = 1,
  },
};

static Fx3GpifRegisters_t registers = {
  .config = (FX3_GPIF_CONFIG_ENABLE |
             FX3_GPIF_CONFIG_THREAD_IN_STATE |
             FX3_GPIF_CONFIG_SYNC_SPEED |
             FX3_GPIF_CONFIG_SYNC |
             FX3_GPIF_CONFIG_DOUT_POP_EN),
  .bus_config = 0,
  .ad_config =
    (FX3_GPIF_OEN_CFG_INPUT << FX3_GPIF_AD_CONFIG_A_OEN_CFG_SHIFT) |
    (FX3_GPIF_OEN_CFG_DYNAMIC << FX3_GPIF_AD_CONFIG_DQ_OEN_CFG_SHIFT),
  .ctrl_bus_direction =
    (FX3_GPIF_CTRL_BUS_DIRECTION_OUTPUT << (3 * 2)),
  .ctrl_bus_polarity = 1UL << 2,
  .ctrl_bus_select[3] = FX3_GPIF_OMEGA_INDEX_ALPHA4,
  .data_ctrl = (7UL << FX3_GPIF_DATA_CTRL_EG_DATA_VALID_SHIFT),
  .thread_config[0] = (FX3_GPIF_THREAD_CONFIG_ENABLE |
                       (0UL << FX3_GPIF_THREAD_CONFIG_THREAD_SOCK_SHIFT)),
  .thread_config[1] = (FX3_GPIF_THREAD_CONFIG_ENABLE |
                       (1UL << FX3_GPIF_THREAD_CONFIG_THREAD_SOCK_SHIFT)),
  .thread_config[2] = (FX3_GPIF_THREAD_CONFIG_ENABLE |
                       (2UL << FX3_GPIF_THREAD_CONFIG_THREAD_SOCK_SHIFT)),
  .waveform_switch =
    ((uint32_t)ULPI_WRITE_DONE << FX3_GPIF_WAVEFORM_SWITCH_DONE_STATE_SHIFT) |
    FX3_GPIF_WAVEFORM_SWITCH_DONE_ENABLE,
  .beta_deassert = 0xffffffc1UL,
};

static Fx3GpifRegisters_t pin_registers = {
  .config = (FX3_GPIF_CONFIG_ENABLE |
             FX3_GPIF_CONFIG_THREAD_IN_STATE |
             FX3_GPIF_CONFIG_SYNC),
  .bus_config = 0,
  .ad_config =
    (FX3_GPIF_OEN_CFG_INPUT << FX3_GPIF_AD_CONFIG_A_OEN_CFG_SHIFT) |
    (FX3_GPIF_OEN_CFG_DYNAMIC << FX3_GPIF_AD_CONFIG_DQ_OEN_CFG_SHIFT),
  .ctrl_bus_direction =
    (FX3_GPIF_CTRL_BUS_DIRECTION_OUTPUT << (3 * 2)),
  .ctrl_bus_polarity = 1UL << 2,
  .ctrl_bus_select[3] = FX3_GPIF_OMEGA_INDEX_ALPHA4,
};

static struct ulpi_status status;
static bool passive_configured;
static enum ulpi_speed passive_speed;

static void ulpi_override_sample_dq(bool enable)
{
  uint8_t gpio;

  for (gpio = ULPI_SAMPLE_DQ_FIRST; gpio <= ULPI_SAMPLE_DQ_LAST; gpio++) {
    if (enable)
      Fx3GpioSetupSimple(gpio,
                         FX3_GPIO_SIMPLE_ENABLE |
                         FX3_GPIO_SIMPLE_INPUT_EN);
    else
      Fx3GpioReleaseSimple(gpio);
  }
}

void ulpi_init(void)
{
  passive_configured = false;
  ulpi_override_sample_dq(true);
  Fx3ClearReg32(FX3_GCTL_WPU_CFG, 0xff);
  Fx3SetReg32(FX3_GCTL_WPD_CFG, 0xff);
}

static void ulpi_record_gpif_status(uint8_t state, Fx3GpifStat_t stat)
{
  status.last_gpif_state = state;
  status.last_gpif_stat = (uint8_t)stat;
  status.last_gpif_status = Fx3ReadReg32(FX3_GPIF_STATUS);
  status.gpio_invalue0 = Fx3ReadReg32(FX3_GPIO_INVALUE0);
  status.alpha_stat = Fx3ReadReg32(FX3_GPIF_ALPHA_STAT);
  status.beta_stat = Fx3ReadReg32(FX3_GPIF_BETA_STAT);
  status.data_ctrl = Fx3ReadReg32(FX3_GPIF_DATA_CTRL);
  status.gpio_simple_override0 = Fx3ReadReg32(FX3_GCTL_GPIO_SIMPLE);
  status.gpio8_config = Fx3ReadReg32(FX3_GPIO_SIMPLE + (8 << 2));
  status.gpio9_config = Fx3ReadReg32(FX3_GPIO_SIMPLE + (9 << 2));
}

static void ulpi_record_write_status(void)
{
  status.write_gpif_state = status.last_gpif_state;
  status.write_gpif_stat = status.last_gpif_stat;
  status.write_gpio_invalue0 = status.gpio_invalue0;
  status.write_alpha_stat = status.alpha_stat;
  status.write_beta_stat = status.beta_stat;
  status.write_data_ctrl = status.data_ctrl;
}

static bool ulpi_write_register(uint8_t address, uint8_t value)
{
  char log[80];
  Fx3GpifStat_t last_stat = FX3_GPIF_INVALID;
  uint8_t last_state = 0xff;
  uint8_t state;
  unsigned transitions = 0;
  unsigned timeout;

  snprintf(log, sizeof(log), "ULPI write %02x=%02x\n",
           (unsigned)address, (unsigned)value);
  Fx3UartTxString(log);

  registers.data_ctrl =
    (7UL << FX3_GPIF_DATA_CTRL_EG_DATA_VALID_SHIFT);
  registers.egress_data[0] = ULPI_WRITE_COMMAND(address);
  registers.egress_data[1] = value;
  registers.egress_data[2] = 0;
  registers.waveform_switch =
    ((uint32_t)ULPI_WRITE_DONE <<
       FX3_GPIF_WAVEFORM_SWITCH_DONE_STATE_SHIFT) |
    FX3_GPIF_WAVEFORM_SWITCH_DONE_ENABLE;

  Fx3GpifStop();
  Fx3GpifConfigure(waveforms,
                   sizeof(waveforms) / sizeof(waveforms[0]),
                   functions, sizeof(functions) / sizeof(functions[0]),
                   &registers);
  Fx3GpifStart(ULPI_WRITE_START, 0);

  for (timeout = 0; timeout < ULPI_TRANSACTION_TIMEOUT_US; timeout++) {
    Fx3GpifStat_t stat = Fx3GpifGetStat(&state);
    ulpi_record_gpif_status(state, stat);
    if ((state != last_state || stat != last_stat) && transitions < 16) {
      snprintf(log, sizeof(log), "  state=%u stat=%u\n",
               (unsigned)state, (unsigned)stat);
      Fx3UartTxString(log);
      transitions++;
      last_state = state;
      last_stat = stat;
    }
    if (state == ULPI_WRITE_DONE &&
        (stat == FX3_GPIF_DONE || stat == FX3_GPIF_PAUSED)) {
      ulpi_record_write_status();
      Fx3UartTxString("  done\n");
      return true;
    }
    Fx3UtilDelayUs(1);
  }

  snprintf(log, sizeof(log), "  timeout gpif_intr=%08lx pib_intr=%08lx\n",
           (unsigned long)Fx3ReadReg32(FX3_GPIF_INTR),
           (unsigned long)Fx3ReadReg32(FX3_PIB_INTR));
  Fx3UartTxString(log);
  snprintf(log, sizeof(log), "  lambda=%08lx alpha=%08lx beta=%08lx\n",
           (unsigned long)Fx3ReadReg32(FX3_GPIF_LAMBDA_STAT),
           (unsigned long)Fx3ReadReg32(FX3_GPIF_ALPHA_STAT),
           (unsigned long)Fx3ReadReg32(FX3_GPIF_BETA_STAT));
  Fx3UartTxString(log);
  snprintf(log, sizeof(log), "  status=%08lx eg0=%08lx gpio=%08lx\n",
           (unsigned long)Fx3ReadReg32(FX3_GPIF_STATUS),
           (unsigned long)Fx3ReadReg32(FX3_GPIF_EGRESS_DATA),
           (unsigned long)Fx3ReadReg32(FX3_GPIO_INVALUE0));
  Fx3UartTxString(log);
  ulpi_record_write_status();
  return false;
}

static bool ulpi_read_register(uint8_t address, uint8_t *value)
{
  char log[80];
  Fx3GpifStat_t last_stat = FX3_GPIF_INVALID;
  uint32_t gpif_status;
  uint32_t ingress_data;
  uint8_t last_state = 0xff;
  uint8_t state = 0xff;
  unsigned transitions = 0;
  unsigned timeout;

  snprintf(log, sizeof(log), "ULPI read %02x\n", (unsigned)address);
  Fx3UartTxString(log);

  registers.data_ctrl =
    (3UL << FX3_GPIF_DATA_CTRL_EG_DATA_VALID_SHIFT) |
    (1UL << ULPI_READ_THREAD);
  registers.egress_data[0] = ULPI_READ_COMMAND(address);
  registers.egress_data[1] = 0;
  registers.egress_data[2] = 0;
  registers.waveform_switch =
    ((uint32_t)ULPI_READ_DONE <<
       FX3_GPIF_WAVEFORM_SWITCH_DONE_STATE_SHIFT) |
    FX3_GPIF_WAVEFORM_SWITCH_DONE_ENABLE;

  Fx3GpifStop();
  Fx3GpifConfigure(read_waveforms,
                   sizeof(read_waveforms) / sizeof(read_waveforms[0]),
                   functions, sizeof(functions) / sizeof(functions[0]),
                   &registers);
  Fx3GpifStart(ULPI_READ_START, 0);

  for (timeout = 0; timeout < ULPI_TRANSACTION_TIMEOUT_US; timeout++) {
    Fx3GpifStat_t stat = Fx3GpifGetStat(&state);
    ulpi_record_gpif_status(state, stat);
    if ((state != last_state || stat != last_stat) && transitions < 32) {
      snprintf(log, sizeof(log),
               "  state=%u stat=%u gpio=%08lx alpha=%08lx\n",
               (unsigned)state, (unsigned)stat,
               (unsigned long)status.gpio_invalue0,
               (unsigned long)status.alpha_stat);
      Fx3UartTxString(log);
      transitions++;
      last_state = state;
      last_stat = stat;
    }
    if (state == ULPI_READ_DONE &&
        (stat == FX3_GPIF_DONE || stat == FX3_GPIF_PAUSED))
      break;
    Fx3UtilDelayUs(1);
  }

  if (timeout == ULPI_TRANSACTION_TIMEOUT_US) {
    snprintf(log, sizeof(log), "  read timeout state=%u stat=%u\n",
             (unsigned)status.last_gpif_state,
             (unsigned)status.last_gpif_stat);
    Fx3UartTxString(log);
    return false;
  }

  gpif_status = Fx3ReadReg32(FX3_GPIF_STATUS);
  ulpi_record_gpif_status(state, Fx3GpifGetStat(0));
  if (!(gpif_status &
        (1UL << (FX3_GPIF_STATUS_IN_DATA_VALID_SHIFT +
                 ULPI_READ_THREAD)))) {
    Fx3UartTxString("  read completed without ingress data\n");
    return false;
  }

  ingress_data = Fx3ReadReg32(FX3_GPIF_INGRESS_DATA +
                              ULPI_READ_THREAD * 4);
  if (ingress_data & 0xff)
    status.read_ingress_data = ingress_data;
  *value = (uint8_t)ingress_data;
  Fx3WriteReg32(FX3_GPIF_DATA_CTRL, 1UL << ULPI_READ_THREAD);
  snprintf(log, sizeof(log), "  value=%02x\n", (unsigned)*value);
  Fx3UartTxString(log);
  return true;
}

static bool ulpi_write_register_retry(uint8_t address, uint8_t value)
{
  unsigned attempt;

  for (attempt = 0; attempt < ULPI_TRANSACTION_ATTEMPTS; attempt++) {
    if (ulpi_write_register(address, value))
      return true;

    Fx3GpifStop();
    Fx3UtilDelayUs(ULPI_RETRY_DELAY_US);
  }

  return false;
}

static bool ulpi_read_register_retry(uint8_t address, uint8_t *value)
{
  unsigned attempt;

  for (attempt = 0; attempt < ULPI_TRANSACTION_ATTEMPTS; attempt++) {
    if (ulpi_read_register(address, value))
      return true;

    Fx3GpifStop();
    Fx3WriteReg32(FX3_GPIF_DATA_CTRL, 1UL << ULPI_READ_THREAD);
    Fx3UtilDelayUs(ULPI_RETRY_DELAY_US);
  }

  return false;
}

static void ulpi_read_status_register(uint8_t address, uint8_t *value,
                                      uint16_t mask)
{
  if (ulpi_read_register_retry(address, value))
    status.read_ok_mask |= mask;
}

static bool ulpi_configure_passive_raw(enum ulpi_speed speed)
{
  bool scratch_ok = false;
  bool success;
  uint8_t function_control;

  if (speed > ULPI_SPEED_LOW)
    return false;

  memset(&status, 0, sizeof(status));
  function_control = ULPI_FUNCTION_SUSPEND_M |
                     ULPI_FUNCTION_OPMODE_NON_DRIVING |
                     (uint8_t)speed;
  status.requested_speed = (uint8_t)speed;
  status.expected_function_control = function_control;
  status.vendor_id_low = 0xff;
  status.vendor_id_high = 0xff;
  status.product_id_low = 0xff;
  status.product_id_high = 0xff;
  status.function_control = 0xff;
  status.interface_control = 0xff;
  status.otg_control = 0xff;
  status.debug = 0xff;
  status.scratch_before = 0xff;
  status.scratch_expected = 0xff;
  status.scratch_after = 0xff;
  status.scratch_restored = 0xff;

  /*
   * SRAM-loaded firmware can inherit an active PIB block from the previous
   * image. Reset GPIF/PIB before the first register transaction so the ULPI
   * command waveform owns the interface from a known turnaround state.
   */
  Fx3GpifStop();
  Fx3GpifPibStop();
  ulpi_override_sample_dq(false);
  Fx3GpifPibStart(ULPI_GPIF_CLOCK_DIVISOR_X2);
  Fx3ClearReg32(FX3_PIB_DLL_CTRL, FX3_PIB_DLL_CTRL_ENABLE);
  ulpi_read_status_register(ULPI_REG_VENDOR_ID_LOW,
                            &status.vendor_id_low,
                            ULPI_STATUS_READ_VENDOR_ID_LOW);
  ulpi_read_status_register(ULPI_REG_VENDOR_ID_HIGH,
                            &status.vendor_id_high,
                            ULPI_STATUS_READ_VENDOR_ID_HIGH);
  ulpi_read_status_register(ULPI_REG_PRODUCT_ID_LOW,
                            &status.product_id_low,
                            ULPI_STATUS_READ_PRODUCT_ID_LOW);
  ulpi_read_status_register(ULPI_REG_PRODUCT_ID_HIGH,
                            &status.product_id_high,
                            ULPI_STATUS_READ_PRODUCT_ID_HIGH);
  ulpi_read_status_register(ULPI_REG_INTERFACE_CONTROL,
                            &status.interface_control,
                            ULPI_STATUS_READ_INTERFACE_CONTROL);
  ulpi_read_status_register(ULPI_REG_OTG_CONTROL,
                            &status.otg_control,
                            ULPI_STATUS_READ_OTG_CONTROL);
  ulpi_read_status_register(ULPI_REG_DEBUG,
                            &status.debug,
                            ULPI_STATUS_READ_DEBUG);

  ulpi_read_status_register(ULPI_REG_SCRATCH,
                            &status.scratch_before,
                            ULPI_STATUS_READ_SCRATCH_BEFORE);
  if (status.read_ok_mask & ULPI_STATUS_READ_SCRATCH_BEFORE) {
    status.scratch_expected = status.scratch_before ^ 0x5a;
    if (!ulpi_write_register_retry(ULPI_REG_SCRATCH,
                                   status.scratch_expected))
      goto stop;
    status.write_ok_mask |= ULPI_STATUS_WRITE_SCRATCH_TEST;
    ulpi_read_status_register(ULPI_REG_SCRATCH,
                              &status.scratch_after,
                              ULPI_STATUS_READ_SCRATCH_AFTER);
    if (!ulpi_write_register_retry(ULPI_REG_SCRATCH,
                                   status.scratch_before))
      goto stop;
    status.write_ok_mask |= ULPI_STATUS_WRITE_SCRATCH_RESTORE;
    ulpi_read_status_register(ULPI_REG_SCRATCH,
                              &status.scratch_restored,
                              ULPI_STATUS_READ_SCRATCH_RESTORED);

    scratch_ok =
      (status.write_ok_mask &
       (ULPI_STATUS_WRITE_SCRATCH_TEST |
        ULPI_STATUS_WRITE_SCRATCH_RESTORE)) ==
      (ULPI_STATUS_WRITE_SCRATCH_TEST |
       ULPI_STATUS_WRITE_SCRATCH_RESTORE) &&
      status.scratch_after == status.scratch_expected &&
      status.scratch_restored == status.scratch_before;
  }

  if (!scratch_ok)
    goto stop;

  if (ulpi_write_register_retry(ULPI_REG_OTG_CONTROL, 0)) {
    status.write_ok_mask |= ULPI_STATUS_WRITE_OTG_CONTROL;
  }
  if (ulpi_write_register_retry(ULPI_REG_FUNCTION_CONTROL,
                                function_control)) {
    status.write_ok_mask |= ULPI_STATUS_WRITE_FUNCTION_CONTROL;
  }

  /* A completed write waveform is not proof that the PHY applied the mode. */
  status.read_ok_mask &= ~(ULPI_STATUS_READ_FUNCTION_CONTROL |
                          ULPI_STATUS_READ_OTG_CONTROL |
                          ULPI_STATUS_READ_DEBUG);
  status.function_control = status.otg_control = status.debug = 0xff;
  ulpi_read_status_register(ULPI_REG_FUNCTION_CONTROL,
                            &status.function_control,
                            ULPI_STATUS_READ_FUNCTION_CONTROL);
  ulpi_read_status_register(ULPI_REG_OTG_CONTROL,
                            &status.otg_control,
                            ULPI_STATUS_READ_OTG_CONTROL);
  ulpi_read_status_register(ULPI_REG_DEBUG, &status.debug,
                            ULPI_STATUS_READ_DEBUG);

 stop:
  Fx3GpifStop();
  Fx3GpifPibStop();
  ulpi_override_sample_dq(true);

  if (status.vendor_id_low == 0x24)
    status.match_mask |= ULPI_STATUS_MATCH_VENDOR_ID_LOW;
  if (status.vendor_id_high == 0x04)
    status.match_mask |= ULPI_STATUS_MATCH_VENDOR_ID_HIGH;
  if (status.product_id_low == 0x04)
    status.match_mask |= ULPI_STATUS_MATCH_PRODUCT_ID_LOW;
  if (status.product_id_high == 0x00)
    status.match_mask |= ULPI_STATUS_MATCH_PRODUCT_ID_HIGH;
  if (status.function_control == function_control)
    status.match_mask |= ULPI_STATUS_MATCH_FUNCTION_CONTROL;
  if (status.interface_control == 0)
    status.match_mask |= ULPI_STATUS_MATCH_INTERFACE_CONTROL;
  if (status.otg_control == 0)
    status.match_mask |= ULPI_STATUS_MATCH_OTG_CONTROL;
  if ((status.read_ok_mask &
       (ULPI_STATUS_READ_SCRATCH_BEFORE |
        ULPI_STATUS_READ_SCRATCH_AFTER |
        ULPI_STATUS_READ_SCRATCH_RESTORED)) ==
      (ULPI_STATUS_READ_SCRATCH_BEFORE |
       ULPI_STATUS_READ_SCRATCH_AFTER |
       ULPI_STATUS_READ_SCRATCH_RESTORED) &&
      (status.write_ok_mask &
       (ULPI_STATUS_WRITE_SCRATCH_TEST |
        ULPI_STATUS_WRITE_SCRATCH_RESTORE)) ==
      (ULPI_STATUS_WRITE_SCRATCH_TEST |
       ULPI_STATUS_WRITE_SCRATCH_RESTORE) &&
      status.scratch_after == status.scratch_expected &&
      status.scratch_restored == status.scratch_before)
    status.match_mask |= ULPI_STATUS_MATCH_SCRATCH;

  success = status.read_ok_mask == ULPI_STATUS_READ_ALL &&
            status.write_ok_mask == ULPI_STATUS_WRITE_ALL &&
            status.match_mask == ULPI_STATUS_MATCH_ALL;
  return success;
}

bool ulpi_force_configure_passive(enum ulpi_speed speed)
{
  bool success;

  passive_configured = false;
  success = ulpi_configure_passive_raw(speed);
  if (success) {
    passive_speed = speed;
    passive_configured = true;
    ulpi_start_idle();
  }
  return success;
}

bool ulpi_configure_passive(enum ulpi_speed speed)
{
  if (passive_configured && passive_speed == speed) {
    ulpi_start_idle();
    return true;
  }
  return ulpi_force_configure_passive(speed);
}

void ulpi_start_idle(void)
{
  if (!passive_configured)
    return;

  ulpi_override_sample_dq(false);
  Fx3GpifStop();
  Fx3GpifPibStop();
  Fx3GpifPibStart(ULPI_GPIF_CLOCK_DIVISOR_X2);
  Fx3ClearReg32(FX3_PIB_DLL_CTRL, FX3_PIB_DLL_CTRL_ENABLE);
  Fx3GpifConfigure(pin_waveforms,
                   sizeof(pin_waveforms) / sizeof(pin_waveforms[0]),
                   functions, sizeof(functions) / sizeof(functions[0]),
                   &pin_registers);
  Fx3GpifStart(ULPI_PIN_START, 0);
}

void ulpi_get_status(volatile struct ulpi_status *out)
{
  *out = status;
}

void ulpi_get_pin_status(volatile struct ulpi_pin_status *out,
			 bool mirror_states)
{
  static const uint32_t masks[ULPI_PIN_SIGNAL_COUNT] = {
    [ULPI_PIN_CTL2_DIR] = ULPI_PIN_CTL2_MASK,
    [ULPI_PIN_DQ10_DIR] = ULPI_PIN_DQ10_MASK,
    [ULPI_PIN_CTL0_NXT] = ULPI_PIN_CTL0_MASK,
    [ULPI_PIN_DQ9_NXT] = ULPI_PIN_DQ9_MASK,
    [ULPI_PIN_CTL3_STP] = ULPI_PIN_CTL3_MASK,
    [ULPI_PIN_DQ8_STP] = ULPI_PIN_DQ8_MASK,
  };
  struct ulpi_pin_status result;
  uint32_t ingress;
  uint32_t gpif;
  uint32_t previous_ingress;
  uint32_t previous;
  uint32_t sample;
  uint8_t drive_state = ULPI_PIN_DRIVE;
  uint8_t receive_state = ULPI_PIN_RECEIVE;
  unsigned i;
  unsigned signal;

  memset(&result, 0, sizeof(result));
  Fx3GpifStop();
  Fx3GpifPibStop();
  Fx3GpifPibStart(ULPI_GPIF_CLOCK_DIVISOR_X2);
  Fx3ClearReg32(FX3_PIB_DLL_CTRL, FX3_PIB_DLL_CTRL_ENABLE);
  if (mirror_states) {
    Fx3GpifRegisters_t mirror_registers = pin_registers;
    const uint32_t edge_bases[] = {
      FX3_GPIF_LEFT_WAVEFORM,
      FX3_GPIF_RIGHT_WAVEFORM,
    };
    unsigned edge;
    unsigned state;
    unsigned word;

    mirror_registers.bus_config2 =
      (ULPI_DIR_LAMBDA << FX3_GPIF_BUS_CONFIG2_STATE7_SHIFT) | 1;
    mirror_registers.ad_config &=
      ~FX3_GPIF_AD_CONFIG_DQ_OEN_CFG_MASK;
    Fx3GpifConfigure(mirror_pin_waveforms,
                     sizeof(mirror_pin_waveforms) /
                       sizeof(mirror_pin_waveforms[0]),
                     functions, sizeof(functions) / sizeof(functions[0]),
                     &mirror_registers);
    for (edge = 0;
         edge < sizeof(edge_bases) / sizeof(edge_bases[0]); edge++) {
      for (state = 0; state < 2; state++) {
        uint32_t words[3];
        uint32_t base = edge_bases[edge] + state * 16;

        for (word = 0; word < 3; word++)
          words[word] = Fx3ReadReg32(base + word * 4);
        base = edge_bases[edge] +
               (state | ULPI_DIRECTION_MIRROR_BIT) * 16;
        Fx3WriteReg32(base, words[0]);
        Fx3WriteReg32(base + 4,
                      words[1] & ~ULPI_WAVEFORM_ALPHA_DQ_OEN);
        Fx3WriteReg32(base + 8, words[2]);
      }
    }
    receive_state = ULPI_PIN_DRIVE | ULPI_DIRECTION_MIRROR_BIT;
  } else {
    Fx3GpifConfigure(pin_waveforms,
                     sizeof(pin_waveforms) / sizeof(pin_waveforms[0]),
                     functions, sizeof(functions) / sizeof(functions[0]),
                     &pin_registers);
  }
  Fx3GpifStart(ULPI_PIN_START, 0);
  Fx3UtilDelayUs(10);
  previous = Fx3ReadReg32(FX3_GPIO_INVALUE0);
  previous_ingress = Fx3ReadReg32(FX3_GPIF_INGRESS_DATA) & 0xff;

  for (i = 0; i < ULPI_PIN_DIAGNOSTIC_SAMPLES; i++) {
    sample = Fx3ReadReg32(FX3_GPIO_INVALUE0);
    ingress = Fx3ReadReg32(FX3_GPIF_INGRESS_DATA) & 0xff;
    gpif = Fx3ReadReg32(FX3_GPIF_WAVEFORM_CTRL_STAT);
    for (signal = 0; signal < ULPI_PIN_SIGNAL_COUNT; signal++) {
      if (sample & masks[signal])
        result.high[signal]++;
      if ((sample ^ previous) & masks[signal])
        result.transitions[signal]++;
    }
    if (!!(sample & ULPI_PIN_CTL2_MASK) !=
        !!(sample & ULPI_PIN_DQ10_MASK))
      result.mismatches[ULPI_PIN_PAIR_DIR]++;
    if (!!(sample & ULPI_PIN_CTL0_MASK) !=
        !!(sample & ULPI_PIN_DQ9_MASK))
      result.mismatches[ULPI_PIN_PAIR_NXT]++;
    if (!!(sample & ULPI_PIN_CTL3_MASK) !=
        !!(sample & ULPI_PIN_DQ8_MASK))
      result.mismatches[ULPI_PIN_PAIR_STP]++;
    if (!!(sample & ULPI_PIN_CTL2_MASK) !=
        !!(sample & ULPI_PIN_DQ8_MASK))
      result.dir_to_dq8_mismatches++;
    if (!!(sample & ULPI_PIN_CTL0_MASK) !=
        !!(sample & ULPI_PIN_DQ10_MASK))
      result.nxt_to_dq10_mismatches++;
    if (sample & 0xff)
      result.data_nonzero_samples++;
    if ((sample ^ previous) & 0xff)
      result.data_transitions++;
    if (sample & ULPI_PIN_CTL2_MASK) {
      result.rx_samples++;
      if (sample & 0xff)
        result.rx_nonzero_samples++;
      if (sample & ULPI_PIN_CTL0_MASK)
        result.rx_nxt_samples++;
      result.rx_data_or |= sample & 0xff;
    }
    if (ingress)
      result.ingress_nonzero_samples++;
    if (ingress != previous_ingress)
      result.ingress_transitions++;
    if (sample & ULPI_PIN_CTL2_MASK) {
      if (ingress)
        result.rx_ingress_nonzero_samples++;
      result.rx_ingress_or |= ingress;
    }
    {
      uint8_t state =
        (gpif & FX3_GPIF_WAVEFORM_CTRL_STAT_CURRENT_STATE_MASK) >>
        FX3_GPIF_WAVEFORM_CTRL_STAT_CURRENT_STATE_SHIFT;

      if (state == drive_state)
      result.drive_state_samples++;
      else if (state == receive_state)
        result.receive_state_samples++;
      else
        result.other_state_samples++;
    }
    if (Fx3ReadReg32(FX3_GPIF_ALPHA_STAT) & FX3_GPIF_ALPHA_DQ_OEN)
      result.dq_oen_samples++;
    previous = sample;
    previous_ingress = ingress;
  }

  Fx3GpifStop();
  Fx3GpifPibStop();
  ulpi_start_idle();
  result.samples = ULPI_PIN_DIAGNOSTIC_SAMPLES;
  *out = result;
}
