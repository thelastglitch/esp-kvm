#!/usr/bin/env python3
"""End-to-end hardware test for ESP-KVM, Linux only.

Run it on a machine that has BOTH board ports plugged in (it plays computer A
and computer B at once). It sends commands over the UART port and checks the
input events that the kernel reports for the board's HID device.

Every ESP-KVM evdev node is grabbed with EVIOCGRAB first, so nothing the test
types or clicks reaches the desktop. Needs read/write access to those nodes
(tools/setup-linux.sh gives it to the plugdev group).
"""
import argparse
import fcntl
import glob
import os
import select
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from espkvm import EspKvm, KvmError, crc16  # noqa: E402

VID, PID = '303a', '4b56'
EVIOCGRAB = 0x40044590
EV_SYN, EV_KEY, EV_REL, EV_ABS, EV_LED = 0, 1, 2, 3, 0x11
REL_X, REL_Y, REL_HWHEEL, REL_WHEEL = 0, 1, 6, 8
ABS_X, ABS_Y = 0, 1
LED_NUM, LED_CAPS = 0, 1
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_EXTRA = 272, 273, 274, 275, 276
EVENT = struct.Struct('llHHi')

# PiKVM key name -> Linux evdev key code (as mapped by the kernel's hid-input).
KEY_SAMPLES = {
    'KeyA': 30, 'KeyZ': 44, 'KeyM': 50, 'Digit1': 2, 'Digit0': 11, 'Enter': 28, 'Escape': 1,
    'Backspace': 14, 'Tab': 15, 'Space': 57, 'Minus': 12, 'Backquote': 41, 'Slash': 53,
    'F1': 59, 'F12': 88, 'F13': 183, 'F24': 194, 'PrintScreen': 99, 'Insert': 110,
    'Delete': 111, 'Home': 102, 'PageDown': 109, 'ArrowUp': 103, 'ArrowLeft': 105,
    'ControlLeft': 29, 'ShiftLeft': 42, 'AltLeft': 56, 'MetaLeft': 125, 'ControlRight': 97,
    'ShiftRight': 54, 'AltRight': 100, 'MetaRight': 126, 'Pause': 119, 'ScrollLock': 70,
    'NumLock': 69, 'ContextMenu': 127, 'NumpadEnter': 96, 'Numpad5': 76, 'NumpadDecimal': 83,
    'IntlBackslash': 86, 'AudioVolumeMute': 113, 'AudioVolumeUp': 115, 'AudioVolumeDown': 114,
}


class Node:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDWR | os.O_NONBLOCK)
        fcntl.ioctl(self.fd, EVIOCGRAB, 1)

    def drain(self):
        while self.read(0.0):
            pass

    def read(self, timeout):
        """Return a list of (type, code, value) up to the next SYN, or []."""
        out, deadline = [], time.monotonic() + timeout
        while True:
            left = max(0.0, deadline - time.monotonic())
            if not select.select([self.fd], [], [], left)[0]:
                return out
            try:
                data = os.read(self.fd, EVENT.size * 64)
            except BlockingIOError:
                continue
            for i in range(0, len(data), EVENT.size):
                _, _, t, c, v = EVENT.unpack_from(data, i)
                if t != EV_SYN and not (t == 4):  # skip SYN and MSC_SCAN
                    out.append((t, c, v))
            if out and timeout == 0.0:
                return out
            if out:
                deadline = min(deadline, time.monotonic() + 0.02)

    def events(self, timeout=0.3):
        evs, deadline = [], time.monotonic() + timeout
        while time.monotonic() < deadline:
            evs += self.read(min(0.05, max(0.0, deadline - time.monotonic())))
        return evs

    def set_led(self, led, on):
        os.write(self.fd, EVENT.pack(0, 0, EV_LED, led, int(on)) + EVENT.pack(0, 0, EV_SYN, 0, 0))

    def close(self):
        fcntl.ioctl(self.fd, EVIOCGRAB, 0)
        os.close(self.fd)


def find_nodes():
    nodes = {}
    for d in glob.glob('/sys/class/input/event*'):
        try:
            ids = [open(f'{d}/device/id/{k}').read().strip() for k in ('vendor', 'product')]
        except OSError:
            continue
        if ids != [VID, PID]:
            continue
        caps = {k: int(open(f'{d}/device/capabilities/{k}').read().split()[-1], 16) for k in ('abs', 'rel')}
        kind = 'abs' if caps['abs'] & 3 else ('rel' if caps['rel'] & 3 else 'kbd')
        nodes[kind] = '/dev/input/' + os.path.basename(d)
    return nodes


class Runner:
    def __init__(self):
        self.passed = self.failed = 0

    def check(self, name, ok, detail=''):
        if ok:
            self.passed += 1
            print(f'  PASS  {name}')
        else:
            self.failed += 1
            print(f'  FAIL  {name}  {detail}')


def keys_of(evs):
    """Key presses/releases, without the kernel's autorepeat events (value 2)."""
    return [(c, v) for t, c, v in evs if t == EV_KEY and v != 2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port', default='/dev/ttyUSB0')
    args = ap.parse_args()

    paths = find_nodes()
    if set(paths) != {'kbd', 'abs', 'rel'}:
        sys.exit(f'ESP-KVM HID nodes not found (got {paths}); is the USB port connected?')
    nodes = {k: Node(p) for k, p in paths.items()}
    kbd, absm, relm = nodes['kbd'], nodes['abs'], nodes['rel']
    print('grabbed', ', '.join(f'{k}={p}' for k, p in sorted(paths.items())))
    r = Runner()
    kvm = EspKvm(args.port)
    try:
        print('[link]')
        pong = kvm.ping()
        r.check('ping (PiKVM binary)', pong.keyboard_online and pong.mouse_online, pong)
        r.check('version (text)', kvm.text('version')[-1].startswith('OK ESP-KVM'))
        kvm.text('watchdog 0')
        kvm.release_all()
        for n in nodes.values():
            n.drain()

        print('[keyboard]')
        bad = []
        for name, code in KEY_SAMPLES.items():
            kvm.key(name, True)
            kvm.key(name, False)
            got = keys_of(kbd.events(0.08))
            if got != [(code, 1), (code, 0)]:
                bad.append(f'{name}: {got}')
        r.check(f'{len(KEY_SAMPLES)} keys map to the right evdev codes', not bad, bad)

        kvm.type('Hi, 9!')
        expect = [(42, 1), (35, 1), (35, 0), (42, 0), (23, 1), (23, 0), (51, 1), (51, 0),
                  (57, 1), (57, 0), (10, 1), (10, 0), (42, 1), (2, 1), (2, 0), (42, 0)]
        got = keys_of(kbd.events(0.4))
        r.check('type "Hi, 9!" (text protocol, US layout)', got == expect, got)

        kvm.text('combo ctrl+alt+Delete')
        got = keys_of(kbd.events(0.3))
        r.check('combo ctrl+alt+Delete', got == [(29, 1), (56, 1), (111, 1), (111, 0), (56, 0), (29, 0)], got)

        for k in ['KeyA', 'KeyB', 'KeyC', 'KeyD', 'KeyE', 'KeyF']:
            kvm.key(k, True)
        try:
            kvm.text('key KeyG down')
            seventh_rejected = False
        except KvmError:
            seventh_rejected = True
        held = keys_of(kbd.events(0.2))
        r.check('6-key rollover: 6 held, 7th refused', seventh_rejected and len(held) == 6, held)
        kvm.release_all()
        got = keys_of(kbd.events(0.2))
        r.check('clear HID releases held keys', sorted(got) == sorted((c, 0) for c, _ in held), got)

        kbd.set_led(LED_CAPS, True)
        time.sleep(0.15)
        caps_on = kvm.ping().caps and 'caps=1' in kvm.text('status')[-1]
        kbd.set_led(LED_CAPS, False)
        kbd.set_led(LED_NUM, True)
        time.sleep(0.15)
        pong = kvm.ping()
        r.check('LED output report -> PONG flags', caps_on and pong.num and not pong.caps, pong)
        kbd.set_led(LED_NUM, False)

        print('[absolute pointer]')
        absm.drain()
        kvm.mouse_move(0.25, 0.75)
        evs = dict(((t, c), v) for t, c, v in absm.events(0.2))
        x, y = evs.get((EV_ABS, ABS_X)), evs.get((EV_ABS, ABS_Y))
        r.check('move to (0.25, 0.75)', x is not None and y is not None and abs(x - 8192) <= 2 and abs(y - 24575) <= 2, (x, y))
        kvm.text('move 0 32767')
        evs = dict(((t, c), v) for t, c, v in absm.events(0.2))
        r.check('text "move 0 32767" hits the corner', evs.get((EV_ABS, ABS_X)) == 0 and evs.get((EV_ABS, ABS_Y)) == 32767, evs)

        expect_btn = {'left': BTN_LEFT, 'right': BTN_RIGHT, 'middle': BTN_MIDDLE, 'back': BTN_SIDE, 'forward': BTN_EXTRA}
        bad = []
        for b, code in expect_btn.items():
            kvm.click(b)
            got = keys_of(absm.events(0.1))
            if got != [(code, 1), (code, 0)]:
                bad.append(f'{b}: {got}')
        r.check('5 buttons click on the absolute pointer', not bad, bad)

        kvm.wheel(1)
        kvm.wheel(-2)
        wheel = [v for t, c, v in absm.events(0.2) if t == EV_REL and c == REL_WHEEL]
        r.check('wheel +1, -2', wheel == [1, -2], wheel)
        kvm.text('wheel 0 3')
        hw = [v for t, c, v in absm.events(0.2) if t == EV_REL and c == REL_HWHEEL]
        r.check('horizontal wheel (text protocol)', hw == [3], hw)

        print('[relative mouse]')
        relm.drain()
        kvm.mouse_rel(50, -30)
        evs = relm.events(0.2)
        r.check('rel (50, -30)', (EV_REL, REL_X, 50) in evs and (EV_REL, REL_Y, -30) in evs, evs)
        kvm.text('rel 300 0')
        sx = sum(v for t, c, v in relm.events(0.2) if t == EV_REL and c == REL_X)
        r.check('rel 300 is split into int8 reports', sx == 300, sx)
        kvm.click('left')
        got = keys_of(relm.events(0.2))
        r.check('buttons follow the last-used pointer (rel)', got == [(BTN_LEFT, 1), (BTN_LEFT, 0)], got)
        kvm.mouse_button('left', True)
        kvm.mouse_move(0.5, 0.5)
        rel_up = keys_of(relm.events(0.2))
        kvm.mouse_button('left', False)
        abs_seen = keys_of(absm.events(0.2))
        r.check('switching pointer moves a held button with it',
                rel_up == [(BTN_LEFT, 1), (BTN_LEFT, 0)] and abs_seen == [(BTN_LEFT, 1), (BTN_LEFT, 0)],
                (rel_up, abs_seen))

        print('[robustness]')
        kvm.text('watchdog 500')
        kvm.key('ShiftLeft', True)
        kbd.events(0.1)
        got = keys_of(kbd.events(1.2))
        r.check('watchdog releases a held key after 500 ms of silence', got == [(42, 0)], got)
        kvm.text('watchdog 3000')

        bad_pkt = bytes([0x33, 0x01, 0, 0, 0, 0, 0, 0])
        kvm.ser.write(bad_pkt)
        resp = kvm.ser.read(8)
        r.check('bad CRC -> CRC error response', len(resp) == 8 and resp[1] == 0x40, resp.hex())
        pkt = bytes([0x33, 0x7e, 0, 0, 0, 0])
        kvm.ser.write(pkt + struct.pack('>H', crc16(pkt)))
        resp = kvm.ser.read(8)
        r.check('unknown command -> invalid error', len(resp) == 8 and resp[1] == 0x45, resp.hex())
        kvm.ser.write(bytes([0x33, 0x01, 0x00]))  # truncated packet, then silence
        time.sleep(0.05)
        r.check('truncated packet is dropped, link recovers', kvm.ping().keyboard_online)
        for line, ok in [('key NoSuchKey', False), ('move 40000 1', False), ('frobnicate', False), ('status', True)]:
            try:
                kvm.text(line)
                res = True
            except KvmError:
                res = False
            r.check(f'text "{line}" -> {"OK" if ok else "ERR"}', res == ok)
        r.check('help lists commands', len(kvm.text('help')) > 8)

        print('[performance]')
        n = 300
        t0 = time.monotonic()
        for _ in range(n):
            kvm.ping()
        ping_ms = (time.monotonic() - t0) / n * 1000
        t0 = time.monotonic()
        for i in range(n):
            kvm.mouse_rel(1 if i % 2 else -1, 0)
        move_ms = (time.monotonic() - t0) / n * 1000
        relm.events(0.3)
        print(f'        ping round trip {ping_ms:.2f} ms, relative move round trip {move_ms:.2f} ms')
        r.check('round trip under 5 ms', ping_ms < 5 and move_ms < 5)
    finally:
        try:
            kvm.release_all()
        except Exception:
            pass
        kvm.close()
        for n in nodes.values():
            n.drain()
            n.close()

    print(f'\n{r.passed} passed, {r.failed} failed')
    return 1 if r.failed else 0


if __name__ == '__main__':
    sys.exit(main())
