RISCV_GNU ?= riscv64-unknown-elf

CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy

CFLAGS = -march=rv64gc -mcmodel=medany -ffreestanding -nostdlib -fno-builtin -g -Wall -DQEMU_VIDEO=1
QEMU ?= qemu-system-riscv64
TARGET = kernel

ifeq ($(OS),Windows_NT)
CLEAN_CMD = powershell -NoProfile -Command "Remove-Item -Force -ErrorAction SilentlyContinue '$(TARGET)', '$(TARGET).elf', '*.o'; exit 0"
else
CLEAN_CMD = rm -f $(TARGET) $(TARGET).elf *.o
endif

build: clean
	$(CC) $(CFLAGS) -c *.S *.c
	$(CC) -T link.ld -nostdlib -o $(TARGET).elf *.o -lgcc
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET)

run: build
	$(QEMU) -M virt -m 8G -kernel $(TARGET) -serial stdio -monitor none -vnc 127.0.0.1:0 -device ramfb -initrd initramfs.cpio

clean:
	$(CLEAN_CMD)
