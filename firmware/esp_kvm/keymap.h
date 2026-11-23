// Key table. The array index is the PiKVM "mcu_code" (kvmd keymap.csv), the
// name is the PiKVM / JS KeyboardEvent.code name, the value is the USB HID
// usage (keyboard page). Modifiers use the standard 0xE0..0xE7 usages.
#pragma once
#include <stdint.h>
#include <string.h>
#include <strings.h>

struct KeyDef {
  const char *name;
  uint8_t usage;
};

static const KeyDef KEYMAP[] = {
  {nullptr, 0x00},  // 0: unused
  {"KeyA", 0x04}, {"KeyB", 0x05}, {"KeyC", 0x06}, {"KeyD", 0x07}, {"KeyE", 0x08}, {"KeyF", 0x09},
  {"KeyG", 0x0a}, {"KeyH", 0x0b}, {"KeyI", 0x0c}, {"KeyJ", 0x0d}, {"KeyK", 0x0e}, {"KeyL", 0x0f},
  {"KeyM", 0x10}, {"KeyN", 0x11}, {"KeyO", 0x12}, {"KeyP", 0x13}, {"KeyQ", 0x14}, {"KeyR", 0x15},
  {"KeyS", 0x16}, {"KeyT", 0x17}, {"KeyU", 0x18}, {"KeyV", 0x19}, {"KeyW", 0x1a}, {"KeyX", 0x1b},
  {"KeyY", 0x1c}, {"KeyZ", 0x1d},
  {"Digit1", 0x1e}, {"Digit2", 0x1f}, {"Digit3", 0x20}, {"Digit4", 0x21}, {"Digit5", 0x22},
  {"Digit6", 0x23}, {"Digit7", 0x24}, {"Digit8", 0x25}, {"Digit9", 0x26}, {"Digit0", 0x27},
  {"Enter", 0x28}, {"Escape", 0x29}, {"Backspace", 0x2a}, {"Tab", 0x2b}, {"Space", 0x2c},
  {"Minus", 0x2d}, {"Equal", 0x2e}, {"BracketLeft", 0x2f}, {"BracketRight", 0x30},
  {"Backslash", 0x31}, {"Semicolon", 0x33}, {"Quote", 0x34}, {"Backquote", 0x35},
  {"Comma", 0x36}, {"Period", 0x37}, {"Slash", 0x38}, {"CapsLock", 0x39},
  {"F1", 0x3a}, {"F2", 0x3b}, {"F3", 0x3c}, {"F4", 0x3d}, {"F5", 0x3e}, {"F6", 0x3f},
  {"F7", 0x40}, {"F8", 0x41}, {"F9", 0x42}, {"F10", 0x43}, {"F11", 0x44}, {"F12", 0x45},
  {"PrintScreen", 0x46}, {"Insert", 0x49}, {"Home", 0x4a}, {"PageUp", 0x4b}, {"Delete", 0x4c},
  {"End", 0x4d}, {"PageDown", 0x4e}, {"ArrowRight", 0x4f}, {"ArrowLeft", 0x50},
  {"ArrowDown", 0x51}, {"ArrowUp", 0x52},
  {"ControlLeft", 0xe0}, {"ShiftLeft", 0xe1}, {"AltLeft", 0xe2}, {"MetaLeft", 0xe3},
  {"ControlRight", 0xe4}, {"ShiftRight", 0xe5}, {"AltRight", 0xe6}, {"MetaRight", 0xe7},
  {"Pause", 0x48}, {"ScrollLock", 0x47}, {"NumLock", 0x53}, {"ContextMenu", 0x65},
  {"NumpadDivide", 0x54}, {"NumpadMultiply", 0x55}, {"NumpadSubtract", 0x56},
  {"NumpadAdd", 0x57}, {"NumpadEnter", 0x58},
  {"Numpad1", 0x59}, {"Numpad2", 0x5a}, {"Numpad3", 0x5b}, {"Numpad4", 0x5c}, {"Numpad5", 0x5d},
  {"Numpad6", 0x5e}, {"Numpad7", 0x5f}, {"Numpad8", 0x60}, {"Numpad9", 0x61}, {"Numpad0", 0x62},
  {"NumpadDecimal", 0x63}, {"Power", 0x66}, {"IntlBackslash", 0x64}, {"IntlYen", 0x89},
  {"IntlRo", 0x87}, {"KanaMode", 0x88}, {"Convert", 0x8a}, {"NonConvert", 0x8b},
  {"AudioVolumeMute", 0x7f}, {"AudioVolumeUp", 0x80}, {"AudioVolumeDown", 0x81},
  {"F20", 0x6f}, {"F21", 0x70}, {"F22", 0x71}, {"F23", 0x72}, {"F24", 0x73},
  {"F13", 0x68}, {"F14", 0x69}, {"F15", 0x6a}, {"F16", 0x6b}, {"F17", 0x6c}, {"F18", 0x6d},
  {"F19", 0x6e},
};
static const int KEYMAP_SIZE = sizeof(KEYMAP) / sizeof(KEYMAP[0]);

// Short aliases accepted by the text protocol, in addition to the names above.
struct KeyAlias {
  const char *alias;
  const char *name;
};
static const KeyAlias KEY_ALIASES[] = {
  {"ctrl", "ControlLeft"}, {"shift", "ShiftLeft"}, {"alt", "AltLeft"}, {"meta", "MetaLeft"},
  {"win", "MetaLeft"}, {"super", "MetaLeft"}, {"cmd", "MetaLeft"}, {"altgr", "AltRight"},
  {"esc", "Escape"}, {"del", "Delete"}, {"ins", "Insert"}, {"bksp", "Backspace"},
  {"return", "Enter"}, {"pgup", "PageUp"}, {"pgdn", "PageDown"}, {"up", "ArrowUp"},
  {"down", "ArrowDown"}, {"left", "ArrowLeft"}, {"right", "ArrowRight"}, {"menu", "ContextMenu"},
  {"prtsc", "PrintScreen"},
};

// Resolve a key name to its USB usage, 0 if unknown. Accepts PiKVM names,
// aliases, and bare single letters/digits ("a", "7") case-insensitively.
static uint8_t keyUsageByName(const char *name) {
  for (const KeyAlias &a : KEY_ALIASES) {
    if (!strcasecmp(name, a.alias)) {
      name = a.name;
      break;
    }
  }
  for (int i = 1; i < KEYMAP_SIZE; i++) {
    if (!strcasecmp(name, KEYMAP[i].name)) return KEYMAP[i].usage;
  }
  if (name[0] && !name[1]) {
    char c = name[0];
    if (c >= 'a' && c <= 'z') return 0x04 + (c - 'a');
    if (c >= 'A' && c <= 'Z') return 0x04 + (c - 'A');
    if (c >= '1' && c <= '9') return 0x1e + (c - '1');
    if (c == '0') return 0x27;
  }
  return 0;
}

// US-layout ASCII -> usage plus whether Shift is needed. Returns false for characters that cannot be typed.
static bool asciiToUsage(char c, uint8_t *usage, bool *shift) {
  static const char *SHIFTED_SYMS = "!@#$%^&*()";
  *shift = false;
  if (c >= 'a' && c <= 'z') { *usage = 0x04 + (c - 'a'); return true; }
  if (c >= 'A' && c <= 'Z') { *usage = 0x04 + (c - 'A'); *shift = true; return true; }
  if (c >= '1' && c <= '9') { *usage = 0x1e + (c - '1'); return true; }
  if (c == '0') { *usage = 0x27; return true; }
  const char *p = strchr(SHIFTED_SYMS, c);
  if (c && p) { *usage = 0x1e + (p - SHIFTED_SYMS); *shift = true; return true; }
  struct { char ch; uint8_t usage; bool shift; } static const T[] = {
    {'\n', 0x28, false}, {'\t', 0x2b, false}, {' ', 0x2c, false}, {'-', 0x2d, false},
    {'_', 0x2d, true},   {'=', 0x2e, false},  {'+', 0x2e, true},  {'[', 0x2f, false},
    {'{', 0x2f, true},   {']', 0x30, false},  {'}', 0x30, true},  {'\\', 0x31, false},
    {'|', 0x31, true},   {';', 0x33, false},  {':', 0x33, true},  {'\'', 0x34, false},
    {'"', 0x34, true},   {'`', 0x35, false},  {'~', 0x35, true},  {',', 0x36, false},
    {'<', 0x36, true},   {'.', 0x37, false},  {'>', 0x37, true},  {'/', 0x38, false},
    {'?', 0x38, true},
  };
  for (auto &e : T) {
    if (e.ch == c) { *usage = e.usage; *shift = e.shift; return true; }
  }
  return false;
}
