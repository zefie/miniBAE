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
 * baescript.c — Public API for BAEScript
 *
 * Provides LoadFile / LoadString / SetSong / Tick / Free.
 ****************************************************************************/

#include "baescript_internal.h"

static void BAEScript_InitEventHandlers(BAEScript_Context *ctx)
{
    int i;

    if (!ctx || ctx->events_initialized || !ctx->program) return;
    if (ctx->program->type != NODE_BLOCK) {
        ctx->events_initialized = 1;
        return;
    }

    memset(ctx->event_handlers, 0, sizeof(ctx->event_handlers));
    for (i = 0; i < ctx->program->data.block.count; i++) {
        BAEScript_Node *stmt = ctx->program->data.block.stmts[i];
        if (!stmt || stmt->type != NODE_EVENT_DECL) continue;
        if (stmt->data.event_decl.event_type >= EVENT_START && stmt->data.event_decl.event_type < EVENT_COUNT)
            ctx->event_handlers[stmt->data.event_decl.event_type] = stmt->data.event_decl.body;
    }
    ctx->events_initialized = 1;
}

static int BAEScript_IsLikelyLoop(uint32_t previous_tick, uint32_t current_tick, uint32_t tick_length)
{
    uint32_t near_end;
    uint32_t near_start;

    if (tick_length == 0) return 0;
    if (current_tick >= previous_tick) return 0;

    near_end = tick_length - (tick_length / 8u);
    near_start = tick_length / 8u;

    return (previous_tick >= near_end && current_tick <= near_start) ? 1 : 0;
}

static void BAEScript_QuerySongState(BAEScript_Context *ctx,
                                     BAE_BOOL *outPaused,
                                     BAE_BOOL *outDone,
                                     uint32_t *outTickPos,
                                     uint32_t *outTickLength)
{
    if (!ctx || !ctx->song) return;
    if (outPaused) BAESong_IsPaused(ctx->song, outPaused);
    if (outDone) BAESong_IsDone(ctx->song, outDone);
    if (outTickPos) BAESong_GetTickPosition(ctx->song, outTickPos);
    if (outTickLength) BAESong_GetTickLength(ctx->song, outTickLength);
}

/* ── Load from file ────────────────────────────────────────────────── */

BAEScript_Context *BAEScript_LoadFile(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "BAEScript: cannot open '%s'\n", path);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 1024 * 1024) {     /* 1 MB cap */
        fprintf(stderr, "BAEScript: file too large or empty '%s'\n", path);
        fclose(f);
        return NULL;
    }

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t read_bytes = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[read_bytes] = '\0';

    BAEScript_Context *ctx = BAEScript_LoadString(buf);
    free(buf);
    return ctx;
}

/* ── Load from string ──────────────────────────────────────────────── */

BAEScript_Context *BAEScript_LoadString(const char *source)
{
    BAEScript_Node *program = BAEScript_Parse(source);
    if (!program) return NULL;

    BAEScript_Context *ctx = (BAEScript_Context *)calloc(1, sizeof(BAEScript_Context));
    if (!ctx) {
        BAEScript_FreeNode(program);
        return NULL;
    }
    ctx->program = program;
    return ctx;
}

/* ── Bind to a song ────────────────────────────────────────────────── */

void BAEScript_SetSong(BAEScript_Context *ctx, BAESong song)
{
    if (!ctx) return;
    if (ctx->song != song) {
        ctx->song = song;
        ctx->events_initialized = 0;
        ctx->script_event_fired = 0;
        ctx->started = 0;
        ctx->was_paused = 0;
        ctx->was_done = 0;
        ctx->has_prev_tick = 0;
        ctx->prev_tick_pos = 0;
        ctx->self_seek_this_tick = 0;
        ctx->last_self_seek = 0;
    }
}

void BAEScript_SetExporting(BAEScript_Context *ctx, int exporting)
{
    if (ctx) ctx->exporting = exporting;
}

/* ── Execute one tick ──────────────────────────────────────────────── */

void BAEScript_Tick(BAEScript_Context *ctx,
                    uint32_t timestamp_ms,
                    uint32_t length_ms)
{
    BAE_BOOL paused = FALSE;
    BAE_BOOL done = FALSE;
    uint32_t tick_pos = 0;
    uint32_t tick_length = 0;
    int fire_seek = 0;
    int fire_loop = 0;

    if (!ctx || !ctx->program) return;

    BAEScript_InitEventHandlers(ctx);

    ctx->timestamp_ms = timestamp_ms;
    ctx->length_ms    = length_ms;

    if (!ctx->script_event_fired) {
        BAEScript_DispatchEvent(ctx, EVENT_SCRIPT);
        ctx->script_event_fired = 1;
    }

    ctx->self_seek_this_tick = 0;
    BAEScript_Exec(ctx, ctx->program);

    BAEScript_QuerySongState(ctx, &paused, &done, &tick_pos, &tick_length);

    if (ctx->song && !done && !ctx->started) {
        BAEScript_DispatchEvent(ctx, EVENT_START);
        ctx->started = 1;
    }

    if (!ctx->was_paused && paused) {
        BAEScript_DispatchEvent(ctx, EVENT_PAUSE);
    } else if (ctx->was_paused && !paused) {
        BAEScript_DispatchEvent(ctx, EVENT_RESUME);
    }

    if (!ctx->was_done && done) {
        BAEScript_DispatchEvent(ctx, EVENT_STOP);
        ctx->started = 0;
    }

    if (ctx->has_prev_tick && !done) {
        if (ctx->self_seek_this_tick) {
            fire_seek = 1;
        } else if (tick_pos < ctx->prev_tick_pos) {
            if (BAEScript_IsLikelyLoop(ctx->prev_tick_pos, tick_pos, tick_length))
                fire_loop = 1;
            else
                fire_seek = 1;
        } else if (tick_pos > ctx->prev_tick_pos) {
            uint32_t delta = tick_pos - ctx->prev_tick_pos;
            if (delta > 10000u)
                fire_seek = 1;
        }
    }

    if (fire_loop)
        BAEScript_DispatchEvent(ctx, EVENT_LOOP);
    if (fire_seek)
        BAEScript_DispatchEvent(ctx, EVENT_SEEK);

    ctx->last_self_seek = ctx->self_seek_this_tick;
    ctx->prev_tick_pos = tick_pos;
    ctx->has_prev_tick = 1;
    ctx->was_paused = paused ? 1 : 0;
    ctx->was_done = done ? 1 : 0;
}

/* ── Cleanup ───────────────────────────────────────────────────────── */

void BAEScript_Free(BAEScript_Context *ctx)
{
    if (!ctx) return;
    BAEScript_FreeNode(ctx->program);
    free(ctx);
}
