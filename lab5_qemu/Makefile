RISCV_GNU ?= riscv64-unknown-elf

CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy

CFLAGS = -march=rv64gc_zicbom -mcmodel=medany -ffreestanding -nostdlib -fno-builtin -g -Wall
QEMU ?= qemu-system-riscv64

TARGET = kernel

build: clean
	$(CC) $(CFLAGS) -c *.S *.c
	$(CC) -T link.ld -nostdlib -o $(TARGET).elf *.o -lgcc
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET)

run: build $(TARGET) initramfs.cpio
	$(QEMU) -M virt -m 8G -kernel $(TARGET) -serial stdio -initrd initramfs.cpio -device ramfb

clean:
	rm -f $(TARGET) $(TARGET).elf *.o
