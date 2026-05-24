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

        /* Render this slice. */
        int y;
        status_pixel_y(line_idx + line, &y);
        if (y + CELL_H > 240) break;
        int x = 0;
        for (int i = 0; i < n && x + 8 <= 320; i++, x += 8) {
            tdeck_display_putchar(x, y, line_start[i], fg, bg);
        }
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
        /* Loop until we get one of the allowed responses, or ESC.
         * Args (per fmt "css0"): query, response-set, default char.
         * For now we just return whatever the user types and let
         * NetHack re-prompt if it doesn't like it. */
        int key = tdeck_keyboard_getchar(portMAX_DELAY);
        if (key < 0) key = '\033';
        if (ret_ptr) *(char *) ret_ptr = (char) key;
    } else if (strcmp(name, "shim_message_menu") == 0) {
        /* Single-key menu: return whatever was typed. */
        int key = tdeck_keyboard_getchar(portMAX_DELAY);
        if (key < 0) key = '\033';
        if (ret_ptr) *(char *) ret_ptr = (char) key;
    } else if (strcmp(name, "shim_select_menu") == 0) {
        /* -1 still means "user cancelled" until we wire up menu nav. */
        if (ret_ptr) *(int *) ret_ptr = -1;
    } else if (strcmp(name, "shim_get_ext_cmd") == 0) {
        if (ret_ptr) *(int *) ret_ptr = -1;
    } else {
        default_return(ret_ptr, ret_code);
    }

    vTaskDelay(1);
}
