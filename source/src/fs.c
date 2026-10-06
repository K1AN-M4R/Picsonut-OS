/* Picsonut OS - a small in-memory Unix-like filesystem (tree of directories and files).
 * Paths use '/', support ".", ".." and a current directory.  Contents live in the kernel heap
 * and are lost on reboot/shutdown. */
#include "kernel.h"

fnode *fs_root, *fs_cwd, *fs_oldcwd;
static u64 total_bytes;
#define MAX_DEPTH 48

u64 fs_total_bytes(void) { return total_bytes; }

void fs_touch(fnode *f) {
    rtc_read();
    f->my = (u16)t_y; f->mmo = (u8)t_mo; f->md = (u8)t_d; f->mh = (u8)t_h; f->mmi = (u8)t_m;
}

static fnode *node_new(const char *name, int is_dir) {
    fnode *n = kzalloc(sizeof(fnode));
    if (!n) return NULL;
    strlcpy(n->name, name, sizeof n->name);
    n->is_dir = (u8)is_dir;
    fs_touch(n);
    return n;
}

void fs_attach(fnode *dir, fnode *n) {
    n->parent = dir;
    fnode **pp = &dir->child;
    while (*pp && strcmp((*pp)->name, n->name) < 0) pp = &(*pp)->next;
    n->next = *pp;
    *pp = n;
    fs_touch(dir);
}
void fs_detach(fnode *n) {
    if (!n->parent) return;
    fnode **pp = &n->parent->child;
    while (*pp && *pp != n) pp = &(*pp)->next;
    if (*pp) *pp = n->next;
    fs_touch(n->parent);
    n->next = NULL; n->parent = NULL;
}

static fnode *child_of(fnode *dir, const char *name, usize len) {
    for (fnode *c = dir->child; c; c = c->next)
        if (strlen(c->name) == len && !memcmp(c->name, name, len)) return c;
    return NULL;
}

/* Resolve every component of `path`. */
fnode *fs_find(const char *path) {
    fnode *cur = (*path == '/') ? fs_root : fs_cwd;
    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        usize len = (usize)(p - s);
        if (!cur->is_dir) return NULL;
        if (len == 1 && s[0] == '.') continue;
        if (len == 2 && s[0] == '.' && s[1] == '.') { if (cur->parent) cur = cur->parent; continue; }
        if (len > FS_NAME_MAX) return NULL;
        cur = child_of(cur, s, len);
        if (!cur) return NULL;
    }
    return cur;
}

/* Split into (existing parent directory, leaf name).  0 ok; -1 no such dir; -2 not a dir; -3 bad name; -4 too long */
int fs_split(const char *path, fnode **parent, char *leaf) {
    char tmp[FS_PATH_MAX];
    usize n = strlen(path);
    if (n == 0 || n >= sizeof tmp) return -3;
    memcpy(tmp, path, n + 1);
    while (n > 1 && tmp[n - 1] == '/') tmp[--n] = 0;
    char *slash = NULL;
    for (char *c = tmp; *c; c++) if (*c == '/') slash = c;
    const char *name; const char *dirpart;
    if (!slash)            { dirpart = ".";  name = tmp; }
    else if (slash == tmp) { dirpart = "/";  name = tmp + 1; }
    else                   { *slash = 0; dirpart = tmp; name = slash + 1; }
    if (!*name) return -3;
    if (strlen(name) > FS_NAME_MAX) return -4;
    fnode *d = fs_find(dirpart);
    if (!d) return -1;
    if (!d->is_dir) return -2;
    *parent = d;
    strlcpy(leaf, name, FS_NAME_MAX + 1);
    return 0;
}

static int depth_of(fnode *n) { int d = 0; while (n->parent) { n = n->parent; d++; } return d; }

fnode *fs_create(fnode *dir, const char *name, int is_dir) {
    if (!dir->is_dir || !*name || strlen(name) > FS_NAME_MAX) return NULL;
    if (!strcmp(name, ".") || !strcmp(name, "..") || child_of(dir, name, strlen(name))) return NULL;
    for (const char *c = name; *c; c++) if (*c == '/') return NULL;
    if (is_dir && depth_of(dir) >= MAX_DEPTH) return NULL;
    fnode *n = node_new(name, is_dir);
    if (!n) return NULL;
    fs_attach(dir, n);
    return n;
}

static int fs_reserve(fnode *f, usize n) {
    if (n > FS_FILE_MAX) return -1;
    if (n <= f->cap) return 0;
    usize ncap = f->cap ? f->cap : 64;
    while (ncap < n) ncap *= 2;
    if (ncap > FS_FILE_MAX) ncap = FS_FILE_MAX;
    u8 *d = krealloc(f->data, ncap);
    if (!d) return -1;
    f->data = d; f->cap = ncap;
    return 0;
}
int fs_write(fnode *f, const void *data, usize n) {
    if (f->is_dir || fs_reserve(f, n) < 0) return -1;
    if (n) memcpy(f->data, data, n);
    total_bytes = total_bytes - f->size + n;
    f->size = n;
    fs_touch(f);
    return 0;
}
int fs_append(fnode *f, const void *data, usize n) {
    if (f->is_dir || fs_reserve(f, f->size + n) < 0) return -1;
    if (n) memcpy(f->data + f->size, data, n);
    f->size += n; total_bytes += n;
    fs_touch(f);
    return 0;
}

static void free_tree(fnode *n) {
    while (n->child) { fnode *c = n->child; n->child = c->next; free_tree(c); }
    total_bytes -= n->size;
    kfree(n->data);
    kfree(n);
}
void fs_unlink(fnode *n) {
    if (!n->parent) return;                               /* never the root */
    fs_detach(n);
    free_tree(n);
}

int fs_is_within(fnode *anc, fnode *n) {
    for (; n; n = n->parent) if (n == anc) return 1;
    return 0;
}

void fs_path(fnode *n, char *out, usize cap) {
    fnode *chain[MAX_DEPTH + 4]; int k = 0;
    for (; n && n->parent && k < MAX_DEPTH + 3; n = n->parent) chain[k++] = n;
    usize w = 0;
    if (!k) { strlcpy(out, "/", cap); return; }
    for (int i = k - 1; i >= 0; i--) {
        if (w + 1 < cap) out[w++] = '/';
        for (const char *c = chain[i]->name; *c && w + 1 < cap; c++) out[w++] = *c;
    }
    out[w] = 0;
}

static void put_file(fnode *dir, const char *name, const char *text) {
    fnode *f = fs_create(dir, name, 0);
    if (f) fs_write(f, text, strlen(text));
}

void fs_init(void) {
    fs_root = node_new("", 1);
    fs_cwd = fs_oldcwd = fs_root;
    fnode *home = fs_create(fs_root, "home", 1);
    fnode *user = fs_create(home, "user", 1);
    fs_create(fs_root, "tmp", 1);
    fs_cwd = fs_oldcwd = user ? user : fs_root;
    if (!user) return;
    put_file(user, "readme.md",
        "# Welcome to Picsonut OS\n"
        "\n"
        "This is a RAM disk: files live in memory and are lost at reboot.\n"
        "\n"
        "## Try\n"
        "\n"
        "    ls -l            list files\n"
        "    mkdir notes      make a directory\n"
        "    vim todo.txt     edit a file (press i to type, Esc, then :wq to save)\n"
        "    cat readme.md    show a file\n"
        "\n"
        "Type `help` for every command.\n");
}
