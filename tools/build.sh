#!/bin/sh
# Build (and optionally flash) the firmware for one board profile.
#
#   tools/build.sh <profile> [serial-port]
#
# Profiles:
#   n16r8     ESP32-S3-N16R8 boards: 16 MB quad flash, 8 MB octal PSRAM
#   devkitc1  ESP32-S3-DevKitC-1 with the N8 module: 8 MB flash, no PSRAM
#
# The serial port is the board's UART port (CP210x/CH34x), e.g. /dev/ttyUSB0;
# the native USB port is taken over by the HID device once the firmware runs.
set -e
cd "$(dirname "$0")/../firmware"

# USB-OTG (TinyUSB) mode, UART0 for the command link, no debug logs on it.
COMMON="USBMode=default,CDCOnBoot=default,DebugLevel=none"
case "$1" in
  n16r8)    OPTS="FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi" ;;
  devkitc1) OPTS="FlashSize=8M,PartitionScheme=default_8MB,PSRAM=disabled" ;;
  *) echo "usage: $0 <n16r8|devkitc1> [serial-port]" >&2; exit 2 ;;
esac
FQBN="esp32:esp32:esp32s3:$COMMON,$OPTS"
OUT="build/$1"

arduino-cli compile -b "$FQBN" --output-dir "$OUT" \
  --build-property "compiler.cpp.extra_flags=-DBOARD_PROFILE=\"$1\"" esp_kvm
if [ -n "$2" ]; then
  arduino-cli upload -b "$FQBN" -p "$2" --input-dir "$OUT" esp_kvm
fi
