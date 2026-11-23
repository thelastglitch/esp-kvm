#!/bin/sh
# One-time host setup (run with sudo):
#  1. bind the cp210x driver to the DevKitC-1's CP2102N, which on this board
#     enumerates with a non-standard VID:PID (11ca:0205, "VeriFone USB modem")
#  2. let the plugdev group read the ESP-KVM HID device (303a:4b56) so the
#     hardware tests can grab its evdev nodes without root
set -e
cat > /etc/udev/rules.d/99-esp-kvm.rules <<'RULES'
ACTION=="add", SUBSYSTEM=="usb", ATTRS{idVendor}=="11ca", ATTRS{idProduct}=="0205", RUN+="/sbin/modprobe cp210x", RUN+="/bin/sh -c 'echo 11ca 0205 > /sys/bus/usb-serial/drivers/cp210x/new_id || true'"
SUBSYSTEM=="tty", ATTRS{idVendor}=="11ca", ATTRS{idProduct}=="0205", SYMLINK+="esp-kvm-uart", MODE="0660", GROUP="dialout"
SUBSYSTEM=="input", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4b56", MODE="0660", GROUP="plugdev"
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="303a", ATTRS{idProduct}=="4b56", MODE="0660", GROUP="plugdev"
RULES
modprobe cp210x
echo 11ca 0205 > /sys/bus/usb-serial/drivers/cp210x/new_id 2>/dev/null || true
udevadm control --reload-rules
udevadm trigger
echo "done:"; ls -l /dev/ttyUSB* /dev/esp-kvm-uart 2>&1
