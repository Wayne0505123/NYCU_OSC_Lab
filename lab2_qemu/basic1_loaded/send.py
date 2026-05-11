import os
import struct
import sys
import time

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
    os.write(fd, b"load\r")
    time.sleep(0.2)

    os.write(fd, header)
    time.sleep(0.05)

    chunk_size = 256
    for i in range(0, len(data), chunk_size):
        os.write(fd, data[i:i+chunk_size])
        time.sleep(0.001)
finally:
    os.close(fd)
