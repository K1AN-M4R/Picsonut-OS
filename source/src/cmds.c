/* Picsonut OS - Unix-style file commands: pwd cd ls mkdir rmdir touch rm cp mv cat echo (+ vim launcher). */
#include "kernel.h"

static fnode *g_redir;

void cmd_out(const char *s) {
    if (g_redir) fs_append(g_redir, s, strlen(s)); else term_write(s);
}
void cmd_out_col(const char *s, u32 col) {
    if (g_redir) fs_append(g_redir, s, strlen(s)); else term_write_col(s, col);
}
static void out_n(const char *s, usize n) {
    if (g_redir) { fs_append(g_redir, s, n); return; }
    char buf[129];
    while (n) {
        usize k = n > 128 ? 128 : n;
        memcpy(buf, s, k); buf[k] = 0;
        term_write(buf);
        s += k; n -= k;
    }
}
void cmd_err(const char *prog, const char *msg, const char *arg) {
    term_write_col(prog, COL_ERR); term_write_col(": ", COL_ERR);
    if (arg) { term_write_col(arg, COL_ERR); term_write_col(": ", COL_ERR); }
    term_write_col(msg, COL_ERR); term_write("\n");
}
static const char *split_err(int e) {
    switch (e) {
    case -1: return "No such file or directory";
    case -2: return "Not a directory";
    case -4: return "File name too long";
    default: return "Invalid argument";
    }
}

/* "~" -> /home/user */
static const char *P(const char *a, char *buf) {
    if (a[0] == '~' && (a[1] == 0 || a[1] == '/')) {
        strlcpy(buf, "/home/user", FS_PATH_MAX);
        usize l = strlen(buf);
        strlcpy(buf + l, a + 1, FS_PATH_MAX - l);
        return buf;
    }
    return a;
}
static const char *base_of(const char *path, char *buf) {
    usize n = strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    usize s = n;
    while (s > 0 && path[s - 1] != '/') s--;
    usize l = n - s; if (l > FS_NAME_MAX) l = FS_NAME_MAX;
    memcpy(buf, path + s, l); buf[l] = 0;
    return buf;
}
static int split_flags(int argc, char **argv, const char *allowed, int *flags, const char *prog) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "--")) { i++; break; }
        for (const char *c = argv[i] + 1; *c; c++) {
            const char *f = allowed;
            while (*f && *f != *c) f++;
            if (!*f) {
                char t[3] = { *c, 0, 0 };
                term_write_col(prog, COL_ERR); term_write_col(": invalid option -- ", COL_ERR);
                term_write_col(t, COL_ERR); term_write("\n");
                return -1;
            }
            *flags |= 1 << (f - allowed);
        }
    }
    return i;
}

/* ---------------------------------------------------------------- pwd / cd */
static void c_pwd(void) { char p[FS_PATH_MAX]; fs_path(fs_cwd, p, sizeof p); cmd_out(p); cmd_out("\n"); }
static void c_cd(int argc, char **argv) {
    char pb[FS_PATH_MAX]; fnode *t;
    if (argc > 2) { cmd_err("cd", "too many arguments", NULL); return; }
    if (argc == 1) t = fs_find("/home/user");
    else if (!strcmp(argv[1], "-")) {
        t = fs_oldcwd;
        char p[FS_PATH_MAX]; fs_path(t, p, sizeof p); term_write(p); term_write("\n");
    } else {
        t = fs_find(P(argv[1], pb));
        if (!t) { cmd_err("cd", "No such file or directory", argv[1]); return; }
    }
    if (!t) t = fs_root;
    if (!t->is_dir) { cmd_err("cd", "Not a directory", argv[1]); return; }
    fs_oldcwd = fs_cwd; fs_cwd = t;
}

/* ---------------------------------------------------------------- ls */
static const char *months[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
static void ls_long_line(fnode *n, const char *name) {
    char b[64], t[24]; usize w = 0;
    const char *perm = n->is_dir ? "drwxr-xr-x" : "-rw-r--r--";
    for (; *perm; perm++) b[w++] = *perm;
    b[w++] = ' ';
    char sz[24]; const char *s = n->is_dir ? "-" : u64_to_dec(n->size, sz);
    int pad = 9 - (int)strlen(s);
    while (pad-- > 0) b[w++] = ' ';
    while (*s) b[w++] = *s++;
    b[w++] = ' ';
    const char *mn = months[(n->mmo ? n->mmo - 1 : 0) % 12];
    for (int i = 0; i < 3; i++) b[w++] = mn[i];
    b[w++] = ' ';
    b[w++] = n->md >= 10 ? (char)('0' + n->md / 10) : ' '; b[w++] = (char)('0' + n->md % 10);
    b[w++] = ' ';
    b[w++] = (char)('0' + n->mh / 10); b[w++] = (char)('0' + n->mh % 10); b[w++] = ':';
    b[w++] = (char)('0' + n->mmi / 10); b[w++] = (char)('0' + n->mmi % 10); b[w++] = ' ';
    b[w] = 0; (void)t;
    cmd_out(b);
    cmd_out_col(name, n->is_dir ? COL_DIR : COL_FG);
    cmd_out("\n");
}
static void ls_dir(fnode *d, int lflag, int aflag) {
    if (lflag) {
        if (aflag) { ls_long_line(d, "."); ls_long_line(d->parent ? d->parent : d, ".."); }
        for (fnode *c = d->child; c; c = c->next) if (aflag || c->name[0] != '.') ls_long_line(c, c->name);
        return;
    }
    fnode *items[512]; const char *names[512]; int n = 0;
    if (aflag) { items[n] = d; names[n++] = "."; items[n] = d->parent ? d->parent : d; names[n++] = ".."; }
    for (fnode *c = d->child; c && n < 512; c = c->next) if (aflag || c->name[0] != '.') { items[n] = c; names[n++] = c->name; }
    if (g_redir) { for (int i = 0; i < n; i++) { cmd_out(names[i]); cmd_out("\n"); } return; }
    int wmax = 0;
    for (int i = 0; i < n; i++) { int l = (int)strlen(names[i]); if (l > wmax) wmax = l; }
    int colw = wmax + 2, cols = term_cols / colw; if (cols < 1) cols = 1;
    int rows = (n + cols - 1) / cols;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int i = c * rows + r;
            if (i >= n) break;
            cmd_out_col(names[i], items[i]->is_dir ? COL_DIR : COL_FG);
            if ((c + 1) * rows + r < n) {                      /* pad every column but the last in this row */
                char sp[130]; int k = colw - (int)strlen(names[i]);
                if (k > 128) k = 128;
                for (int j = 0; j < k; j++) sp[j] = ' ';
                sp[k] = 0; cmd_out(sp);
            }
        }
        cmd_out("\n");
    }
}
static void c_ls(int argc, char **argv) {
    int fl = 0, i = split_flags(argc, argv, "la", &fl, "ls");
    if (i < 0) return;
    int lflag = fl & 1, aflag = fl & 2, npaths = argc - i;
    char pb[FS_PATH_MAX];
    if (npaths <= 0) { ls_dir(fs_cwd, lflag, aflag); return; }
    for (int k = i; k < argc; k++) {
        fnode *n = fs_find(P(argv[k], pb));
        if (!n) { cmd_err("ls", "cannot access: No such file or directory", argv[k]); continue; }
        if (npaths > 1 && n->is_dir) { if (k > i) cmd_out("\n"); cmd_out(argv[k]); cmd_out(":\n"); }
        if (n->is_dir) ls_dir(n, lflag, aflag);
        else if (lflag) ls_long_line(n, argv[k]);
        else { cmd_out(argv[k]); cmd_out("\n"); }
    }
}

/* ---------------------------------------------------------------- mkdir / rmdir / touch */
static void c_mkdir(int argc, char **argv) {
    int fl = 0, i = split_flags(argc, argv, "p", &fl, "mkdir");
    if (i < 0) return;
    if (i >= argc) { cmd_err("mkdir", "missing operand", NULL); return; }
    char pb[FS_PATH_MAX];
    for (; i < argc; i++) {
        const char *path = P(argv[i], pb);
        if (fl & 1) {                                        /* -p: create every missing component */
            char acc[FS_PATH_MAX]; usize w = 0; const char *p = path;
            fnode *cur = (*p == '/') ? fs_root : fs_cwd; int bad = 0;
            while (*p && !bad) {
                while (*p == '/') p++;
                if (!*p) break;
                char comp[FS_NAME_MAX + 2]; usize l = 0;
                while (*p && *p != '/') { if (l <= FS_NAME_MAX) comp[l] = *p; l++; p++; }
                if (l > FS_NAME_MAX) { cmd_err("mkdir", "File name too long", argv[i]); bad = 1; break; }
                comp[l] = 0;
                if (!strcmp(comp, ".")) continue;
                if (!strcmp(comp, "..")) { if (cur->parent) cur = cur->parent; continue; }
                fnode *ex = NULL;
                for (fnode *c = cur->child; c; c = c->next) if (!strcmp(c->name, comp)) ex = c;
                if (ex) { if (!ex->is_dir) { cmd_err("mkdir", "Not a directory", argv[i]); bad = 1; } else cur = ex; }
                else { fnode *n = fs_create(cur, comp, 1); if (!n) { cmd_err("mkdir", "cannot create directory", argv[i]); bad = 1; } else cur = n; }
            }
            (void)acc; (void)w;
            continue;
        }
        fnode *parent; char leaf[FS_NAME_MAX + 1];
        int e = fs_split(path, &parent, leaf);
        if (e) { cmd_err("mkdir", split_err(e), argv[i]); continue; }
        if (fs_find(path)) { cmd_err("mkdir", "File exists", argv[i]); continue; }
        if (!fs_create(parent, leaf, 1)) cmd_err("mkdir", "cannot create directory", argv[i]);
    }
}
static void c_rmdir(int argc, char **argv) {
    if (argc < 2) { cmd_err("rmdir", "missing operand", NULL); return; }
    char pb[FS_PATH_MAX];
    for (int i = 1; i < argc; i++) {
        fnode *n = fs_find(P(argv[i], pb));
        if (!n) cmd_err("rmdir", "No such file or directory", argv[i]);
        else if (!n->is_dir) cmd_err("rmdir", "Not a directory", argv[i]);
        else if (n->child) cmd_err("rmdir", "Directory not empty", argv[i]);
        else if (!n->parent || fs_is_within(n, fs_cwd)) cmd_err("rmdir", "Device or resource busy", argv[i]);
        else fs_unlink(n);
    }
}
static void c_touch(int argc, char **argv) {
    if (argc < 2) { cmd_err("touch", "missing file operand", NULL); return; }
    char pb[FS_PATH_MAX];
    for (int i = 1; i < argc; i++) {
        const char *path = P(argv[i], pb);
        fnode *n = fs_find(path);
        if (n) { fs_touch(n); continue; }
        fnode *parent; char leaf[FS_NAME_MAX + 1];
        int e = fs_split(path, &parent, leaf);
        if (e) { cmd_err("touch", split_err(e), argv[i]); continue; }
        if (!fs_create(parent, leaf, 0)) cmd_err("touch", "cannot create file", argv[i]);
    }
}

/* ---------------------------------------------------------------- rm */
static void c_rm(int argc, char **argv) {
    int fl = 0, i = split_flags(argc, argv, "rRf", &fl, "rm");
    if (i < 0) return;
    int rec = fl & 3, force = fl & 4;
    if (i >= argc) { if (!force) cmd_err("rm", "missing operand", NULL); return; }
    char pb[FS_PATH_MAX];
    for (; i < argc; i++) {
        fnode *n = fs_find(P(argv[i], pb));
        if (!n) { if (!force) cmd_err("rm", "cannot remove: No such file or directory", argv[i]); continue; }
        if (!n->parent) { cmd_err("rm", "it is dangerous to operate recursively on '/'", NULL); continue; }
        if (n->is_dir && !rec) { cmd_err("rm", "cannot remove: Is a directory", argv[i]); continue; }
        if (fs_is_within(n, fs_cwd)) { cmd_err("rm", "cannot remove: Device or resource busy (current directory)", argv[i]); continue; }
        fs_unlink(n);
    }
}

/* ---------------------------------------------------------------- cp / mv */
static int copy_tree(fnode *src, fnode *dstdir, const char *name) {
    fnode *n = fs_create(dstdir, name, src->is_dir);
    if (!n) return -1;
    if (!src->is_dir) return src->size ? fs_write(n, src->data, src->size) : 0;
    for (fnode *c = src->child; c; c = c->next) if (copy_tree(c, n, c->name) < 0) return -1;
    return 0;
}
/* Work out the destination (directory + final name) for one source. 0 ok */
static int dest_for(const char *prog, const char *src_arg, const char *dst_path, int dst_is_dir_ctx,
                    fnode **ddir, char *dname, fnode **existing) {
    fnode *d = fs_find(dst_path);
    char bb[FS_NAME_MAX + 1];
    if (d && d->is_dir) {                                    /* copy/move *into* the directory */
        *ddir = d; strlcpy(dname, base_of(src_arg, bb), FS_NAME_MAX + 1);
        for (fnode *c = d->child; c; c = c->next) if (!strcmp(c->name, dname)) { *existing = c; break; }
        return 0;
    }
    if (dst_is_dir_ctx) { cmd_err(prog, "target is not a directory", dst_path); return -1; }
    int e = fs_split(dst_path, ddir, dname);
    if (e) { cmd_err(prog, split_err(e), dst_path); return -1; }
    *existing = d;
    return 0;
}
static void c_cp(int argc, char **argv) {
    int fl = 0, i = split_flags(argc, argv, "rR", &fl, "cp");
    if (i < 0) return;
    int rec = fl & 3;
    if (argc - i < 2) { cmd_err("cp", argc - i < 1 ? "missing file operand" : "missing destination file operand", NULL); return; }
    char dbuf[FS_PATH_MAX], sbuf[FS_PATH_MAX];
    const char *dst = P(argv[argc - 1], dbuf);
    int multi = (argc - 1 - i) > 1;
    for (int k = i; k < argc - 1; k++) {
        fnode *src = fs_find(P(argv[k], sbuf));
        if (!src) { cmd_err("cp", "cannot stat: No such file or directory", argv[k]); continue; }
        if (src->is_dir && !rec) { cmd_err("cp", "-r not specified; omitting directory", argv[k]); continue; }
        fnode *dd, *ex = NULL; char dn[FS_NAME_MAX + 1];
        if (dest_for("cp", argv[k], dst, multi, &dd, dn, &ex) < 0) continue;
        if (ex == src) { cmd_err("cp", "source and destination are the same file", argv[k]); continue; }
        if (src->is_dir && fs_is_within(src, dd)) { cmd_err("cp", "cannot copy a directory into itself", argv[k]); continue; }
        if (ex) {
            if (ex->is_dir || src->is_dir) { cmd_err("cp", "cannot overwrite: type mismatch", dn); continue; }
            if (fs_write(ex, src->data, src->size) < 0) cmd_err("cp", "write failed (out of memory?)", dn);
            continue;
        }
        if (copy_tree(src, dd, dn) < 0) cmd_err("cp", "cannot create (out of memory or name invalid)", dn);
    }
}
static void c_mv(int argc, char **argv) {
    if (argc < 3) { cmd_err("mv", argc < 2 ? "missing file operand" : "missing destination file operand", NULL); return; }
    char dbuf[FS_PATH_MAX], sbuf[FS_PATH_MAX];
    const char *dst = P(argv[argc - 1], dbuf);
    int multi = (argc - 2) > 1;
    for (int k = 1; k < argc - 1; k++) {
        fnode *src = fs_find(P(argv[k], sbuf));
        if (!src) { cmd_err("mv", "cannot stat: No such file or directory", argv[k]); continue; }
        if (!src->parent) { cmd_err("mv", "cannot move the root directory", NULL); continue; }
        fnode *dd, *ex = NULL; char dn[FS_NAME_MAX + 1];
        if (dest_for("mv", argv[k], dst, multi, &dd, dn, &ex) < 0) continue;
        if (ex == src) { cmd_err("mv", "source and destination are the same file", argv[k]); continue; }
        if (src->is_dir && fs_is_within(src, dd)) { cmd_err("mv", "cannot move a directory into itself", argv[k]); continue; }
        if (ex) {
            if (ex->is_dir && !src->is_dir) { cmd_err("mv", "cannot overwrite directory with non-directory", dn); continue; }
            if (!ex->is_dir && src->is_dir) { cmd_err("mv", "cannot overwrite non-directory with directory", dn); continue; }
            if (ex->is_dir && ex->child)    { cmd_err("mv", "Directory not empty", dn); continue; }
            if (fs_is_within(ex, fs_cwd))   { cmd_err("mv", "Device or resource busy", dn); continue; }
            fs_unlink(ex);
        }
        fs_detach(src);
        strlcpy(src->name, dn, sizeof src->name);
        fs_attach(dd, src);
    }
}

/* ---------------------------------------------------------------- cat / echo */
static void c_cat(int argc, char **argv) {
    if (argc < 2) { cmd_err("cat", "missing file operand", NULL); return; }
    char pb[FS_PATH_MAX];
    for (int i = 1; i < argc; i++) {
        fnode *n = fs_find(P(argv[i], pb));
        if (!n) cmd_err("cat", "No such file or directory", argv[i]);
        else if (n->is_dir) cmd_err("cat", "Is a directory", argv[i]);
        else if (n == g_redir) cmd_err("cat", "input file is output file", argv[i]);
        else out_n((const char *)n->data, n->size);
    }
}
static void c_echo(int argc, char **argv) {
    int i = 1, nl = 1;
    if (i < argc && !strcmp(argv[i], "-n")) { nl = 0; i++; }
    for (; i < argc; i++) { cmd_out(argv[i]); if (i + 1 < argc) cmd_out(" "); }
    if (nl) cmd_out("\n");
}

static void c_vim(int argc, char **argv) {
    if (argc > 2) { cmd_err(argv[0], "only one file can be edited at a time", NULL); return; }
    char pb[FS_PATH_MAX];
    vim_run(argc == 2 ? P(argv[1], pb) : NULL);
}

int cmd_fs_dispatch(int argc, char **argv, fnode *redir) {
    if (argc < 1) return 0;
    const char *c = argv[0];
    g_redir = redir;
    if      (!strcmp(c, "pwd"))   c_pwd();
    else if (!strcmp(c, "cd"))    c_cd(argc, argv);
    else if (!strcmp(c, "ls") || !strcmp(c, "dir") || !strcmp(c, "ll")) {
        if (!strcmp(c, "ll")) { char *a[8]; int n = 0; a[n++] = argv[0]; a[n++] = (char *)"-l"; for (int i = 1; i < argc && n < 7; i++) a[n++] = argv[i]; c_ls(n, a); }
        else c_ls(argc, argv);
    }
    else if (!strcmp(c, "mkdir")) c_mkdir(argc, argv);
    else if (!strcmp(c, "rmdir")) c_rmdir(argc, argv);
    else if (!strcmp(c, "touch")) c_touch(argc, argv);
    else if (!strcmp(c, "rm"))    c_rm(argc, argv);
    else if (!strcmp(c, "cp"))    c_cp(argc, argv);
    else if (!strcmp(c, "mv"))    c_mv(argc, argv);
    else if (!strcmp(c, "cat"))   c_cat(argc, argv);
    else if (!strcmp(c, "echo"))  c_echo(argc, argv);
    else if (!strcmp(c, "vim") || !strcmp(c, "vi") || !strcmp(c, "edit")) c_vim(argc, argv);
    else { g_redir = NULL; return 0; }
    g_redir = NULL;
    return 1;
}
