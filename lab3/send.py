import os
import struct
import sys
import time
import termios

BOOT_MAGIC = 0x544F4F42

if len(sys.argv) != 3:
    print(f"Usage: {sys.argv[0]} <tty> <kernel.bin>")
    sys.exit(1)

tty_path = sys.argv[1]
kernel_path = sys.argv[2]

with open(kernel_path, "rb") as f:
    data = f.read()

header = struct.pack("<II", BOOT_MAGIC, len(data))

fd = os.open(tty_path, os.O_RDWR | os.O_NOCTTY)

try:
    attrs = termios.tcgetattr(fd)

    # raw mode
    attrs[0] = 0
    attrs[1] = 0
    attrs[2] = attrs[2] | termios.CLOCAL | termios.CREAD
    attrs[3] = 0

    attrs[4] = termios.B115200
    attrs[5] = termios.B115200

    attrs[6][termios.VMIN] = 1
    attrs[6][termios.VTIME] = 0

    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)

    os.write(fd, header)
    time.sleep(0.1)

    chunk_size = 128

    sent = 0
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i + chunk_size]
        os.write(fd, chunk)
        sent += len(chunk)
        time.sleep(0.003)

    print(f"sent {sent} bytes")

finally:
    os.close(fd)
