import os
import struct
import sys
import time
import termios

BOOT_MAGIC = 0x544F4F42
CHUNK_SIZE = 128
HEADER_PAUSE_SEC = 0.1
CHUNK_PAUSE_SEC = 0.003


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def send_payload(fd, label, data):
    header = struct.pack("<II", BOOT_MAGIC, len(data))

    print(f"sending {label}: {len(data)} bytes")
    os.write(fd, header)
    time.sleep(HEADER_PAUSE_SEC)

    sent = 0
    for i in range(0, len(data), CHUNK_SIZE):
        chunk = data[i:i + CHUNK_SIZE]
        os.write(fd, chunk)
        sent += len(chunk)
        time.sleep(CHUNK_PAUSE_SEC)

    print(f"sent {label}: {sent} bytes")


def setup_tty(fd):
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


def main():
    if len(sys.argv) != 4:
        print(f"Usage: {sys.argv[0]} <tty> <kernel.bin> <initramfs.cpio>")
        sys.exit(1)

    tty_path = sys.argv[1]
    kernel_path = sys.argv[2]
    cpio_path = sys.argv[3]

    kernel_data = read_file(kernel_path)
    cpio_data = read_file(cpio_path)

    fd = os.open(tty_path, os.O_RDWR | os.O_NOCTTY)

    try:
        setup_tty(fd)

        # The bootloader now expects two payloads:
        #   1. BOOT_MAGIC + kernel_size + kernel_data
        #   2. BOOT_MAGIC + cpio_size   + cpio_data
        send_payload(fd, "kernel", kernel_data)
        send_payload(fd, "cpio", cpio_data)

        print("done")

    finally:
        os.close(fd)


if __name__ == "__main__":
    main()

