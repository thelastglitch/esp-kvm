# ESP-KVM

ESP32-S3-DevKitC-1 as a serial-controlled USB keyboard + mouse, the input half
of an IP-less KVM.

```
computer A  <── USB HID ──  [USB  port] ESP32-S3 [UART port]  ── serial ──>  computer B
(controlled)                                                                 (controller)
```

* **USB port** (native OTG) → computer A. Shows up as `303a:4b56 ESP-KVM Keyboard Mouse`.
  It has three HID collections: a boot-protocol keyboard with LED feedback (works in BIOS/UEFI), an absolute
  pointer (0..32767, for clicking on a captured video frame), and a relative mouse
  (5 buttons, vertical and horizontal wheel).
* **UART port** (CP2102N) → computer B, 115200 8N1. Opening the port does not reset the board.
  On this board unit the CP2102N enumerates as `11ca:0205 "VeriFone USB modem"`, so Linux needs
  `tools/setup-linux.sh` (run once with sudo) to bind the `cp210x` driver. Windows/macOS may need
  the same VID:PID added to the Silicon Labs driver.

## Layout

| Path | What |
|---|---|
| `firmware/esp_kvm/` | Arduino sketch (`esp_kvm.ino`, `kvm_hid.h`, `proto.h`, `keymap.h`) |
| `host/espkvm.py` | Python client library + CLI for computer B (needs `pyserial`) |
| `host/hw_test.py` | End-to-end hardware test (Linux; both ports on the same machine) |
| `tools/setup-linux.sh` | udev rules: cp210x binding, plugdev access to the HID nodes |

## Build and flash

```bash
cd firmware
arduino-cli compile -b esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,DebugLevel=none --output-dir build esp_kvm
arduino-cli upload  -b esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,DebugLevel=none -p /dev/ttyUSB0 --input-dir build esp_kvm
```

`USBMode=default` means USB-OTG (TinyUSB). Flashing always goes through the UART port, because
the native port is taken over by the HID device.

## Protocols

Both protocols share the UART. A message starting with byte `0x33` is binary; a line starting with a letter is text.

### 1. PiKVM kvmd MCU protocol (binary)

This is the same wire format as PiKVM's Arduino/Pico HID (`pikvm/kvmd` `hid/arduino/src/proto.h`), so
tools that drive that firmware can drive this one.

Request: `33 CMD D0 D1 D2 D3 CRC_HI CRC_LO`. Response: `34 CODE OUT1 OUT2 00 00 CRC_HI CRC_LO`.
The CRC is CRC-16/MODBUS (poly 0xA001, init 0xFFFF) over the first 6 bytes, sent big-endian.

| CMD | Meaning | Data |
|---|---|---|
| `01` | ping | – |
| `02` | repeat last response | – |
| `03` / `04` / `05` | set keyboard / mouse output, set connected | accepted, no-op (outputs are fixed) |
| `10` | clear HID (release everything) | – |
| `11` | key | D0 = PiKVM key code (1..126, see `KEY_NAMES` in `host/espkvm.py`), D1 = 1 down / 0 up |
| `12` | absolute move | D0D1 = x, D2D3 = y, int16 big-endian, -32768..32767 |
| `13` | mouse buttons | D0: left `80`/`08`, right `40`/`04`, middle `20`/`02` (select/state); D1: back `80`/`08`, forward `40`/`04` |
| `14` | wheel | D1 = int8 vertical (D0 = int8 horizontal, an extension) |
| `15` | relative move | D0 = dx, D1 = dy, int8 |

Response CODE: `0x80 | flags` on success. The flags are caps `01`, scroll `02`, num `04`, keyboard offline `08`
and mouse offline `10`. Error codes are `40` (bad CRC), `45` (unknown command) and `24` (no previous response).
OUT1 reports the active mouse: `09` = absolute, `11` = relative.

### 2. Text protocol (for humans and scripts)

One command per line. Replies are `OK ...`, `ERR ...` or `PONG`. Send `help` for the list.

```
ping | status | version | help
key <name> [down|up|press]        KeyA, Digit1, Enter, F5, ControlLeft, ... or a, ctrl, esc, win
combo ctrl+alt+Delete
type Hello, world!\n              US layout; \n \t \\ escapes
move <x> <y>                      absolute 0..32767
rel <dx> <dy>
button <left|right|middle|back|forward> [down|up|click]
click [button]
wheel <v> [h]
release
watchdog <ms>                     auto-release after silence (default 3000, 0 = off)
```

### Behaviour notes

* **Watchdog:** if nothing arrives for `watchdog` ms while a key or button is held, the firmware releases everything.
  A controller that holds keys for long periods should send `ping` regularly.
* **Active pointer:** buttons and the wheel go to whichever pointer, absolute or relative, moved last. A button that
  is held when the pointer type changes moves to the new pointer.
* **Key rollover:** 6 keys plus modifiers. A 7th key is refused (the text protocol replies `ERR`).
* If computer A is asleep (USB suspended), any input triggers remote wakeup.

## CLI (computer B)

```bash
python3 host/espkvm.py ping
python3 host/espkvm.py type "hello"
python3 host/espkvm.py combo ctrl+alt+Delete
python3 host/espkvm.py move 0.5 0.5          # fractions of the screen
python3 host/espkvm.py click right
python3 host/espkvm.py raw status
```

## Hardware test

```bash
python3 host/hw_test.py -p /dev/ttyUSB0
```

The test grabs the ESP-KVM evdev nodes exclusively (EVIOCGRAB), so nothing it types reaches the desktop. It covers
43 key mappings, typing, combos, rollover, LED feedback, both pointers, all buttons and both wheels, the watchdog,
protocol errors, and round-trip latency (about 3 ms).
