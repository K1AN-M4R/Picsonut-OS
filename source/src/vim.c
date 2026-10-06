/* Picsonut OS - "vim": a small Vim-style modal text editor for the RAM filesystem.
 *
 * Modes   : NORMAL, INSERT, COMMAND (":" and "/" prompts)
 * Normal  : h j k l / arrows, 0 ^ $, w b, gg G (nG), Ctrl-F/B/D/U, PgUp/PgDn
 *           i a I A o O, x X, r, ~, J, dd dw d$ d0 dj dk dG (also y and c), counts (3dd, d2w),
 *           p P, u, Ctrl-R, /pattern n N, ZZ ZQ
 * Command : :w [file]  :q  :q!  :wq  :x  :e[!] [file]  :<number>  :set nu|nonu  :help
 * Files   : any name; .txt and .md are the intended targets.  Lines are plain 8-bit text. */
#include "kernel.h"

#define TAB 4
#define UNDO_MAX 24
#define UNDO_BYTES (6u * 1024 * 1024)

typedef struct { char *s; int len, cap; } line_t;
typedef struct { char *d; usize n; int cy, cx; } snap_t;
enum { M_NORMAL, M_INSERT, M_CMD };

static line_t *ln; static int nln, capln;
static int cy, cx, top, left;
static int modified, show_nu, mode, running;
static char fname[FS_PATH_MAX];
static char msg[200]; static u32 msg_col;
static char cmdbuf[120]; static int cmdlen; static char cmdpre;
static char last_search[64];
static char *reg; static usize reg_len; static int reg_line;
static snap_t undo_st[UNDO_MAX], redo_st[UNDO_MAX]; static int n_undo, n_redo;
static int cnt, op, opcnt, pend_g, pend_r, pend_Z;

#define C_MSG 0xd7dae0
static void say(const char *m, u32 col) { strlcpy(msg, m, sizeof msg); msg_col = col; }
static void say_err(const char *m) { say(m, 0xff7b72); }

/* ---------------------------------------------------------------- buffer */
static void lines_free(void) {
    for (int i = 0; i < nln; i++) kfree(ln[i].s);
    kfree(ln); ln = NULL; nln = capln = 0;
}
static int lines_grow(int need) {
    if (need <= capln) return 1;
    int nc = capln ? capln * 2 : 64; while (nc < need) nc *= 2;
    line_t *p = krealloc(ln, (usize)nc * sizeof(line_t));
    if (!p) return 0;
    ln = p; capln = nc; return 1;
}
static int line_reserve(line_t *l, int need) {
    if (need + 1 <= l->cap) return 1;
    int nc = l->cap ? l->cap * 2 : 16; while (nc < need + 1) nc *= 2;
    char *p = krealloc(l->s, (usize)nc);
    if (!p) return 0;
    l->s = p; l->cap = nc; return 1;
}
static int line_insert(int row, const char *s, int len) {
    if (!lines_grow(nln + 1)) return 0;
    line_t nl = { NULL, 0, 0 };
    if (!line_reserve(&nl, len)) return 0;
    if (len) memcpy(nl.s, s, (usize)len);
    nl.len = len;
    memmove(&ln[row + 1], &ln[row], (usize)(nln - row) * sizeof(line_t));
    ln[row] = nl; nln++;
    return 1;
}
static void line_remove(int row) {
    kfree(ln[row].s);
    memmove(&ln[row], &ln[row + 1], (usize)(nln - row - 1) * sizeof(line_t));
    nln--;
}
static int line_ins(line_t *l, int x, const char *s, int n) {
    if (!line_reserve(l, l->len + n)) return 0;
    memmove(l->s + x + n, l->s + x, (usize)(l->len - x));
    memcpy(l->s + x, s, (usize)n);
    l->len += n; return 1;
}
static void line_del(line_t *l, int x, int n) {
    if (x < 0 || x >= l->len) return;
    if (x + n > l->len) n = l->len - x;
    memmove(l->s + x, l->s + x + n, (usize)(l->len - x - n));
    l->len -= n;
}
static int load_text(const char *d, usize n) {
    lines_free();
    usize i = 0;
    do {
        usize s = i;
        while (i < n && d[i] != '\n') i++;
        if (!line_insert(nln, d + s, (int)(i - s))) { lines_free(); line_insert(0, "", 0); return 0; }
        i++;
    } while (i < n);
    return 1;
}
static char *serialize(usize *outn) {
    usize tot = 0;
    for (int i = 0; i < nln; i++) tot += (usize)ln[i].len + 1;
    char *b = kmalloc(tot ? tot : 1);
    if (!b) return NULL;
    usize w = 0;
    for (int i = 0; i < nln; i++) { memcpy(b + w, ln[i].s, (usize)ln[i].len); w += (usize)ln[i].len; b[w++] = '\n'; }
    *outn = tot; return b;
}

/* ---------------------------------------------------------------- undo / redo (whole-buffer snapshots) */
static void snap_free(snap_t *s) { kfree(s->d); s->d = NULL; s->n = 0; }
static int snap_take(snap_t *s) { s->d = serialize(&s->n); s->cy = cy; s->cx = cx; return s->d != NULL; }
static usize snap_bytes(snap_t *a, int n) { usize t = 0; for (int i = 0; i < n; i++) t += a[i].n; return t; }
static void stack_push(snap_t *st, int *n) {
    snap_t s;
    if (!snap_take(&s)) return;
    while (*n >= UNDO_MAX || (*n > 0 && snap_bytes(st, *n) + s.n > UNDO_BYTES)) {
        snap_free(&st[0]);
        memmove(&st[0], &st[1], (usize)(*n - 1) * sizeof(snap_t));
        (*n)--;
    }
    st[(*n)++] = s;
}
static void undo_push(void) {
    stack_push(undo_st, &n_undo);
    while (n_redo) snap_free(&redo_st[--n_redo]);
}
static void undo_clear(void) {
    while (n_undo) snap_free(&undo_st[--n_undo]);
    while (n_redo) snap_free(&redo_st[--n_redo]);
}
static void undo_redo(int redo) {
    snap_t *from = redo ? redo_st : undo_st; int *nf = redo ? &n_redo : &n_undo;
    snap_t *to = redo ? undo_st : redo_st;   int *nt = redo ? &n_undo : &n_redo;
    if (!*nf) { say(redo ? "Already at newest change" : "Already at oldest change", C_MSG); return; }
    stack_push(to, nt);
    snap_t s = from[--(*nf)];
    load_text(s.d, s.n);
    cy = s.cy; cx = s.cx; modified = 1;
    snap_free(&s);
    say(redo ? "1 change redone" : "1 change undone", C_MSG);
}

/* ---------------------------------------------------------------- register */
static void reg_set(const char *d, usize n, int linewise) {
    char *p = kmalloc(n ? n : 1);
    if (!p) return;
    if (n) memcpy(p, d, n);
    kfree(reg); reg = p; reg_len = n; reg_line = linewise;
}
static void reg_set_lines(int y1, int y2) {
    usize tot = 0;
    for (int i = y1; i <= y2; i++) tot += (usize)ln[i].len + 1;
    char *p = kmalloc(tot ? tot : 1);
    if (!p) return;
    usize w = 0;
    for (int i = y1; i <= y2; i++) { memcpy(p + w, ln[i].s, (usize)ln[i].len); w += (usize)ln[i].len; p[w++] = '\n'; }
    kfree(reg); reg = p; reg_len = tot; reg_line = 1;
}

/* ---------------------------------------------------------------- cursor helpers */
static int is_blank(char c) { return c == ' ' || c == '\t'; }
static int cls(char c) { if (is_blank(c)) return 0; return (c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) ? 1 : 2; }
static void clamp(void) {
    if (cy < 0) cy = 0;
    if (cy >= nln) cy = nln - 1;
    int mx = ln[cy].len - (mode == M_INSERT ? 0 : 1);
    if (mx < 0) mx = 0;
    if (cx > mx) cx = mx;
    if (cx < 0) cx = 0;
}
static int first_nonblank(int y) { int x = 0; while (x < ln[y].len && is_blank(ln[y].s[x])) x++; return x < ln[y].len ? x : (ln[y].len ? ln[y].len - 1 : 0); }
static void next_word(int *py, int *px) {
    int y = *py, x = *px;
    line_t *l = &ln[y];
    if (x < l->len && cls(l->s[x])) { int k = cls(l->s[x]); while (x < l->len && cls(l->s[x]) == k) x++; }
    for (;;) {
        l = &ln[y];
        if (x >= l->len) {
            if (y + 1 >= nln) { x = l->len ? l->len - 1 : 0; break; }
            y++; x = 0;
            if (ln[y].len == 0) break;
            continue;
        }
        if (!cls(l->s[x])) x++; else break;
    }
    *py = y; *px = x;
}
static void prev_word(int *py, int *px) {
    int y = *py, x = *px - 1;
    for (;;) {
        if (x < 0) {
            if (y == 0) { *py = 0; *px = 0; return; }
            y--; x = ln[y].len - 1;
            if (ln[y].len == 0) { *py = y; *px = 0; return; }
            continue;
        }
        if (!cls(ln[y].s[x])) x--; else break;
    }
    int k = cls(ln[y].s[x]);
    while (x > 0 && cls(ln[y].s[x - 1]) == k) x--;
    *py = y; *px = x;
}

/* ---------------------------------------------------------------- file I/O */
static int resolve_parent_ok(const char *path) {
    fnode *p; char leaf[FS_NAME_MAX + 1];
    return fs_split(path, &p, leaf) == 0;
}
static void num_str(char *d, u64 v) { char b[24]; strlcpy(d, u64_to_dec(v, b), 24); }
static void cat_str(char *d, usize cap, const char *s) { usize l = strlen(d); strlcpy(d + l, s, cap - l); }

static void open_file(const char *path) {
    char m[200], nb[24];
    if (!path || !*path) { load_text("", 0); fname[0] = 0; say("[No Name]", C_MSG); return; }
    strlcpy(fname, path, sizeof fname);
    fnode *n = fs_find(path);
    if (!n) {
        load_text("", 0);
        m[0] = '"'; m[1] = 0; cat_str(m, sizeof m, path); cat_str(m, sizeof m, "\" [New File]");
        say(m, C_MSG);
        return;
    }
    if (!load_text((const char *)n->data, n->size)) say_err("E342: Out of memory! (file truncated)");
    else {
        m[0] = '"'; m[1] = 0; cat_str(m, sizeof m, path); cat_str(m, sizeof m, "\" ");
        num_str(nb, (u64)nln - ((n->size && n->data[n->size - 1] == '\n') ? 0 : 0)); cat_str(m, sizeof m, nb);
        cat_str(m, sizeof m, "L, "); num_str(nb, n->size); cat_str(m, sizeof m, nb); cat_str(m, sizeof m, "B");
        say(m, C_MSG);
    }
}
static int write_file(const char *name, int is_current) {
    char m[200], nb[24];
    const char *path = (name && *name) ? name : fname;
    if (!*path) { say_err("E32: No file name"); return 0; }
    fnode *n = fs_find(path);
    if (n && n->is_dir) { say_err("E502: is a directory"); return 0; }
    if (!n) {
        fnode *par; char leaf[FS_NAME_MAX + 1];
        if (fs_split(path, &par, leaf)) { say_err("E212: Can't open file for writing (no such directory)"); return 0; }
        n = fs_create(par, leaf, 0);
        if (!n) { say_err("E212: Can't open file for writing"); return 0; }
    }
    usize len; char *buf = serialize(&len);
    if (!buf) { say_err("E342: Out of memory!"); return 0; }
    int rc = fs_write(n, buf, len);
    kfree(buf);
    if (rc < 0) { say_err("E514: write error (file too large or out of memory)"); return 0; }
    if (is_current || !fname[0]) { if (!fname[0]) strlcpy(fname, path, sizeof fname); modified = 0; }
    m[0] = '"'; m[1] = 0; cat_str(m, sizeof m, path); cat_str(m, sizeof m, "\" ");
    num_str(nb, (u64)nln); cat_str(m, sizeof m, nb); cat_str(m, sizeof m, "L, ");
    num_str(nb, len); cat_str(m, sizeof m, nb); cat_str(m, sizeof m, "B written");
    say(m, 0x7ee787);
    return 1;
}

/* ---------------------------------------------------------------- drawing */
static int text_rows(void) { return term_rows - 2; }
static int gutter_w(void) {
    if (!show_nu) return 0;
    int d = 1, v = nln; while (v >= 10) { v /= 10; d++; }
    return d + 1;
}
static int disp_col(const line_t *l, int x) {
    int c = 0;
    for (int i = 0; i < x && i < l->len; i++) c = l->s[i] == '\t' ? (c / TAB + 1) * TAB : c + 1;
    return c;
}
static void scroll_fix(void) {
    int tr = text_rows(), w = term_cols - gutter_w();
    if (w < 4) w = 4;
    if (cy < top) top = cy;
    if (cy >= top + tr) top = cy - tr + 1;
    if (top < 0) top = 0;
    int dc = disp_col(&ln[cy], cx);
    if (mode == M_NORMAL && ln[cy].len && cx < ln[cy].len && ln[cy].s[cx] == '\t') dc = disp_col(&ln[cy], cx + 1) - 1;
    if (dc < left) left = dc;
    if (dc >= left + w) left = dc - w + 1;
    if (left < 0) left = 0;
}
static void put_text(int r, int c, const char *s, int n, u32 fg, int inv) {
    for (int i = 0; i < n && c + i < term_cols; i++) term_set(r, c + i, s[i], fg, inv);
}
static void fill_row(int r, int from, u32 fg, int inv) { for (int c = from; c < term_cols; c++) term_set(r, c, ' ', fg, inv); }

static void draw(void) {
    scroll_fix();
    int tr = text_rows(), g = gutter_w(), W = term_cols - g;
    int cdc = disp_col(&ln[cy], cx);
    int cur_r = -1, cur_c = -1;
    char row[256];
    for (int r = 0; r < tr; r++) {
        int y = top + r;
        if (y >= nln) {
            term_set(r, 0, '~', 0x4c6ef5, 0); fill_row(r, 1, COL_FG, 0);
            continue;
        }
        if (g) {
            char nb[24]; char *s = u64_to_dec((u64)y + 1, nb);
            int l = (int)strlen(s);
            for (int c = 0; c < g; c++) term_set(r, c, ' ', COL_DIM, 0);
            put_text(r, g - 1 - l, s, l, COL_DIM, 0);
        }
        line_t *l = &ln[y];
        int dc = 0;
        for (int c = 0; c < W && c < (int)sizeof row; c++) row[c] = ' ';
        for (int i = 0; i < l->len; i++) {
            int w = l->s[i] == '\t' ? TAB - dc % TAB : 1;
            for (int j = 0; j < w; j++) {
                int sc = dc + j - left;
                if (sc >= 0 && sc < W && sc < (int)sizeof row) row[sc] = (l->s[i] == '\t') ? ' ' : l->s[i];
            }
            dc += w;
            if (dc - left >= W) break;
        }
        for (int c = 0; c < W && c < (int)sizeof row; c++) term_set(r, g + c, row[c], COL_FG, 0);
        if (y == cy) { cur_r = r; cur_c = g + cdc - left; }
    }
    if (mode != M_CMD && cur_r >= 0 && cur_c >= 0 && cur_c < term_cols) {
        char ch = ' ';
        int x = cx;
        if (x < ln[cy].len && ln[cy].s[x] != '\t') ch = ln[cy].s[x];
        term_set(cur_r, cur_c, ch, COL_FG, 1);
    }

    /* status bar */
    int sr = term_rows - 2;
    const char *mname = mode == M_INSERT ? " INSERT " : mode == M_CMD ? " COMMAND " : " NORMAL ";
    u32 mcol = mode == M_INSERT ? 0x7ee787 : mode == M_CMD ? 0xffb454 : 0x79c0ff;
    fill_row(sr, 0, 0x8b949e, 1);
    put_text(sr, 0, mname, (int)strlen(mname), mcol, 1);
    int c = (int)strlen(mname) + 1;
    const char *nm = fname[0] ? fname : "[No Name]";
    put_text(sr, c, nm, (int)strlen(nm), 0xe6edf3, 1); c += (int)strlen(nm);
    if (modified) { put_text(sr, c + 1, "[+]", 3, 0xffb454, 1); }
    char pos[64], nb[24]; pos[0] = 0;
    cat_str(pos, sizeof pos, u64_to_dec((u64)cy + 1, nb)); cat_str(pos, sizeof pos, ",");
    cat_str(pos, sizeof pos, u64_to_dec((u64)cx + 1, nb)); cat_str(pos, sizeof pos, "  ");
    int pct = nln > 1 ? (int)((u64)cy * 100 / (u64)(nln - 1)) : 100;
    cat_str(pos, sizeof pos, cy == 0 ? "Top" : cy == nln - 1 ? "Bot" : u64_to_dec((u64)pct, nb));
    if (cy != 0 && cy != nln - 1) cat_str(pos, sizeof pos, "%");
    put_text(sr, term_cols - (int)strlen(pos) - 1, pos, (int)strlen(pos), 0xe6edf3, 1);

    /* message / command line */
    int mr = term_rows - 1;
    fill_row(mr, 0, COL_FG, 0);
    if (mode == M_CMD) {
        term_set(mr, 0, cmdpre, 0xffb454, 0);
        put_text(mr, 1, cmdbuf, cmdlen, COL_FG, 0);
        term_set(mr, 1 + cmdlen < term_cols ? 1 + cmdlen : term_cols - 1, ' ', COL_FG, 1);
    } else if (mode == M_INSERT) {
        put_text(mr, 0, "-- INSERT --", 12, 0x7ee787, 0);
    } else if (msg[0]) {
        put_text(mr, 0, msg, (int)strlen(msg), msg_col, 0);
    }
}

static void help_screen(void) {
    static const char *h[] = {
        "Picsonut vim - quick help",
        "",
        "  MODES     i a I A o O = insert     Esc = normal     : = command line     / = search",
        "  MOVE      h j k l  arrows  0 ^ $  w b  gg G (5G)  Ctrl-F/B/D/U  PgUp/PgDn",
        "  EDIT      x X  r<c>  ~  J  dd dw d$ d0  yy yw  cc cw  p P  u  Ctrl-R  (counts: 3dd d2w)",
        "  SEARCH    /text <Enter>   n = next   N = previous",
        "  FILE      :w  :w name  :q  :q!  :wq  :x  :e name  :e!   ZZ = save+quit   ZQ = quit",
        "  OTHER     :set nu / :set nonu    :42 = go to line 42",
        "",
        "  Files are kept in the RAM disk (lost at reboot).  .txt and .md are the intended types.",
        "",
        "  Press any key to return",
    };
    for (int r = 0; r < term_rows; r++) fill_row(r, 0, COL_FG, 0);
    for (int i = 0; i < (int)(sizeof h / sizeof *h) && i < term_rows; i++) put_text(i, 0, h[i], (int)strlen(h[i]), i == 0 ? 0xffb454 : COL_FG, 0);
    key_wait();
}

/* ---------------------------------------------------------------- editing primitives */
static void ins_char(char c) {
    if (ln[cy].len >= 8000) { say_err("E: line too long"); return; }
    if (line_ins(&ln[cy], cx, &c, 1)) { cx++; modified = 1; }
}
static void ins_newline(void) {
    line_t *l = &ln[cy];
    int tail = l->len - cx;
    if (!line_insert(cy + 1, l->s + cx, tail)) { say_err("E342: Out of memory!"); return; }
    ln[cy].len = cx;
    cy++; cx = 0; modified = 1;
}
static void ins_backspace(void) {
    if (cx > 0) { line_del(&ln[cy], cx - 1, 1); cx--; modified = 1; }
    else if (cy > 0) {
        line_t *p = &ln[cy - 1], *l = &ln[cy];
        int px = p->len;
        if (!line_ins(p, p->len, l->s, l->len)) { say_err("E342: Out of memory!"); return; }
        line_remove(cy); cy--; cx = px; modified = 1;
    }
}
static void ins_delete(void) {
    if (cx < ln[cy].len) { line_del(&ln[cy], cx, 1); modified = 1; }
    else if (cy + 1 < nln) {
        line_t *p = &ln[cy], *n = &ln[cy + 1];
        if (!line_ins(p, p->len, n->s, n->len)) return;
        line_remove(cy + 1); modified = 1;
    }
}
static void delete_lines(int y1, int y2) {
    for (int i = y2; i >= y1; i--) line_remove(i);
    if (!nln) line_insert(0, "", 0);
    cy = y1 < nln ? y1 : nln - 1;
    cx = first_nonblank(cy);
    modified = 1;
}
static void join_lines(void) {
    if (cy + 1 >= nln) return;
    line_t *a = &ln[cy], *b = &ln[cy + 1];
    int s = 0; while (s < b->len && is_blank(b->s[s])) s++;
    int jx = a->len;
    if (a->len && b->len - s > 0 && !is_blank(a->s[a->len - 1])) { if (!line_ins(a, a->len, " ", 1)) return; }
    if (b->len - s > 0 && !line_ins(a, a->len, b->s + s, b->len - s)) return;
    line_remove(cy + 1);
    cx = jx; modified = 1;
}
static void paste(int after) {
    if (!reg || !reg_len) { say_err("E353: Nothing in register \""); return; }
    undo_push();
    if (reg_line) {
        int at = after ? cy + 1 : cy, start = at;
        usize i = 0;
        while (i < reg_len) {
            usize s = i;
            while (i < reg_len && reg[i] != '\n') i++;
            if (!line_insert(at, reg + s, (int)(i - s))) { say_err("E342: Out of memory!"); break; }
            at++; i++;
        }
        cy = start; cx = first_nonblank(cy);
    } else {
        int at = (after && ln[cy].len) ? cx + 1 : cx;
        if (!line_ins(&ln[cy], at, reg, (int)reg_len)) { say_err("E342: Out of memory!"); return; }
        cx = at + (int)reg_len - 1;
    }
    modified = 1;
}

static int search(int dir) {
    if (!last_search[0]) { say_err("E35: No previous regular expression"); return 0; }
    int plen = (int)strlen(last_search);
    int y = cy, x = cx + (dir > 0 ? 1 : -1);
    for (int step = 0; step <= nln; step++) {
        line_t *l = &ln[y];
        if (dir > 0) {
            for (int i = (step == 0 ? x : 0); i + plen <= l->len; i++)
                if (i >= 0 && !memcmp(l->s + i, last_search, (usize)plen)) { cy = y; cx = i; return 1; }
            y = (y + 1) % nln;
        } else {
            for (int i = (step == 0 ? x : l->len - plen); i >= 0; i--)
                if (i + plen <= l->len && !memcmp(l->s + i, last_search, (usize)plen)) { cy = y; cx = i; return 1; }
            y = (y + nln - 1) % nln;
        }
    }
    char m[120]; strlcpy(m, "E486: Pattern not found: ", sizeof m); cat_str(m, sizeof m, last_search);
    say_err(m);
    return 0;
}

/* ---------------------------------------------------------------- ex commands */
static void ex_command(char *c) {
    while (*c == ' ') c++;
    if (!*c) return;
    int ok; u64 num = parse_u(c, &ok);
    if (ok) { cy = num ? (int)(num > (u64)nln ? (u64)nln : num) - 1 : 0; cx = first_nonblank(cy); return; }
    char *arg = c;
    while (*arg && *arg != ' ') arg++;
    char cmd[16]; usize cl = (usize)(arg - c); if (cl >= sizeof cmd) cl = sizeof cmd - 1;
    memcpy(cmd, c, cl); cmd[cl] = 0;
    while (*arg == ' ') arg++;
    int bang = 0;
    if (cl && cmd[cl - 1] == '!') { bang = 1; cmd[cl - 1] = 0; }

    if (!strcmp(cmd, "w") || !strcmp(cmd, "write")) { write_file(*arg ? arg : NULL, !*arg || !strcmp(arg, fname)); }
    else if (!strcmp(cmd, "q") || !strcmp(cmd, "quit") || !strcmp(cmd, "close")) {
        if (modified && !bang) say_err("E37: No write since last change (add ! to override)");
        else running = 0;
    } else if (!strcmp(cmd, "wq") || !strcmp(cmd, "x") || !strcmp(cmd, "xit")) {
        if (write_file(*arg ? arg : NULL, !*arg || !strcmp(arg, fname))) running = 0;
    } else if (!strcmp(cmd, "e") || !strcmp(cmd, "edit") || !strcmp(cmd, "open")) {
        if (modified && !bang) { say_err("E37: No write since last change (add ! to override)"); return; }
        const char *target = *arg ? arg : fname;
        if (!*target) { say_err("E32: No file name"); return; }
        fnode *n = fs_find(target);
        if (n && n->is_dir) { say_err("E502: is a directory"); return; }
        if (!n && !resolve_parent_ok(target)) { say_err("E212: no such directory"); return; }
        char t[FS_PATH_MAX]; strlcpy(t, target, sizeof t);
        undo_clear(); open_file(t); cy = cx = top = left = 0; modified = 0;
    } else if (!strcmp(cmd, "set")) {
        if (!strcmp(arg, "nu") || !strcmp(arg, "number")) show_nu = 1;
        else if (!strcmp(arg, "nonu") || !strcmp(arg, "nonumber")) show_nu = 0;
        else say_err("E518: Unknown option (supported: nu, nonu)");
    } else if (!strcmp(cmd, "help") || !strcmp(cmd, "h")) help_screen();
    else { char m[120]; strlcpy(m, "E492: Not an editor command: ", sizeof m); cat_str(m, sizeof m, c); say_err(m); }
}

/* ---------------------------------------------------------------- normal mode */
static void enter_insert(void) { mode = M_INSERT; clamp(); msg[0] = 0; }

static void operate(int o, int n, int k) {
    /* o = 'd' 'y' 'c';  k = the motion key (or o itself for the linewise forms) */
    line_t *l = &ln[cy];
    if (k == o || k == 'j' || k == 'k' || k == 'G') {              /* linewise */
        int y1 = cy, y2 = cy;
        if (k == o) y2 = cy + n - 1;
        else if (k == 'j') y2 = cy + n;
        else if (k == 'k') y1 = cy - n;
        else if (k == 'G') { y2 = nln - 1; }
        if (y1 < 0) y1 = 0;
        if (y2 >= nln) y2 = nln - 1;
        reg_set_lines(y1, y2);
        if (o == 'y') { cy = y1; return; }
        undo_push();
        if (o == 'd') { delete_lines(y1, y2); }
        else {                                                      /* cc */
            for (int i = y2; i > y1; i--) line_remove(i);
            ln[y1].len = 0; cy = y1; cx = 0; modified = 1; enter_insert();
        }
        return;
    }
    int x1 = cx, x2 = cx;
    switch (k) {
    case 'w': {
        if (o == 'c' && l->len && cx < l->len && cls(l->s[cx])) {  /* cw behaves like ce */
            int c = cls(l->s[cx]); x2 = cx;
            for (int i = 0; i < n; i++) {
                while (x2 < l->len && cls(l->s[x2]) == c) x2++;
                if (i + 1 < n) { while (x2 < l->len && !cls(l->s[x2])) x2++; if (x2 < l->len) c = cls(l->s[x2]); }
            }
        } else {
            int y = cy, x = cx;
            for (int i = 0; i < n; i++) next_word(&y, &x);
            x2 = (y > cy) ? l->len : x;
            if (y == cy && x == cx) x2 = l->len;
        }
        break; }
    case 'b': { int y = cy, x = cx; for (int i = 0; i < n; i++) prev_word(&y, &x); x1 = (y < cy) ? 0 : x; x2 = cx; break; }
    case '$': x2 = l->len; break;
    case '0': x1 = 0; x2 = cx; break;
    case '^': x1 = first_nonblank(cy); if (x1 > cx) { x2 = x1; x1 = cx; } else x2 = cx; break;
    case 'h': x1 = cx - n; if (x1 < 0) x1 = 0; x2 = cx; break;
    case 'l': x2 = cx + n; if (x2 > l->len) x2 = l->len; break;
    default: return;
    }
    if (x2 < x1) { int t = x1; x1 = x2; x2 = t; }
    if (x2 > l->len) x2 = l->len;
    if (x1 == x2 && o != 'c') return;
    reg_set(l->s + x1, (usize)(x2 - x1), 0);
    if (o == 'y') { cx = x1; return; }
    undo_push();
    line_del(&ln[cy], x1, x2 - x1);
    cx = x1; modified = 1;
    if (o == 'c') enter_insert(); else clamp();
}

static void normal_key(int k) {
    int n;
    if (pend_r) {
        pend_r = 0;
        if (k >= 32 && k < 127 && cx < ln[cy].len) {
            undo_push();
            int m = cnt ? cnt : 1; cnt = 0;
            if (cx + m > ln[cy].len) m = ln[cy].len - cx;
            for (int i = 0; i < m; i++) ln[cy].s[cx + i] = (char)k;
            cx += m - 1; modified = 1;
        }
        return;
    }
    if (pend_Z) {
        pend_Z = 0;
        if (k == 'Z') { if (!*fname) say_err("E32: No file name"); else if (write_file(NULL, 1)) running = 0; }
        else if (k == 'Q') running = 0;
        return;
    }
    if (pend_g) {
        pend_g = 0;
        if (k == 'g') { cy = cnt ? cnt - 1 : 0; cnt = 0; clamp(); cx = first_nonblank(cy); }
        return;
    }
    if ((k >= '1' && k <= '9') || (k == '0' && cnt > 0)) { cnt = cnt * 10 + (k - '0'); if (cnt > 100000) cnt = 100000; return; }
    int had = cnt; n = cnt ? cnt : 1; cnt = 0;
    msg[0] = 0;

    if (op) {
        int o = op, total = (opcnt ? opcnt : 1) * n;
        op = 0; opcnt = 0;
        if (k == 27) return;
        operate(o, total, k);
        return;
    }
    switch (k) {
    case 27: break;
    case 'h': case KEY_LEFT:  cx -= n; break;
    case 'l': case KEY_RIGHT: cx += n; break;
    case 'j': case KEY_DOWN:  cy += n; break;
    case 'k': case KEY_UP:    cy -= n; break;
    case '0': case KEY_HOME:  cx = 0; break;
    case '^': cx = first_nonblank(cy); break;
    case '$': case KEY_END:   cx = ln[cy].len; break;
    case 'w': for (int i = 0; i < n; i++) next_word(&cy, &cx); break;
    case 'b': for (int i = 0; i < n; i++) prev_word(&cy, &cx); break;
    case 'G': cy = had ? had - 1 : nln - 1; clamp(); cx = first_nonblank(cy); break;
    case 'g': pend_g = 1; cnt = had; break;
    case KEY_CTRL | 'f': case KEY_PGDN: cy += text_rows() - 2; break;
    case KEY_CTRL | 'b': case KEY_PGUP: cy -= text_rows() - 2; break;
    case KEY_CTRL | 'd': cy += text_rows() / 2; break;
    case KEY_CTRL | 'u': cy -= text_rows() / 2; break;
    case 'i': undo_push(); enter_insert(); break;
    case 'a': undo_push(); if (ln[cy].len) cx++; enter_insert(); break;
    case 'I': undo_push(); cx = first_nonblank(cy); if (ln[cy].len && is_blank(ln[cy].s[ln[cy].len - 1]) && cx == ln[cy].len - 1 && is_blank(ln[cy].s[cx])) cx = ln[cy].len; enter_insert(); break;
    case 'A': undo_push(); cx = ln[cy].len; enter_insert(); break;
    case 'o': undo_push(); if (line_insert(cy + 1, "", 0)) { cy++; cx = 0; modified = 1; enter_insert(); } else say_err("E342: Out of memory!"); break;
    case 'O': undo_push(); if (line_insert(cy, "", 0)) { cx = 0; modified = 1; enter_insert(); } else say_err("E342: Out of memory!"); break;
    case 'x': case KEY_DEL:
        if (ln[cy].len) { undo_push(); int m = n; if (cx + m > ln[cy].len) m = ln[cy].len - cx; reg_set(ln[cy].s + cx, (usize)m, 0); line_del(&ln[cy], cx, m); modified = 1; }
        break;
    case 'X':
        if (cx > 0) { undo_push(); int m = n > cx ? cx : n; reg_set(ln[cy].s + cx - m, (usize)m, 0); line_del(&ln[cy], cx - m, m); cx -= m; modified = 1; }
        break;
    case 'D': operate('d', 1, '$'); break;
    case 'C': operate('c', 1, '$'); break;
    case 'd': case 'y': case 'c': op = k; opcnt = had; break;
    case 'p': for (int i = 0; i < n; i++) paste(1); break;
    case 'P': for (int i = 0; i < n; i++) paste(0); break;
    case 'u': for (int i = 0; i < n; i++) undo_redo(0); break;
    case KEY_CTRL | 'r': for (int i = 0; i < n; i++) undo_redo(1); break;
    case 'J': undo_push(); for (int i = 0; i < n; i++) join_lines(); break;
    case 'r': pend_r = 1; cnt = had; break;
    case '~':
        if (ln[cy].len) {
            undo_push();
            for (int i = 0; i < n && cx < ln[cy].len; i++, cx++) {
                char *c = &ln[cy].s[cx];
                if (*c >= 'a' && *c <= 'z') *c -= 32; else if (*c >= 'A' && *c <= 'Z') *c += 32;
            }
            modified = 1;
        }
        break;
    case 'Z': pend_Z = 1; break;
    case ':': mode = M_CMD; cmdpre = ':'; cmdlen = 0; cmdbuf[0] = 0; break;
    case '/': mode = M_CMD; cmdpre = '/'; cmdlen = 0; cmdbuf[0] = 0; break;
    case 'n': for (int i = 0; i < n; i++) if (!search(1)) break; break;
    case 'N': for (int i = 0; i < n; i++) if (!search(-1)) break; break;
    default: break;
    }
    clamp();
}

static void insert_key(int k) {
    switch (k) {
    case 27: mode = M_NORMAL; if (cx > 0) cx--; break;
    case '\n': ins_newline(); break;
    case 8: ins_backspace(); break;
    case KEY_DEL: ins_delete(); break;
    case '\t': ins_char('\t'); break;
    case KEY_LEFT:  if (cx > 0) cx--; break;
    case KEY_RIGHT: if (cx < ln[cy].len) cx++; break;
    case KEY_UP:    if (cy > 0) cy--; break;
    case KEY_DOWN:  if (cy + 1 < nln) cy++; break;
    case KEY_HOME:  cx = 0; break;
    case KEY_END:   cx = ln[cy].len; break;
    case KEY_PGUP:  cy -= text_rows() - 2; break;
    case KEY_PGDN:  cy += text_rows() - 2; break;
    default: if (k >= 32 && k < 127) ins_char((char)k); break;
    }
    clamp();
}

static void cmd_key(int k) {
    if (k == 27) { mode = M_NORMAL; return; }
    if (k == '\n') {
        mode = M_NORMAL;
        cmdbuf[cmdlen] = 0;
        if (cmdpre == ':') ex_command(cmdbuf);
        else { if (cmdlen) strlcpy(last_search, cmdbuf, sizeof last_search); search(1); }
        clamp();
        return;
    }
    if (k == 8) { if (cmdlen) cmdbuf[--cmdlen] = 0; else mode = M_NORMAL; return; }
    if (k >= 32 && k < 127 && cmdlen < (int)sizeof cmdbuf - 1) { cmdbuf[cmdlen++] = (char)k; cmdbuf[cmdlen] = 0; }
}

/* ---------------------------------------------------------------- entry point */
void vim_run(const char *path) {
    char tmp[FS_PATH_MAX]; tmp[0] = 0;
    if (path) {
        strlcpy(tmp, path, sizeof tmp);
        fnode *n = fs_find(tmp);
        if (n && n->is_dir) { cmd_err("vim", "Is a directory", path); return; }
        if (!n && !resolve_parent_ok(tmp)) { cmd_err("vim", "No such directory", path); return; }
    }
    if (term_rows < 5 || term_cols < 20) { cmd_err("vim", "terminal too small", NULL); return; }
    if (!term_save()) { cmd_err("vim", "out of memory", NULL); return; }

    ln = NULL; nln = capln = 0; reg = NULL; reg_len = 0; n_undo = n_redo = 0;
    cy = cx = top = left = 0; modified = 0; mode = M_NORMAL; running = 1;
    cnt = op = opcnt = pend_g = pend_r = pend_Z = 0; msg[0] = 0; fname[0] = 0;
    open_file(tmp);
    if (!nln) line_insert(0, "", 0);

    term_cursor_off();
    while (running) {
        draw();
        int k = key_wait();
        if (mode == M_NORMAL) normal_key(k);
        else if (mode == M_INSERT) insert_key(k);
        else cmd_key(k);
        if (!nln) line_insert(0, "", 0);
    }
    undo_clear();
    lines_free();
    kfree(reg); reg = NULL;
    term_restore();
}
