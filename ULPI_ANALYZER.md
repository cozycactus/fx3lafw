FX3 USB3300 ULPI analyzer
=========================

This experimental target combines a CYUSB3KIT-003 with a USB3300 ULPI PHY. It
streams the raw ULPI bus to sigrok. USB packet and request decoding remains in
libsigrokdecode rather than in the firmware.

The normal `fx3lafw-cypress-fx3.fw` target is unchanged. The specialized image
enumerates as `sigrok / fx3ulpifw` and is recognized as the `FX3 ULPI analyzer`
profile by the `fx3lafw` libsigrok driver.


sigrok architecture
-------------------

The analyzer follows the normal sigrok layering:

* `fx3ulpifw` only configures the PHY and streams fixed-width raw logic samples.
* The existing `fx3lafw` driver owns USB transport and exposes this hardware as
  a device profile; there is no duplicate transport driver.
* D0-D7, DIR, NXT, and STP remain ordinary logic channels.
* USB speed selection uses the standard `SR_CONF_DEVICE_MODE` configuration
  key, so frontends do not need device-specific controls.
* The `ulpi` protocol decoder interprets the bus and emits the standard
  `usb_packet` Python output, which can be stacked directly with `usb_request`.


Current scope
-------------

* Fixed 60 MHz sampling from USB3300 CLKOUT.
* Fixed 16-bit samples.
* Raw channels: D0-D7, DIR, NXT, and optional STP.
* Manually selected high-speed, full-speed, or low-speed receiver mode.
* The USB3300 transmitter and all integrated USB pull-up/pull-down resistors are
  disabled by setting `OpMode=Non-Driving` and clearing OTG Control.
* Common token, data, and handshake packets stack into the standard
  `usb_request` decoder.

Automatic tracking of USB reset and high-speed chirp negotiation is not
implemented yet. High-speed mode therefore targets a bus that has already
negotiated high-speed operation. Capturing one uninterrupted trace from initial
full-speed attach through the transition to high-speed requires an additional
speed-tracking state machine.


Build and load
--------------

Build the dedicated image and host loader:

```
make -j4 ulpi host-tools
```

Load it explicitly:

```
tools/fx3lafw-load fx3ulpifw-cypress-fx3.fw
```

The loader validates the Cypress image before writing it. If standard
`fx3lafw` or an older `fx3ulpifw` is already running, it resets that firmware,
waits for the same physical USB port to return in bootloader mode, uploads the
new image, and verifies the `sigrok / fx3ulpifw` product identity.

For a non-sigrok image, provide the expected runtime USB ID explicitly. For
example, the Cypress SDK BulkSrcSink example uses `04b4:00f1`:

```
tools/fx3lafw-load --verify --expect-usb 04b4:00f1 cyfxbulksrcsink.img
```

The SDK High-Speed HID control uses `04b4:6025`. A successful SRAM readback or
launch request is not proof of firmware enumeration: the loader must find the
runtime identity, not the old `04b4:00f3` WestBridge BootROM. Enumeration alone
also does not prove working bulk transfers or valid ULPI samples. Use
`make check-host` to run the loader identity and raw ULPI parser regression
tests without hardware.

The ordinary libsigrok scan never chooses `fx3ulpifw` for a bare
CYUSB3KIT-003. This avoids an ambiguous automatic firmware choice between the
logic-analyzer and ULPI-analyzer images.


ULPI wiring
-----------

Connect the USB3300 signals to the FX3 GPIF signals as follows. The J6 pin
numbers are from Figure 4-5 of the CYUSB3KIT-003 user guide, viewed from the
connector side with pin 1 at the marked end.

| USB3300 signal | Waveshare CN1 | FX3 signal | CYUSB3KIT-003 | Sample bit |
|---|---:|---|---:|---:|
| DATA0 | 1 | DQ0 | J6-38 | 0 |
| DATA1 | 3 | DQ1 | J6-36 | 1 |
| DATA2 | 5 | DQ2 | J6-34 | 2 |
| DATA3 | 7 | DQ3 | J6-32 | 3 |
| DATA4 | 9 | DQ4 | J6-30 | 4 |
| DATA5 | 11 | DQ5 | J6-28 | 5 |
| DATA6 | 13 | DQ6 | J6-26 | 6 |
| DATA7 | 15 | DQ7 | J6-24 | 7 |
| DIR | 12 | CTL2 and DQ10 | J6-27 and J6-18 | 10 |
| NXT | 14 | CTL0 and DQ9 | J6-31 and J6-20 | 9 |
| STP | 16 | CTL3 and DQ8 | J6-25 and J6-22 | 8 |
| CLKOUT | 10 | PCLK | J6-35 | not captured |
| RESET | 8 | optional | optional | not captured |
| 3.3 V | 19 or 20 | V3P3 | J6-1 | - |
| GND | 17 or 18 | GND | J6-5 or another board GND | - |

DIR and NXT are each wired to both a control input and a data input. GPIF uses
CTL2's dedicated active-low OE function for DIR and CTL0 for NXT; DQ10 and DQ9
preserve them as ordinary logic channels in the raw trace. STP is driven by
CTL3 and looped back to DQ8 so it remains visible in the raw trace. The
libsigrok profile names sample bits 8-10 as STP, NXT, and DIR, respectively, so
protocol-decoder channel assignment by name works without rewriting samples.
CLKOUT is the GPIF sampling clock, so it does not consume a capture channel.
The analyzer firmware keeps STP low while capturing, as required for an active
ULPI Link interface.

The acquisition waveform also drives the zero ULPI Idle word on DQ0-DQ7
while DIR is low, and releases those drivers through CTL2 when DIR is high.
The upper DQ8-DQ15 pins remain GPIO input overrides, so enabling the GPIF
data outputs does not drive the mirrored PHY control signals. The fixed
delay-zero waveform uses the unchanged zero data counter as its output source.

The CYUSB3KIT-003 VIO domains must be set to 3.3 V (J2 short, which is the
factory default). Remove the J5 SRAM-enable jumper (or park it on one pin).
J5 connects CTL0 to the onboard SRAM chip select; leaving it closed lets the
SRAM contend with USB3300 and FX3 on DQ0-DQ15. J6-5 is mechanically keyed off
on some kits; use another accessible board ground when it cannot be contacted.
Do not connect the Waveshare 5 V pins, J6 USB3_VBUS, or J6 V1P2 to the ULPI
module.


Waveshare USB connector caution
-------------------------------

The published Waveshare schematic directly joins VBUS, D+, D-, and ground
between USB2 (Micro-B) and USB1 (Type-A). R2 = 510 ohms is only in series with
the USB3300 VBUS sense input. Leave R2 unchanged.

For an inline prototype, connect the USB host to USB2 and the device under test
to USB1. Power the module from 3.3 V through CN1 and leave both CN1 5 V pins
disconnected. Before attaching a device, verify continuity between the two
connector VBUS pins and check that neither CN1 5 V pin is driving VBUS.

This three-node D+/D- connection is a passive electrical tap, not a
USB-IF-compliant analyzer front end. The USB3300 input capacitance, PCB branch,
and two connectors can degrade a 480 Mbit/s link. Use short cables and prove
high-speed operation on the physical setup; firmware and logic-level tests
cannot establish signal integrity.


sigrok decoding
---------------

The `ulpi` decoder accepts either:

* one sample for every rising CLKOUT edge, with no CLK channel; or
* an asynchronously sampled trace with CLK assigned, sampled on its rising
  edge.

For the FX3 profile, map channels D0-D7, DIR, NXT, and optionally STP. Stack
`usb_request` directly after `ulpi`. The firmware sends raw bus states and does
not filter or reinterpret packets.

RXCMD bytes with RxActive set may appear between bytes of the same packet;
they must not split it. Either an RxInactive RXCMD or deassertion of DIR ends
the packet. Both the native checker and decoder validate length and CRC after
that boundary. The optional NXT tail correction is diagnostic, not evidence
of a bit-correct capture; use raw mode for capture qualification.


Validation status (2026-09-09)
-----------------------------

The dedicated firmware remains experimental. An SDK USB3 control and a
short 3,000,000-sample ULPI capture passed, but this does not establish a
working continuous analyzer. The short capture contained 398 SOF tokens and
no DATA packets. A 60,000,000-sample capture contained 7,998 SOF tokens, one
with a spurious fourth byte. Longer/active-audio attempts also encountered
PIB write overflows and stopped early; received DATA packets had CRC16 errors.
No software byte correction was enabled for these results.

DLL phase sweeps did not establish a clean data-capture setting. Keep the
default rising edge/phase zero as a baseline, not as a qualified final timing
configuration. Ordinary logic-analyzer firmware was byte-for-byte unchanged
by the acquisition idle-drive changes.

With only the upstream host-to-Waveshare USB cable disconnected, register
access recovered without a reset. Two batches of 50 complete register
setup/readback tests passed, and two static-input captures each returned all
60,000,000 requested samples without a reported PIB error. These empty
captures do not qualify USB packet fidelity or sample continuity. An attempted
register-read preemption guard failed the offline hardware test and was
reverted; the original image is restored. Capture under active USB traffic
and reliable register access during that traffic remain unqualified.

After reconnecting the upstream cable with PHY register setup explicitly
skipped, a short capture again passed, but the 60M-sample run stopped at
58,589,184 samples with PIB 0x1005 and one malformed SOF. Under a 48 kHz
silent playback stream, capture stopped at 48,176 samples with PIB 0x1006;
two complete DATA packets failed CRC16 and the final packet was truncated.
Scarlett remained enumerated at 480 Mbps. Thus the acquisition failure is
not eliminated by avoiding register setup, and does not require an input
USB disconnect. Its underlying timing/electrical/GPIF cause remains unproven.

A subsequent isolation test used a temporary ULPI image with an internal
96 MHz sample clock, PCLK output disabled, and DLL off. It returned all 60M
and 120M requested 16-bit samples under checked audio playback, at a nominal
192 MB/s, with active DATA/NXT/DIR and no reported PIB overflow. These are
asynchronous snapshots without a CLK channel, not bit-perfect USB captures;
the unchanged CLI's 60 MHz metadata does not describe this diagnostic image.
After restoring the original external-60-MHz image, active capture failed
again at 62168 samples with PIB 0x1006 and CRC16 errors. External capture with
DLL disabled also failed, at 23068672 samples with 2170 CRC16 errors.

This rules against a simple USB3 bandwidth ceiling in the tested setup and
focuses investigation on the external-clock/GPIF synchronous acquisition
path. It does not distinguish clock wiring from GPIF timing/configuration,
or qualify the electrical USB2 tap. The original source, both firmware-image
hashes and rising-edge phase-zero baseline were restored. The final idle
3M-sample capture contained 398 valid SOFs, err=0, pause=0. Continuous ULPI
capture remains unqualified. Detailed results and diagnostic source/images
are under libsigrok/tmp/fx3-ulpi-20260909.86G0Ac/timing-isolation/.


References
----------

* [CYUSB3KIT-003 user guide](https://www.infineon.com/dgdl/Infineon-SuperSpeed_Explorer_Kit_User_Guide-UserManual-v01_00-EN.pdf?fileId=8ac78c8c7d0d8da4017d0ef82cf70d57),
  Figure 4-5.
* [USB3300 data sheet](https://ww1.microchip.com/downloads/aemDocuments/documents/UNG/ProductDocuments/DataSheets/00001783C.pdf),
  Sections 6.1.4.5, 6.1.5.1, and 6.2.2.
* [Waveshare USB3300 board schematic](https://www.waveshare.com/w/upload/9/9e/USB3300-USB-HS-Board-Schematic.pdf).
