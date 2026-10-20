# HP LaserJet 1010 print server (ESP32-S3)

This board joins your Wi-Fi and accepts print jobs for an HP LaserJet 1010. The computer must send PCL 5e. The board only forwards those bytes to the printer.

It does not support the HP LaserJet 1020 or 1018. Those models need a firmware upload after every power-on.

## What plugs in where

The board has two USB-C sockets.

1. **Computer socket**, the one with the CH343 chip (often labeled UART). Cable from this socket to the computer. That cable powers the board, loads the firmware, and shows the log.
2. **Printer socket**, the other USB-C. Cable from this socket to the printer's USB-B port (the square one).

The printer socket has to feed 5 V down the printer cable, or the printer never appears. On an official Espressif ESP32-S3-DevKitC-1 that 5 V is disconnected until the USB-OTG jumper on the back of the board is soldered closed. The two-USB-C board this firmware was brought up on already connects it. If the log stays on `printer GONE`, that jumper is open.

The matching module is an ESP32-S3 with 16 MB flash and 8 MB PSRAM (N16R8). `boards/esp32-s3-n16r8.json` selects that.

## Build

[PlatformIO](https://platformio.org/) is required.

```sh
cp .env.example .env
# set WIFI_SSID and WIFI_PASS
pio run -t upload
pio device monitor
```

Flash and the log both use the computer socket. If the upload picks the wrong port, we recommend `--upload-port` for that cable.

`gen_wifi.py` runs before the build and writes `src/wifi_secret.h`. That file and `.env` are gitignored. The Wi-Fi password is stored in the firmware image in the clear. We recommend not publishing a `.bin` built from a real `.env`.

The serial log prints `listen 9100` and the DHCP address when Wi-Fi connects. The DHCP hostname is `lj1010`. We recommend plugging the printer in before sending a job. The first job waits up to three seconds for the printer to appear.

## Printing

The listener accepts one raw TCP job at a time, the same way as an HP Jetdirect socket on port 9100. There is no authentication, IPP, or AirPrint. Anyone who can open that port can print. We recommend keeping it on the LAN.

The board does not answer status requests. A driver that waits for a reply will stall. We recommend a raw PCL 5e queue.

A client that sends no bytes for 30 seconds is dropped.

A PCL 5e test page is in `samples/testpage.pcl`. It is laid out for A4. A Letter tray may stop or clip it.

```sh
nc -w 10 <board-ip> 9100 < samples/testpage.pcl
```

`<board-ip>` is the address printed in the log after `listen 9100`. We recommend pointing the OS queue at that address, protocol raw / Jetdirect / socket, port 9100, with a PCL 5e driver (HP LaserJet 1010, or HP's PCL5 universal driver). PCL 6 and PostScript are forwarded unchanged and the 1010 will not print them.

## License

MIT. See [LICENSE](LICENSE).
