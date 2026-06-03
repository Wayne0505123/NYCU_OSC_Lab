RISCV_GNU ?= riscv64-unknown-elf

CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy

CFLAGS = -march=rv64gc -mcmodel=medany -ffreestanding -nostdlib -fno-builtin -g -Wall

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
	@echo Built $(TARGET) for Orange Pi RV2. Boot it on the board via U-Boot/OpenSBI with DTB and initramfs.

clean:
	$(CLEAN_CMD)
