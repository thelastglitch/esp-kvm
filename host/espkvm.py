#!/usr/bin/env python3
"""Client for the ESP-KVM serial HID bridge (runs on the controlling computer).

Library use:
    from espkvm import EspKvm
    with EspKvm("/dev/ttyUSB0") as kvm:
        kvm.key("ControlLeft", True); kvm.key("KeyC", True)
        kvm.release_all()
        kvm.mouse_move(0.5, 0.5)          # absolute, fractions of the screen
        kvm.click("left")

Commands go over the PiKVM kvmd MCU protocol (8-byte packets with CRC); the
text protocol is available through EspKvm.text() for anything else.

CLI:  espkvm.py [-p PORT] <command> ...   (espkvm.py -h for the list)
"""
import argparse
import struct
import sys
import time

import serial

# Index = PiKVM mcu_code (kvmd keymap.csv); names are JS KeyboardEvent.code.
KEY_NAMES = [
    None,
    'KeyA', 'KeyB', 'KeyC', 'KeyD', 'KeyE', 'KeyF', 'KeyG', 'KeyH', 'KeyI', 'KeyJ', 'KeyK', 'KeyL',
    'KeyM', 'KeyN', 'KeyO', 'KeyP', 'KeyQ', 'KeyR', 'KeyS', 'KeyT', 'KeyU', 'KeyV', 'KeyW', 'KeyX',
    'KeyY', 'KeyZ', 'Digit1', 'Digit2', 'Digit3', 'Digit4', 'Digit5', 'Digit6', 'Digit7', 'Digit8',
    'Digit9', 'Digit0', 'Enter', 'Escape', 'Backspace', 'Tab', 'Space', 'Minus', 'Equal',
    'BracketLeft', 'BracketRight', 'Backslash', 'Semicolon', 'Quote', 'Backquote', 'Comma',
    'Period', 'Slash', 'CapsLock', 'F1', 'F2', 'F3', 'F4', 'F5', 'F6', 'F7', 'F8', 'F9', 'F10',
    'F11', 'F12', 'PrintScreen', 'Insert', 'Home', 'PageUp', 'Delete', 'End', 'PageDown',
    'ArrowRight', 'ArrowLeft', 'ArrowDown', 'ArrowUp', 'ControlLeft', 'ShiftLeft', 'AltLeft',
    'MetaLeft', 'ControlRight', 'ShiftRight', 'AltRight', 'MetaRight', 'Pause', 'ScrollLock',
    'NumLock', 'ContextMenu', 'NumpadDivide', 'NumpadMultiply', 'NumpadSubtract', 'NumpadAdd',
    'NumpadEnter', 'Numpad1', 'Numpad2', 'Numpad3', 'Numpad4', 'Numpad5', 'Numpad6', 'Numpad7',
    'Numpad8', 'Numpad9', 'Numpad0', 'NumpadDecimal', 'Power', 'IntlBackslash', 'IntlYen', 'IntlRo',
    'KanaMode', 'Convert', 'NonConvert', 'AudioVolumeMute', 'AudioVolumeUp', 'AudioVolumeDown',
    'F20', 'F21', 'F22', 'F23', 'F24', 'F13', 'F14', 'F15', 'F16', 'F17', 'F18', 'F19',
]
KEY_CODES = {name: code for code, name in enumerate(KEY_NAMES) if name}

ALIASES = {
    'ctrl': 'ControlLeft', 'shift': 'ShiftLeft', 'alt': 'AltLeft', 'meta': 'MetaLeft',
    'win': 'MetaLeft', 'super': 'MetaLeft', 'cmd': 'MetaLeft', 'altgr': 'AltRight',
    'esc': 'Escape', 'del': 'Delete', 'ins': 'Insert', 'bksp': 'Backspace', 'return': 'Enter',
    'pgup': 'PageUp', 'pgdn': 'PageDown', 'up': 'ArrowUp', 'down': 'ArrowDown',
    'left': 'ArrowLeft', 'right': 'ArrowRight', 'menu': 'ContextMenu', 'prtsc': 'PrintScreen',
}

CMD_PING, CMD_REPEAT = 0x01, 0x02
CMD_CLEAR_HID, CMD_KEYBOARD = 0x10, 0x11
CMD_MOUSE_MOVE, CMD_MOUSE_BUTTON, CMD_MOUSE_WHEEL, CMD_MOUSE_RELATIVE = 0x12, 0x13, 0x14, 0x15

# (byte index, select bit, state bit) for CMD_MOUSE_BUTTON
BUTTONS = {
    'left': (0, 0x80, 0x08), 'right': (0, 0x40, 0x04), 'middle': (0, 0x20, 0x02),
    'back': (1, 0x80, 0x08), 'forward': (1, 0x40, 0x04),
}

RESP_ERRORS = {0x24: 'no previous response', 0x40: 'CRC error', 0x45: 'invalid command',
               0x48: 'timeout'}


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def resolve_key(name: str) -> int:
    name = ALIASES.get(name.lower(), name)
    for known, code in KEY_CODES.items():
        if known.lower() == name.lower():
            return code
    if len(name) == 1 and name.isalnum():
        return KEY_CODES['Key' + name.upper()] if name.isalpha() else KEY_CODES['Digit' + name]
    raise ValueError(f'unknown key: {name}')


class Pong:
    def __init__(self, code: int, out1: int, out2: int):
        self.caps = bool(code & 0x01)
        self.scroll = bool(code & 0x02)
        self.num = bool(code & 0x04)
        self.keyboard_online = not code & 0x08
        self.mouse_online = not code & 0x10
        self.mouse_mode = {0x08: 'abs', 0x10: 'rel'}.get(out1 & 0x38, 'unknown')

    def __repr__(self):
        return (f'Pong(keyboard_online={self.keyboard_online}, mouse_online={self.mouse_online}, '
                f'caps={self.caps}, num={self.num}, scroll={self.scroll}, mouse={self.mouse_mode})')


class KvmError(Exception):
    pass


class EspKvm:
    def __init__(self, port='/dev/ttyUSB0', baud=115200, timeout=0.5):
        # DTR and RTS stay asserted together (pyserial's default), which leaves
        # the DevKitC auto-reset circuit idle: opening the port does not reset
        # the ESP32.
        self.ser = serial.Serial(port, baud, timeout=timeout)
        self.ser.reset_input_buffer()

    def close(self):
        self.ser.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    # ---- PiKVM binary protocol ----

    def _request(self, cmd: int, data: bytes = b'') -> Pong:
        packet = bytes([0x33, cmd]) + data.ljust(4, b'\0')
        packet += struct.pack('>H', crc16(packet))
        for _ in range(3):
            self.ser.write(packet)
            resp = self.ser.read(8)
            if len(resp) == 8 and resp[0] == 0x34 and crc16(resp[:6]) == struct.unpack('>H', resp[6:])[0]:
                code = resp[1]
                if code & 0x80:
                    return Pong(code, resp[2], resp[3])
                if code != 0x40:  # CRC errors are retried, anything else is final
                    raise KvmError(RESP_ERRORS.get(code, f'error 0x{code:02x}'))
            self.ser.reset_input_buffer()
            time.sleep(0.03)  # let the firmware drop any partial packet
        raise KvmError('no valid response from ESP-KVM')

    def ping(self) -> Pong:
        return self._request(CMD_PING)

    def key(self, name: str, pressed: bool) -> Pong:
        return self._request(CMD_KEYBOARD, bytes([resolve_key(name), int(pressed)]))

    def press(self, *names: str, hold: float = 0.0):
        """Press keys in order, then release them in reverse (a key combo)."""
        for n in names:
            self.key(n, True)
        if hold:
            time.sleep(hold)
        for n in reversed(names):
            self.key(n, False)

    def release_all(self) -> Pong:
        return self._request(CMD_CLEAR_HID)

    def mouse_move(self, x: float, y: float) -> Pong:
        """Absolute move; x and y are fractions of the screen, 0.0..1.0."""
        to_wire = lambda v: max(-32768, min(32767, round(min(max(v, 0.0), 1.0) * 65535 - 32768)))
        return self._request(CMD_MOUSE_MOVE, struct.pack('>hh', to_wire(x), to_wire(y)))

    def mouse_move_raw(self, x: int, y: int) -> Pong:
        """Absolute move in PiKVM wire units, -32768..32767."""
        return self._request(CMD_MOUSE_MOVE, struct.pack('>hh', x, y))

    def mouse_rel(self, dx: int, dy: int) -> Pong:
        pong = None
        while True:
            sx, sy = max(-127, min(127, dx)), max(-127, min(127, dy))
            pong = self._request(CMD_MOUSE_RELATIVE, struct.pack('>bb', sx, sy))
            dx, dy = dx - sx, dy - sy
            if not dx and not dy:
                return pong

    def mouse_button(self, button: str, pressed: bool) -> Pong:
        idx, select, state = BUTTONS[button]
        data = bytearray(2)
        data[idx] = select | (state if pressed else 0)
        return self._request(CMD_MOUSE_BUTTON, bytes(data))

    def click(self, button='left'):
        self.mouse_button(button, True)
        self.mouse_button(button, False)

    def wheel(self, delta: int) -> Pong:
        return self._request(CMD_MOUSE_WHEEL, struct.pack('>xb', max(-127, min(127, delta))))

    # ---- text protocol ----

    def text(self, line: str, timeout: float = 2.0) -> list:
        """Send one text command, return the reply lines (last one is OK/ERR/PONG)."""
        self.ser.reset_input_buffer()
        self.ser.write(line.encode() + b'\n')
        lines, deadline = [], time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = self.ser.readline()
            if not raw:
                continue
            lines.append(raw.decode(errors='replace').rstrip('\r\n'))
            if lines[-1].startswith(('OK', 'ERR', 'PONG')):
                if lines[-1].startswith('ERR'):
                    raise KvmError(lines[-1])
                return lines
        raise KvmError(f'timeout waiting for reply to {line!r}')

    def type(self, text: str):
        """Type ASCII text with a US layout (done by the firmware)."""
        esc = text.replace('\\', '\\\\').replace('\n', '\\n').replace('\t', '\\t')
        for i in range(0, len(esc), 200):
            chunk = esc[i:i + 200]
            if chunk.endswith('\\') and not chunk.endswith('\\\\'):
                chunk = esc[i:i + 199]  # never split an escape sequence
            self.text('type ' + chunk, timeout=10)


def main(argv=None):
    ap = argparse.ArgumentParser(description='Control a computer through the ESP-KVM bridge.')
    ap.add_argument('-p', '--port', default='/dev/ttyUSB0')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('ping', help='check the link and show LED/online state')
    p = sub.add_parser('key', help='tap, press or release one key')
    p.add_argument('name'), p.add_argument('action', nargs='?', default='tap', choices=['tap', 'down', 'up'])
    p = sub.add_parser('combo', help='key combination, e.g. ctrl+alt+Delete')
    p.add_argument('keys')
    p = sub.add_parser('type', help='type ASCII text')
    p.add_argument('text')
    p = sub.add_parser('move', help='absolute move, fractions 0..1')
    p.add_argument('x', type=float), p.add_argument('y', type=float)
    p = sub.add_parser('rel', help='relative move')
    p.add_argument('dx', type=int), p.add_argument('dy', type=int)
    p = sub.add_parser('click', help='click a mouse button')
    p.add_argument('button', nargs='?', default='left', choices=list(BUTTONS))
    p = sub.add_parser('wheel', help='scroll (positive = up)')
    p.add_argument('delta', type=int)
    sub.add_parser('release', help='release every key and button')
    p = sub.add_parser('raw', help='send a text-protocol command, e.g. "status"')
    p.add_argument('line', nargs='+')
    sub.add_parser('keys', help='list key names')
    args = ap.parse_args(argv)

    if args.cmd == 'keys':
        print(' '.join(KEY_NAMES[1:]))
        return 0
    with EspKvm(args.port) as kvm:
        if args.cmd == 'ping':
            print(kvm.ping())
        elif args.cmd == 'key':
            if args.action == 'tap':
                kvm.press(args.name)
            else:
                kvm.key(args.name, args.action == 'down')
        elif args.cmd == 'combo':
            kvm.press(*args.keys.split('+'))
        elif args.cmd == 'type':
            kvm.type(args.text)
        elif args.cmd == 'move':
            kvm.mouse_move(args.x, args.y)
        elif args.cmd == 'rel':
            kvm.mouse_rel(args.dx, args.dy)
        elif args.cmd == 'click':
            kvm.click(args.button)
        elif args.cmd == 'wheel':
            kvm.wheel(args.delta)
        elif args.cmd == 'release':
            kvm.release_all()
        elif args.cmd == 'raw':
            print('\n'.join(kvm.text(' '.join(args.line))))
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main())
    except (KvmError, ValueError, serial.SerialException) as e:
        sys.exit(f'error: {e}')
