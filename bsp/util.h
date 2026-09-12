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

#ifndef BSP_UTIL_H_
#define BSP_UTIL_H_

#include <stdint.h>

extern void Fx3UtilDelayUs(uint32_t delay_us);

/* Poll a register until (value & mask) == expected, for up to about
 * timeout_us microseconds.  Returns 1 when the condition was met and 0
 * on timeout.  Hardware handshakes must never spin forever: a block
 * that stays stuck would otherwise take the whole firmware off the USB
 * bus and need a physical reset.
 */
extern int Fx3UtilPollReg32(uint32_t reg, uint32_t mask, uint32_t expected,
			    uint32_t timeout_us);

#endif /* BSP_UTIL_H_ */
