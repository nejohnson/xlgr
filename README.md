# xlgr

`xlgr` is a small X11 log message viewer in the style of `xload`, `xdf` and
`xmem`. It shows log lines from a file, standard input, a serial port or a
telnet connection, with regex highlighting rules, a scrollbar and a cursor.
Lines are kept in memory (100,000 by default; up to 100,000,000 with
`-scrollback`) and the oldest are dropped when the limit is reached.

Build it with `make` (a C compiler and Xlib development files are required),
then run:

```sh
./xlgr app.log                         # shows the file, then follows it like tail -f
some_command | ./xlgr -                # standard input
./xlgr -serial /dev/ttyUSB0            # 115200 8N1; reconnects if the device goes away
./xlgr -serial /dev/ttyUSB0 -baud 921600 -serialmode 8N1 -flow rtscts
./xlgr -telnet 192.168.1.50:8070       # reconnects if the connection drops
./xlgr app.log -scrollback 1000000 -font 9x15 -rules ~/my.rules
```

Options: `-serial`, `-baud`, `-serialmode`, `-flow`, `-telnet`, `-file`,
`-scrollback`, `-rules`, `-rule`, `-norules`, `-font`, `-display`,
`-geometry` (in pixels), `-fg`/`-foreground`, `-bg`/`-background` and
`-hl`/`-highlight` (cursor line colour). Run `./xlgr -help` for a summary.

## Keys and mouse

| Input | Action |
|---|---|
| Up/Down, `k`/`j` | move the cursor line (starts at the edge of the view) |
| PgUp/PgDn | scroll a page (moves the cursor if it is shown) |
| mouse wheel | scroll 3 lines (1 with Shift); scrolling to the bottom resumes following |
| scrollbar | drag the thumb, or click the trough to page |
| click a line | put the cursor on it |
| Home/`g`, End/`G` | top of the buffer; newest line and follow |
| space | pause or resume following |
| Left/Right | scroll sideways 8 columns |
| `/` text, Enter | regex search (Esc clears); `n`/`N` next/previous |
| `f` text, Enter | show only lines matching a regex; an empty filter clears it |
| Enter or `c` | copy the cursor line (or the newest) to PRIMARY and CLIPBOARD |
| `w` | write the visible lines to a file (prompts for the name) |
| `C` | clear the buffer |
| `q` | quit |

Search and filter patterns are POSIX extended regexes and ignore case unless
the pattern contains an upper-case letter. The status line shows the source,
its connection state, the line count, the active search or filter, and
FOLLOW or PAUSED. Moving the cursor up or scrolling back pauses following.

## Highlighting rules

Rules come from `-rules file`, otherwise from `$XDG_CONFIG_HOME/xlgr/rules`,
`~/.config/xlgr/rules` or `~/.xlgr.rules`, plus any `-rule` options. With none
of those, a few built-in rules colour errors, warnings and hex numbers; use
`-norules` to turn all rules off. The file is re-read when it changes, so you
can tune it while a log is streaming. See `xlgr.rules.example`.

```
/regex/[i]  fg=COLOR bg=COLOR bold underline dim group=N line   # comment
```

- The regex is POSIX extended (use `[0-9]`, not `\d`; GNU `\<` `\>` `\b` work).
  A trailing `i` makes it case-insensitive.
- `fg`/`bg` take X colour names or `#rrggbb`.
- `group=N` colours only parenthesised group N instead of the whole match.
- `line` colours the whole line if the regex matches anywhere.
- `dim` fades the text colour towards the background.
- Each property (fg, bg, styles) is taken from the first rule that sets it, so
  put specific rules first.

## Input handling

Lines end at LF, CRLF or a lone CR. A line is shown as soon as its first
character arrives, so prompts without a newline are visible. Tabs expand to 8
columns, ANSI escape sequences are removed, other control characters show as
`.`, and UTF-8 is converted to Latin-1 for the core X font (characters outside
Latin-1 show as `?`). Lines over 4096 characters are split. Telnet option
negotiation is handled; every option except ECHO and SGA is refused.

The window sets its own icon (`_NET_WM_ICON`). GNOME ignores that and looks up
a desktop entry matching `WM_CLASS`; run `make install` to install the binary,
`xlgr.desktop` and `xlgr.svg` under `~/.local` (or set `PREFIX`), then restart
xlgr.
