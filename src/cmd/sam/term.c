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
#include <sys/ioctl.h>
#include <termios.h>
#include <signal.h>

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
static int viewmode = ModeCmd;
static int term_rows = 24;
static int term_cols = 80;
static int term_raw = 0;
static Posn buf_origin = 0;
static Posn buf_cursor = 0;
static int mouse_enabled = 0;
static int mouse_selecting = 0;
static Posn mouse_sel_start = 0;
static Posn mouse_sel_end = 0;
static int needs_redraw = 1;

/* Input queue for returning characters to sam */
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
static void enter_bufmode(void);
static void exit_bufmode(void);
static void term_getsize(void);
static void term_clear(void);
static void term_goto(int row, int col);
static void term_write(char *s, int n);
static void term_puts(char *s);
static void term_flush(void);
static void term_status(char *msg);
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
term_status(char *msg)
{
	int len, i;

	term_goto(term_rows - 1, 0);
	term_puts(CSI "7m");  /* reverse video */
	len = strlen(msg);
	if(len > term_cols)
		len = term_cols;
	term_write(msg, len);
	for(i = len; i < term_cols; i++)
		term_puts(" ");
	term_puts(CSI "0m");
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
	term_puts(CSI "?1000h");
	term_puts(CSI "?1006h");
	mouse_enabled = 1;

	term_flush();
	term_raw = 1;
	viewmode = ModeBuf;
	needs_redraw = 1;

	/* Position cursor at current dot */
	if(curfile)
		buf_cursor = curfile->dot.r.p1;
	buf_origin = 0;
	if(curfile && buf_cursor > 0)
		buf_origin = file_linestart(curfile, buf_cursor);
}

/*
 * Exit buffer mode: restore terminal to normal state
 */
static void
exit_bufmode(void)
{
	if(!term_raw)
		return;

	/* Disable mouse */
	if(mouse_enabled){
		term_puts(CSI "?1000l");
		term_puts(CSI "?1006l");
		mouse_enabled = 0;
	}

	/* Switch back to main screen (restores previous content) */
	term_puts(CSI "?1049l");
	term_flush();

	/* Restore terminal settings */
	tcsetattr(0, TCSAFLUSH, &orig_termios);
	term_raw = 0;
	viewmode = ModeCmd;
}

void
termcleanup(void)
{
	if(term_raw)
		exit_bufmode();
}

/*
 * Initialize terminal mode.
 * Just check if stdin is a tty - don't change terminal settings.
 */
void
terminit(void)
{
	if(!isatty(0)){
		termmode = 0;
		return;
	}

	termmode = 1;
	viewmode = ModeCmd;
	inputhead = inputtail = 0;

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
		return KEY_ESC;

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

static void
buf_scrollto(Posn p)
{
	int lines;
	Posn pos;

	if(!curfile)
		return;

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
		buf_origin = file_linestart(curfile, p);
	}else if(lines >= term_rows - 2){
		int i;
		buf_origin = file_linestart(curfile, p);
		for(i = 0; i < (term_rows - 2) / 2 && buf_origin > 0; i++)
			buf_origin = file_prevline(curfile, buf_origin);
	}
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
		term_status(" SAM [Buffer] No file  ESC=return ");
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

			if(p >= curfile->dot.r.p1 && p < curfile->dot.r.p2)
				term_puts(CSI "7m");

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

		while(p < curfile->b.nc && filereadc(curfile, p) != '\n')
			p++;
		if(p < curfile->b.nc)
			p++;
	}

	/* Status bar */
	if(curfile->name.s[0]){
		fname = Strtoc(&curfile->name);
		snprint(status, sizeof status, " %s%s  #%ld  Dot:#%ld,#%ld  ESC=return ",
			curfile->mod ? "*" : "", fname,
			buf_cursor, curfile->dot.r.p1, curfile->dot.r.p2);
		free(fname);
	}else{
		snprint(status, sizeof status, " (unnamed)  #%ld  ESC=return ", buf_cursor);
	}
	term_status(status);

	/* Position cursor */
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
	if(viewmode == ModeBuf && term_raw)
		draw_bufmode();
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
		curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
		exit_bufmode();
		break;

	case KEY_UP:
	case 16:  /* Ctrl-P */
		buf_cursor = file_prevline(curfile, buf_cursor);
		needs_redraw = 1;
		break;

	case KEY_DOWN:
	case 14:  /* Ctrl-N */
		buf_cursor = file_nextline(curfile, buf_cursor);
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

	case KEY_PGUP:
		for(i = 0; i < term_rows - 2; i++){
			Posn prev = file_prevline(curfile, buf_cursor);
			if(prev == buf_cursor)
				break;
			buf_cursor = prev;
		}
		needs_redraw = 1;
		break;

	case KEY_PGDN:
	case 22:  /* Ctrl-V */
		for(i = 0; i < term_rows - 2; i++){
			buf_cursor = file_nextline(curfile, buf_cursor);
			if(buf_cursor >= curfile->b.nc){
				buf_cursor = curfile->b.nc;
				break;
			}
		}
		needs_redraw = 1;
		break;

	case 21:  /* Ctrl-U */
		for(i = 0; i < (term_rows - 2) / 2; i++){
			Posn prev = file_prevline(curfile, buf_cursor);
			if(prev == buf_cursor)
				break;
			buf_cursor = prev;
		}
		needs_redraw = 1;
		break;

	case ' ':
		if(curfile->dot.r.p1 == curfile->dot.r.p2){
			curfile->dot.r.p1 = buf_cursor;
			curfile->dot.r.p2 = buf_cursor;
		}else{
			if(buf_cursor < curfile->dot.r.p1)
				curfile->dot.r.p1 = buf_cursor;
			else
				curfile->dot.r.p2 = buf_cursor;
		}
		needs_redraw = 1;
		break;

	case 7:  /* Ctrl-G */
		curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
		needs_redraw = 1;
		break;

	case 23:  /* Ctrl-W */
		if(curfile->dot.r.p1 != curfile->dot.r.p2)
			snarf(curfile, curfile->dot.r.p1, curfile->dot.r.p2, &snarfbuf, 0);
		break;

	case 25:  /* Ctrl-Y */
		if(snarfbuf.nc > 0){
			Posn l, m;
			char cmd[64];
			snprint(cmd, sizeof cmd, "#%ld,#%lda/", buf_cursor, buf_cursor);
			queue_string(cmd);
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
			exit_bufmode();
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

	default:
		break;
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
	int row, col;

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

	while(col < x && p < curfile->b.nc){
		Rune ch = filereadc(curfile, p);
		if(ch == '\n')
			break;
		col++;
		p++;
	}

	btn = button & 3;

	if(btn == 0){
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
 * Main input function for terminal mode.
 * In command mode: read normally, check for ESC to enter buffer mode.
 * In buffer mode: handle navigation keys.
 */
int
terminputc(void)
{
	int key;
	int n, nbuf;
	char buf[UTFmax];
	Rune r;

	/* Return queued characters first */
	if(!queue_empty())
		return dequeue_char();

	if(viewmode == ModeCmd){
		/* Command mode: read like normal -d mode */
		/* But check for ESC to enter buffer mode */
		nbuf = 0;
		do{
			n = read(0, buf+nbuf, 1);
			if(n <= 0)
				return -1;
			nbuf += n;
		}while(!fullrune(buf, nbuf));
		chartorune(&r, buf);

		if(r == 27){  /* ESC */
			enter_bufmode();
			/* Now in buffer mode - fall through to handle it */
		}else{
			return r;
		}
	}

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

		/* If we exited buffer mode, check for queued chars or read next */
		if(viewmode == ModeCmd){
			if(!queue_empty())
				return dequeue_char();
			/* Recursively call to read next command char */
			return terminputc();
		}
	}
}
