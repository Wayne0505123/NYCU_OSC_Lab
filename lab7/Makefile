RISCV_GNU ?= riscv64-unknown-elf
QEMU ?= qemu-system-riscv64
QEMU_VIDEO ?= 0
QEMU_FB_WIDTH ?= 640
QEMU_FB_HEIGHT ?= 480
QEMU_DISPLAY ?=
FIT ?= kernel.fit

CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy

CFLAGS = -march=rv64gc -mcmodel=medany -ffreestanding -nostdlib -fno-builtin -g -Wall -DQEMU_VIDEO=$(QEMU_VIDEO) -DQEMU_FB_WIDTH=$(QEMU_FB_WIDTH) -DQEMU_FB_HEIGHT=$(QEMU_FB_HEIGHT)
QEMU_FLAGS ?= -machine virt -m 1G -bios default -kernel $(TARGET) -initrd initramfs.cpio -serial stdio -monitor none -device ramfb $(QEMU_DISPLAY)

TARGET = kernel

ifeq ($(OS),Windows_NT)
CLEAN_CMD = powershell -NoProfile -Command "Remove-Item -Force -ErrorAction SilentlyContinue '$(TARGET)', '$(TARGET).elf', '$(FIT)', '*.o'; exit 0"
else
CLEAN_CMD = rm -f $(TARGET) $(TARGET).elf $(FIT) *.o
endif

build: clean
	$(CC) $(CFLAGS) -c *.S *.c
	$(CC) -T link.ld -nostdlib -o $(TARGET).elf *.o -lgcc
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET)

run: build
	$(QEMU) $(QEMU_FLAGS)

fit: build
	mkimage -f kernel.its $(FIT)

run-rv2: fit
	@echo Built $(FIT) for Orange Pi RV2. Copy it to the SD card boot partition.

clean:
	$(CLEAN_CMD)
