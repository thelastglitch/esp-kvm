// ESP-KVM: serial-controlled USB keyboard + mouse for the ESP32-S3-DevKitC-1.
//
//   "USB"  port (native OTG)  -> target computer: composite HID (kvm_hid.h)
//   "UART" port (CP2102N)     -> controlling computer: 115200 8N1 on UART0
//
// Two protocols share the UART, told apart by the first byte of a message:
//   0x33        PiKVM kvmd MCU protocol, 8-byte binary packets (proto.h)
//   a letter    text commands, one per line ("help" lists them)
#include "kvm_hid.h"
#include "proto.h"

#define FW_VERSION "1.1.0"
#define UART_BAUD 115200
#ifndef BOARD_PROFILE
#define BOARD_PROFILE "generic"  // set by tools/build.sh
#endif
#define BINARY_BYTE_GAP_MS 20     // a packet whose bytes are further apart is discarded
#define DEFAULT_WATCHDOG_MS 3000  // release everything if the host goes silent

static KvmHid kvm;
static HardwareSerial &uart = Serial0;
static uint32_t last_rx_ms = 0;
static uint32_t watchdog_ms = DEFAULT_WATCHDOG_MS;
static bool watchdog_fired = false;

// ---------------------------------------------------------------- PiKVM binary

static uint8_t pkt[8];
static uint8_t pkt_len = 0;
static uint32_t pkt_last_ms = 0;
static uint8_t last_code = proto::RESP_NONE;

static uint8_t pongFlags() {
  uint8_t r = proto::PONG_OK;
  if (kvm.leds & 0x02) r |= proto::PONG_CAPS;
  if (kvm.leds & 0x04) r |= proto::PONG_SCROLL;
  if (kvm.leds & 0x01) r |= proto::PONG_NUM;
  if (!kvm.online()) r |= proto::PONG_KEYBOARD_OFFLINE | proto::PONG_MOUSE_OFFLINE;
  return r;
}

static void sendResponse(uint8_t code) {
  if (code) last_code = code;
  uint8_t r[8] = {proto::MAGIC_RESP, last_code, 0, 0, 0, 0, 0, 0};
  if (last_code & proto::PONG_OK) {
    r[1] = pongFlags();
    r[2] = proto::OUT1_KBD_USB | (kvm.mode == MOUSE_ABS ? proto::OUT1_MOUSE_USB_ABS : proto::OUT1_MOUSE_USB_REL);
    r[3] = proto::OUT2_HAS_USB;
  }
  uint16_t crc = proto::crc16(r, 6);
  r[6] = crc >> 8;
  r[7] = crc & 0xff;
  uart.write(r, 8);
}

static void mainButton(uint8_t data, uint8_t select, uint8_t state, uint8_t bit) {
  if (data & select) kvm.mouseButton(bit, data & state);
}

static uint8_t handlePacket(const uint8_t *p) {
  if (p[0] != proto::MAGIC || proto::crc16(p, 6) != ((p[6] << 8) | p[7])) return proto::RESP_CRC_ERROR;
  const uint8_t *d = p + 2;
  switch (p[1]) {
    case proto::CMD_PING: return proto::PONG_OK;
    case proto::CMD_REPEAT: return 0;
    case proto::CMD_SET_KEYBOARD:   // outputs are fixed (USB only); accept and ignore
    case proto::CMD_SET_MOUSE:
    case proto::CMD_SET_CONNECTED: return proto::PONG_OK;
    case proto::CMD_CLEAR_HID: kvm.releaseAll(); return proto::PONG_OK;
    case proto::CMD_KEYBOARD:
      if (d[0] == 0 || d[0] >= KEYMAP_SIZE) return proto::RESP_INVALID_ERROR;
      kvm.keyEvent(KEYMAP[d[0]].usage, d[1]);
      return proto::PONG_OK;
    case proto::CMD_MOUSE_MOVE: {
      // kvmd sends signed -32768..32767; the HID pointer wants 0..32767.
      int32_t x = (int16_t)((d[0] << 8) | d[1]);
      int32_t y = (int16_t)((d[2] << 8) | d[3]);
      kvm.mouseMoveAbs((x + 32768) >> 1, (y + 32768) >> 1);
      return proto::PONG_OK;
    }
    case proto::CMD_MOUSE_BUTTON:
      mainButton(d[0], proto::BTN_LEFT_SELECT, proto::BTN_LEFT_STATE, MB_LEFT);
      mainButton(d[0], proto::BTN_RIGHT_SELECT, proto::BTN_RIGHT_STATE, MB_RIGHT);
      mainButton(d[0], proto::BTN_MIDDLE_SELECT, proto::BTN_MIDDLE_STATE, MB_MIDDLE);
      mainButton(d[1], proto::BTN_UP_SELECT, proto::BTN_UP_STATE, MB_BACK);
      mainButton(d[1], proto::BTN_DOWN_SELECT, proto::BTN_DOWN_STATE, MB_FORWARD);
      return proto::PONG_OK;
    case proto::CMD_MOUSE_WHEEL: kvm.mouseWheel((int8_t)d[1], (int8_t)d[0]); return proto::PONG_OK;
    case proto::CMD_MOUSE_RELATIVE: kvm.mouseMoveRel((int8_t)d[0], (int8_t)d[1]); return proto::PONG_OK;
    default: return proto::RESP_INVALID_ERROR;
  }
}

// ---------------------------------------------------------------- text protocol

static char line[256];
static size_t line_len = 0;
static bool line_overflow = false;

static void reply(const char *fmt, ...) {
  char buf[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  uart.print(buf);
  uart.print("\r\n");
}

static uint8_t buttonByName(const char *s) {
  if (!strcasecmp(s, "left")) return MB_LEFT;
  if (!strcasecmp(s, "right")) return MB_RIGHT;
  if (!strcasecmp(s, "middle")) return MB_MIDDLE;
  if (!strcasecmp(s, "back")) return MB_BACK;
  if (!strcasecmp(s, "forward")) return MB_FORWARD;
  return 0;
}

static bool parseInt(const char *s, long *out) {
  if (!s || !*s) return false;
  char *end;
  *out = strtol(s, &end, 0);
  return *end == 0;
}

static void typeText(const char *s) {
  for (; *s; s++) {
    uint8_t usage;
    bool shift;
    if (!asciiToUsage(*s, &usage, &shift)) continue;
    if (shift) kvm.keyEvent(0xe1, true);
    kvm.keyEvent(usage, true);
    kvm.keyEvent(usage, false);
    if (shift) kvm.keyEvent(0xe1, false);
  }
}

// Press each key of "a+b+c" in order, then release in reverse.
static bool combo(char *spec) {
  uint8_t usages[8];
  int n = 0;
  for (char *tok = strtok(spec, "+"); tok; tok = strtok(nullptr, "+")) {
    if (n == 8 || !(usages[n] = keyUsageByName(tok))) return false;
    n++;
  }
  for (int i = 0; i < n; i++) kvm.keyEvent(usages[i], true);
  for (int i = n - 1; i >= 0; i--) kvm.keyEvent(usages[i], false);
  return n > 0;
}

static void printStatus() {
  reply("OK online=%d mounted=%d suspended=%d caps=%d num=%d scroll=%d mouse=%s buttons=0x%02x watchdog=%lu",
        kvm.online(), tud_mounted(), tud_suspended(), !!(kvm.leds & 0x02), !!(kvm.leds & 0x01),
        !!(kvm.leds & 0x04), kvm.mode == MOUSE_ABS ? "abs" : "rel", kvm.buttons, (unsigned long)watchdog_ms);
}

// Hardware the firmware is running on, to check the build profile matches
// the board (e.g. N16R8: flash=16MB psram=8MB).
static void printInfo() {
  uint64_t mac = ESP.getEfuseMac();  // first MAC byte is the lowest
  reply("OK board=%s chip=%s rev=%u cores=%u flash=%luMB psram=%luMB mac=%02x:%02x:%02x:%02x:%02x:%02x",
        BOARD_PROFILE, ESP.getChipModel(), ESP.getChipRevision(), ESP.getChipCores(),
        (unsigned long)(ESP.getFlashChipSize() >> 20), (unsigned long)(ESP.getPsramSize() >> 20),
        (int)(mac & 0xff), (int)(mac >> 8 & 0xff), (int)(mac >> 16 & 0xff), (int)(mac >> 24 & 0xff),
        (int)(mac >> 32 & 0xff), (int)(mac >> 40 & 0xff));
}

static void printHelp() {
  static const char *const HELP[] = {
    "ESP-KVM " FW_VERSION " text commands (one per line, replies OK/ERR):",
    "  ping | status | version | info | help",
    "  key <name> [down|up|press]   names: KeyA, Digit1, Enter, F5, ControlLeft... or a, ctrl, esc",
    "  combo <k1+k2+...>            e.g. combo ctrl+alt+Delete",
    "  type <text>                  US layout ASCII; '\\n' and '\\t' escapes",
    "  move <x> <y>                 absolute, 0..32767 each",
    "  rel <dx> <dy>                relative counts",
    "  button <left|right|middle|back|forward> [down|up|click]",
    "  click [button]               default left",
    "  wheel <v> [h]                scroll, -127..127",
    "  release                      release every key and button",
    "  watchdog <ms>                auto-release after silence, 0 = off",
  };
  for (const char *l : HELP) reply("%s", l);
  reply("OK");
}

static void handleType(const char *text) {
  char unescaped[256];
  size_t n = 0;
  for (const char *p = text; *p && n < sizeof(unescaped) - 1; p++) {
    if (p[0] == '\\' && (p[1] == 'n' || p[1] == 't' || p[1] == '\\')) {
      p++;
      unescaped[n++] = *p == 'n' ? '\n' : (*p == 't' ? '\t' : '\\');
    } else {
      unescaped[n++] = *p;
    }
  }
  unescaped[n] = 0;
  typeText(unescaped);
  reply("OK");
}

static void handleLine(char *s) {
  // "type" takes the rest of the line verbatim, so handle it before tokenizing.
  if (!strncasecmp(s, "type ", 5)) return handleType(s + 5);

  char *cmd = strtok(s, " \t");
  if (!cmd) return;
  char *a1 = strtok(nullptr, " \t");
  char *a2 = strtok(nullptr, " \t");

  if (!strcasecmp(cmd, "ping")) return reply("PONG");
  if (!strcasecmp(cmd, "status")) return printStatus();
  if (!strcasecmp(cmd, "version")) return reply("OK ESP-KVM " FW_VERSION);
  if (!strcasecmp(cmd, "info")) return printInfo();
  if (!strcasecmp(cmd, "help")) return printHelp();
  if (!strcasecmp(cmd, "release")) { kvm.releaseAll(); return reply("OK"); }

  if (!strcasecmp(cmd, "key")) {
    uint8_t usage = a1 ? keyUsageByName(a1) : 0;
    if (!usage) return reply("ERR unknown key");
    if (!a2 || !strcasecmp(a2, "press")) {
      kvm.keyEvent(usage, true);
      kvm.keyEvent(usage, false);
    } else if (!strcasecmp(a2, "down")) {
      if (!kvm.keyEvent(usage, true)) return reply("ERR too many keys held");
    } else if (!strcasecmp(a2, "up")) {
      kvm.keyEvent(usage, false);
    } else {
      return reply("ERR expected down|up|press");
    }
    return reply("OK");
  }

  if (!strcasecmp(cmd, "combo")) {
    if (!a1 || !combo(a1)) return reply("ERR bad combo");
    return reply("OK");
  }

  if (!strcasecmp(cmd, "move") || !strcasecmp(cmd, "rel")) {
    long x, y;
    if (!parseInt(a1, &x) || !parseInt(a2, &y)) return reply("ERR expected two integers");
    if (cmd[0] == 'm' || cmd[0] == 'M') {
      if (x < 0 || x > 32767 || y < 0 || y > 32767) return reply("ERR range is 0..32767");
      kvm.mouseMoveAbs(x, y);
    } else {
      if (labs(x) > 32767 || labs(y) > 32767) return reply("ERR delta too large");
      kvm.mouseMoveRel(x, y);
    }
    return reply("OK");
  }

  if (!strcasecmp(cmd, "button") || !strcasecmp(cmd, "click")) {
    uint8_t bit = buttonByName(a1 ? a1 : "left");
    if (!bit) return reply("ERR unknown button");
    const char *action = !strcasecmp(cmd, "click") ? "click" : (a2 ? a2 : "click");
    if (!strcasecmp(action, "down")) kvm.mouseButton(bit, true);
    else if (!strcasecmp(action, "up")) kvm.mouseButton(bit, false);
    else if (!strcasecmp(action, "click")) { kvm.mouseButton(bit, true); kvm.mouseButton(bit, false); }
    else return reply("ERR expected down|up|click");
    return reply("OK");
  }

  if (!strcasecmp(cmd, "wheel")) {
    long v, h = 0;
    if (!parseInt(a1, &v) || (a2 && !parseInt(a2, &h))) return reply("ERR expected integers");
    if (labs(v) > 127 || labs(h) > 127) return reply("ERR range is -127..127");
    kvm.mouseWheel(v, h);
    return reply("OK");
  }

  if (!strcasecmp(cmd, "watchdog")) {
    long ms;
    if (!parseInt(a1, &ms) || ms < 0) return reply("ERR expected milliseconds");
    watchdog_ms = ms;
    return reply("OK");
  }

  reply("ERR unknown command (try help)");
}

// ---------------------------------------------------------------- main loop

static void feed(uint8_t c, uint32_t now) {
  if (pkt_len > 0) {
    if (now - pkt_last_ms > BINARY_BYTE_GAP_MS) {
      pkt_len = 0;  // stale partial packet, start over with this byte
    } else {
      pkt[pkt_len++] = c;
      pkt_last_ms = now;
      if (pkt_len == 8) {
        pkt_len = 0;
        sendResponse(handlePacket(pkt));
      }
      return;
    }
  }
  if (line_len == 0 && c == proto::MAGIC) {
    pkt[0] = c;
    pkt_len = 1;
    pkt_last_ms = now;
    return;
  }
  if (c == '\r' || c == '\n') {
    if (line_overflow) reply("ERR line too long");
    else if (line_len) {
      line[line_len] = 0;
      handleLine(line);
    }
    line_len = 0;
    line_overflow = false;
  } else if (line_len < sizeof(line) - 1) {
    line[line_len++] = c;
  } else {
    line_overflow = true;
  }
}

void setup() {
  uart.setRxBufferSize(1024);
  uart.begin(UART_BAUD);
  USB.VID(0x303a);
  USB.PID(0x4b56);
  USB.manufacturerName("ESP-KVM");
  USB.productName("ESP-KVM Keyboard Mouse");
  kvm.begin();
  USB.begin();
  reply("ESP-KVM " FW_VERSION " ready");
}

void loop() {
  uint32_t now = millis();
  while (uart.available()) {
    feed(uart.read(), now);
    last_rx_ms = now;
    watchdog_fired = false;
  }
  if (watchdog_ms && !watchdog_fired && now - last_rx_ms > watchdog_ms) {
    watchdog_fired = true;
    if (kvm.anyKeyHeld() || kvm.buttons) kvm.releaseAll();
  }
  delay(1);
}
