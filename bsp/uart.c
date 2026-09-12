/*
 * Copyright (C) 2018 Marcus Comstedt
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of
 * this software and associated documentation files (the "Software"), to deal in
 * the Software without restriction, including without limitation the rights to
 * use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
 * the Software, and to permit persons to whom the Software is furnished to do so,
 * subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 * IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <bsp/uart.h>
#include <bsp/gctl.h>
#include <bsp/regaccess.h>
#include <bsp/util.h>
#include <rdb/uart.h>
#include <rdb/gctl.h>

/*
 * The console must never be able to stop the firmware: it is the only way to
 * see how far boot got, and a UART that is unpowered, unrouted or not being
 * drained would otherwise spin forever inside Fx3UartTxString() long before
 * Fx3UsbConnect(), leaving the board looking dead to the host.
 */
#define FX3_UART_WAIT_US 10000

static uint8_t uart_ready;

static uint8_t Fx3UartWaitForBit(uint32_t reg, uint32_t mask,
				 unsigned timeout_us)
{
  unsigned waited;

  for (waited = 0; waited < timeout_us; waited += 5) {
    if (Fx3ReadReg32(reg) & mask)
      return 1;
    Fx3UtilDelayUs(5);
  }
  return (Fx3ReadReg32(reg) & mask) != 0;
}

void Fx3UartInit(uint32_t baud_rate, Fx3UartParity_t parity, Fx3UartStopBits_t stop_bits)
{
  uart_ready = 0;

  /* Configure baud rate generator */
  Fx3WriteReg32(FX3_GCTL_UART_CORE_CLK,
		(((SYS_CLK/16 / baud_rate - 1) << FX3_GCTL_UART_CORE_CLK_DIV_SHIFT) & FX3_GCTL_UART_CORE_CLK_DIV_MASK) |
		(3UL << FX3_GCTL_UART_CORE_CLK_SRC_SHIFT) |
		FX3_GCTL_UART_CORE_CLK_CLK_EN);

  /* Reset UART */
  Fx3WriteReg32(FX3_UART_POWER, 0);
  Fx3UtilDelayUs(10);
  Fx3WriteReg32(FX3_UART_POWER, FX3_UART_POWER_RESETN);
  if (!Fx3UartWaitForBit(FX3_UART_POWER, FX3_UART_POWER_ACTIVE,
			 FX3_UART_WAIT_US))
    return;

  /* Configure and enable UART */
  Fx3WriteReg32(FX3_UART_CONFIG,
		stop_bits | parity |
		FX3_UART_CONFIG_ENABLE |
		FX3_UART_CONFIG_TX_ENABLE);
  uart_ready = 1;
}

uint8_t Fx3UartIsReady(void)
{
  return uart_ready;
}

void Fx3UartTxByte(uint8_t byte)
{
  if (!uart_ready)
    return;
  if (!Fx3UartWaitForBit(FX3_UART_STATUS, FX3_UART_STATUS_TX_SPACE,
			 FX3_UART_WAIT_US)) {
    /* A working UART drains a byte in ~87us at 115200; give up for good. */
    uart_ready = 0;
    return;
  }
  Fx3WriteReg32(FX3_UART_EGRESS_DATA, byte);
}

void Fx3UartTxBytes(const uint8_t *byte, size_t cnt)
{
  while(cnt--)
    Fx3UartTxByte(*byte++);
}

extern void Fx3UartTxChar(char c)
{
  if (c == '\n')
    Fx3UartTxByte('\r');
  Fx3UartTxByte(c);
}

extern void Fx3UartTxString(const char *str)
{
  char c;
  while ((c = *str++))
    Fx3UartTxChar(c);
}

void Fx3UartTxFlush(void)
{
  if (!uart_ready)
    return;
  Fx3UartWaitForBit(FX3_UART_STATUS, FX3_UART_STATUS_TX_DONE,
		    FX3_UART_WAIT_US);
}
