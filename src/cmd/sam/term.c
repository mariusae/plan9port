/*
 * Terminal mode interface for sam -d flag
 *
 * When running sam -d with a tty, provides enhanced terminal features:
 * - Starts in command mode (like regular -d)
 * - Press ESC to toggle to buffer mode (view file content)
 * - Buffer mode supports emacs-like navigation and mouse selection
 * - Press ESC again to return to command mode
 *
 * If stdin is not a tty, behaves like regular -d mode.
 */

/* Include system headers before plan9port headers to avoid conflicts */
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>

#include "sam.h"

/* Global: terminal mode active (exported to sam.h) */
int	termmode = 0;

/* Terminal mode states */
enum {
	ModeCmd = 0,	/* command mode - entering sam commands */
	ModeBuf = 1,	/* buffer mode - viewing file content */
};

/* Terminal state */
static struct termios orig_termios;
static int viewmode = ModeCmd;	/* current view: command or buffer */
static int term_rows = 24;
static int term_cols = 80;
static int term_raw = 0;
static int term_initialized = 0;
static Posn buf_origin = 0;	/* first char position shown in buffer view */
static Posn buf_cursor = 0;	/* cursor position in buffer view */
static int mouse_enabled = 0;
static int mouse_selecting = 0;
static Posn mouse_sel_start = 0;
static Posn mouse_sel_end = 0;
static int needs_redraw = 1;

/* Command line buffer for cmd mode - ring buffer of characters to return */
static Rune inputqueue[4096];
static int inputhead = 0;
static int inputtail = 0;

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

/* Forward declarations */
static void term_setraw(void);
static void term_restore(void);
static void term_getsize(void);
static void term_clear(void);
static void term_goto(int row, int col);
static void term_write(char *s, int n);
static void term_puts(char *s);
static void term_flush(void);
static void term_status(char *msg);
static int term_readkey(void);
static void draw_cmdmode(void);
static void draw_bufmode(void);
static int handle_cmdkey(int key);
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
term_setraw(void)
{
	struct termios raw;

	if(term_raw)
		return;

	if(tcgetattr(0, &orig_termios) < 0)
		return;

	raw = orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;

	if(tcsetattr(0, TCSAFLUSH, &raw) < 0)
		return;

	term_raw = 1;
}

static void
term_restore(void)
{
	if(!term_raw)
		return;

	/* Disable mouse */
	if(mouse_enabled){
		term_puts(CSI "?1000l");
		term_puts(CSI "?1006l");
		mouse_enabled = 0;
	}

	/* Show cursor */
	term_puts(CSI "?25h");
	term_flush();

	tcsetattr(0, TCSAFLUSH, &orig_termios);
	term_raw = 0;
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
term_status(char *msg)
{
	int len, i;

	term_goto(term_rows - 1, 0);
	term_puts(CSI "7m");  /* reverse video */
	len = strlen(msg);
	if(len > term_cols)
		len = term_cols;
	term_write(msg, len);
	/* Pad with spaces */
	for(i = len; i < term_cols; i++)
		term_puts(" ");
	term_puts(CSI "0m");  /* reset */
}

static void
sigwinch_handler(int sig)
{
	USED(sig);
	term_getsize();
	needs_redraw = 1;
}

void
termcleanup(void)
{
	if(term_initialized){
		term_restore();
		term_clear();
		term_flush();
	}
}

/*
 * Initialize terminal mode if stdin is a tty.
 * Called from main() when -d flag is set.
 */
void
terminit(void)
{
	/* Check if stdin is a tty */
	if(!isatty(0)){
		termmode = 0;
		return;
	}

	termmode = 1;
	signal(SIGWINCH, sigwinch_handler);

	term_getsize();
	term_setraw();

	/* Enable mouse tracking (SGR extended mode) */
	term_puts(CSI "?1000h");  /* Enable mouse click tracking */
	term_puts(CSI "?1006h");  /* Enable SGR extended mode */
	mouse_enabled = 1;

	/* Show cursor */
	term_puts(CSI "?25h");

	term_flush();

	/* Start in command mode */
	viewmode = ModeCmd;
	inputhead = inputtail = 0;
	term_initialized = 1;

	/* Register cleanup */
	atexit(termcleanup);
}

/* Queue management for returning characters to sam */
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
 * Read a key from the terminal.
 * Returns the key code, or special codes for function keys.
 */
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

static int
term_readkey(void)
{
	int c = term_readchar();
	if(c < 0)
		return -1;

	if(c != 27)
		return c;

	/* Escape sequence */
	c = term_readchar();
	if(c < 0 || c == 27)
		return KEY_ESC;  /* Just escape, or ESC ESC */

	if(c == '['){
		c = term_readchar();
		if(c < 0)
			return KEY_ESC;

		/* CSI sequences */
		if(c == 'A') return KEY_UP;
		if(c == 'B') return KEY_DOWN;
		if(c == 'C') return KEY_RIGHT;
		if(c == 'D') return KEY_LEFT;
		if(c == 'H') return KEY_HOME;
		if(c == 'F') return KEY_END;

		/* CSI with number */
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
				}
			}
		}

		/* Mouse events (SGR mode: ESC [ < ... ) */
		if(c == '<'){
			return KEY_MOUSE;
		}
	}

	if(c == 'O'){
		c = term_readchar();
		if(c == 'H') return KEY_HOME;
		if(c == 'F') return KEY_END;
	}

	/* Unknown escape sequence - ignore */
	return -1;
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
		p++;  /* skip newline */
	return p;
}

static Posn
file_prevline(File *f, Posn p)
{
	p = file_linestart(f, p);
	if(p > 0)
		p--;  /* back over newline */
	p = file_linestart(f, p);
	return p;
}

static void
buf_scrollto(Posn p)
{
	int lines;
	Posn pos;

	if(!curfile)
		return;

	/* Ensure p is visible */
	/* Count lines from buf_origin to p */
	lines = 0;
	pos = buf_origin;

	while(pos < p && pos < curfile->b.nc){
		if(filereadc(curfile, pos) == '\n')
			lines++;
		pos++;
		if(lines >= term_rows - 2)
			break;
	}

	if(p < buf_origin){
		/* Scroll up */
		buf_origin = file_linestart(curfile, p);
	}else if(lines >= term_rows - 2){
		/* Scroll down - put cursor in middle */
		int i;
		buf_origin = file_linestart(curfile, p);
		for(i = 0; i < (term_rows - 2) / 2 && buf_origin > 0; i++)
			buf_origin = file_prevline(curfile, buf_origin);
	}
}

static void
draw_cmdmode(void)
{
	char status[256];
	char *fname;
	int row;
	Posn p;
	Rune ch;

	term_clear();

	/* Show recent output from cmd buffer if available */
	if(cmd && cmd->b.nc > 0){
		/* Find start position - show last N lines that fit */
		Posn start = cmd->b.nc;
		int maxlines = term_rows - 3;
		int lines = 0;

		while(start > 0 && lines < maxlines){
			start--;
			if(filereadc(cmd, start) == '\n')
				lines++;
		}
		if(start > 0)
			start++;  /* Skip the newline we stopped at */

		/* Draw from start */
		p = start;
		row = 0;
		while(p < cmd->b.nc && row < term_rows - 2){
			term_goto(row, 0);
			term_puts(CSI "K");  /* clear line */

			int col = 0;
			while(p < cmd->b.nc && col < term_cols){
				ch = filereadc(cmd, p);
				if(ch == '\n'){
					p++;
					break;
				}else if(ch == '\t'){
					int spaces = 8 - (col % 8);
					while(spaces-- > 0 && col < term_cols){
						term_puts(" ");
						col++;
					}
				}else if(ch < 32){
					char ctl[4];
					snprint(ctl, sizeof ctl, "^%c", (int)(ch + '@'));
					term_puts(ctl);
					col += 2;
				}else{
					char buf[UTFmax + 1];
					int n = runetochar(buf, &ch);
					buf[n] = 0;
					term_puts(buf);
					col++;
				}
				p++;
			}
			/* Skip rest of long line */
			while(p < cmd->b.nc && filereadc(cmd, p) != '\n')
				p++;
			if(p < cmd->b.nc)
				p++;
			row++;
		}
	}

	/* Draw status bar at bottom */
	if(curfile && curfile->name.s[0]){
		fname = Strtoc(&curfile->name);
		snprint(status, sizeof status, " SAM [Cmd] %s%s  Dot:#%ld,#%ld  ESC=buffer ",
			curfile->mod ? "*" : "", fname,
			curfile->dot.r.p1, curfile->dot.r.p2);
		free(fname);
	}else{
		snprint(status, sizeof status, " SAM [Cmd]  ESC=buffer mode ");
	}
	term_status(status);

	/* Position cursor at bottom for command input */
	term_goto(term_rows - 2, 0);
	term_puts(CSI "K");
	term_puts(":");

	term_flush();
}

static void
draw_bufmode(void)
{
	char status[256];
	char *fname;
	int row, col;
	Posn p;
	Rune ch;

	if(!curfile){
		term_clear();
		term_status(" SAM [Buffer] No file  ESC=command mode ");
		term_flush();
		return;
	}

	buf_scrollto(buf_cursor);

	term_clear();

	/* Draw file content */
	p = buf_origin;
	for(row = 0; row < term_rows - 1 && p <= curfile->b.nc; row++){
		term_goto(row, 0);
		col = 0;

		while(p < curfile->b.nc && col < term_cols){
			ch = filereadc(curfile, p);

			/* Highlight selection */
			if(p >= curfile->dot.r.p1 && p < curfile->dot.r.p2){
				term_puts(CSI "7m");  /* reverse */
			}

			if(ch == '\n'){
				if(p >= curfile->dot.r.p1 && p < curfile->dot.r.p2)
					term_puts(CSI "0m");
				p++;
				break;
			}else if(ch == '\t'){
				int spaces = 8 - (col % 8);
				while(spaces-- > 0 && col < term_cols){
					term_puts(" ");
					col++;
				}
			}else if(ch < 32){
				char ctl[4];
				snprint(ctl, sizeof ctl, "^%c", (int)(ch + '@'));
				term_puts(ctl);
				col += 2;
			}else{
				char buf[UTFmax + 1];
				int n = runetochar(buf, &ch);
				buf[n] = 0;
				term_puts(buf);
				col++;
			}

			if(p >= curfile->dot.r.p1 && p < curfile->dot.r.p2)
				term_puts(CSI "0m");

			p++;
		}

		/* Skip rest of long line */
		while(p < curfile->b.nc && filereadc(curfile, p) != '\n')
			p++;
		if(p < curfile->b.nc)
			p++;  /* skip newline */
	}

	/* Draw status bar */
	if(curfile->name.s[0]){
		fname = Strtoc(&curfile->name);
		snprint(status, sizeof status, " SAM [Buf] %s%s  #%ld  Dot:#%ld,#%ld  ESC=cmd ",
			curfile->mod ? "*" : "", fname,
			buf_cursor, curfile->dot.r.p1, curfile->dot.r.p2);
		free(fname);
	}else{
		snprint(status, sizeof status, " SAM [Buf] (unnamed)  #%ld  ESC=cmd ", buf_cursor);
	}
	term_status(status);

	/* Position cursor - find row/col for buf_cursor */
	p = buf_origin;
	row = 0;
	col = 0;
	while(p < buf_cursor && row < term_rows - 1){
		ch = filereadc(curfile, p);
		if(ch == '\n'){
			row++;
			col = 0;
		}else if(ch == '\t'){
			col = (col + 8) & ~7;
		}else{
			col++;
		}
		p++;
	}
	if(col >= term_cols)
		col = term_cols - 1;
	term_goto(row, col);

	term_flush();
}

void
termdraw(void)
{
	if(!term_raw)
		return;

	if(viewmode == ModeCmd)
		draw_cmdmode();
	else
		draw_bufmode();

	needs_redraw = 0;
}

/*
 * Handle key in command mode.
 * Returns 1 if key was consumed (ESC to switch mode), 0 otherwise.
 */
static int
handle_cmdkey(int key)
{
	if(key == KEY_ESC){
		/* Switch to buffer mode */
		viewmode = ModeBuf;
		if(curfile)
			buf_cursor = curfile->dot.r.p1;
		needs_redraw = 1;
		return 1;  /* consumed */
	}
	return 0;  /* not consumed - pass through */
}

static void
handle_bufkey(int key)
{
	int i;
	Posn tmp;

	if(!curfile){
		if(key == KEY_ESC){
			viewmode = ModeCmd;
			needs_redraw = 1;
		}
		return;
	}

	switch(key){
	case KEY_ESC:
		/* Switch to command mode, update dot */
		viewmode = ModeCmd;
		curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
		needs_redraw = 1;
		break;

	/* Emacs-style navigation */
	case KEY_UP:
	case 16:  /* Ctrl-P - previous line */
		buf_cursor = file_prevline(curfile, buf_cursor);
		needs_redraw = 1;
		break;

	case KEY_DOWN:
	case 14:  /* Ctrl-N - next line */
		buf_cursor = file_nextline(curfile, buf_cursor);
		if(buf_cursor > curfile->b.nc)
			buf_cursor = curfile->b.nc;
		needs_redraw = 1;
		break;

	case KEY_LEFT:
	case 2:  /* Ctrl-B - backward char */
		if(buf_cursor > 0)
			buf_cursor--;
		needs_redraw = 1;
		break;

	case KEY_RIGHT:
	case 6:  /* Ctrl-F - forward char */
		if(buf_cursor < curfile->b.nc)
			buf_cursor++;
		needs_redraw = 1;
		break;

	case KEY_HOME:
	case 1:  /* Ctrl-A - beginning of line */
		buf_cursor = file_linestart(curfile, buf_cursor);
		needs_redraw = 1;
		break;

	case KEY_END:
	case 5:  /* Ctrl-E - end of line */
		buf_cursor = file_lineend(curfile, buf_cursor);
		needs_redraw = 1;
		break;

	case KEY_PGUP:
		/* Page up */
		for(i = 0; i < term_rows - 2; i++){
			Posn prev = file_prevline(curfile, buf_cursor);
			if(prev == buf_cursor)
				break;
			buf_cursor = prev;
		}
		needs_redraw = 1;
		break;

	case KEY_PGDN:
	case 22:  /* Ctrl-V - page down */
		for(i = 0; i < term_rows - 2; i++){
			buf_cursor = file_nextline(curfile, buf_cursor);
			if(buf_cursor >= curfile->b.nc){
				buf_cursor = curfile->b.nc;
				break;
			}
		}
		needs_redraw = 1;
		break;

	case 21:  /* Ctrl-U - page up (half screen) */
		for(i = 0; i < (term_rows - 2) / 2; i++){
			Posn prev = file_prevline(curfile, buf_cursor);
			if(prev == buf_cursor)
				break;
			buf_cursor = prev;
		}
		needs_redraw = 1;
		break;

	/* Selection */
	case ' ':  /* Space - set mark / toggle selection mode */
		if(curfile->dot.r.p1 == curfile->dot.r.p2){
			/* Start selection at cursor */
			curfile->dot.r.p1 = buf_cursor;
			curfile->dot.r.p2 = buf_cursor;
		}else{
			/* Extend selection to cursor */
			if(buf_cursor < curfile->dot.r.p1)
				curfile->dot.r.p1 = buf_cursor;
			else
				curfile->dot.r.p2 = buf_cursor;
		}
		needs_redraw = 1;
		break;

	case 7:  /* Ctrl-G - deselect */
		curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
		needs_redraw = 1;
		break;

	case 23:  /* Ctrl-W - copy selection to snarf buffer */
		if(curfile->dot.r.p1 != curfile->dot.r.p2){
			snarf(curfile, curfile->dot.r.p1, curfile->dot.r.p2, &snarfbuf, 0);
		}
		break;

	case 25:  /* Ctrl-Y - yank (paste) at cursor */
		if(snarfbuf.nc > 0){
			Posn l, m;
			/* Queue a paste command */
			char cmd[64];
			snprint(cmd, sizeof cmd, "#%ld,#%lda/", buf_cursor, buf_cursor);
			queue_string(cmd);
			/* Read snarf buffer and queue it */
			for(l = 0; l < snarfbuf.nc; l += m){
				m = snarfbuf.nc - l;
				if(m > BLOCKSIZE)
					m = BLOCKSIZE;
				bufread(&snarfbuf, l, genbuf, m);
				for(Posn k = 0; k < m; k++){
					if(genbuf[k] == '/')
						queue_char('\\');
					queue_char(genbuf[k]);
				}
			}
			queue_string("/\n");
			viewmode = ModeCmd;  /* Switch to process command */
			needs_redraw = 1;
		}
		break;

	case 24:  /* Ctrl-X - swap point and mark */
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

	default:
		break;
	}

	/* Keep cursor in bounds */
	if(buf_cursor < 0)
		buf_cursor = 0;
	if(buf_cursor > curfile->b.nc)
		buf_cursor = curfile->b.nc;
}

static void
handle_mouse(void)
{
	/* Parse SGR mouse event: ESC [ < Cb ; Cx ; Cy M/m */
	int button = 0, x = 0, y = 0;
	int c;
	int pressed;
	int btn;
	Posn p;
	int row, col;

	/* Read button */
	c = term_readchar();
	while(c >= '0' && c <= '9'){
		button = button * 10 + (c - '0');
		c = term_readchar();
	}
	if(c != ';') return;

	/* Read x */
	c = term_readchar();
	while(c >= '0' && c <= '9'){
		x = x * 10 + (c - '0');
		c = term_readchar();
	}
	if(c != ';') return;

	/* Read y */
	c = term_readchar();
	while(c >= '0' && c <= '9'){
		y = y * 10 + (c - '0');
		c = term_readchar();
	}

	pressed = (c == 'M');  /* 'M' = press, 'm' = release */

	/* Convert to 0-indexed */
	x--;
	y--;

	/* Only handle in buffer mode */
	if(viewmode != ModeBuf || !curfile)
		return;

	/* Convert screen position to file position */
	p = buf_origin;
	row = 0;
	col = 0;

	while(row < y && p < curfile->b.nc){
		if(filereadc(curfile, p) == '\n'){
			row++;
			col = 0;
		}else{
			col++;
		}
		p++;
	}

	/* Now find the column */
	while(col < x && p < curfile->b.nc){
		Rune ch = filereadc(curfile, p);
		if(ch == '\n')
			break;
		col++;
		p++;
	}

	btn = button & 3;  /* Button number: 0=left, 1=middle, 2=right */

	if(btn == 0){  /* Left button */
		if(pressed){
			mouse_selecting = 1;
			mouse_sel_start = p;
			mouse_sel_end = p;
			buf_cursor = p;
			curfile->dot.r.p1 = p;
			curfile->dot.r.p2 = p;
		}else{
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
			}
		}
		needs_redraw = 1;
	}else if(button == 64){  /* Scroll up */
		int i;
		for(i = 0; i < 3; i++){
			if(buf_origin > 0)
				buf_origin = file_prevline(curfile, buf_origin);
		}
		needs_redraw = 1;
	}else if(button == 65){  /* Scroll down */
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
 * Main input function for terminal mode.
 * Called from inputc() in cmd.c when termmode is set.
 * Returns characters one at a time to the command parser.
 */
int
terminputc(void)
{
	int key;

	/* First return any queued characters */
	if(!queue_empty())
		return dequeue_char();

	/* Draw UI and get input */
	for(;;){
		if(needs_redraw)
			termdraw();

		key = term_readkey();
		if(key < 0)
			return -1;

		if(key == KEY_MOUSE){
			handle_mouse();
			continue;
		}

		if(viewmode == ModeCmd){
			/* In command mode, check for ESC to switch to buffer mode */
			if(handle_cmdkey(key))
				continue;  /* ESC was consumed, loop back */

			/* Pass through other keys to sam */
			/* Handle special keys */
			if(key >= KEY_UP){
				/* Arrow keys, etc - ignore in command mode */
				continue;
			}

			/* Regular character - return it */
			return key;
		}else{
			/* Buffer mode - handle navigation */
			handle_bufkey(key);
			/* Check if we have commands queued (e.g., from paste) */
			if(!queue_empty())
				return dequeue_char();
		}
	}
}
