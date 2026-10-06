#!/bin/sh
# Builds boot/uefi.c natively against a fake UEFI firmware and checks the boot info it produces.
set -e
cd "$(dirname "$0")/.."
H=build/hosttest; mkdir -p $H
sed -e 's#"../build/kernel_info.h"#"hostinfo.h"#' -e '/__asm__ volatile("cli");  /d' boot/uefi.c > $H/uefi_host_copy.c
FILESZ=$(grep KERNEL_FILESZ build/kernel_info.h | awk '{print $3}'); MEMSZ=$(grep KERNEL_MEMSZ build/kernel_info.h | awk '{print $3}')
cat > $H/hostinfo.h <<EOT
extern void host_kernel_entry(unsigned long long);
#define KERNEL_LOAD   0x100000ULL
#define KERNEL_FILESZ $FILESZ
#define KERNEL_MEMSZ  $MEMSZ
#define KERNEL_ENTRY64 ((unsigned long long)host_kernel_entry)
EOT
gcc -O1 -Wall -Wno-unused -fno-pie -I $H -c $H/uefi_host_copy.c -o $H/uefi.o
gcc -c boot/uefi_blob.S -o $H/blob.o
gcc -O1 -Wall -no-pie -Wl,-z,noexecstack tests/uefi_host.c $H/uefi.o $H/blob.o -o $H/uefi_host
$H/uefi_host $H/mbi.bin build/kernel.bin > $H/run1.txt
$H/uefi_host $H/mbi_rgb.bin build/kernel.bin variant > $H/run2.txt
python3 tests/check_uefi_mbi.py $H/mbi.bin $H/mbi_rgb.bin
