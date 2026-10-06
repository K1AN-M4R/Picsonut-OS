# Picsonut OS 1.0

یک سیستم‌عامل ساده‌ی ۶۴ بیتی (x86_64) با رابط گرافیکی مینیمال و ترمینال.
بوت روی **BIOS و UEFI** با بوت‌لودرهای اختصاصی (بدون GRUB).

## ساخت ISO
نیازها: `gcc binutils nasm python3` (برای اجرا در شبیه‌ساز: `qemu-system-x86 ovmf`)

    make            # خروجی: picsonut-os.iso
    make check      # تست‌های بدون شبیه‌ساز
    make run-bios   # QEMU در حالت BIOS
    make run-uefi   # QEMU در حالت UEFI
    make iso-grub   # مسیر قدیمی GRUB2 (نیاز به grub-mkrescue)

## روی فلش (دقت کنید /dev/sdX درست باشد!)

    sudo dd if=picsonut-os.iso of=/dev/sdX bs=4M status=progress conv=fsync

## دستورات ترمینال
help, info, about, echo, clear, date, uptime, mem, cpu, color, reboot, shutdown
(↑ و ↓ برای تاریخچه)

## ساختار
- src/boot.S – ورود ۳۲ بیتی (BIOS) و ورود ۶۴ بیتی (UEFI) به long mode
- src/kernel.c – گرافیک، کیبورد، شل
- boot/bios.asm – لودر BIOS (El Torito): A20، VBE، E820، خواندن کرنل
- boot/mbr.asm – MBR هیبرید برای بوت از فلش
- boot/uefi.c – لودر UEFI (BOOTX64.EFI): GOP، نقشه‌ی حافظه، ExitBootServices
- tools/mkiso.py – ساخت ISO9660 + El Torito دوگانه + ESP (FAT12)

## نکات
- Secure Boot باید خاموش باشد.
- کیبورد PS/2 (یا شبیه‌سازی PS/2 فریم‌ور)؛ کیبورد USB روی بعضی دستگاه‌های UEFI کار نمی‌کند.
- کرنل باید در آدرس ۱ مگابایت جا بگیرد (روی بعضی سخت‌افزارهای UEFI ممکن است اشغال باشد).
- وضعیت تست: ساختار ISO، لودر BIOS (در شبیه‌ساز x86 داخلی با BIOS جعلی) و لودر UEFI (روی فریم‌ور جعلی) تست شده‌اند؛
  بوت واقعی در QEMU/سخت‌افزار انجام نشده است.
