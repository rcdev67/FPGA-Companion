# Tang Nano 20K with the onboard BL616: board versions 3921 and 3923

The Tang Nano 20K exists in two board versions that need different
companion images. I run one of each, both with this fork. This page says
what differs, how to find out which one you have, and how to flash them.

## What differs

| | 3921 | 3923 |
|---|---|---|
| BL616 SPI MISO (from the FPGA) | GPIO 2 | GPIO 30 |
| BL616 SPI MOSI (to the FPGA) | GPIO 3 | GPIO 27 |
| SPI CSn, SCK, IRQ, debug console | same | same |
| HDMI termination | 49.9 ohm to 3.3 V | 499 ohm to GND |
| Companion build | `make TANG_BOARD=nano20k` | `make TANG_BOARD=nano20k_v3923` |
| FPGA Partner (boot stage at 0x0) | same file for both | same file for both |
| FPGA core | same bitstream for both | same bitstream for both |

So only the companion image is version specific. The maintainers ship the
FPGA Partner under two names, the two files are byte for byte identical.
According to the maintainers the 3923 appeared in mid-2026; its bottom
silkscreen also has the pin labels 25/26 corrected. My 3923 was bought in
September 2026.

## Which one do I have?

I found no marking that tells them apart reliably, but the board tells you
itself, and trying the wrong image does no harm because the two GPIO pairs
are unused on the other version:

1. Flash the core and one of the two companion images (see below).
2. Power the board from a plain supply, not from a PC (see the next
   section for why).
3. With the right image the companion finds the FPGA: the settings from
   `atarist.ini` apply (for me: monochrome and the autostart disk), F12
   opens the OSD, and the console says `FPGA ready after ...ms`. With the
   wrong image the ST still boots, but with the core's defaults (colour,
   plain desktop), F12 does nothing and the console stays silent. Then
   flash the other image.

## PC or power supply decides what the BL616 does

The FPGA Partner starts the companion only when the board's USB-C socket
is **not** connected to a computer. Plugged into a PC the BL616 stays a
`SIPEED USB Debugger` (FT2232), the companion does not run, and the
console is silent. On a USB power supply, a power bank, the 5V pins or a
dock the companion starts. This is not a fault, but it looks like one when
you test a fresh flash on the PC's USB port.

The useful side: while the board hangs on a PC, the core can be flashed
with openFPGALoader. My 3923 came up as the debugger on the PC without
any button; on my 3921 I hold S2 while plugging it into the PC.

## Flashing

Save the factory state first, it takes a minute:

    openFPGALoader -b tangnano20k --external-flash --dump-flash --file-size 8388608 -o 0 factory_fpga_flash.bin
    BLFlashCommand --interface=uart --baudrate=2000000 --port=COMx --chipname=bl616 --read --flash --start 0x0 --len 0x400000 --file factory_bl616.bin

(the second one with the board in update mode, see below).

The core and TOS, board on a PC:

    openFPGALoader -b tangnano20k -f --verify atarist.fs
    openFPGALoader -b tangnano20k --external-flash -o 0x100000 tos104de.img

The companion: unplug, hold `UPDATE`, plug into the PC, release. The BL616
shows up as a serial port. Then, from `src/bl616` with the partner binary
and the freshly built companion next to the ini:

    BLFlashCommand --interface=uart --baudrate=2000000 --port=COMx --chipname=bl616 --config=flash_nano20k.ini
    BLFlashCommand --interface=uart --baudrate=2000000 --port=COMx --chipname=bl616 --config=flash_nano20k_v3923.ini

Both write the FPGA Partner to `0x0` and the companion to `0x40000`; they
differ only in the companion's file name. The partner binary
`bl616_fpga_partner_nano20k.bin` comes from the maintainers' releases, its
source is not published.

## The console

The core passes the BL616's debug console to **pin 48** of the right pin
header (output, 2,000,000 baud, 8N1; pin 55 is the input). A USB serial
adapter with RX on 48 and GND on GND shows what the companion is doing:
start-up, USB devices coming and going, the modem dialogue of the network
download. FTDI, CH340 and CH343 adapters manage 2 Mbaud, a CP2102 does not.

Mind the header: **5V is the top pin, GND the one right below it.** I
once put the adapter's GND on 5V; that shorts two USB ports of the PC
through the board and the PC locks them. A printable pin label sheet with
true-to-scale strips is in the
[MiSTeryNano fork](https://github.com/rcdev67/MiSTeryNano/tree/nano20k-running/doc).
