# Loading files onto the SD card over WiFi (ESP32-C3 SuperMini)

This fork lets the companion pull files from a web server on your PC
straight onto the SD card in the Tang Nano 20K, chosen from the OSD. A
disk image for the Atari ST goes from a folder on the PC to drive A in
well under a minute, no card swapping.

The Tang Nano 20K's onboard BL616 has no antenna, so the network comes
from an ESP32-C3 SuperMini on the free M0S connector. It runs Zimodem
and serves two masters: the ST uses it as a Hayes modem on the serial
port, and the companion uses it to fetch files. Both are just AT commands.

## What you need

- Tang Nano 20K with the MiSTeryNano core from
  https://github.com/rcdev67/MiSTeryNano, branch `nano20k-running`
  (second serial port on the M0S connector, SD controller fixes)
- this companion, branch `net-download`, built for `TANG_BOARD=nano20k`
- an ESP32-C3 SuperMini with Zimodem from
  https://github.com/rcdev67/Zimodem/releases (release `c3-supermini-v1`)
- a PC on the same WiFi with Python 3

## Wiring

| ESP32-C3 SuperMini | Tang Nano 20K |
|---|---|
| pin 7 (GPIO7, TX) | 41 |
| pin 6 (GPIO6, RX) | 51 |
| GND | GND |
| 5V | 5V |

Power the C3 from the Tang only, plug it in or out with the board off,
and keep the 5V lead short: a thin jumper sags under WiFi bursts. Pin 56
stays untouched.

## One-time setup

1. Flash Zimodem to the C3 (see the release page), then join it to your
   WiFi once, from the ST's terminal program or from a PC:
   `atw"MyNetwork,MyPassword"` followed by `at&w`. It reconnects on its own
   from then on.
2. Put the PC's address and a port into `atarist.ini` on the card:

       server=192.168.1.20:8888

   The port must be free on the PC; 8000 is often taken by Windows itself.
3. Make a folder on the PC for the files, say `C:\atari\share`, and start
   the server there. Windows PowerShell:

       cd C:\atari\share; python -m http.server 8888 --bind 192.168.1.20

   Linux or macOS:

       cd ~/atari/share && python3 -m http.server 8888 --bind 192.168.1.20

   Use the PC's own LAN address after `--bind`. Windows may ask once whether
   Python may accept connections: allow it for private networks. The window
   shows every request the Atari makes, which is handy when something does
   not work.
4. Put the files into that folder. That is all: the companion reads the
   folder listing the server shows for `/`. Names may be long and may
   contain spaces, they arrive on the card as they are. `.ST` disk images
   are what the OSD's disk selector lists afterwards.

   If you would rather hand out a fixed list, serve a plain text file
   instead of the listing: one file name per line, `;` starts a comment.

## Using it

1. Open the companion menu with Shift+F12 and choose `Download...`.
2. `Load file list` fetches the folder listing and shows the names.
3. Pick a file. A bar shows the progress, the end says how many bytes
   arrived. The file lands in the root of the card.
4. F12, `Disk A:`, pick the new image. Games usually want a cold start:
   set `Video: Color` if needed and use the reset entry, TOS reads the
   monitor type only at boot.

The `Serial:` setting in the OSD does not matter for downloads: the
companion switches the port to itself for the request and puts your
setting back afterwards. During a download the ST has no modem, that is
the one moment the two masters cannot share it.

## How it works

The companion talks to the modem over port 1 of the core's port protocol,
a second UART in the FPGA on the M0S pins. It sends
`AT&G"xmodem:http://server:port/name"`. Zimodem fetches the resource
into its own flash and hands it over in XMODEM blocks, each one checked
and acknowledged, so a byte lost on the serial line costs a repeated
block, not the file. For the transfer both sides switch to 115200 and
back to 19200 afterwards, so the ST finds its modem as it left it.

Every result is appended to `NETLOG.TXT` in the root of the card. If a
download fails, that line says why.

## Other hardware

The same companion runs on the M0S Dock and, with its own port, on an
ESP32-S3. Both have WiFi of their own, so no modem is needed there and
the maintainer's FTP server is available as well. This document covers
the Tang Nano 20K with its onboard BL616, where the C3 is the way to get
a network at all.
