/* shim_callback.c -- bridge between libnh's shim windowport and ESP-IDF.
 *
 * Phase 2: logged every windowport call to the serial monitor.
 * Phase 3b: caches the NetHack map and renders it to the ST7789 LCD via
 *           tdeck_display.h.  8x8 monospace font, 40-column viewport
 *           tracking the player position.
 *
 * Window IDs used by libnh's shim windowport:
 *     0 = NHW_MAP (the dungeon map)
 *     1 = NHW_MESSAGE
 *     2 = NHW_STATUS
 *     3 = NHW_MENU
 * (See include/wintype.h.)
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nethack.h"
#include "tdeck_display.h"
#include "tdeck_keyboard.h"

static const char *TAG = "nh-shim";

/* NetHack map is 80 wide, 21 tall in the classic ASCII windowport.  We
 * cache the printable char + a "drawn" flag for each cell. */
#define NH_MAP_W 80
#define NH_MAP_H 21
static char     map_ch[NH_MAP_H][NH_MAP_W];
static int      cursor_x = 0;
static int      cursor_y = 0;
static int      view_x   = 0;     /* leftmost column of the LCD viewport */
static bool     map_dirty = true;

/* NetHack window IDs.  We assign sequential ids in shim_create_nhwindow
 * and remember which one was the NHW_MAP (=3) window so that
 * clear/print_glyph/curs/display calls for OTHER windows (message,
 * status, menu) don't trample our map cache.
 *
 * The previous version returned winid=0 from every create call -- which
 * caused NetHack's clear_nhwindow(WIN_MESSAGE) to wipe the map cache
 * (same winid=0) on every moveloop iteration. */
#define NHW_MESSAGE 1
#define NHW_STATUS  2
#define NHW_MAP     3
#define NHW_MENU    4
static int next_winid = 1;
static int win_map    = -1;

/* Viewport: 40 cols x 21 rows of 8x8 cells = 320 x 168 px on the LCD.
 * Bottom 72 px reserved for status / messages (still TBD). */
#define VIEW_COLS    40
#define VIEW_ROWS    NH_MAP_H
#define CELL_W       8
#define CELL_H       8
#define MAP_PX_Y0    0

/* --- Menu state -------------------------------------------------------
 *
 * NetHack opens menus through start_menu / add_menu* / end_menu /
 * select_menu.  We accumulate the items here during add_menu and render
 * the menu on the LCD when select_menu is called.  Any keypress closes
 * the menu; nothing is selected (PICK_ONE/PICK_ANY appear as cancel).
 * Letter-driven item selection is a follow-up. */
#define MENU_MAX_ITEMS    80
#define MENU_MAX_TEXT     52    /* 320/8 = 40 cols of text + a bit slack */
#define MENU_VISIBLE_ROWS 27    /* 240/8 = 30 rows, minus title + spacer + hint */
static struct {
    bool    in_progress;        /* between start_menu and end_menu */
    int     count;
    int     how;                /* PICK_NONE=0, PICK_ONE=1, PICK_ANY=2 */
    int     scroll_top;
    char    title[MENU_MAX_TEXT];
    char    items[MENU_MAX_ITEMS][MENU_MAX_TEXT];
} g_menu;

static void
recenter_view_on_cursor(void)
{
    /* Try to keep the cursor in the middle of the viewport.  Clamp to
     * the dungeon bounds. */
    int target = cursor_x - VIEW_COLS / 2;
    if (target < 0) target = 0;
    if (target > NH_MAP_W - VIEW_COLS) target = NH_MAP_W - VIEW_COLS;
    if (target != view_x) {
        view_x = target;
        map_dirty = true;
    }
}

static void
render_map(void)
{
    for (int row = 0; row < VIEW_ROWS; row++) {
        for (int col = 0; col < VIEW_COLS; col++) {
            int mx = view_x + col;
            char c = map_ch[row][mx];
            uint16_t fg = TDECK_COLOR_WHITE;
            uint16_t bg = TDECK_COLOR_BLACK;
            /* Highlight cursor / player position in green. */
            if (row == cursor_y && mx == cursor_x) {
                fg = TDECK_COLOR_BLACK;
                bg = TDECK_COLOR_GREEN;
                if (c == 0 || c == ' ') c = '@';
            } else if (c == 0) {
                c = ' ';
            }
            tdeck_display_putchar(col * CELL_W,
                                  MAP_PX_Y0 + row * CELL_H,
                                  c, fg, bg);
        }
    }
}

/* Status / message area below the map: rows VIEW_ROWS..29 of 8-pixel
 * height (Y = 168..240).  9 lines × 40 chars max. */
#define STATUS_FIRST_LINE   VIEW_ROWS         /* row index of first line below map */
#define STATUS_LINE_COUNT   ((240 - VIEW_ROWS * CELL_H) / CELL_H)
#define STATUS_LINE_COLS    (320 / CELL_W)

static void
status_pixel_y(int line_idx, int *y_out)
{
    *y_out = MAP_PX_Y0 + VIEW_ROWS * CELL_H + line_idx * CELL_H;
}

static void
clear_status_line(int line_idx, uint16_t bg)
{
    int y;
    status_pixel_y(line_idx, &y);
    if (y + CELL_H > 240) return;
    static uint16_t blank[8 * 8];
    for (int i = 0; i < 8 * 8; i++) blank[i] = bg;
    for (int x = 0; x < 320; x += 8) {
        tdeck_display_blit(x, y, 8, 8, blank);
    }
}

/* Word-wrap a message into the status area starting at line `line_idx`,
 * using up to `max_lines` of 8-pixel rows.  Wraps on the last whitespace
 * that fits within STATUS_LINE_COLS; if a single word is too long it is
 * chopped at the column boundary. */
static void
draw_status_message(int line_idx, const char *msg, int max_lines,
                    uint16_t fg, uint16_t bg)
{
    /* Clear the whole region first so leftovers from the previous
     * message don't bleed through. */
    for (int i = 0; i < max_lines; i++) clear_status_line(line_idx + i, bg);

    if (!msg || !*msg) return;

    int line = 0;
    while (*msg && line < max_lines) {
        /* Skip leading spaces (but preserve them at start of explicit
         * empty lines like NetHack paragraph breaks). */
        while (*msg == ' ') msg++;
        if (!*msg) break;

        /* How many chars fit on this line? */
        const char *line_start = msg;
        int run = 0;
        int last_space = -1;
        while (msg[run] && run < STATUS_LINE_COLS) {
            if (msg[run] == ' ') last_space = run;
            run++;
        }
        int n;
        if (!msg[run]) {
            n = run;                       /* remaining fits */
        } else if (last_space > 0) {
            n = last_space;                /* wrap at last space */
        } else {
            n = STATUS_LINE_COLS;          /* one long word, chop */
        }

        /* Render this slice via tdeck_display_print -- which is the
         * exact code path the boot diagnostic used for "ABCDEF..." at
         * x=0 (and showed the 'A' correctly).  Copy into a NUL-terminated
         * buffer first since the source isn't terminated at `n`. */
        int y;
        status_pixel_y(line_idx + line, &y);
        if (y + CELL_H > 240) break;
        char line_buf[STATUS_LINE_COLS + 1];
        int copy_n = (n <= STATUS_LINE_COLS) ? n : STATUS_LINE_COLS;
        memcpy(line_buf, line_start, copy_n);
        line_buf[copy_n] = '\0';
        tdeck_display_print(0, y, line_buf, fg, bg);
        msg += n;
        line++;
    }
}

/* Legacy thin wrapper kept for one-shot lines that fit. */
static void
draw_status_line(int line_idx, const char *msg, uint16_t fg, uint16_t bg)
{
    draw_status_message(line_idx, msg, STATUS_LINE_COUNT - line_idx, fg, bg);
}

/* --- string-arg pretty printer (unchanged from Phase 2) ----------- */

static void
log_string_arg(char *buf, size_t buf_size, const char *s)
{
    if (!s) {
        snprintf(buf, buf_size, "(null)");
        return;
    }
    size_t out = 0;
    out += snprintf(buf + out, buf_size - out, "\"");
    for (size_t i = 0; s[i] && out < buf_size - 4; i++) {
        char c = s[i];
        if (c == '\n')
            out += snprintf(buf + out, buf_size - out, "\\n");
        else if (c == '\t')
            out += snprintf(buf + out, buf_size - out, "\\t");
        else if ((unsigned char) c < 0x20 || (unsigned char) c >= 0x7f)
            out += snprintf(buf + out, buf_size - out, "\\x%02x",
                            (unsigned char) c);
        else
            out += snprintf(buf + out, buf_size - out, "%c", c);
    }
    snprintf(buf + out, buf_size - out, "\"");
}

static void
default_return(void *ret_ptr, char retcode)
{
    if (!ret_ptr)
        return;
    switch (retcode) {
    case 'v':
        break;
    case 'i':
    case 'b':
    case '1':
    case '2':
        *(int *) ret_ptr = 0;
        break;
    case 'c':
        *(char *) ret_ptr = '\0';
        break;
    case 's':
    case 'p':
        *(void **) ret_ptr = NULL;
        break;
    default:
        *(int *) ret_ptr = 0;
        break;
    }
}

/* --- shim_print_glyph / shim_curs / display / clear handlers ------ */

/* shim_create_nhwindow(type) -- assign a unique winid and remember
 * which one is the map.  Returns the new winid via ret_ptr. */
static void
handle_create_nhwindow(va_list *ap_in, void *ret_ptr)
{
    va_list ap = *ap_in;
    int type = va_arg(ap, int);
    int winid = next_winid++;
    if (type == NHW_MAP) {
        win_map = winid;
    }
    if (ret_ptr) *(int *) ret_ptr = winid;
}

/* The format string for shim_print_glyph is "vi11pp":
 *   v   ret void
 *   i   window
 *   1   coordxy x
 *   1   coordxy y
 *   p   glyph_info *
 *   p   bk_glyph_info *
 * We parse out the four args ourselves so we can read x/y/gi. */
static void
handle_print_glyph(va_list *ap_in)
{
    va_list ap = *ap_in;
    int   window = va_arg(ap, int);
    int   x = va_arg(ap, int);          /* coordxy promoted to int in va_arg */
    int   y = va_arg(ap, int);
    const void *gi = va_arg(ap, const void *);
    (void) va_arg(ap, const void *);    /* bk_glyph_info, ignored for now */

    if (window != win_map) return;
    if (x < 0 || x >= NH_MAP_W) return;
    if (y < 0 || y >= NH_MAP_H) return;

    int ch = nh_glyph_info_char(gi);
    if (ch < 0x20 || ch > 0x7e) ch = '?';
    map_ch[y][x] = (char) ch;
    map_dirty = true;
}

static void
handle_curs(va_list *ap_in)
{
    va_list ap = *ap_in;
    int window = va_arg(ap, int);
    int x = va_arg(ap, int);
    int y = va_arg(ap, int);
    if (window != win_map) return;
    cursor_x = x;
    cursor_y = y;
    recenter_view_on_cursor();
    map_dirty = true;
}

static void
handle_clear(va_list *ap_in)
{
    va_list ap = *ap_in;
    int window = va_arg(ap, int);
    if (window != win_map) return;
    memset(map_ch, 0, sizeof(map_ch));
    map_dirty = true;
}

static void
handle_display(va_list *ap_in)
{
    va_list ap = *ap_in;
    int window = va_arg(ap, int);
    (void) va_arg(ap, int); /* blocking */
    if (window != win_map) return;
    if (!map_dirty) return;
    render_map();
    map_dirty = false;
}

/* --- menu rendering -------------------------------------------------- */

static void
copy_truncated(char *dst, size_t dst_size, const char *src)
{
    if (!src) { dst[0] = '\0'; return; }
    size_t n = strlen(src);
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* shim_start_menu(window, how) fmt "vii" */
static void
handle_start_menu(va_list *ap_in)
{
    va_list ap = *ap_in;
    (void) va_arg(ap, int);            /* window */
    int how = va_arg(ap, int);
    g_menu.in_progress = true;
    g_menu.count = 0;
    g_menu.how = how;
    g_menu.scroll_top = 0;
    g_menu.title[0] = '\0';
}

/* shim_add_menu(window, glyphinfo*, identifier, ch, gch, attr, clr, str,
 *               itemflags) fmt "vipi00iisi" */
static void
handle_add_menu(va_list *ap_in)
{
    va_list ap = *ap_in;
    (void) va_arg(ap, int);            /* window */
    (void) va_arg(ap, void *);         /* glyphinfo */
    (void) va_arg(ap, int);            /* identifier */
    (void) va_arg(ap, void *);         /* ch (opaque) */
    (void) va_arg(ap, void *);         /* gch (opaque) */
    (void) va_arg(ap, int);            /* attr */
    (void) va_arg(ap, int);            /* clr */
    const char *str = va_arg(ap, const char *);
    (void) va_arg(ap, int);            /* itemflags */

    if (!g_menu.in_progress) return;
    if (g_menu.count >= MENU_MAX_ITEMS) return;
    copy_truncated(g_menu.items[g_menu.count],
                   sizeof(g_menu.items[g_menu.count]),
                   str);
    g_menu.count++;
}

/* shim_end_menu(window, prompt) fmt "vis" */
static void
handle_end_menu(va_list *ap_in)
{
    va_list ap = *ap_in;
    (void) va_arg(ap, int);            /* window */
    const char *prompt = va_arg(ap, const char *);
    copy_truncated(g_menu.title, sizeof(g_menu.title), prompt);
    g_menu.in_progress = false;
}

static void
render_menu(void)
{
    tdeck_display_fill(TDECK_COLOR_BLACK);

    /* Title row in green. */
    const char *title = g_menu.title[0] ? g_menu.title : "(menu)";
    tdeck_display_print(0, 0, title, TDECK_COLOR_GREEN, TDECK_COLOR_BLACK);

    /* Visible items, starting at y=16 (title row + spacer). */
    int n = g_menu.count;
    int top = g_menu.scroll_top;
    if (top < 0) top = 0;
    if (top > n) top = n;
    int visible = n - top;
    if (visible > MENU_VISIBLE_ROWS) visible = MENU_VISIBLE_ROWS;

    for (int i = 0; i < visible; i++) {
        int y = 16 + i * 8;
        tdeck_display_print(0, y, g_menu.items[top + i],
                            TDECK_COLOR_WHITE, TDECK_COLOR_BLACK);
    }

    /* Footer hint: how to dismiss / scroll. */
    char hint[64];
    if (n > MENU_VISIBLE_ROWS) {
        int last = top + visible;
        snprintf(hint, sizeof(hint),
                 "[%d-%d/%d j/k=scroll any=close]",
                 top + 1, last, n);
    } else {
        snprintf(hint, sizeof(hint), "[press any key]");
    }
    tdeck_display_print(0, 240 - 8, hint,
                        TDECK_COLOR_GREEN, TDECK_COLOR_BLACK);
}

/* shim_get_ext_cmd() fmt "iv" -- NetHack invokes this when the user
 * presses '#'.  We pop up a tiny "#: " input field at the bottom of the
 * screen, accept a-z, and look up the typed name in extcmdlist[] via
 * nh_lookup_ext_cmd.  Backspace edits, Enter submits, ESC cancels. */
static int
prompt_ext_cmd(void)
{
    char input[40];
    int len = 0;
    input[0] = '\0';

    /* Drain any stale keystrokes (e.g. the '#' that opened this prompt). */
    int dummy;
    while (tdeck_keyboard_peek(&dummy)) { }

    /* Render at the very bottom of the screen.  Use the last status line
     * so we don't trample the map. */
    int prompt_y = 240 - CELL_H;

    for (;;) {
        /* Clear the row and redraw "#: <input>_". */
        static uint16_t blank[8 * 8];
        for (int i = 0; i < 64; i++) blank[i] = TDECK_COLOR_BLACK;
        for (int x = 0; x < 320; x += 8)
            tdeck_display_blit(x, prompt_y, 8, 8, blank);

        char render[64];
        snprintf(render, sizeof(render), "#: %s_", input);
        tdeck_display_print(0, prompt_y, render,
                            TDECK_COLOR_GREEN, TDECK_COLOR_BLACK);

        int key = tdeck_keyboard_getchar(portMAX_DELAY);
        if (key < 0) continue;

        if (key == 0x1B) {            /* ESC -- cancel */
            return -1;
        } else if (key == '\r' || key == '\n') {
            break;
        } else if (key == 0x08 || key == 0x7F) {    /* backspace / DEL */
            if (len > 0) input[--len] = '\0';
        } else if (key >= 'a' && key <= 'z' && len < (int) sizeof(input) - 1) {
            input[len++] = (char) key;
            input[len] = '\0';
        } else if (key >= 'A' && key <= 'Z' && len < (int) sizeof(input) - 1) {
            input[len++] = (char) (key - 'A' + 'a');
            input[len] = '\0';
        }
        /* ignore everything else */
    }

    /* Wipe the prompt row so it doesn't linger. */
    static uint16_t blank2[8 * 8];
    for (int i = 0; i < 64; i++) blank2[i] = TDECK_COLOR_BLACK;
    for (int x = 0; x < 320; x += 8)
        tdeck_display_blit(x, prompt_y, 8, 8, blank2);

    if (len == 0) return -1;
    return nh_lookup_ext_cmd(input);
}

/* shim_select_menu(window, how, menu_list**) fmt "iiip".
 * Returns count selected via ret_ptr.  Currently returns 0 (no selection)
 * after any keypress -- which closes display-only menus cleanly and
 * cancels PICK_ONE / PICK_ANY without picking anything. */
static void
handle_select_menu(va_list *ap_in, void *ret_ptr)
{
    /* args ignored -- we already cached everything in start/add/end */
    (void) ap_in;

    if (g_menu.count == 0) {
        /* No items -- nothing to render.  Close immediately. */
        if (ret_ptr) *(int *) ret_ptr = -1;
        return;
    }

    /* Drain any stale keypress so the menu doesn't auto-dismiss on the
     * keystroke that triggered it. */
    int dummy;
    while (tdeck_keyboard_peek(&dummy)) { }

    render_menu();

    /* Block for input; allow j/k to scroll if there's overflow. */
    for (;;) {
        int key = tdeck_keyboard_getchar(portMAX_DELAY);
        if (key < 0) continue;
        if (key == 'j' && g_menu.count > MENU_VISIBLE_ROWS) {
            if (g_menu.scroll_top + MENU_VISIBLE_ROWS < g_menu.count) {
                g_menu.scroll_top++;
                render_menu();
                continue;
            }
        } else if (key == 'k' && g_menu.scroll_top > 0) {
            g_menu.scroll_top--;
            render_menu();
            continue;
        }
        break;
    }

    /* Force the map to redraw on the next display_nhwindow. */
    map_dirty = true;

    if (ret_ptr) *(int *) ret_ptr = -1;
}

/* --- main callback ----------------------------------------------- */

void
nh_shim_callback(const char *name, void *ret_ptr, const char *fmt, ...)
{
    if (!fmt) {
        ESP_LOGI(TAG, "%s()", name);
        default_return(ret_ptr, 'v');
        return;
    }

    char ret_code = fmt[0];
    const char *arg_codes = fmt + 1;

    bool ret_handled = false;

    va_list ap;
    va_start(ap, fmt);

    /* Intercept the calls we actually render to the LCD.  We start a
     * fresh va_list copy for each handler so they don't fight over the
     * cursor. */
    if (strcmp(name, "shim_create_nhwindow") == 0) {
        va_list ap2;
        va_copy(ap2, ap);
        handle_create_nhwindow(&ap2, ret_ptr);
        va_end(ap2);
        ret_handled = true;
    } else if (strcmp(name, "shim_print_glyph") == 0) {
        va_list ap2;
        va_copy(ap2, ap);
        handle_print_glyph(&ap2);
        va_end(ap2);
    } else if (strcmp(name, "shim_curs") == 0) {
        va_list ap2;
        va_copy(ap2, ap);
        handle_curs(&ap2);
        va_end(ap2);
    } else if (strcmp(name, "shim_clear_nhwindow") == 0) {
        va_list ap2;
        va_copy(ap2, ap);
        handle_clear(&ap2);
        va_end(ap2);
    } else if (strcmp(name, "shim_display_nhwindow") == 0) {
        va_list ap2;
        va_copy(ap2, ap);
        handle_display(&ap2);
        va_end(ap2);
    } else if (strcmp(name, "shim_start_menu") == 0) {
        va_list ap2;
        va_copy(ap2, ap);
        handle_start_menu(&ap2);
        va_end(ap2);
    } else if (strcmp(name, "shim_add_menu") == 0) {
        va_list ap2;
        va_copy(ap2, ap);
        handle_add_menu(&ap2);
        va_end(ap2);
    } else if (strcmp(name, "shim_end_menu") == 0) {
        va_list ap2;
        va_copy(ap2, ap);
        handle_end_menu(&ap2);
        va_end(ap2);
    } else if (strcmp(name, "shim_select_menu") == 0) {
        va_list ap2;
        va_copy(ap2, ap);
        handle_select_menu(&ap2, ret_ptr);
        va_end(ap2);
        ret_handled = true;
    } else if (strcmp(name, "shim_raw_print") == 0
               && arg_codes[0] == 's') {
        /* Show the message on the bottom status line. */
        va_list ap2;
        va_copy(ap2, ap);
        const char *s = va_arg(ap2, const char *);
        draw_status_line(0, s ? s : "", TDECK_COLOR_WHITE, TDECK_COLOR_BLACK);
        va_end(ap2);
    } else if (strcmp(name, "shim_putstr") == 0
               && arg_codes[0] == 'i' && arg_codes[1] == 'i'
               && arg_codes[2] == 's') {
        va_list ap2;
        va_copy(ap2, ap);
        (void) va_arg(ap2, int);                /* window */
        (void) va_arg(ap2, int);                /* attr */
        const char *s = va_arg(ap2, const char *);
        draw_status_line(0, s ? s : "", TDECK_COLOR_WHITE, TDECK_COLOR_BLACK);
        va_end(ap2);
    }

    /* Serial log -- summary form, mostly for diagnostics during bring-up.
     * Skip super-chatty calls now that we render them properly. */
    if (strcmp(name, "shim_print_glyph") != 0
        && strcmp(name, "shim_curs") != 0
        && strcmp(name, "shim_status_update") != 0) {
        char summary[160];
        size_t pos = 0;
        summary[0] = '\0';
        for (size_t i = 0; arg_codes[i] && pos < sizeof(summary) - 8; i++) {
            if (i > 0)
                pos += snprintf(summary + pos, sizeof(summary) - pos, ", ");
            switch (arg_codes[i]) {
            case 'i': case 'b': case '1': case '2': {
                int v = va_arg(ap, int);
                pos += snprintf(summary + pos, sizeof(summary) - pos,
                                "%c=%d", arg_codes[i], v);
                break;
            }
            case 'c': {
                int v = va_arg(ap, int);
                pos += snprintf(summary + pos, sizeof(summary) - pos,
                                "c='%c'", (char) v);
                break;
            }
            case 's': {
                const char *s = va_arg(ap, const char *);
                char strbuf[120];
                log_string_arg(strbuf, sizeof(strbuf), s);
                pos += snprintf(summary + pos, sizeof(summary) - pos,
                                "s=%s", strbuf);
                break;
            }
            case 'p': {
                void *p = va_arg(ap, void *);
                pos += snprintf(summary + pos, sizeof(summary) - pos,
                                "p=%p", p);
                break;
            }
            case '0':
                (void) va_arg(ap, void *);
                pos += snprintf(summary + pos, sizeof(summary) - pos, "0");
                break;
            default:
                pos += snprintf(summary + pos, sizeof(summary) - pos,
                                "?(%c)", arg_codes[i]);
                (void) va_arg(ap, void *);
                break;
            }
        }
        ESP_LOGI(TAG, "%s ret=%c (%s)", name, ret_code, summary);
    }

    va_end(ap);

    /* Input callbacks: block on the keyboard queue for real keypresses. */
    if (ret_handled) {
        /* handler already wrote ret_ptr; leave it alone */
    } else if (strcmp(name, "shim_nhgetch") == 0
               || strcmp(name, "shim_doprev_message") == 0) {
        /* Block forever for a key; NetHack expects an int back. */
        int key = tdeck_keyboard_getchar(portMAX_DELAY);
        if (key < 0) key = '\033';
        if (ret_ptr) *(int *) ret_ptr = key;
    } else if (strcmp(name, "shim_nh_poskey") == 0) {
        /* nh_poskey expects an int char OR a mouse event.  We only
         * deliver keys; for non-key (mouse) events the int is 0 and
         * x/y/mod are set.  Since we have no mouse, just deliver a key.
         * Set *mod=0 too (the three p args are coordxy*, coordxy*, int*). */
        int key = tdeck_keyboard_getchar(portMAX_DELAY);
        if (key < 0) key = '\033';
        if (ret_ptr) *(int *) ret_ptr = key;
        /* The x/y/mod outputs were passed as pointers in args; we
         * don't have easy access to them here, but the windowport docs
         * say they may be left untouched when a real key is returned
         * (NetHack ignores them in that case). */
    } else if (strcmp(name, "shim_yn_function") == 0) {
        /* fmt "css0": return char, query string, response-set string,
         * default char (opaque).  Render the query on the LCD bottom
         * row so the user can see what NetHack is asking, then loop
         * until they give a key in the response set (or ESC). */
        va_list ap_yn;
        va_start(ap_yn, fmt);
        const char *query = va_arg(ap_yn, const char *);
        const char *resp  = va_arg(ap_yn, const char *);
        va_end(ap_yn);

        int prompt_y = 240 - CELL_H;
        /* Clear the row and draw "<query> [resp]" */
        static uint16_t yn_blank[8 * 8];
        for (int i = 0; i < 64; i++) yn_blank[i] = TDECK_COLOR_BLACK;
        for (int x = 0; x < 320; x += 8)
            tdeck_display_blit(x, prompt_y, 8, 8, yn_blank);
        char yn_prompt[64];
        snprintf(yn_prompt, sizeof(yn_prompt), "%s [%s] ",
                 query ? query : "?", resp ? resp : "");
        tdeck_display_print(0, prompt_y, yn_prompt,
                            TDECK_COLOR_GREEN, TDECK_COLOR_BLACK);

        /* Drain any stale keypress so the menu/prompt that opened this
         * doesn't auto-dismiss. */
        int dummy;
        while (tdeck_keyboard_peek(&dummy)) { }

        char picked = '\033';
        for (;;) {
            int key = tdeck_keyboard_getchar(portMAX_DELAY);
            if (key < 0) continue;
            if (key == 0x1B) { picked = '\033'; break; }       /* ESC */
            if (key == '\r' || key == '\n') {
                /* Treat Enter as accepting whatever the default is --
                 * NetHack handles default-on-enter by interpreting '\r'
                 * as the default response. */
                picked = '\r';
                break;
            }
            /* Accept the key if it's in the response set, or if the
             * response set is empty/unspecified. */
            if (!resp || !*resp || strchr(resp, key)) {
                picked = (char) key;
                break;
            }
            /* Otherwise quietly re-prompt: leave the query on screen
             * and read another key. */
        }
        /* Wipe the prompt row when we're done so it doesn't linger. */
        for (int x = 0; x < 320; x += 8)
            tdeck_display_blit(x, prompt_y, 8, 8, yn_blank);
        if (ret_ptr) *(char *) ret_ptr = picked;
    } else if (strcmp(name, "shim_message_menu") == 0) {
        /* Single-key menu: return whatever was typed. */
        int key = tdeck_keyboard_getchar(portMAX_DELAY);
        if (key < 0) key = '\033';
        if (ret_ptr) *(char *) ret_ptr = (char) key;
    } else if (strcmp(name, "shim_get_ext_cmd") == 0) {
        if (ret_ptr) *(int *) ret_ptr = prompt_ext_cmd();
        map_dirty = true;   /* prompt overwrote the bottom row */
    } else {
        default_return(ret_ptr, ret_code);
    }

    vTaskDelay(1);
}
