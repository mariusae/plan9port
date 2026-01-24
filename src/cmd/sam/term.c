/*
 * Terminal mode interface for sam -d flag
 *
 * When running sam -d with a tty, provides enhanced terminal features:
 * - Starts in normal command mode (exactly like regular -d)
 * - Press ESC to toggle to full-screen buffer mode (view file content)
 * - Buffer mode supports emacs-like navigation and mouse selection
 * - Press ESC again to return to command mode, restoring terminal state
 *
 * If stdin is not a tty, behaves exactly like regular -d mode.
 */

/* Include system headers before plan9port headers to avoid conflicts */

/*
 * Enable GNU/POSIX features for wcwidth() and SIGWINCH.
 * Must be defined before any system headers are included.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <signal.h>
#include <wchar.h>
#include <locale.h>

#include "sam.h"

/* Global: terminal mode available (exported to sam.h) */
int	termmode = 0;

/* Terminal mode states */
enum {
	ModeCmd = 0,	/* command mode - normal terminal, like regular -d */
	ModeBuf = 1,	/* buffer mode - full screen view */
};

/* Terminal state */
static struct termios orig_termios;
static struct termios cmd_termios;  /* non-canonical mode for command mode */
static int viewmode = ModeCmd;
static int term_rows = 24;
static int term_cols = 80;
static int term_raw = 0;
static int term_inited = 0;  /* terminal settings have been modified */
static Posn buf_origin = 0;
static Posn buf_cursor = 0;
static int mouse_enabled = 0;
static int mouse_selecting = 0;
static Posn mouse_sel_start = 0;
static Posn mouse_sel_end = 0;
static int needs_redraw = 1;
static int mark_mode = 0;      /* Emacs-style mark active */
static Posn mark_pos = 0;      /* Position where mark was set */

/* Per-file view state */
#define MAX_FILE_STATES 64
static struct {
	File *file;
	Posn origin;
	Posn cursor;
} file_states[MAX_FILE_STATES];
static int nfile_states = 0;
static File *last_file = nil;  /* Track file switches */

/* Input queue for returning characters to sam */
static Rune inputqueue[4096];
static int inputhead = 0;
static int inputtail = 0;

/* Line buffer for command mode line editing */
static Rune linebuf[4096];
static int linelen = 0;
static int linepos = 0;  /* position in line being returned */
static int linedone = 0; /* line is complete, return chars from linebuf */

/* ANSI escape sequences */
#define ESC "\033"
#define CSI ESC "["

/* Special key codes */
#define KEY_ESC		27
#define KEY_UP		0x100
#define KEY_DOWN	0x101
#define KEY_LEFT	0x102
#define KEY_RIGHT	0x103
#define KEY_HOME	0x104
#define KEY_END		0x105
#define KEY_PGUP	0x106
#define KEY_PGDN	0x107
#define KEY_DEL		0x108
#define KEY_MOUSE	0x200
#define KEY_ALT_BS	0x109  /* Alt/Option + Backspace */
#define KEY_ALT_C	0x10a  /* Alt/Option + C - copy */
#define KEY_ALT_V	0x10b  /* Alt/Option + V - page up (Meta-V) */
#define KEY_ALT_X	0x10c  /* Alt/Option + X - cut to clipboard */
#define KEY_PASTE	0x10d  /* Bracketed paste start */

/* Forward declarations */
static void enter_bufmode(void);
static void exit_bufmode(void);
static void term_getsize(void);
static void term_clear(void);
static void term_goto(int row, int col);
static void term_write(char *s, int n);
static void term_puts(char *s);
static void term_flush(void);
static int term_readkey(void);
static void draw_bufmode(void);
static void handle_bufkey(int key);
static void handle_mouse(void);
static Posn file_linestart(File *f, Posn p);
static Posn file_lineend(File *f, Posn p);
static Posn file_nextline(File *f, Posn p);
static Posn file_prevline(File *f, Posn p);
static void buf_scrollto(Posn p);
static void queue_char(Rune c);
static int dequeue_char(void);
static int queue_empty(void);
static void queue_string(char *s);
static int runewidth(Rune ch);
static int charwidth(Rune ch, int col);
static int count_visual_rows(Posn from, Posn to);
static void save_file_state(File *f);
static void restore_file_state(File *f);
static int read_bracketed_paste(Rune **bufp);

/* Output buffer for terminal */
static char outbuf[16384];
static int outbufn = 0;

static void
term_write(char *s, int n)
{
	while(n > 0){
		int m = sizeof(outbuf) - outbufn;
		if(m > n)
			m = n;
		if(m > 0){
			memmove(outbuf + outbufn, s, m);
			outbufn += m;
			s += m;
			n -= m;
		}
		if(outbufn >= (int)sizeof(outbuf))
			term_flush();
	}
}

static void
term_puts(char *s)
{
	term_write(s, strlen(s));
}

static void
term_flush(void)
{
	if(outbufn > 0){
		write(1, outbuf, outbufn);
		outbufn = 0;
	}
}

static void
term_getsize(void)
{
	struct winsize ws;

	if(ioctl(1, TIOCGWINSZ, &ws) >= 0){
		term_rows = ws.ws_row;
		term_cols = ws.ws_col;
	}
	if(term_rows < 3)
		term_rows = 3;
	if(term_cols < 10)
		term_cols = 10;
}

static void
term_clear(void)
{
	term_puts(CSI "2J");
	term_puts(CSI "H");
}

static void
term_goto(int row, int col)
{
	char buf[32];
	snprint(buf, sizeof buf, CSI "%d;%dH", row + 1, col + 1);
	term_puts(buf);
}

static void
sigwinch_handler(int sig)
{
	USED(sig);
	term_getsize();
	if(viewmode == ModeBuf)
		needs_redraw = 1;
}

/*
 * Enter buffer mode: switch to alternate screen, raw mode
 */
static void
enter_bufmode(void)
{
	struct termios raw;

	if(term_raw)
		return;

	term_getsize();

	/* Save terminal state and switch to alternate screen */
	term_puts(CSI "?1049h");  /* Save cursor & switch to alternate screen */

	/* Enter raw mode */
	if(tcgetattr(0, &orig_termios) == 0){
		raw = orig_termios;
		raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
		raw.c_oflag &= ~(OPOST);
		raw.c_cflag |= (CS8);
		raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
		raw.c_cc[VMIN] = 1;
		raw.c_cc[VTIME] = 0;
		tcsetattr(0, TCSAFLUSH, &raw);
	}

	/* Enable mouse tracking */
	term_puts(CSI "?1000h");  /* basic mouse tracking */
	term_puts(CSI "?1002h");  /* button-event tracking (drag) */
	term_puts(CSI "?1006h");  /* SGR extended mode */
	mouse_enabled = 1;

	/* Enable bracketed paste mode */
	term_puts(CSI "?2004h");

	term_flush();
	term_raw = 1;
	viewmode = ModeBuf;
	needs_redraw = 1;

	/* Handle per-file view state preservation */
	if(curfile){
		/* Save state for previous file if switching */
		if(last_file && last_file != curfile)
			save_file_state(last_file);

		/* Restore state for current file (or initialize from dot if new) */
		restore_file_state(curfile);
		last_file = curfile;
	}
}

/*
 * Exit buffer mode: restore terminal to command mode state
 */
static void
exit_bufmode(void)
{
	if(!term_raw)
		return;

	/* Save current file's view state before exiting */
	if(curfile)
		save_file_state(curfile);

	/* Disable mouse */
	if(mouse_enabled){
		term_puts(CSI "?1000l");
		term_puts(CSI "?1002l");
		term_puts(CSI "?1006l");
		mouse_enabled = 0;
	}

	/* Disable bracketed paste mode */
	term_puts(CSI "?2004l");

	/* Switch back to main screen (restores previous content) */
	term_puts(CSI "?1049l");
	term_flush();

	/* Restore command mode terminal settings (non-canonical, with echo) */
	tcsetattr(0, TCSAFLUSH, &cmd_termios);
	term_raw = 0;
	viewmode = ModeCmd;
}

void
termcleanup(void)
{
	if(term_raw)
		exit_bufmode();
	/* Restore original terminal settings */
	if(term_inited){
		tcsetattr(0, TCSAFLUSH, &orig_termios);
		term_inited = 0;
	}
}

/*
 * Initialize terminal mode.
 * Set terminal to non-canonical mode so ESC is detected immediately.
 */
void
terminit(void)
{
	if(!isatty(0)){
		termmode = 0;
		return;
	}

	/* Initialize locale for proper wcwidth() support */
	setlocale(LC_CTYPE, "");

	termmode = 1;
	viewmode = ModeCmd;
	inputhead = inputtail = 0;

	/* Save original terminal settings */
	if(tcgetattr(0, &orig_termios) == 0){
		/* Set up command mode: non-canonical, no echo (we handle both) */
		cmd_termios = orig_termios;
		cmd_termios.c_lflag &= ~(ICANON | ECHO);  /* disable canonical mode and echo */
		cmd_termios.c_cc[VMIN] = 1;
		cmd_termios.c_cc[VTIME] = 0;
		tcsetattr(0, TCSAFLUSH, &cmd_termios);
		term_inited = 1;
	}

	signal(SIGWINCH, sigwinch_handler);
	atexit(termcleanup);
}

/* Queue management */
static void
queue_char(Rune c)
{
	int next = (inputtail + 1) % (sizeof(inputqueue)/sizeof(inputqueue[0]));
	if(next != inputhead){
		inputqueue[inputtail] = c;
		inputtail = next;
	}
}

static int
dequeue_char(void)
{
	int c;
	if(inputhead == inputtail)
		return -1;
	c = inputqueue[inputhead];
	inputhead = (inputhead + 1) % (sizeof(inputqueue)/sizeof(inputqueue[0]));
	return c;
}

static int
queue_empty(void)
{
	return inputhead == inputtail;
}

static void
queue_string(char *s)
{
	while(*s)
		queue_char(*s++);
}

/*
 * Save view state for a file (origin and cursor positions).
 */
static void
save_file_state(File *f)
{
	int i;

	if(!f)
		return;

	/* Look for existing entry */
	for(i = 0; i < nfile_states; i++){
		if(file_states[i].file == f){
			file_states[i].origin = buf_origin;
			file_states[i].cursor = buf_cursor;
			return;
		}
	}

	/* Add new entry if space available */
	if(nfile_states < MAX_FILE_STATES){
		file_states[nfile_states].file = f;
		file_states[nfile_states].origin = buf_origin;
		file_states[nfile_states].cursor = buf_cursor;
		nfile_states++;
	}
}

/*
 * Restore view state for a file.
 * Returns 1 if state was found and restored, 0 otherwise.
 */
static void
restore_file_state(File *f)
{
	int i;

	if(!f)
		return;

	/* Look for existing entry */
	for(i = 0; i < nfile_states; i++){
		if(file_states[i].file == f){
			buf_origin = file_states[i].origin;
			buf_cursor = file_states[i].cursor;
			/* Clamp to valid range */
			if(buf_origin > f->b.nc)
				buf_origin = f->b.nc > 0 ? file_linestart(f, f->b.nc) : 0;
			if(buf_cursor > f->b.nc)
				buf_cursor = f->b.nc;
			return;
		}
	}

	/* No saved state - initialize from dot */
	buf_cursor = f->dot.r.p1;
	buf_origin = 0;
	if(buf_cursor > 0)
		buf_origin = file_linestart(f, buf_cursor);
}

/* Read a key in raw mode */
static int pending_char = -1;

static int
term_readchar(void)
{
	unsigned char c;
	if(pending_char >= 0){
		int ch = pending_char;
		pending_char = -1;
		return ch;
	}
	if(read(0, &c, 1) != 1)
		return -1;
	return c;
}

/*
 * Read a character with timeout (for escape sequence detection).
 * Returns -1 if no character available within timeout_ms milliseconds.
 */
static int
term_readchar_timeout(int timeout_ms)
{
	fd_set fds;
	struct timeval tv;
	unsigned char c;

	if(pending_char >= 0){
		int ch = pending_char;
		pending_char = -1;
		return ch;
	}

	FD_ZERO(&fds);
	FD_SET(0, &fds);
	tv.tv_sec = timeout_ms / 1000;
	tv.tv_usec = (timeout_ms % 1000) * 1000;

	if(select(1, &fds, NULL, NULL, &tv) <= 0)
		return -1;

	if(read(0, &c, 1) != 1)
		return -1;
	return c;
}

/*
 * Read a UTF-8 character from raw mode input.
 * Returns the full Rune value for multi-byte UTF-8 sequences.
 */
static int
term_readutf8(int first_byte)
{
	char buf[UTFmax];
	int nbuf = 1;
	Rune r;

	buf[0] = first_byte;

	/* Check if this is a UTF-8 lead byte */
	if((first_byte & 0x80) == 0){
		/* ASCII - single byte */
		return first_byte;
	}

	/* Multi-byte UTF-8: read remaining bytes */
	while(!fullrune(buf, nbuf) && nbuf < UTFmax){
		int c = term_readchar_timeout(50);
		if(c < 0)
			break;
		buf[nbuf++] = c;
	}

	if(fullrune(buf, nbuf)){
		chartorune(&r, buf);
		return (int)r;
	}

	/* Invalid UTF-8, return first byte */
	return first_byte;
}

static int
term_readkey(void)
{
	int c = term_readchar();
	if(c < 0)
		return -1;

	/* Check for UTF-8 multi-byte sequence (non-ASCII, non-escape) */
	if(c >= 0x80)
		return term_readutf8(c);

	if(c != 27)
		return c;

	/* Escape sequence - use timeout to detect bare ESC vs sequence */
	c = term_readchar_timeout(50);  /* 50ms timeout */
	if(c < 0 || c == 27)
		return KEY_ESC;

	/* Alt/Option + Backspace */
	if(c == 127 || c == 8)
		return KEY_ALT_BS;

	/* Alt/Option + C - copy */
	if(c == 'c')
		return KEY_ALT_C;

	/* Alt/Option + V - paste */
	if(c == 'v')
		return KEY_ALT_V;

	/* Alt/Option + X - cut to clipboard */
	if(c == 'x')
		return KEY_ALT_X;

	if(c == '['){
		c = term_readchar();
		if(c < 0)
			return KEY_ESC;

		if(c == 'A') return KEY_UP;
		if(c == 'B') return KEY_DOWN;
		if(c == 'C') return KEY_RIGHT;
		if(c == 'D') return KEY_LEFT;
		if(c == 'H') return KEY_HOME;
		if(c == 'F') return KEY_END;

		if(c >= '0' && c <= '9'){
			int num = c - '0';
			c = term_readchar();
			while(c >= '0' && c <= '9'){
				num = num * 10 + (c - '0');
				c = term_readchar();
			}
			if(c == '~'){
				switch(num){
				case 1: return KEY_HOME;
				case 3: return KEY_DEL;
				case 4: return KEY_END;
				case 5: return KEY_PGUP;
				case 6: return KEY_PGDN;
				case 200: return KEY_PASTE;  /* Bracketed paste start */
				}
			}
		}

		if(c == '<')
			return KEY_MOUSE;
	}

	if(c == 'O'){
		c = term_readchar();
		if(c == 'H') return KEY_HOME;
		if(c == 'F') return KEY_END;
	}

	/* Unrecognized escape sequence - ignore and get next key */
	return term_readkey();
}

/*
 * Read bracketed paste content until ESC[201~
 * Returns number of Runes read, allocates buffer at *bufp
 * Caller must free the buffer
 */
static int
read_bracketed_paste(Rune **bufp)
{
	int bufsize = 1024;
	int n = 0;
	Rune *buf;
	int c;
	int escape_state = 0;  /* 0=normal, 1=ESC, 2=ESC[, 3=ESC[2, 4=ESC[20, 5=ESC[201 */

	buf = emalloc(bufsize * sizeof(Rune));

	while((c = term_readchar()) >= 0){
		/* Check for ESC[201~ terminator */
		switch(escape_state){
		case 0:
			if(c == 27) { escape_state = 1; continue; }
			break;
		case 1:
			if(c == '[') { escape_state = 2; continue; }
			/* Not a sequence, output the ESC we skipped */
			if(n + 1 >= bufsize){
				bufsize *= 2;
				buf = erealloc(buf, bufsize * sizeof(Rune));
			}
			buf[n++] = 27;
			escape_state = 0;
			break;
		case 2:
			if(c == '2') { escape_state = 3; continue; }
			/* Not our sequence, output ESC[ */
			if(n + 2 >= bufsize){
				bufsize *= 2;
				buf = erealloc(buf, bufsize * sizeof(Rune));
			}
			buf[n++] = 27;
			buf[n++] = '[';
			escape_state = 0;
			break;
		case 3:
			if(c == '0') { escape_state = 4; continue; }
			if(n + 3 >= bufsize){
				bufsize *= 2;
				buf = erealloc(buf, bufsize * sizeof(Rune));
			}
			buf[n++] = 27;
			buf[n++] = '[';
			buf[n++] = '2';
			escape_state = 0;
			break;
		case 4:
			if(c == '1') { escape_state = 5; continue; }
			if(n + 4 >= bufsize){
				bufsize *= 2;
				buf = erealloc(buf, bufsize * sizeof(Rune));
			}
			buf[n++] = 27;
			buf[n++] = '[';
			buf[n++] = '2';
			buf[n++] = '0';
			escape_state = 0;
			break;
		case 5:
			if(c == '~'){
				/* Found ESC[201~ - end of paste */
				*bufp = buf;
				return n;
			}
			if(n + 5 >= bufsize){
				bufsize *= 2;
				buf = erealloc(buf, bufsize * sizeof(Rune));
			}
			buf[n++] = 27;
			buf[n++] = '[';
			buf[n++] = '2';
			buf[n++] = '0';
			buf[n++] = '1';
			escape_state = 0;
			break;
		}

		/* Handle UTF-8 decoding */
		if((c & 0x80) == 0){
			/* ASCII */
			if(n >= bufsize){
				bufsize *= 2;
				buf = erealloc(buf, bufsize * sizeof(Rune));
			}
			buf[n++] = c;
		}else if((c & 0xe0) == 0xc0){
			/* 2-byte UTF-8 */
			int c2 = term_readchar();
			if(c2 >= 0 && (c2 & 0xc0) == 0x80){
				if(n >= bufsize){
					bufsize *= 2;
					buf = erealloc(buf, bufsize * sizeof(Rune));
				}
				buf[n++] = ((c & 0x1f) << 6) | (c2 & 0x3f);
			}
		}else if((c & 0xf0) == 0xe0){
			/* 3-byte UTF-8 */
			int c2 = term_readchar();
			int c3 = term_readchar();
			if(c2 >= 0 && c3 >= 0 && (c2 & 0xc0) == 0x80 && (c3 & 0xc0) == 0x80){
				if(n >= bufsize){
					bufsize *= 2;
					buf = erealloc(buf, bufsize * sizeof(Rune));
				}
				buf[n++] = ((c & 0x0f) << 12) | ((c2 & 0x3f) << 6) | (c3 & 0x3f);
			}
		}else if((c & 0xf8) == 0xf0){
			/* 4-byte UTF-8 */
			int c2 = term_readchar();
			int c3 = term_readchar();
			int c4 = term_readchar();
			if(c2 >= 0 && c3 >= 0 && c4 >= 0 &&
			   (c2 & 0xc0) == 0x80 && (c3 & 0xc0) == 0x80 && (c4 & 0xc0) == 0x80){
				if(n >= bufsize){
					bufsize *= 2;
					buf = erealloc(buf, bufsize * sizeof(Rune));
				}
				buf[n++] = ((c & 0x07) << 18) | ((c2 & 0x3f) << 12) |
				           ((c3 & 0x3f) << 6) | (c4 & 0x3f);
			}
		}
	}

	*bufp = buf;
	return n;
}

static Posn
file_linestart(File *f, Posn p)
{
	while(p > 0 && filereadc(f, p - 1) != '\n')
		p--;
	return p;
}

static Posn
file_lineend(File *f, Posn p)
{
	while(p < f->b.nc && filereadc(f, p) != '\n')
		p++;
	return p;
}

static Posn
file_nextline(File *f, Posn p)
{
	p = file_lineend(f, p);
	if(p < f->b.nc)
		p++;
	return p;
}

static Posn
file_prevline(File *f, Posn p)
{
	p = file_linestart(f, p);
	if(p > 0)
		p--;
	p = file_linestart(f, p);
	return p;
}

/*
 * Count the number of visual rows from position 'from' to position 'to'.
 * Accounts for line wrapping.
 */
static int
count_visual_rows(Posn from, Posn to)
{
	int rows = 0;
	int col = 0;
	Posn pos = from;
	Rune ch;
	int w;

	while(pos < to && pos < curfile->b.nc){
		ch = filereadc(curfile, pos);
		if(ch == '\n'){
			rows++;
			col = 0;
		}else{
			w = charwidth(ch, col);
			if(col + w > term_cols){
				/* This char wraps to next row */
				rows++;
				col = w;  /* Char is on new row */
			}else{
				col += w;
			}
		}
		pos++;
	}
	return rows;
}

static void
buf_scrollto(Posn p)
{
	int visual_rows;

	if(!curfile)
		return;

	/* Count visual rows from origin to cursor */
	visual_rows = count_visual_rows(buf_origin, p);

	if(p < buf_origin){
		/* Cursor is above the screen - scroll up */
		buf_origin = file_linestart(curfile, p);
	}else if(visual_rows >= term_rows){
		/* Cursor is below the screen - center it */
		int i;
		buf_origin = file_linestart(curfile, p);
		for(i = 0; i < term_rows / 2 && buf_origin > 0; i++)
			buf_origin = file_prevline(curfile, buf_origin);
	}
}

/*
 * Calculate the display width of a Unicode character.
 * Uses wcwidth() for proper Unicode support (CJK, emoji, combining chars).
 * Returns:
 *   0 for combining characters and zero-width chars
 *   1 for most characters
 *   2 for wide characters (CJK, emoji, etc.)
 */
static int
runewidth(Rune ch)
{
	int w;

	/* ASCII fast path */
	if(ch < 0x80){
		if(ch < 32 || ch == 127)
			return 0;  /* Control characters handled separately */
		return 1;
	}

	/* Use wcwidth for Unicode characters */
	w = wcwidth((wchar_t)ch);
	if(w < 0)
		return 1;  /* Non-printable or unknown - assume width 1 */
	return w;
}

/*
 * Calculate the visual column width of a character for display purposes.
 * Handles tabs and control characters specially.
 */
static int
charwidth(Rune ch, int col)
{
	if(ch == '\t')
		return 8 - (col % 8);
	if(ch < 32)
		return 2;  /* ^X - control char display format */
	if(ch == 127)
		return 2;  /* ^? */
	return runewidth(ch);
}

/*
 * Convert a file position to screen (row, col) coordinates.
 * Returns the row, sets *colp to the column.
 * Accounts for line wrapping and tab expansion.
 */
static int
pos_to_screen(Posn p, int *colp)
{
	int row = 0;
	int col = 0;
	Posn pos = buf_origin;
	Rune ch;
	int w;

	while(pos < p && pos < curfile->b.nc && row < term_rows){
		ch = filereadc(curfile, pos);
		if(ch == '\n'){
			row++;
			col = 0;
			pos++;
		}else{
			w = charwidth(ch, col);
			if(col + w > term_cols){
				/* This char wraps to next row */
				row++;
				col = 0;
				/* Don't increment pos - reconsider this char on new row */
			}else{
				col += w;
				pos++;
			}
		}
	}

	*colp = col;
	return row;
}

/*
 * Convert screen (row, col) coordinates to a file position.
 * Accounts for line wrapping and tab expansion.
 */
static Posn
screen_to_pos(int target_row, int target_col)
{
	int row = 0;
	int col = 0;
	Posn pos = buf_origin;
	Rune ch;
	int w;

	while(pos < curfile->b.nc && row < target_row){
		ch = filereadc(curfile, pos);
		if(ch == '\n'){
			row++;
			col = 0;
			pos++;
		}else{
			w = charwidth(ch, col);
			if(col + w > term_cols){
				/* This char wraps to next row */
				row++;
				col = 0;
				/* Don't increment pos - this char belongs to new row */
			}else{
				col += w;
				pos++;
			}
		}
	}

	/* Now find the column on the target row */
	col = 0;
	while(pos < curfile->b.nc && col < target_col){
		ch = filereadc(curfile, pos);
		if(ch == '\n')
			break;
		w = charwidth(ch, col);
		if(col + w > term_cols)
			break;  /* would wrap to next row */
		if(col + w > target_col)
			break;  /* past target */
		col += w;
		pos++;
	}

	return pos;
}

/*
 * Move cursor up one visual line (accounting for wrapping).
 * Tries to maintain the same column position.
 */
static Posn
move_visual_up(Posn p)
{
	int cur_row, cur_col;
	int target_row;

	cur_row = pos_to_screen(p, &cur_col);
	if(cur_row == 0 && buf_origin == 0)
		return p;  /* already at top */

	target_row = cur_row - 1;
	if(target_row < 0){
		/* Need to scroll up first */
		return file_prevline(curfile, p);
	}

	return screen_to_pos(target_row, cur_col);
}

/*
 * Move cursor down one visual line (accounting for wrapping).
 * Tries to maintain the same column position.
 */
static Posn
move_visual_down(Posn p)
{
	int cur_row, cur_col;

	cur_row = pos_to_screen(p, &cur_col);
	return screen_to_pos(cur_row + 1, cur_col);
}

static void
draw_bufmode(void)
{
	int row, col;
	Posn p;
	Rune ch;
	int w;

	if(!curfile){
		term_clear();
		term_goto(0, 0);
		term_puts("No file");
		term_flush();
		return;
	}

	buf_scrollto(buf_cursor);
	term_clear();

	/* Draw file content with wrapping */
	p = buf_origin;
	row = 0;
	col = 0;
	term_goto(row, col);

	while(row < term_rows && p <= curfile->b.nc){
		if(p >= curfile->b.nc)
			break;

		ch = filereadc(curfile, p);

		if(ch == '\n'){
			if(p >= curfile->dot.r.p1 && p < curfile->dot.r.p2){
				term_puts(CSI "7m");
				term_puts(" ");  /* show selected newline */
				term_puts(CSI "0m");
			}
			p++;
			row++;
			col = 0;
			if(row < term_rows)
				term_goto(row, col);
			continue;
		}

		/* Check if character fits on current line */
		w = charwidth(ch, col);
		if(col + w > term_cols){
			/* Wrap to next line */
			row++;
			col = 0;
			if(row >= term_rows)
				break;
			term_goto(row, col);
		}

		/* Draw the character */
		if(p >= curfile->dot.r.p1 && p < curfile->dot.r.p2)
			term_puts(CSI "7m");

		if(ch == '\t'){
			int spaces = 8 - (col % 8);
			while(spaces-- > 0)
				term_puts(" ");
		}else if(ch < 32){
			char ctl[4];
			snprint(ctl, sizeof ctl, "^%c", (int)(ch + '@'));
			term_puts(ctl);
		}else{
			char buf[UTFmax + 1];
			int n = runetochar(buf, &ch);
			buf[n] = 0;
			term_puts(buf);
		}

		if(p >= curfile->dot.r.p1 && p < curfile->dot.r.p2)
			term_puts(CSI "0m");

		col += w;
		p++;
	}

	/* Position cursor */
	row = pos_to_screen(buf_cursor, &col);
	if(row >= term_rows)
		row = term_rows - 1;
	if(col >= term_cols)
		col = term_cols - 1;
	term_goto(row, col);

	term_flush();
}

void
termdraw(void)
{
	if(viewmode == ModeBuf && term_raw)
		draw_bufmode();
}

/*
 * Base64 encoding table
 */
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/*
 * Copy text from file positions p1 to p2 to system clipboard using OSC 52.
 * This escape sequence is supported by most modern terminals (iTerm2, Terminal.app, etc.)
 */
static void
copy_to_clipboard(Posn p1, Posn p2)
{
	Posn p, len;
	Rune ch;
	char *text, *b64text;
	int textlen, b64len, i, j;
	char buf[UTFmax];
	int n;

	if(p1 >= p2 || !curfile)
		return;

	len = p2 - p1;
	if(len > 100000)  /* Limit to 100KB */
		len = 100000;

	/* Collect text */
	text = malloc(len * UTFmax + 1);
	if(!text)
		return;

	textlen = 0;
	for(p = p1; p < p2 && p < curfile->b.nc && textlen < len * UTFmax; p++){
		ch = filereadc(curfile, p);
		n = runetochar(buf, &ch);
		memmove(text + textlen, buf, n);
		textlen += n;
	}

	/* Base64 encode */
	b64len = ((textlen + 2) / 3) * 4;
	b64text = malloc(b64len + 1);
	if(!b64text){
		free(text);
		return;
	}

	j = 0;
	for(i = 0; i < textlen; i += 3){
		int a = (unsigned char)text[i];
		int b = (i + 1 < textlen) ? (unsigned char)text[i + 1] : 0;
		int c = (i + 2 < textlen) ? (unsigned char)text[i + 2] : 0;

		b64text[j++] = b64[a >> 2];
		b64text[j++] = b64[((a & 3) << 4) | (b >> 4)];
		b64text[j++] = (i + 1 < textlen) ? b64[((b & 15) << 2) | (c >> 6)] : '=';
		b64text[j++] = (i + 2 < textlen) ? b64[c & 63] : '=';
	}
	b64text[j] = '\0';

	/* Send OSC 52 sequence */
	term_puts("\033]52;c;");
	term_puts(b64text);
	term_puts("\007");
	term_flush();

	free(text);
	free(b64text);
}

static void
handle_bufkey(int key)
{
	int i;
	Posn tmp;

	if(!curfile){
		if(key == KEY_ESC)
			exit_bufmode();
		return;
	}

	switch(key){
	case KEY_ESC:
		/* Preserve selection (dot) when exiting buffer mode */
		exit_bufmode();
		break;

	case 0:  /* Ctrl-Space - toggle mark for selection */
		if(mark_mode){
			/* Second Ctrl-Space: finalize selection */
			mark_mode = 0;
		}else{
			/* First Ctrl-Space: set mark and start selection */
			mark_mode = 1;
			mark_pos = buf_cursor;
			curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
		}
		needs_redraw = 1;
		break;

	case KEY_UP:
	case 16:  /* Ctrl-P */
		buf_cursor = move_visual_up(buf_cursor);
		needs_redraw = 1;
		break;

	case KEY_DOWN:
	case 14:  /* Ctrl-N */
		buf_cursor = move_visual_down(buf_cursor);
		if(buf_cursor > curfile->b.nc)
			buf_cursor = curfile->b.nc;
		needs_redraw = 1;
		break;

	case KEY_LEFT:
	case 2:  /* Ctrl-B */
		if(buf_cursor > 0)
			buf_cursor--;
		needs_redraw = 1;
		break;

	case KEY_RIGHT:
	case 6:  /* Ctrl-F */
		if(buf_cursor < curfile->b.nc)
			buf_cursor++;
		needs_redraw = 1;
		break;

	case KEY_HOME:
	case 1:  /* Ctrl-A */
		buf_cursor = file_linestart(curfile, buf_cursor);
		needs_redraw = 1;
		break;

	case KEY_END:
	case 5:  /* Ctrl-E */
		buf_cursor = file_lineend(curfile, buf_cursor);
		needs_redraw = 1;
		break;

	case KEY_ALT_V:  /* Alt+V (Meta-V) - page up */
	case KEY_PGUP:
		for(i = 0; i < term_rows; i++){
			Posn prev = file_prevline(curfile, buf_cursor);
			if(prev == buf_cursor)
				break;
			buf_cursor = prev;
		}
		needs_redraw = 1;
		break;

	case KEY_PGDN:
	case 22:  /* Ctrl-V */
		for(i = 0; i < term_rows; i++){
			buf_cursor = file_nextline(curfile, buf_cursor);
			if(buf_cursor >= curfile->b.nc){
				buf_cursor = curfile->b.nc;
				break;
			}
		}
		needs_redraw = 1;
		break;

	case 21:  /* Ctrl-U - kill to beginning of line */
		{
			Posn linestart = file_linestart(curfile, buf_cursor);
			if(linestart < buf_cursor){
				/* Snarf the text first */
				snarf(curfile, linestart, buf_cursor, &snarfbuf, 0);
				/* Delete it */
				logdelete(curfile, linestart, buf_cursor);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
				buf_cursor = linestart;
				curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
			}
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	case 11:  /* Ctrl-K - kill to end of line */
		{
			Posn lineend = file_lineend(curfile, buf_cursor);
			if(buf_cursor < lineend){
				/* Snarf the text first */
				snarf(curfile, buf_cursor, lineend, &snarfbuf, 0);
				/* Delete it */
				logdelete(curfile, buf_cursor, lineend);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
				curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
			}
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	case 7:  /* Ctrl-G - cancel mark/selection */
		mark_mode = 0;
		curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
		needs_redraw = 1;
		break;

	case 23:  /* Ctrl-W - kill region (cut selection) */
		if(curfile->dot.r.p1 != curfile->dot.r.p2){
			/* Snarf the selection first */
			snarf(curfile, curfile->dot.r.p1, curfile->dot.r.p2, &snarfbuf, 0);
			/* Delete it */
			logdelete(curfile, curfile->dot.r.p1, curfile->dot.r.p2);
			if(fileupdate(curfile, FALSE, FALSE))
				seq++;
			buf_cursor = curfile->dot.r.p1;
			curfile->dot.r.p2 = curfile->dot.r.p1;
			needs_redraw = 1;
		}
		mark_mode = 0;
		break;

	case 25:  /* Ctrl-Y - paste */
		if(snarfbuf.nc > 0){
			Posn p0, l, m;
			Rune *buf;

			/* Delete selection first if any */
			if(curfile->dot.r.p1 != curfile->dot.r.p2){
				p0 = curfile->dot.r.p1;
				logdelete(curfile, curfile->dot.r.p1, curfile->dot.r.p2);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
			}else{
				p0 = buf_cursor;
			}

			/* Insert snarfbuf contents */
			buf = fbufalloc();
			for(l = 0; l < snarfbuf.nc; l += m){
				m = snarfbuf.nc - l;
				if(m > RBUFSIZE)
					m = RBUFSIZE;
				bufread(&snarfbuf, l, buf, m);
				loginsert(curfile, p0 + l, buf, m);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
			}
			fbuffree(buf);

			buf_cursor = p0 + snarfbuf.nc;
			curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	case 24:  /* Ctrl-X */
		tmp = curfile->dot.r.p1;
		curfile->dot.r.p1 = buf_cursor;
		buf_cursor = tmp;
		if(curfile->dot.r.p1 > curfile->dot.r.p2){
			tmp = curfile->dot.r.p1;
			curfile->dot.r.p1 = curfile->dot.r.p2;
			curfile->dot.r.p2 = tmp;
		}
		needs_redraw = 1;
		break;

	case KEY_ALT_C:  /* Alt+C - copy to system clipboard */
		if(curfile->dot.r.p1 != curfile->dot.r.p2)
			copy_to_clipboard(curfile->dot.r.p1, curfile->dot.r.p2);
		break;

	case KEY_ALT_X:  /* Alt+X - cut to system clipboard */
		if(curfile->dot.r.p1 != curfile->dot.r.p2){
			/* Copy to internal clipboard and system clipboard */
			snarf(curfile, curfile->dot.r.p1, curfile->dot.r.p2, &snarfbuf, 0);
			copy_to_clipboard(curfile->dot.r.p1, curfile->dot.r.p2);
			/* Delete the selection */
			logdelete(curfile, curfile->dot.r.p1, curfile->dot.r.p2);
			if(fileupdate(curfile, FALSE, FALSE))
				seq++;
			buf_cursor = curfile->dot.r.p1;
			curfile->dot.r.p2 = curfile->dot.r.p1;
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	case KEY_PASTE:  /* Bracketed paste from system clipboard */
		{
			Rune *paste_buf;
			int paste_len;
			Posn p0;

			paste_len = read_bracketed_paste(&paste_buf);
			if(paste_len > 0){
				/* Delete selection first if any */
				if(curfile->dot.r.p1 != curfile->dot.r.p2){
					p0 = curfile->dot.r.p1;
					logdelete(curfile, curfile->dot.r.p1, curfile->dot.r.p2);
					if(fileupdate(curfile, FALSE, FALSE))
						seq++;
				}else{
					p0 = buf_cursor;
				}

				/* Insert pasted content */
				loginsert(curfile, p0, paste_buf, paste_len);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;

				buf_cursor = p0 + paste_len;
				curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
				mark_mode = 0;
				needs_redraw = 1;
			}
			free(paste_buf);
		}
		break;

	case KEY_DEL:  /* Delete key - delete char after cursor or selection */
		{
			Posn p0, p1;
			if(curfile->dot.r.p1 != curfile->dot.r.p2){
				/* Delete selection */
				p0 = curfile->dot.r.p1;
				p1 = curfile->dot.r.p2;
			}else if(buf_cursor < curfile->b.nc){
				/* Delete char after cursor */
				p0 = buf_cursor;
				p1 = buf_cursor + 1;
			}else{
				break;
			}
			logdelete(curfile, p0, p1);
			if(fileupdate(curfile, FALSE, FALSE))
				seq++;
			buf_cursor = p0;
			curfile->dot.r.p1 = curfile->dot.r.p2 = p0;
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	case 127:  /* DEL - backspace */
	case 8:    /* Ctrl-H - backspace */
		{
			Posn p0, p1;
			if(curfile->dot.r.p1 != curfile->dot.r.p2){
				/* Delete selection */
				p0 = curfile->dot.r.p1;
				p1 = curfile->dot.r.p2;
			}else if(buf_cursor > 0){
				/* Delete char before cursor */
				p0 = buf_cursor - 1;
				p1 = buf_cursor;
			}else{
				break;
			}
			logdelete(curfile, p0, p1);
			if(fileupdate(curfile, FALSE, FALSE))
				seq++;
			buf_cursor = p0;
			curfile->dot.r.p1 = curfile->dot.r.p2 = p0;
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	case KEY_ALT_BS:  /* Option/Alt + Backspace - delete previous word */
		{
			Posn p0, p1;
			Rune ch;
			if(curfile->dot.r.p1 != curfile->dot.r.p2){
				/* Delete selection */
				p0 = curfile->dot.r.p1;
				p1 = curfile->dot.r.p2;
			}else if(buf_cursor > 0){
				/* Find start of previous word */
				p1 = buf_cursor;
				p0 = buf_cursor;
				/* Skip trailing whitespace */
				while(p0 > 0){
					ch = filereadc(curfile, p0 - 1);
					if(ch != ' ' && ch != '\t' && ch != '\n')
						break;
					p0--;
				}
				/* Skip word characters */
				while(p0 > 0){
					ch = filereadc(curfile, p0 - 1);
					if(ch == ' ' || ch == '\t' || ch == '\n')
						break;
					p0--;
				}
				if(p0 == p1)
					break;
			}else{
				break;
			}
			logdelete(curfile, p0, p1);
			if(fileupdate(curfile, FALSE, FALSE))
				seq++;
			buf_cursor = p0;
			curfile->dot.r.p1 = curfile->dot.r.p2 = p0;
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	case 31:  /* Ctrl-/ - undo */
		{
			uint p0, p1;
			if(curfile->delta.nc > 0){
				fileundo(curfile, TRUE, 1, &p0, &p1, FALSE);
				buf_cursor = p1;
				curfile->dot.r.p1 = p0;
				curfile->dot.r.p2 = p1;
				mark_mode = 0;
				needs_redraw = 1;
			}
		}
		break;

	case '\r':
	case '\n':  /* Enter - insert newline */
		{
			Posn p0;
			Rune nl = '\n';
			if(curfile->dot.r.p1 != curfile->dot.r.p2){
				/* Delete selection first, then insert */
				p0 = curfile->dot.r.p1;
				logdelete(curfile, curfile->dot.r.p1, curfile->dot.r.p2);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
			}else{
				p0 = buf_cursor;
			}
			loginsert(curfile, p0, &nl, 1);
			if(fileupdate(curfile, FALSE, FALSE))
				seq++;
			buf_cursor = p0 + 1;
			curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	default:
		/*
		 * Printable characters - insert/replace
		 * Allow Unicode: anything >= 32 that isn't a special key code.
		 * Special key codes are 0x100-0x2FF (KEY_UP through KEY_MOUSE range).
		 * Unicode codepoints below 0x100 are allowed (Latin-1 supplement).
		 * Unicode codepoints 0x300+ are allowed (most of Unicode).
		 */
		if(key >= 32 && (key < 0x100 || key >= 0x300)){
			Posn p0;
			Rune r = key;
			if(curfile->dot.r.p1 != curfile->dot.r.p2){
				/* Delete selection first, then insert */
				p0 = curfile->dot.r.p1;
				logdelete(curfile, curfile->dot.r.p1, curfile->dot.r.p2);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
			}else{
				p0 = buf_cursor;
			}
			loginsert(curfile, p0, &r, 1);
			if(fileupdate(curfile, FALSE, FALSE))
				seq++;
			buf_cursor = p0 + 1;
			curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;
	}

	/* Update selection if mark mode is active */
	if(mark_mode && curfile){
		if(buf_cursor < mark_pos){
			curfile->dot.r.p1 = buf_cursor;
			curfile->dot.r.p2 = mark_pos;
		}else{
			curfile->dot.r.p1 = mark_pos;
			curfile->dot.r.p2 = buf_cursor;
		}
		/* Sync selection to system clipboard */
		if(curfile->dot.r.p1 != curfile->dot.r.p2)
			copy_to_clipboard(curfile->dot.r.p1, curfile->dot.r.p2);
	}

	if(buf_cursor < 0)
		buf_cursor = 0;
	if(curfile && buf_cursor > curfile->b.nc)
		buf_cursor = curfile->b.nc;
}

static void
handle_mouse(void)
{
	int button = 0, x = 0, y = 0;
	int c;
	int pressed;
	int btn;
	Posn p;

	c = term_readchar();
	while(c >= '0' && c <= '9'){
		button = button * 10 + (c - '0');
		c = term_readchar();
	}
	if(c != ';') return;

	c = term_readchar();
	while(c >= '0' && c <= '9'){
		x = x * 10 + (c - '0');
		c = term_readchar();
	}
	if(c != ';') return;

	c = term_readchar();
	while(c >= '0' && c <= '9'){
		y = y * 10 + (c - '0');
		c = term_readchar();
	}

	pressed = (c == 'M');
	x--;
	y--;

	if(!curfile)
		return;

	/* Convert screen position (x, y) to file position using wrapping-aware function */
	p = screen_to_pos(y, x);

	btn = button & 3;

	/* Check for motion events first (button 32-35 are motion with button held) */
	if(button >= 32 && button < 64){
		/* Mouse motion while button held (drag) */
		if(mouse_selecting){
			mouse_sel_end = p;
			buf_cursor = p;
			/* Update selection live during drag */
			if(mouse_sel_start <= mouse_sel_end){
				curfile->dot.r.p1 = mouse_sel_start;
				curfile->dot.r.p2 = mouse_sel_end;
			}else{
				curfile->dot.r.p1 = mouse_sel_end;
				curfile->dot.r.p2 = mouse_sel_start;
			}
			needs_redraw = 1;
		}
	}else if(btn == 0){
		/* Left button press/release */
		if(pressed){
			mark_mode = 0;  /* Cancel any keyboard selection */
			mouse_selecting = 1;
			mouse_sel_start = p;
			mouse_sel_end = p;
			buf_cursor = p;
			curfile->dot.r.p1 = p;
			curfile->dot.r.p2 = p;
		}else{
			/* Left button release */
			if(mouse_selecting){
				mouse_selecting = 0;
				mouse_sel_end = p;
				if(mouse_sel_start <= mouse_sel_end){
					curfile->dot.r.p1 = mouse_sel_start;
					curfile->dot.r.p2 = mouse_sel_end;
				}else{
					curfile->dot.r.p1 = mouse_sel_end;
					curfile->dot.r.p2 = mouse_sel_start;
				}
				buf_cursor = p;
				/* Sync selection to system clipboard */
				if(curfile->dot.r.p1 != curfile->dot.r.p2)
					copy_to_clipboard(curfile->dot.r.p1, curfile->dot.r.p2);
			}
		}
		needs_redraw = 1;
	}else if(button == 64){
		int i;
		for(i = 0; i < 3; i++){
			if(buf_origin > 0)
				buf_origin = file_prevline(curfile, buf_origin);
		}
		needs_redraw = 1;
	}else if(button == 65){
		int i;
		for(i = 0; i < 3; i++){
			buf_origin = file_nextline(curfile, buf_origin);
			if(buf_origin >= curfile->b.nc)
				buf_origin = file_linestart(curfile, curfile->b.nc);
		}
		needs_redraw = 1;
	}
}

/*
 * Read a full rune from stdin (for command mode).
 */
static Rune
cmd_readrune(void)
{
	int n, nbuf;
	char buf[UTFmax];
	Rune r;

	nbuf = 0;
	do{
		n = read(0, buf+nbuf, 1);
		if(n <= 0)
			return (Rune)-1;
		nbuf += n;
	}while(!fullrune(buf, nbuf));
	chartorune(&r, buf);
	return r;
}

/*
 * Echo a rune to stdout (for command mode line editing).
 */
static void
cmd_echorune(Rune r)
{
	char buf[UTFmax];
	int n;

	n = runetochar(buf, &r);
	write(1, buf, n);
}

/*
 * Main input function for terminal mode.
 * In command mode: handle line editing, check for ESC to enter buffer mode.
 * In buffer mode: handle navigation keys.
 */
int
terminputc(void)
{
	int key;
	Rune r;

	/* Return queued characters first (from buffer mode operations) */
	if(!queue_empty())
		return dequeue_char();

	/* If we have a completed line, return characters from it */
	if(linedone){
		if(linepos < linelen){
			return linebuf[linepos++];
		}
		/* Line fully returned, reset */
		linedone = 0;
		linelen = 0;
		linepos = 0;
	}

	if(viewmode == ModeCmd){
		/* Command mode: line editing with immediate ESC detection */
		for(;;){
			r = cmd_readrune();
			if(r == (Rune)-1)
				return -1;

			switch(r){
			case 27:  /* ESC */
				enter_bufmode();
				goto bufmode;

			case '\r':
			case '\n':  /* Enter - submit line */
				linebuf[linelen++] = '\n';
				cmd_echorune('\n');
				linedone = 1;
				linepos = 0;
				if(linelen > 0)
					return linebuf[linepos++];
				return '\n';

			case 127:  /* DEL - backspace */
			case 8:    /* Ctrl-H - backspace */
				if(linelen > 0){
					linelen--;
					write(1, "\b \b", 3);
				}
				break;

			case 21:  /* Ctrl-U - kill line */
				while(linelen > 0){
					linelen--;
					write(1, "\b \b", 3);
				}
				break;

			case 23:  /* Ctrl-W - kill word */
				/* Skip trailing spaces */
				while(linelen > 0 && linebuf[linelen-1] == ' '){
					linelen--;
					write(1, "\b \b", 3);
				}
				/* Delete word */
				while(linelen > 0 && linebuf[linelen-1] != ' '){
					linelen--;
					write(1, "\b \b", 3);
				}
				break;

			case 3:  /* Ctrl-C - interrupt */
				/* Clear line and return newline to cancel current input */
				write(1, "^C\n", 3);
				linelen = 0;
				linepos = 0;
				linedone = 0;
				return '\n';

			case 4:  /* Ctrl-D - EOF if line is empty */
				if(linelen == 0)
					return -1;
				break;

			default:
				if(r >= 32 || r == '\t'){
					/* Printable character or tab */
					if(linelen < (int)(sizeof(linebuf)/sizeof(linebuf[0])) - 1){
						linebuf[linelen++] = r;
						cmd_echorune(r);
					}
				}
				/* Ignore other control characters */
				break;
			}
		}
	}

bufmode:
	/* Buffer mode */
	for(;;){
		if(needs_redraw){
			draw_bufmode();
			needs_redraw = 0;
		}

		key = term_readkey();
		if(key < 0)
			return -1;

		if(key == KEY_MOUSE){
			handle_mouse();
			continue;
		}

		handle_bufkey(key);

		/* If we have queued commands, return them to sam for processing */
		if(!queue_empty())
			return dequeue_char();

		/* If we exited buffer mode, read next command char */
		if(viewmode == ModeCmd)
			return terminputc();
	}
}
