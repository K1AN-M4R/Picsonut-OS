/* Picsonut OS - tiny freestanding C library helpers. */
#include "kernel.h"

void *memcpy(void *d, const void *s, usize n) {
    void *r = d; __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) :: "memory"); return r;
}
void *memset(void *d, int c, usize n) {
    void *r = d; __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory"); return r;
}
void *memmove(void *d, const void *s, usize n) {
    if ((usize)d <= (usize)s || (usize)d >= (usize)s + n) return memcpy(d, s, n);
    u8 *dd = (u8 *)d + n - 1; const u8 *ss = (const u8 *)s + n - 1;
    __asm__ volatile("std; rep movsb; cld" : "+D"(dd), "+S"(ss), "+c"(n) :: "memory");
    return d;
}
int memcmp(const void *a, const void *b, usize n) {
    const u8 *x = a, *y = b;
    for (usize i = 0; i < n; i++) if (x[i] != y[i]) return x[i] - y[i];
    return 0;
}
usize strlen(const char *s) { usize n = 0; while (s[n]) n++; return n; }
int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (u8)*a - (u8)*b;
}
int strncmp(const char *a, const char *b, usize n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (u8)*a - (u8)*b : 0;
}
void strlcpy(char *d, const char *s, usize n) {
    if (!n) return;
    usize i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}
u64 parse_u(const char *s, int *ok) {
    u64 v = 0; int any = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (u64)(*s - '0'); s++; any = 1; }
    if (ok) *ok = any && !*s;
    return v;
}
char *u64_to_dec(u64 v, char *b) {
    int i = 23; b[i] = 0;
    if (!v) b[--i] = '0';
    while (v) { b[--i] = (char)('0' + v % 10); v /= 10; }
    return &b[i];
}
char *u64_to_hex(u64 v, char *b) {
    static const char hx[] = "0123456789abcdef";
    b[0] = '0'; b[1] = 'x';
    for (int i = 0; i < 16; i++) b[2 + i] = hx[(v >> (60 - 4 * i)) & 15];
    b[18] = 0;
    return b;
}
