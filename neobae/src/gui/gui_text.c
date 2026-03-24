// gui_text.c - Text rendering and measurement

#include "gui_text.h"
#include <string.h>

// Global font variables
TTF_Font *g_font = NULL;
int g_bitmap_font_scale = 2; // fallback bitmap scale

// Minimal 5x7 digit glyphs for fallback use (only digits needed for UI layout centering)
static const unsigned char kGlyph5x7Digits[10][7] = {
    {0x1E, 0x21, 0x23, 0x25, 0x29, 0x31, 0x1E}, // 0
    {0x08, 0x18, 0x08, 0x08, 0x08, 0x08, 0x1C}, // 1
    {0x1E, 0x21, 0x01, 0x0E, 0x10, 0x20, 0x3F}, // 2
    {0x1E, 0x21, 0x01, 0x0E, 0x01, 0x21, 0x1E}, // 3
    {0x02, 0x06, 0x0A, 0x12, 0x22, 0x3F, 0x02}, // 4
    {0x3F, 0x20, 0x3E, 0x01, 0x01, 0x21, 0x1E}, // 5
    {0x0E, 0x10, 0x20, 0x3E, 0x21, 0x21, 0x1E}, // 6
    {0x3F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10}, // 7
    {0x1E, 0x21, 0x21, 0x1E, 0x21, 0x21, 0x1E}, // 8
    {0x1E, 0x21, 0x21, 0x1F, 0x01, 0x02, 0x1C}, // 9
};

void gui_set_font_scale(int scale)
{
    if (scale < 1)
        scale = 1;
    g_bitmap_font_scale = scale;
}

void bitmap_draw(SDL_Renderer *R, int x, int y, const char *text, SDL_Color col)
{
    SDL_SetRenderDrawColor(R, col.r, col.g, col.b, col.a);
    for (const char *p = text; *p; ++p)
    {
        unsigned char c = *p;
        if (c >= '0' && c <= '9')
        {
            const unsigned char *g = kGlyph5x7Digits[c - '0'];
            for (int row = 0; row < 7; row++)
            {
                unsigned char bits = g[row];
                for (int bit = 0; bit < 5; bit++)
                { // 5 columns (bit4..0)
                    /* use unsigned shift and explicit mask to avoid negative shift UB */
                    unsigned int mask = 1u << (4 - bit);
                    if (bits & mask)
                    {
#if defined(USE_SDL2)
                        SDL_Rect rr = {x + bit * g_bitmap_font_scale, y + row * g_bitmap_font_scale, g_bitmap_font_scale, g_bitmap_font_scale};
                        SDL_RenderFillRect(R, &rr);
#else
                        SDL_FRect rr = {(float)(x + bit * g_bitmap_font_scale), (float)(y + row * g_bitmap_font_scale), (float)g_bitmap_font_scale, (float)g_bitmap_font_scale};
                        SDL_RenderFillRect(R, &rr);
#endif
                    }
                }
            }
        }
    x += 6 * g_bitmap_font_scale; // glyph width (5) + spacing (1)
    }
}

void measure_text(const char *text, int *w, int *h)
{
    if (!text)
    {
        if (w)
            *w = 0;
        if (h)
            *h = 0;
        return;
    }
    if (g_font)
    {
        int tw = 0, th = 0;
#if defined(USE_SDL2)
        if (TTF_SizeUTF8(g_font, text, &tw, &th) == 0)
#else
        if (TTF_GetStringSize(g_font, text, XStrLen(text), &tw, &th) == true)
#endif
        {
            if (w)
                *w = tw;
            if (h)
                *h = th;
            return;
        }
    }
    int len = (int)strlen(text);
    if (w)
        *w = len * (5 * g_bitmap_font_scale + g_bitmap_font_scale);
    if (h)
        *h = 7 * g_bitmap_font_scale;
}

void draw_text(SDL_Renderer *R, int x, int y, const char *text, SDL_Color col)
{
    if (g_font)
    {
#if defined(USE_SDL2)
        SDL_Surface *s = TTF_RenderUTF8_Blended(g_font, text, col);
#else
        SDL_Surface *s = TTF_RenderText_Blended(g_font, text, 0, col);
#endif
        if (s)
        {
            SDL_Texture *tx = SDL_CreateTextureFromSurface(R, s);
#if defined(USE_SDL2)
            SDL_Rect dst = {x, y, s->w, s->h};
            SDL_RenderCopy(R, tx, NULL, &dst);
#else
            SDL_FRect dst = {(float)x, (float)y, (float)s->w, (float)s->h};
            SDL_RenderTexture(R, tx, NULL, &dst);
#endif
            SDL_DestroyTexture(tx);
#if defined(USE_SDL2)
            SDL_FreeSurface(s);
#else
            SDL_DestroySurface(s);
#endif
            return;
        }
    }
    bitmap_draw(R, x, y, text, col);
}

static int count_wrapped_single_line(const char *text, int max_w)
{
    if (!text || !*text)
        return 0;
    int lines = 0;
    char buf[1024];
    buf[0] = '\0';
    const char *p = text;
    while (*p)
    {
        // Extract next word
        const char *q = p;
        while (*q && *q != ' ' && *q != '\t')
            q++;
        int wlen = (int)(q - p);
        char word[512];
        if (wlen >= (int)sizeof(word))
            wlen = (int)sizeof(word) - 1;
        safe_strncpy(word, p, wlen);
        word[wlen] = '\0';

        char attempt[1536];
        if (buf[0])
            snprintf(attempt, sizeof(attempt), "%s %s", buf, word);
        else
            snprintf(attempt, sizeof(attempt), "%s", word);
        int tw, th;
        measure_text(attempt, &tw, &th);
        if (tw <= max_w)
        {
            if (buf[0])
                strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, word, sizeof(buf) - strlen(buf) - 1);
        }
        else
        {
            if (buf[0])
            {
                lines++;
                buf[0] = '\0';
            }
            measure_text(word, &tw, &th);
            if (tw <= max_w)
            {
                safe_strncpy(buf, word, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
            }
            else
            {
                // Break long word into chunks that fit
                int start = 0, len = (int)strlen(word);
                while (start < len)
                {
                    int take = len - start;
                    while (take > 0)
                    {
                        char sub[512];
                        if (take >= (int)sizeof(sub))
                            take = (int)sizeof(sub) - 1;
                        safe_strncpy(sub, word + start, take);
                        sub[take] = '\0';
                        measure_text(sub, &tw, &th);
                        if (tw <= max_w)
                            break;
                        take--;
                    }
                    if (take == 0)
                        take = 1;
                    start += take;
                    lines++;
                }
                buf[0] = '\0';
            }
        }
        // Advance past whitespace
        p = q;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    if (buf[0])
        lines++;
    return lines;
}

static int draw_wrapped_single_line(SDL_Renderer *R, int x, int y, const char *text, SDL_Color col, int max_w, int lineH)
{
    if (!text || !*text)
        return 0;
    int lines = 0;
    char buf[1024];
    buf[0] = '\0';
    const char *p = text;
    while (*p)
    {
        const char *q = p;
        while (*q && *q != ' ' && *q != '\t')
            q++;
        int wlen = (int)(q - p);
        char word[512];
        if (wlen >= (int)sizeof(word))
            wlen = (int)sizeof(word) - 1;
        safe_strncpy(word, p, wlen);
        word[wlen] = '\0';

        char attempt[1536];
        if (buf[0])
            snprintf(attempt, sizeof(attempt), "%s %s", buf, word);
        else
            snprintf(attempt, sizeof(attempt), "%s", word);
        int tw, th;
        measure_text(attempt, &tw, &th);
        if (tw <= max_w)
        {
            if (buf[0])
                strncat(buf, " ", sizeof(buf) - strlen(buf) - 1);
            strncat(buf, word, sizeof(buf) - strlen(buf) - 1);
        }
        else
        {
            if (buf[0])
            {
                draw_text(R, x, y + lines * lineH, buf, col);
                lines++;
                buf[0] = '\0';
            }
            measure_text(word, &tw, &th);
            if (tw <= max_w)
            {
                safe_strncpy(buf, word, sizeof(buf) - 1);
                buf[sizeof(buf) - 1] = '\0';
            }
            else
            {
                int start = 0, len = (int)strlen(word);
                while (start < len)
                {
                    int take = len - start;
                    while (take > 0)
                    {
                        char sub[512];
                        if (take >= (int)sizeof(sub))
                            take = (int)sizeof(sub) - 1;
                        safe_strncpy(sub, word + start, take);
                        sub[take] = '\0';
                        measure_text(sub, &tw, &th);
                        if (tw <= max_w)
                            break;
                        take--;
                    }
                    if (take == 0)
                        take = 1;
                    char sub[512];
                    if (take >= (int)sizeof(sub))
                        take = (int)sizeof(sub) - 1;
                    safe_strncpy(sub, word + start, take);
                    sub[take] = '\0';
                    draw_text(R, x, y + lines * lineH, sub, col);
                    lines++;
                    start += take;
                }
                buf[0] = '\0';
            }
        }
        p = q;
        while (*p == ' ' || *p == '\t')
            p++;
    }
    if (buf[0])
    {
        draw_text(R, x, y + lines * lineH, buf, col);
        lines++;
    }
    return lines;
}

// Simple word-wrapping helpers used by RMF Info dialog.
// Returns number of wrapped lines that the text would occupy within max_w pixels.
int count_wrapped_lines(const char *text, int max_w)
{
    int total;
    const char *p;

    if (!text || !*text)
        return 0;

    total = 0;
    p = text;
    while (1)
    {
        const char *nl;
        char line[2048];
        int len;

        nl = strchr(p, '\n');
        len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len >= (int)sizeof(line))
            len = (int)sizeof(line) - 1;
        safe_strncpy(line, p, len);
        line[len] = '\0';

        if (line[0])
            total += count_wrapped_single_line(line, max_w);
        else
            total += 1;

        if (!nl)
            break;
        p = nl + 1;
        if (*p == '\0')
        {
            total += 1;
            break;
        }
    }

    return total;
}

// Draw text with simple word-wrapping within max_w pixels. Returns number of lines drawn.
int draw_wrapped_text(SDL_Renderer *R, int x, int y, const char *text, SDL_Color col, int max_w, int lineH)
{
    int lines;
    const char *p;

    if (!text || !*text)
        return 0;

    lines = 0;
    p = text;
    while (1)
    {
        const char *nl;
        char line[2048];
        int len;
        int drawn;

        nl = strchr(p, '\n');
        len = nl ? (int)(nl - p) : (int)strlen(p);
        if (len >= (int)sizeof(line))
            len = (int)sizeof(line) - 1;
        safe_strncpy(line, p, len);
        line[len] = '\0';

        if (line[0])
        {
            drawn = draw_wrapped_single_line(R, x, y + lines * lineH, line, col, max_w, lineH);
            if (drawn <= 0)
                drawn = 1;
            lines += drawn;
        }
        else
        {
            lines += 1;
        }

        if (!nl)
            break;
        p = nl + 1;
        if (*p == '\0')
        {
            lines += 1;
            break;
        }
    }

    return lines;
}
