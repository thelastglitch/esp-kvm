// Composite USB HID device: boot-compatible keyboard, absolute pointer and
// relative mouse, all on the S3's native USB port (TinyUSB, one HID interface
// with report IDs; the keyboard falls back to boot protocol in BIOS/UEFI).
#pragma once
#include "USB.h"
#include "USBHID.h"
#include "tusb.h"
#include "keymap.h"

enum : uint8_t {
  RID_KEYBOARD = 1,
  RID_MOUSE_ABS = 2,
  RID_MOUSE_REL = 3,
};

// Mouse button bits in the HID reports.
enum : uint8_t {
  MB_LEFT = 0x01,
  MB_RIGHT = 0x02,
  MB_MIDDLE = 0x04,
  MB_BACK = 0x08,
  MB_FORWARD = 0x10,
};

enum MouseMode : uint8_t { MOUSE_ABS, MOUSE_REL };

class KvmHid : public USBHIDDevice {
public:
  KvmHid() : hid(HID_ITF_PROTOCOL_KEYBOARD) {
    static bool added = false;
    if (!added) {
      added = true;
      hid.addDevice(this, sizeof(DESCRIPTOR));
    }
  }

  void begin() {
    hid.begin();
  }

  uint16_t _onGetDescriptor(uint8_t *dst) override {
    memcpy(dst, DESCRIPTOR, sizeof(DESCRIPTOR));
    return sizeof(DESCRIPTOR);
  }

  // LED output report from the host (report ID 1, or 0 in boot protocol).
  void _onOutput(uint8_t report_id, const uint8_t *buffer, uint16_t len) override {
    if ((report_id == RID_KEYBOARD || report_id == 0) && len >= 1) leds = buffer[0];
  }

  bool online() {
    return tud_mounted() && !tud_suspended();
  }

  // ---- keyboard ----

  bool keyEvent(uint8_t usage, bool pressed) {
    if (!usage) return false;
    if (usage >= 0xe0 && usage <= 0xe7) {
      uint8_t bit = 1 << (usage - 0xe0);
      kbd.modifier = pressed ? (kbd.modifier | bit) : (kbd.modifier & ~bit);
    } else if (pressed) {
      int free_slot = -1;
      for (int i = 0; i < 6; i++) {
        if (kbd.keycode[i] == usage) return sendKeyboard();
        if (!kbd.keycode[i] && free_slot < 0) free_slot = i;
      }
      if (free_slot < 0) return false;  // 6KRO: no room
      kbd.keycode[free_slot] = usage;
    } else {
      for (int i = 0; i < 6; i++) {
        if (kbd.keycode[i] == usage) kbd.keycode[i] = 0;
      }
    }
    return sendKeyboard();
  }

  bool anyKeyHeld() {
    if (kbd.modifier) return true;
    for (int i = 0; i < 6; i++) {
      if (kbd.keycode[i]) return true;
    }
    return false;
  }

  // ---- mouse ----

  void mouseButton(uint8_t bit, bool pressed) {
    buttons = pressed ? (buttons | bit) : (buttons & ~bit);
    sendMouse(0, 0);
  }

  void mouseMoveAbs(uint16_t x, uint16_t y) {
    abs_x = x > 32767 ? 32767 : x;
    abs_y = y > 32767 ? 32767 : y;
    setMode(MOUSE_ABS);
    sendMouse(0, 0);
  }

  // Relative motion; deltas outside int8 are split into several reports.
  void mouseMoveRel(int dx, int dy) {
    setMode(MOUSE_REL);
    do {
      int8_t sx = dx > 127 ? 127 : (dx < -127 ? -127 : dx);
      int8_t sy = dy > 127 ? 127 : (dy < -127 ? -127 : dy);
      dx -= sx;
      dy -= sy;
      sendRel(sx, sy, 0, 0);
    } while (dx || dy);
  }

  void mouseWheel(int8_t v, int8_t h) {
    sendMouse(v, h);
  }

  void releaseAll() {
    memset(&kbd, 0, sizeof(kbd));
    sendKeyboard();
    buttons = 0;
    sendMouse(0, 0);
  }

  uint8_t leds = 0;  // bit0 num, bit1 caps, bit2 scroll (HID LED page order)
  uint8_t buttons = 0;
  MouseMode mode = MOUSE_ABS;

private:
  static constexpr uint8_t DESCRIPTOR[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),
    // Absolute pointer, same report layout as TUD_HID_REPORT_DESC_ABSMOUSE
    // (hid_abs_mouse_report_t) but with a Pointer application collection
    // instead of Mouse, so hosts that group by application (Linux
    // hid-generic) expose it as its own device rather than merging it with
    // the relative mouse.
    HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),
    HID_USAGE(HID_USAGE_DESKTOP_POINTER),
    HID_COLLECTION(HID_COLLECTION_APPLICATION),
      HID_REPORT_ID(RID_MOUSE_ABS)
      HID_USAGE(HID_USAGE_DESKTOP_POINTER),
      HID_COLLECTION(HID_COLLECTION_PHYSICAL),
        HID_USAGE_PAGE(HID_USAGE_PAGE_BUTTON),
        HID_USAGE_MIN(1), HID_USAGE_MAX(5), HID_LOGICAL_MIN(0), HID_LOGICAL_MAX(1),
        HID_REPORT_COUNT(5), HID_REPORT_SIZE(1), HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
        HID_REPORT_COUNT(1), HID_REPORT_SIZE(3), HID_INPUT(HID_CONSTANT),
        HID_USAGE_PAGE(HID_USAGE_PAGE_DESKTOP),
        HID_USAGE(HID_USAGE_DESKTOP_X), HID_USAGE(HID_USAGE_DESKTOP_Y),
        HID_LOGICAL_MIN(0), HID_LOGICAL_MAX_N(0x7fff, 2),
        HID_REPORT_SIZE(16), HID_REPORT_COUNT(2), HID_INPUT(HID_DATA | HID_VARIABLE | HID_ABSOLUTE),
        HID_USAGE(HID_USAGE_DESKTOP_WHEEL), HID_LOGICAL_MIN(0x81), HID_LOGICAL_MAX(0x7f),
        HID_REPORT_COUNT(1), HID_REPORT_SIZE(8), HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE),
        HID_USAGE_PAGE(HID_USAGE_PAGE_CONSUMER), HID_USAGE_N(HID_USAGE_CONSUMER_AC_PAN, 2),
        HID_LOGICAL_MIN(0x81), HID_LOGICAL_MAX(0x7f),
        HID_REPORT_COUNT(1), HID_REPORT_SIZE(8), HID_INPUT(HID_DATA | HID_VARIABLE | HID_RELATIVE),
      HID_COLLECTION_END,
    HID_COLLECTION_END,
    TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(RID_MOUSE_REL)),
  };

  // Held buttons follow the active pointer: release them on the old one.
  void setMode(MouseMode m) {
    if (m == mode) return;
    if (buttons) {
      uint8_t held = buttons;
      buttons = 0;
      sendMouse(0, 0);
      buttons = held;
    }
    mode = m;
  }

  void wake() {
    if (tud_suspended()) tud_remote_wakeup();
  }

  bool sendKeyboard() {
    wake();
    return hid.SendReport(RID_KEYBOARD, &kbd, sizeof(kbd));
  }

  // Button and wheel changes go to whichever pointer was used last, so a
  // client that only ever sends relative motion never sees the cursor jump.
  void sendMouse(int8_t wheel, int8_t pan) {
    if (mode == MOUSE_ABS) sendAbs(wheel, pan);
    else sendRel(0, 0, wheel, pan);
  }

  void sendAbs(int8_t wheel, int8_t pan) {
    wake();
    hid_abs_mouse_report_t r = {buttons, (int16_t)abs_x, (int16_t)abs_y, wheel, pan};
    hid.SendReport(RID_MOUSE_ABS, &r, sizeof(r));
  }

  void sendRel(int8_t dx, int8_t dy, int8_t wheel, int8_t pan) {
    wake();
    hid_mouse_report_t r = {buttons, dx, dy, wheel, pan};
    hid.SendReport(RID_MOUSE_REL, &r, sizeof(r));
  }

  USBHID hid;
  hid_keyboard_report_t kbd = {};
  uint16_t abs_x = 16384, abs_y = 16384;
};
