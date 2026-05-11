RISCV_GNU ?= riscv64-unknown-elf
CC = $(RISCV_GNU)-gcc
LD = $(RISCV_GNU)-ld
OBJCOPY = $(RISCV_GNU)-objcopy
CFLAGS = -mcmodel=medany -ffreestanding -nostdlib -g -Wall -fno-pie
TARGET = loaded

build: clean
	$(CC) $(CFLAGS) -c *.S *.c
	$(LD) -T link.ld -o $(TARGET).elf *.o
	$(OBJCOPY) -O binary $(TARGET).elf $(TARGET).bin

clean:
	rm -f $(TARGET).elf $(TARGET).bin *.o
