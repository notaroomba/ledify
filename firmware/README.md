# Ledify firmware

ESP-IDF v6.1 project for the ESP32-C6 on the Ledify board.

| File | Does |
| --- | --- |
| `main/main.c` | Startup, the command handler, the frame loop that composes colour × brightness × overlay, USB serial console |
| `main/pd.c` | USB-PD sink on the FUSB302B: requests the highest fixed voltage up to 20 V |
| `main/leds.c` | Addressable strip over RMT (WS2812 family, WS2811, SK6812, WS2816), the colour sources, the power cap, buck enable, 5 V / 12 V select |
| `main/sensors.c` | OPT4001 light sensor (I2C), BMI323 IMU and BMP581 barometer (SPI) |
| `main/mic.c` | ICS-43434 microphone over I2S in its own task: loudness with auto gain, beat onsets, bass/mid/treble |
| `main/fft.c` | 256-point fixed-point FFT and the band sums, plain C (`test/fft_test.c` checks it on the host) |
| `main/ble.c` | Nordic UART service on NimBLE |
| `main/net.c` | Wi-Fi station (primary + fallback network), MQTT client, Home Assistant discovery |
| `main/ota.c` | Over-the-air update: fetches an app image over Wi-Fi into the spare slot |
| `main/board.h` | Pin map and tunables (pixel limits, frame rate, power budget, PD voltage, LED power settle time) |
| `partitions.csv` | 16 MB flash: 24 KB NVS for settings, two 4 MB app slots for OTA |
| `creds.example.csv` | Template for the NVS settings image (Wi-Fi, broker, strip, lighting tunables) |

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
| `mode NAME` | Preset, the list Home Assistant shows as effects: `solid`, `rainbow`, `temp`, `spectrum`, `meter`, `sound`, `vu`, `pulse`. A colour resets the brightness source and overlay, `vu` brings its own meter colours, `sound` and `pulse` keep the colour. `mode auto` still works as `auto on` |
| `color NAME` | Colour source on its own: `solid`, `rainbow`, `temp`, `spectrum`, `meter` (see [Mixing styles](#mixing-styles)) |
| `bright level` / `bright sound` | Brightness source: the fixed `level`, or `level` scaled by the microphone |
| `overlay none` / `vu` / `pulse` | Light only as many pixels as the sound level, or flash on beats |
| `auto on` / `auto off` | Scale the brightness with the light sensor between `lux_lo` and `lux_hi`, off under `night` |
| `gain N` | Microphone gain 1-100, default 25: how much a quiet room is amplified. Saved |
| `lux LO HI` | Lux at which auto brightness sits at its floor and reaches full, default `0 500`. Saved; also one at a time as `lux_lo N` / `lux_hi N` |
| `night N` | Lux below which auto brightness turns the strip off, 0 (default) disables. Saved |
| `temp LO HI` | °C shown as blue and as red by the `temp` colour, white halfway, default `15 30`. Saved; also `temp_lo N` / `temp_hi N` |
| `temp 21.5` | Feed a temperature in; the `temp` colour uses it instead of the barometer until the next restart |
| `volt 5` / `volt 12` | Select the output voltage. 12 V needs a PD contract of 15 V or more, and the output turns off first |
| `status` | JSON with power state, strip settings, colour/brightness/overlay, the saved tunables, the brightness cap, PD contract, network state, lux, temperature, pressure, accel, gyro, sound level and loudness after gain |
| `pd` | The source's offered voltages and the current contract |
| `net` | Wi-Fi and MQTT state |
| `wifi SSID PASSWORD` | Save Wi-Fi credentials and restart |
| `wifi2 SSID PASSWORD` | Save a fallback network and restart (`wifi2 off` clears it). The board joins the primary first and switches after `WIFI_FAILS_BEFORE_SWITCH` failed attempts, alternating until one works |
| `mqtt URL [USER PASSWORD]` | Save the broker and restart, e.g. `mqtt mqtt://host:1883 ledify secret` |
| `ota URL` | Download a new app image over Wi-Fi and restart into it (see below) |
| `help` | Command list |

Replies go to the log and, over BLE, to the TX characteristic. Every peripheral is probed at startup and skipped if it doesn't answer, so a board with a missing sensor still runs; sound styles on a board without a microphone answer `ok, no mic` and stay dark.

## Mixing styles

Every frame is **colour × brightness × overlay**. `main.c` composes the three from the sensors and `leds_frame()` in `leds.c` draws it; that is the only path to the strip, so the power cap applies to every style. Each part has its own command, and `mode` is a preset over them.

| Colour source | What each pixel shows |
| --- | --- |
| `solid` | The `rgb` colour |
| `rainbow` | A hue sweep drifting along the strip |
| `temp` | One colour from blue at `temp_lo` through white to red at `temp_hi`, from the barometer or a fed-in `temp` |
| `spectrum` | Three bars, red / green / blue, whose lengths follow bass / mid / treble |
| `meter` | Green to yellow to red along the strip, the classic VU colours |

| Brightness | What happens to `level` |
| --- | --- |
| `bright level` | Used as is |
| `auto on` | Scaled between a floor of 20/255 at `lux_lo` and full at `lux_hi`; 0 below `night`, back on above twice `night` |
| `bright sound` | Multiplied by the loudness |

| Overlay | |
| --- | --- |
| `none` | |
| `vu` | Only the first `loudness × count` pixels are lit |
| `pulse` | Full brightness on each beat, a quarter in between |

They stack: `color rainbow`, `auto on`, `overlay vu` is a rainbow VU meter that dims in a dark room. The presets map onto the three like this:

| `mode` / Home Assistant effect | colour | bright | overlay |
| --- | --- | --- | --- |
| `solid`, `rainbow`, `temp`, `spectrum`, `meter` | that one | `level` | `none` |
| `sound` | unchanged | `sound` | `none` |
| `vu` | `meter` | `level` | `vu` |
| `pulse` | unchanged | `level` | `pulse` |
| `auto` | unchanged | unchanged, turns `auto` on | unchanged |

`auto` stays out of the presets so it survives switching effects; in Home Assistant it is the "auto brightness" switch.

### Sound processing

A microphone task reads 256 samples (8 ms) at a time at 32 kHz, so the main loop never waits for audio. Per block: RMS × `gain`, held under a peak follower with a 2 s decay, gives the loudness, so a quiet room stays dark at a given gain while a loud room isn't pinned at 100 %. A beat is a block more than 1.5× its 1 s running average, at most one per 100 ms. Every fourth block the last 16 ms, decimated to 16 kHz, go through a Hann window and the fixed-point FFT in `fft.c` (62.5 Hz bins: bass 1-3, mid 4-31, treble 32-127), each band normalised against its own peak. `status` shows `sound` (raw RMS, 0-1) and `loud` (0-255 after gain) for tuning `gain`. Styles that move redraw every `FRAME_MS` (33 ms, about 9 ms of which is the wire time for 300 pixels); still ones once a second.

## Wiring the strip

The three pads are VCC, SIG and GND. SIG goes to the strip's **DIN** (data in; the arrows printed on the strip point away from the board). Each pixel regenerates the signal on its DOUT for the next one, so a strip's DOUT end is only for chaining another strip. WS2815 also has a backup input BI; tie the first pixel's BI to GND.

SIG is 3.3 V logic. 5 V chips specify a high level of 0.7 × VDD (3.5 V), which normally works over a short lead but is out of spec, so if a strip stays dark while VCC is present either put a level shifter (74AHCT125) on SIG or feed the first pixel's VCC through a diode so it sits at about 4.3 V. The board waits `LED_POWER_SETTLE_MS` after enabling the buck before sending the first frame and resends a still frame once a second, so pixels that powered up late still catch it.

## Power budget

A 5 V pixel pulls about 60 mA at full white, so 300 of them want 18 A and a laptop port gives 3 A. The firmware never sends more than the supply can take: the budget is the PD contract minus buck losses (`LED_BUCK_EFF_PCT`), capped at the buck's 10 A, or 7.5 W when there is no contract; each frame's brightness is scaled so `count × colour × LED_MW_PER_CHANNEL` fits. `status` reports the resulting `cap` next to the requested `level`. On a 15 W port 300 red pixels run at 108/255 and 300 white ones at 36/255; on a 100 W charger white gets about 240. The strip's own copper still can't carry that much, so inject power every 100 pixels or so on long 5 V runs.

## Updating over the air

Flash with the cable once to get the two-slot partition table; after that `ota URL` from the console, BLE or `ledify/<id>/cmd` pulls `ledify.bin` over Wi-Fi into the other slot, verifies it and restarts. `status` shows the running version as `fw` (the git describe of the build). The bootloader boots the new image on trial: if the board does not stay up for 30 s (`ota_mark_ok()` in `main.c`) the next reset falls back to the previous image, so a bad build never bricks it. The Wi-Fi radio never modem-sleeps, so commands and MQTT keepalives are not delayed.

Two easy places to host the image:

```bash
# same network as the board (e.g. both on the hotspot): serve the build directory
python3 -m http.server 8000 --directory firmware/build
# then on the board:  ota http://<this computer's ip>:8000/ledify.bin
```

or attach `ledify.bin` to a GitHub release and use its `https://github.com/.../releases/download/...` URL; TLS is checked against the built-in certificate bundle and redirects are followed. Progress is logged every 10 %.

## Wi-Fi, MQTT and Home Assistant

Settings live in the NVS partition (namespace `cfg`): `ssid`, `pass`, `ssid2`, `pass2`, `mqtt_url`, `mqtt_user`, `mqtt_pass`, `count`, `type`, and the tunables `gain`, `lux_lo`, `lux_hi`, `night`, `temp_lo`, `temp_hi`. Nothing secret is in the code. Set them with the `wifi` and `mqtt` commands, or build an NVS image from `creds.example.csv` (copy it outside the repo first) and flash it to `0x9000`; the comments in that file have the two commands. `mqtt://` and `mqtts://` (certificate bundle) URLs both work.

The board id is `ledify_` plus the last three bytes of its MAC, e.g. `ledify_d3ed08`.

| Topic | Direction | Payload |
| --- | --- | --- |
| `ledify/<id>/cmd` | in | Any text command from the table above |
| `ledify/<id>/light/set` | in | Home Assistant JSON light: `{"state":"ON","brightness":128,"color":{"r":255,"g":0,"b":0},"effect":"rainbow"}` |
| `ledify/<id>/KEY/set` | in | A bare value, run as the command `KEY value`. The Home Assistant entities use `count`, `type`, `auto` (`ON`/`OFF`), `gain`, `lux_lo`, `lux_hi`, `night`, `temp_lo`, `temp_hi`; `temp` feeds a temperature in for the `temp` colour |
| `ledify/<id>/light/state` | out | JSON light state |
| `ledify/<id>/state` | out | Same JSON as `status` |
| `ledify/<id>/sensors` | out | `{"lux":..,"temp_c":..,"pressure_kpa":..,"sound_db":..}` (kPa, approximate dB SPL) |
| `ledify/<id>/status` | out | `online` / `offline`, retained, set by the last will |

State is published after every command and every 30 s. On connect the board sends retained discovery messages under `homeassistant/`, so with Home Assistant's MQTT integration pointed at the same broker these appear by themselves: a light with the presets as effects (`solid`, `rainbow`, `temp`, `spectrum`, `meter`, `sound`, `vu`, `pulse`), an "auto brightness" switch, one number entity per saved value (pixels, gain, lux low, lux high, night lux, temp low, temp high), a chip-type select, and illuminance, temperature, pressure and sound sensors.

### Automations

Three examples that combine the pieces. The board id is `ledify_d3ed08` here; entity ids come from the names above.

```yaml
# Sunset: temperature colours at 40 %, fed from the weather integration instead of the warm on-board sensor
- alias: Ledify sunset
  triggers:
    - trigger: sun
      event: sunset
  actions:
    - action: light.turn_on
      target:
        entity_id: light.ledify
      data:
        effect: temp
        brightness_pct: 40
    - action: mqtt.publish
      data:
        topic: ledify/ledify_d3ed08/temp/set
        payload: "{{ state_attr('weather.home', 'temperature') }}"

# Dark room for two minutes: off. (The "night lux" number does the same on the board, without Home Assistant.)
- alias: Ledify lights out
  triggers:
    - trigger: numeric_state
      entity_id: sensor.ledify_illuminance
      below: 5
      for: "00:02:00"
  actions:
    - action: light.turn_off
      target:
        entity_id: light.ledify

# Party mode: spectrum bars with the gain set for the room
- alias: Ledify party
  triggers:
    - trigger: state
      entity_id: input_boolean.party_mode
      to: "on"
  actions:
    - action: number.set_value
      target:
        entity_id: number.ledify_gain
      data:
        value: 20
    - action: light.turn_on
      target:
        entity_id: light.ledify
      data:
        effect: spectrum
        brightness: 255
```

### Broker and Home Assistant on Railway

Any MQTT broker works. Mine is Mosquitto on Railway: the `eclipse-mosquitto:2` image, a TCP proxy on application port 1883 (Railway gives you a `host:port` to put in the `mqtt://` URL), `MQTT_USER` and `MQTT_PASS` variables, and this start command so the config and password file are generated at boot instead of living in a repo:

```bash
sh -c 'printf "listener 1883\nlistener 8080\nprotocol websockets\nallow_anonymous false\npassword_file /mosquitto/config/passwd\npersistence false\nlog_dest stdout\n" > /mosquitto/config/mosquitto.conf && mosquitto_passwd -b -c /mosquitto/config/passwd "$MQTT_USER" "$MQTT_PASS" && (chown mosquitto:mosquitto /mosquitto/config/passwd || true) && exec mosquitto -c /mosquitto/config/mosquitto.conf'
```

The second listener speaks MQTT over WebSockets on port 8080; put a Railway domain on that port and browsers can reach the broker over `wss://<domain>:443`.

Home Assistant runs next to it as a second Railway service from the official `ghcr.io/home-assistant/home-assistant:stable` image with a volume mounted at `/config` and a domain on port 8123. Railway's start command replaces the image entrypoint, so it seeds `configuration.yaml` with `http.use_x_forwarded_for` and `trusted_proxies` for Railway's internal ranges on the first boot and then runs `python3 -m homeassistant --config /config`. Recent Home Assistant versions migrate that `http:` block into `.storage/http` once and treat it as a pending trial: log in and confirm it within five minutes or it reverts and every proxied request gets a 400. If that happens, `railway ssh` into the service, copy the `pending` object over `stable` in `/config/.storage/http` (clearing its `error`), set `pending` to null, drop the `http:` block from `configuration.yaml`, and restart the service. Inside the project Home Assistant reaches the broker at `mosquitto.railway.internal:1883`, and the phone app just gets the Railway URL.
