RISCV_GNU ?= riscv64-unknown-elf
CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy
CFLAGS = -mcmodel=medany -ffreestanding -nostdlib -g -Wall
QEMU ?= qemu-system-riscv64
TARGET = kernel

build: clean
	$(CC) $(CFLAGS) -c *.S *.c
	$(LD) -T link.ld -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET)

# -serial stdio -> test all normal bootloader kernel's function
# -serial pty -> test "load" with send.py(in basic1_loaded)
run: build $(TARGET)
	$(QEMU) -M virt -m 8G -kernel $(TARGET) -initrd initramfs.cpio -display none -serial stdio

clean:
	rm -f $(TARGET) $(TARGET).elf *.o
