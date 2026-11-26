#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>
#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <poll.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAXLINE 4096     /* longer lines are split */
#define SCROLLBAR 14     /* scrollbar width in pixels */
#define FRAME_MS 16      /* minimum time between redraws */

/* ------------------------------------------------------------------ */
/* Options                                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *display_name;
    const char *geometry;
    const char *foreground;
    const char *background;
    const char *highlight;
    const char *font;
    const char *file;        /* "-" is standard input */
    const char *serial;
    const char *telnet;
    const char *serialmode;
    const char *flow;
    const char *rules_path;
    const char *cli_rules[32];
    int ncli_rules;
    long scrollback;
    int baud;
    bool norules;
    bool noansi;
} Options;

static Options opt;

static void usage(FILE *out)
{
    fprintf(out,
        "Usage: xlgr [file | -] [-serial device] [-baud rate] [-serialmode 8N1]\n"
        "            [-flow none|rtscts|xonxoff] [-telnet host:port]\n"
        "            [-scrollback lines] [-rules file] [-rule 'regex rule'] [-norules]\n"
        "            [-noansi]\n"
        "            [-font font] [-display display] [-geometry geometry]\n"
        "            [-fg color] [-bg color] [-hl color]\n"
        "View log messages from a file (followed as it grows), standard input, a\n"
        "serial port or a telnet connection, with regex highlighting rules.\n"
        "Keys: arrows/PgUp/PgDn/Home/End scroll, space pauses, / searches (n/N),\n"
        "f filters, Enter copies the cursor line, w writes, C clears, q quits.\n");
}

static const char *option_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) {
        fprintf(stderr, "xlgr: missing value for %s\n", argv[*i]);
        exit(EXIT_FAILURE);
    }
    return argv[++*i];
}

static long number_value(int argc, char **argv, int *i, long lo, long hi)
{
    const char *name = argv[*i];
    const char *value = option_value(argc, argv, i);
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (errno || *end || end == value || n < lo || n > hi) {
        fprintf(stderr, "xlgr: %s must be %ld-%ld\n", name + 1, lo, hi);
        exit(EXIT_FAILURE);
    }
    return n;
}

static void parse_options(int argc, char **argv)
{
    opt = (Options){ .foreground = "black", .background = "white",
                     .highlight = "gray85", .font = "fixed",
                     .scrollback = 100000, .baud = 115200,
                     .serialmode = "8N1" };
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (!strcmp(arg, "-help") || !strcmp(arg, "--help")) {
            usage(stdout);
            exit(EXIT_SUCCESS);
        } else if (!strcmp(arg, "-serial")) {
            opt.serial = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-baud")) {
            opt.baud = (int)number_value(argc, argv, &i, 50, 4000000);
        } else if (!strcmp(arg, "-serialmode")) {
            opt.serialmode = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-flow")) {
            opt.flow = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-telnet")) {
            opt.telnet = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-file")) {
            opt.file = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-scrollback")) {
            opt.scrollback = number_value(argc, argv, &i, 100, 100000000);
        } else if (!strcmp(arg, "-rules")) {
            opt.rules_path = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-rule")) {
            const char *rule = option_value(argc, argv, &i);
            if (opt.ncli_rules >= 32) {
                fprintf(stderr, "xlgr: too many -rule options\n");
                exit(EXIT_FAILURE);
            }
            opt.cli_rules[opt.ncli_rules++] = rule;
        } else if (!strcmp(arg, "-norules")) {
            opt.norules = true;
        } else if (!strcmp(arg, "-noansi")) {
            opt.noansi = true;
        } else if (!strcmp(arg, "-font")) {
            opt.font = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-display")) {
            opt.display_name = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-geometry")) {
            opt.geometry = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-fg") || !strcmp(arg, "-foreground")) {
            opt.foreground = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-bg") || !strcmp(arg, "-background")) {
            opt.background = option_value(argc, argv, &i);
        } else if (!strcmp(arg, "-hl") || !strcmp(arg, "-highlight")) {
            opt.highlight = option_value(argc, argv, &i);
        } else if (arg[0] != '-' || !strcmp(arg, "-")) {
            if (opt.file) {
                fprintf(stderr, "xlgr: only one file may be given\n");
                exit(EXIT_FAILURE);
            }
            opt.file = arg;
        } else {
            fprintf(stderr, "xlgr: unknown option: %s\n", arg);
            usage(stderr);
            exit(EXIT_FAILURE);
        }
    }
    int sources = (opt.file != NULL) + (opt.serial != NULL) + (opt.telnet != NULL);
    if (sources > 1) {
        fprintf(stderr, "xlgr: give only one of a file, -serial and -telnet\n");
        exit(EXIT_FAILURE);
    }
    if (sources == 0) {
        if (isatty(STDIN_FILENO)) {
            usage(stderr);
            exit(EXIT_FAILURE);
        }
        opt.file = "-";
    }
}

static long long monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* Line buffer: a ring of lines addressed by absolute line number     */
/* ------------------------------------------------------------------ */

/* ANSI SGR style. Colours are -1 (default), 0xRRGGBB, or ANSI_INDEXED | n
   for the 256-colour palette. */
#define ANSI_INDEXED 0x1000000
enum { A_BOLD = 1, A_UNDERLINE = 2, A_REVERSE = 4 };

typedef struct {
    int32_t fg, bg;
    uint8_t attrs;
} AnsiStyle;

typedef struct {
    uint32_t col;        /* first column the style applies to */
    AnsiStyle style;
} AnsiRun;

typedef struct {
    char *s;             /* NUL-terminated, one Latin-1 byte per column */
    uint32_t len, cap;
    AnsiRun *runs;       /* style changes, in column order; NULL if none */
    uint32_t nruns, rcap;
} Line;

static const AnsiStyle ansi_default = { -1, -1, 0 };

static struct {
    bool skip_lf;
    int esc;             /* 0 none, 1 after ESC, 2 inside CSI */
    int utf_need;
    unsigned utf_cp;
    char csi[64];        /* CSI parameter bytes */
    size_t csilen;
    AnsiStyle cur;       /* style applied to characters from now on */
} ing = { .cur = { -1, -1, 0 } };

static Line *ring;
static size_t ring_cap;
static int64_t base;     /* absolute number of the oldest line held */
static int64_t total;    /* absolute number of the next line */
static bool open_line;   /* the newest line is still being received */
static bool dirty;       /* the window needs redrawing */

/* Optional filter: absolute numbers of matching closed lines. */
static bool filter_on;
static regex_t filter_re;
static char filter_text[256];
static int64_t *fidx;
static size_t fcount, fcap, fstart;

static void die_oom(void)
{
    fprintf(stderr, "xlgr: out of memory\n");
    exit(EXIT_FAILURE);
}

static Line *line_at(int64_t n)
{
    return &ring[(size_t)(n % (int64_t)ring_cap)];
}

/* Smart case: ignore case unless the pattern contains an upper-case letter. */
static int compile_user(regex_t *re, const char *pattern, char *err, size_t errlen)
{
    int flags = REG_EXTENDED;
    bool upper = false;
    for (const char *p = pattern; *p; ++p)
        if (isupper((unsigned char)*p)) upper = true;
    if (!upper) flags |= REG_ICASE;
    int rc = regcomp(re, pattern, flags);
    if (rc) regerror(rc, re, err, errlen);
    return rc;
}

static void filter_push(int64_t n)
{
    if (fcount == fcap) {
        fcap = fcap ? fcap * 2 : 1024;
        fidx = realloc(fidx, fcap * sizeof(*fidx));
        if (!fidx) die_oom();
    }
    fidx[fcount++] = n;
}

static void new_line(void)
{
    if (total - base == (int64_t)ring_cap) {
        Line *old = line_at(base);
        free(old->s);
        free(old->runs);
        *old = (Line){ 0 };
        ++base;
    }
    Line *l = line_at(total);
    l->cap = 64;
    l->len = 0;
    l->runs = NULL;
    l->nruns = l->rcap = 0;
    l->s = malloc(l->cap + 1);
    if (!l->s) die_oom();
    l->s[0] = 0;
    ++total;
    open_line = true;
}

static void close_line(void)
{
    Line *l = line_at(total - 1);
    char *shrunk = realloc(l->s, l->len + 1);
    if (shrunk) l->s = shrunk;
    l->cap = l->len;
    open_line = false;
    if (filter_on && regexec(&filter_re, l->s, 0, NULL, 0) == 0)
        filter_push(total - 1);
    dirty = true;
}

static void append_char(unsigned char c)
{
    if (!open_line) new_line();
    Line *l = line_at(total - 1);
    if (l->len >= MAXLINE) {
        close_line();
        new_line();
        l = line_at(total - 1);
    }
    if (!opt.noansi) {
        AnsiStyle eff = l->nruns ? l->runs[l->nruns - 1].style : ansi_default;
        if (eff.fg != ing.cur.fg || eff.bg != ing.cur.bg || eff.attrs != ing.cur.attrs) {
            if (l->nruns == l->rcap) {
                l->rcap = l->rcap ? l->rcap * 2 : 4;
                l->runs = realloc(l->runs, l->rcap * sizeof(*l->runs));
                if (!l->runs) die_oom();
            }
            l->runs[l->nruns++] = (AnsiRun){ .col = l->len, .style = ing.cur };
        }
    }
    if (l->len == l->cap) {
        l->cap *= 2;
        l->s = realloc(l->s, l->cap + 1);
        if (!l->s) die_oom();
    }
    l->s[l->len++] = (char)c;
    l->s[l->len] = 0;
    dirty = true;
}

static void end_line(void)
{
    if (!open_line) new_line();
    close_line();
}

static void clear_buffer(void)
{
    for (int64_t n = base; n < total; ++n) {
        free(line_at(n)->s);
        free(line_at(n)->runs);
        *line_at(n) = (Line){ 0 };
    }
    base = total;
    open_line = false;
    fcount = fstart = 0;
    dirty = true;
}

/* Visible lines: all lines, or those passing the filter. */
static int64_t nvis(void)
{
    if (!filter_on) return total - base;
    while (fstart < fcount && fidx[fstart] < base) ++fstart;
    if (fstart > 65536 && fstart > fcount / 2) {
        memmove(fidx, fidx + fstart, (fcount - fstart) * sizeof(*fidx));
        fcount -= fstart;
        fstart = 0;
    }
    return (int64_t)(fcount - fstart);
}

/* Absolute number of visible line k; call nvis() first. */
static int64_t vabs(int64_t k)
{
    return filter_on ? fidx[fstart + (size_t)k] : base + k;
}

/* Number of visible lines before absolute line a; call nvis() first. */
static int64_t vlower(int64_t a)
{
    if (!filter_on) {
        int64_t k = a - base;
        return k < 0 ? 0 : k > total - base ? total - base : k;
    }
    size_t lo = fstart, hi = fcount;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (fidx[mid] < a) lo = mid + 1; else hi = mid;
    }
    return (int64_t)(lo - fstart);
}

static void rebuild_filter(void)
{
    fcount = fstart = 0;
    int64_t end = total - (open_line ? 1 : 0);
    for (int64_t n = base; n < end; ++n)
        if (regexec(&filter_re, line_at(n)->s, 0, NULL, 0) == 0)
            filter_push(n);
}

/* ------------------------------------------------------------------ */
/* Ingest: bytes -> lines (UTF-8, tabs, ANSI escapes, CR/LF)          */
/* ------------------------------------------------------------------ */



/* Apply the parameters of an SGR (CSI ... m) sequence to ing.cur. */
static void apply_sgr(const char *params)
{
    int v[32], n = 0;
    bool colon[32] = { false };      /* separator after v[k] was ':' */
    const char *p = params;
    for (;;) {
        int val = 0;
        while (isdigit((unsigned char)*p)) {
            if (val < 100000) val = val * 10 + (*p - '0');
            ++p;
        }
        if (n < 32) { colon[n] = *p == ':'; v[n++] = val; }
        if (*p == ';' || *p == ':') ++p; else break;
    }
    AnsiStyle *st = &ing.cur;
    for (int i = 0; i < n; ++i) {
        int c = v[i];
        if (c == 0) *st = ansi_default;
        else if (c == 1) st->attrs |= A_BOLD;
        else if (c == 4) st->attrs |= A_UNDERLINE;
        else if (c == 7) st->attrs |= A_REVERSE;
        else if (c == 22) st->attrs &= (uint8_t)~A_BOLD;
        else if (c == 24) st->attrs &= (uint8_t)~A_UNDERLINE;
        else if (c == 27) st->attrs &= (uint8_t)~A_REVERSE;
        else if (c >= 30 && c <= 37) st->fg = ANSI_INDEXED | (c - 30);
        else if (c >= 40 && c <= 47) st->bg = ANSI_INDEXED | (c - 40);
        else if (c >= 90 && c <= 97) st->fg = ANSI_INDEXED | (c - 90 + 8);
        else if (c >= 100 && c <= 107) st->bg = ANSI_INDEXED | (c - 100 + 8);
        else if (c == 39) st->fg = -1;
        else if (c == 49) st->bg = -1;
        else if (c == 38 || c == 48) {
            int32_t color = -2;
            if (i + 2 < n && v[i + 1] == 5) {
                color = ANSI_INDEXED | (v[i + 2] & 0xff);
                i += 2;
            } else if (i + 4 < n && v[i + 1] == 2) {
                /* 38:2:r:g:b, or 38:2:colourspace:r:g:b (colon form only). */
                int chain = 0;
                while (i + chain < n && colon[i + chain]) ++chain;
                int k = (chain >= 5 && i + 5 < n) ? 3 : 2;
                color = ((v[i + k] & 0xff) << 16) | ((v[i + k + 1] & 0xff) << 8) | (v[i + k + 2] & 0xff);
                i += k + 2;
            } else {
                i = n;
            }
            if (color != -2) { if (c == 38) st->fg = color; else st->bg = color; }
        }
    }
}

static void put_char(unsigned char c)
{
    if (c == '\t') {
        uint32_t col = open_line ? line_at(total - 1)->len : 0;
        do append_char(' '); while (++col % 8);
    } else if (c < 0x20 || c == 0x7f) {
        if (c) append_char('.');
    } else {
        append_char(c);
    }
}

static void ingest_byte(unsigned char c)
{
    if (ing.skip_lf) {
        ing.skip_lf = false;
        if (c == '\n') return;
    }
    if (ing.esc == 1) {
        ing.esc = (c == '[') ? 2 : 0;
        ing.csilen = 0;
        return;
    }
    if (ing.esc == 2) {
        if (c >= 0x40 && c <= 0x7e) {
            ing.esc = 0;
            if (c == 'm' && !opt.noansi) {
                ing.csi[ing.csilen] = 0;
                apply_sgr(ing.csi);
            }
        } else if (ing.csilen + 1 < sizeof ing.csi) {
            ing.csi[ing.csilen++] = (char)c;
        }
        return;
    }
    if (c >= 0x80) {
        if (ing.utf_need && (c & 0xC0) == 0x80) {
            ing.utf_cp = (ing.utf_cp << 6) | (c & 0x3f);
            if (--ing.utf_need == 0) append_char(ing.utf_cp < 256 ? (unsigned char)ing.utf_cp : '?');
            return;
        }
        if (ing.utf_need) { ing.utf_need = 0; append_char('.'); }
        if ((c & 0xE0) == 0xC0) { ing.utf_need = 1; ing.utf_cp = c & 0x1f; }
        else if ((c & 0xF0) == 0xE0) { ing.utf_need = 2; ing.utf_cp = c & 0x0f; }
        else if ((c & 0xF8) == 0xF0) { ing.utf_need = 3; ing.utf_cp = c & 0x07; }
        else append_char('.');
        return;
    }
    if (ing.utf_need) { ing.utf_need = 0; append_char('.'); }
    if (c == 0x1b) ing.esc = 1;
    else if (c == '\n') end_line();
    else if (c == '\r') { end_line(); ing.skip_lf = true; }
    else put_char(c);
}

/* ------------------------------------------------------------------ */
/* X state and colour palette                                         */
/* ------------------------------------------------------------------ */

static Display *dpy;
static int scr;
static Window win;
static Pixmap backbuf;
static GC gc;
static XFontStruct *font;
static int cw, lh;               /* character cell size */
static unsigned win_w, win_h;
static Colormap cmap;

typedef struct { char *name; XColor c; } PalEntry;
#define PAL_MAX 512
static PalEntry pal[PAL_MAX];
static int npal = 1;             /* entry 0 means "unset" */
static int ifg, ibg, ihl;        /* default palette indices */
static int isb_trough, isb_thumb, isearch_fg, isearch_bg;

static int pal_get(const char *name)
{
    for (int i = 1; i < npal; ++i)
        if (pal[i].name && !strcmp(pal[i].name, name)) return i;
    XColor c;
    if (npal >= PAL_MAX || !XParseColor(dpy, cmap, name, &c) ||
        !XAllocColor(dpy, cmap, &c))
        return -1;
    pal[npal].name = strdup(name);
    pal[npal].c = c;
    return npal++;
}

static int pal_required(const char *name)
{
    int i = pal_get(name);
    if (i < 0) {
        fprintf(stderr, "xlgr: unknown colour: %s\n", name);
        exit(EXIT_FAILURE);
    }
    return i;
}

/* Colour halfway between palette entries a and b. */
static int pal_blend(int a, int b)
{
    if (npal >= PAL_MAX) return a;
    XColor c = pal[a].c;
    c.red = (unsigned short)(((unsigned)pal[a].c.red + pal[b].c.red) / 2);
    c.green = (unsigned short)(((unsigned)pal[a].c.green + pal[b].c.green) / 2);
    c.blue = (unsigned short)(((unsigned)pal[a].c.blue + pal[b].c.blue) / 2);
    if (!XAllocColor(dpy, cmap, &c)) return a;
    pal[npal].name = NULL;
    pal[npal].c = c;
    return npal++;
}

/* ANSI colours: RGB for a 256-colour index. The first 16 are chosen to be
   readable on a light background, or are the xterm colours on a dark one. */
static bool dark_background;

static uint32_t ansi_rgb(int n)
{
    static const uint32_t light[16] = {
        0x000000, 0xcd0000, 0x008800, 0x907000, 0x0000ee, 0xcd00cd, 0x008888, 0x909090,
        0x555555, 0xff0000, 0x00b000, 0xb08800, 0x5c5cff, 0xff00ff, 0x00b0b0, 0xb0b0b0 };
    static const uint32_t dark[16] = {
        0x000000, 0xcd0000, 0x00cd00, 0xcdcd00, 0x5c5cff, 0xcd00cd, 0x00cdcd, 0xe5e5e5,
        0x7f7f7f, 0xff0000, 0x00ff00, 0xffff00, 0x5c5cff, 0xff00ff, 0x00ffff, 0xffffff };
    if (n < 16) return dark_background ? dark[n] : light[n];
    if (n < 232) {
        static const int level[6] = { 0, 95, 135, 175, 215, 255 };
        n -= 16;
        return (uint32_t)(level[n / 36] << 16 | level[n / 6 % 6] << 8 | level[n % 6]);
    }
    int g = 8 + (n - 232) * 10;
    return (uint32_t)(g << 16 | g << 8 | g);
}

/* Palette index for an ANSI colour value, or 0 for the default/unavailable. */
static int ansi_pal(int32_t color)
{
    static struct { int32_t key; int pi; } cache[1024];
    if (color < 0) return 0;
    uint32_t rgb = color & ANSI_INDEXED ? ansi_rgb(color & 0xff) : (uint32_t)color;
    size_t slot = (size_t)((color * 2654435761u) >> 22) & 1023;
    if (cache[slot].pi && cache[slot].key == color) return cache[slot].pi;
    char name[16];
    snprintf(name, sizeof name, "#%06x", rgb);
    int pi = pal_get(name);
    if (pi < 0) return 0;
    cache[slot].key = color;
    cache[slot].pi = pi;
    return pi;
}

/* ------------------------------------------------------------------ */
/* Highlighting rules                                                 */
/* ------------------------------------------------------------------ */

typedef struct {
    regex_t re;
    int group;
    int fg, bg;                  /* palette indices, 0 = unset */
    unsigned attrs;
    bool line;
} Rule;

static Rule *rules;
static size_t nrules;
static char rules_file[1024];
static time_t rules_mtime;
static char msg[256];            /* transient status message */

static void set_msg(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    dirty = true;
}

/* Parse one rule line. Returns 0 on success, 1 for blank/comment, -1 on
   error (described in err). */
static int parse_rule(const char *text, Rule *r, char *err, size_t errlen)
{
    const char *p = text;
    while (isspace((unsigned char)*p)) ++p;
    if (!*p || *p == '#') return 1;
    if (*p != '/') {
        snprintf(err, errlen, "rule must start with /");
        return -1;
    }
    const char *start = ++p;
    for (; *p; ++p) {
        if (*p == '\\' && p[1]) { ++p; continue; }
        if (*p == '/' && (!p[1] || isspace((unsigned char)p[1]) ||
                          (p[1] == 'i' && (!p[2] || isspace((unsigned char)p[2])))))
            break;
    }
    if (!*p) {
        snprintf(err, errlen, "missing closing /");
        return -1;
    }
    char *pattern = strndup(start, (size_t)(p - start));
    if (!pattern) die_oom();
    ++p;
    int flags = REG_EXTENDED;
    if (*p == 'i') { flags |= REG_ICASE; ++p; }

    *r = (Rule){ 0 };
    bool dim = false;
    int rc = -1;
    char fgname[96] = "", bgname[96] = "";
    while (*p) {
        while (isspace((unsigned char)*p)) ++p;
        if (!*p || *p == '#') break;
        const char *tok = p;
        while (*p && !isspace((unsigned char)*p)) ++p;
        size_t n = (size_t)(p - tok);
        char word[96];
        if (n >= sizeof word) n = sizeof word - 1;
        memcpy(word, tok, n);
        word[n] = 0;
        if (!strncmp(word, "fg=", 3)) snprintf(fgname, sizeof fgname, "%s", word + 3);
        else if (!strncmp(word, "bg=", 3)) snprintf(bgname, sizeof bgname, "%s", word + 3);
        else if (!strncmp(word, "group=", 6)) r->group = atoi(word + 6);
        else if (!strcmp(word, "bold")) r->attrs |= A_BOLD;
        else if (!strcmp(word, "underline")) r->attrs |= A_UNDERLINE;
        else if (!strcmp(word, "dim")) dim = true;
        else if (!strcmp(word, "line")) r->line = true;
        else {
            snprintf(err, errlen, "unknown attribute: %s", word);
            goto out;
        }
    }
    if (*fgname && (r->fg = pal_get(fgname)) < 0) {
        snprintf(err, errlen, "unknown colour: %s", fgname);
        goto out;
    }
    if (*bgname && (r->bg = pal_get(bgname)) < 0) {
        snprintf(err, errlen, "unknown colour: %s", bgname);
        goto out;
    }
    if (dim) r->fg = pal_blend(r->fg ? r->fg : ifg, ibg);
    int re = regcomp(&r->re, pattern, flags);
    if (re) {
        regerror(re, &r->re, err, errlen);
        goto out;
    }
    if (r->group < 0 || (size_t)r->group > r->re.re_nsub) {
        snprintf(err, errlen, "group=%d but the regex has %zu groups", r->group, r->re.re_nsub);
        regfree(&r->re);
        goto out;
    }
    rc = 0;
out:
    free(pattern);
    return rc;
}

static void add_rule_line(const char *text, const char *where)
{
    Rule r;
    char err[200];
    int rc = parse_rule(text, &r, err, sizeof err);
    if (rc == 1) return;
    if (rc < 0) {
        fprintf(stderr, "xlgr: %s: %s\n", where, err);
        set_msg("%s: %s", where, err);
        return;
    }
    rules = realloc(rules, (nrules + 1) * sizeof(*rules));
    if (!rules) die_oom();
    rules[nrules++] = r;
}

static void free_rules(void)
{
    for (size_t i = 0; i < nrules; ++i) regfree(&rules[i].re);
    free(rules);
    rules = NULL;
    nrules = 0;
}

static const char *default_rules[] = {
    "/(error|fatal|panic|fail(ed|ure)?)/i fg=#c00000 bold",
    "/warn(ing)?/i fg=#b06000",
    "/\\<0x[0-9a-f]+\\>/i fg=#0000b0",
};

static bool find_rules_file(void)
{
    if (opt.rules_path) {
        snprintf(rules_file, sizeof rules_file, "%s", opt.rules_path);
        return true;
    }
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    char path[1024];
    if (xdg && *xdg) {
        snprintf(path, sizeof path, "%s/xlgr/rules", xdg);
        if (access(path, R_OK) == 0) { snprintf(rules_file, sizeof rules_file, "%s", path); return true; }
    }
    if (home) {
        snprintf(path, sizeof path, "%s/.config/xlgr/rules", home);
        if (access(path, R_OK) == 0) { snprintf(rules_file, sizeof rules_file, "%s", path); return true; }
        snprintf(path, sizeof path, "%s/.xlgr.rules", home);
        if (access(path, R_OK) == 0) { snprintf(rules_file, sizeof rules_file, "%s", path); return true; }
    }
    return false;
}

static void load_rules(bool startup)
{
    free_rules();
    if (opt.norules) return;
    bool have_file = rules_file[0] != 0;
    if (have_file) {
        FILE *f = fopen(rules_file, "r");
        if (!f) {
            fprintf(stderr, "xlgr: %s: %s\n", rules_file, strerror(errno));
            if (startup && opt.rules_path) exit(EXIT_FAILURE);
        } else {
            struct stat st;
            if (fstat(fileno(f), &st) == 0) rules_mtime = st.st_mtime;
            char line[1024];
            int n = 0;
            while (fgets(line, sizeof line, f)) {
                char where[1100];
                const char *slash = strrchr(rules_file, '/');
                snprintf(where, sizeof where, "%s line %d", slash ? slash + 1 : rules_file, ++n);
                add_rule_line(line, where);
            }
            fclose(f);
        }
    }
    for (int i = 0; i < opt.ncli_rules; ++i) add_rule_line(opt.cli_rules[i], "-rule");
    if (!have_file && opt.ncli_rules == 0)
        for (size_t i = 0; i < sizeof default_rules / sizeof *default_rules; ++i)
            add_rule_line(default_rules[i], "built-in rule");
    dirty = true;
}

static void check_rules_file(void)
{
    struct stat st;
    if (!rules_file[0] || stat(rules_file, &st) < 0 || st.st_mtime == rules_mtime) return;
    msg[0] = 0;
    load_rules(false);
    if (!msg[0]) set_msg("reloaded %s (%zu rules)", rules_file, nrules);
}

/* ------------------------------------------------------------------ */
/* Sources: file, standard input, serial port, telnet                 */
/* ------------------------------------------------------------------ */

typedef enum { SRC_FILE, SRC_STDIN, SRC_SERIAL, SRC_TELNET } SrcKind;
typedef enum { ST_OPEN, ST_WAIT, ST_CONNECTING, ST_EOF } SrcState;

typedef struct {
    SrcKind kind;
    SrcState state;
    int fd;
    bool idle;                   /* file at end: re-check on a timer */
    long long next_at;           /* retry or re-check time */
    off_t pos;
    ino_t ino;
    char host[256], port[32];
    int telnet_state;            /* 0 data, 1 IAC, 2 option, 3 SB, 4 SB+IAC */
    unsigned char telnet_cmd;
    char label[300];
} Source;

static Source src;

static speed_t baud_constant(int baud)
{
    static const struct { int rate; speed_t c; } table[] = {
        { 50, B50 }, { 75, B75 }, { 110, B110 }, { 134, B134 }, { 150, B150 },
        { 200, B200 }, { 300, B300 }, { 600, B600 }, { 1200, B1200 },
        { 1800, B1800 }, { 2400, B2400 }, { 4800, B4800 }, { 9600, B9600 },
        { 19200, B19200 }, { 38400, B38400 }, { 57600, B57600 },
        { 115200, B115200 }, { 230400, B230400 }, { 460800, B460800 },
        { 500000, B500000 }, { 576000, B576000 }, { 921600, B921600 },
        { 1000000, B1000000 }, { 1500000, B1500000 }, { 2000000, B2000000 },
        { 2500000, B2500000 }, { 3000000, B3000000 }, { 3500000, B3500000 },
        { 4000000, B4000000 },
    };
    for (size_t i = 0; i < sizeof table / sizeof *table; ++i)
        if (table[i].rate == baud) return table[i].c;
    return B0;
}

static bool configure_serial(int fd)
{
    struct termios t;
    if (tcgetattr(fd, &t) < 0) return false;
    cfmakeraw(&t);
    speed_t speed = baud_constant(opt.baud);
    if (speed == B0) {
        fprintf(stderr, "xlgr: unsupported baud rate %d\n", opt.baud);
        exit(EXIT_FAILURE);
    }
    cfsetispeed(&t, speed);
    cfsetospeed(&t, speed);
    const char *m = opt.serialmode;
    if (strlen(m) != 3 || m[0] < '5' || m[0] > '8' || !strchr("NEOneo", m[1]) ||
        (m[2] != '1' && m[2] != '2')) {
        fprintf(stderr, "xlgr: serialmode must look like 8N1\n");
        exit(EXIT_FAILURE);
    }
    t.c_cflag &= ~(tcflag_t)(CSIZE | PARENB | PARODD | CSTOPB | CRTSCTS);
    t.c_cflag |= m[0] == '5' ? CS5 : m[0] == '6' ? CS6 : m[0] == '7' ? CS7 : CS8;
    if (toupper((unsigned char)m[1]) == 'E') t.c_cflag |= PARENB;
    if (toupper((unsigned char)m[1]) == 'O') t.c_cflag |= PARENB | PARODD;
    if (m[2] == '2') t.c_cflag |= CSTOPB;
    t.c_cflag |= CLOCAL | CREAD;
    if (opt.flow && !strcmp(opt.flow, "rtscts")) t.c_cflag |= CRTSCTS;
    if (opt.flow && !strcmp(opt.flow, "xonxoff")) t.c_iflag |= IXON | IXOFF;
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    return tcsetattr(fd, TCSANOW, &t) == 0;
}

static void source_close(Source *s)
{
    if (s->fd >= 0 && s->kind != SRC_STDIN) close(s->fd);
    s->fd = -1;
}

static void source_wait(Source *s, long long now, int ms)
{
    source_close(s);
    s->state = ST_WAIT;
    s->next_at = now + ms;
    dirty = true;
}

static void source_open(Source *s, long long now)
{
    s->telnet_state = 0;
    switch (s->kind) {
    case SRC_STDIN:
        s->fd = STDIN_FILENO;
        s->state = ST_OPEN;
        break;
    case SRC_FILE: {
        int fd = open(opt.file, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        struct stat st;
        if (fd < 0 || fstat(fd, &st) < 0) {
            if (fd >= 0) close(fd);
            source_wait(s, now, 1000);
            return;
        }
        s->fd = fd;
        s->ino = st.st_ino;
        s->pos = 0;
        s->idle = false;
        s->state = ST_OPEN;
        /* A pipe or device does not "end"; only regular files are followed. */
        break;
    }
    case SRC_SERIAL: {
        int fd = open(opt.serial, O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) { source_wait(s, now, 1000); return; }
        if (!configure_serial(fd)) {
            close(fd);
            source_wait(s, now, 1000);
            return;
        }
        s->fd = fd;
        s->state = ST_OPEN;
        break;
    }
    case SRC_TELNET: {
        struct addrinfo hints = { .ai_socktype = SOCK_STREAM }, *res = NULL;
        if (getaddrinfo(s->host, s->port, &hints, &res) != 0 || !res) {
            source_wait(s, now, 2000);
            return;
        }
        int fd = socket(res->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) { freeaddrinfo(res); source_wait(s, now, 2000); return; }
        int rc = connect(fd, res->ai_addr, res->ai_addrlen);
        freeaddrinfo(res);
        if (rc < 0 && errno != EINPROGRESS) {
            close(fd);
            source_wait(s, now, 2000);
            return;
        }
        s->fd = fd;
        s->state = rc == 0 ? ST_OPEN : ST_CONNECTING;
        break;
    }
    }
    dirty = true;
}

/* Strip telnet protocol bytes, refusing every option but ECHO and SGA. */
static void telnet_byte(Source *s, unsigned char c)
{
    enum { IAC = 255, DONT = 254, DO = 253, WONT = 252, WILL = 251, SB = 250, SE = 240 };
    switch (s->telnet_state) {
    case 0:
        if (c == IAC) s->telnet_state = 1; else ingest_byte(c);
        break;
    case 1:
        if (c == IAC) { s->telnet_state = 0; ingest_byte(IAC); }
        else if (c >= WILL && c <= DONT) { s->telnet_cmd = c; s->telnet_state = 2; }
        else if (c == SB) s->telnet_state = 3;
        else s->telnet_state = 0;
        break;
    case 2: {
        unsigned char reply[3] = { IAC, 0, c };
        bool accept = c == 1 || c == 3;
        if (s->telnet_cmd == WILL) reply[1] = accept ? DO : DONT;
        else if (s->telnet_cmd == DO) reply[1] = WONT;
        if (reply[1] && write(s->fd, reply, 3) < 0) { /* best effort */ }
        s->telnet_state = 0;
        break;
    }
    case 3:
        if (c == IAC) s->telnet_state = 4;
        break;
    case 4:
        s->telnet_state = (c == SE) ? 0 : 3;
        break;
    }
}

static void source_read(Source *s, long long now)
{
    unsigned char buf[65536];
    ssize_t n = read(s->fd, buf, sizeof buf);
    if (n > 0) {
        s->pos += n;
        if (s->kind == SRC_TELNET)
            for (ssize_t i = 0; i < n; ++i) telnet_byte(s, buf[i]);
        else
            for (ssize_t i = 0; i < n; ++i) ingest_byte(buf[i]);
        return;
    }
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    switch (s->kind) {
    case SRC_FILE:
        if (n == 0) { s->idle = true; s->next_at = now + 200; return; }
        source_close(s);
        s->state = ST_EOF;
        break;
    case SRC_STDIN:
        s->state = ST_EOF;
        break;
    case SRC_SERIAL:
        source_wait(s, now, 1000);
        break;
    case SRC_TELNET:
        source_wait(s, now, 2000);
        break;
    }
    if (open_line) end_line();
    dirty = true;
}

/* Timer work: retries, file rotation and truncation. */
static void source_tick(Source *s, long long now)
{
    if (s->state == ST_WAIT && now >= s->next_at) {
        source_open(s, now);
    } else if (s->kind == SRC_FILE && s->state == ST_OPEN && s->idle && now >= s->next_at) {
        struct stat st;
        if (stat(opt.file, &st) == 0 && st.st_ino != s->ino) {
            source_close(s);
            if (open_line) end_line();
            source_open(s, now);
            if (s->state == ST_OPEN) set_msg("file replaced; reopened");
        } else {
            if (stat(opt.file, &st) == 0 && st.st_size < s->pos) {
                lseek(s->fd, 0, SEEK_SET);
                s->pos = 0;
                if (open_line) end_line();
                set_msg("file truncated");
            }
            s->idle = false;
        }
    }
}

static void source_connect_check(Source *s, long long now)
{
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err)
        source_wait(s, now, 2000);
    else
        s->state = ST_OPEN;
    dirty = true;
}

static void source_init(Source *s)
{
    *s = (Source){ .fd = -1 };
    if (opt.serial) {
        s->kind = SRC_SERIAL;
        snprintf(s->label, sizeof s->label, "%s %d %s", opt.serial, opt.baud, opt.serialmode);
    } else if (opt.telnet) {
        s->kind = SRC_TELNET;
        const char *colon = strrchr(opt.telnet, ':');
        if (!colon || colon == opt.telnet || !colon[1] ||
            (size_t)(colon - opt.telnet) >= sizeof s->host || strlen(colon + 1) >= sizeof s->port) {
            fprintf(stderr, "xlgr: -telnet needs host:port\n");
            exit(EXIT_FAILURE);
        }
        snprintf(s->host, sizeof s->host, "%.*s", (int)(colon - opt.telnet), opt.telnet);
        snprintf(s->port, sizeof s->port, "%s", colon + 1);
        snprintf(s->label, sizeof s->label, "%s", opt.telnet);
    } else if (!strcmp(opt.file, "-")) {
        s->kind = SRC_STDIN;
        snprintf(s->label, sizeof s->label, "stdin");
    } else {
        s->kind = SRC_FILE;
        snprintf(s->label, sizeof s->label, "%s", opt.file);
    }
}

static const char *source_status(const Source *s)
{
    switch (s->state) {
    case ST_WAIT: return s->kind == SRC_FILE ? "(waiting for file)" : "DISCONNECTED";
    case ST_CONNECTING: return "connecting";
    case ST_EOF: return "EOF";
    default: return s->kind == SRC_SERIAL || s->kind == SRC_TELNET ? "connected" : "";
    }
}

/* ------------------------------------------------------------------ */
/* View state                                                         */
/* ------------------------------------------------------------------ */

static bool follow = true;       /* stick to the newest line */
static int64_t top_abs;          /* absolute number of the first row's line */
static bool cursor_on;
static int64_t cur_abs;
static int xoff;                 /* horizontal scroll in columns */

static bool search_on;
static regex_t search_re;
static char search_text[256];

enum { PM_NONE, PM_SEARCH, PM_FILTER, PM_WRITE };
static int prompt_mode;
static char pbuf[256];
static size_t plen;

static int64_t text_rows(void)
{
    int64_t rows = ((int64_t)win_h - (lh + 4)) / lh;
    return rows < 1 ? 1 : rows;
}

static int64_t max_top(void)
{
    int64_t n = nvis(), rows = text_rows();
    return n > rows ? n - rows : 0;
}

/* Index of the first visible row, keeping top_abs in step. */
static int64_t sync_view(void)
{
    int64_t n = nvis(), maxtop = max_top(), vt;
    if (follow) vt = maxtop;
    else {
        vt = vlower(top_abs);
        if (vt > maxtop) vt = maxtop;
    }
    if (n > 0) top_abs = vabs(vt);
    return vt;
}

static void scroll_to(int64_t vt, bool scrolled_up)
{
    int64_t n = nvis(), maxtop = max_top();
    if (vt < 0) vt = 0;
    if (vt >= maxtop) {
        vt = maxtop;
        if (!scrolled_up) { follow = true; cursor_on = false; }
    }
    if (scrolled_up && vt < maxtop) follow = false;
    if (n > 0) top_abs = vabs(vt);
    dirty = true;
}

static void scroll_by(int64_t d)
{
    int64_t vt = sync_view();
    scroll_to(vt + d, d < 0);
}

/* Move the cursor to visible line vc and scroll it into view. */
static void cursor_to(int64_t vc)
{
    int64_t n = nvis();
    if (n == 0) return;
    int64_t vt = sync_view(), rows = text_rows();
    if (vc < 0) vc = 0;
    if (vc >= n) vc = n - 1;
    cursor_on = true;
    cur_abs = vabs(vc);
    if (vc < vt) vt = vc;
    else if (vc >= vt + rows) vt = vc - rows + 1;
    follow = false;
    top_abs = vabs(vt);
    dirty = true;
}

static void cursor_move(int64_t d)
{
    int64_t n = nvis();
    if (n == 0) return;
    int64_t vt = sync_view(), rows = text_rows();
    int64_t vc;
    if (!cursor_on) {
        int64_t last = vt + rows - 1 < n ? vt + rows - 1 : n - 1;
        vc = d < 0 ? last : vt;
    } else {
        vc = vlower(cur_abs);
    }
    cursor_to(vc + d);
}

/* ------------------------------------------------------------------ */
/* Searching                                                          */
/* ------------------------------------------------------------------ */

/* Find the first match at or after visible line start in direction dir,
   wrapping around, and move the cursor to it. */
static void search_find(int64_t start, int dir)
{
    int64_t n = nvis();
    if (n == 0) return;
    int64_t vt = sync_view(), rows = text_rows();
    for (int64_t i = 0; i < n; ++i) {
        int64_t k = start + dir * i;
        bool wrapped = k < 0 || k >= n;
        k = ((k % n) + n) % n;
        if (regexec(&search_re, line_at(vabs(k))->s, 0, NULL, 0) == 0) {
            cursor_on = true;
            cur_abs = vabs(k);
            follow = false;
            if (k < vt || k >= vt + rows) {
                int64_t nt = k - rows / 2, maxtop = max_top();
                top_abs = vabs(nt < 0 ? 0 : nt > maxtop ? maxtop : nt);
            }
            if (wrapped) set_msg("search wrapped");
            dirty = true;
            return;
        }
    }
    set_msg("not found: %s", search_text);
}

static void search_next(int dir)
{
    if (!search_on) { set_msg("no search; press /"); return; }
    int64_t vt = sync_view();
    int64_t from = cursor_on ? vlower(cur_abs) : dir > 0 ? vt - 1 : vt + text_rows();
    search_find(from + dir, dir);
}

static void apply_search(const char *text)
{
    if (search_on) { regfree(&search_re); search_on = false; }
    search_text[0] = 0;
    dirty = true;
    if (!*text) return;
    char err[200];
    if (compile_user(&search_re, text, err, sizeof err)) {
        set_msg("regex: %s", err);
        return;
    }
    search_on = true;
    snprintf(search_text, sizeof search_text, "%s", text);
    search_find(cursor_on ? vlower(cur_abs) : sync_view(), 1);
}

static void apply_filter(const char *text)
{
    int64_t anchor = cursor_on ? cur_abs : top_abs;
    if (filter_on) { regfree(&filter_re); filter_on = false; }
    filter_text[0] = 0;
    if (*text) {
        char err[200];
        if (compile_user(&filter_re, text, err, sizeof err)) {
            set_msg("regex: %s", err);
        } else {
            filter_on = true;
            snprintf(filter_text, sizeof filter_text, "%s", text);
            rebuild_filter();
        }
    }
    /* Keep the same place in the buffer: anchors are absolute line numbers. */
    top_abs = anchor;
    if (cursor_on) cur_abs = anchor;
    dirty = true;
}

/* ------------------------------------------------------------------ */
/* Clipboard                                                          */
/* ------------------------------------------------------------------ */

static char *clip_text;
static Atom a_clipboard, a_utf8, a_targets;

static void copy_line(void)
{
    int64_t n = nvis();
    if (n == 0) return;
    int64_t a = cursor_on ? vabs(vlower(cur_abs) < n ? vlower(cur_abs) : n - 1) : vabs(n - 1);
    free(clip_text);
    clip_text = strdup(line_at(a)->s);
    if (!clip_text) die_oom();
    XSetSelectionOwner(dpy, XA_PRIMARY, win, CurrentTime);
    XSetSelectionOwner(dpy, a_clipboard, win, CurrentTime);
    set_msg("copied line %lld", (long long)(a + 1));
}

static void answer_selection(const XSelectionRequestEvent *e)
{
    XSelectionEvent r = { .type = SelectionNotify, .display = e->display,
                          .requestor = e->requestor, .selection = e->selection,
                          .target = e->target, .property = None, .time = e->time };
    Atom prop = e->property ? e->property : e->target;
    if (clip_text && e->target == a_targets) {
        Atom targets[] = { a_targets, XA_STRING, a_utf8 };
        XChangeProperty(dpy, e->requestor, prop, XA_ATOM, 32, PropModeReplace,
                        (unsigned char *)targets, 3);
        r.property = prop;
    } else if (clip_text && e->target == XA_STRING) {
        XChangeProperty(dpy, e->requestor, prop, XA_STRING, 8, PropModeReplace,
                        (unsigned char *)clip_text, (int)strlen(clip_text));
        r.property = prop;
    } else if (clip_text && e->target == a_utf8) {
        size_t len = strlen(clip_text);
        unsigned char *u = malloc(len * 2 + 1), *q = u;
        if (u) {
            for (size_t i = 0; i < len; ++i) {
                unsigned char c = (unsigned char)clip_text[i];
                if (c < 0x80) *q++ = c;
                else { *q++ = 0xC0 | (c >> 6); *q++ = 0x80 | (c & 0x3f); }
            }
            XChangeProperty(dpy, e->requestor, prop, a_utf8, 8, PropModeReplace, u, (int)(q - u));
            free(u);
            r.property = prop;
        }
    }
    XSendEvent(dpy, e->requestor, False, 0, (XEvent *)&r);
}

static void write_buffer(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) { set_msg("%s: %s", path, strerror(errno)); return; }
    int64_t n = nvis();
    for (int64_t k = 0; k < n; ++k) fprintf(f, "%s\n", line_at(vabs(k))->s);
    if (fclose(f)) set_msg("%s: %s", path, strerror(errno));
    else set_msg("wrote %lld lines to %s", (long long)n, path);
}

/* ------------------------------------------------------------------ */
/* Drawing                                                            */
/* ------------------------------------------------------------------ */

static void fill(int ci, int x, int y, int w, int h)
{
    XSetForeground(dpy, gc, pal[ci].c.pixel);
    XFillRectangle(dpy, backbuf, gc, x, y, (unsigned)w, (unsigned)h);
}

static void draw_row(const Line *l, int y, bool cursor)
{
    static uint16_t fgi[MAXLINE], bgi[MAXLINE];
    static uint8_t at[MAXLINE];
    int len = (int)l->len;
    memset(fgi, 0, (size_t)len * sizeof *fgi);
    memset(bgi, 0, (size_t)len * sizeof *bgi);
    memset(at, 0, (size_t)len);
    int row_fg = 0, row_bg = 0;
    unsigned row_at = 0;

    for (size_t ri = 0; ri < nrules; ++ri) {
        const Rule *r = &rules[ri];
        regmatch_t pm[10];
        if (r->line) {
            if (regexec(&r->re, l->s, 0, NULL, 0) == 0) {
                if (!row_fg) row_fg = r->fg;
                if (!row_bg) row_bg = r->bg;
                row_at |= r->attrs;
            }
            continue;
        }
        size_t nm = (size_t)r->group + 1 > 10 ? 10 : (size_t)r->group + 1;
        for (int off = 0; off <= len;) {
            pm[0].rm_so = off;
            pm[0].rm_eo = len;
            if (regexec(&r->re, l->s, nm, pm, REG_STARTEND)) break;
            int so = (int)pm[r->group].rm_so, eo = (int)pm[r->group].rm_eo;
            if (so >= 0)
                for (int i = so; i < eo; ++i) {
                    if (!fgi[i]) fgi[i] = (uint16_t)r->fg;
                    if (!bgi[i]) bgi[i] = (uint16_t)r->bg;
                    at[i] |= (uint8_t)r->attrs;
                }
            off = (int)pm[0].rm_eo + (pm[0].rm_eo == pm[0].rm_so);
        }
    }
    /* ANSI styling fills whatever the rules left unset. */
    for (uint32_t ri = 0; ri < l->nruns; ++ri) {
        const AnsiRun *run = &l->runs[ri];
        int from = (int)run->col, to = ri + 1 < l->nruns ? (int)l->runs[ri + 1].col : len;
        int f = ansi_pal(run->style.fg), b = ansi_pal(run->style.bg);
        if (run->style.attrs & A_REVERSE) {
            int nf = b ? b : ibg, nb = f ? f : ifg;
            f = nf;
            b = nb;
        }
        for (int i = from; i < to && i < len; ++i) {
            if (!fgi[i]) fgi[i] = (uint16_t)f;
            if (!bgi[i]) bgi[i] = (uint16_t)b;
            at[i] |= run->style.attrs & (A_BOLD | A_UNDERLINE);
        }
    }
    if (search_on) {
        for (int off = 0; off <= len;) {
            regmatch_t pm[1] = { { off, len } };
            if (regexec(&search_re, l->s, 1, pm, REG_STARTEND)) break;
            for (int i = (int)pm[0].rm_so; i < (int)pm[0].rm_eo; ++i) {
                fgi[i] = (uint16_t)isearch_fg;
                bgi[i] = (uint16_t)isearch_bg;
            }
            off = (int)pm[0].rm_eo + (pm[0].rm_eo == pm[0].rm_so);
        }
    }

    int text_w = (int)win_w - SCROLLBAR;
    int base_bg = cursor ? ihl : row_bg ? row_bg : ibg;
    fill(base_bg, 0, y, text_w, lh);
    int cols = text_w / cw + 1;
    int end = len < xoff + cols ? len : xoff + cols;
    for (int i = xoff; i < end;) {
        int f = fgi[i] ? fgi[i] : row_fg ? row_fg : ifg;
        int b = bgi[i] ? bgi[i] : 0;
        unsigned a = at[i] | row_at;
        int j = i + 1;
        while (j < end && (fgi[j] ? fgi[j] : row_fg ? row_fg : ifg) == f &&
               bgi[j] == bgi[i] && (at[j] | row_at) == a)
            ++j;
        int x = (i - xoff) * cw;
        if (b && b != base_bg) fill(b, x, y, (j - i) * cw, lh);
        XSetForeground(dpy, gc, pal[f].c.pixel);
        XDrawString(dpy, backbuf, gc, x, y + font->ascent, l->s + i, j - i);
        if (a & A_BOLD) XDrawString(dpy, backbuf, gc, x + 1, y + font->ascent, l->s + i, j - i);
        if (a & A_UNDERLINE) XDrawLine(dpy, backbuf, gc, x, y + font->ascent + 1, x + (j - i) * cw - 1, y + font->ascent + 1);
        i = j;
    }
}

static void draw_status(void)
{
    int y = (int)win_h - (lh + 4);
    fill(ihl, 0, y, (int)win_w, lh + 4);
    XSetForeground(dpy, gc, pal[ifg].c.pixel);
    XDrawLine(dpy, backbuf, gc, 0, y, (int)win_w, y);
    char left[1024], right[128];
    if (prompt_mode) {
        const char *label = prompt_mode == PM_SEARCH ? "/" : prompt_mode == PM_FILTER ? "filter: " : "write to: ";
        snprintf(left, sizeof left, "%s%s_", label, pbuf);
    } else {
        int64_t n = nvis(), all = total - base;
        char counts[64];
        if (filter_on) snprintf(counts, sizeof counts, "%lld/%lld lines", (long long)n, (long long)all);
        else snprintf(counts, sizeof counts, "%lld lines", (long long)all);
        const char *st = source_status(&src);
        char rest[700];
        int k = snprintf(rest, sizeof rest, "%s%s \xb7 %s", *st ? " " : "", st, counts);
        if (search_on) k += snprintf(rest + k, sizeof rest - (size_t)k, " \xb7 /%s", search_text);
        if (filter_on) k += snprintf(rest + k, sizeof rest - (size_t)k, " \xb7 filter %s", filter_text);
        if (msg[0]) snprintf(rest + k, sizeof rest - (size_t)k, " \xb7 %s", msg);
        /* Trim the source name from the left so the state stays visible. */
        int room = ((int)win_w - (int)strlen("FOLLOW") * cw - 12) / cw - (int)strlen(rest);
        size_t ln = strlen(src.label);
        if (room < 8) snprintf(left, sizeof left, "%s", rest + (rest[0] == ' ' ? 1 : 0));
        else if ((int)ln > room) snprintf(left, sizeof left, "...%s%s", src.label + ln - (size_t)(room - 3), rest);
        else snprintf(left, sizeof left, "%s%s", src.label, rest);
    }
    snprintf(right, sizeof right, "%s", follow ? "FOLLOW" : "PAUSED");
    int rw = (int)strlen(right) * cw;
    int avail = ((int)win_w - rw - 12) / cw;
    int ll = (int)strlen(left);
    const char *ls = left;
    if (prompt_mode && ll > avail) { ls = left + (ll - avail); ll = avail; }
    if (ll > avail) ll = avail > 0 ? avail : 0;
    XSetForeground(dpy, gc, pal[ifg].c.pixel);
    XDrawString(dpy, backbuf, gc, 4, y + 2 + font->ascent, ls, ll);
    XDrawString(dpy, backbuf, gc, (int)win_w - rw - 4, y + 2 + font->ascent, right, (int)strlen(right));
}

/* Thumb geometry in pixels for a scrollbar of height h. */
static void thumb_geometry(int h, int *ty, int *th)
{
    int64_t n = nvis(), rows = text_rows(), vt = sync_view();
    if (n <= rows) { *ty = 0; *th = h; return; }
    int t = (int)((int64_t)h * rows / n);
    if (t < 20) t = 20;
    if (t > h) t = h;
    *th = t;
    *ty = (int)((int64_t)(h - t) * vt / (n - rows));
}

static void draw(void)
{
    dirty = false;
    int text_w = (int)win_w - SCROLLBAR;
    int text_h = (int)win_h - (lh + 4);
    fill(ibg, 0, 0, (int)win_w, (int)win_h);
    int64_t n = nvis(), vt = sync_view(), rows = text_rows();
    int64_t cur_v = cursor_on ? vlower(cur_abs) : -1;
    for (int64_t k = 0; k < rows && vt + k < n; ++k) {
        const Line *l = line_at(vabs(vt + k));
        draw_row(l, (int)k * lh, vt + k == cur_v);
    }
    fill(isb_trough, text_w, 0, SCROLLBAR, text_h);
    int ty, th;
    thumb_geometry(text_h, &ty, &th);
    fill(isb_thumb, text_w + 2, ty + 1, SCROLLBAR - 4, th > 2 ? th - 2 : th);
    draw_status();
    XCopyArea(dpy, backbuf, win, gc, 0, 0, win_w, win_h, 0, 0);
    XFlush(dpy);
}

static void make_backbuf(void)
{
    if (backbuf) XFreePixmap(dpy, backbuf);
    backbuf = XCreatePixmap(dpy, win, win_w, win_h, (unsigned)DefaultDepth(dpy, scr));
}

/* ------------------------------------------------------------------ */
/* Icon                                                               */
/* ------------------------------------------------------------------ */

static bool in_rounded_square(double u, double v, double inset, double radius)
{
    double lo = inset + radius, hi = 1.0 - inset - radius;
    double dx = u < lo ? lo - u : u > hi ? u - hi : 0.0;
    double dy = v < lo ? lo - v : v > hi ? v - hi : 0.0;
    if (u < inset || u > 1.0 - inset || v < inset || v > 1.0 - inset) return false;
    return dx * dx + dy * dy <= radius * radius;
}

typedef struct { unsigned long fg, bg, red, amber; } IconColors;

/* A miniature of the window: coloured log lines beside a scrollbar. */
static unsigned long icon_color(double u, double v, const IconColors *c)
{
    const double margin = 0.04, radius = 0.18, frame = 0.07;
    if (!in_rounded_square(u, v, margin, radius)) return 0;
    if (!in_rounded_square(u, v, margin + frame, radius - frame)) return c->fg;
    if (u > 0.77 && u < 0.83 && v > 0.18 && v < 0.82)
        return v > 0.52 && v < 0.78 ? c->fg : 0xffcccccc;
    static const double len[] = { 0.44, 0.52, 0.38, 0.48, 0.30, 0.42 };
    for (int i = 0; i < 6; ++i) {
        double y0 = 0.20 + 0.11 * i;
        if (v > y0 && v < y0 + 0.06 && u > 0.17 && u < 0.17 + len[i])
            return i == 1 ? c->red : i == 3 ? c->amber : c->fg;
    }
    return c->bg;
}

static void set_icon(void)
{
    IconColors colors = {
        .fg = 0xff000000 | ((pal[ifg].c.red >> 8) << 16) | ((pal[ifg].c.green >> 8) << 8) | (pal[ifg].c.blue >> 8),
        .bg = 0xff000000 | ((pal[ibg].c.red >> 8) << 16) | ((pal[ibg].c.green >> 8) << 8) | (pal[ibg].c.blue >> 8),
        .red = 0xffdd0000, .amber = 0xffcc8800 };
    static const int sizes[] = { 16, 24, 32, 48, 64 };
    enum { SUB = 4 };
    size_t count = 0;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i)
        count += 2 + (size_t)sizes[i] * (size_t)sizes[i];
    unsigned long *data = malloc(count * sizeof(*data));
    if (!data) return;
    unsigned long *p = data;
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; ++i) {
        int n = sizes[i];
        *p++ = (unsigned long)n;
        *p++ = (unsigned long)n;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                unsigned a = 0, r = 0, g = 0, b = 0;
                for (int sy = 0; sy < SUB; ++sy)
                    for (int sx = 0; sx < SUB; ++sx) {
                        unsigned long c = icon_color((x + (sx + 0.5) / SUB) / n,
                                                     (y + (sy + 0.5) / SUB) / n, &colors);
                        unsigned ca = (c >> 24) & 0xff;
                        a += ca;
                        r += ((c >> 16) & 0xff) * ca / 255;
                        g += ((c >> 8) & 0xff) * ca / 255;
                        b += (c & 0xff) * ca / 255;
                    }
                a /= SUB * SUB;
                if (a) {
                    r = r / (SUB * SUB) * 255 / a;
                    g = g / (SUB * SUB) * 255 / a;
                    b = b / (SUB * SUB) * 255 / a;
                }
                *p++ = (unsigned long)a << 24 | (unsigned long)(r > 255 ? 255 : r) << 16 |
                       (unsigned long)(g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b);
            }
    }
    XChangeProperty(dpy, win, XInternAtom(dpy, "_NET_WM_ICON", False), XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)data, (int)count);
    free(data);
}

/* ------------------------------------------------------------------ */
/* Input                                                              */
/* ------------------------------------------------------------------ */

static bool dragging;
static int drag_off;

static void run_prompt(int mode)
{
    prompt_mode = mode;
    plen = 0;
    pbuf[0] = 0;
    if (mode == PM_WRITE) { snprintf(pbuf, sizeof pbuf, "xlgr.log"); plen = strlen(pbuf); }
    msg[0] = 0;
    dirty = true;
}

static void finish_prompt(bool accept)
{
    int mode = prompt_mode;
    prompt_mode = PM_NONE;
    dirty = true;
    if (!accept) return;
    if (mode == PM_SEARCH) apply_search(pbuf);
    else if (mode == PM_FILTER) apply_filter(pbuf);
    else if (mode == PM_WRITE && *pbuf) write_buffer(pbuf);
}

/* Returns false when the program should quit. */
static bool handle_key(XKeyEvent *ev)
{
    char text[32];
    KeySym sym;
    int len = XLookupString(ev, text, sizeof text - 1, &sym, NULL);
    int64_t rows = text_rows();

    if (prompt_mode) {
        if (sym == XK_Return || sym == XK_KP_Enter) finish_prompt(true);
        else if (sym == XK_Escape) finish_prompt(false);
        else if (sym == XK_BackSpace) { if (plen) pbuf[--plen] = 0; dirty = true; }
        else if ((ev->state & ControlMask) && (sym == XK_u)) { plen = 0; pbuf[0] = 0; dirty = true; }
        else if (len == 1 && (unsigned char)text[0] >= 0x20 && plen + 1 < sizeof pbuf) {
            pbuf[plen++] = text[0];
            pbuf[plen] = 0;
            dirty = true;
        }
        return true;
    }
    msg[0] = 0;
    dirty = true;
    switch (sym) {
    case XK_Up: case XK_k: cursor_move(-1); break;
    case XK_Down: case XK_j: cursor_move(1); break;
    case XK_Prior: case XK_KP_Prior: {
        int64_t d = rows > 1 ? rows - 1 : 1;
        if (cursor_on) cursor_move(-d); else scroll_by(-d);
        break;
    }
    case XK_Next: case XK_KP_Next: {
        int64_t d = rows > 1 ? rows - 1 : 1;
        if (cursor_on) cursor_move(d); else scroll_by(d);
        break;
    }
    case XK_Home: case XK_g:
        follow = false;
        cursor_on = false;
        scroll_to(0, true);
        follow = false;
        break;
    case XK_End: case XK_G:
        follow = true;
        cursor_on = false;
        break;
    case XK_Left: xoff = xoff >= 8 ? xoff - 8 : 0; break;
    case XK_Right: xoff = xoff + 8 < MAXLINE ? xoff + 8 : xoff; break;
    case XK_space:
        if (follow) { follow = false; set_msg("paused"); }
        else { follow = true; cursor_on = false; }
        break;
    case XK_slash: run_prompt(PM_SEARCH); break;
    case XK_f: run_prompt(PM_FILTER); break;
    case XK_w: run_prompt(PM_WRITE); break;
    case XK_n: search_next(1); break;
    case XK_N: search_next(-1); break;
    case XK_C: clear_buffer(); break;
    case XK_Return: case XK_KP_Enter: case XK_c: copy_line(); break;
    case XK_Escape:
        if (search_on) apply_search("");
        else cursor_on = false;
        break;
    case XK_q: return false;
    default: break;
    }
    return true;
}

static void handle_button(XButtonEvent *ev)
{
    int text_w = (int)win_w - SCROLLBAR;
    int text_h = (int)win_h - (lh + 4);
    if (ev->button == Button4 || ev->button == Button5) {
        int64_t d = (ev->state & ShiftMask) ? 1 : 3;
        scroll_by(ev->button == Button4 ? -d : d);
        return;
    }
    if (ev->button != Button1) return;
    if (ev->x >= text_w && ev->y < text_h) {
        int ty, th;
        thumb_geometry(text_h, &ty, &th);
        if (ev->y >= ty && ev->y < ty + th) {
            dragging = true;
            drag_off = ev->y - ty;
        } else {
            int64_t page = text_rows() > 1 ? text_rows() - 1 : 1;
            scroll_by(ev->y < ty ? -page : page);
        }
    } else if (ev->y < text_h) {
        int64_t vt = sync_view(), n = nvis();
        int64_t k = vt + ev->y / lh;
        if (k < n) cursor_to(k);
    }
}

static void handle_motion(XMotionEvent *ev)
{
    if (!dragging) return;
    int text_h = (int)win_h - (lh + 4);
    int ty, th;
    thumb_geometry(text_h, &ty, &th);
    int64_t n = nvis(), rows = text_rows();
    if (n <= rows || text_h <= th) return;
    int64_t pos = ev->y - drag_off;
    if (pos < 0) pos = 0;
    if (pos > text_h - th) pos = text_h - th;
    int64_t target = pos * (n - rows) / (text_h - th);
    int64_t cur = sync_view();
    scroll_to(target, target < cur || target < n - rows);
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    parse_options(argc, argv);
    source_init(&src);

    ring_cap = (size_t)opt.scrollback;
    ring = calloc(ring_cap, sizeof(*ring));
    if (!ring) die_oom();

    dpy = XOpenDisplay(opt.display_name);
    if (!dpy) {
        fprintf(stderr, "xlgr: cannot open display %s\n",
                opt.display_name ? opt.display_name :
                (getenv("DISPLAY") ? getenv("DISPLAY") : "(unset)"));
        return EXIT_FAILURE;
    }
    scr = DefaultScreen(dpy);
    cmap = DefaultColormap(dpy, scr);

    font = XLoadQueryFont(dpy, opt.font);
    if (!font && strcmp(opt.font, "fixed")) {
        fprintf(stderr, "xlgr: cannot load font %s; using fixed\n", opt.font);
        font = XLoadQueryFont(dpy, "fixed");
    }
    if (!font) {
        fprintf(stderr, "xlgr: cannot load the fixed font\n");
        return EXIT_FAILURE;
    }
    cw = font->max_bounds.width;
    if (cw < 1) cw = 1;
    lh = font->ascent + font->descent + 1;

    ifg = pal_required(opt.foreground);
    ibg = pal_required(opt.background);
    ihl = pal_required(opt.highlight);
    dark_background = (pal[ibg].c.red + pal[ibg].c.green + pal[ibg].c.blue) / 3 < 0x8000;
    isb_trough = pal_required("gray90");
    isb_thumb = pal_required("gray55");
    isearch_fg = pal_required("black");
    isearch_bg = pal_required("yellow");

    int x = 0, y = 0;
    unsigned width = 100 * (unsigned)cw + SCROLLBAR, height = 30 * (unsigned)lh + (unsigned)lh + 4;
    if (opt.geometry) {
        unsigned gw, gh;
        int flags = XParseGeometry(opt.geometry, &x, &y, &gw, &gh);
        if (!flags || ((flags & WidthValue) && !gw) || ((flags & HeightValue) && !gh)) {
            fprintf(stderr, "xlgr: invalid geometry: %s\n", opt.geometry);
            return EXIT_FAILURE;
        }
        /* Geometry is in pixels, as for xmem and xdf. */
        if (flags & WidthValue) width = gw;
        if (flags & HeightValue) height = gh;
        if (flags & XNegative) x += DisplayWidth(dpy, scr) - (int)width;
        if (flags & YNegative) y += DisplayHeight(dpy, scr) - (int)height;
    }
    win_w = width;
    win_h = height;
    win = XCreateSimpleWindow(dpy, RootWindow(dpy, scr), x, y, width, height, 1,
                              pal[ifg].c.pixel, pal[ibg].c.pixel);
    char title[400];
    snprintf(title, sizeof title, "xlgr: %s", src.label);
    XStoreName(dpy, win, title);
    XSelectInput(dpy, win, ExposureMask | StructureNotifyMask | KeyPressMask |
                 ButtonPressMask | ButtonReleaseMask | Button1MotionMask);
    XClassHint hint = { .res_name = "xlgr", .res_class = "Xlgr" };
    XSetClassHint(dpy, win, &hint);
    XSizeHints size = { .flags = PMinSize, .min_width = 8 * cw + SCROLLBAR, .min_height = 3 * lh + 4 };
    XSetWMNormalHints(dpy, win, &size);
    set_icon();
    Atom wm_delete = XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(dpy, win, &wm_delete, 1);
    a_clipboard = XInternAtom(dpy, "CLIPBOARD", False);
    a_utf8 = XInternAtom(dpy, "UTF8_STRING", False);
    a_targets = XInternAtom(dpy, "TARGETS", False);

    XGCValues values = { .font = font->fid };
    gc = XCreateGC(dpy, win, GCFont, &values);
    make_backbuf();

    if (!opt.norules) find_rules_file();
    load_rules(true);
    long long now = monotonic_ms();
    source_open(&src, now);
    XMapWindow(dpy, win);

    long long last_draw = 0, next_rules_check = now + 1000;
    bool running = true;
    while (running) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            switch (ev.type) {
            case DestroyNotify: running = false; break;
            case ClientMessage:
                if ((Atom)ev.xclient.data.l[0] == wm_delete) running = false;
                break;
            case ConfigureNotify:
                if ((unsigned)ev.xconfigure.width != win_w || (unsigned)ev.xconfigure.height != win_h) {
                    win_w = (unsigned)ev.xconfigure.width;
                    win_h = (unsigned)ev.xconfigure.height;
                    make_backbuf();
                    dirty = true;
                }
                break;
            case Expose: if (ev.xexpose.count == 0) dirty = true; break;
            case KeyPress: if (!handle_key(&ev.xkey)) running = false; break;
            case ButtonPress: handle_button(&ev.xbutton); break;
            case ButtonRelease: if (ev.xbutton.button == Button1) dragging = false; break;
            case MotionNotify: handle_motion(&ev.xmotion); break;
            case SelectionRequest: answer_selection(&ev.xselectionrequest); break;
            case SelectionClear: free(clip_text); clip_text = NULL; break;
            default: break;
            }
        }
        if (!running) break;

        now = monotonic_ms();
        long long deadline = now + 1000;
        if (dirty) {
            long long due = last_draw + FRAME_MS;
            if (due <= now) {
                draw();
                last_draw = now;
            } else if (due < deadline) {
                deadline = due;
            }
        }
        if ((src.state == ST_WAIT || (src.kind == SRC_FILE && src.idle)) && src.next_at < deadline)
            deadline = src.next_at;
        int timeout = deadline > now ? (int)(deadline - now) : 0;

        struct pollfd pfds[2] = { { .fd = ConnectionNumber(dpy), .events = POLLIN },
                                  { .fd = -1 } };
        if (src.fd >= 0 && src.state == ST_CONNECTING) {
            pfds[1].fd = src.fd;
            pfds[1].events = POLLOUT;
        } else if (src.fd >= 0 && src.state == ST_OPEN && !src.idle) {
            pfds[1].fd = src.fd;
            pfds[1].events = POLLIN;
        }
        if (poll(pfds, 2, timeout) < 0 && errno != EINTR) {
            perror("xlgr: poll");
            break;
        }
        now = monotonic_ms();
        if (pfds[1].fd >= 0 && pfds[1].revents) {
            if (src.state == ST_CONNECTING) source_connect_check(&src, now);
            else source_read(&src, now);
        }
        source_tick(&src, now);
        if (now >= next_rules_check) {
            check_rules_file();
            next_rules_check = now + 1000;
        }
    }

    source_close(&src);
    XFreeGC(dpy, gc);
    XFreeFont(dpy, font);
    XCloseDisplay(dpy);
    return EXIT_SUCCESS;
}
