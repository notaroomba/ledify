# Ledify firmware

ESP-IDF v6.1 project for the ESP32-C6 on the Ledify board.

| File | Does |
| --- | --- |
| `main/main.c` | Startup, the command handler, the frame loop that composes colour × brightness × overlay, USB serial console |
| `main/pd.c` | USB-PD sink on the FUSB302B: requests the most powerful fixed offer up to 20 V, renegotiates for 15 V+ when the 12 V output is selected |
| `main/leds.c` | Addressable strip over RMT (WS2812 family, WS2811, SK6812, WS2816), the colour sources, the power cap, buck enable, 5 V / 12 V select |
| `main/sensors.c` | OPT4001 light sensor (I2C), BMI323 IMU and BMP581 barometer (SPI) |
| `main/mic.c` | ICS-43434 microphone over I2S in its own task: loudness with auto gain, beat onsets, bass/mid/treble and a 16-band spectrum |
| `main/beat.c` | Tempo tracking and beat prediction: tempo by autocorrelation of the onset strength, phase by a comb, kept in step by each onset (`test/beat_test.c` checks it on the host) |
| `main/fx.c` | The whole-strip sound styles (`eq`, `ripple`, `mirror`, `chroma`, `flow`) and motion styles (`bubble`, `tilt`, `shake`, `spin`, `slosh`, `height`), plain C (`test/fx_test.c` checks them on the host) |
| `main/fusion.c` | IMU calibration, sensor fusion and barometric height for the motion styles, plain C (`test/fusion_test.c` checks it on the host) |
| `main/motion.c` | Reads the BMI323 at 200 Hz and the BMP581 at 20 Hz in its own task and runs the fusion |
| `main/fft.c` | 256-point fixed-point FFT and the band sums, plain C (`test/fft_test.c` checks it on the host) |
| `main/ble.c` | Nordic UART service on NimBLE |
| `main/net.c` | Wi-Fi station (primary + fallback network), MQTT client, Home Assistant discovery |
| `main/ota.c` | Over-the-air update: fetches an app image over Wi-Fi into the spare slot |
| `main/board.h` | Pin map and tunables (pixel limits, frame rate, power budget, PD voltage, LED power settle time) |
| `partitions.csv` | 16 MB flash: 24 KB NVS for settings, two 4 MB app slots for OTA |
| `creds.example.csv` | Template for the NVS settings image (Wi-Fi, broker, strip, lighting tunables) |
| `creds.py` | `pull` a board's settings into the local `creds.csv`, `push` them onto another board |

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
| `blink [N]` | Flash the strip blue N times (default 10, 0 stops), two flashes a second at the current `level`, then back to what it showed. Turns the output on for the blink if it was off, and off again afterwards. Never changes the 5 V / 12 V selection |
| `rgb R G B` | Fill the strip, 0-255 each |
| `level N` | Brightness cap, 0-255 |
| `count N` | How many pixels are driven, 1-1000 (`MAX_LEDS`). Saved. Also a slider in Home Assistant |
| `type ws2812` | Chip type: `ws2812` (also B/C/D/E, WS2813, WS2815), `ws2811`, `sk6812`, `sk6812w` (RGBW), `ws2816`. Saved |
| `mode NAME` | Preset, the list Home Assistant shows as effects: `solid`, `rainbow`, `temp`, `spectrum`, `meter`, `eq`, `ripple`, `mirror`, `chroma`, `flow`, `bubble`, `tilt`, `shake`, `spin`, `slosh`, `height`, `sound`, `vu`, `pulse`. A colour resets the brightness source and overlay, `vu` brings its own meter colours, `sound` and `pulse` keep the colour. `mode auto` still works as `auto on` |
| `color NAME` | Colour source on its own: `solid`, `rainbow`, `temp`, `spectrum`, `meter`, `eq`, `ripple`, `mirror`, `chroma`, `flow`, `bubble`, `tilt`, `shake`, `spin`, `slosh`, `height` (see [Mixing styles](#mixing-styles)) |
| `bright level` / `bright sound` | Brightness source: the fixed `level`, or `level` scaled by the microphone |
| `overlay none` / `vu` / `pulse` | Light only as many pixels as the sound level, or flash on beats |
| `auto on` / `auto off` | Scale the brightness with the light sensor between `lux_lo` and `lux_hi`, off under `night` |
| `gain N` | Microphone gain 1-100, default 25: how much a quiet room is amplified. Saved |
| `predict 0\|1` | 1 (default): once the music has a steady tempo, beats are lit on time from the tempo tracker, early by the time the LEDs take, instead of after each one is heard. 0: every beat is a heard onset. Saved; `status` shows `bpm` and `lock` |
| `react N` | How fast the sound styles fall back after a sound, 1-100, default 60: the release runs from about 1 s at 1 to 40 ms at 100; the rise is always instant. Saved |
| `lux LO HI` | Lux at which auto brightness sits at its floor and reaches full, default `0 500`. Saved; also one at a time as `lux_lo N` / `lux_hi N` |
| `night N` | Lux below which auto brightness turns the strip off, 0 (default) disables. Saved |
| `temp LO HI` | °C shown as blue and as red by the `temp` colour, white halfway, default `15 30`. Saved; also `temp_lo N` / `temp_hi N` |
| `temp 21.5` | Feed a temperature in; the `temp` colour uses it instead of the barometer until the next restart |
| `volt 5` / `volt 12 yes` | Select the output voltage. 12 V must be confirmed with `yes` because it destroys 5 V pixels; it needs a charger offering 15 V or more (the board renegotiates for it) and the output turns off first |
| `status` | JSON with power state, strip settings, colour/brightness/overlay, the saved tunables, the brightness cap, PD contract, network state, board time, `sleep`/`rise` seconds left, `hot` (percent removed by the thermal limit), the schedule, zones, an active LAN stream, lux, temperature, pressure, accel, gyro, sound level and loudness after gain |
| `pd` | The source's offered voltages and the current contract |
| `net` | Wi-Fi and MQTT state |
| `wifi SSID PASSWORD` | Save Wi-Fi credentials and restart |
| `wifi2 SSID PASSWORD` | Add or update a fallback network and restart, up to 5 (`wifi2 SSID -` for an open network). The board tries the primary first, then each fallback once in turn, and loops back to the primary after the last one until one connects. If a connected network drops, it moves on to the next one the same way |
| `wifi2 list` / `wifi2 del SSID\|N` / `wifi2 off` | Show the fallbacks (names only), remove one and restart, or remove them all and restart |
| `wifi2 try SSID\|N` | Join that fallback now and stay on it until the next boot, saves nothing |
| `mqtt URL [USER PASSWORD]` | Save the broker and restart, e.g. `mqtt mqtt://host:1883 ledify secret` |
| `pddelay N` | Milliseconds after boot before the board starts talking PD, 0-30000 (saved); a diagnostic for chargers that cut power during the handshake |
| `pdmax N` | Highest PD voltage to request, 5000-20000 mV (saved). `status` also reports `boots`, the reset reason and a log of the last four boots with what each charger offered; two boots in a row that die early make the next one stay at 5 V |
| `limit 0\|1` | 1 dims to the power budget, 0 (default) sends what was asked and only reports the estimate. Saved |
| `droop N` | Tail colour fix, 0-100 (saved, default 0). A strip fed from one end sags towards the far end, where blue and green dim before red, so white turns warm along the strip. `droop` cuts red (and half as much green) progressively along the strip, scaled by how much of a full-white load the frame draws; set it so the far end looks white at your usual brightness. The real fix is feeding 5 V (and ground) into the far end of the strip as well |
| `ota URL` | Download a new app image over Wi-Fi and restart into it (see below) |
| `palette hex[@pct],...` | Up to 16 colour stops as a gradient along the strip, each with an optional position in percent (`ff8800@0,2244ff@35,ffffff@100`; without positions they are spread evenly, and the stops are sorted by position). Also selects the `palette` colour source; `status` reports the stops as `palette`. Saved. The panel's gradient editor uses this |
| `stripe N` / `stripe N,N,...` | Show the gradient as stripes instead of a blend: each distinct stop colour in turn, N pixels each, or one length per colour (`stripe 4,2,2`), repeating along the strip; `stripe 0` blends again. Saved |
| `scroll N` | The gradient or its stripes travel along the strip at N pixels per second (negative = the other way, 0 = still), so a ring turns clockwise or counterclockwise. Saved |
| `cover hex[@pct],...` | The same, into a second gradient shown by the `cover` colour source, so the album-cover colours the panel sends from Spotify never replace your own gradient; `status` reports it as `cover` |
| `seg N START END [color [rrggbb]]` | Zone N (1-6) covers pixels START to END-1 with its own colour source (`solid`, `rainbow`, `temp`, `spectrum`, `meter`, `palette`) and solid colour; pixels outside every zone keep the strip's. `seg` lists them, `seg del N` and `seg off` remove. Saved |
| `sleep N` | Fade out over N minutes, then off; `sleep 0` cancels. `status` shows the seconds left as `sleep` |
| `rise N` | Sunrise: on, and from dark up to the set level over N minutes (`rise` in `status`) |
| `tz RULE` | Timezone as a POSIX rule, e.g. `EST5EDT,M3.2.0,M11.1.0` (the panel has a list). Saved; the clock itself comes from the internet once Wi-Fi is up |
| `sched add HH:MM CMD` | Run any command every day at that time on the board's clock, up to 8 entries; `sched` lists them, `sched del N` and `sched clear` remove. Saved |
| `fade MS` | Transition time: colour and brightness changes ease over about this long, default 300, 0 = instant. Saved. Sound-reactive styles and LAN streams are never eased |
| `tmax C` | Thermal limit: the strip dims over the last 10 °C before the board reaches this temperature and is dark at it (default 70, 0 = off). `status` reports the percent removed as `hot`. Saved |
| `name HOST` | mDNS hostname for the local panel, `http://HOST.local` (default `ledify`). Saved |
| `otabase URL` | Only accept `ota` images whose URL starts with this (`otabase any` clears it), so a leaked broker login cannot flash other firmware. Saved |
| `live N` | Stream loudness, bands, pulse, dB, the brightness cap, the 16-band spectrum (`spec`) and the estimated sound-to-light delay in ms (`lat`), the tempo (`bpm`) and how sure the tracker is (`lock`, 0-100) to `ledify/<id>/live` at 10 Hz for N seconds (the web panel renews this) |
| `mic [N]` | Print the microphone level on the USB console 4 times a second for N seconds (default 10, 0 stops): the loudest 8 ms block as dBFS and approximate dB SPL, the raw 24-bit min/max/mean, and a bar. A quiet room should sit around -60 to -70 dBFS and a clap should jump 30 dB or more. All zeros or a fixed value means no data from the mic |
| `motion [N\|cal\|zero\|hzero]` | The IMU: alone, a JSON snapshot (calibration state, tilt, heading, energy, gyro bias, height); `motion N` prints it on the USB console 4 times a second for N seconds; `motion cal` calibrates again (hold the board still for a second); `motion zero` makes the way the board sits now count as level and resets the heading; `motion hzero` makes the current height 0 |
| `hstep CM` | The `height` style's colour step, 1-500 cm, default 10. Saved |
| `axis N` | Which board axis the strip runs along, for the motion styles: 0 +x, 1 +y, 2 +z, 3 -x, 4 -y, 5 -z, default 0. Saved |
| `micraw` | Diagnostics on the live topic: the current block's samples and the loudest block of the last minute |
| `audio L B M T P [SPEC]` | Feed loudness, bass, mid, treble and pulse (0-255) from outside, and optionally the 16-band spectrum as 32 hex digits; overrides the microphone for 1 s per packet. Without SPEC the three bands are spread over the spectrum |
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
| `eq` | 16-band spectrum: one zone per band, bass (red) at the start to treble (pink) at the end, each zone as bright as its band |
| `ripple` | Each beat sends a ring outward from the centre, coloured by where the sound sits (bass red, mid green, treble blue), over a faint glow of the `rgb` colour |
| `mirror` | Level meter growing from the centre both ways, green to red, with a white peak dot that holds 0.4 s and then falls |
| `chroma` | The whole strip in the colour of the sound: bass red, mid green, treble blue |
| `flow` | A waterfall: the sound's colour and loudness enter at the start and scroll along the strip in about 2 s |
| `bubble` | A spirit level: a bubble glides to the raised end of the strip (all the way at about 30°), green when level and warming to red towards the ends, over a faint glow of the `rgb` colour |
| `tilt` | The whole strip takes the hue of the direction the board leans (all the way at about 30°), the `rgb` colour when level; the lower end glows brighter |
| `shake` | Sparks while the board moves, more the harder it is shaken, in hues that turn with the heading; a knock flashes the whole strip |
| `spin` | A rainbow that stays put in the room: turn the board about the vertical and the colours slide along the strip, one turn for one rainbow; brighter the faster it turns |
| `slosh` | Half the strip is liquid in the `rgb` colour: it runs to the lower end and sloshes (about 1 Hz, settling after a few rocks) when the board is pushed along the strip, frothing white at its edges |
| `height` | Raise or lower the board and the whole strip changes colour every `hstep` cm (default 10) from where the style was switched on (or `motion hzero`): the `rgb` colour at 0, an eighth of the way round the colour wheel per step. Hysteresis of a fifth of a step (at least 3 cm) keeps it from flickering on a line; a faint dot runs along the strip as the board climbs through each step |

The last eleven draw the whole strip at once and keep state between frames, so they don't go in a `seg` zone (zones on top of them still work). The sound and motion styles redraw at about 60 fps.

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

### Motion processing

The BMI323 runs at 200 Hz with its filter at 50 Hz, and `motion.c` reads it every 5 ms in its own task (the only task that talks to it; `status` gets the latest sample from there). `fusion.c` does the rest:

- **Calibration at startup.** The first second the board holds still (gyro scattering under 0.8 °/s and the accelerometer's magnitude under 0.008 g, as standard deviations over the second), the gyro's mean becomes its bias and that pose becomes "level", heading 0. If it never holds still it starts after 6 s with no bias and calibrates at the first rest. The console says `calibrated: gyro bias ...` when it is done; `motion cal` does it again.
- **Fusion.** The up vector, in board coordinates, is turned by the bias-corrected gyro every sample (immediate and smooth, but it drifts) and pulled towards the accelerometer with a 0.5 s time constant (noisy, but never drifts): a complementary filter. The pull fades to nothing as the measured acceleration strays 0.2 g from 1 g, so shaking the board doesn't read as tilting it. Heading is the gyro's rate about that up vector, integrated, so it works however the board is mounted; with no magnetometer it can creep, so the bias is re-learnt every time the board rests for a second, and the heading is frozen while it rests.
- **What the styles get.** Tilt along and across the strip (sine of the angle, against the zero pose), heading and turn rate, gravity-free acceleration, a motion energy (20 ms attack, 300 ms release) and a count of knocks (over 1.2 g).
- **Smoothing in the styles.** The bubble rides a critically damped spring (no jitter, no overshoot), the liquid a lightly damped one (so it sloshes), the tilt colour a 80 ms low-pass, and shake's sparks fade with the same `react` release as the sound styles.

- **Height.** The BMP581 now converts at 20 Hz with 32x pressure oversampling and is read by the same task. Its altitude has no drift but a few cm of noise and some lag; the accelerometer's vertical part (along the fused up vector, minus 1 g), integrated twice, follows a lift at once but drifts within seconds. A third-order complementary filter, crossing over at 2 rad/s while the board moves (the accelerometer's vertical offset shifts with how the board is held, about 0.03 g on the board, and a slower crossover turned that into 11 cm of overshoot) and 0.8 rad/s once it has stopped, takes the slow part from the barometer and the fast part from the accelerometer and learns the accelerometer's vertical offset; each still second also measures that offset directly and stops any vertical creep. Height is relative: after boot the first second of barometer readings is skipped (the sensor is still settling into its new configuration) and the next half second averaged is height 0; it is zeroed again when the board first calibrates, and every time the `height` style is picked. After 2 s at rest the height is taken from a 2 s barometer average; once the IMU has seen no movement for 4 s (judged on the change in acceleration and on turning, each smoothed over 50 ms so sensor noise or a tap on the desk doesn't count), height is held and height 0 follows the pressure instead, since any change then is the air or the sensor and not a climb: on the board the pressure crept about 1.5 Pa a minute (13 cm/min) after a restart while it sat still, and this cancels it. When the hold breaks (a button pressed, a knock) but the accelerometer alone never saw the board move faster than 5 cm/s, the height goes back to what it was once the board rests again, whatever the barometer did meanwhile; so a lift slower than about 5 cm/s isn't counted. Limits: while the board is being carried around the creep still adds up, and the barometer reads the room's air, so a door slamming, an air conditioner or the weather can move it by several Pa (tens of cm); re-zero with `motion hzero` (Zero height here in the panel) if it wanders, and expect steps under ~5 cm to be noisy.

In the host simulation (`test/fusion_test.c`, BMI323 noise and a 0.8 °/s bias) the bias comes out within 0.01 °/s, a 20° tilt reads within 0.02 of sin 20° and never moves more than 0.002 in one sample, a 0.8 g shake along the strip reads as at most 0.03 of false tilt, and a 90° turn reads 90° with no creep afterwards. With 0.5 Pa of barometer noise and an accelerometer reading 0.02 g high, a 30 cm lift in 0.6 s is tracked within 3.4 cm while moving and 2.5 cm while held.

### Sound processing

A microphone task reads 256 samples (8 ms) at a time at 32 kHz, so the main loop never waits for audio. Per block: RMS × `gain`, held under a peak follower with a 2 s decay, gives the loudness, so a quiet room stays dark at a given gain while a loud room isn't pinned at 100 %. A beat is a block more than 1.5× its 1 s running average, at most one per 100 ms. Every second block (16 ms, half-overlapping windows) the last 16 ms, decimated to 16 kHz, go through a Hann window and the fixed-point FFT in `fft.c` (62.5 Hz bins: bass 1-3, mid 4-31, treble 32-127, plus 16 log-spaced bands from 62.5 Hz to 8 kHz), each band normalised against its own peak but never below 1/8 of the loudest band's peak or of full scale, so mid and treble move at room levels while a silent room stays dark. `status` shows `sound` (raw RMS, 0-1) and `loud` (0-255 after gain) for tuning `gain`. Sound styles redraw every `SOUND_FRAME_MS` (16 ms, about 60 fps; longer strips as fast as the wire allows, 30 µs per pixel), other moving styles every `FRAME_MS` (33 ms), still ones once a second. A new beat is drawn at once rather than at the next 16 ms tick, so a heard beat reaches the LEDs about 10 ms after the hit (13-16 ms at worst with 120 pixels): the 8 ms block plus the wire time.

**Beat prediction.** Every 16 ms frame also gives an onset strength: how much each of the 16 bands rose since the last frame, on a log scale, with the kick bands counted three times. `beat.c` keeps 4 s of it, finds the tempo by autocorrelation every 0.5 s (60-187 BPM, leaning towards 120 so it picks the beat rather than the bar), and the phase with a comb of beats at that period. Each heard onset then nudges the phase and a little of the tempo, like a phase-locked loop. Once the history is periodic enough and 6 of the last 8 predicted beats had an onset under them, beats are fired from the prediction, early by the render and wire time, so the LEDs light on the hit (in the simulator within about ±10 ms, slightly early on average) and keep the rhythm through a missing kick. Three empty beats in a row, or the periodicity fading, hands it back to the heard onsets within a couple of seconds, so speech and silence don't get phantom beats. A fast song can lock at half tempo (lighting every other beat). `predict 0` turns it off. The log line every 3 s prints the board's own estimate as `sound to light ~N ms`, and `live` sends it as `lat`.

`web/public/sim.html` (the hosted panel's `/sim.html`) runs this whole pipeline in the browser, line for line, on a microphone simulated from the ICS-43434 datasheet, with test signals, an audio file or the device's mic, so styles and settings can be tried without a board.

## Wiring the strip

The three pads are VCC, SIG and GND. SIG goes to the strip's **DIN** (data in; the arrows printed on the strip point away from the board). Each pixel regenerates the signal on its DOUT for the next one, so a strip's DOUT end is only for chaining another strip. WS2815 also has a backup input BI; tie the first pixel's BI to GND.

SIG is 3.3 V logic. 5 V chips specify a high level of 0.7 × VDD (3.5 V), which normally works over a short lead but is out of spec, so if a strip stays dark while VCC is present either put a level shifter (74AHCT125) on SIG or feed the first pixel's VCC through a diode so it sits at about 4.3 V. The board waits `LED_POWER_SETTLE_MS` after enabling the buck before sending the first frame and resends a still frame once a second, so pixels that powered up late still catch it.

## Power budget

A 5 V pixel pulls about 60 mA at full white, so 300 of them want 18 A and a laptop port gives 3 A. The firmware estimates every frame's draw and reports it (`need_mw`, `budget_mw` and `cap` in `status`); with `limit 1` it also scales brightness so the frame fits, otherwise (the default) it sends what was asked and the panel shows a warning. The estimate works the same way [FastLED's power model](https://fastled.io/docs/dc/d4d/struct_power_model_r_g_b.html) and [WLED's automatic brightness limiter](https://wled.discourse.group/t/automatic-brightness-limiter-confusion/10821) do: every frame's draw is estimated from each pixel's colour (`LED_RED_MW` 80, `LED_GREEN_MW` 55, `LED_BLUE_MW` 75 at full, FastLED's 16/11/15 mA WS2812 figures at 5 V) plus `LED_DARK_MW` 5 per pixel for the idle current, and the brightness is scaled so the total fits the budget. The budget is the PD contract minus buck losses (`LED_BUCK_EFF_PCT`), capped at the buck's 10 A, or 7.5 W when there is no contract. `status` reports the resulting `cap` next to the requested `level`; on a 15 W port 300 red pixels run at about 108/255 and 300 white ones at about 36/255, on a 100 W charger white gets about 240. Like WLED's limiter this is arithmetic, not a measurement: [measured draw varies by batch](https://pjrc.com/how-much-current-do-ws2812-neopixel-leds-really-use), so adjust the per-channel numbers if a meter disagrees, and it cannot see voltage drop along the strip, so inject power every 100 pixels or so on long 5 V runs.

## Timers, zones, local control

**Transitions.** Brightness changes ease in, and so does a change from one solid colour to another: the frame on the
strip moves towards the new one by a share of the remaining distance each redraw, so a change settles in about
`fade` ms whatever the frame rate, and `off` fades to dark before the rail is cut. Gradients, stripes, zones and the
moving styles switch at once (a cross-fade between two stripe layouts shows muddled in-between colours), and
sound-reactive styles and LAN streams always draw instantly. The BOOT button on the board toggles the
output with a short press.

**Across a restart.** An update or a saved network setting restarts the board; what the strip showed (power, colour, effect, brightness) is kept in RTC memory and restored once the supply is negotiated. A power cut still starts dark.

**Sleep and sunrise.** `sleep N` scales the brightness linearly to zero over N minutes and turns the output off;
`rise N` turns it on and scales from zero up to the set level. Both show their remaining seconds in `status`.

**Schedules.** `sched add 06:30 rise 20` runs that command every day at 06:30 on the board's clock, which comes
from `pool.ntp.org` once Wi-Fi is up and uses the `tz` rule. Any command works, so "warm and dim in the evening" is
two entries: `sched add 21:00 rgb 255 170 100` and `sched add 21:00 level 90`.

**Zones.** `seg 1 0 100 solid ff0000` and `seg 2 100 300 rainbow` give the first hundred pixels a red and the rest
a rainbow; brightness, overlays and the power cap stay global. The panel draws the zones as a bar.

**Thermal limit.** The barometer on the board sits next to the buck; above `tmax - 10` °C the brightness drops
linearly, at `tmax` the strip is dark, and the panel says so.

**Local panel, no cloud.** With Wi-Fi up the board announces itself over mDNS and serves the same panel at
`http://ledify.local/` (the `name` setting) from its own flash; that page talks to the board over a WebSocket on
the LAN, so it works with the internet down. The hosted panel shows the link. The WebSocket carries the MQTT
payloads as text frames (`state {...}`, `live {...}`, `reply ok`, no binary frames) and takes
command lines back. Only the board's own page may open it: the `Host` must be `NAME.local`, `NAME` or the board's IP
and a browser's `Origin` must match, so another website open on the same network can't drive the board or listen
to the mic.

**Microphone audio.** Raw audio never leaves the board: there is no command, topic or WebSocket frame that
carries it. The mic is only analysed on the board, and what goes out (the `live` topic, the status JSON) is levels:
loudness, three bands, a 16-band spectrum at 10 Hz, dB, the beat and BPM. That is too coarse to rebuild speech from.

**Pixel streams.** The board listens for DDP on UDP 4048 and E1.31/sACN on UDP 5568 (170 pixels per universe,
universes 1 and up, unicast or the 239.255.0.U multicast groups). While frames arrive they are shown as they come,
through the power cap; `STREAM_TIMEOUT_MS` after the last one the normal styles resume. In LedFx add a device of type
DDP (or E1.31) with the board's IP and the pixel count; xLights and Hyperion use the same two protocols.

**Setup over Bluetooth.** The panel's Setup button (Chrome or Edge on a computer or Android) sends the Wi-Fi and
broker settings to a new board over the Nordic UART service, so the USB console is only needed for the first flash.
A saved network setting restarts the board two seconds later so several can be sent in a row.

**Accounts, board links and sharing.** The hosted panel's server (`web/server/index.mjs`) gives every board and
every browser its own broker account through Mosquitto's dynamic security plugin, so no shared password reaches a
browser. Anyone can open the panel, but a browser only sees the boards it has been linked to:

- Hold a board's button for 3 s (three blue flashes). For 2 minutes its *setup window* is open: it sends a one-time
  code to `ledify/<id>/pair`, which only the panel server can read, and to its own page and Bluetooth. Its own page
  (`http://NAME.local`) then shows **Link this browser**, which opens the hosted panel and links it; or, from any
  network, the hosted panel's **Link over Bluetooth** (Chrome or Edge, not iPhone) reads the code over Bluetooth. The first link of
  a board still on the old shared login also gives it its own broker account (the server sends it `mqtt ...`).
- The owner logs in with `OWNER_PASS` ("remember me", 90 days), sees every board, and in **Access and sharing**
  makes single-use share links (24 h), removes browsers (their broker account is deleted, which drops them at once)
  or forgets a board.
- A browser remembers its boards with an HttpOnly cookie for a year.
- `wifi`, `wifi2`, `mqtt`, `otabase`, and `ota` while no `otabase` is saved, are refused unless they come from the USB
  console or arrive while the setup window is open. `pair` returns the code over USB, Bluetooth or the board's own
  page, never over MQTT.

Setting it up on Railway (accounts stay off, and everything works as before, until `OWNER_PASS` is set):

1. **mosquitto** service: add a volume at `/mosquitto/data`, variables `ADMIN_USER` and `ADMIN_PASS`, and this start
   command (it replaces the password file with the dynamic security plugin):

   ```bash
   sh -c '[ -f /mosquitto/data/dynsec.json ] || mosquitto_ctrl dynsec init /mosquitto/data/dynsec.json "$ADMIN_USER" "$ADMIN_PASS"; chown -R mosquitto:mosquitto /mosquitto/data; printf "listener 1883\nlistener 8080\nprotocol websockets\nallow_anonymous false\nplugin /usr/lib/mosquitto_dynamic_security.so\nplugin_opt_config_file /mosquitto/data/dynsec.json\npersistence false\nlog_dest stdout\n" > /mosquitto/config/mosquitto.conf && exec mosquitto -c /mosquitto/config/mosquitto.conf'
   ```

2. **panel** service: add a volume at `/data` (the board list, links and board passwords live there) and the
   variables `OWNER_PASS`, `BROKER_ADMIN_URL=mqtt://mosquitto.railway.internal:1883`, `BROKER_ADMIN_USER` and
   `BROKER_ADMIN_PASS` (the same as the mosquitto service's admin), `MQTT_WS_URL=wss://<broker domain>` and
   `BOARD_MQTT_URL=wss://<broker domain>:443/`. While boards move over, `LEGACY_MQTT_USER`/`LEGACY_MQTT_PASS` keep
   the old shared login working (it can no longer read setup codes); `HA_MQTT_USER`/`HA_MQTT_PASS` give Home Assistant
   its own account. Remove `MQTT_USER`, `MQTT_PASS` and `PANEL_PASS`.
3. Link each board once (button, then its own page), point Home Assistant at its own account, then delete the
   `LEGACY_*` variables and the broker's TCP proxy.

**Install and demo.** The hosted panel is a PWA: "Add to Home Screen" on a phone gives it an icon and a standalone
window. `https://.../?demo` runs it against a pretend board for trying the controls without hardware.

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

Settings live in the NVS partition (namespace `cfg`): `ssid`, `pass`, `ssid2`, `pass2`, `mqtt_url`, `mqtt_user`, `mqtt_pass`, `count`, `type`, and the tunables `gain`, `lux_lo`, `lux_hi`, `night`, `temp_lo`, `temp_hi`. Nothing secret is in the code. Set them with the `wifi` and `mqtt` commands, or build an NVS image from `creds.example.csv` (copy it outside the repo first) and flash it to `0x9000`; the comments in that file have the two commands. To copy one board's settings to another, `python creds.py pull` saves the plugged-in board's settings to `creds.csv` (gitignored, stays on this machine) and `python creds.py push` writes them to the next board. `mqtt://` and `mqtts://` (certificate bundle) URLs both work.

The board id is `ledify_` plus the last three bytes of its MAC, e.g. `ledify_d3ed08`.

| Topic | Direction | Payload |
| --- | --- | --- |
| `ledify/<id>/cmd` | in | Any text command from the table above |
| `ledify/<id>/light/set` | in | Home Assistant JSON light: `{"state":"ON","brightness":128,"color":{"r":255,"g":0,"b":0},"effect":"rainbow"}` |
| `ledify/<id>/KEY/set` | in | A bare value, run as the command `KEY value`. The Home Assistant entities use `count`, `type`, `auto` (`ON`/`OFF`), `gain`, `lux_lo`, `lux_hi`, `night`, `temp_lo`, `temp_hi`; `temp` feeds a temperature in for the `temp` colour |
| `ledify/<id>/audio/set` | in | `L B M T P` from the web panel's audio analysis (file or microphone) |
| `ledify/<id>/reply` | out | The text reply to the last command (`ok`, `err ...`), what the console and BLE see |
| `ledify/<id>/live` | out | `{"loud","bands","pulse","db","cap"}` at 10 Hz while a `live` lease is active |
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
