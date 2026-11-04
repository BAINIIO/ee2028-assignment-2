"""Read-only bench measurement of real OLED transfers, NFC state and USB frames."""
import argparse
import json
from pathlib import Path
import subprocess
import time

from live_display_transport import LiveDisplayTransport


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--serial', required=True)
    parser.add_argument('--seconds', type=float, default=6)
    parser.add_argument('--output', type=Path, default=Path('build/run/display-nfc-verification.json'))
    args = parser.parse_args()
    if not 1 <= args.seconds <= 30:
        parser.error('Measurement must be between 1 and 30 seconds')
    table = subprocess.check_output(['sh', 'tools/arm-tool.sh', 'objdump', '-t',
                                     'build/firmware/zombie_game.elf'], text=True)
    names = {'uwTick', 'oled_frames_completed', 'oled_last_frame_ms', 'oled_last_transfer_ms',
             'oled_longest_transfer_ms', 'oled_failures', 'oled_recoveries', 'oled_online',
             'online', 'ready', 'errors', 'image_size', 'image', 'base_url', 'step'}
    symbols = {}
    for line in table.splitlines():
        fields = line.split()
        if len(fields) >= 6 and fields[-1] in names:
            symbols[fields[-1]] = (int(fields[0], 16), int(fields[-2], 16))
    if set(symbols) != names:
        raise RuntimeError('Missing bench symbols: ' + ', '.join(names - set(symbols)))
    transport = LiveDisplayTransport(serial=args.serial)

    def read(name):
        address, size = symbols[name]
        values = transport._rpc(f'join [read_memory 0x{address:x} 8 {size}]').split()
        return bytes(int(value, 0) for value in values)

    def value(name):
        return int.from_bytes(read(name), 'little')

    try:
        transport.connect()
        before = {name: value(name) for name in ('uwTick', 'oled_frames_completed', 'oled_failures')}
        start = time.monotonic()
        deadline = start
        frames = 0
        while time.monotonic() - start < args.seconds:
            if transport.read_frame() is not None:
                frames += 1
            deadline += 1 / 30
            time.sleep(max(0, deadline - time.monotonic()))
        elapsed = time.monotonic() - start
        after = {name: value(name) for name in ('uwTick', 'oled_frames_completed', 'oled_failures')}
        measured_ms = (after['uwTick'] - before['uwTick']) & 0xffffffff
        size = value('image_size')
        ndef = read('image')[:size]
        url = ('https://' if len(ndef) > 11 and ndef[10] == 4 else '') + ndef[11:-1].decode('ascii')
        report = {
            'method': 'verified ELF/BIN; read-only SWD without halt, reset or injected inputs',
            'physical_oled': {
                'online': bool(value('oled_online')),
                'completed_frames': after['oled_frames_completed'] - before['oled_frames_completed'],
                'measurement_ms': measured_ms,
                'frames_per_second': round(1000 * (after['oled_frames_completed'] - before['oled_frames_completed']) / measured_ms, 2),
                'failures_before': before['oled_failures'], 'failures_after': after['oled_failures'],
                'recoveries': value('oled_recoveries'),
                'last_transfer_ms': value('oled_last_transfer_ms'),
                'longest_transfer_ms': value('oled_longest_transfer_ms'),
            },
            'usb_capture': {'frames': frames, 'seconds': round(elapsed, 3),
                            'frames_per_second': round(frames / elapsed, 2)},
            'nfc': {'online': bool(value('online')), 'ready': bool(value('ready')),
                    'errors': value('errors'), 'image_size': size,
                    'verified_ndef_url': url, 'ndef_hex': ndef.hex()},
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
    finally:
        transport.close()


if __name__ == '__main__':
    main()
