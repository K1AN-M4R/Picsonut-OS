# Picsonut OS 1.1

A small 64-bit (x86_64) hobby OS: framebuffer desktop, terminal shell, RAM filesystem, Vim-style editor.
Boots on **BIOS and UEFI** with its own bootloaders (no GRUB). Multi-core, >4 GiB RAM, six standard resolutions.

## Build
Needs: `gcc binutils nasm python3`  (tests: `qemu-system-x86 ovmf`)

    make            # -> picsonut-os.iso
    make check      # structural check of the ISO
    make test       # boot + feature tests in QEMU
    make run-bios   # QEMU, BIOS, 4 CPUs
    make run-uefi   # QEMU, UEFI

## VirtualBox
New VM -> Type "Other", Version "Other/Unknown (64-bit)", 512 MB+ RAM, 2+ CPUs, enable I/O APIC,
no hard disk needed. Storage: attach `picsonut-os.iso` as the optical drive. (Tick "Enable EFI" for UEFI boot.)
At the boot menu press 1-6 to choose a resolution (default 3 = 1024x768 after 3 s).
Without I/O APIC the OS still boots, on one core.

## Resolutions
640x480, 800x600, 1024x768, 1280x720, 1280x800, 1920x1080 - boot menu, or `res N` / `res WxH` while running
(needs a Bochs/QEMU/VirtualBox-style display). `font 1|2` sets text size (auto 2x at >=1600 px wide).

## Shell
    ls [-la]  cd [dir|-|~]  pwd  mkdir [-p]  rmdir  touch  rm [-rf]  cp [-r]  mv  cat  echo [-n]
    echo text > file    echo more >> file    ls > file
    vim|vi|edit FILE
    info about cpu cores mem memtest date uptime res font color clear reboot shutdown
Files live in a RAM disk (/home/user, /tmp): **they are lost at reboot**.

## vim
Modes: Normal, Insert, Command.  `i a I A o O` insert, `Esc` normal, `:` command, `/` search.
Move: `h j k l`, arrows, `0 ^ $`, `w b`, `gg G nG`, Ctrl-F/B/D/U.  Edit: `x X r ~ J dd dw d$ d0 yy yw cc cw p P u Ctrl-R`, counts (`3dd`).
File: `:w [name]  :q  :q!  :wq  :x  :e[!] name  :set nu|nonu  :N  :help`.  `ZZ` save+quit.

## Layout
- src/boot.S - 32/64-bit entry, boot page tables        - src/mem.c - memory map, paging, heap
- src/arch.S, src/cpu.c - interrupts, APIC, SMP          - src/fs.c, src/cmds.c - filesystem + commands
- src/kernel.c - drawing, terminal, keyboard, shell      - src/vim.c - editor
- boot/bios.asm, boot/uefi.c - bootloaders (resolution menu) - tools/mkiso.py - ISO builder

## Notes / limits
- Secure Boot must be off. PS/2 keyboard (VM default) required.
- Other cores are started and can run jobs (`cores`); the shell/editor run on the boot core (no scheduler).
- Tested in QEMU (BIOS+UEFI, 64 MiB-20 GiB RAM, 1-8 CPUs, all six resolutions). Not tested on VirtualBox or real hardware.
