# Ledify boilerplate: Bluetooth (Nordic UART service) -> addressable LEDs.
# Connect with any BLE UART app (nRF Connect, Bluefruit Connect) and send:
#   on | off | rgb R G B | volt 5 | volt 12
# Runs on MicroPython for ESP32-C6. On a laptop, `python3 main.py` runs the
# parser self-check instead.

try:
    import bluetooth
    import neopixel
    from machine import Pin
except ImportError:
    bluetooth = None

NUM_LEDS = 30  # length of your strip
MAX_LEVEL = 128  # brightness cap (0-255), lower it if the supply browns out

PIN_LED_DATA = 5  # LED_CON, the SIG pad
PIN_LED_ENABLE = 3  # LED_ENABLE, high = LED buck on
PIN_LED_12V = 2  # LED_SW_V, low = 5 V, high = 12 V


def parse(line):
    """'rgb 255 0 10' -> ('rgb', (255, 0, 10)). Returns None if invalid."""
    try:
        if isinstance(line, bytes):
            line = line.decode()
        parts = line.strip().lower().split()
        cmd, nums = parts[0], tuple(int(p) for p in parts[1:])
    except (ValueError, IndexError):  # UnicodeError is a ValueError
        return None
    if cmd in ("on", "off") and not nums:
        return cmd, nums
    if cmd == "rgb" and len(nums) == 3 and all(0 <= n <= 255 for n in nums):
        return cmd, nums
    if cmd == "volt" and nums in ((5,), (12,)):
        return cmd, nums
    return None


def run():
    enable = Pin(PIN_LED_ENABLE, Pin.OUT, value=0)
    sel_12v = Pin(PIN_LED_12V, Pin.OUT, value=0)
    strip = neopixel.NeoPixel(Pin(PIN_LED_DATA), NUM_LEDS)

    def fill(rgb):
        strip.fill(tuple(c * MAX_LEVEL // 255 for c in rgb))
        strip.write()

    def handle(line):
        parsed = parse(line)
        if parsed is None:
            return "err"
        cmd, nums = parsed
        if cmd == "on":
            enable.value(1)
        elif cmd == "off":
            fill((0, 0, 0))
            enable.value(0)
        elif cmd == "rgb":
            fill(nums)
        elif cmd == "volt":
            # never change the rail while the strip is powered, send "on" after
            enable.value(0)
            sel_12v.value(nums[0] == 12)
        return "ok"

    uart = bluetooth.UUID("6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    tx = (bluetooth.UUID("6E400003-B5A3-F393-E0A9-E50E24DCCA9E"), bluetooth.FLAG_NOTIFY)
    rx = (
        bluetooth.UUID("6E400002-B5A3-F393-E0A9-E50E24DCCA9E"),
        bluetooth.FLAG_WRITE | bluetooth.FLAG_WRITE_NO_RESPONSE,
    )
    name = b"Ledify"
    adv = bytes((2, 0x01, 0x06, len(name) + 1, 0x09)) + name

    ble = bluetooth.BLE()
    ble.active(True)
    ((tx_handle, rx_handle),) = ble.gatts_register_services(((uart, (tx, rx)),))
    connections = set()

    def irq(event, data):
        if event == 1:  # central connected
            connections.add(data[0])
        elif event == 2:  # central disconnected
            connections.discard(data[0])
            ble.gap_advertise(100_000, adv_data=adv)
        elif event == 3 and data[1] == rx_handle:  # write to RX
            reply = handle(ble.gatts_read(rx_handle))
            for conn in connections:
                ble.gatts_notify(conn, tx_handle, reply)

    ble.irq(irq)
    ble.gap_advertise(100_000, adv_data=adv)


def demo():
    assert parse("rgb 255 0 10") == ("rgb", (255, 0, 10))
    assert parse(b"ON\n") == ("on", ())
    assert parse("volt 12") == ("volt", (12,))
    assert parse("volt 9") is None
    assert parse("rgb 256 0 0") is None
    assert parse("rgb a b c") is None
    assert parse("on 1") is None
    assert parse("") is None
    assert parse(b"\xff\xfe") is None
    print("parser ok")


if bluetooth:
    run()
else:
    demo()
