# Picsonut OS - build system.   Needs: gcc, binutils (ld, nm, objdump), nasm, python3.
# No GRUB / xorriso required: the ISO is produced by tools/mkiso.py with our own BIOS and UEFI loaders.
CC      ?= gcc
LD      ?= ld
NASM    ?= nasm

CFLAGS  := -m64 -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fno-pic -fno-pie -mno-red-zone \
           -mgeneral-regs-only -mcmodel=small -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables
LDFLAGS := -nostdlib -n -z noexecstack --no-warn-rwx-segments -z max-page-size=0x1000 -T src/linker.ld

# UEFI loader: position independent, flat, wrapped into a PE32+ file by tools/mkpe.py
EFLAGS  := -m64 -std=gnu11 -O2 -Wall -Wextra -ffreestanding -fPIE -fno-plt -fvisibility=hidden -mno-red-zone \
           -mgeneral-regs-only -fno-stack-protector -fno-builtin -fno-asynchronous-unwind-tables -fshort-wchar -I.

KERNEL  := build/picsonut.elf
ISO     := picsonut-os.iso

all: iso

build:
	mkdir -p build

# ---- kernel
OBJS := build/boot.o build/arch.o build/lib.o build/mem.o build/cpu.o build/fs.o build/cmds.o build/vim.o build/kernel.o

build/%.o: src/%.S | build
	$(CC) $(CFLAGS) -c $< -o $@

build/%.o: src/%.c src/kernel.h | build
	$(CC) $(CFLAGS) -c $< -o $@

build/kernel.o: src/font.h

$(KERNEL): $(OBJS) src/linker.ld
	$(LD) $(LDFLAGS) $(OBJS) -o $@
	python3 tools/check_header.py $@

build/kernel.bin build/kernel_info.h build/kernel_info.inc: $(KERNEL) tools/mkkernel.py
	python3 tools/mkkernel.py $(KERNEL) build

# ---- BIOS loader (El Torito boot image) and hybrid MBR
build/bios.img: boot/bios.asm build/kernel_info.inc
	$(NASM) -f bin -w+all boot/bios.asm -o $@ -l build/bios.lst

build/mbr.bin: boot/mbr.asm
	$(NASM) -f bin -w+all boot/mbr.asm -o $@ -l build/mbr.lst

# ---- UEFI loader
build/uefi.o: boot/uefi.c build/kernel_info.h | build
	$(CC) $(EFLAGS) -c $< -o $@

build/uefi_blob.o: boot/uefi_blob.S build/kernel.bin
	$(CC) $(EFLAGS) -c $< -o $@

build/uefi.elf: build/uefi.o build/uefi_blob.o boot/uefi.ld
	$(LD) -nostdlib -static -z noexecstack --no-warn-rwx-segments -z max-page-size=0x1000 -T boot/uefi.ld \
	      build/uefi.o build/uefi_blob.o -o $@

build/BOOTX64.EFI: build/uefi.elf tools/mkpe.py
	python3 tools/mkpe.py $< $@

# ---- ISO
iso: $(ISO)
$(ISO): build/bios.img build/mbr.bin build/BOOTX64.EFI build/kernel.bin README_ISO.txt tools/mkiso.py
	python3 tools/mkiso.py $@ --bios build/bios.img --mbr build/mbr.bin --efi build/BOOTX64.EFI \
	        --kernel build/kernel.bin --readme README_ISO.txt
	@echo "==> $(ISO) is ready (BIOS + UEFI bootable; also dd-able to a USB stick)"

# ---- structural check of the ISO (no emulator needed)
check: $(ISO)
	python3 tools/verify_iso.py $(ISO)

# ---- boot + feature tests in QEMU (needs qemu-system-x86; UEFI tests also need OVMF)
test: $(ISO)
	python3 tests/qemu_test.py $(ISO)

# ---- run in QEMU
run-bios: iso
	qemu-system-x86_64 -m 512 -smp 4 -cdrom $(ISO)

run-uefi: iso
	qemu-system-x86_64 -m 512 -smp 4 -bios /usr/share/ovmf/OVMF.fd -cdrom $(ISO)

# ---- optional: the old GRUB2/Multiboot2 route (needs grub-pc-bin grub-efi-amd64-bin xorriso mtools)
iso-grub: $(KERNEL)
	mkdir -p iso/boot/grub
	cp $(KERNEL) iso/boot/picsonut.elf
	grub-mkrescue -o picsonut-os-grub.iso iso -volid PICSONUT_OS

clean:
	rm -rf build $(ISO) picsonut-os-grub.iso iso/boot/picsonut.elf

.PHONY: all iso check test run-bios run-uefi iso-grub clean
