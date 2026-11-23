// PiKVM kvmd MCU HID protocol constants (from pikvm/kvmd hid/arduino/src/proto.h).
//
// Request:  0x33 CMD D0 D1 D2 D3 CRC_HI CRC_LO   (CRC16/MODBUS over bytes 0..5)
// Response: 0x34 CODE OUT1 OUT2 0 0 CRC_HI CRC_LO
#pragma once
#include <stdint.h>

namespace proto {

constexpr uint8_t MAGIC = 0x33;
constexpr uint8_t MAGIC_RESP = 0x34;

constexpr uint8_t RESP_NONE = 0x24;
constexpr uint8_t RESP_CRC_ERROR = 0x40;
constexpr uint8_t RESP_INVALID_ERROR = 0x45;
constexpr uint8_t RESP_TIMEOUT_ERROR = 0x48;

constexpr uint8_t PONG_OK = 0x80;
constexpr uint8_t PONG_CAPS = 0x01;
constexpr uint8_t PONG_SCROLL = 0x02;
constexpr uint8_t PONG_NUM = 0x04;
constexpr uint8_t PONG_KEYBOARD_OFFLINE = 0x08;
constexpr uint8_t PONG_MOUSE_OFFLINE = 0x10;
constexpr uint8_t PONG_RESET_REQUIRED = 0x40;

constexpr uint8_t OUT1_DYNAMIC = 0x80;
constexpr uint8_t OUT1_KBD_USB = 0x01;
constexpr uint8_t OUT1_MOUSE_USB_ABS = 0x08;
constexpr uint8_t OUT1_MOUSE_USB_REL = 0x10;
constexpr uint8_t OUT2_HAS_USB = 0x01;

constexpr uint8_t CMD_PING = 0x01;
constexpr uint8_t CMD_REPEAT = 0x02;
constexpr uint8_t CMD_SET_KEYBOARD = 0x03;
constexpr uint8_t CMD_SET_MOUSE = 0x04;
constexpr uint8_t CMD_SET_CONNECTED = 0x05;
constexpr uint8_t CMD_CLEAR_HID = 0x10;
constexpr uint8_t CMD_KEYBOARD = 0x11;
constexpr uint8_t CMD_MOUSE_MOVE = 0x12;
constexpr uint8_t CMD_MOUSE_BUTTON = 0x13;
constexpr uint8_t CMD_MOUSE_WHEEL = 0x14;
constexpr uint8_t CMD_MOUSE_RELATIVE = 0x15;

// CMD_MOUSE_BUTTON: D0 holds left/right/middle, D1 holds back("up")/forward("down").
// SELECT = this button changes, STATE = pressed.
constexpr uint8_t BTN_LEFT_SELECT = 0x80, BTN_LEFT_STATE = 0x08;
constexpr uint8_t BTN_RIGHT_SELECT = 0x40, BTN_RIGHT_STATE = 0x04;
constexpr uint8_t BTN_MIDDLE_SELECT = 0x20, BTN_MIDDLE_STATE = 0x02;
constexpr uint8_t BTN_UP_SELECT = 0x80, BTN_UP_STATE = 0x08;
constexpr uint8_t BTN_DOWN_SELECT = 0x40, BTN_DOWN_STATE = 0x04;

inline uint16_t crc16(const uint8_t *buf, unsigned len) {
  uint16_t crc = 0xffff;
  for (unsigned i = 0; i < len; i++) {
    crc ^= buf[i];
    for (int b = 0; b < 8; b++) crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
  }
  return crc;
}

}  // namespace proto
