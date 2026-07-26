/*
 * © 2021–2026 zefie
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

/****************************************************************************
 * gui_script_editor.c - Real-time BAEScript editor window for zefidi
 *
 * Provides a separate SDL window with syntax-highlighted text editing,
 * live linting, and a toggle to enable/disable script processing during
 * MIDI playback.
 ****************************************************************************/

#ifdef SUPPORT_BAESCRIPT

#include "gui_script_editor.h"
#include "gui_text.h"
#include "gui_theme.h"
#include "gui_widgets.h"
#include "gui_bae.h"
#include "gui_export.h"
#include "baescript.h"
#include "baescript_internal.h"
#if defined(USE_SDL2)
#include <SDL2/SDL.h>
#else
#include <SDL3/SDL.h>
#endif
#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* ── Configuration ─────────────────────────────────────────────────── */

#define SE_WINDOW_W       500
#define SE_WINDOW_H       600
#define SE_LINE_HEIGHT     18
#define SE_PADDING         10
#define SE_GUTTER_W        40      /* line-number gutter width       */
#define SE_TOOLBAR_H       36      /* top toolbar (checkbox + label) */
#define SE_LINT_BAR_H      24      /* bottom lint status bar         */
#define SE_SCROLLBAR_W     14
#define SE_MAX_LINES      1024
#define SE_MAX_LINE_LEN    512
#define SE_MAX_TEXT     (SE_MAX_LINES * SE_MAX_LINE_LEN)
#define SE_TAB_WIDTH        4

/* ── Syntax highlight color categories ─────────────────────────────── */
typedef enum {
    SYN_NORMAL,
    SYN_KEYWORD,     /* var, if, else, while, true, false */
    SYN_BUILTIN,     /* ch, midi, print, noteOn, noteOff  */
    SYN_NUMBER,      /* 42, 0xFF                          */
    SYN_STRING,      /* "hello"                           */
    SYN_COMMENT,     /* // ...  or  / * ... * /            */
    SYN_OPERATOR,    /* + - * / = == != < > etc            */
    SYN_PROPERTY,    /* .instrument, .volume, etc           */
    SYN_ERROR,       /* lint-highlighted tokens             */
    SYN_COUNT
} SyntaxKind;

/* ── Editor state ──────────────────────────────────────────────────── */

/* Simple gap-buffer-esque text storage: just a flat char buffer. */
static char  g_text[SE_MAX_TEXT] = "";
static int   g_text_len = 0;

/* Cursor position (byte offset into g_text) */
static int   g_cursor = 0;

/* Selection anchor (-1 = no selection) */
static int   g_sel_anchor = -1;

/* Scroll offset (in lines, from top) */
static int   g_scroll_y = 0;

/* Horizontal scroll (in pixels) */
static int   g_scroll_x = 0;

/* Undo buffer (simple snapshot) */
#define SE_UNDO_MAX 64
static char *g_undo_buf[SE_UNDO_MAX];
static int   g_undo_cursor[SE_UNDO_MAX];
static int   g_undo_len[SE_UNDO_MAX];
static int   g_undo_count = 0;
static int   g_undo_pos   = 0;

/* Processing toggle */
static bool  g_script_enabled = false;

/* Compiled script context */
static BAEScript_Context *g_script_ctx   = NULL;
static bool  g_script_dirty = true;  /* text changed since last compile */

/* Lint state */
static char  g_lint_message[512] = "";
static int   g_lint_error_line = -1;   /* 1-based line with error */

/* Window state */
static SDL_Window   *g_se_window   = NULL;
static SDL_Renderer *g_se_renderer = NULL;
static bool          g_se_visible  = false;

/* Blink timer for cursor */
static Uint32 g_cursor_blink_time = 0;
static bool   g_cursor_visible = true;

/* Mouse state tracking */
static bool g_mouse_selecting = false;

/* Currently loaded file path (empty = unsaved) */
static char g_loaded_path[1024] = "";

/* ── Console output buffer ──────────────────────────────────────────── */
#define SE_CONSOLE_MAX    (32 * 1024)   /* 32 KB ring buffer           */
#define SE_CONSOLE_DEF_H  120           /* default console panel height */
static char g_console[SE_CONSOLE_MAX] = "";
static int  g_console_len = 0;
static int  g_console_scroll = 0;      /* scroll offset (lines from bottom) */
static int  g_console_h = SE_CONSOLE_DEF_H; /* current panel height       */

static void console_append(const char *text)
{
    int len = (int)strlen(text);
    if (g_console_len + len >= SE_CONSOLE_MAX - 1) {
        /* Shift buffer: drop first half */
        int keep = g_console_len / 2;
        /* Find the next newline after the drop point */
        const char *nl = strchr(g_console + (g_console_len - keep), '\n');
        if (nl) keep = (int)(g_console + g_console_len - nl - 1);
        memmove(g_console, g_console + g_console_len - keep, keep);
        g_console_len = keep;
    }
    memcpy(g_console + g_console_len, text, len);
    g_console_len += len;
    g_console[g_console_len] = '\0';
    g_console_scroll = 0; /* auto-scroll to bottom on new output */
}

static void console_clear(void)
{
    g_console[0] = '\0';
    g_console_len = 0;
    g_console_scroll = 0;
}

static void script_console_output_cb(const char *text, void *userdata)
{
    (void)userdata;
    console_append(text);
}

static void export_tick_adapter(void *userdata);

static void script_stop_cb(void *userdata)
{
    (void)userdata;
    extern BAESong g_live_song;
    extern void gui_panic_all_notes(BAESong song);
    if (g_exporting) {
        // During export this callback runs FROM the export thread.
        // We must NOT call bae_stop_wav_export() (it joins the thread = deadlock).
        // Just signal the thread to exit; cleanup happens in bae_service_wav_export.
        if (g_bae.is_audio_file && g_bae.sound)
            BAESound_Stop(g_bae.sound, FALSE);
        else if (g_bae.song)
            BAESong_Stop(g_bae.song, FALSE);
        g_bae.is_playing = false;
        bae_signal_export_stop();
        return;
    }
    // Normal playback - replicate the full bae_stop behavior
    if (g_bae.is_audio_file && g_bae.sound) {
        BAESound_Stop(g_bae.sound, FALSE);
    } else if (g_bae.song) {
        BAESong_Stop(g_bae.song, FALSE);
        gui_panic_all_notes(g_bae.song);
        if (g_live_song)
            gui_panic_all_notes(g_live_song);
        if (g_bae.mixer)
            for (int i = 0; i < 3; i++)
                BAEMixer_Idle(g_bae.mixer);
        BAESong_SetMicrosecondPosition(g_bae.song, 0);
    }
    g_bae.is_playing = false;
    g_bae.song_finished = false;
}

extern SDL_Window *g_main_window;

/* ── Window title helper ───────────────────────────────────────────── */

static void update_window_title(void)
{
    if (!g_se_window) return;
    char title[256];
    if (g_loaded_path[0]) {
        const char *name = g_loaded_path;
        const char *slash = strrchr(g_loaded_path, '/');
        if (slash) name = slash + 1;
#ifdef _WIN32
        const char *bslash = strrchr(g_loaded_path, '\\');
        if (bslash && bslash > name - 1) name = bslash + 1;
#endif
        snprintf(title, sizeof(title), "BAEScript Editor - %s", name);
    } else {
        snprintf(title, sizeof(title), "BAEScript Editor - Untitled");
    }
    SDL_SetWindowTitle(g_se_window, title);
}

/* ── Helper: line metrics ──────────────────────────────────────────── */

/* Count the number of lines in the text buffer. */
static int count_lines(void)
{
    int n = 1;
    for (int i = 0; i < g_text_len; i++)
        if (g_text[i] == '\n') n++;
    return n;
}

/* Return byte offset of the start of line `line` (0-based). */
static int line_start(int line)
{
    int cur = 0;
    for (int i = 0; i < line && cur < g_text_len; cur++)
        if (g_text[cur] == '\n') i++;
    return cur;
}

/* Return byte offset just past the end of line `line` (before the '\n'). */
static int line_end(int line)
{
    int s = line_start(line);
    int e = s;
    while (e < g_text_len && g_text[e] != '\n') e++;
    return e;
}

/* Return which line a byte offset falls on (0-based). */
static int offset_to_line(int off)
{
    int n = 0;
    for (int i = 0; i < off && i < g_text_len; i++)
        if (g_text[i] == '\n') n++;
    return n;
}

/* Return column (chars from line start) for a byte offset. */
static int offset_to_col(int off)
{
    int ls = line_start(offset_to_line(off));
    return off - ls;
}

/* Convert line + col to byte offset. */
static int line_col_to_offset(int line, int col)
{
    int s = line_start(line);
    int e = line_end(line);
    int off = s + col;
    if (off > e) off = e;
    return off;
}

/* Measure the pixel width of text on a given line up to `col` characters.
   Handles tabs by expanding them to SE_TAB_WIDTH spaces. */
static int measure_col_px(int line, int col)
{
    int ls = line_start(line);
    int le = line_end(line);
    int len = le - ls;
    if (col > len) col = len;

    /* Build a string with tabs expanded to spaces */
    char buf[SE_MAX_LINE_LEN];
    int out = 0;
    for (int i = 0; i < col && i < len && out < (int)sizeof(buf) - 1; i++) {
        char c = g_text[ls + i];
        if (c == '\t') {
            for (int t = 0; t < SE_TAB_WIDTH && out < (int)sizeof(buf) - 1; t++)
                buf[out++] = ' ';
        } else {
            buf[out++] = c;
        }
    }
    buf[out] = '\0';

    int tw = 0, th = 0;
    measure_text(buf, &tw, &th);
    return tw;
}

/* Convert a pixel x-offset within a line to a column index. */
static int px_to_col(int line, int px)
{
    int ls = line_start(line);
    int le = line_end(line);
    int len = le - ls;

    /* Binary-ish search: walk columns, measure, find closest */
    int best_col = 0;
    int best_dist = px; /* distance from col 0 */
    for (int c = 0; c <= len; c++) {
        int cpx = measure_col_px(line, c);
        int dist = (px > cpx) ? (px - cpx) : (cpx - px);
        if (dist < best_dist) {
            best_dist = dist;
            best_col = c;
        }
        if (cpx > px) break; /* past the click point, we found best */
    }
    return best_col;
}

/* ── Undo ──────────────────────────────────────────────────────────── */

static void undo_push(void)
{
    /* Discard any redo entries */
    for (int i = g_undo_pos; i < g_undo_count; i++) {
        free(g_undo_buf[i]);
        g_undo_buf[i] = NULL;
    }
    g_undo_count = g_undo_pos;

    /* If full, shift */
    if (g_undo_count >= SE_UNDO_MAX) {
        free(g_undo_buf[0]);
        memmove(g_undo_buf, g_undo_buf + 1, (SE_UNDO_MAX - 1) * sizeof(g_undo_buf[0]));
        memmove(g_undo_cursor, g_undo_cursor + 1, (SE_UNDO_MAX - 1) * sizeof(g_undo_cursor[0]));
        memmove(g_undo_len, g_undo_len + 1, (SE_UNDO_MAX - 1) * sizeof(g_undo_len[0]));
        g_undo_count--;
    }

    g_undo_buf[g_undo_count] = (char *)malloc(g_text_len + 1);
    if (g_undo_buf[g_undo_count]) {
        memcpy(g_undo_buf[g_undo_count], g_text, g_text_len);
        g_undo_buf[g_undo_count][g_text_len] = '\0';
    }
    g_undo_cursor[g_undo_count] = g_cursor;
    g_undo_len[g_undo_count] = g_text_len;
    g_undo_count++;
    g_undo_pos = g_undo_count;
}

static void do_undo(void)
{
    if (g_undo_pos <= 0) return;
    /* Save current state for redo if we're at the tip */
    if (g_undo_pos == g_undo_count) undo_push();
    g_undo_pos--;
    if (g_undo_buf[g_undo_pos]) {
        memcpy(g_text, g_undo_buf[g_undo_pos], g_undo_len[g_undo_pos]);
        g_text_len = g_undo_len[g_undo_pos];
        g_text[g_text_len] = '\0';
        g_cursor = g_undo_cursor[g_undo_pos];
    }
    g_sel_anchor = -1;
    g_script_dirty = true;
}

static void do_redo(void)
{
    if (g_undo_pos >= g_undo_count - 1) return;
    g_undo_pos++;
    if (g_undo_buf[g_undo_pos]) {
        memcpy(g_text, g_undo_buf[g_undo_pos], g_undo_len[g_undo_pos]);
        g_text_len = g_undo_len[g_undo_pos];
        g_text[g_text_len] = '\0';
        g_cursor = g_undo_cursor[g_undo_pos];
    }
    g_sel_anchor = -1;
    g_script_dirty = true;
}

/* ── Text editing helpers ──────────────────────────────────────────── */

static int sel_start(void) { return (g_sel_anchor < g_cursor) ? g_sel_anchor : g_cursor; }
static int sel_end(void)   { return (g_sel_anchor < g_cursor) ? g_cursor : g_sel_anchor; }
static bool has_selection(void) { return g_sel_anchor >= 0 && g_sel_anchor != g_cursor; }

static void delete_selection(void)
{
    if (!has_selection()) return;
    int s = sel_start(), e = sel_end();
    undo_push();
    memmove(g_text + s, g_text + e, g_text_len - e);
    g_text_len -= (e - s);
    g_text[g_text_len] = '\0';
    g_cursor = s;
    g_sel_anchor = -1;
    g_script_dirty = true;
}

static void insert_text(const char *txt, int len)
{
    if (has_selection()) delete_selection();
    if (g_text_len + len >= SE_MAX_TEXT) return;
    undo_push();
    memmove(g_text + g_cursor + len, g_text + g_cursor, g_text_len - g_cursor);
    memcpy(g_text + g_cursor, txt, len);
    g_text_len += len;
    g_cursor += len;
    g_text[g_text_len] = '\0';
    g_sel_anchor = -1;
    g_script_dirty = true;
}

static void normalize_crlf_inplace(char *buf, int *len)
{
    if (!buf || !len || *len <= 0) return;
    int r = 0;
    int w = 0;
    int n = *len;
    while (r < n) {
        char c = buf[r++];
        if (c == '\r') {
            if (r < n && buf[r] == '\n') {
                continue;
            }
            c = '\n';
        }
        buf[w++] = c;
    }
    buf[w] = '\0';
    *len = w;
}

static void insert_text_normalized(const char *txt, int len)
{
    if (!txt || len <= 0) return;
    if (!memchr(txt, '\r', (size_t)len)) {
        insert_text(txt, len);
        return;
    }

    char *tmp = (char *)malloc((size_t)len + 1u);
    if (!tmp) return;
    memcpy(tmp, txt, (size_t)len);
    tmp[len] = '\0';
    normalize_crlf_inplace(tmp, &len);
    insert_text(tmp, len);
    free(tmp);
}

static void insert_char(char c)
{
    insert_text(&c, 1);
}

static void delete_char_forward(void)
{
    if (has_selection()) { delete_selection(); return; }
    if (g_cursor >= g_text_len) return;
    undo_push();
    memmove(g_text + g_cursor, g_text + g_cursor + 1, g_text_len - g_cursor - 1);
    g_text_len--;
    g_text[g_text_len] = '\0';
    g_script_dirty = true;
}

static void delete_char_back(void)
{
    if (has_selection()) { delete_selection(); return; }
    if (g_cursor <= 0) return;
    g_cursor--;
    undo_push();
    memmove(g_text + g_cursor, g_text + g_cursor + 1, g_text_len - g_cursor - 1);
    g_text_len--;
    g_text[g_text_len] = '\0';
    g_script_dirty = true;
}

/* ── Selection helpers ─────────────────────────────────────────────── */

static void select_all(void)
{
    g_sel_anchor = 0;
    g_cursor = g_text_len;
}

static char *get_selected_text(void)
{
    if (!has_selection()) return NULL;
    int s = sel_start(), e = sel_end();
    int len = e - s;
    char *buf = (char *)malloc(len + 1);
    if (buf) {
        memcpy(buf, g_text + s, len);
        buf[len] = '\0';
    }
    return buf;
}

/* ── Cursor movement ───────────────────────────────────────────────── */

static void cursor_reset_blink(void)
{
    g_cursor_blink_time = SDL_GetTicks();
    g_cursor_visible = true;
}

static void move_left(bool shift)
{
    if (!shift && has_selection()) {
        g_cursor = sel_start();
        g_sel_anchor = -1;
    } else {
        if (!shift) g_sel_anchor = -1;
        else if (g_sel_anchor < 0) g_sel_anchor = g_cursor;
        if (g_cursor > 0) g_cursor--;
    }
    cursor_reset_blink();
}

static void move_right(bool shift)
{
    if (!shift && has_selection()) {
        g_cursor = sel_end();
        g_sel_anchor = -1;
    } else {
        if (!shift) g_sel_anchor = -1;
        else if (g_sel_anchor < 0) g_sel_anchor = g_cursor;
        if (g_cursor < g_text_len) g_cursor++;
    }
    cursor_reset_blink();
}

static void move_up(bool shift)
{
    if (!shift) g_sel_anchor = -1;
    else if (g_sel_anchor < 0) g_sel_anchor = g_cursor;
    int line = offset_to_line(g_cursor);
    int col  = offset_to_col(g_cursor);
    if (line > 0) g_cursor = line_col_to_offset(line - 1, col);
    cursor_reset_blink();
}

static void move_down(bool shift)
{
    if (!shift) g_sel_anchor = -1;
    else if (g_sel_anchor < 0) g_sel_anchor = g_cursor;
    int line = offset_to_line(g_cursor);
    int col  = offset_to_col(g_cursor);
    int nlines = count_lines();
    if (line < nlines - 1) g_cursor = line_col_to_offset(line + 1, col);
    cursor_reset_blink();
}

static void move_home(bool shift)
{
    if (!shift) g_sel_anchor = -1;
    else if (g_sel_anchor < 0) g_sel_anchor = g_cursor;
    int line = offset_to_line(g_cursor);
    g_cursor = line_start(line);
    cursor_reset_blink();
}

static void move_end(bool shift)
{
    if (!shift) g_sel_anchor = -1;
    else if (g_sel_anchor < 0) g_sel_anchor = g_cursor;
    int line = offset_to_line(g_cursor);
    g_cursor = line_end(line);
    cursor_reset_blink();
}

/* ── Ensure cursor is visible (auto-scroll) ─────────────────────────*/

static void ensure_cursor_visible(int win_h)
{
    int line = offset_to_line(g_cursor);
    int content_h = win_h - SE_TOOLBAR_H - g_console_h - SE_LINT_BAR_H - SE_PADDING;
    int visible_lines = content_h / SE_LINE_HEIGHT;
    if (visible_lines < 1) visible_lines = 1;

    if (line < g_scroll_y) g_scroll_y = line;
    if (line >= g_scroll_y + visible_lines) g_scroll_y = line - visible_lines + 1;
}

/* ── Mouse click to cursor position ─────────────────────────────────*/

static int mouse_to_offset(int mx, int my, int win_w, int win_h)
{
    (void)win_w;
    int content_x = SE_PADDING + SE_GUTTER_W;
    int content_y = SE_TOOLBAR_H;

    int click_line = g_scroll_y + (my - content_y) / SE_LINE_HEIGHT;
    int nlines = count_lines();
    if (click_line < 0) click_line = 0;
    if (click_line >= nlines) click_line = nlines - 1;

    /* Convert pixel position to column using actual text measurement */
    int px = mx - content_x + g_scroll_x;
    if (px < 0) px = 0;
    int click_col = px_to_col(click_line, px);

    return line_col_to_offset(click_line, click_col);
}

/* ── Syntax highlighting ───────────────────────────────────────────── */

static SDL_Color syntax_color(SyntaxKind kind)
{
    if (g_is_dark_mode) {
        switch (kind) {
            case SYN_KEYWORD:   return (SDL_Color){198, 120, 221, 255}; /* purple */
            case SYN_BUILTIN:   return (SDL_Color){ 97, 175, 239, 255}; /* blue   */
            case SYN_NUMBER:    return (SDL_Color){209, 154, 102, 255}; /* orange */
            case SYN_STRING:    return (SDL_Color){152, 195, 121, 255}; /* green  */
            case SYN_COMMENT:   return (SDL_Color){128, 128, 128, 255}; /* gray   */
            case SYN_OPERATOR:  return (SDL_Color){200, 200, 200, 255}; /* light  */
            case SYN_PROPERTY:  return (SDL_Color){224, 108, 117, 255}; /* red    */
            case SYN_ERROR:     return (SDL_Color){255,  80,  80, 255}; /* bright red */
            default:            return (SDL_Color){220, 220, 220, 255}; /* white-ish */
        }
    } else {
        switch (kind) {
            case SYN_KEYWORD:   return (SDL_Color){160,  32, 240, 255};
            case SYN_BUILTIN:   return (SDL_Color){  0,  92, 197, 255};
            case SYN_NUMBER:    return (SDL_Color){  0, 128, 128, 255};
            case SYN_STRING:    return (SDL_Color){  0, 128,   0, 255};
            case SYN_COMMENT:   return (SDL_Color){128, 128, 128, 255};
            case SYN_OPERATOR:  return (SDL_Color){ 60,  60,  60, 255};
            case SYN_PROPERTY:  return (SDL_Color){215,  58,  73, 255};
            case SYN_ERROR:     return (SDL_Color){200,   0,   0, 255};
            default:            return (SDL_Color){ 30,  30,  30, 255};
        }
    }
}

/* Classify a word for syntax highlighting. */
static SyntaxKind classify_word(const char *word, int len)
{
    /* Keywords */
    if ((len == 3 && strncmp(word, "var", 3) == 0) ||
        (len == 2 && strncmp(word, "if", 2) == 0)  ||
        (len == 4 && strncmp(word, "else", 4) == 0) ||
        (len == 5 && strncmp(word, "while", 5) == 0) ||
        (len == 4 && strncmp(word, "true", 4) == 0) ||
        (len == 5 && strncmp(word, "false", 5) == 0))
        return SYN_KEYWORD;

    /* Built-ins */
    if ((len == 2 && strncmp(word, "ch", 2) == 0) ||
        (len == 4 && strncmp(word, "midi", 4) == 0) ||
        (len == 5 && strncmp(word, "print", 5) == 0) ||
        (len == 4 && strncmp(word, "help", 4) == 0) ||
        (len == 6 && strncmp(word, "noteOn", 6) == 0) ||
        (len == 7 && strncmp(word, "noteOff", 7) == 0) ||
        (len == 4 && strncmp(word, "stop", 4) == 0))
        return SYN_BUILTIN;

    /* Properties (instrument, volume, pan, expression, pitchbend, mute, timestamp, length) */
    if ((len == 10 && strncmp(word, "instrument", 10) == 0) ||
        (len == 6 && strncmp(word, "volume", 6) == 0) ||
        (len == 3 && strncmp(word, "pan", 3) == 0) ||
        (len == 10 && strncmp(word, "expression", 10) == 0) ||
        (len == 9 && strncmp(word, "pitchbend", 9) == 0) ||
        (len == 4 && strncmp(word, "mute", 4) == 0) ||
        (len == 9 && strncmp(word, "timestamp", 9) == 0) ||
        (len == 8 && strncmp(word, "position", 8) == 0) ||
        (len == 6 && strncmp(word, "length", 6) == 0) ||
        (len == 9 && strncmp(word, "exporting", 9) == 0))
        return SYN_PROPERTY;

    return SYN_NORMAL;
}

/* ── Lint (try to parse, capture errors) ───────────────────────────── */

/* We redirect stderr to capture parse errors. Since BAEScript_Parse writes
   errors to stderr, we'll try to parse and check the return value.
   For a cleaner approach, we re-lex the source and validate structure. */

static void lint_update(void)
{
    if (!g_script_dirty) return;
    g_script_dirty = false;

    /* Free old context */
    if (g_script_ctx) {
        BAEScript_Free(g_script_ctx);
        g_script_ctx = NULL;
    }

    if (g_text_len == 0) {
        g_lint_message[0] = '\0';
        g_lint_error_line = -1;
        return;
    }

    /* Try to parse */
    g_script_ctx = BAEScript_LoadString(g_text);
    if (!g_script_ctx) {
        /* Walk through the source with the lexer to find the error location */
        BAEScript_Lexer lex;
        BAEScript_Lexer_Init(&lex, g_text);
        int last_good_line = 1;
        for (;;) {
            BAEScript_Token tok = BAEScript_Lexer_Next(&lex);
            if (tok.type == TOK_EOF) break;
            if (tok.type == TOK_ERROR) {
                snprintf(g_lint_message, sizeof(g_lint_message),
                         "Line %d: %s", tok.line, tok.value.str);
                g_lint_error_line = tok.line;
                return;
            }
            last_good_line = tok.line;
        }
        /* Lexer succeeded but parser failed */
        snprintf(g_lint_message, sizeof(g_lint_message),
                 "Syntax error near line %d", last_good_line);
        g_lint_error_line = last_good_line;
    } else {
        g_lint_message[0] = '\0';
        g_lint_error_line = -1;
        /* Set output callback so print() goes to the console */
        BAEScript_SetOutputCallback(g_script_ctx, script_console_output_cb, NULL);
        BAEScript_SetStopCallback(g_script_ctx, script_stop_cb, NULL);
    }
}

/* ── Rendering helpers ─────────────────────────────────────────────── */

static void draw_se_rect(SDL_Renderer *R, int x, int y, int w, int h, SDL_Color c)
{
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
#if defined(USE_SDL2)
    SDL_Rect r = {x, y, w, h};
    SDL_RenderFillRect(R, &r);
#else
    SDL_FRect fr = {(float)x, (float)y, (float)w, (float)h};
    SDL_RenderFillRect(R, &fr);
#endif
}

static void draw_se_frame(SDL_Renderer *R, int x, int y, int w, int h, SDL_Color c)
{
    SDL_SetRenderDrawColor(R, c.r, c.g, c.b, c.a);
#if defined(USE_SDL2)
    SDL_Rect r = {x, y, w, h};
    SDL_RenderDrawRect(R, &r);
#else
    SDL_FRect fr = {(float)x, (float)y, (float)w, (float)h};
    SDL_RenderRect(R, &fr);
#endif
}

/* Draw a single line of source code with syntax highlighting.
   `line_text` is a NUL-terminated string for one line.
   Returns nothing; draws at (x, y) clipped to content area. */
static void draw_highlighted_line(SDL_Renderer *R, int x, int y,
                                  const char *line_text, int line_len,
                                  int clip_x, int clip_w)
{
    (void)clip_x; (void)clip_w;
    int cx = x;
    int i = 0;
    int tw = 0, th = 0;

    /* Helper: measure a space character once for tab/space advancing */
    measure_text(" ", &tw, &th);
    int space_w = tw;

    while (i < line_len) {
        char c = line_text[i];
        SyntaxKind kind = SYN_NORMAL;

        /* Comments: // line comment */
        if (c == '/' && i + 1 < line_len && line_text[i + 1] == '/') {
            char buf[SE_MAX_LINE_LEN];
            int len = line_len - i;
            if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
            memcpy(buf, line_text + i, len);
            buf[len] = '\0';
            draw_text(R, cx, y, buf, syntax_color(SYN_COMMENT));
            return;
        }

        /* String literal */
        if (c == '"' || c == '\'') {
            char quote = c;
            int j = i + 1;
            while (j < line_len && line_text[j] != quote) {
                if (line_text[j] == '\\' && j + 1 < line_len) j++;
                j++;
            }
            if (j < line_len) j++;
            char buf[SE_MAX_LINE_LEN];
            int len = j - i;
            if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;
            memcpy(buf, line_text + i, len);
            buf[len] = '\0';
            draw_text(R, cx, y, buf, syntax_color(SYN_STRING));
            measure_text(buf, &tw, &th);
            cx += tw;
            i = j;
            continue;
        }

        /* Number */
        if (isdigit((unsigned char)c)) {
            int j = i;
            if (c == '0' && j + 1 < line_len && (line_text[j+1] == 'x' || line_text[j+1] == 'X')) {
                j += 2;
                while (j < line_len && isxdigit((unsigned char)line_text[j])) j++;
            } else {
                while (j < line_len && isdigit((unsigned char)line_text[j])) j++;
            }
            char buf[64];
            int len = j - i;
            if (len > 63) len = 63;
            memcpy(buf, line_text + i, len);
            buf[len] = '\0';
            draw_text(R, cx, y, buf, syntax_color(SYN_NUMBER));
            measure_text(buf, &tw, &th);
            cx += tw;
            i = j;
            continue;
        }

        /* Word (identifier/keyword/builtin) */
        if (isalpha((unsigned char)c) || c == '_') {
            int j = i;
            while (j < line_len && (isalnum((unsigned char)line_text[j]) || line_text[j] == '_')) j++;
            int len = j - i;
            kind = classify_word(line_text + i, len);

            if (kind == SYN_PROPERTY && i > 0 && line_text[i-1] != '.')
                kind = SYN_NORMAL;

            char buf[256];
            if (len > 255) len = 255;
            memcpy(buf, line_text + i, len);
            buf[len] = '\0';
            draw_text(R, cx, y, buf, syntax_color(kind));
            measure_text(buf, &tw, &th);
            cx += tw;
            i = j;
            continue;
        }

        /* Operator characters */
        if (strchr("+-*/%=!<>&|", c)) {
            char buf[4] = {c, '\0'};
            if (i + 1 < line_len) {
                char c2 = line_text[i+1];
                if ((c == '=' && c2 == '=') || (c == '!' && c2 == '=') ||
                    (c == '<' && c2 == '=') || (c == '>' && c2 == '=') ||
                    (c == '&' && c2 == '&') || (c == '|' && c2 == '|')) {
                    buf[1] = c2; buf[2] = '\0';
                    i++;
                }
            }
            draw_text(R, cx, y, buf, syntax_color(SYN_OPERATOR));
            measure_text(buf, &tw, &th);
            cx += tw;
            i += 1;
            continue;
        }

        /* Punctuation / delimiters and whitespace */
        {
            if (c == '\t') {
                cx += space_w * SE_TAB_WIDTH;
            } else if (c == ' ') {
                cx += space_w;
            } else {
                char buf[2] = {c, '\0'};
                draw_text(R, cx, y, buf, syntax_color(SYN_NORMAL));
                measure_text(buf, &tw, &th);
                cx += tw;
            }
            i++;
        }
    }
}

/* ── Public API ────────────────────────────────────────────────────── */

void script_editor_init(void)
{
    memset(g_text, 0, sizeof(g_text));
    g_text_len = 0;
    g_cursor = 0;
    g_sel_anchor = -1;
    g_scroll_y = 0;
    g_scroll_x = 0;
    g_script_enabled = false;
    g_script_dirty = true;
    g_lint_message[0] = '\0';
    g_lint_error_line = -1;
    g_undo_count = 0;
    g_undo_pos = 0;
    memset(g_undo_buf, 0, sizeof(g_undo_buf));

    /* Insert a starter template */
    const char *starter =
        "// BAEScript editor\n"
        "// Toggle 'Enable' to run this script during playback.\n"
        "\n"
        "// Example: force all channels to piano\n"
        "// var i = 0;\n"
        "// while (i < 16) {\n"
        "//     ch[i].instrument = 0;\n"
        "//     i = i + 1;\n"
        "// }\n";
    int slen = (int)strlen(starter);
    memcpy(g_text, starter, slen);
    g_text_len = slen;
    g_text[g_text_len] = '\0';

    /* Register export tick so the script engine ticks during export */
    bae_set_export_tick_callback(export_tick_adapter, NULL);
}

void script_editor_shutdown(void)
{
    if (g_script_ctx) {
        BAEScript_Free(g_script_ctx);
        g_script_ctx = NULL;
    }
    if (g_se_window) {
        SDL_DestroyRenderer(g_se_renderer);
        SDL_DestroyWindow(g_se_window);
        g_se_window = NULL;
        g_se_renderer = NULL;
    }
    for (int i = 0; i < g_undo_count; i++) {
        free(g_undo_buf[i]);
        g_undo_buf[i] = NULL;
    }
    g_undo_count = 0;
    g_undo_pos = 0;
}

void script_editor_hide(void)
{
    if (!g_se_visible) return;
    if (g_se_window) {
        SDL_HideWindow(g_se_window);
#if defined(USE_SDL2)
        SDL_StopTextInput();
#else
        SDL_StopTextInput(g_se_window);
#endif
    }
    g_se_visible = false;
}

void script_editor_show(void)
{
    if (g_se_visible) return;

    if (!g_se_window) {
        /* Position to the right of the main window */
        int main_x = 100, main_y = 100;
        if (g_main_window) {
            SDL_GetWindowPosition(g_main_window, &main_x, &main_y);
            int main_w = 0, main_h = 0;
            SDL_GetWindowSize(g_main_window, &main_w, &main_h);
            main_x += main_w + 8; /* 8px gap to the right */
        }

        g_se_window = SDL_CreateWindow(
            "BAEScript Editor",
#if defined(USE_SDL2)
            main_x, main_y,
#endif
            SE_WINDOW_W,
            SE_WINDOW_H,
            SDL_WINDOW_RESIZABLE
        );
        if (!g_se_window) return;

#if !defined(USE_SDL2)
        SDL_SetWindowPosition(g_se_window, main_x, main_y);
#endif

#if defined(USE_SDL2)
        g_se_renderer = SDL_CreateRenderer(g_se_window, -1, 0);
#else
        g_se_renderer = SDL_CreateRenderer(g_se_window, NULL);
#endif
        if (!g_se_renderer) {
            SDL_DestroyWindow(g_se_window);
            g_se_window = NULL;
            return;
        }
    }

    SDL_ShowWindow(g_se_window);
    update_window_title();
    g_se_visible = true;
#if defined(USE_SDL2)
    SDL_StartTextInput();
#else
    SDL_StartTextInput(g_se_window);
#endif
}

void script_editor_toggle(void)
{
    if (g_se_visible)
        script_editor_hide();
    else
        script_editor_show();
}

bool script_editor_is_visible(void)
{
    return g_se_visible;
}

bool script_editor_is_enabled(void)
{
    return g_script_enabled && g_script_ctx != NULL;
}

const char *script_editor_get_path(void)
{
    return g_loaded_path;
}

const char *script_editor_get_text(void)
{
    return g_text;
}

bool script_editor_get_enabled(void)
{
    return g_script_enabled;
}

void script_editor_restore_state(const char *path, const char *text, bool enabled)
{
    if (path && path[0]) {
        /* Try to load the file; if it exists, use it */
        FILE *f = fopen(path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz >= 0) {
                if (sz > SE_MAX_TEXT - 1) sz = SE_MAX_TEXT - 1;
                fseek(f, 0, SEEK_SET);
                int n = (int)fread(g_text, 1, (size_t)sz, f);
                g_text[n] = '\0';
                g_text_len = n;
                normalize_crlf_inplace(g_text, &g_text_len);
            }
            fclose(f);
            snprintf(g_loaded_path, sizeof(g_loaded_path), "%s", path);
        } else if (text && text[0]) {
            /* File gone, use saved text */
            int len = (int)strlen(text);
            if (len > SE_MAX_TEXT - 1) len = SE_MAX_TEXT - 1;
            memcpy(g_text, text, len);
            g_text[len] = '\0';
            g_text_len = len;
            normalize_crlf_inplace(g_text, &g_text_len);
            g_loaded_path[0] = '\0';
        }
    } else if (text && text[0]) {
        int len = (int)strlen(text);
        if (len > SE_MAX_TEXT - 1) len = SE_MAX_TEXT - 1;
        memcpy(g_text, text, len);
        g_text[len] = '\0';
        g_text_len = len;
        normalize_crlf_inplace(g_text, &g_text_len);
        g_loaded_path[0] = '\0';
    }
    g_cursor = 0;
    g_sel_anchor = -1;
    g_scroll_y = 0;
    g_scroll_x = 0;
    g_script_enabled = enabled;
    g_script_dirty = true;
    update_window_title();
}

/* ── File I/O ──────────────────────────────────────────────────────── */

static void script_load_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return; }
    if (sz > SE_MAX_TEXT - 1) sz = SE_MAX_TEXT - 1;
    fseek(f, 0, SEEK_SET);
    int n = (int)fread(g_text, 1, (size_t)sz, f);
    fclose(f);
    g_text[n] = '\0';
    g_text_len = n;
    normalize_crlf_inplace(g_text, &g_text_len);
    g_cursor = 0;
    g_sel_anchor = -1;
    g_scroll_y = 0;
    g_scroll_x = 0;
    g_script_dirty = true;
    snprintf(g_loaded_path, sizeof(g_loaded_path), "%s", path);
    update_window_title();
}

static void script_save_file(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fwrite(g_text, 1, (size_t)g_text_len, f);
    fclose(f);
    snprintf(g_loaded_path, sizeof(g_loaded_path), "%s", path);
    update_window_title();
}

#ifdef _WIN32
static void do_open_dialog(void)
{
    char fileBuf[1024] = {0};
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "BAEScript Files (*.bscript)\0*.bscript\0"
                      "All Files\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrDefExt = "bscript";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (GetOpenFileNameA(&ofn)) {
        undo_push();
        script_load_file(fileBuf);
    }
}

static void do_save_dialog(void)
{
    char fileBuf[1024] = {0};
    if (g_loaded_path[0])
        snprintf(fileBuf, sizeof(fileBuf), "%s", g_loaded_path);
    else
        strcpy(fileBuf, "script.bscript");
    OPENFILENAMEA ofn;
    ZeroMemory(&ofn, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = NULL;
    ofn.lpstrFilter = "BAEScript Files (*.bscript)\0*.bscript\0"
                      "All Files\0*.*\0";
    ofn.lpstrFile = fileBuf;
    ofn.nMaxFile = sizeof(fileBuf);
    ofn.lpstrDefExt = "bscript";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameA(&ofn)) {
        script_save_file(fileBuf);
    }
}
#elif defined(__APPLE__)
static void do_open_dialog(void)
{
    FILE *fp = popen("osascript -e 'POSIX path of (choose file with prompt \"Open BAEScript\" of type {\"bscript\"})' 2>/dev/null", "r");
    if (!fp) return;
    char buf[1024];
    if (fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        size_t l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r'))
            buf[--l] = '\0';
        if (l > 0) {
            undo_push();
            script_load_file(buf);
        }
    } else {
        pclose(fp);
    }
}

static void do_save_dialog(void)
{
    const char *def = g_loaded_path[0] ? g_loaded_path : "script.bscript";
    char cmd[2048];
    snprintf(cmd, sizeof(cmd),
        "osascript -e 'POSIX path of (choose file name with prompt \"Save BAEScript As\" default name \"%s\")' 2>/dev/null",
        def);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char buf[1024];
    if (fgets(buf, sizeof(buf), fp)) {
        pclose(fp);
        size_t l = strlen(buf);
        while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r'))
            buf[--l] = '\0';
        if (l > 0)
            script_save_file(buf);
    } else {
        pclose(fp);
    }
}
#else
/* Linux: try zenity, kdialog, yad in order */
static void do_open_dialog(void)
{
    const char *cmds[] = {
        "zenity --file-selection --title='Open BAEScript' "
            "--file-filter='BAEScript Files | *.bscript' --file-filter='All Files | *' 2>/dev/null",
        "kdialog --getopenfilename . '*.bscript' 2>/dev/null",
        "yad --file-selection --title='Open BAEScript' 2>/dev/null",
        NULL
    };
    for (int i = 0; cmds[i]; ++i) {
        FILE *p = popen(cmds[i], "r");
        if (!p) continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p)) {
            pclose(p);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r'))
                buf[--l] = '\0';
            if (l > 0) {
                undo_push();
                script_load_file(buf);
                return;
            }
        } else {
            pclose(p);
        }
    }
}

static void do_save_dialog(void)
{
    const char *def = g_loaded_path[0] ? g_loaded_path : "script.bscript";
    char cmd_zenity[1024], cmd_kdialog[1024], cmd_yad[1024];
    snprintf(cmd_zenity, sizeof(cmd_zenity),
        "zenity --file-selection --save --confirm-overwrite --title='Save BAEScript As' "
        "--filename='%s' --file-filter='BAEScript Files | *.bscript' --file-filter='All Files | *' 2>/dev/null",
        def);
    snprintf(cmd_kdialog, sizeof(cmd_kdialog),
        "kdialog --getsavefilename '%s' '*.bscript' 2>/dev/null", def);
    snprintf(cmd_yad, sizeof(cmd_yad),
        "yad --file-selection --save --confirm-overwrite --title='Save BAEScript As' "
        "--filename='%s' 2>/dev/null", def);
    const char *cmds[] = { cmd_zenity, cmd_kdialog, cmd_yad, NULL };
    for (int i = 0; cmds[i]; ++i) {
        FILE *p = popen(cmds[i], "r");
        if (!p) continue;
        char buf[1024];
        if (fgets(buf, sizeof(buf), p)) {
            pclose(p);
            size_t l = strlen(buf);
            while (l > 0 && (buf[l-1] == '\n' || buf[l-1] == '\r'))
                buf[--l] = '\0';
            if (l > 0) {
                script_save_file(buf);
                return;
            }
        } else {
            pclose(p);
        }
    }
}
#endif

void script_editor_tick(void)
{
    if (!g_script_enabled || !g_script_ctx) return;
    if (!g_bae.song) return;

    uint32_t pos_ms = (uint32_t)bae_get_pos_ms();
    uint32_t len_ms = g_bae.song_length_us / 1000;

    BAEScript_SetSong(g_script_ctx, g_bae.song);
    BAEScript_SetExporting(g_script_ctx, g_exporting ? 1 : 0);
    BAEScript_Tick(g_script_ctx, pos_ms, len_ms);
}

static void export_tick_adapter(void *userdata)
{
    (void)userdata;
    script_editor_tick();
}

void script_editor_reset_exporter_options(void)
{
    if (!g_script_ctx) return;
#if BAESCRIPT_EXPORTER_LOOPCOUNT == TRUE
    BAEScript_ResetExporterOptions(g_script_ctx);
#endif
}

bool script_editor_get_exporter_loopcount(int *outLoopCount)
{
    if (!g_script_enabled || !g_script_ctx || !outLoopCount) return false;
#if BAESCRIPT_EXPORTER_LOOPCOUNT == TRUE
    return BAEScript_GetExporterLoopCount(g_script_ctx, outLoopCount) ? true : false;
#else
    (void)outLoopCount;
    return false;
#endif
}

/* ── Event handling ────────────────────────────────────────────────── */

bool script_editor_handle_event(SDL_Event *event)
{
    if (!g_se_visible || !g_se_window || !event) return false;

    /* SDL2 returns Uint32; SDL3 exposes SDL_WindowID. */
#if defined(USE_SDL2)
    Uint32 se_win_id = SDL_GetWindowID(g_se_window);
#else
    SDL_WindowID se_win_id = SDL_GetWindowID(g_se_window);
#endif
    bool is_ours = false;

    switch (event->type) {
#if defined(USE_SDL2)
        case SDL_WINDOWEVENT:
#else
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        case SDL_EVENT_WINDOW_SHOWN:
        case SDL_EVENT_WINDOW_HIDDEN:
        case SDL_EVENT_WINDOW_EXPOSED:
        case SDL_EVENT_WINDOW_MOVED:
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_MINIMIZED:
        case SDL_EVENT_WINDOW_MAXIMIZED:
        case SDL_EVENT_WINDOW_RESTORED:
        case SDL_EVENT_WINDOW_MOUSE_ENTER:
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
        case SDL_EVENT_WINDOW_FOCUS_LOST:
#endif
            is_ours = (event->window.windowID == se_win_id);
            break;
#if defined(USE_SDL2)
        case SDL_KEYDOWN:
        case SDL_KEYUP:
#else
        case SDL_EVENT_KEY_DOWN:
        case SDL_EVENT_KEY_UP:
#endif
            is_ours = (event->key.windowID == se_win_id);
            break;
#if defined(USE_SDL2)
        case SDL_MOUSEMOTION:
#else
        case SDL_EVENT_MOUSE_MOTION:
#endif
            is_ours = (event->motion.windowID == se_win_id);
            break;
#if defined(USE_SDL2)
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
#else
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
#endif
            is_ours = (event->button.windowID == se_win_id);
            break;
#if defined(USE_SDL2)
        case SDL_MOUSEWHEEL:
#else
        case SDL_EVENT_MOUSE_WHEEL:
#endif
            is_ours = (event->wheel.windowID == se_win_id);
            break;
#if defined(USE_SDL2)
        case SDL_TEXTINPUT:
#else
        case SDL_EVENT_TEXT_INPUT:
#endif
            is_ours = (event->text.windowID == se_win_id);
            break;
    }

    if (!is_ours) return false;

    /* Window close */
#if defined(USE_SDL2)
    if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_CLOSE) {
#else
    if (event->type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
#endif
        if (event->window.windowID == se_win_id) {
            script_editor_hide();
            return true;
        }
        /* Main window close - hide ourselves */
        script_editor_hide();
        return false;
    }

    int win_w = SE_WINDOW_W, win_h = SE_WINDOW_H;
    SDL_GetWindowSize(g_se_window, &win_w, &win_h);

    /* Text input */
#if defined(USE_SDL2)
    if (event->type == SDL_TEXTINPUT) {
#else
    if (event->type == SDL_EVENT_TEXT_INPUT) {
#endif
        insert_text_normalized(event->text.text, (int)strlen(event->text.text));
        cursor_reset_blink();
        ensure_cursor_visible(win_h);
        return true;
    }

    /* Keyboard */
#if defined(USE_SDL2)
    if (event->type == SDL_KEYDOWN) {
        SDL_Keycode key = event->key.keysym.sym;
        SDL_Keymod mod = event->key.keysym.mod;
#else
    if (event->type == SDL_EVENT_KEY_DOWN) {
        SDL_Keycode key = event->key.key;
        SDL_Keymod mod = event->key.mod;
#endif
        bool ctrl =
#if defined(USE_SDL2)
            (mod & KMOD_CTRL) != 0;
#else
            (mod & SDL_KMOD_CTRL) != 0;
#endif
        bool shift =
#if defined(USE_SDL2)
            (mod & KMOD_SHIFT) != 0;
#else
            (mod & SDL_KMOD_SHIFT) != 0;
#endif

        if (ctrl) {
            if (key == SDLK_A) { select_all(); return true; }
            if (key == SDLK_C) {
                char *sel = get_selected_text();
                if (sel) { SDL_SetClipboardText(sel); free(sel); }
                return true;
            }
            if (key == SDLK_X) {
                char *sel = get_selected_text();
                if (sel) { SDL_SetClipboardText(sel); free(sel); }
                delete_selection();
                ensure_cursor_visible(win_h);
                return true;
            }
            if (key == SDLK_V) {
                char *clip = SDL_GetClipboardText();
                if (clip && clip[0]) {
                    insert_text_normalized(clip, (int)strlen(clip));
                    ensure_cursor_visible(win_h);
                }
                SDL_free(clip);
                return true;
            }
            if (key == SDLK_Z) { do_undo(); ensure_cursor_visible(win_h); return true; }
            if (key == SDLK_Y) { do_redo(); ensure_cursor_visible(win_h); return true; }
            if (key == SDLK_O) { do_open_dialog(); return true; }
            if (key == SDLK_S) {
                if (g_loaded_path[0])
                    script_save_file(g_loaded_path);
                else
                    do_save_dialog();
                return true;
            }
            return true;
        }

        switch (key) {
            case SDLK_RETURN:
            case SDLK_KP_ENTER: {
                /* Auto-indent: match leading whitespace of current line */
                int cur_line = offset_to_line(g_cursor);
                int ls = line_start(cur_line);
                char indent[128] = "";
                int ind_len = 0;
                for (int p = ls; p < g_text_len && (g_text[p] == ' ' || g_text[p] == '\t') && ind_len < 126; p++)
                    indent[ind_len++] = g_text[p];
                indent[ind_len] = '\0';

                insert_char('\n');
                if (ind_len > 0)
                    insert_text(indent, ind_len);
                ensure_cursor_visible(win_h);
                return true;
            }
            case SDLK_TAB: {
                /* Insert spaces */
                char spaces[SE_TAB_WIDTH + 1];
                memset(spaces, ' ', SE_TAB_WIDTH);
                spaces[SE_TAB_WIDTH] = '\0';
                insert_text(spaces, SE_TAB_WIDTH);
                ensure_cursor_visible(win_h);
                return true;
            }
            case SDLK_BACKSPACE:
                delete_char_back();
                ensure_cursor_visible(win_h);
                return true;
            case SDLK_DELETE:
                delete_char_forward();
                ensure_cursor_visible(win_h);
                return true;
            case SDLK_LEFT:   move_left(shift);  ensure_cursor_visible(win_h); return true;
            case SDLK_RIGHT:  move_right(shift); ensure_cursor_visible(win_h); return true;
            case SDLK_UP:     move_up(shift);    ensure_cursor_visible(win_h); return true;
            case SDLK_DOWN:   move_down(shift);  ensure_cursor_visible(win_h); return true;
            case SDLK_HOME:   move_home(shift);  ensure_cursor_visible(win_h); return true;
            case SDLK_END:    move_end(shift);   ensure_cursor_visible(win_h); return true;
            case SDLK_ESCAPE:
                /* Deselect */
                g_sel_anchor = -1;
                return true;
            default: break;
        }
        return true;
    }

    /* Mouse button */
#if defined(USE_SDL2)
    if (event->type == SDL_MOUSEBUTTONDOWN && event->button.button == SDL_BUTTON_LEFT) {
#else
    if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN && event->button.button == SDL_BUTTON_LEFT) {
#endif
        int mx = (int)event->button.x;
        int my = (int)event->button.y;

        /* Check if clicking the enable checkbox area */
        int cbx = SE_PADDING;
        int cbsize = 18;
        int cb_cby = (SE_TOOLBAR_H - cbsize) / 2;
        int tw_en_hit, th_en_hit;
        measure_text("Enable", &tw_en_hit, &th_en_hit);
        if (mx >= cbx && mx < cbx + cbsize + 6 + tw_en_hit && my >= cb_cby && my < cb_cby + cbsize) {
            g_script_enabled = !g_script_enabled;
            return true;
        }

        /* Check if clicking Open/Save buttons */
        if (my < SE_TOOLBAR_H) {
            int tw_en, th_en;
            measure_text("Enable", &tw_en, &th_en);
            int btn_x = cbx + cbsize + 6 + tw_en + 12;
            int btn_h = 22;
            int btn_y = (SE_TOOLBAR_H - btn_h) / 2;
            int tw_open, th_open, tw_save, th_save, tw_saveas, th_saveas, tw_close, th_close;
            measure_text("Open", &tw_open, &th_open);
            measure_text("Save", &tw_save, &th_save);
            measure_text("Save As", &tw_saveas, &th_saveas);
            measure_text("Close", &tw_close, &th_close);
            int btn_pad = 10;
            int open_w = tw_open + btn_pad * 2;
            int save_x = btn_x + open_w + 6;
            int save_w = tw_save + btn_pad * 2;
            int saveas_x = save_x + save_w + 6;
            int saveas_w = tw_saveas + btn_pad * 2;
            int close_x = saveas_x + saveas_w + 6;
            int close_w = tw_close + btn_pad * 2;

            if (mx >= btn_x && mx < btn_x + open_w && my >= btn_y && my < btn_y + btn_h) {
                do_open_dialog();
                return true;
            }
            if (mx >= save_x && mx < save_x + save_w && my >= btn_y && my < btn_y + btn_h) {
                if (g_loaded_path[0])
                    script_save_file(g_loaded_path);
                else
                    do_save_dialog();
                return true;
            }
            if (mx >= saveas_x && mx < saveas_x + saveas_w && my >= btn_y && my < btn_y + btn_h) {
                do_save_dialog();
                return true;
            }
            if (mx >= close_x && mx < close_x + close_w && my >= btn_y && my < btn_y + btn_h) {
                g_loaded_path[0] = '\0';
                g_text[0] = '\0';
                g_text_len = 0;
                g_cursor = 0;
                g_sel_anchor = 0;
                g_scroll_y = 0;
                g_scroll_x = 0;
                g_script_dirty = true;
                lint_update();
                update_window_title();
                return true;
            }
        }

        /* Click in console header - Clear button */
        if (g_console_h > 0 && my >= win_h - g_console_h - SE_LINT_BAR_H
            && my < win_h - g_console_h - SE_LINT_BAR_H + 22) {
            int tw_clr, th_clr;
            measure_text("Clear", &tw_clr, &th_clr);
            int clr_w = tw_clr + 12;
            int clr_x = win_w - SE_PADDING - clr_w;
            if (mx >= clr_x && mx < clr_x + clr_w) {
                console_clear();
                return true;
            }
        }

        /* Click in editor area - set cursor */
        if (my >= SE_TOOLBAR_H && my < win_h - g_console_h - SE_LINT_BAR_H) {
#if defined(USE_SDL2)
            bool shift = (SDL_GetModState() & KMOD_SHIFT) != 0;
#else
            bool shift = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
#endif
            int off = mouse_to_offset(mx, my, win_w, win_h);
            if (shift) {
                if (g_sel_anchor < 0) g_sel_anchor = g_cursor;
            } else {
                g_sel_anchor = off;
            }
            g_cursor = off;
            g_mouse_selecting = true;
            cursor_reset_blink();
        }
        return true;
    }

#if defined(USE_SDL2)
    if (event->type == SDL_MOUSEBUTTONUP && event->button.button == SDL_BUTTON_LEFT) {
#else
    if (event->type == SDL_EVENT_MOUSE_BUTTON_UP && event->button.button == SDL_BUTTON_LEFT) {
#endif
        if (g_mouse_selecting) {
            g_mouse_selecting = false;
            /* If anchor == cursor, clear selection */
            if (g_sel_anchor == g_cursor) g_sel_anchor = -1;
        }
        return true;
    }

    /* Mouse motion (for drag-select) */
#if defined(USE_SDL2)
    if (event->type == SDL_MOUSEMOTION && g_mouse_selecting) {
#else
    if (event->type == SDL_EVENT_MOUSE_MOTION && g_mouse_selecting) {
#endif
        int mx = (int)event->motion.x;
        int my = (int)event->motion.y;
        g_cursor = mouse_to_offset(mx, my, win_w, win_h);
        cursor_reset_blink();
        return true;
    }

    /* Mouse wheel for scrolling */
#if defined(USE_SDL2)
    if (event->type == SDL_MOUSEWHEEL) {
        int delta = event->wheel.y;
        int my; SDL_GetMouseState(NULL, &my);
#else
    if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        int delta = (int)event->wheel.y;
        float mouse_y_f; SDL_GetMouseState(NULL, &mouse_y_f);
        int my = (int)mouse_y_f;
#endif
        int con_body_y = win_h - g_console_h - SE_LINT_BAR_H + 22;
        int con_bottom = win_h - SE_LINT_BAR_H;
        if (my >= con_body_y && my < con_bottom) {
            /* scroll console */
            g_console_scroll -= delta * 3;
            if (g_console_scroll < 0) g_console_scroll = 0;
            int con_lines = 0;
            for (int i = 0; i < g_console_len; i++)
                if (g_console[i] == '\n') con_lines++;
            int con_vis = (g_console_h - 22) / SE_LINE_HEIGHT;
            int max_scroll = con_lines - con_vis;
            if (max_scroll < 0) max_scroll = 0;
            if (g_console_scroll > max_scroll)
                g_console_scroll = max_scroll;
        } else {
            /* scroll editor */
            g_scroll_y -= delta * 3;
            if (g_scroll_y < 0) g_scroll_y = 0;
            int nlines = count_lines();
            int content_h = win_h - SE_TOOLBAR_H - g_console_h - SE_LINT_BAR_H - SE_PADDING;
            int visible_lines = content_h / SE_LINE_HEIGHT;
            if (g_scroll_y > nlines - visible_lines)
                g_scroll_y = nlines - visible_lines;
            if (g_scroll_y < 0) g_scroll_y = 0;
        }
        return true;
    }

    return is_ours; /* consume any other event that belongs to us */
}

/* ── Rendering ─────────────────────────────────────────────────────── */

void script_editor_render(void)
{
    if (!g_se_visible || !g_se_window || !g_se_renderer) return;

    SDL_Renderer *R = g_se_renderer;
    int win_w = SE_WINDOW_W, win_h = SE_WINDOW_H;
    SDL_GetWindowSize(g_se_window, &win_w, &win_h);

    /* Update lint */
    lint_update();

    /* Background */
    SDL_Color bg = g_is_dark_mode ? (SDL_Color){30, 30, 30, 255} : (SDL_Color){255, 255, 255, 255};
    SDL_SetRenderDrawColor(R, bg.r, bg.g, bg.b, 255);
    SDL_RenderClear(R);

    /* ── Toolbar ────────────────────────────────────────────────── */
    SDL_Color toolbar_bg = g_is_dark_mode ? (SDL_Color){45, 45, 45, 255} : (SDL_Color){240, 240, 240, 255};
    draw_se_rect(R, 0, 0, win_w, SE_TOOLBAR_H, toolbar_bg);
    /* Separator line */
    SDL_Color sep = g_panel_border;
    draw_se_rect(R, 0, SE_TOOLBAR_H - 1, win_w, 1, sep);

    /* Enable checkbox */
    int cbsize = 18;
    int cbx = SE_PADDING;
    int cby = (SE_TOOLBAR_H - cbsize) / 2;
    Rect cbRect = {cbx, cby, cbsize, cbsize};

    /* Get mouse position for hover */
    int mouse_x = 0, mouse_y = 0;
#if defined(USE_SDL2)
    SDL_GetMouseState(&mouse_x, &mouse_y);
#else
    float fmx, fmy;
    SDL_GetMouseState(&fmx, &fmy);
    mouse_x = (int)fmx;
    mouse_y = (int)fmy;
#endif
    /* Only use mouse pos if this window is focused */
    /* SDL2 returns Uint32; SDL3 exposes SDL_WindowID. */
#if defined(USE_SDL2)
    Uint32 focus_id = SDL_GetWindowID(g_se_window);
#else
    SDL_WindowID focus_id = SDL_GetWindowID(g_se_window);
#endif
    SDL_Window *focused_win = SDL_GetKeyboardFocus();
    bool has_focus = focused_win && SDL_GetWindowID(focused_win) == focus_id;
    if (!has_focus) { mouse_x = -1000; mouse_y = -1000; }

    bool cb_hover = point_in(mouse_x, mouse_y, cbRect);
    draw_custom_checkbox(R, cbRect, g_script_enabled, cb_hover);

    /* Label */
    SDL_Color label_col = g_text_color;
    draw_text(R, cbx + cbsize + 6, cby + 2, "Enable", label_col);

    /* Open / Save buttons */
    {
        int tw_en, th_en;
        measure_text("Enable", &tw_en, &th_en);
        int btn_x = cbx + cbsize + 6 + tw_en + 12;
        int btn_h = 22;
        int btn_y = (SE_TOOLBAR_H - btn_h) / 2;
        int tw_open, th_open, tw_save, th_save, tw_saveas, th_saveas;
        measure_text("Open", &tw_open, &th_open);
        measure_text("Save", &tw_save, &th_save);
        measure_text("Save As", &tw_saveas, &th_saveas);
        int btn_pad = 10;

        /* Open button */
        int open_w = tw_open + btn_pad * 2;
        Rect openR = {btn_x, btn_y, open_w, btn_h};
        bool open_hover = point_in(mouse_x, mouse_y, openR);
        SDL_Color btn_bg = open_hover
            ? (g_is_dark_mode ? (SDL_Color){70, 70, 70, 255} : (SDL_Color){210, 210, 210, 255})
            : (g_is_dark_mode ? (SDL_Color){55, 55, 55, 255} : (SDL_Color){225, 225, 225, 255});
        draw_se_rect(R, openR.x, openR.y, openR.w, openR.h, btn_bg);
        draw_se_rect(R, openR.x, openR.y, openR.w, 1, sep);
        draw_se_rect(R, openR.x, openR.y + openR.h - 1, openR.w, 1, sep);
        draw_se_rect(R, openR.x, openR.y, 1, openR.h, sep);
        draw_se_rect(R, openR.x + openR.w - 1, openR.y, 1, openR.h, sep);
        draw_text(R, openR.x + btn_pad, openR.y + (btn_h - th_open) / 2, "Open", label_col);

        /* Save button */
        int save_x = btn_x + open_w + 6;
        int save_w = tw_save + btn_pad * 2;
        Rect saveR = {save_x, btn_y, save_w, btn_h};
        bool save_hover = point_in(mouse_x, mouse_y, saveR);
        btn_bg = save_hover
            ? (g_is_dark_mode ? (SDL_Color){70, 70, 70, 255} : (SDL_Color){210, 210, 210, 255})
            : (g_is_dark_mode ? (SDL_Color){55, 55, 55, 255} : (SDL_Color){225, 225, 225, 255});
        draw_se_rect(R, saveR.x, saveR.y, saveR.w, saveR.h, btn_bg);
        draw_se_rect(R, saveR.x, saveR.y, saveR.w, 1, sep);
        draw_se_rect(R, saveR.x, saveR.y + saveR.h - 1, saveR.w, 1, sep);
        draw_se_rect(R, saveR.x, saveR.y, 1, saveR.h, sep);
        draw_se_rect(R, saveR.x + saveR.w - 1, saveR.y, 1, saveR.h, sep);
        draw_text(R, saveR.x + btn_pad, saveR.y + (btn_h - th_save) / 2, "Save", label_col);

        /* Save As button */
        int saveas_x = save_x + save_w + 6;
        int saveas_w = tw_saveas + btn_pad * 2;
        Rect saveasR = {saveas_x, btn_y, saveas_w, btn_h};
        bool saveas_hover = point_in(mouse_x, mouse_y, saveasR);
        btn_bg = saveas_hover
            ? (g_is_dark_mode ? (SDL_Color){70, 70, 70, 255} : (SDL_Color){210, 210, 210, 255})
            : (g_is_dark_mode ? (SDL_Color){55, 55, 55, 255} : (SDL_Color){225, 225, 225, 255});
        draw_se_rect(R, saveasR.x, saveasR.y, saveasR.w, saveasR.h, btn_bg);
        draw_se_rect(R, saveasR.x, saveasR.y, saveasR.w, 1, sep);
        draw_se_rect(R, saveasR.x, saveasR.y + saveasR.h - 1, saveasR.w, 1, sep);
        draw_se_rect(R, saveasR.x, saveasR.y, 1, saveasR.h, sep);
        draw_se_rect(R, saveasR.x + saveasR.w - 1, saveasR.y, 1, saveasR.h, sep);
        draw_text(R, saveasR.x + btn_pad, saveasR.y + (btn_h - th_saveas) / 2, "Save As", label_col);

        /* Close button */
        int tw_close, th_close;
        measure_text("Close", &tw_close, &th_close);
        int close_x = saveas_x + saveas_w + 6;
        int close_w = tw_close + btn_pad * 2;
        Rect closeR = {close_x, btn_y, close_w, btn_h};
        bool close_hover = point_in(mouse_x, mouse_y, closeR);
        btn_bg = close_hover
            ? (g_is_dark_mode ? (SDL_Color){70, 70, 70, 255} : (SDL_Color){210, 210, 210, 255})
            : (g_is_dark_mode ? (SDL_Color){55, 55, 55, 255} : (SDL_Color){225, 225, 225, 255});
        draw_se_rect(R, closeR.x, closeR.y, closeR.w, closeR.h, btn_bg);
        draw_se_rect(R, closeR.x, closeR.y, closeR.w, 1, sep);
        draw_se_rect(R, closeR.x, closeR.y + closeR.h - 1, closeR.w, 1, sep);
        draw_se_rect(R, closeR.x, closeR.y, 1, closeR.h, sep);
        draw_se_rect(R, closeR.x + closeR.w - 1, closeR.y, 1, closeR.h, sep);
        draw_text(R, closeR.x + btn_pad, closeR.y + (btn_h - th_close) / 2, "Close", label_col);
    }

    /* Status indicator */
    {
        int tw, th;
        const char *status = g_script_enabled ? (g_script_ctx ? "Running" : "Error") : "Disabled";
        SDL_Color status_col = g_script_enabled
            ? (g_script_ctx ? (SDL_Color){80, 200, 80, 255} : (SDL_Color){200, 80, 80, 255})
            : (SDL_Color){150, 150, 150, 255};
        measure_text(status, &tw, &th);
        draw_text(R, win_w - SE_PADDING - tw, cby + 2, status, status_col);
    }

    /* ── Editor area ────────────────────────────────────────────── */
    int content_x = SE_PADDING + SE_GUTTER_W;
    int content_y = SE_TOOLBAR_H;
    int content_w = win_w - content_x - SE_SCROLLBAR_W - SE_PADDING;
    int content_h = win_h - SE_TOOLBAR_H - g_console_h - SE_LINT_BAR_H;
    int visible_lines = content_h / SE_LINE_HEIGHT;
    if (visible_lines < 1) visible_lines = 1;
    int nlines = count_lines();

    /* Set clipping rectangle for editor area */
#if defined(USE_SDL2)
    SDL_Rect clip = {0, content_y, win_w, content_h};
    SDL_RenderSetClipRect(R, &clip);
#else
    SDL_Rect clip = {0, content_y, win_w, content_h};
    SDL_SetRenderClipRect(R, &clip);
#endif

    /* Gutter background */
    SDL_Color gutter_bg = g_is_dark_mode ? (SDL_Color){40, 40, 40, 255} : (SDL_Color){235, 235, 235, 255};
    draw_se_rect(R, 0, content_y, SE_PADDING + SE_GUTTER_W - 4, content_h, gutter_bg);

    /* Draw selection highlight */
    if (has_selection()) {
        int ss = sel_start(), se = sel_end();
        int s_line = offset_to_line(ss), e_line = offset_to_line(se);
        SDL_Color sel_bg = g_is_dark_mode ? (SDL_Color){50, 80, 140, 180} : (SDL_Color){180, 210, 255, 180};

        for (int l = s_line; l <= e_line; l++) {
            if (l < g_scroll_y || l >= g_scroll_y + visible_lines) continue;
            int dy = content_y + (l - g_scroll_y) * SE_LINE_HEIGHT;
            int ls_off = line_start(l), le_off = line_end(l);

            int sel_col_start = (l == s_line) ? (ss - ls_off) : 0;
            int sel_col_end   = (l == e_line) ? (se - ls_off) : (le_off - ls_off);

            int sx = content_x + measure_col_px(l, sel_col_start) - g_scroll_x;
            int sw = measure_col_px(l, sel_col_end) - measure_col_px(l, sel_col_start);
            if (sw <= 0 && l != e_line) {
                int _tw, _th;
                measure_text(" ", &_tw, &_th);
                sw = _tw;
            }
            draw_se_rect(R, sx, dy, sw, SE_LINE_HEIGHT, sel_bg);
        }
    }

    /* Draw error line highlight */
    if (g_lint_error_line > 0) {
        int err_l = g_lint_error_line - 1; /* 0-based */
        if (err_l >= g_scroll_y && err_l < g_scroll_y + visible_lines) {
            int dy = content_y + (err_l - g_scroll_y) * SE_LINE_HEIGHT;
            SDL_Color err_bg = g_is_dark_mode ? (SDL_Color){80, 30, 30, 100} : (SDL_Color){255, 220, 220, 150};
            draw_se_rect(R, SE_PADDING + SE_GUTTER_W - 4, dy, win_w - SE_GUTTER_W - SE_PADDING, SE_LINE_HEIGHT, err_bg);
        }
    }

    /* Draw lines */
    for (int l = g_scroll_y; l < nlines && l < g_scroll_y + visible_lines; l++) {
        int dy = content_y + (l - g_scroll_y) * SE_LINE_HEIGHT;

        /* Line number */
        char lnum[16];
        snprintf(lnum, sizeof(lnum), "%3d", l + 1);
        SDL_Color lnum_col = g_is_dark_mode ? (SDL_Color){100, 100, 100, 255} : (SDL_Color){150, 150, 150, 255};
        /* Highlight current line number */
        if (l == offset_to_line(g_cursor))
            lnum_col = g_is_dark_mode ? (SDL_Color){180, 180, 180, 255} : (SDL_Color){80, 80, 80, 255};
        draw_text(R, SE_PADDING, dy + 1, lnum, lnum_col);

        /* Line text with syntax highlighting */
        int ls = line_start(l);
        int le = line_end(l);
        int line_len = le - ls;
        char line_buf[SE_MAX_LINE_LEN];
        if (line_len > (int)sizeof(line_buf) - 1) line_len = (int)sizeof(line_buf) - 1;
        memcpy(line_buf, g_text + ls, line_len);
        line_buf[line_len] = '\0';

        draw_highlighted_line(R, content_x - g_scroll_x, dy + 1, line_buf, line_len, content_x, content_w);
    }

    /* Draw cursor */
    if (has_focus) {
        Uint32 now = SDL_GetTicks();
        if (now - g_cursor_blink_time > 500) {
            g_cursor_visible = !g_cursor_visible;
            g_cursor_blink_time = now;
        }
        if (g_cursor_visible) {
            int c_line = offset_to_line(g_cursor);
            int c_col  = offset_to_col(g_cursor);
            if (c_line >= g_scroll_y && c_line < g_scroll_y + visible_lines) {
                int cx = content_x + measure_col_px(c_line, c_col) - g_scroll_x;
                int cy = content_y + (c_line - g_scroll_y) * SE_LINE_HEIGHT;
                SDL_Color cur_col = g_is_dark_mode ? (SDL_Color){255, 255, 255, 255} : (SDL_Color){0, 0, 0, 255};
                draw_se_rect(R, cx, cy, 2, SE_LINE_HEIGHT, cur_col);
            }
        }
    }

    /* Remove clip */
#if defined(USE_SDL2)
    SDL_RenderSetClipRect(R, NULL);
#else
    SDL_SetRenderClipRect(R, NULL);
#endif

    /* ── Scrollbar ──────────────────────────────────────────────── */
    if (nlines > visible_lines) {
        int sb_x = win_w - SE_SCROLLBAR_W;
        int sb_y = content_y;
        int sb_h = content_h;
        SDL_Color sb_bg = g_is_dark_mode ? (SDL_Color){45, 45, 45, 255} : (SDL_Color){230, 230, 230, 255};
        draw_se_rect(R, sb_x, sb_y, SE_SCROLLBAR_W, sb_h, sb_bg);

        /* Thumb */
        float ratio = (float)visible_lines / (float)nlines;
        int thumb_h = (int)(sb_h * ratio);
        if (thumb_h < 20) thumb_h = 20;
        float scroll_ratio = (float)g_scroll_y / (float)(nlines - visible_lines);
        int thumb_y = sb_y + (int)((sb_h - thumb_h) * scroll_ratio);
        SDL_Color thumb_col = g_is_dark_mode ? (SDL_Color){80, 80, 80, 255} : (SDL_Color){180, 180, 180, 255};
        draw_se_rect(R, sb_x + 2, thumb_y, SE_SCROLLBAR_W - 4, thumb_h, thumb_col);
    }

    /* ── Console output panel ────────────────────────────────────── */
    if (g_console_h > 0) {
        int con_y = win_h - g_console_h - SE_LINT_BAR_H;
        int con_header_h = 22;

        /* Console header bar */
        SDL_Color con_hdr_bg = g_is_dark_mode ? (SDL_Color){38, 38, 38, 255} : (SDL_Color){238, 238, 238, 255};
        draw_se_rect(R, 0, con_y, win_w, con_header_h, con_hdr_bg);
        draw_se_rect(R, 0, con_y, win_w, 1, sep);

        SDL_Color con_hdr_col = g_is_dark_mode ? (SDL_Color){170, 170, 170, 255} : (SDL_Color){80, 80, 80, 255};
        draw_text(R, SE_PADDING, con_y + 3, "Console", con_hdr_col);

        /* Clear button */
        {
            int tw_clr, th_clr;
            measure_text("Clear", &tw_clr, &th_clr);
            int clr_w = tw_clr + 12;
            int clr_h = con_header_h - 4;
            int clr_x = win_w - SE_PADDING - clr_w;
            int clr_y = con_y + 2;
            bool clr_hover = has_focus && mouse_x >= clr_x && mouse_x < clr_x + clr_w
                             && mouse_y >= clr_y && mouse_y < clr_y + clr_h;
            SDL_Color clr_bg = clr_hover
                ? (g_is_dark_mode ? (SDL_Color){70, 70, 70, 255} : (SDL_Color){210, 210, 210, 255})
                : (g_is_dark_mode ? (SDL_Color){50, 50, 50, 255} : (SDL_Color){228, 228, 228, 255});
            draw_se_rect(R, clr_x, clr_y, clr_w, clr_h, clr_bg);
            draw_text(R, clr_x + 6, clr_y + (clr_h - th_clr) / 2, "Clear", con_hdr_col);
        }

        /* Console body */
        int con_body_y = con_y + con_header_h;
        int con_body_h = g_console_h - con_header_h;
        SDL_Color con_bg = g_is_dark_mode ? (SDL_Color){25, 25, 25, 255} : (SDL_Color){250, 250, 250, 255};
        draw_se_rect(R, 0, con_body_y, win_w, con_body_h, con_bg);

        /* Set clip for console body */
#if defined(USE_SDL2)
        SDL_Rect con_clip = {0, con_body_y, win_w, con_body_h};
        SDL_RenderSetClipRect(R, &con_clip);
#else
        SDL_Rect con_clip = {0, con_body_y, win_w, con_body_h};
        SDL_SetRenderClipRect(R, &con_clip);
#endif

        /* Draw console text lines */
        if (g_console_len > 0) {
            SDL_Color con_text_col = g_is_dark_mode ? (SDL_Color){180, 200, 180, 255} : (SDL_Color){40, 60, 40, 255};
            int max_vis_lines = con_body_h / SE_LINE_HEIGHT;
            /* Count total lines in console */
            int total_con_lines = 1;
            for (int i = 0; i < g_console_len; i++)
                if (g_console[i] == '\n') total_con_lines++;
            /* If last char is newline, don't count trailing empty line */
            if (g_console_len > 0 && g_console[g_console_len - 1] == '\n')
                total_con_lines--;

            /* Calculate start line (scrolled from bottom) */
            int start_line = total_con_lines - max_vis_lines - g_console_scroll;
            if (start_line < 0) start_line = 0;

            /* Find the byte offset for start_line */
            int cur_line = 0;
            int byte_off = 0;
            while (cur_line < start_line && byte_off < g_console_len) {
                if (g_console[byte_off] == '\n') cur_line++;
                byte_off++;
            }

            /* Draw visible lines */
            int dy = con_body_y + 2;
            int drawn = 0;
            while (byte_off < g_console_len && drawn < max_vis_lines) {
                /* Find end of line */
                int eol = byte_off;
                while (eol < g_console_len && g_console[eol] != '\n') eol++;
                int line_len = eol - byte_off;
                if (line_len > 0) {
                    char lbuf[512];
                    if (line_len > (int)sizeof(lbuf) - 1) line_len = (int)sizeof(lbuf) - 1;
                    memcpy(lbuf, g_console + byte_off, line_len);
                    lbuf[line_len] = '\0';
                    draw_text(R, SE_PADDING, dy, lbuf, con_text_col);
                }
                dy += SE_LINE_HEIGHT;
                drawn++;
                byte_off = eol + 1;
            }
        }

        /* Remove console clip */
#if defined(USE_SDL2)
        SDL_RenderSetClipRect(R, NULL);
#else
        SDL_SetRenderClipRect(R, NULL);
#endif
    }

    /* ── Lint status bar ────────────────────────────────────────── */
    SDL_Color lint_bg = g_is_dark_mode ? (SDL_Color){35, 35, 35, 255} : (SDL_Color){245, 245, 245, 255};
    draw_se_rect(R, 0, win_h - SE_LINT_BAR_H, win_w, SE_LINT_BAR_H, lint_bg);
    draw_se_rect(R, 0, win_h - SE_LINT_BAR_H, win_w, 1, sep);

    if (g_lint_message[0]) {
        SDL_Color err_col = (SDL_Color){220, 60, 60, 255};
        draw_text(R, SE_PADDING, win_h - SE_LINT_BAR_H + 4, g_lint_message, err_col);
    } else {
        /* Show cursor position */
        char pos_buf[64];
        int c_line = offset_to_line(g_cursor);
        int c_col  = offset_to_col(g_cursor);
        snprintf(pos_buf, sizeof(pos_buf), "Ln %d, Col %d", c_line + 1, c_col + 1);
        SDL_Color pos_col = g_is_dark_mode ? (SDL_Color){140, 140, 140, 255} : (SDL_Color){120, 120, 120, 255};
        draw_text(R, SE_PADDING, win_h - SE_LINT_BAR_H + 4, pos_buf, pos_col);
    }

    SDL_RenderPresent(R);
}

#endif /* SUPPORT_BAESCRIPT */
