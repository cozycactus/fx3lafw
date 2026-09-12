fx3lafw
=======

This is an open source firmware for using a Cypress FX3 USB controller
as a logic analyzer with sigrok.  It does not rely on any libraries or
tools provided by Cypress under license.

This is a work in progress, some functionality such as HighSpeed
compatibility is not yet implemented.

Currently supported hardware is the CYUSB3KIT-003 "SuperSpeed Explorer
Kit".

There is [a branch of libsigrok on github](https://github.com/zeldin/libsigrok/commits/fx3lafw)
which contains the host side support.


Building
--------

On Gentoo, use `crossdev -t arm-none-eabi` to build toolchain and
newlib.

On macOS, use a full Arm GNU Toolchain build that includes the
`arm-none-eabi` newlib headers and libraries. The Homebrew
`arm-none-eabi-gcc` formula can be built `--without-headers`, which is
not enough for this firmware and fails while including standard headers
such as `stdint.h`. The Arm GNU Toolchain package works; one no-sudo
setup is to expand it under the home directory and put its tools ahead
of Homebrew on `PATH`, for example:

```
~/.local/toolchains/arm-gnu-toolchain-14.2.rel1/bin/arm-none-eabi-gcc
~/.local/toolchains/arm-gnu-toolchain-14.2.rel1/bin/arm-none-eabi-gdb
```

The firmware image can then be built using `make`. The host reset helper
can be built at the same time:

```
make clean
make -j4 all host-tools
```

The optional host reset helper can be built with `make host-tools`. It sends the
firmware `CMD_RESET` vendor request to a loaded `sigrok/fx3lafw` device:

```
tools/fx3lafw-reset
tools/fx3lafw-reset 20.45
```

`tools/fx3lafw-reload` wraps the reset command and runs `sigrok-cli` in fresh
processes to upload the firmware again and confirm the expected firmware version.
On macOS/libusb, the first post-upload process can time out waiting for
same-process re-enumeration; the helper retries with a fresh scan and only
passes after that scan reports the expected firmware.
Set `FX3_SIGROK_CLI`, `FX3_PREFIX`, or `FX3_EXPECT_FW` when using a non-default
test install.


USB link recovery
-----------------

Host power events can take the port away from the device without removing
USB power: resuming from sleep, a hub reset, or a driver restarting the
device. When the SuperSpeed link does not retrain after such an event, the
board stays powered but off the bus, and it used to need a physical replug
before any host could see it again.

The firmware now watches its own link. Once the host has enumerated the
device, a link that stays out of the connected states for a few seconds
resets the chip, so the board comes back as the FX3 bootloader. The
bootloader is always enumerable and the host driver uploads the firmware
again on the next scan, which means `sigrok-cli` or PulseView simply
re-detect the analyzer instead of asking for a replug. A capture that was
running when the link died is lost.

`tools/fx3lafw-usb-events` prints the counters behind this behaviour
(suspend/resume/reset events, link up/down, setup packets, self resets) for
a loaded device, which makes a misbehaving port visible.


License
-------

Copyright (c) 2018 Marcus Comstedt

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be
included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
