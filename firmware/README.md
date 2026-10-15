# Ledify firmware

ESP-IDF v6.1 project for the ESP32-C6 on the Ledify board.

| File | Does |
| --- | --- |
| `main/main.c` | Startup, the command handler, LED mode loop, USB serial console |
| `main/pd.c` | USB-PD sink on the FUSB302B: requests the highest fixed voltage up to 20 V |
| `main/leds.c` | Addressable strip over RMT (WS2812 family, WS2811, SK6812, WS2816), buck enable, 5 V / 12 V select |
| `main/sensors.c` | OPT4001 light sensor (I2C), BMI323 IMU and BMP581 barometer (SPI) |
| `main/mic.c` | ICS-43434 microphone over I2S, returns an RMS level |
| `main/ble.c` | Nordic UART service on NimBLE |
| `main/net.c` | Wi-Fi station, MQTT client, Home Assistant discovery |
| `main/board.h` | Pin map and tunables (pixel limits, power budget, PD voltage, LED power settle time) |
| `partitions.csv` | 16 MB flash: 24 KB NVS for settings, 4 MB app |
| `creds.example.csv` | Template for the NVS settings image (Wi-Fi, broker, strip) |

## Build and flash

```bash
source ~/.espressif/tools/activate_idf_v6.1.sh
idf.py build
idf.py -p /dev/cu.usbmodem2101 flash monitor
```

`sdkconfig.defaults` turns on NimBLE, sets the flash size to 16 MB, selects the custom partition table and moves the console to the USB port (UART0 TX is GPIO16, which the FUSB302 interrupt uses).

## Commands

The same text commands work from three places: typed into `idf.py monitor` (USB serial console), written to the Nordic UART RX characteristic after connecting to `Ledify` with a BLE UART app, or published to the MQTT topic `ledify/<id>/cmd`.

| Command | Action |
| --- | --- |
| `on` / `off` | LED buck on or off. Boots off |
| `rgb R G B` | Fill the strip, 0-255 each |
| `level N` | Brightness cap, 0-255 |
| `count N` | How many pixels are driven, 1-1000 (`MAX_LEDS`). Saved. Also a slider in Home Assistant |
| `type ws2812` | Chip type: `ws2812` (also B/C/D/E, WS2813, WS2815), `ws2811`, `sk6812`, `sk6812w` (RGBW), `ws2816`. Saved |
| `mode solid` | Hold the colour |
| `mode rainbow` | Animated rainbow |
| `mode sound` | Brightness follows the microphone |
| `mode auto` | Brightness follows the light sensor |
| `volt 5` / `volt 12` | Select the output voltage. 12 V needs a PD contract of 15 V or more, and the output turns off first |
| `status` | JSON with power state, strip settings, the brightness cap, PD contract, network state, lux, temperature, pressure, accel, gyro and sound level |
| `pd` | The source's offered voltages and the current contract |
| `net` | Wi-Fi and MQTT state |
| `wifi SSID PASSWORD` | Save Wi-Fi credentials and restart |
| `mqtt URL [USER PASSWORD]` | Save the broker and restart, e.g. `mqtt mqtt://host:1883 ledify secret` |
| `help` | Command list |

Replies go to the log and, over BLE, to the TX characteristic. Every peripheral is probed at startup and skipped if it doesn't answer, so a board with a missing sensor still runs.

## Wiring the strip

The three pads are VCC, SIG and GND. SIG goes to the strip's **DIN** (data in; the arrows printed on the strip point away from the board). Each pixel regenerates the signal on its DOUT for the next one, so a strip's DOUT end is only for chaining another strip. WS2815 also has a backup input BI; tie the first pixel's BI to GND.

SIG is 3.3 V logic. 5 V chips specify a high level of 0.7 × VDD (3.5 V), which normally works over a short lead but is out of spec, so if a strip stays dark while VCC is present either put a level shifter (74AHCT125) on SIG or feed the first pixel's VCC through a diode so it sits at about 4.3 V. The board waits `LED_POWER_SETTLE_MS` after enabling the buck before sending the first frame and resends the solid colour once a second, so pixels that powered up late still catch it.

## Power budget

A 5 V pixel pulls about 60 mA at full white, so 300 of them want 18 A and a laptop port gives 3 A. The firmware never sends more than the supply can take: the budget is the PD contract minus buck losses (`LED_BUCK_EFF_PCT`), capped at the buck's 10 A, or 7.5 W when there is no contract; each frame's brightness is scaled so `count × colour × LED_MW_PER_CHANNEL` fits. `status` reports the resulting `cap` next to the requested `level`. On a 15 W port 300 red pixels run at 108/255 and 300 white ones at 36/255; on a 100 W charger white gets about 240. The strip's own copper still can't carry that much, so inject power every 100 pixels or so on long 5 V runs.

## Wi-Fi, MQTT and Home Assistant

Settings live in the NVS partition (namespace `cfg`): `ssid`, `pass`, `mqtt_url`, `mqtt_user`, `mqtt_pass`, `count`, `type`. Nothing secret is in the code. Set them with the `wifi` and `mqtt` commands, or build an NVS image from `creds.example.csv` (copy it outside the repo first) and flash it to `0x9000`; the comments in that file have the two commands. `mqtt://` and `mqtts://` (certificate bundle) URLs both work.

The board id is `ledify_` plus the last three bytes of its MAC, e.g. `ledify_d3ed08`.

| Topic | Direction | Payload |
| --- | --- | --- |
| `ledify/<id>/cmd` | in | Any text command from the table above |
| `ledify/<id>/light/set` | in | Home Assistant JSON light: `{"state":"ON","brightness":128,"color":{"r":255,"g":0,"b":0},"effect":"rainbow"}` |
| `ledify/<id>/count/set` | in | Pixel count as a number, from the Home Assistant slider |
| `ledify/<id>/type/set` | in | Chip type name, from the Home Assistant select |
| `ledify/<id>/light/state` | out | JSON light state |
| `ledify/<id>/state` | out | Same JSON as `status` |
| `ledify/<id>/sensors` | out | `{"lux":..,"temp_c":..,"pressure_hpa":..,"sound":..}` |
| `ledify/<id>/status` | out | `online` / `offline`, retained, set by the last will |

State is published after every command and every 30 s. On connect the board sends retained discovery messages under `homeassistant/`, so with Home Assistant's MQTT integration pointed at the same broker a light (with the four modes as effects), a pixel-count slider, a chip-type select and illuminance, temperature, pressure and sound sensors appear by themselves.

### Broker and Home Assistant on Railway

Any MQTT broker works. Mine is Mosquitto on Railway: the `eclipse-mosquitto:2` image, a TCP proxy on application port 1883 (Railway gives you a `host:port` to put in the `mqtt://` URL), `MQTT_USER` and `MQTT_PASS` variables, and this start command so the config and password file are generated at boot instead of living in a repo:

```bash
sh -c 'printf "listener 1883\nlistener 8080\nprotocol websockets\nallow_anonymous false\npassword_file /mosquitto/config/passwd\npersistence false\nlog_dest stdout\n" > /mosquitto/config/mosquitto.conf && mosquitto_passwd -b -c /mosquitto/config/passwd "$MQTT_USER" "$MQTT_PASS" && (chown mosquitto:mosquitto /mosquitto/config/passwd || true) && exec mosquitto -c /mosquitto/config/mosquitto.conf'
```

The second listener speaks MQTT over WebSockets on port 8080; put a Railway domain on that port and browsers can reach the broker over `wss://<domain>:443`.

Home Assistant runs next to it as a second Railway service from the official `ghcr.io/home-assistant/home-assistant:stable` image with a volume mounted at `/config` and a domain on port 8123. Railway's start command replaces the image entrypoint, so it seeds `configuration.yaml` with `http.use_x_forwarded_for` and `trusted_proxies` for Railway's internal ranges on the first boot and then runs `python3 -m homeassistant --config /config`. Inside the project Home Assistant reaches the broker at `mosquitto.railway.internal:1883`, and the phone app just gets the Railway URL.
