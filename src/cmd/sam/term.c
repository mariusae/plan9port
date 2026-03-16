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
#include <sys/time.h>
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
static struct timeval last_click_time = {0, 0};
static Posn last_click_pos = 0;
static int needs_redraw = 1;
static int mark_mode = 0;      /* Emacs-style mark active */
static Posn mark_pos = 0;      /* Position where mark was set */

/* Status message display */
static char status_msg[256];
static struct timeval status_expire = {0, 0};

/* Output capture for buffer mode operations (e.g., Ctrl-S write) */
static char capture_buf[8192];
static int capture_len = 0;
static int capturing = 0;

/* Command overlay window state */
static int overlay_visible = 0;
static Rune overlay_input[4096];
static int overlay_inputlen = 0;
static int overlay_cursor = 0;  /* cursor position within overlay_input */

static char **overlay_history = nil;
static int overlay_hist_count = 0;
static int overlay_hist_cap = 0;
static int overlay_hist_scroll = 0;

/* Overlay mouse selection state */
static int overlay_selecting = 0;
static int overlay_sel_start_line = -1;  /* history index */
static int overlay_sel_start_col = 0;    /* character offset in line */
static int overlay_sel_end_line = -1;
static int overlay_sel_end_col = 0;

/* Screen row to history index mapping (set during draw) */
static int overlay_row_to_hist[256];  /* overlay_row_to_hist[screen_row] = hist index, or -1 */
static int overlay_row_first = -1;    /* first screen row of overlay history */
static int overlay_row_last = -1;     /* last screen row of overlay history */

/* Command recall: indices of command lines in overlay_history */
static int overlay_recall_idx = -1;  /* -1 = not recalling */
static Rune overlay_saved_input[4096];  /* input saved before recall */
static int overlay_saved_len = 0;

/* Per-file view state */
#define MAX_FILE_STATES 64
static struct {
	File *file;
	Posn origin;
	Posn cursor;
	Posn dotp1;	/* dot.r.p1 when state was saved */
	Posn dotp2;	/* dot.r.p2 when state was saved */
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
#define KEY_ALT_V	0x10b  /* Alt/Option + V - page up (Meta-V) */
#define KEY_ALT_W	0x10d  /* Alt/Option + W - copy to clipboard (Emacs-style) */
#define KEY_PASTE	0x10e  /* Bracketed paste start */
#define KEY_ALT_LEFT	0x10f  /* Alt/Option + Left - backward word */
#define KEY_ALT_RIGHT	0x110  /* Alt/Option + Right - forward word */

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
static void set_status(char *msg);
static int runewidth(Rune ch);
static int charwidth(Rune ch, int col);
static int count_visual_rows(Posn from, Posn to);
static void save_file_state(File *f);
static void restore_file_state(File *f);
static int read_bracketed_paste(Rune **bufp);
static int iswordchar(Rune ch);
static void handle_overlay_key(int key);
static void overlay_submit(void);
static void overlay_add_history(char *line);
static int overlay_find_cmd(int n);
static void detect_darkbg(void);
static void overlay_copy_selection(void);
static void overlay_screen_to_pos(int sy, int sx, int *hist_line, int *hist_col);

/* Dark/light mode detection via OSC 11 */
static int term_darkbg = 1;  /* assume dark background by default */

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
	overlay_visible = 0;
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
	overlay_visible = 0;
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
 * If startbuf is true, start directly in buffer mode.
 */
void
terminit(int startbuf)
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

	if(startbuf)
		enter_bufmode();
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
 * Set a status message to display at bottom of screen for 2 seconds.
 */
static void
set_status(char *msg)
{
	strncpy(status_msg, msg, sizeof(status_msg) - 1);
	status_msg[sizeof(status_msg) - 1] = '\0';
	gettimeofday(&status_expire, NULL);
	status_expire.tv_sec += 2;
	needs_redraw = 1;
}

/*
 * Check if status message is still active.
 */
static int
status_active(void)
{
	struct timeval now;
	if(status_msg[0] == '\0')
		return 0;
	gettimeofday(&now, NULL);
	if(now.tv_sec > status_expire.tv_sec ||
	   (now.tv_sec == status_expire.tv_sec && now.tv_usec >= status_expire.tv_usec)){
		status_msg[0] = '\0';
		return 0;
	}
	return 1;
}

/*
 * Check if we're in buffer mode (for suppressing output).
 */
int
in_bufmode(void)
{
	return termmode && viewmode == ModeBuf;
}

/*
 * Start capturing output for buffer mode display.
 */
void
bufmode_capture_start(void)
{
	capture_len = 0;
	capture_buf[0] = '\0';
	capturing = 1;
}

/*
 * End capture and return the captured string (or NULL if nothing captured).
 * Strips trailing newline if present.
 */
char*
bufmode_capture_end(void)
{
	capturing = 0;
	if(capture_len == 0)
		return nil;
	/* Strip trailing newline */
	while(capture_len > 0 && (capture_buf[capture_len-1] == '\n' || capture_buf[capture_len-1] == '\r'))
		capture_buf[--capture_len] = '\0';
	if(capture_len == 0)
		return nil;
	return capture_buf;
}

/*
 * Append to capture buffer (called from termwrite when capturing).
 * Returns 1 if capturing, 0 otherwise.
 */
int
bufmode_capture_append(char *s)
{
	int len;
	if(!capturing)
		return 0;
	len = strlen(s);
	if(capture_len + len >= (int)sizeof(capture_buf) - 1)
		len = sizeof(capture_buf) - 1 - capture_len;
	if(len > 0){
		memcpy(capture_buf + capture_len, s, len);
		capture_len += len;
		capture_buf[capture_len] = '\0';
	}
	return 1;
}

/*
 * Add a line to the overlay history ring buffer.
 */
static void
overlay_add_history(char *line)
{
	if(overlay_hist_count >= overlay_hist_cap){
		int newcap = overlay_hist_cap ? overlay_hist_cap * 2 : 256;
		overlay_history = erealloc(overlay_history, newcap * sizeof(char*));
		overlay_hist_cap = newcap;
	}
	overlay_history[overlay_hist_count] = emalloc(strlen(line) + 1);
	strcpy(overlay_history[overlay_hist_count], line);
	overlay_hist_count++;
	overlay_hist_scroll = 0;
}

/*
 * Map screen coordinates to overlay history line and character position.
 * Returns via pointers; sets *hist_line = -1 if not on a history line.
 */
static void
overlay_screen_to_pos(int sy, int sx, int *hist_line, int *hist_col)
{
	char *text;
	int text_offset, vcol, charpos;

	*hist_line = -1;
	*hist_col = 0;

	if(sy < 0 || sy >= (int)(sizeof(overlay_row_to_hist)/sizeof(overlay_row_to_hist[0])))
		return;

	*hist_line = overlay_row_to_hist[sy];
	if(*hist_line < 0)
		return;

	/* Map screen column to character offset */
	if(overlay_history[*hist_line][0] == '\x01'){
		text = overlay_history[*hist_line] + 1;
		text_offset = 0;
	}else{
		text = overlay_history[*hist_line];
		text_offset = 2;  /* after █ + space */
	}

	vcol = text_offset;
	charpos = 0;
	while(*text && vcol < sx){
		if(*text == '\t'){
			int stop = ((vcol / 8) + 1) * 8;
			vcol = stop;
		}else{
			vcol++;
		}
		text++;
		charpos++;
	}
	*hist_col = charpos;
}

/*
 * Copy overlay selection to system clipboard via OSC 52.
 */
static void
overlay_copy_selection(void)
{
	int s1, sc1, s2, sc2, i;
	char *collected;
	int total, cap;

	if(overlay_sel_start_line < 0 || overlay_sel_end_line < 0)
		return;

	/* Normalize */
	if(overlay_sel_start_line < overlay_sel_end_line ||
	   (overlay_sel_start_line == overlay_sel_end_line &&
	    overlay_sel_start_col <= overlay_sel_end_col)){
		s1 = overlay_sel_start_line; sc1 = overlay_sel_start_col;
		s2 = overlay_sel_end_line; sc2 = overlay_sel_end_col;
	}else{
		s1 = overlay_sel_end_line; sc1 = overlay_sel_end_col;
		s2 = overlay_sel_start_line; sc2 = overlay_sel_start_col;
	}

	if(s1 == s2 && sc1 == sc2)
		return;

	/* Collect selected text */
	cap = 4096;
	collected = malloc(cap);
	if(!collected)
		return;
	total = 0;

	for(i = s1; i <= s2; i++){
		char *text;
		int start_ch, end_ch, len, ch;

		if(overlay_history[i][0] == '\x01')
			text = overlay_history[i] + 1;
		else
			text = overlay_history[i];

		len = strlen(text);
		start_ch = (i == s1) ? sc1 : 0;
		end_ch = (i == s2) ? sc2 : len;
		if(end_ch > len)
			end_ch = len;

		for(ch = start_ch; ch < end_ch; ch++){
			if(total + 2 >= cap){
				cap *= 2;
				collected = realloc(collected, cap);
				if(!collected)
					return;
			}
			collected[total++] = text[ch];
		}

		/* Add newline between lines */
		if(i < s2){
			if(total + 2 >= cap){
				cap *= 2;
				collected = realloc(collected, cap);
				if(!collected)
					return;
			}
			collected[total++] = '\n';
		}
	}
	collected[total] = '\0';

	/* Base64 encode and send OSC 52 */
	{
		static char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		int b64len = ((total + 2) / 3) * 4;
		char *b64text = malloc(b64len + 1);
		int j = 0;

		if(!b64text){
			free(collected);
			return;
		}

		for(i = 0; i < total; i += 3){
			int a = (unsigned char)collected[i];
			int b = (i + 1 < total) ? (unsigned char)collected[i + 1] : 0;
			int c = (i + 2 < total) ? (unsigned char)collected[i + 2] : 0;

			b64text[j++] = b64[a >> 2];
			b64text[j++] = b64[((a & 3) << 4) | (b >> 4)];
			b64text[j++] = (i + 1 < total) ? b64[((b & 15) << 2) | (c >> 6)] : '=';
			b64text[j++] = (i + 2 < total) ? b64[c & 63] : '=';
		}
		b64text[j] = '\0';

		term_puts("\033]52;c;");
		term_puts(b64text);
		term_puts("\007");
		term_flush();

		free(b64text);
	}

	free(collected);
}

/*
 * Submit the current overlay input as a sam command.
 */
static void
overlay_submit(void)
{
	char cmd[8192];
	char prompt_line[8192];
	int i, n;

	if(overlay_inputlen == 0){
		/* Empty input: repeat last command, append output */
		int idx = overlay_find_cmd(0);
		if(idx < 0)
			return;
		/* Re-execute the last command without adding a new history line */
		bufmode_capture_start();
		queue_string(overlay_history[idx] + 1);  /* skip \x01 marker */
		queue_char('\n');
		overlay_hist_scroll = 0;
		needs_redraw = 1;
		return;
	}

	/* Convert Rune input to UTF-8 */
	n = 0;
	for(i = 0; i < overlay_inputlen && n < (int)sizeof(cmd) - 2; i++){
		char buf[UTFmax];
		int nb = runetochar(buf, &overlay_input[i]);
		if(n + nb < (int)sizeof(cmd) - 2){
			memcpy(cmd + n, buf, nb);
			n += nb;
		}
	}
	cmd[n] = '\0';

	/* Add command to history with \x01 marker prefix for command lines */
	snprint(prompt_line, sizeof(prompt_line), "\x01%s", cmd);
	overlay_add_history(prompt_line);

	/* Execute: capture output, queue command */
	bufmode_capture_start();
	queue_string(cmd);
	queue_char('\n');

	/* Clear input */
	overlay_inputlen = 0;
	overlay_cursor = 0;
	overlay_hist_scroll = 0;
	needs_redraw = 1;
}

/*
 * Handle a key press when the overlay is visible.
 */
/*
 * Find the nth command line in overlay history, searching backwards.
 * n=0 is the most recent command, n=1 the one before, etc.
 * Returns the history index, or -1 if not found.
 */
static int
overlay_find_cmd(int n)
{
	int i, count;

	count = 0;
	for(i = overlay_hist_count - 1; i >= 0; i--){
		if(overlay_history[i][0] == '\x01'){
			if(count == n)
				return i;
			count++;
		}
	}
	return -1;
}

/*
 * Load a command from history into the overlay input buffer.
 */
static void
overlay_load_cmd(int hist_idx)
{
	char *s;
	int i, n;
	Rune r;

	s = overlay_history[hist_idx] + 1;  /* skip \x01 marker */
	overlay_inputlen = 0;
	n = strlen(s);
	i = 0;
	while(i < n && overlay_inputlen < (int)(sizeof(overlay_input)/sizeof(overlay_input[0])) - 1){
		i += chartorune(&r, s + i);
		overlay_input[overlay_inputlen++] = r;
	}
	overlay_cursor = overlay_inputlen;
}

static void
handle_overlay_key(int key)
{
	switch(key){
	case 7:  /* Ctrl-G */
	case KEY_ESC:
		overlay_visible = 0;
		overlay_recall_idx = -1;
		needs_redraw = 1;
		break;

	case '\r':
	case '\n':
		overlay_recall_idx = -1;
		overlay_submit();
		break;

	case 127:  /* DEL */
	case 8:    /* Ctrl-H / Backspace */
		if(overlay_cursor > 0){
			memmove(&overlay_input[overlay_cursor-1], &overlay_input[overlay_cursor],
				(overlay_inputlen - overlay_cursor) * sizeof(Rune));
			overlay_cursor--;
			overlay_inputlen--;
			needs_redraw = 1;
		}
		break;

	case KEY_DEL:  /* Forward delete */
		if(overlay_cursor < overlay_inputlen){
			memmove(&overlay_input[overlay_cursor], &overlay_input[overlay_cursor+1],
				(overlay_inputlen - overlay_cursor - 1) * sizeof(Rune));
			overlay_inputlen--;
			needs_redraw = 1;
		}
		break;

	case KEY_LEFT:
	case 2:  /* Ctrl-B */
		if(overlay_cursor > 0){
			overlay_cursor--;
			needs_redraw = 1;
		}
		break;

	case KEY_RIGHT:
	case 6:  /* Ctrl-F */
		if(overlay_cursor < overlay_inputlen){
			overlay_cursor++;
			needs_redraw = 1;
		}
		break;

	case 1:  /* Ctrl-A - beginning of line */
	case KEY_HOME:
		if(overlay_cursor != 0){
			overlay_cursor = 0;
			needs_redraw = 1;
		}
		break;

	case 5:  /* Ctrl-E - end of line */
	case KEY_END:
		if(overlay_cursor != overlay_inputlen){
			overlay_cursor = overlay_inputlen;
			needs_redraw = 1;
		}
		break;

	case KEY_UP:
	case 16:  /* Ctrl-P */
		{
			int next_recall, idx;
			next_recall = overlay_recall_idx + 1;
			idx = overlay_find_cmd(next_recall);
			if(idx >= 0){
				if(overlay_recall_idx == -1){
					/* Save current input before first recall */
					memmove(overlay_saved_input, overlay_input,
						overlay_inputlen * sizeof(Rune));
					overlay_saved_len = overlay_inputlen;
				}
				overlay_recall_idx = next_recall;
				overlay_load_cmd(idx);
				needs_redraw = 1;
			}
		}
		break;

	case KEY_DOWN:
	case 14:  /* Ctrl-N */
		if(overlay_recall_idx > 0){
			int idx;
			overlay_recall_idx--;
			idx = overlay_find_cmd(overlay_recall_idx);
			if(idx >= 0)
				overlay_load_cmd(idx);
			needs_redraw = 1;
		}else if(overlay_recall_idx == 0){
			/* Restore saved input */
			overlay_recall_idx = -1;
			memmove(overlay_input, overlay_saved_input,
				overlay_saved_len * sizeof(Rune));
			overlay_inputlen = overlay_saved_len;
			overlay_cursor = overlay_inputlen;
			needs_redraw = 1;
		}
		break;

	case 21:  /* Ctrl-U - kill line */
		overlay_inputlen = 0;
		overlay_cursor = 0;
		needs_redraw = 1;
		break;

	case 23:  /* Ctrl-W - kill word */
		if(overlay_cursor > 0){
			int old_cursor = overlay_cursor;
			/* Skip trailing spaces */
			while(overlay_cursor > 0 && overlay_input[overlay_cursor-1] == ' ')
				overlay_cursor--;
			/* Delete word */
			while(overlay_cursor > 0 && overlay_input[overlay_cursor-1] != ' ')
				overlay_cursor--;
			memmove(&overlay_input[overlay_cursor], &overlay_input[old_cursor],
				(overlay_inputlen - old_cursor) * sizeof(Rune));
			overlay_inputlen -= (old_cursor - overlay_cursor);
			needs_redraw = 1;
		}
		break;

	case 11:  /* Ctrl-K - clear history */
		{
			int k;
			for(k = 0; k < overlay_hist_count; k++)
				free(overlay_history[k]);
		}
		overlay_hist_count = 0;
		overlay_hist_scroll = 0;
		overlay_recall_idx = -1;
		needs_redraw = 1;
		break;

	case KEY_PGUP:
		{
			int oh, hist_visible, max_scroll;
			oh = 1 + overlay_hist_count + 1 + 1;
			if(oh > term_rows / 2)
				oh = term_rows / 2;
			if(oh < 3)
				oh = 3;
			hist_visible = oh - 3;
			max_scroll = overlay_hist_count - hist_visible;
			if(max_scroll < 0)
				max_scroll = 0;
			overlay_hist_scroll += 5;
			if(overlay_hist_scroll > max_scroll)
				overlay_hist_scroll = max_scroll;
		}
		needs_redraw = 1;
		break;

	case KEY_PGDN:
		overlay_hist_scroll -= 5;
		if(overlay_hist_scroll < 0)
			overlay_hist_scroll = 0;
		needs_redraw = 1;
		break;

	default:
		if(key >= 32 && overlay_inputlen < (int)(sizeof(overlay_input)/sizeof(overlay_input[0])) - 1){
			/* Insert at cursor position */
			memmove(&overlay_input[overlay_cursor+1], &overlay_input[overlay_cursor],
				(overlay_inputlen - overlay_cursor) * sizeof(Rune));
			overlay_input[overlay_cursor] = key;
			overlay_cursor++;
			overlay_inputlen++;
			needs_redraw = 1;
		}else if(key < 32 || key >= KEY_UP){
			/* Unknown control key or special key: dismiss overlay, pass to buffer */
			overlay_visible = 0;
			overlay_recall_idx = -1;
			handle_bufkey(key);
		}
		break;
	}
}

/*
 * Display a message inverted at the bottom of the screen (just the message length,
 * not a full status bar), then wait for any key or mouse event before continuing.
 * The key/mouse event is passed through and processed normally.
 * This is used to show output from operations like Ctrl-S write.
 */
static void
show_output_and_wait(char *msg)
{
	int key, slen;

	if(!msg || !msg[0])
		return;

	/* Draw the buffer first */
	draw_bufmode();

	/* Draw the message inverted at bottom, just the message length */
	term_goto(term_rows - 1, 0);
	term_puts(CSI "7m");  /* inverse video */
	slen = strlen(msg);
	if(slen > term_cols)
		slen = term_cols;
	term_write(msg, slen);
	term_puts(CSI "0m");  /* reset */
	term_flush();

	/* Wait for any key or mouse event, then process it */
	key = term_readkey();
	if(key >= 0){
		if(key == KEY_MOUSE)
			handle_mouse();
		else
			handle_bufkey(key);
	}

	/* Redraw normally */
	needs_redraw = 1;
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
			file_states[i].dotp1 = f->dot.r.p1;
			file_states[i].dotp2 = f->dot.r.p2;
			return;
		}
	}

	/* Add new entry if space available */
	if(nfile_states < MAX_FILE_STATES){
		file_states[nfile_states].file = f;
		file_states[nfile_states].origin = buf_origin;
		file_states[nfile_states].cursor = buf_cursor;
		file_states[nfile_states].dotp1 = f->dot.r.p1;
		file_states[nfile_states].dotp2 = f->dot.r.p2;
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
			if(f->dot.r.p1 != file_states[i].dotp1
			|| f->dot.r.p2 != file_states[i].dotp2){
				/* Dot changed since we saved - scroll to new selection */
				buf_cursor = f->dot.r.p1;
				buf_origin = file_linestart(f, buf_cursor);
				/* Center the selection on screen */
				for(i = 0; i < term_rows / 2 && buf_origin > 0; i++)
					buf_origin = file_prevline(f, buf_origin);
			}else{
				buf_origin = file_states[i].origin;
				buf_cursor = file_states[i].cursor;
				/* Clamp to valid range */
				if(buf_origin > f->b.nc)
					buf_origin = f->b.nc > 0 ? file_linestart(f, f->b.nc) : 0;
				if(buf_cursor > f->b.nc)
					buf_cursor = f->b.nc;
			}
			return;
		}
	}

	/* No saved state - initialize from dot */
	buf_cursor = f->dot.r.p1;
	buf_origin = file_linestart(f, buf_cursor);
	/* Center the selection on screen */
	for(i = 0; i < term_rows / 2 && buf_origin > 0; i++)
		buf_origin = file_prevline(f, buf_origin);
}

/*
 * Detect terminal background color via OSC 11.
 * Sends OSC 11 query, parses response to determine dark vs light background.
 * Sets term_darkbg accordingly. Must be called when terminal is in raw mode.
 */
static void
detect_darkbg(void)
{
	fd_set fds;
	struct timeval tv;
	unsigned char buf[128];
	int n;
	unsigned long r, g, b;
	char *p;

	/* Flush any pending output first */
	term_flush();

	/* Send OSC 11 query: request background color */
	write(1, "\033]11;?\033\\", 8);

	/* Wait for response with timeout */
	FD_ZERO(&fds);
	FD_SET(0, &fds);
	tv.tv_sec = 0;
	tv.tv_usec = 200000;  /* 200ms timeout */

	if(select(1, &fds, NULL, NULL, &tv) <= 0)
		return;  /* no response, keep current setting */

	/* Read response: ESC ] 11 ; rgb:RRRR/GGGG/BBBB ESC \ (or BEL) */
	n = 0;
	while(n < (int)sizeof(buf) - 1){
		FD_ZERO(&fds);
		FD_SET(0, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = 50000;  /* 50ms between chars */
		if(select(1, &fds, NULL, NULL, &tv) <= 0)
			break;
		if(read(0, &buf[n], 1) != 1)
			break;
		/* Stop at BEL or backslash (ST terminator) */
		if(buf[n] == '\007')
			break;
		if(n > 0 && buf[n] == '\\' && buf[n-1] == '\033')
			break;
		n++;
	}
	buf[n] = '\0';

	/* Parse: look for "rgb:" followed by hex/hex/hex */
	p = strstr((char*)buf, "rgb:");
	if(p == nil)
		return;
	p += 4;

	/* Parse hex color components (variable length: 1-4 hex digits each) */
	r = strtoul(p, &p, 16);
	if(*p != '/') return;
	p++;
	g = strtoul(p, &p, 16);
	if(*p != '/') return;
	p++;
	b = strtoul(p, &p, 16);

	/* Normalize to 8-bit range */
	/* Components may be 1, 2, 3, or 4 hex digits (4, 8, 12, or 16 bits) */
	if(r > 0xFF || g > 0xFF || b > 0xFF){
		if(r > 0xFFF || g > 0xFFF || b > 0xFFF){
			/* 16-bit values */
			r >>= 8;
			g >>= 8;
			b >>= 8;
		}else{
			/* 12-bit values */
			r >>= 4;
			g >>= 4;
			b >>= 4;
		}
	}

	/* Compute perceived luminance (ITU-R BT.601) */
	/* Y = 0.299*R + 0.587*G + 0.114*B */
	{
		int lum = (299 * (int)r + 587 * (int)g + 114 * (int)b) / 1000;
		term_darkbg = (lum < 128);
	}
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

	/* Alt/Option + V - page up (Meta-V) */
	if(c == 'v')
		return KEY_ALT_V;

	/* Alt/Option + W - copy to clipboard (Emacs-style) */
	if(c == 'w')
		return KEY_ALT_W;

	/* ESC b - backward word (Meta-b, Option+Left on some terminals) */
	if(c == 'b')
		return KEY_ALT_LEFT;

	/* ESC f - forward word (Meta-f, Option+Right on some terminals) */
	if(c == 'f')
		return KEY_ALT_RIGHT;

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
			/* Handle modifier sequences: ESC [ 1 ; <mod> <dir> */
			if(c == ';'){
				int mod;
				c = term_readchar();
				if(c < '0' || c > '9')
					return term_readkey();
				mod = c - '0';
				c = term_readchar();
				while(c >= '0' && c <= '9'){
					mod = mod * 10 + (c - '0');
					c = term_readchar();
				}
				/* mod 3 = Alt, mod 5 = Ctrl, mod 9 = Alt (some terminals) */
				if(mod == 3 || mod == 9){
					if(c == 'C') return KEY_ALT_RIGHT;
					if(c == 'D') return KEY_ALT_LEFT;
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
			/* ASCII - convert CR and CRLF to LF */
			if(c == '\r'){
				/* Peek at next char to check for CRLF */
				int next = term_readchar_timeout(10);
				if(next >= 0 && next != '\n'){
					/* Not CRLF, push back the char we peeked */
					pending_char = next;
				}
				/* Convert CR (or CRLF) to LF */
				c = '\n';
			}
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
		return 4 - (col % 4);
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
	int content_rows;
	int overlay_height;

	if(!curfile){
		term_clear();
		term_goto(0, 0);
		term_puts("No file");
		term_flush();
		return;
	}

	/* Detect file switch or first display after entering buffer mode */
	if(curfile != last_file){
		if(last_file)
			save_file_state(last_file);
		restore_file_state(curfile);
		last_file = curfile;
	}

	/* Compute overlay height: pad + history + prompt + pad, max half screen */
	overlay_height = 0;
	if(overlay_visible){
		overlay_height = 1 + overlay_hist_count + 1 + 1;  /* top pad + history + prompt + bottom pad */
		if(overlay_height > term_rows / 2)
			overlay_height = term_rows / 2;
		if(overlay_height < 3)
			overlay_height = 3;  /* at minimum: pad + prompt + pad */
	}

	content_rows = term_rows - overlay_height;

	buf_scrollto(buf_cursor);
	term_clear();
	term_puts(CSI "0m");  /* Reset attributes to ensure clean state */

	/* Draw file content with wrapping */
	p = buf_origin;
	row = 0;
	col = 0;
	term_goto(row, col);

	while(row < content_rows && p <= curfile->b.nc){
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
			if(row < content_rows)
				term_goto(row, col);
			continue;
		}

		/* Check if character fits on current line */
		w = charwidth(ch, col);
		if(col + w > term_cols){
			/* Wrap to next line */
			row++;
			col = 0;
			if(row >= content_rows)
				break;
			term_goto(row, col);
		}

		/* Draw the character */
		if(p >= curfile->dot.r.p1 && p < curfile->dot.r.p2)
			term_puts(CSI "7m");

		if(ch == '\t'){
			int spaces = 4 - (col % 4);
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

	if(overlay_visible){
		/* Draw overlay window with background adapted to dark/light mode */
		int orow, i, j, hist_visible, hist_start, prompt_row;
		char overlay_bg[32], output_fg[32], cmd_fg[32], input_fg[32];

		/*
		 * Color scheme:
		 * cmd_fg: bold, used for entered command history lines
		 * input_fg: normal weight, used for current input being typed
		 */
		if(term_darkbg){
			snprint(overlay_bg, sizeof overlay_bg, CSI "48;2;65;67;75m");
			snprint(output_fg, sizeof output_fg, CSI "38;2;180;180;190m");
			snprint(cmd_fg, sizeof cmd_fg, CSI "1m");
			snprint(input_fg, sizeof input_fg, CSI "0m");
		}else{
			snprint(overlay_bg, sizeof overlay_bg, CSI "48;2;220;220;220m");
			snprint(output_fg, sizeof output_fg, CSI "38;2;80;80;95m");
			snprint(cmd_fg, sizeof cmd_fg, CSI "1;38;2;30;30;40m");
			snprint(input_fg, sizeof input_fg, CSI "0m" CSI "38;2;30;30;40m");
		}

		orow = content_rows;

		/* Initialize row-to-hist mapping */
		memset(overlay_row_to_hist, -1, sizeof(overlay_row_to_hist));
		overlay_row_first = -1;
		overlay_row_last = -1;

		/* Top padding line */
		term_goto(orow, 0);
		term_puts(overlay_bg);
		for(j = 0; j < term_cols; j++)
			term_puts(" ");
		term_puts(CSI "0m");
		orow++;

		/* How many history lines can we show? */
		hist_visible = overlay_height - 3;  /* minus top pad, prompt, bottom pad */
		if(hist_visible < 0)
			hist_visible = 0;

		/* Compute which history lines to show */
		hist_start = overlay_hist_count - overlay_hist_scroll - hist_visible;
		if(hist_start < 0)
			hist_start = 0;

		prompt_row = term_rows - 2;  /* second-to-last row */

		/* Normalize selection so s1/sc1 <= s2/sc2 */
		{
			int s1, sc1, s2, sc2;
			if(overlay_sel_start_line < 0 || overlay_sel_end_line < 0){
				s1 = s2 = sc1 = sc2 = -1;
			}else if(overlay_sel_start_line < overlay_sel_end_line ||
			         (overlay_sel_start_line == overlay_sel_end_line &&
			          overlay_sel_start_col <= overlay_sel_end_col)){
				s1 = overlay_sel_start_line; sc1 = overlay_sel_start_col;
				s2 = overlay_sel_end_line; sc2 = overlay_sel_end_col;
			}else{
				s1 = overlay_sel_end_line; sc1 = overlay_sel_end_col;
				s2 = overlay_sel_start_line; sc2 = overlay_sel_start_col;
			}

		for(i = hist_start; i < overlay_hist_count - overlay_hist_scroll && orow < prompt_row; i++){
			char *text;
			int text_offset;  /* column offset for gutter */
			int in_sel;

			/* Record mapping */
			if(orow < (int)(sizeof(overlay_row_to_hist)/sizeof(overlay_row_to_hist[0])))
				overlay_row_to_hist[orow] = i;
			if(overlay_row_first < 0)
				overlay_row_first = orow;
			overlay_row_last = orow;

			term_goto(orow, 0);
			term_puts(overlay_bg);
			if(overlay_history[i][0] == '\x01'){
				/* Command line: flush left, bold */
				term_puts(cmd_fg);
				term_puts(overlay_bg);
				text = overlay_history[i] + 1;
				text_offset = 0;
			}else{
				/* Output line: full block gutter + text */
				term_puts(output_fg);
				term_puts(overlay_bg);
				term_puts("\xe2\x96\x88 ");  /* U+2588 █ + space */
				text = overlay_history[i];
				text_offset = 2;
			}

			/* Render text character by character with selection highlighting */
			{
				char *s = text;
				int vcol = text_offset;
				int charpos = 0;
				while(*s && vcol < term_cols){
					/* Check if this character is in selection */
					in_sel = 0;
					if(s1 >= 0 && i >= s1 && i <= s2){
						if(i > s1 && i < s2)
							in_sel = 1;
						else if(i == s1 && i == s2)
							in_sel = (charpos >= sc1 && charpos < sc2);
						else if(i == s1)
							in_sel = (charpos >= sc1);
						else /* i == s2 */
							in_sel = (charpos < sc2);
					}

					if(in_sel)
						term_puts(CSI "7m");  /* inverse */

					if(*s == '\t'){
						int stop = ((vcol / 8) + 1) * 8;
						if(stop > term_cols) stop = term_cols;
						while(vcol < stop){
							term_puts(" ");
							vcol++;
						}
					}else{
						term_write(s, 1);
						vcol++;
					}

					if(in_sel){
						term_puts(CSI "27m");  /* un-inverse */
						/* Restore colors */
						if(overlay_history[i][0] == '\x01'){
							term_puts(cmd_fg);
						}else{
							term_puts(output_fg);
						}
						term_puts(overlay_bg);
					}
					s++;
					charpos++;
				}
				for(j = vcol; j < term_cols; j++)
					term_puts(" ");
			}
			term_puts(CSI "0m");
			orow++;
		}
		} /* end selection normalization scope */

		/* Draw prompt line with background */
		term_goto(prompt_row, 0);
		term_puts(overlay_bg);
		term_puts(input_fg);
		term_puts(overlay_bg);

		/* Draw current input, tracking cursor column */
		col = 0;
		{
			int cursor_col = 0;
			for(i = 0; i < overlay_inputlen && col < term_cols; i++){
				char buf[UTFmax + 1];
				int n = runetochar(buf, &overlay_input[i]);
				buf[n] = 0;
				if(i == overlay_cursor)
					cursor_col = col;
				term_puts(buf);
				col += charwidth(overlay_input[i], col);
			}
			if(overlay_cursor >= overlay_inputlen)
				cursor_col = col;

			/* Pad rest of prompt line with background */
			for(i = col; i < term_cols; i++)
				term_puts(" ");
			term_puts(CSI "0m");

			/* Bottom padding line */
			term_goto(term_rows - 1, 0);
			term_puts(overlay_bg);
			for(i = 0; i < term_cols; i++)
				term_puts(" ");
			term_puts(CSI "0m");

			/* Position cursor on prompt line */
			term_goto(prompt_row, cursor_col);
		}
	}else{
		/* Draw status message at bottom if active */
		if(status_active()){
			int slen, i;
			term_goto(term_rows - 1, 0);
			term_puts(CSI "7m");  /* inverse video */
			slen = strlen(status_msg);
			if(slen > term_cols)
				slen = term_cols;
			term_write(status_msg, slen);
			/* Pad with spaces to fill the line */
			for(i = slen; i < term_cols; i++)
				term_puts(" ");
			term_puts(CSI "0m");
		}

		/* Position cursor */
		row = pos_to_screen(buf_cursor, &col);
		if(row >= term_rows)
			row = term_rows - 1;
		if(col >= term_cols)
			col = term_cols - 1;
		term_goto(row, col);
	}

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

/*
 * Look: search for next occurrence of selected text.
 * Returns 1 if found, 0 if not.
 */
static int
look_forward(void)
{
	Posn p1, p2, len, pos, match_start;
	Rune ch, target;
	int matched;

	if(!curfile)
		return 0;

	p1 = curfile->dot.r.p1;
	p2 = curfile->dot.r.p2;
	len = p2 - p1;

	if(len <= 0)
		return 0;

	/* Search starting after current selection */
	pos = p2;
	while(pos <= curfile->b.nc - len){
		/* Try to match at this position */
		matched = 1;
		for(Posn i = 0; i < len; i++){
			target = filereadc(curfile, p1 + i);
			ch = filereadc(curfile, pos + i);
			if(ch != target){
				matched = 0;
				break;
			}
		}
		if(matched){
			match_start = pos;
			/* Found - update selection and cursor */
			curfile->dot.r.p1 = match_start;
			curfile->dot.r.p2 = match_start + len;
			buf_cursor = match_start;
			mark_mode = 0;
			return 1;
		}
		pos++;
	}

	/* Wrap around to beginning */
	pos = 0;
	while(pos < p1){
		matched = 1;
		for(Posn i = 0; i < len; i++){
			target = filereadc(curfile, p1 + i);
			ch = filereadc(curfile, pos + i);
			if(ch != target){
				matched = 0;
				break;
			}
		}
		if(matched){
			match_start = pos;
			curfile->dot.r.p1 = match_start;
			curfile->dot.r.p2 = match_start + len;
			buf_cursor = match_start;
			mark_mode = 0;
			return 1;
		}
		pos++;
	}

	return 0;
}

/*
 * Reverse look: search for previous occurrence of selected text.
 * Returns 1 if found, 0 if not.
 */
static int
look_backward(void)
{
	Posn p1, p2, len, pos, match_start;
	Rune ch, target;
	int matched;

	if(!curfile)
		return 0;

	p1 = curfile->dot.r.p1;
	p2 = curfile->dot.r.p2;
	len = p2 - p1;

	if(len <= 0)
		return 0;

	/* Search backward starting before current selection */
	if(p1 == 0)
		pos = curfile->b.nc - len;  /* Wrap to end */
	else
		pos = p1 - 1;

	/* Search backward from pos to beginning */
	while(pos >= 0){
		matched = 1;
		for(Posn i = 0; i < len; i++){
			target = filereadc(curfile, p1 + i);
			ch = filereadc(curfile, pos + i);
			if(ch != target){
				matched = 0;
				break;
			}
		}
		if(matched){
			match_start = pos;
			curfile->dot.r.p1 = match_start;
			curfile->dot.r.p2 = match_start + len;
			buf_cursor = match_start;
			mark_mode = 0;
			return 1;
		}
		if(pos == 0)
			break;
		pos--;
	}

	/* Wrap around to end */
	pos = curfile->b.nc - len;
	while(pos > p1){
		matched = 1;
		for(Posn i = 0; i < len; i++){
			target = filereadc(curfile, p1 + i);
			ch = filereadc(curfile, pos + i);
			if(ch != target){
				matched = 0;
				break;
			}
		}
		if(matched){
			match_start = pos;
			curfile->dot.r.p1 = match_start;
			curfile->dot.r.p2 = match_start + len;
			buf_cursor = match_start;
			mark_mode = 0;
			return 1;
		}
		pos--;
	}

	return 0;
}

static void
handle_bufkey(int key)
{
	int i;

	if(!curfile){
		if(key == KEY_ESC)
			exit_bufmode();
		return;
	}

	switch(key){
	case KEY_ESC:
		if(overlay_visible){
			overlay_visible = 0;
			needs_redraw = 1;
		}else{
			/* Preserve selection (dot) when exiting buffer mode */
			exit_bufmode();
		}
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
	case 16:  /* Ctrl-P - move up (Emacs-style) */
		buf_cursor = move_visual_up(buf_cursor);
		needs_redraw = 1;
		break;

	case KEY_DOWN:
	case 14:  /* Ctrl-N - move down (Emacs-style) */
		buf_cursor = move_visual_down(buf_cursor);
		if(buf_cursor > curfile->b.nc)
			buf_cursor = curfile->b.nc;
		needs_redraw = 1;
		break;

	case KEY_LEFT:
	case 2:  /* Ctrl-B - move left (Emacs-style) */
		if(buf_cursor > 0)
			buf_cursor--;
		needs_redraw = 1;
		break;

	case KEY_RIGHT:
	case 6:  /* Ctrl-F - move right (Emacs-style) */
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

	case KEY_ALT_LEFT:  /* Alt+Left - backward word */
		{
			Posn p = buf_cursor;
			Rune ch;
			/* Skip whitespace/non-word chars backwards */
			while(p > 0){
				ch = filereadc(curfile, p - 1);
				if(iswordchar(ch))
					break;
				p--;
			}
			/* Skip word chars backwards */
			while(p > 0){
				ch = filereadc(curfile, p - 1);
				if(!iswordchar(ch))
					break;
				p--;
			}
			buf_cursor = p;
			needs_redraw = 1;
		}
		break;

	case KEY_ALT_RIGHT:  /* Alt+Right - forward word */
		{
			Posn p = buf_cursor;
			Posn nc = curfile->b.nc;
			Rune ch;
			/* Skip word chars forward */
			while(p < nc){
				ch = filereadc(curfile, p);
				if(!iswordchar(ch))
					break;
				p++;
			}
			/* Skip whitespace/non-word chars forward */
			while(p < nc){
				ch = filereadc(curfile, p);
				if(iswordchar(ch))
					break;
				p++;
			}
			buf_cursor = p;
			needs_redraw = 1;
		}
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
	case 22:  /* Ctrl-V - page down (Emacs-style) */
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
			}else if(buf_cursor < curfile->b.nc){
				/* At end of line but not end of file - delete the newline */
				logdelete(curfile, buf_cursor, buf_cursor + 1);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
				curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
			}
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	case 7:  /* Ctrl-G - toggle command overlay */
		overlay_visible = !overlay_visible;
		if(overlay_visible){
			overlay_inputlen = 0;
			overlay_cursor = 0;
			overlay_hist_scroll = 0;
			overlay_recall_idx = -1;
			overlay_selecting = 0;
			overlay_sel_start_line = -1;
			overlay_sel_end_line = -1;
			detect_darkbg();
		}
		needs_redraw = 1;
		break;

	case 12:  /* Ctrl-L - look (find next occurrence of selection) */
		if(look_forward())
			needs_redraw = 1;
		break;

	case 18:  /* Ctrl-R - reverse look (find previous occurrence of selection) */
		if(look_backward())
			needs_redraw = 1;
		break;

	case 19:  /* Ctrl-S - save (write file in background, stay in buffer mode) */
		if(curfile->name.s[0] == 0){
			/* No filename - need to exit to command mode */
			exit_bufmode();
			queue_string("w ");
		}else{
			Address save_addr = addr;
			char *output;
			addr.r.p1 = 0;
			addr.r.p2 = curfile->b.nc;
			addr.f = curfile;
			getname(curfile, 0, FALSE);
			bufmode_capture_start();
			writef(curfile);
			output = bufmode_capture_end();
			addr = save_addr;
			if(output)
				show_output_and_wait(output);
		}
		break;

	case 17:  /* Ctrl-Q - quit (return to command mode, issue 'q' command) */
		exit_bufmode();
		queue_string("q\n");
		break;

	case 23:  /* Ctrl-W - kill region (cut selection) */
		if(curfile->dot.r.p1 != curfile->dot.r.p2){
			/* Snarf the selection first */
			snarf(curfile, curfile->dot.r.p1, curfile->dot.r.p2, &snarfbuf, 0);
			copy_to_clipboard(curfile->dot.r.p1, curfile->dot.r.p2);
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

	case 24:  /* Ctrl-X - cut selection */
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

	case KEY_ALT_W:  /* Alt+W - copy to snarf buffer and system clipboard (Emacs-style) */
		if(curfile->dot.r.p1 != curfile->dot.r.p2){
			snarf(curfile, curfile->dot.r.p1, curfile->dot.r.p2, &snarfbuf, 0);
			copy_to_clipboard(curfile->dot.r.p1, curfile->dot.r.p2);
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

	case 127:  /* DEL/Backspace - delete char before cursor */
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

	case 26:  /* Ctrl-Z - undo */
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

	case '\t':  /* Tab - insert tab character */
		{
			Posn p0;
			Rune tab = '\t';
			if(curfile->dot.r.p1 != curfile->dot.r.p2){
				/* Delete selection first, then insert */
				p0 = curfile->dot.r.p1;
				logdelete(curfile, curfile->dot.r.p1, curfile->dot.r.p2);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
			}else{
				p0 = buf_cursor;
			}
			loginsert(curfile, p0, &tab, 1);
			if(fileupdate(curfile, FALSE, FALSE))
				seq++;
			buf_cursor = p0 + 1;
			curfile->dot.r.p1 = curfile->dot.r.p2 = buf_cursor;
			mark_mode = 0;
			needs_redraw = 1;
		}
		break;

	case '\r':  /* Enter - insert newline */
		{
			Posn p0, linestart;
			Rune nl = '\n';
			Rune indent[256];
			int nindent = 0;

			if(curfile->dot.r.p1 != curfile->dot.r.p2){
				/* Delete selection first, then insert */
				p0 = curfile->dot.r.p1;
				logdelete(curfile, curfile->dot.r.p1, curfile->dot.r.p2);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
			}else{
				p0 = buf_cursor;
			}

			/* If autoindent, collect leading whitespace from current line */
			if(aflag){
				/* Find start of current line */
				linestart = p0;
				while(linestart > 0 && filereadc(curfile, linestart-1) != '\n')
					linestart--;
				/* Collect leading whitespace */
				while(nindent < 255 && linestart + nindent < p0){
					Rune ch = filereadc(curfile, linestart + nindent);
					if(ch == ' ' || ch == '\t')
						indent[nindent++] = ch;
					else
						break;
				}
			}

			loginsert(curfile, p0, &nl, 1);
			if(fileupdate(curfile, FALSE, FALSE))
				seq++;
			buf_cursor = p0 + 1;

			/* Insert autoindent whitespace */
			if(aflag && nindent > 0){
				loginsert(curfile, buf_cursor, indent, nindent);
				if(fileupdate(curfile, FALSE, FALSE))
					seq++;
				buf_cursor += nindent;
			}

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
	}

	if(buf_cursor < 0)
		buf_cursor = 0;
	if(curfile && buf_cursor > curfile->b.nc)
		buf_cursor = curfile->b.nc;
}

/*
 * Check if a character is a word character (for double-click selection).
 * Word characters are alphanumeric plus underscore.
 */
static int
iswordchar(Rune ch)
{
	return (ch >= 'a' && ch <= 'z') ||
	       (ch >= 'A' && ch <= 'Z') ||
	       (ch >= '0' && ch <= '9') ||
	       ch == '_';
}

/*
 * Find the start of a word at or before position p.
 */
static Posn
word_start(File *f, Posn p)
{
	Rune ch;
	if(p <= 0 || p > f->b.nc)
		return p;
	/* First check if we're on a word character */
	ch = filereadc(f, p);
	if(!iswordchar(ch) && p > 0){
		/* Check the character before */
		ch = filereadc(f, p - 1);
		if(!iswordchar(ch))
			return p;  /* Not on a word */
		p--;
	}
	/* Move backward to start of word */
	while(p > 0){
		ch = filereadc(f, p - 1);
		if(!iswordchar(ch))
			break;
		p--;
	}
	return p;
}

/*
 * Find the end of a word at or after position p.
 */
static Posn
word_end(File *f, Posn p)
{
	Rune ch;
	if(p < 0 || p >= f->b.nc)
		return p;
	/* Check if we're on a word character */
	ch = filereadc(f, p);
	if(!iswordchar(ch)){
		/* Not on a word character */
		return p;
	}
	/* Move forward to end of word */
	while(p < f->b.nc){
		ch = filereadc(f, p);
		if(!iswordchar(ch))
			break;
		p++;
	}
	return p;
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

	/* If overlay is visible, handle mouse events in overlay */
	if(overlay_visible){
		int overlay_height, in_overlay;
		overlay_height = 1 + overlay_hist_count + 1 + 1;
		if(overlay_height > term_rows / 2)
			overlay_height = term_rows / 2;
		if(overlay_height < 3)
			overlay_height = 3;
		in_overlay = (y >= term_rows - overlay_height);

		/* Scroll events */
		if(button == 64 || button == 65){
			int oh, hist_visible, max_scroll;
			oh = overlay_height;
			hist_visible = oh - 3;
			max_scroll = overlay_hist_count - hist_visible;
			if(max_scroll < 0)
				max_scroll = 0;
			if(button == 64){
				overlay_hist_scroll += 3;
				if(overlay_hist_scroll > max_scroll)
					overlay_hist_scroll = max_scroll;
			}else{
				overlay_hist_scroll -= 3;
				if(overlay_hist_scroll < 0)
					overlay_hist_scroll = 0;
			}
			needs_redraw = 1;
			return;
		}

		/* Drag events (motion with button held) */
		if(button >= 32 && button < 64){
			if(overlay_selecting){
				int hl, hc;
				overlay_screen_to_pos(y, x, &hl, &hc);
				if(hl >= 0){
					overlay_sel_end_line = hl;
					overlay_sel_end_col = hc;
					needs_redraw = 1;
				}
			}
			return;
		}

		/* Click in buffer area above overlay: dismiss */
		if(!in_overlay && pressed){
			overlay_visible = 0;
			overlay_recall_idx = -1;
			overlay_selecting = 0;
			overlay_sel_start_line = -1;
			overlay_sel_end_line = -1;
			needs_redraw = 1;
			/* Fall through to process click in buffer */
			goto buffer_click;
		}

		/* Click inside overlay: start selection */
		if(in_overlay && pressed && (button & 3) == 0){
			int hl, hc;
			overlay_screen_to_pos(y, x, &hl, &hc);
			if(hl >= 0){
				overlay_selecting = 1;
				overlay_sel_start_line = hl;
				overlay_sel_start_col = hc;
				overlay_sel_end_line = hl;
				overlay_sel_end_col = hc;
				needs_redraw = 1;
			}
			return;
		}

		/* Release inside overlay: finish selection, copy */
		if(!pressed){
			if(overlay_selecting){
				int hl, hc;
				overlay_screen_to_pos(y, x, &hl, &hc);
				if(hl >= 0){
					overlay_sel_end_line = hl;
					overlay_sel_end_col = hc;
				}
				overlay_selecting = 0;
				overlay_copy_selection();
				overlay_sel_start_line = -1;
				overlay_sel_end_line = -1;
				needs_redraw = 1;
			}
			return;
		}

		return;
	}

buffer_click:

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
	}else if(button == 64){
		/* Scroll up (show earlier content) - check before btn==0 since 64&3==0 */
		int i;
		for(i = 0; i < 3; i++){
			if(buf_origin > 0)
				buf_origin = file_prevline(curfile, buf_origin);
		}
		/* Move cursor up if it's now below the view */
		if(count_visual_rows(buf_origin, buf_cursor) >= term_rows){
			/* Place cursor on the last visible line */
			Posn p = buf_origin;
			for(i = 0; i < term_rows - 1; i++){
				Posn next = file_nextline(curfile, p);
				if(next >= curfile->b.nc || next == p)
					break;
				p = next;
			}
			buf_cursor = p;
		}
		needs_redraw = 1;
	}else if(button == 65){
		/* Scroll down (show later content) */
		int i;
		for(i = 0; i < 3; i++){
			buf_origin = file_nextline(curfile, buf_origin);
			if(buf_origin >= curfile->b.nc){
				buf_origin = file_linestart(curfile, curfile->b.nc);
				break;
			}
		}
		/* Move cursor down if it's now above the view */
		if(buf_cursor < buf_origin)
			buf_cursor = buf_origin;
		needs_redraw = 1;
	}else if(button == 8 && pressed){
		/* Option/Alt + left click: look for selection */
		Posn ws, we;

		/* If no selection, select word under cursor first */
		if(curfile->dot.r.p1 == curfile->dot.r.p2){
			ws = word_start(curfile, p);
			we = word_end(curfile, p);
			if(ws < we){
				curfile->dot.r.p1 = ws;
				curfile->dot.r.p2 = we;
			}
		}

		/* Now execute look if we have a selection */
		if(curfile->dot.r.p1 != curfile->dot.r.p2){
			if(look_forward()){
				/* Move cursor to start of new selection */
				buf_cursor = curfile->dot.r.p1;
				/* Sync to clipboard */
				copy_to_clipboard(curfile->dot.r.p1, curfile->dot.r.p2);
			}
		}

		mark_mode = 0;
		mouse_selecting = 0;
		needs_redraw = 1;
	}else if(btn == 0){
		/* Left button press/release */
		if(pressed){
			struct timeval now;
			long elapsed_ms;
			int is_doubleclick = 0;

			gettimeofday(&now, NULL);
			elapsed_ms = (now.tv_sec - last_click_time.tv_sec) * 1000 +
			             (now.tv_usec - last_click_time.tv_usec) / 1000;

			/* Check for double-click: same position within 400ms */
			if(elapsed_ms < 400 && p == last_click_pos){
				is_doubleclick = 1;
			}

			last_click_time = now;
			last_click_pos = p;

			mark_mode = 0;  /* Cancel any keyboard selection */

			if(is_doubleclick){
				/* Double-click: select word */
				Posn ws, we;
				ws = word_start(curfile, p);
				we = word_end(curfile, p);
				if(ws < we){
					curfile->dot.r.p1 = ws;
					curfile->dot.r.p2 = we;
					buf_cursor = ws;  /* Position cursor at start to avoid visual confusion */
					mouse_selecting = 0;
					/* Sync selection to system clipboard */
					copy_to_clipboard(ws, we);
				}else{
					/* Not on a word, just place cursor */
					mouse_selecting = 1;
					mouse_sel_start = p;
					mouse_sel_end = p;
					buf_cursor = p;
					curfile->dot.r.p1 = p;
					curfile->dot.r.p2 = p;
				}
			}else{
				/* Single click: start selection */
				mouse_selecting = 1;
				mouse_sel_start = p;
				mouse_sel_end = p;
				buf_cursor = p;
				curfile->dot.r.p1 = p;
				curfile->dot.r.p2 = p;
			}
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
			}
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
		/* Collect captured output after command execution */
		if(capturing){
			char *output = bufmode_capture_end();
			if(output){
				char *p, *nl;
				p = output;
				while(*p){
					nl = strchr(p, '\n');
					if(nl){
						*nl = '\0';
						overlay_add_history(p);
						p = nl + 1;
					}else{
						overlay_add_history(p);
						break;
					}
				}
			}
			/* Sync cursor to new dot and scroll if needed */
			if(curfile){
				buf_cursor = curfile->dot.r.p2;
				if(buf_cursor > curfile->b.nc)
					buf_cursor = curfile->b.nc;
			}
			needs_redraw = 1;
		}

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

		if(overlay_visible)
			handle_overlay_key(key);
		else
			handle_bufkey(key);

		/* If we have queued commands, return them to sam for processing */
		if(!queue_empty())
			return dequeue_char();

		/* If we exited buffer mode, read next command char */
		if(viewmode == ModeCmd)
			return terminputc();
	}
}
