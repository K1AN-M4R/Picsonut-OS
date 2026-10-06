#!/usr/bin/env python3
"""Host-side preview: compiles the kernel's drawing/shell code for Linux, runs a
scripted session and writes preview.png (no emulator needed)."""
import subprocess, sys, re
src = open("src/kernel.c").read()
a = src.index("/* LOWLEVEL_BEGIN */"); b = src.index("/* LOWLEVEL_END */")
stub = '''
#include <string.h>
#include <stdlib.h>
static inline void outb(u16 p,u8 v){(void)p;(void)v;}
static inline void outw(u16 p,u16 v){(void)p;(void)v;}
static inline u8 inb(u16 p){(void)p;return 0;}
static inline void cpuid(u32 l,u32*a,u32*b,u32*c,u32*d){*a=l?0:0;*b=0x756e6547;*d=0x49656e69;*c=0x6c65746e;}
static inline void pause(void){}
'''
test = src[:a] + stub + src[b:]
test = test.replace('void kmain(u64 mbi)', 'void kmain_real(u64 mbi)')
test += r'''
#include <stdio.h>
int main(int argc,char**argv){
    fbw=1024; fbh=768; pitch=fbw*4; fb=calloc(pitch,fbh); have_fb=1;
    t_h=14;t_m=5;t_s=9;t_d=4;t_mo=10;t_y=2026;
    layout(); term_clear_cells(); draw_desktop(); draw_window(); term_redraw_all(); draw_clock();
    term_write_col("Welcome to " OS_NAME " " OS_VER "\n", 0xffffff);
    term_write("Type 'help' to see the available commands.\n\n");
    const char *cmds[] = {"info","help","echo hello Picsonut","color cyan","date","about","nope"};
    for (unsigned i=0;i<sizeof cmds/sizeof*cmds;i++){
        prompt(); term_write(cmds[i]); term_write("\n");
        char buf[200]; strcpy(buf,cmds[i]); run(buf);
    }
    prompt();
    FILE*f=fopen("/tmp/ht/out.ppm","wb"); fprintf(f,"P6\n%u %u\n255\n",fbw,fbh);
    for(u32 y=0;y<fbh;y++)for(u32 x=0;x<fbw;x++){u32 p=((u32*)(fb+y*pitch))[x];fputc((p>>rpos)&255,f);fputc((p>>gpos)&255,f);fputc((p>>bpos)&255,f);}
    fclose(f); printf("cols=%d rows=%d win=%dx%d\n",term_cols,term_rows,win_w,win_h); return 0;}
'''
open("/tmp/ht/test.c","w").write(test)
subprocess.check_call(["gcc","-O1","-w","-I","src","-o","/tmp/ht/test","/tmp/ht/test.c"])
subprocess.check_call(["/tmp/ht/test"])
from PIL import Image
Image.open("/tmp/ht/out.ppm").save(sys.argv[1] if len(sys.argv)>1 else "preview.png")
