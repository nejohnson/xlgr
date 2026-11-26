/* Unit tests for xlgr's display-independent logic. They include xlgr.c
   directly (with main renamed), so no X server is needed. */
#define main xlgr_main
#include "../xlgr.c"
#undef main

static int checks, fails;

#define CHECK(cond) do { ++checks; if (!(cond)) { ++fails; \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_STR(got, want) do { ++checks; const char *g_ = (got), *w_ = (want); \
    if (strcmp(g_, w_)) { ++fails; \
    printf("FAIL %s:%d: got \"%s\", want \"%s\"\n", __FILE__, __LINE__, g_, w_); } } while (0)
#define CHECK_INT(got, want) do { ++checks; long long g_ = (got), w_ = (want); \
    if (g_ != w_) { ++fails; \
    printf("FAIL %s:%d: %s = %lld, want %lld\n", __FILE__, __LINE__, #got, g_, w_); } } while (0)

/* Start from an empty buffer with room for cap lines and a 10-row window. */
static void reset(size_t cap)
{
    if (filter_on) { regfree(&filter_re); filter_on = false; }
    if (search_on) { regfree(&search_re); search_on = false; }
    if (ring) { clear_buffer(); free(ring); }
    ring_cap = cap;
    ring = calloc(cap, sizeof(*ring));
    base = total = 0;
    open_line = false;
    fcount = fstart = 0;
    memset(&ing, 0, sizeof ing);
    ing.cur = ansi_default;
    opt = (Options){ 0 };
    follow = true;
    top_abs = cur_abs = 0;
    cursor_on = false;
    sel_on = false;
    msg[0] = 0;
    lh = 10;
    cw = 6;
    win_w = 400;
    win_h = 10 * 10 + 10 + 4;       /* ten text rows plus the status line */
}

static void feed(const char *s)
{
    for (; *s; ++s) ingest_byte((unsigned char)*s);
}

static void feed_lines(const char *prefix, int n)
{
    char buf[64];
    for (int i = 0; i < n; ++i) {
        snprintf(buf, sizeof buf, "%s%d\n", prefix, i);
        feed(buf);
    }
}

static const char *text(int64_t n) { return line_at(n)->s; }

static void test_line_endings(void)
{
    reset(100);
    feed("a\nb\r\nc\rd\n");
    CHECK_INT(total - base, 4);
    CHECK_STR(text(0), "a");
    CHECK_STR(text(1), "b");
    CHECK_STR(text(2), "c");
    CHECK_STR(text(3), "d");
    CHECK(!open_line);

    reset(100);
    feed("\n\n");                    /* blank lines are lines */
    CHECK_INT(total - base, 2);
    CHECK_STR(text(0), "");
}

static void test_partial_lines(void)
{
    reset(100);
    feed("abc");
    CHECK_INT(total - base, 1);      /* visible before the newline arrives */
    CHECK(open_line);
    CHECK_STR(text(0), "abc");
    feed("def\n");
    CHECK(!open_line);
    CHECK_INT(total - base, 1);
    CHECK_STR(text(0), "abcdef");
    feed("p\r");
    feed("\nq\n");                   /* CRLF split across feeds is one break */
    CHECK_INT(total - base, 3);
    CHECK_STR(text(1), "p");
    CHECK_STR(text(2), "q");
}

static void test_characters(void)
{
    reset(100);
    feed("a\tb\n");
    CHECK_STR(text(0), "a       b");
    feed("\t\n");
    CHECK_STR(text(1), "        ");
    feed("caf\xc3\xa9 \xe2\x80\xa2\n");   /* e-acute fits Latin-1, bullet does not */
    CHECK_STR(text(2), "caf\xe9 ?");
    feed("x\xff" "y\x01" "z\n");          /* invalid UTF-8 and control bytes */
    CHECK_STR(text(3), "x.y.z");
    ingest_byte('n');
    ingest_byte(0);                       /* NUL is dropped */
    ingest_byte('\n');
    CHECK_STR(text(4), "n");
    feed("\xc3");                         /* truncated sequence, then ASCII */
    feed("q\n");
    CHECK_STR(text(5), ".q");
}

static void test_long_lines(void)
{
    reset(100);
    char *big = malloc(MAXLINE + 11);
    memset(big, 'x', MAXLINE + 10);
    big[MAXLINE + 10] = 0;
    feed(big);
    feed("\n");
    free(big);
    CHECK_INT(total - base, 2);
    CHECK_INT(line_at(0)->len, MAXLINE);
    CHECK_INT(line_at(1)->len, 10);
}

static void test_ansi_stripping(void)
{
    reset(100);
    opt.noansi = true;
    feed("\x1b[31mred\x1b[0m plain\n");
    CHECK_STR(text(0), "red plain");
    CHECK_INT(line_at(0)->nruns, 0);
    reset(100);
    feed("\x1b[2J\x1b[1;1Hhi\x1b[?25l there\n");   /* non-colour sequences */
    CHECK_STR(text(0), "hi there");
    CHECK_INT(line_at(0)->nruns, 0);
    feed("x\x1b" "cy\n");                           /* two-byte escape */
    CHECK_STR(text(1), "xy");
}

static void test_ansi_styles(void)
{
    reset(100);
    feed("\x1b[31mred\x1b[0m plain\n");
    const Line *l = line_at(0);
    CHECK_STR(l->s, "red plain");
    CHECK_INT(l->nruns, 2);
    CHECK_INT(l->runs[0].col, 0);
    CHECK_INT(l->runs[0].style.fg, ANSI_INDEXED | 1);
    CHECK_INT(l->runs[1].col, 3);
    CHECK_INT(l->runs[1].style.fg, -1);

    feed("\x1b[1;4;92;44mx\n");            /* style carries to the next line */
    feed("next\x1b[0m\n");
    l = line_at(1);
    CHECK_INT(l->nruns, 1);
    CHECK_INT(l->runs[0].col, 0);
    CHECK_INT(l->runs[0].style.attrs, A_BOLD | A_UNDERLINE);
    CHECK_INT(l->runs[0].style.fg, ANSI_INDEXED | 10);
    CHECK_INT(l->runs[0].style.bg, ANSI_INDEXED | 4);
    l = line_at(2);
    CHECK_STR(l->s, "next");
    CHECK_INT(l->nruns, 1);                /* inherited; the reset follows no text */
    CHECK_INT(l->runs[0].col, 0);
    CHECK_INT(l->runs[0].style.attrs, A_BOLD | A_UNDERLINE);
    feed("after\n");                       /* the reset applies from here on */
    CHECK_INT(line_at(3)->nruns, 0);

    reset(100);
    feed("\x1b[38;5;208ma\x1b[38;2;1;2;3mb\x1b[48;2;255;0;16mc\x1b[0m\n");
    l = line_at(0);
    CHECK_INT(l->nruns, 3);
    CHECK_INT(l->runs[0].style.fg, ANSI_INDEXED | 208);
    CHECK_INT(l->runs[1].style.fg, 0x010203);
    CHECK_INT(l->runs[2].style.bg, 0xff0010);
    CHECK_INT(l->runs[2].style.fg, 0x010203);   /* foreground kept */

    reset(100);
    feed("\x1b[38:2::10:20:30mc\x1b[7mr\x1b[27;39mn\x1b[m\n");   /* colon form, reverse */
    l = line_at(0);
    CHECK_INT(l->runs[0].style.fg, (10 << 16) | (20 << 8) | 30);
    CHECK_INT(l->runs[1].style.attrs, A_REVERSE);
    CHECK_INT(l->runs[2].style.attrs, 0);
    CHECK_INT(l->runs[2].style.fg, -1);

    reset(100);                             /* colon form without a colourspace id */
    feed("\x1b[38:2:10:20:30mc\x1b[0m\n");
    CHECK_INT(line_at(0)->runs[0].style.fg, (10 << 16) | (20 << 8) | 30);

    reset(100);
    feed("\x1b[3");                         /* sequence split across reads */
    feed("1mred\n");
    CHECK_STR(text(0), "red");
    CHECK_INT(line_at(0)->runs[0].style.fg, ANSI_INDEXED | 1);

    reset(100);
    feed("\x1b[31m\x1b[0mnone\n");          /* no visible chars styled: no runs */
    CHECK_INT(line_at(0)->nruns, 0);
}

static void test_escape_strings(void)
{
    reset(100);
    feed("a\x1b]0;window title\x07" "b\n");              /* OSC ended by BEL */
    CHECK_STR(text(0), "ab");
    feed("a\x1b]8;;http://example.com\x1b\\link\x1b]8;;\x1b\\z\n");   /* OSC 8, ended by ST */
    CHECK_STR(text(1), "alinkz");
    feed("x\x1bP1$r0m\x1b\\y\x1b_app data\x1b\\z\n");       /* DCS and APC */
    CHECK_STR(text(2), "xyz");
    feed("t\x1b]0;caf\xc3\xa9 \xe2\x80\xa2\x07" "u\n");      /* UTF-8 inside a string */
    CHECK_STR(text(3), "tu");
    feed("m\x1b]2;sp");                                    /* split across reads */
    feed("lit\x07n\n");
    CHECK_STR(text(4), "mn");
    feed("x\x1b]0;unterminated\nnext\n");                 /* a newline ends the string */
    CHECK_STR(text(5), "x");
    CHECK_STR(text(6), "next");
    feed("a\x1b]0;t\x1b[31mred\n");                       /* ESC [ inside a string starts a CSI */
    CHECK_STR(text(7), "ared");
    CHECK_INT(line_at(7)->runs[0].col, 1);
    CHECK_INT(line_at(7)->runs[0].style.fg, ANSI_INDEXED | 1);
    feed("p\x1b]0;a\x1b\x1b\\q\n");                      /* ESC ESC \ */
    CHECK_STR(text(8), "pq");

    reset(100);
    feed("a\x1b(Bb\x1b(0c\x1b)0d\x1b#8e\n");             /* charset and ESC # sequences */
    CHECK_STR(text(0), "abcde");

    reset(100);
    feed("a\x07" "b\x07\n");                              /* BEL is dropped */
    CHECK_STR(text(0), "ab");
    feed("c\x1b" "7d\x1b" "8e\n");                       /* save/restore cursor */
    CHECK_STR(text(1), "cde");
    feed("x\x1b\ny\n");                                   /* a lone ESC must not eat the newline */
    CHECK_STR(text(2), "x");
    CHECK_STR(text(3), "y");
    feed("p\x1b[31\nq\x1b(\nr\n");                       /* nor an unfinished CSI or charset */
    CHECK_STR(text(4), "p");
    CHECK_STR(text(5), "q");
    CHECK_STR(text(6), "r");
    feed("a\x1b[3\x1b[32mb\n");                          /* ESC inside a CSI restarts it */
    CHECK_STR(text(7), "ab");
    CHECK_INT(line_at(7)->runs[0].style.fg, ANSI_INDEXED | 2);
}

static void test_ansi_dim_strike(void)
{
    reset(100);
    feed("\x1b[2mdim\x1b[22m \x1b[9mstrike\x1b[29m \x1b[1;2mboth\x1b[22m.\n");
    const Line *l = line_at(0);
    CHECK_STR(l->s, "dim strike both.");
    CHECK_INT(l->nruns, 6);
    CHECK_INT(l->runs[0].style.attrs, A_DIM);
    CHECK_INT(l->runs[1].col, 3);
    CHECK_INT(l->runs[1].style.attrs, 0);
    CHECK_INT(l->runs[2].style.attrs, A_STRIKE);
    CHECK_INT(l->runs[3].style.attrs, 0);
    CHECK_INT(l->runs[4].style.attrs, A_BOLD | A_DIM);
    CHECK_INT(l->runs[5].style.attrs, 0);           /* 22 clears bold and dim */

    reset(100);
    feed("\x1b[2;9;4mx\x1b[0m\n");
    CHECK_INT(line_at(0)->runs[0].style.attrs, A_DIM | A_STRIKE | A_UNDERLINE);
}

static void test_ansi_colours(void)
{
    dark_background = false;
    CHECK_INT(ansi_rgb(0), 0x000000);
    CHECK_INT(ansi_rgb(7), 0x909090);       /* readable on white */
    dark_background = true;
    CHECK_INT(ansi_rgb(7), 0xe5e5e5);
    dark_background = false;
    CHECK_INT(ansi_rgb(16), 0x000000);      /* start of the 6x6x6 cube */
    CHECK_INT(ansi_rgb(196), 0xff0000);
    CHECK_INT(ansi_rgb(231), 0xffffff);
    CHECK_INT(ansi_rgb(232), 0x080808);     /* grey ramp */
    CHECK_INT(ansi_rgb(255), 0xeeeeee);
}

static void test_ring(void)
{
    reset(5);
    feed_lines("l", 12);
    CHECK_INT(total, 12);
    CHECK_INT(base, 7);
    CHECK_INT(nvis(), 5);
    CHECK_STR(text(7), "l7");
    CHECK_STR(text(11), "l11");

    clear_buffer();
    CHECK_INT(nvis(), 0);
    CHECK_INT(base, total);                 /* numbers stay monotonic */
    feed("again\n");
    CHECK_INT(nvis(), 1);
    CHECK_STR(text(total - 1), "again");

    reset(3);                               /* ANSI runs are freed on trimming */
    for (int i = 0; i < 10; ++i) feed("\x1b[31mred\x1b[0m\n");
    CHECK_INT(nvis(), 3);
}

static void test_filter(void)
{
    reset(5);
    feed("x0\ny\nx1\ny\nx2\n");
    apply_filter("x");
    CHECK(filter_on);
    CHECK_INT(nvis(), 3);
    CHECK_STR(text(vabs(1)), "x1");
    feed("y\nx3\n");                        /* new lines are filtered as they close */
    CHECK_INT(total, 7);
    CHECK_INT(base, 2);
    CHECK_INT(nvis(), 3);                   /* x0 was trimmed away */
    CHECK_INT(vabs(0), 2);
    CHECK_INT(vabs(2), 6);
    CHECK_INT(vlower(5), 2);                /* visible lines before absolute 5 */
    CHECK_INT(vlower(0), 0);
    CHECK_INT(vlower(100), 3);
    apply_filter("");
    CHECK(!filter_on);
    CHECK_INT(nvis(), 5);

    reset(50);                              /* smart case */
    feed("Error one\nerror two\nfine\n");
    apply_filter("error");
    CHECK_INT(nvis(), 2);
    apply_filter("Error");
    CHECK_INT(nvis(), 1);
    apply_filter("(");                      /* bad regex leaves no filter */
    CHECK(!filter_on);
    CHECK(msg[0] != 0);
}

static void test_view(void)
{
    reset(100);
    feed_lines("l", 50);
    CHECK_INT(sync_view(), 40);             /* following: bottom ten rows */
    scroll_by(-5);
    CHECK(!follow);
    CHECK_INT(sync_view(), 35);
    scroll_by(2);
    CHECK(!follow);
    CHECK_INT(sync_view(), 37);
    scroll_by(100);                         /* reaching the bottom resumes */
    CHECK(follow);
    CHECK_INT(sync_view(), 40);

    cursor_move(-1);                        /* cursor starts on the bottom row */
    CHECK(cursor_on);
    CHECK(!follow);
    CHECK_INT(cur_abs, 48);
    cursor_move(-20);
    CHECK_INT(cur_abs, 28);
    CHECK_INT(sync_view(), 28);             /* the view followed the cursor */
    cursor_move(-1000);
    CHECK_INT(cur_abs, 0);
    cursor_move(1000);
    CHECK_INT(cur_abs, 49);

    reset(100);                             /* fewer lines than rows */
    feed_lines("l", 3);
    CHECK_INT(sync_view(), 0);
    scroll_by(5);
    CHECK_INT(sync_view(), 0);

    reset(20);                              /* a paused view survives trimming */
    feed_lines("l", 20);
    follow = false;
    top_abs = 8;
    feed_lines("m", 5);                     /* base is now 5 */
    CHECK_INT(sync_view(), 3);
    feed_lines("m", 10);                    /* line 8 is trimmed: clamp to oldest */
    CHECK_INT(sync_view(), 0);
}

static void test_search(void)
{
    reset(100);
    feed_lines("l", 50);
    apply_search("9");                      /* first match from the bottom view */
    CHECK(search_on);
    CHECK(cursor_on);
    CHECK_INT(cur_abs, 49);
    search_next(-1);
    CHECK_INT(cur_abs, 39);
    search_next(1);
    CHECK_INT(cur_abs, 49);
    search_next(1);                         /* wraps */
    CHECK_INT(cur_abs, 9);
    apply_search("");
    CHECK(!search_on);

    apply_search("nomatch");
    CHECK(msg[0] != 0);
    apply_search("");

    reset(100);                             /* search honours the filter */
    feed_lines("l", 30);
    apply_filter("5");                      /* l5 l15 l25 */
    apply_search("l");
    CHECK_INT(cur_abs, 5);
    search_next(1);
    CHECK_INT(cur_abs, 15);
}

static void test_selection(void)
{
    int64_t count = 0, lo, hi;
    reset(100);
    feed_lines("l", 10);
    CHECK(selection_text(&count) == NULL);          /* nothing selected */
    CHECK(!sel_range(&lo, &hi));

    sel_on = true;
    sel_a = 2;
    sel_b = 4;
    char *t = selection_text(&count);
    CHECK_STR(t, "l2\nl3\nl4");                      /* whole lines, no trailing newline */
    CHECK_INT(count, 3);
    free(t);

    sel_a = 4;                                      /* either direction */
    sel_b = 2;
    t = selection_text(&count);
    CHECK_STR(t, "l2\nl3\nl4");
    free(t);

    sel_a = sel_b = 7;                              /* a single line */
    t = selection_text(&count);
    CHECK_STR(t, "l7");
    CHECK_INT(count, 1);
    free(t);

    sel_a = 8;                                      /* past the end is clamped */
    sel_b = 500;
    CHECK(sel_range(&lo, &hi));
    CHECK_INT(lo, 8);
    CHECK_INT(hi, 9);

    clear_buffer();                                 /* clearing drops the selection */
    CHECK(!sel_on);

    reset(5);                                       /* a trimmed anchor clamps to the oldest */
    feed_lines("l", 12);                            /* l7..l11 remain */
    sel_on = true;
    sel_a = 2;
    sel_b = 9;
    t = selection_text(&count);
    CHECK_STR(t, "l7\nl8\nl9");
    free(t);

    reset(100);                                     /* selections span only filtered lines */
    feed("a1\nb\na2\nb\na3\nb\n");
    apply_filter("a");
    CHECK(!sel_on);                                 /* a new filter drops the selection */
    sel_on = true;
    sel_a = 0;
    sel_b = 4;
    t = selection_text(&count);
    CHECK_STR(t, "a1\na2\na3");
    CHECK_INT(count, 3);
    free(t);

    reset(100);                                     /* a line with ANSI colour copies as plain text */
    feed("\x1b[31mred\x1b[0m text\n");
    sel_on = true;
    sel_a = sel_b = 0;
    t = selection_text(&count);
    CHECK_STR(t, "red text");
    free(t);
}

static void test_rules(void)
{
    Rule r;
    char err[200];
    CHECK_INT(parse_rule("", &r, err, sizeof err), 1);
    CHECK_INT(parse_rule("   # just a comment", &r, err, sizeof err), 1);

    CHECK_INT(parse_rule("/warn(ing)?/i bold underline", &r, err, sizeof err), 0);
    CHECK_INT(r.attrs, A_BOLD | A_UNDERLINE);
    CHECK(!r.line);
    CHECK(regexec(&r.re, "A WARNING here", 0, NULL, 0) == 0);   /* /i flag */
    regfree(&r.re);

    CHECK_INT(parse_rule("/Warn/ bold", &r, err, sizeof err), 0);
    CHECK(regexec(&r.re, "WARN", 0, NULL, 0) != 0);             /* case sensitive */
    regfree(&r.re);

    CHECK_INT(parse_rule("/a\\/b/ line # trailing comment", &r, err, sizeof err), 0);
    CHECK(r.line);
    CHECK(regexec(&r.re, "x a/b y", 0, NULL, 0) == 0);
    regfree(&r.re);

    CHECK_INT(parse_rule("/\\[([a-z]+)\\]/ group=1", &r, err, sizeof err), 0);
    CHECK_INT(r.group, 1);
    regfree(&r.re);

    CHECK_INT(parse_rule("/x/ strike", &r, err, sizeof err), 0);
    CHECK_INT(r.attrs, A_STRIKE);
    regfree(&r.re);

    CHECK_INT(parse_rule("/x/ \t bold   # c", &r, err, sizeof err), 0);
    CHECK_INT(r.attrs, A_BOLD);
    regfree(&r.re);

    CHECK_INT(parse_rule("error bold", &r, err, sizeof err), -1);
    CHECK_STR(err, "rule must start with /");
    CHECK_INT(parse_rule("/unterminated bold", &r, err, sizeof err), -1);
    CHECK_STR(err, "missing closing /");
    CHECK_INT(parse_rule("/x/ sparkle", &r, err, sizeof err), -1);
    CHECK_STR(err, "unknown attribute: sparkle");
    CHECK_INT(parse_rule("/(/ bold", &r, err, sizeof err), -1);
    CHECK(err[0] != 0);
    CHECK_INT(parse_rule("/x/ group=2", &r, err, sizeof err), -1);
    CHECK(strstr(err, "group=2") != NULL);
}

static void test_telnet(void)
{
    reset(100);
    int p[2];
    CHECK(pipe(p) == 0);
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    Source s = { .kind = SRC_TELNET, .fd = p[1] };
    const unsigned char in[] = {
        255, 251, 1,                       /* WILL ECHO -> DO ECHO */
        255, 253, 24,                      /* DO TERMINAL-TYPE -> WONT */
        255, 251, 31,                      /* WILL NAWS -> DONT */
        255, 250, 24, 1, 255, 240,         /* subnegotiation is skipped */
        'h', 'i', 255, 255, '\n' };
    for (size_t i = 0; i < sizeof in; ++i) telnet_byte(&s, in[i]);
    CHECK_STR(text(0), "hi.");             /* IAC IAC is a literal 0xff */
    unsigned char out[32];
    ssize_t n = read(p[0], out, sizeof out);
    const unsigned char want[] = { 255, 253, 1, 255, 252, 24, 255, 254, 31 };
    CHECK_INT(n, (long long)sizeof want);
    CHECK(n == (ssize_t)sizeof want && !memcmp(out, want, sizeof want));
    close(p[0]);
    close(p[1]);
}

static void test_serial_baud(void)
{
    CHECK(baud_constant(9600) == B9600);
    CHECK(baud_constant(115200) == B115200);
    CHECK(baud_constant(921600) == B921600);
    CHECK(baud_constant(12345) == B0);
}

static void test_write_buffer(void)
{
    reset(100);
    feed("one\ntwo\nthree\n");
    char path[] = "/tmp/xlgr_test_XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    close(fd);
    write_buffer(path);
    FILE *f = fopen(path, "r");
    char buf[64] = "";
    size_t n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = 0;
    if (f) fclose(f);
    CHECK_STR(buf, "one\ntwo\nthree\n");
    apply_filter("t");                      /* only the visible lines are written */
    write_buffer(path);
    f = fopen(path, "r");
    n = f ? fread(buf, 1, sizeof buf - 1, f) : 0;
    buf[n] = 0;
    if (f) fclose(f);
    CHECK_STR(buf, "two\nthree\n");
    unlink(path);
}

int main(void)
{
    test_line_endings();
    test_partial_lines();
    test_characters();
    test_long_lines();
    test_ansi_stripping();
    test_ansi_styles();
    test_escape_strings();
    test_ansi_dim_strike();
    test_ansi_colours();
    test_ring();
    test_filter();
    test_view();
    test_search();
    test_selection();
    test_rules();
    test_telnet();
    test_serial_baud();
    test_write_buffer();
    printf("%d checks, %d failed\n", checks, fails);
    return fails ? EXIT_FAILURE : EXIT_SUCCESS;
}
