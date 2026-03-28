/****************************************************************************
 * baescript_lexer.c — Tokenizer for BAEScript
 ****************************************************************************/

#include "baescript_internal.h"

/* ── helpers ────────────────────────────────────────────────────────── */

static int lex_eof(BAEScript_Lexer *lex)
{
    return lex->pos >= lex->len;
}

static char lex_peek(BAEScript_Lexer *lex)
{
    if (lex_eof(lex)) return '\0';
    return lex->src[lex->pos];
}

static char lex_advance(BAEScript_Lexer *lex)
{
    char c = lex->src[lex->pos++];
    if (c == '\n') { lex->line++; lex->col = 1; }
    else           { lex->col++; }
    return c;
}

static void lex_skip_ws(BAEScript_Lexer *lex)
{
    while (!lex_eof(lex)) {
        char c = lex_peek(lex);
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            lex_advance(lex);
        }
        /* // line comment */
        else if (c == '/' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '/') {
            lex_advance(lex); lex_advance(lex);
            while (!lex_eof(lex) && lex_peek(lex) != '\n') lex_advance(lex);
        }
        /* block comment */
        else if (c == '/' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '*') {
            lex_advance(lex); lex_advance(lex);
            while (!lex_eof(lex)) {
                if (lex_peek(lex) == '*' && lex->pos + 1 < lex->len && lex->src[lex->pos + 1] == '/') {
                    lex_advance(lex); lex_advance(lex);
                    break;
                }
                lex_advance(lex);
            }
        }
        else break;
    }
}

static BAEScript_Token make_tok(BAEScript_TokenType type, int line, int col)
{
    BAEScript_Token t;
    memset(&t, 0, sizeof(t));
    t.type = type;
    t.line = line;
    t.col  = col;
    return t;
}

/* ── public API ─────────────────────────────────────────────────────── */

void BAEScript_Lexer_Init(BAEScript_Lexer *lex, const char *source)
{
    lex->src  = source;
    lex->pos  = 0;
    lex->len  = (int)strlen(source);
    lex->line = 1;
    lex->col  = 1;
}

BAEScript_Token BAEScript_Lexer_Next(BAEScript_Lexer *lex)
{
    lex_skip_ws(lex);

    if (lex_eof(lex)) return make_tok(TOK_EOF, lex->line, lex->col);

    int line = lex->line;
    int col  = lex->col;
    char c   = lex_peek(lex);

    /* ── number literal ──────────────────────────────────────────── */
    if (isdigit((unsigned char)c)) {
        BAEScript_Token t = make_tok(TOK_NUMBER, line, col);
        int32_t val = 0;
        /* hex literal 0x... */
        if (c == '0' && lex->pos + 1 < lex->len &&
            (lex->src[lex->pos + 1] == 'x' || lex->src[lex->pos + 1] == 'X')) {
            lex_advance(lex); lex_advance(lex); /* skip 0x */
            while (!lex_eof(lex) && isxdigit((unsigned char)lex_peek(lex))) {
                char d = lex_advance(lex);
                if (d >= '0' && d <= '9')      val = val * 16 + (d - '0');
                else if (d >= 'a' && d <= 'f') val = val * 16 + (d - 'a' + 10);
                else if (d >= 'A' && d <= 'F') val = val * 16 + (d - 'A' + 10);
            }
        } else {
            while (!lex_eof(lex) && isdigit((unsigned char)lex_peek(lex))) {
                val = val * 10 + (lex_advance(lex) - '0');
            }
        }
        t.value.num = val;
        return t;
    }

    /* ── string literal ──────────────────────────────────────────── */
    if (c == '"' || c == '\'') {
        char quote = lex_advance(lex);
        BAEScript_Token t = make_tok(TOK_STRING, line, col);
        int i = 0;
        while (!lex_eof(lex) && lex_peek(lex) != quote && i < 254) {
            char ch = lex_advance(lex);
            if (ch == '\\' && !lex_eof(lex)) {
                char esc = lex_advance(lex);
                switch (esc) {
                    case 'n':  t.value.str[i++] = '\n'; break;
                    case 't':  t.value.str[i++] = '\t'; break;
                    case '\\': t.value.str[i++] = '\\'; break;
                    case '"':  t.value.str[i++] = '"';  break;
                    case '\'': t.value.str[i++] = '\''; break;
                    default:   t.value.str[i++] = esc;  break;
                }
            } else {
                t.value.str[i++] = ch;
            }
        }
        t.value.str[i] = '\0';
        if (!lex_eof(lex)) lex_advance(lex); /* closing quote */
        return t;
    }

    /* ── identifier / keyword ────────────────────────────────────── */
    if (isalpha((unsigned char)c) || c == '_') {
        char buf[256];
        int i = 0;
        while (!lex_eof(lex) && (isalnum((unsigned char)lex_peek(lex)) || lex_peek(lex) == '_') && i < 254) {
            buf[i++] = lex_advance(lex);
        }
        buf[i] = '\0';

        /* check keywords */
        if (strcmp(buf, "var")   == 0) return make_tok(TOK_VAR,   line, col);
        if (strcmp(buf, "if")    == 0) return make_tok(TOK_IF,    line, col);
        if (strcmp(buf, "else")  == 0) return make_tok(TOK_ELSE,  line, col);
        if (strcmp(buf, "while") == 0) return make_tok(TOK_WHILE, line, col);
        if (strcmp(buf, "for")   == 0) return make_tok(TOK_FOR,   line, col);
        if (strcmp(buf, "on")    == 0) return make_tok(TOK_ON,    line, col);
        if (strcmp(buf, "true")  == 0) return make_tok(TOK_TRUE,  line, col);
        if (strcmp(buf, "false") == 0) return make_tok(TOK_FALSE, line, col);
        if (strcmp(buf, "ch")    == 0) return make_tok(TOK_CH,    line, col);
        if (strcmp(buf, "midi")  == 0) return make_tok(TOK_MIDI,  line, col);
        if (strcmp(buf, "mixer") == 0) return make_tok(TOK_MIXER, line, col);

        BAEScript_Token t = make_tok(TOK_IDENT, line, col);
        strncpy(t.value.str, buf, sizeof(t.value.str) - 1);
        t.value.str[sizeof(t.value.str) - 1] = '\0';
        return t;
    }

    /* ── operators & punctuation ─────────────────────────────────── */
    lex_advance(lex);
    switch (c) {
        case '+':
            if (!lex_eof(lex) && lex_peek(lex) == '+') {
                lex_advance(lex);
                return make_tok(TOK_INC, line, col);
            }
            return make_tok(TOK_PLUS, line, col);
        case '-':
            if (!lex_eof(lex) && lex_peek(lex) == '-') {
                lex_advance(lex);
                return make_tok(TOK_DEC, line, col);
            }
            return make_tok(TOK_MINUS, line, col);
        case '*': return make_tok(TOK_STAR,     line, col);
        case '/': return make_tok(TOK_SLASH,    line, col);
        case '%': return make_tok(TOK_PERCENT,  line, col);
        case '(': return make_tok(TOK_LPAREN,   line, col);
        case ')': return make_tok(TOK_RPAREN,   line, col);
        case '{': return make_tok(TOK_LBRACE,   line, col);
        case '}': return make_tok(TOK_RBRACE,   line, col);
        case '[': return make_tok(TOK_LBRACKET, line, col);
        case ']': return make_tok(TOK_RBRACKET, line, col);
        case '.': return make_tok(TOK_DOT,      line, col);
        case ',': return make_tok(TOK_COMMA,    line, col);
        case ';': return make_tok(TOK_SEMICOLON,line, col);

        case '=':
            if (!lex_eof(lex) && lex_peek(lex) == '=') {
                lex_advance(lex);
                return make_tok(TOK_EQ, line, col);
            }
            return make_tok(TOK_ASSIGN, line, col);

        case '!':
            if (!lex_eof(lex) && lex_peek(lex) == '=') {
                lex_advance(lex);
                return make_tok(TOK_NEQ, line, col);
            }
            return make_tok(TOK_NOT, line, col);

        case '<':
            if (!lex_eof(lex) && lex_peek(lex) == '=') {
                lex_advance(lex);
                return make_tok(TOK_LTE, line, col);
            }
            return make_tok(TOK_LT, line, col);

        case '>':
            if (!lex_eof(lex) && lex_peek(lex) == '=') {
                lex_advance(lex);
                return make_tok(TOK_GTE, line, col);
            }
            return make_tok(TOK_GT, line, col);

        case '&':
            if (!lex_eof(lex) && lex_peek(lex) == '&') {
                lex_advance(lex);
                return make_tok(TOK_AND, line, col);
            }
            /* single & not supported */
            break;

        case '|':
            if (!lex_eof(lex) && lex_peek(lex) == '|') {
                lex_advance(lex);
                return make_tok(TOK_OR, line, col);
            }
            break;
    }

    /* unknown character */
    BAEScript_Token errtok = make_tok(TOK_ERROR, line, col);
    snprintf(errtok.value.str, sizeof(errtok.value.str), "Unexpected character '%c'", c);
    return errtok;
}

BAEScript_Token BAEScript_Lexer_Peek(BAEScript_Lexer *lex)
{
    /* Save state, get next token, restore state */
    BAEScript_Lexer saved = *lex;
    BAEScript_Token tok = BAEScript_Lexer_Next(lex);
    *lex = saved;
    return tok;
}
