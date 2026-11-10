#!/usr/bin/env python
# Copy a board's saved settings (Wi-Fi, fallbacks, MQTT, strip, tunables) to and from creds.csv,
# which is gitignored and stays on this machine. Run inside the IDF environment:
#   python creds.py pull [PORT]   board -> creds.csv
#   python creds.py push [PORT]   creds.csv -> board (replaces its whole NVS, then it restarts)
import csv, json, os, subprocess, sys, tempfile

IDF = os.environ['IDF_PATH'] + '/components/nvs_flash/'
HERE = os.path.dirname(os.path.abspath(__file__))
CSV = os.path.join(HERE, 'creds.csv')
SKIP = {'boots', 'l0', 'l1', 'l2', 'l3'}   # per-board boot log, not settings
TYPES = {'uint8_t': 'u8', 'int8_t': 'i8', 'uint16_t': 'u16', 'int16_t': 'i16',
         'uint32_t': 'u32', 'int32_t': 'i32', 'uint64_t': 'u64', 'int64_t': 'i64', 'string': 'string'}


def esptool(port, *args):
    subprocess.run([sys.executable, '-m', 'esptool', '--chip', 'esp32c6', '-p', port, *args], check=True)


def pull(port, tmp):
    esptool(port, 'read-flash', '0x9000', '0x6000', tmp)
    out = subprocess.run([sys.executable, IDF + 'nvs_partition_tool/nvs_tool.py', '-d', 'minimal', '-f', 'json', tmp],
                         check=True, capture_output=True, text=True).stdout
    rows = [r for r in json.loads(out) if r['namespace'] == 'cfg' and r['key'] not in SKIP and r['encoding'] in TYPES]
    with open(CSV, 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['key', 'type', 'encoding', 'value'])
        w.writerow(['cfg', 'namespace', '', ''])
        for r in rows:
            w.writerow([r['key'], 'data', TYPES[r['encoding']], r['data']])
    os.chmod(CSV, 0o600)
    print(f'{len(rows)} settings saved to {CSV}: {", ".join(r["key"] for r in rows)}')


def push(port, tmp):
    subprocess.run([sys.executable, IDF + 'nvs_partition_generator/nvs_partition_gen.py', 'generate', CSV, tmp, '0x6000'],
                   check=True, stdout=subprocess.DEVNULL)
    esptool(port, 'write-flash', '0x9000', tmp)


if __name__ == '__main__':
    if len(sys.argv) < 2 or sys.argv[1] not in ('pull', 'push'):
        sys.exit(__doc__ or 'usage: creds.py pull|push [PORT]')
    port = sys.argv[2] if len(sys.argv) > 2 else '/dev/cu.usbmodem101'
    with tempfile.TemporaryDirectory() as d:
        (pull if sys.argv[1] == 'pull' else push)(port, os.path.join(d, 'nvs.bin'))
