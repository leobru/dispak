#define _XOPEN_SOURCE_EXTENDED 1
#include <ctype.h>
#include <string.h>
#include <unistd.h>
#include <wchar.h>
#define trace besm6_trace_extern
#include "defs.h"
#undef trace
#include <curses.h>
#include "encoding.h"
#include "gost10859.h"

#define VT_ROWS 16
#define VT_COLS 80
#define VT_SPACING 2
#define VT_E71BUFSZ (324*6)
#define CTRL_NONE 0
#define CTRL_C 1
#define CTRL_M 2
#define CTRL_J 3

typedef struct {
	uchar value;
	uchar raw;
	uchar ctrl;
} cell_t;

typedef struct {
	const char *label1;
	const char *label2;
	int x0;
	int x1;
} button_t;

static cell_t screen_cells[VT_ROWS][VT_COLS];
static int active;
static int offline_mode;
static int ctrl_blink;
static int roll_mode;
static int cursor_row;
static int cursor_col;
static int pending_input;
static uchar pending_flags;
static ushort pending_a1;
static ushort pending_a2;
static int pending_start_row;
static int pending_start_col;
static uchar online_buf[VT_E71BUFSZ];
static int online_len;
static int screen_top = 1;
static int screen_left = 2;
static int button_row;

static void pace_output(void)
{
	if (vt340_curses_baud)
		usleep(10000000 / vt340_curses_baud);
}

static button_t buttons[] = {
	{ "OFF", "LINE", 0, 0 },
	{ "ON", "LINE", 0, 0 },
	{ "SEND", 0, 0, 0 },
	{ "ROLL", 0, 0, 0 },
	{ "CTRL", "BLINK", 0, 0 },
	{ "ERASE", 0, 0, 0 },
	{ "DL", 0, 0, 0 },
	{ "IL", 0, 0, 0 },
	{ "DC", 0, 0, 0 },
	{ "IC", 0, 0, 0 },
};

static int host_row_for(int row)
{
	return screen_top + 1 + row * VT_SPACING;
}

static int min_host_rows(void)
{
	return screen_top + 1 + VT_ROWS * VT_SPACING + 5;
}

static int min_host_cols(void)
{
	return screen_left + VT_COLS + 3;
}

static void clear_line(int row)
{
	int col;

	if (row < 0 || row >= VT_ROWS)
		return;
	for (col = 0; col < VT_COLS; ++col) {
		screen_cells[row][col].value = GOST_SPACE;
		screen_cells[row][col].raw = 0;
		screen_cells[row][col].ctrl = CTRL_NONE;
	}
}

static void scroll_up(void)
{
	int row;

	for (row = 0; row < VT_ROWS - 1; ++row)
		memcpy(screen_cells[row], screen_cells[row + 1], sizeof(screen_cells[row]));
	clear_line(VT_ROWS - 1);
	cursor_row = VT_ROWS - 1;
	if (cursor_col >= VT_COLS)
		cursor_col = VT_COLS - 1;
}

static void line_feed(void)
{
	cursor_col = 0;
	if (cursor_row == VT_ROWS - 1) {
		if (roll_mode)
			cursor_row = 0;
		else
			scroll_up();
	} else
		++cursor_row;
}

static void move_left(void)
{
	if (cursor_col > 0)
		--cursor_col;
}

static void move_right(void)
{
	if (cursor_col < VT_COLS - 1)
		++cursor_col;
}

static void move_up(void)
{
	if (cursor_row > 0)
		--cursor_row;
}

static void move_down(void)
{
	if (cursor_row < VT_ROWS - 1)
		++cursor_row;
}

static wchar_t cell_wchar(const cell_t *cell)
{
	unsigned short ch;

	if (cell->ctrl != CTRL_NONE) {
		switch (cell->ctrl) {
		case CTRL_C: return ctrl_blink ? L'C' : L' ';
		case CTRL_M: return ctrl_blink ? L'M' : L' ';
		case CTRL_J: return ctrl_blink ? L'J' : L' ';
		default: return ctrl_blink ? L'^' : L' ';
		}
	}
	if (cell->raw) {
		unsigned char raw = cell->value & 0x7f;
		unsigned short u = koi7_to_unicode[raw];

		if (raw < 32 || raw == 127)
			return L'.';
		if (u)
			return (wchar_t) u;
		return (wchar_t) raw;
	}
	if (cell->value == 0136)
		ch = '?';
	else {
		ch = gost_to_unicode(cell->value);
		if (!ch)
			ch = ' ';
	}
	return (wchar_t) ch;
}

static void draw_buttons(void)
{
	int i;
	int x = 2;

	button_row = screen_top + 1 + VT_ROWS * VT_SPACING + 2;
	for (i = 0; i < (int) (sizeof(buttons) / sizeof(buttons[0])); ++i) {
		int attr = A_NORMAL;
		int width = (int) strlen(buttons[i].label1);
		char fkey[4];

		if (buttons[i].label2 && (int) strlen(buttons[i].label2) > width)
			width = (int) strlen(buttons[i].label2);
		buttons[i].x0 = x;
		buttons[i].x1 = x + width + 1;
		if ((i == 0 && offline_mode) || (i == 1 && !offline_mode) ||
		    (i == 3 && roll_mode) || (i == 4 && ctrl_blink))
			attr = A_REVERSE;
		mvprintw(button_row, x, "[%-*s]", width, buttons[i].label1);
		mvchgat(button_row, x, buttons[i].x1 - buttons[i].x0 + 1, attr, 0, NULL);
		if (buttons[i].label2) {
			mvprintw(button_row + 1, x, "[%-*s]", width, buttons[i].label2);
			mvchgat(button_row + 1, x, buttons[i].x1 - buttons[i].x0 + 1, attr, 0, NULL);
		}
		snprintf(fkey, sizeof(fkey), "F%d", i + 1);
		mvprintw(button_row + 2, x + (width + 2 - (int) strlen(fkey)) / 2, "%s", fkey);
		x = buttons[i].x1 + 3;
	}
}

static void draw_screen(void)
{
	int row, col;
	int top = screen_top;
	int left = screen_left;
	int bottom = top + VT_ROWS * VT_SPACING;
	int right = left + VT_COLS + 1;

	button_row = screen_top + 1 + VT_ROWS * VT_SPACING + 2;
	erase();
	mvaddch(top, left, ACS_ULCORNER);
	mvaddch(top, right, ACS_URCORNER);
	mvaddch(bottom, left, ACS_LLCORNER);
	mvaddch(bottom, right, ACS_LRCORNER);
	mvprintw(0, 2, " Videoton-340 ");
	for (col = left + 1; col < right; ++col) {
		mvaddch(top, col, ACS_HLINE);
		mvaddch(bottom, col, ACS_HLINE);
	}
	for (row = top + 1; row < bottom; ++row) {
		mvaddch(row, left, ACS_VLINE);
		mvaddch(row, right, ACS_VLINE);
	}
	for (row = 0; row < VT_ROWS; ++row) {
		int y = host_row_for(row);
		wchar_t linebuf[VT_COLS + 1];

		mvhline(y, left + 1, ' ', VT_COLS);
		for (col = 0; col < VT_COLS; ++col)
			linebuf[col] = cell_wchar(&screen_cells[row][col]);
		linebuf[VT_COLS] = 0;
		mvaddnwstr(y, left + 1, linebuf, VT_COLS);
		if (ctrl_blink) {
			for (col = 0; col < VT_COLS; ++col)
				if (screen_cells[row][col].ctrl != CTRL_NONE)
					mvchgat(y, left + 1 + col, 1, A_DIM | A_BLINK, 0, NULL);
		}
		if (y + 1 < bottom)
			mvhline(y + 1, left + 1, ' ', VT_COLS);
	}
	draw_buttons();
	move(host_row_for(cursor_row), left + 1 + cursor_col);
	refresh();
}

static void put_cell(uchar value, int raw, int ctrl)
{
	cell_t *cell = &screen_cells[cursor_row][cursor_col];

	cell->value = value;
	cell->raw = raw;
	cell->ctrl = ctrl;
	if (cursor_col == VT_COLS - 1)
		line_feed();
	else
		++cursor_col;
}

static void insert_char_at_cursor(uchar value, int raw, int ctrl)
{
	int col;

	for (col = VT_COLS - 1; col > cursor_col; --col)
		screen_cells[cursor_row][col] = screen_cells[cursor_row][col - 1];
	screen_cells[cursor_row][cursor_col].value = value;
	screen_cells[cursor_row][cursor_col].raw = raw;
	screen_cells[cursor_row][cursor_col].ctrl = ctrl;
	if (cursor_col < VT_COLS - 1)
		++cursor_col;
}

static void delete_char_at_cursor(void)
{
	int col;

	for (col = cursor_col; col < VT_COLS - 1; ++col)
		screen_cells[cursor_row][col] = screen_cells[cursor_row][col + 1];
	screen_cells[cursor_row][VT_COLS - 1].value = GOST_SPACE;
	screen_cells[cursor_row][VT_COLS - 1].raw = 0;
	screen_cells[cursor_row][VT_COLS - 1].ctrl = CTRL_NONE;
}

static void insert_line_at_cursor(void)
{
	int row;

	for (row = VT_ROWS - 1; row > cursor_row; --row)
		memcpy(screen_cells[row], screen_cells[row - 1], sizeof(screen_cells[row]));
	clear_line(cursor_row);
}

static void delete_line_at_cursor(void)
{
	int row;

	for (row = cursor_row; row < VT_ROWS - 1; ++row)
		memcpy(screen_cells[row], screen_cells[row + 1], sizeof(screen_cells[row]));
	clear_line(VT_ROWS - 1);
}

static void home_screen(void)
{
	cursor_row = 0;
	cursor_col = 0;
}

static void erase_screen(void)
{
	int row;

	for (row = 0; row < VT_ROWS; ++row)
		clear_line(row);
	home_screen();
}

static int encode_key(wint_t key, int is_key_code, uchar *value, int *raw, int *ctrl)
{
	*ctrl = CTRL_NONE;
	*raw = pending_flags & 1;
	if (!is_key_code && key == 3) {
		*value = *raw ? 3 : GOST_EOF;
		*ctrl = CTRL_C;
		return 1;
	}
	if (!is_key_code && key == 13) {
		*value = *raw ? 3 : GOST_EOF;
		*ctrl = CTRL_C;
		return 1;
	}
	if ((is_key_code && key == KEY_ENTER) || (!is_key_code && key == '\n')) {
		*value = *raw ? 3 : GOST_EOF;
		*ctrl = CTRL_C;
		return 1;
	}
	if (!is_key_code && key == 10) {
		*value = *raw ? 10 : GOST_NEWLINE;
		*ctrl = CTRL_J;
		return 1;
	}
	if (is_key_code)
		return 0;
	if (*raw) {
		if (key >= 0 && key < 128) {
			*value = key & 0x7f;
			return 1;
		}
		return 0;
	}
	if (key >= 0 && key <= 0xffff) {
		*value = unicode_to_gost((unsigned short) key);
		return *value != 0 || key == '0';
	}
	return 0;
}

static int extract_and_store_from(int start_row, int start_col)
{
	uchar *dp = core[pending_a1].w_b;
	int limit = (pending_a2 - pending_a1 + 1) * 6;
	int row = start_row;
	int col = start_col;

	while (row < VT_ROWS && dp - core[pending_a1].w_b < limit) {
		cell_t *cell = &screen_cells[row][col];
		if (cell->ctrl == CTRL_C)
			break;
		*dp++ = cell->value;
		if (++col >= VT_COLS) {
			col = 0;
			++row;
		}
	}
	if (dp - core[pending_a1].w_b < limit) {
		if (pending_flags & 1)
			*dp++ = 0;
		else if (dp == core[pending_a1].w_b || dp[-1] != GOST_EOF)
			*dp++ = GOST_EOF;
	}
	while ((dp - core[pending_a1].w_b) % 6)
		*dp++ = 0;
	eraise(4);
	pending_input = 0;
	return E_SUCCESS;
}

static int extract_and_store(void)
{
	return extract_and_store_from(cursor_row, 0);
}

static int store_online_input(void)
{
	uchar *dp = core[pending_a1].w_b;
	int limit = (pending_a2 - pending_a1 + 1) * 6;
	int i;

	for (i = 0; i < online_len && i < limit; ++i)
		*dp++ = online_buf[i];
	while ((dp - core[pending_a1].w_b) % 6)
		*dp++ = 0;
	eraise(4);
	pending_input = 0;
	return E_SUCCESS;
}

static int retract_online_input(void)
{
	if (!pending_input || offline_mode || (pending_flags & 1) || online_len <= 0)
		return 0;
	--online_len;
	move_left();
	return 1;
}

static void handle_button(int idx)
{
	switch (idx) {
	case 0:
		offline_mode = 1;
		break;
	case 1:
		offline_mode = 0;
		break;
	case 2:
		if (pending_input)
			(void) extract_and_store();
		offline_mode = 0;
		break;
	case 3:
		roll_mode = !roll_mode;
		break;
	case 4:
		ctrl_blink = !ctrl_blink;
		break;
	case 5:
		erase_screen();
		online_len = 0;
		break;
	case 6:
		delete_line_at_cursor();
		break;
	case 7:
		insert_line_at_cursor();
		break;
	case 8:
		delete_char_at_cursor();
		break;
	case 9:
		insert_char_at_cursor(GOST_SPACE, 0, CTRL_NONE);
		break;
	}
}

static void handle_key_code(int ch)
{
	switch (ch) {
	case KEY_F(1):
		handle_button(0);
		return;
	case KEY_F(2):
		handle_button(1);
		return;
	case KEY_F(3):
		handle_button(2);
		return;
	case KEY_F(4):
		handle_button(3);
		return;
	case KEY_F(5):
		handle_button(4);
		return;
	case KEY_F(6):
		handle_button(5);
		return;
	case KEY_F(7):
		handle_button(6);
		return;
	case KEY_F(8):
		handle_button(7);
		return;
	case KEY_F(9):
		handle_button(8);
		return;
	case KEY_F(10):
		handle_button(9);
		return;
	case KEY_LEFT:
		if (retract_online_input())
			return;
		move_left();
		return;
	case KEY_RIGHT:
		move_right();
		return;
	case KEY_UP:
		move_up();
		return;
	case KEY_DOWN:
		move_down();
		return;
	case KEY_HOME:
		cursor_col = 0;
		return;
	case KEY_END:
		cursor_col = VT_COLS - 1;
		return;
	case KEY_BACKSPACE:
	case 127:
		if (retract_online_input())
			return;
		move_left();
		delete_char_at_cursor();
		return;
	case KEY_DC:
		delete_char_at_cursor();
		return;
	case KEY_IC:
		insert_char_at_cursor(GOST_SPACE, 0, CTRL_NONE);
		return;
	}
}

static void handle_input_char(wint_t key, int is_key_code)
{
	uchar value;
	int raw;
	int ctrl;
	int enter_key;

	if (!pending_input)
		return;
	if (!encode_key(key, is_key_code, &value, &raw, &ctrl))
		return;
	enter_key = (is_key_code && key == KEY_ENTER) ||
		(!is_key_code && (key == '\n' || key == '\r'));
	if (ctrl == CTRL_J) {
		put_cell(value, raw, ctrl);
		if (!offline_mode && online_len < VT_E71BUFSZ)
			online_buf[online_len++] = value;
		line_feed();
		return;
	}
	if (offline_mode)
		put_cell(value, raw, ctrl);
	else {
		put_cell(value, raw, ctrl);
		if (online_len < VT_E71BUFSZ)
			online_buf[online_len++] = value;
	}
	if (!offline_mode && ctrl == CTRL_C) {
		(void) store_online_input();
		if (enter_key) {
			put_cell(raw ? '\n' : GOST_NEWLINE, raw, CTRL_J);
			line_feed();
		}
	}
}

static void handle_key(int ch)
{
	handle_key_code(ch);
	handle_input_char((wint_t) ch, 1);
}

int
vt340_curses_init(void)
{
	int row;

	if (active || !vt340_curses || notty || !isatty(0) || !isatty(1))
		return 0;
	if (!initscr()) {
		fprintf(stderr, "Unable to initialize curses terminal.\n");
		return 0;
	}
	if (LINES < min_host_rows() || COLS < min_host_cols()) {
		endwin();
		fprintf(stderr, "Terminal too small for VT340 curses mode; falling back.\n");
		return 0;
	}
	cbreak();
	noecho();
	keypad(stdscr, TRUE);
	meta(stdscr, TRUE);
	nonl();
	curs_set(1);
	for (row = 0; row < VT_ROWS; ++row)
		clear_line(row);
	home_screen();
	offline_mode = 0;
	ctrl_blink = 0;
	roll_mode = 0;
	pending_input = 0;
	active = 1;
	draw_screen();
	return 1;
}

void
vt340_curses_shutdown(void)
{
	if (!active)
		return;
	active = 0;
	endwin();
}

int
vt340_curses_is_active(void)
{
	return active;
}

static int process_raw_byte(uchar ch)
{
	switch (ch & 0x7f) {
	case 0:
		return 0;
	case '\037':
		erase_screen();
		return 1;
	case '\014':
		home_screen();
		return 1;
	case '\031':
		move_up();
		return 1;
	case '\032':
		move_down();
		return 1;
	case '\030':
		move_right();
		return 1;
	case '\010':
		move_left();
		return 1;
	case 3:
		put_cell(3, 1, CTRL_C);
		return 1;
	case 13:
		put_cell(13, 1, CTRL_M);
		return 1;
	case 10:
		put_cell(10, 1, CTRL_J);
		line_feed();
		return 1;
	default:
		put_cell(ch & 0x7f, 1, CTRL_NONE);
		return 1;
	}
}

static int process_gost_byte(uchar ch, uchar flags)
{
	switch (ch) {
	case GOST_END_OF_INFORMATION:
	case GOST_EOF:
		if (!(flags & 010)) {
			put_cell(GOST_NEWLINE, 0, CTRL_J);
			line_feed();
		}
		return 0;
	case GOST_CARRIAGE_RETURN:
		put_cell(ch, 0, CTRL_M);
		return 1;
	case GOST_NEWLINE:
		put_cell(ch, 0, CTRL_J);
		line_feed();
		return 1;
	case 0136:
		put_cell(ch, 0, CTRL_NONE);
		return 1;
	case 0141:
	case 0142:
	case 0143:
		return 1;
	case 0146:
	case 0170:
		move_left();
		return 1;
	case 0171:
		move_right();
		return 1;
	case 0176:
		move_up();
		return 1;
	case 0177:
		move_down();
		return 1;
	case 0162:
		erase_screen();
		return 1;
	case 0167:
		home_screen();
		return 1;
	case 021:
		if (flags == 0220) {
			put_cell(GOST_NEWLINE, 0, CTRL_J);
			line_feed();
			return 0;
		}
	default:
		put_cell(ch, 0, CTRL_NONE);
		return 1;
	}
}

int
vt340_curses_ttout(uchar flags, ushort a1, ushort a2)
{
	uchar *sp, *start;

	if (!active)
		return E_UNIMP;
	start = sp = core[a1].w_b;
	if (flags == 0220 && sp - start < (a2 - a1 + 1) * 6)
		++sp;
	while ((sp - start < VT_E71BUFSZ) && (sp - start < (a2 - a1 + 1) * 6)) {
		if (flags & 1) {
			if (!process_raw_byte(*sp++))
				break;
		} else {
			if (!process_gost_byte(*sp++, flags))
				break;
		}
		if (vt340_curses_baud) {
			draw_screen();
			pace_output();
		}
	}
	if (!vt340_curses_baud)
		draw_screen();
	if (!(flags & 010))
		(void) eraise(010);
	return E_SUCCESS;
}

int
vt340_curses_ttin(uchar flags, ushort a1, ushort a2)
{
	wint_t ch;
	int rc;

	if (!active)
		return E_UNIMP;
	pending_input = 1;
	pending_flags = flags;
	pending_a1 = a1;
	pending_a2 = a2;
	pending_start_row = cursor_row;
	pending_start_col = cursor_col;
	online_len = 0;
	if (flags & 4)
		(void) vt340_curses_ttout(flags, a1, a2);
	draw_screen();
	while (pending_input) {
		rc = get_wch(&ch);
		if (rc == KEY_CODE_YES)
			handle_key((int) ch);
		else if (rc == OK)
			handle_input_char(ch, 0);
		draw_screen();
	}
	return E_SUCCESS;
}
