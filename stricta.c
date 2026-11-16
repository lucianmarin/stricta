#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libgen.h>

/* =========================================================================
 * 1. ARENA ALLOCATOR, STRING BUILDER & DIAGNOSTICS
 * ========================================================================= */

typedef struct ArenaBlock {
    struct ArenaBlock* next;
    size_t used;
    size_t cap;
    char data[];
} ArenaBlock;

typedef struct {
    ArenaBlock* head;
} Arena;

static Arena g_arena = {NULL};

static void* arena_alloc(size_t size) {
    size = (size + 7u) & ~7u;
    if (size == 0) size = 8;
    if (!g_arena.head || g_arena.head->used + size > g_arena.head->cap) {
        size_t cap = size > 65536 ? size + 4096 : 65536;
        ArenaBlock* blk = (ArenaBlock*)malloc(sizeof(ArenaBlock) + cap);
        if (!blk) { fprintf(stderr, "fatal: out of memory\n"); exit(1); }
        blk->used = 0;
        blk->cap = cap;
        blk->next = g_arena.head;
        g_arena.head = blk;
    }
    void* p = g_arena.head->data + g_arena.head->used;
    g_arena.head->used += size;
    memset(p, 0, size);
    return p;
}

static char* arena_strndup(const char* s, size_t len) {
    char* p = (char*)arena_alloc(len + 1);
    if (len > 0 && s) memcpy(p, s, len);
    p[len] = '\0';
    return p;
}

static char* arena_strdup(const char* s) {
    return s ? arena_strndup(s, strlen(s)) : arena_strndup("", 0);
}

static char* arena_sprintf(const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    char* p = (char*)arena_alloc((size_t)n + 1);
    vsnprintf(p, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    return p;
}

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf* sb) {
    sb->cap = 4096;
    sb->len = 0;
    sb->data = (char*)malloc(sb->cap);
    sb->data[0] = '\0';
}

static void sb_ensure(StrBuf* sb, size_t add) {
    if (sb->len + add + 1 <= sb->cap) return;
    while (sb->len + add + 1 > sb->cap) sb->cap *= 2;
    sb->data = (char*)realloc(sb->data, sb->cap);
}

static void sb_append_n(StrBuf* sb, const char* s, size_t n) {
    sb_ensure(sb, n);
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

static void sb_append(StrBuf* sb, const char* s) {
    if (s) sb_append_n(sb, s, strlen(s));
}

static void sb_printf(StrBuf* sb, const char* fmt, ...) {
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_ensure(sb, (size_t)n);
        vsnprintf(sb->data + sb->len, (size_t)n + 1, fmt, ap2);
        sb->len += (size_t)n;
    }
    va_end(ap2);
}

typedef struct {
    const char* file;
    int line;
    int col;
} SrcLoc;

static void error_at(SrcLoc loc, const char* fmt, ...) {
    fprintf(stderr, "%s:%d:%d: error: ", loc.file ? loc.file : "<input>", loc.line, loc.col);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fprintf(stderr, "\n");
    exit(1);
}

static char* read_entire_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) sz = 0;
    char* buf = (char*)malloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    if (out_len) *out_len = n;
    return buf;
}

/* =========================================================================
 * 2. LEXER
 * ========================================================================= */

typedef enum {
    TOK_EOF = 0,
    TOK_NEWLINE,
    TOK_INDENT,
    TOK_DEDENT,
    TOK_NAME,
    TOK_INT,
    TOK_FLOAT,
    TOK_STRING,
    TOK_BYTES,
    TOK_FSTRING,
    /* Keywords */
    TOK_DEF, TOK_CLASS, TOK_IF, TOK_ELIF, TOK_ELSE, TOK_WHILE, TOK_FOR,
    TOK_IN, TOK_NOT, TOK_AND, TOK_OR, TOK_IS, TOK_RETURN, TOK_PASS,
    TOK_BREAK, TOK_CONTINUE, TOK_RAISE, TOK_ASSERT, TOK_DEL, TOK_GLOBAL,
    TOK_NONLOCAL, TOK_TRY, TOK_EXCEPT, TOK_FINALLY, TOK_WITH, TOK_AS,
    TOK_IMPORT, TOK_FROM, TOK_MATCH, TOK_CASE, TOK_YIELD, TOK_LAMBDA,
    TOK_TRUE, TOK_FALSE, TOK_NONE, TOK_ASYNC, TOK_AWAIT,
    /* Operators & Punctuation */
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_DSLASH, TOK_PERCENT, TOK_DSTAR,
    TOK_EQ, TOK_NE, TOK_LT, TOK_LE, TOK_GT, TOK_GE,
    TOK_ASSIGN, TOK_PLUSEQ, TOK_MINUSEQ, TOK_STAREQ, TOK_SLASHEQ, TOK_DSLASHEQ,
    TOK_PERCENTEQ, TOK_DSTAREQ, TOK_AMPEQ, TOK_PIPEEQ, TOK_CARETEQ, TOK_LSHIFTEQ,
    TOK_RSHIFTEQ, TOK_ATEQ, TOK_WALRUS, TOK_ARROW,
    TOK_AMP, TOK_PIPE, TOK_CARET, TOK_TILDE, TOK_LSHIFT, TOK_RSHIFT, TOK_AT,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACK, TOK_RBRACK, TOK_LBRACE, TOK_RBRACE,
    TOK_COLON, TOK_COMMA, TOK_DOT, TOK_ELLIPSIS
} TokKind;

typedef struct {
    TokKind kind;
    const char* text;
    size_t len;
    SrcLoc loc;
} Token;

typedef struct {
    Token* items;
    int count;
    int cap;
} TokenList;

static void tl_push(TokenList* tl, TokKind kind, const char* text, size_t len, SrcLoc loc) {
    if (tl->count >= tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 256;
        tl->items = (Token*)realloc(tl->items, sizeof(Token) * (size_t)tl->cap);
    }
    tl->items[tl->count++] = (Token){kind, arena_strndup(text, len), len, loc};
}

static TokKind keyword_kind(const char* s, size_t n) {
    #define KW(str, k) if (n == sizeof(str) - 1 && memcmp(s, str, n) == 0) return k
    KW("def", TOK_DEF); KW("class", TOK_CLASS); KW("if", TOK_IF); KW("elif", TOK_ELIF);
    KW("else", TOK_ELSE); KW("while", TOK_WHILE); KW("for", TOK_FOR); KW("in", TOK_IN);
    KW("not", TOK_NOT); KW("and", TOK_AND); KW("or", TOK_OR); KW("is", TOK_IS);
    KW("return", TOK_RETURN); KW("pass", TOK_PASS); KW("break", TOK_BREAK);
    KW("continue", TOK_CONTINUE); KW("raise", TOK_RAISE); KW("assert", TOK_ASSERT);
    KW("del", TOK_DEL); KW("global", TOK_GLOBAL); KW("nonlocal", TOK_NONLOCAL);
    KW("try", TOK_TRY); KW("except", TOK_EXCEPT); KW("finally", TOK_FINALLY);
    KW("with", TOK_WITH); KW("as", TOK_AS); KW("import", TOK_IMPORT); KW("from", TOK_FROM);
    KW("match", TOK_MATCH); KW("case", TOK_CASE); KW("yield", TOK_YIELD);
    KW("lambda", TOK_LAMBDA); KW("True", TOK_TRUE); KW("False", TOK_FALSE);
    KW("None", TOK_NONE); KW("async", TOK_ASYNC); KW("await", TOK_AWAIT);
    #undef KW
    return TOK_NAME;
}

static const char* tok_name(TokKind k) {
    switch (k) {
        case TOK_EOF: return "EOF";
        case TOK_NEWLINE: return "NEWLINE";
        case TOK_INDENT: return "INDENT";
        case TOK_DEDENT: return "DEDENT";
        case TOK_NAME: return "identifier";
        case TOK_INT: return "int literal";
        case TOK_FLOAT: return "float literal";
        case TOK_STRING: return "string literal";
        case TOK_BYTES: return "bytes literal";
        case TOK_FSTRING: return "f-string";
        case TOK_COLON: return "':'";
        case TOK_LPAREN: return "'('";
        case TOK_RPAREN: return "')'";
        case TOK_LBRACK: return "'['";
        case TOK_RBRACK: return "']'";
        case TOK_LBRACE: return "'{'";
        case TOK_RBRACE: return "'}'";
        case TOK_COMMA: return "','";
        case TOK_ARROW: return "'->'";
        case TOK_ASSIGN: return "'='";
        default: return "token";
    }
}

static TokenList lex_source(const char* file, const char* src, size_t len) {
    TokenList tl = {NULL, 0, 0};
    int indent_stack[256];
    int indent_top = 0;
    indent_stack[0] = 0;

    int paren_depth = 0;
    size_t p = 0;
    int line = 1;
    size_t line_start = 0;

    while (p < len) {
        /* At start of a physical line (if not inside (), [], {}) */
        if (paren_depth == 0) {
            size_t scan = p;
            int spaces = 0;
            while (scan < len && (src[scan] == ' ' || src[scan] == '\t')) {
                spaces += (src[scan] == '\t') ? 4 : 1;
                scan++;
            }
            if (scan < len && src[scan] == '#') {
                while (scan < len && src[scan] != '\n' && src[scan] != '\r') scan++;
            }
            if (scan >= len || src[scan] == '\n' || src[scan] == '\r') {
                if (scan < len && src[scan] == '\r') scan++;
                if (scan < len && src[scan] == '\n') scan++;
                p = scan;
                line++;
                line_start = p;
                continue;
            }
            int cur = indent_stack[indent_top];
            if (spaces > cur) {
                if (indent_top >= 255) error_at((SrcLoc){file, line, 1}, "indentation too deep");
                indent_stack[++indent_top] = spaces;
                tl_push(&tl, TOK_INDENT, "", 0, (SrcLoc){file, line, 1});
            } else if (spaces < cur) {
                while (indent_top > 0 && indent_stack[indent_top] > spaces) {
                    indent_top--;
                    tl_push(&tl, TOK_DEDENT, "", 0, (SrcLoc){file, line, 1});
                }
                if (indent_stack[indent_top] != spaces) {
                    error_at((SrcLoc){file, line, 1}, "inconsistent dedent level");
                }
            }
            p = scan;
        }

        /* Tokenize line contents */
        while (p < len) {
            char c = src[p];
            if (c == ' ' || c == '\t') { p++; continue; }
            if (c == '\\' && p + 1 < len && (src[p + 1] == '\n' || src[p + 1] == '\r')) {
                p++;
                if (src[p] == '\r') p++;
                if (src[p] == '\n') p++;
                line++;
                line_start = p;
                continue;
            }
            if (c == '#') {
                while (p < len && src[p] != '\n' && src[p] != '\r') p++;
                continue;
            }
            if (c == '\r' || c == '\n') {
                if (c == '\r' && p + 1 < len && src[p + 1] == '\n') p += 2;
                else p += 1;
                if (paren_depth == 0) {
                    tl_push(&tl, TOK_NEWLINE, "\n", 1, (SrcLoc){file, line, (int)(p - line_start)});
                    line++;
                    line_start = p;
                    break;
                }
                line++;
                line_start = p;
                continue;
            }

            SrcLoc loc = {file, line, (int)(p - line_start + 1)};

            /* String / bytes / f-string / raw string prefixes */
            bool is_fstr = false, is_bytes = false, is_raw = false;
            size_t str_start = p;
            if ((c == 'f' || c == 'F' || c == 'b' || c == 'B' || c == 'r' || c == 'R') &&
                p + 1 < len && (src[p + 1] == '"' || src[p + 1] == '\'')) {
                if (c == 'f' || c == 'F') is_fstr = true;
                else if (c == 'b' || c == 'B') is_bytes = true;
                else is_raw = true;
                str_start = p + 1;
            } else if ((c == 'r' || c == 'R' || c == 'f' || c == 'F') &&
                       p + 2 < len && (src[p + 1] == 'f' || src[p + 1] == 'F' || src[p + 1] == 'r' || src[p + 1] == 'R') &&
                       (src[p + 2] == '"' || src[p + 2] == '\'')) {
                is_fstr = (c == 'f' || c == 'F' || src[p + 1] == 'f' || src[p + 1] == 'F');
                is_raw = (c == 'r' || c == 'R' || src[p + 1] == 'r' || src[p + 1] == 'R');
                str_start = p + 2;
            }

            if (src[str_start] == '"' || src[str_start] == '\'') {
                char q = src[str_start];
                bool triple = (str_start + 2 < len && src[str_start + 1] == q && src[str_start + 2] == q);
                p = str_start + (triple ? 3 : 1);
                StrBuf sb;
                sb_init(&sb);
                while (p < len) {
                    if (triple) {
                        if (p + 2 < len && src[p] == q && src[p + 1] == q && src[p + 2] == q) {
                            p += 3;
                            break;
                        }
                    } else {
                        if (src[p] == q) { p++; break; }
                        if (src[p] == '\n' || src[p] == '\r') {
                            error_at(loc, "unterminated string literal");
                        }
                    }
                    if (!is_raw && src[p] == '\\' && p + 1 < len) {
                        p++;
                        char ec = src[p++];
                        if (ec == 'n') sb_append_n(&sb, "\n", 1);
                        else if (ec == 't') sb_append_n(&sb, "\t", 1);
                        else if (ec == 'r') sb_append_n(&sb, "\r", 1);
                        else if (ec == '\\') sb_append_n(&sb, "\\", 1);
                        else if (ec == '"') sb_append_n(&sb, "\"", 1);
                        else if (ec == '\'') sb_append_n(&sb, "'", 1);
                        else if (ec == '0') { char z = 0; sb_append_n(&sb, &z, 1); }
                        else if (ec == 'x' && p + 1 < len) {
                            char hex[3] = {src[p], src[p + 1], 0};
                            char ch = (char)strtol(hex, NULL, 16);
                            sb_append_n(&sb, &ch, 1);
                            p += 2;
                        } else {
                            sb_append_n(&sb, &ec, 1);
                        }
                    } else {
                        if (src[p] == '\n') { line++; line_start = p + 1; }
                        sb_append_n(&sb, &src[p], 1);
                        p++;
                    }
                }
                TokKind tk = is_fstr ? TOK_FSTRING : (is_bytes ? TOK_BYTES : TOK_STRING);
                tl_push(&tl, tk, sb.data, sb.len, loc);
                free(sb.data);
                continue;
            }

            /* Identifiers / Keywords */
            if (isalpha((unsigned char)c) || c == '_') {
                size_t start = p;
                while (p < len && (isalnum((unsigned char)src[p]) || src[p] == '_')) p++;
                TokKind kk = keyword_kind(src + start, p - start);
                tl_push(&tl, kk, src + start, p - start, loc);
                continue;
            }

            /* Numbers */
            if (isdigit((unsigned char)c) || (c == '.' && p + 1 < len && isdigit((unsigned char)src[p + 1]))) {
                size_t start = p;
                bool is_float = (c == '.');
                StrBuf nb;
                sb_init(&nb);
                if (c == '0' && p + 1 < len && (src[p + 1] == 'x' || src[p + 1] == 'X' ||
                    src[p + 1] == 'b' || src[p + 1] == 'B' || src[p + 1] == 'o' || src[p + 1] == 'O')) {
                    sb_append_n(&nb, src + p, 2);
                    p += 2;
                    while (p < len && (isxdigit((unsigned char)src[p]) || src[p] == '_')) {
                        if (src[p] != '_') sb_append_n(&nb, &src[p], 1);
                        p++;
                    }
                } else {
                    while (p < len) {
                        char ch = src[p];
                        if (isdigit((unsigned char)ch)) { sb_append_n(&nb, &ch, 1); p++; }
                        else if (ch == '_') { p++; }
                        else if (ch == '.' && !is_float && !(p + 1 < len && src[p + 1] == '.')) {
                            is_float = true;
                            sb_append_n(&nb, &ch, 1);
                            p++;
                        } else if ((ch == 'e' || ch == 'E')) {
                            is_float = true;
                            sb_append_n(&nb, &ch, 1);
                            p++;
                            if (p < len && (src[p] == '+' || src[p] == '-')) {
                                sb_append_n(&nb, &src[p], 1);
                                p++;
                            }
                        } else break;
                    }
                }
                if (nb.len == 0) {
                    error_at(loc, "invalid numeric literal near '%c'", src[start]);
                }
                tl_push(&tl, is_float ? TOK_FLOAT : TOK_INT, nb.data, nb.len, loc);
                free(nb.data);
                continue;
            }

            /* 3-char operators */
            if (p + 2 < len) {
                if (memcmp(src + p, "//=", 3) == 0) { tl_push(&tl, TOK_DSLASHEQ, "//=", 3, loc); p += 3; continue; }
                if (memcmp(src + p, "**=", 3) == 0) { tl_push(&tl, TOK_DSTAREQ, "**=", 3, loc); p += 3; continue; }
                if (memcmp(src + p, "<<=", 3) == 0) { tl_push(&tl, TOK_LSHIFTEQ, "<<=", 3, loc); p += 3; continue; }
                if (memcmp(src + p, ">>=", 3) == 0) { tl_push(&tl, TOK_RSHIFTEQ, ">>=", 3, loc); p += 3; continue; }
                if (memcmp(src + p, "...", 3) == 0) { tl_push(&tl, TOK_ELLIPSIS, "...", 3, loc); p += 3; continue; }
            }

            /* 2-char operators */
            if (p + 1 < len) {
                #define OP2(s, k) if (src[p] == s[0] && src[p + 1] == s[1]) { tl_push(&tl, k, s, 2, loc); p += 2; continue; }
                OP2("==", TOK_EQ); OP2("!=", TOK_NE); OP2("<=", TOK_LE); OP2(">=", TOK_GE);
                OP2("+=", TOK_PLUSEQ); OP2("-=", TOK_MINUSEQ); OP2("*=", TOK_STAREQ); OP2("/=", TOK_SLASHEQ);
                OP2("%=", TOK_PERCENTEQ); OP2("&=", TOK_AMPEQ); OP2("|=", TOK_PIPEEQ); OP2("^=", TOK_CARETEQ);
                OP2("@=", TOK_ATEQ); OP2("**", TOK_DSTAR); OP2("//", TOK_DSLASH);
                OP2("<<", TOK_LSHIFT); OP2(">>", TOK_RSHIFT); OP2(":=", TOK_WALRUS); OP2("->", TOK_ARROW);
                #undef OP2
            }

            /* 1-char operators */
            TokKind ok = TOK_EOF;
            switch (c) {
                case '+': ok = TOK_PLUS; break;
                case '-': ok = TOK_MINUS; break;
                case '*': ok = TOK_STAR; break;
                case '/': ok = TOK_SLASH; break;
                case '%': ok = TOK_PERCENT; break;
                case '=': ok = TOK_ASSIGN; break;
                case '<': ok = TOK_LT; break;
                case '>': ok = TOK_GT; break;
                case '(': ok = TOK_LPAREN; paren_depth++; break;
                case ')': ok = TOK_RPAREN; if (paren_depth > 0) paren_depth--; break;
                case '[': ok = TOK_LBRACK; paren_depth++; break;
                case ']': ok = TOK_RBRACK; if (paren_depth > 0) paren_depth--; break;
                case '{': ok = TOK_LBRACE; paren_depth++; break;
                case '}': ok = TOK_RBRACE; if (paren_depth > 0) paren_depth--; break;
                case ':': ok = TOK_COLON; break;
                case ',': ok = TOK_COMMA; break;
                case '.': ok = TOK_DOT; break;
                case '&': ok = TOK_AMP; break;
                case '|': ok = TOK_PIPE; break;
                case '^': ok = TOK_CARET; break;
                case '~': ok = TOK_TILDE; break;
                case '@': ok = TOK_AT; break;
                default: error_at(loc, "unexpected character '%c'", c);
            }
            tl_push(&tl, ok, &c, 1, loc);
            p++;
        }
    }

    if (tl.count > 0 && tl.items[tl.count - 1].kind != TOK_NEWLINE && tl.items[tl.count - 1].kind != TOK_DEDENT) {
        tl_push(&tl, TOK_NEWLINE, "\n", 1, (SrcLoc){file, line, 1});
    }
    while (indent_top > 0) {
        indent_top--;
        tl_push(&tl, TOK_DEDENT, "", 0, (SrcLoc){file, line, 1});
    }
    tl_push(&tl, TOK_EOF, "", 0, (SrcLoc){file, line, 1});
    return tl;
}

/* =========================================================================
 * 3. AST & PARSER
 * ========================================================================= */

typedef struct TypeExpr TypeExpr;
typedef struct Expr Expr;
typedef struct Stmt Stmt;
typedef struct Type Type;

typedef enum {
    TEXPR_NAME,
    TEXPR_GENERIC,
    TEXPR_UNION,
    TEXPR_CALLABLE,
    TEXPR_TUPLE
} TypeExprKind;

struct TypeExpr {
    TypeExprKind kind;
    SrcLoc loc;
    const char* name;
    TypeExpr** args;
    int arg_count;
    TypeExpr* ret;
};

typedef enum {
    EXPR_INT, EXPR_FLOAT, EXPR_BOOL, EXPR_NONE, EXPR_STR, EXPR_BYTES, EXPR_FSTRING,
    EXPR_VAR, EXPR_BINOP, EXPR_UNOP, EXPR_CALL, EXPR_ATTR, EXPR_INDEX, EXPR_SLICE,
    EXPR_LIST, EXPR_TUPLE, EXPR_SET, EXPR_DICT,
    EXPR_LISTCOMP, EXPR_SETCOMP, EXPR_DICTCOMP, EXPR_GENEXPR,
    EXPR_TERNARY, EXPR_WALRUS, EXPR_LAMBDA
} ExprKind;

typedef struct CompGen {
    const char** targets;
    int target_count;
    Expr* iter;
    Expr** ifs;
    int if_count;
} CompGen;

typedef struct FStrPart {
    bool is_expr;
    const char* text;
    Expr* expr;
    const char* fmt_spec;
} FStrPart;

struct Expr {
    ExprKind kind;
    SrcLoc loc;
    Type* ty; /* resolved by sema */
    union {
        struct { const char* raw; int64_t ival; uint64_t uval; bool is_big; } int_lit;
        struct { double fval; } float_lit;
        struct { bool bval; } bool_lit;
        struct { const char* text; size_t len; } str_lit;
        struct { FStrPart* parts; int count; } fstr;
        struct { const char* name; } var;
        struct { const char* op; Expr* left; Expr* right; } binop;
        struct { const char* op; Expr* operand; } unop;
        struct { Expr* callee; Expr** args; const char** kw_names; int arg_count; } call;
        struct { Expr* obj; const char* attr; } attr;
        struct { Expr* obj; Expr* index; } index;
        struct { Expr* obj; Expr* start; Expr* stop; } slice;
        struct { Expr** items; int count; } seq;
        struct { Expr** keys; Expr** vals; int count; } dict;
        struct { Expr* elt; Expr* val; CompGen* gens; int gen_count; } comp;
        struct { Expr* cond; Expr* then_e; Expr* else_e; } ternary;
        struct { const char* name; Expr* val; } walrus;
        struct {
            const char** params; TypeExpr** param_types; int param_count;
            TypeExpr* ret_type; Expr* body;
        } lambda;
    };
};

typedef enum {
    STMT_PASS, STMT_BREAK, STMT_CONTINUE, STMT_RETURN, STMT_EXPR,
    STMT_ASSIGN, STMT_MULTI_ASSIGN, STMT_AUG_ASSIGN,
    STMT_IF, STMT_WHILE, STMT_FOR, STMT_DEF, STMT_CLASS,
    STMT_IMPORT, STMT_FROM_IMPORT, STMT_TRY, STMT_RAISE, STMT_ASSERT,
    STMT_WITH, STMT_DEL, STMT_GLOBAL, STMT_NONLOCAL, STMT_MATCH,
    STMT_YIELD, STMT_YIELD_FROM
} StmtKind;

typedef struct Param {
    const char* name;
    TypeExpr* type_ann;
    Expr* default_val;
    SrcLoc loc;
} Param;

typedef struct ExceptHandler {
    const char* exc_type;
    const char* as_name;
    Stmt** body;
    int body_count;
} ExceptHandler;

typedef struct MatchCase {
    Expr* pattern; /* NULL for wildcard '_' */
    const char* capture; /* non-NULL for capture var */
    Stmt** body;
    int body_count;
} MatchCase;

struct Stmt {
    StmtKind kind;
    SrcLoc loc;
    union {
        struct { Expr* expr; } ret;
        struct { Expr* expr; } expr_s;
        struct { Expr* target; TypeExpr* type_ann; Expr* value; } assign;
        struct { Expr** targets; int target_count; Expr* value; } multi_assign;
        struct { Expr* target; const char* op; Expr* value; } aug_assign;
        struct {
            Expr** conds; Stmt*** bodies; int* body_counts; int branch_count;
            Stmt** else_body; int else_count;
        } if_s;
        struct { Expr* cond; Stmt** body; int body_count; Stmt** else_body; int else_count; } while_s;
        struct {
            const char** targets; int target_count; Expr* iter;
            Stmt** body; int body_count; Stmt** else_body; int else_count;
        } for_s;
        struct {
            const char* name; Param* params; int param_count;
            TypeExpr* ret_type; Stmt** body; int body_count;
            Expr** decorators; int deco_count; bool is_async;
        } def_s;
        struct {
            const char* name; const char** bases; int base_count;
            Stmt** body; int body_count; Expr** decorators; int deco_count;
        } class_s;
        struct { const char* mod_name; const char* alias; } import_s;
        struct { const char* mod_name; const char** names; const char** aliases; int count; } from_import_s;
        struct {
            Stmt** try_body; int try_count;
            ExceptHandler* handlers; int handler_count;
            Stmt** else_body; int else_count;
            Stmt** finally_body; int finally_count;
        } try_s;
        struct { Expr* exc; } raise_s;
        struct { Expr* cond; Expr* msg; } assert_s;
        struct { Expr* ctx; const char* as_name; Stmt** body; int body_count; } with_s;
        struct { Expr* target; } del_s;
        struct { const char** names; int count; } scope_s;
        struct { Expr* subject; MatchCase* cases; int case_count; } match_s;
    };
};

typedef struct {
    Token* toks;
    int count;
    int pos;
    const char* file;
} Parser;

static Token* p_peek(Parser* ps) { return &ps->toks[ps->pos < ps->count ? ps->pos : ps->count - 1]; }
static Token* p_peek_at(Parser* ps, int off) {
    int idx = ps->pos + off;
    if (idx < 0) idx = 0;
    if (idx >= ps->count) idx = ps->count - 1;
    return &ps->toks[idx];
}
static bool p_check(Parser* ps, TokKind k) { return p_peek(ps)->kind == k; }
static bool p_match(Parser* ps, TokKind k) { if (p_check(ps, k)) { ps->pos++; return true; } return false; }
static Token* p_consume(Parser* ps, TokKind k) {
    Token* t = p_peek(ps);
    if (t->kind != k) error_at(t->loc, "expected %s, got %s ('%s')", tok_name(k), tok_name(t->kind), t->text);
    ps->pos++;
    return t;
}
static void skip_newlines(Parser* ps) { while (p_match(ps, TOK_NEWLINE)) {} }

static Expr* parse_expr(Parser* ps);
static Expr* parse_no_ternary(Parser* ps);
static Expr* parse_factor(Parser* ps);
static Stmt* parse_stmt(Parser* ps);
static Stmt** parse_suite(Parser* ps, int* out_count);
static TypeExpr* parse_type_expr(Parser* ps);

static Expr* new_expr(ExprKind k, SrcLoc loc) {
    Expr* e = (Expr*)arena_alloc(sizeof(Expr));
    e->kind = k;
    e->loc = loc;
    return e;
}

static Stmt* new_stmt(StmtKind k, SrcLoc loc) {
    Stmt* s = (Stmt*)arena_alloc(sizeof(Stmt));
    s->kind = k;
    s->loc = loc;
    return s;
}

static TypeExpr* parse_type_atom(Parser* ps) {
    Token* t = p_peek(ps);
    TypeExpr* te = (TypeExpr*)arena_alloc(sizeof(TypeExpr));
    te->loc = t->loc;
    if (p_match(ps, TOK_NONE)) {
        te->kind = TEXPR_NAME;
        te->name = "None";
        return te;
    }
    if (p_match(ps, TOK_LBRACK)) {
        /* [T1, T2] inside Callable[[T1, T2], R] */
        te->kind = TEXPR_TUPLE;
        TypeExpr** arr = NULL; int cnt = 0, cap = 0;
        if (!p_check(ps, TOK_RBRACK)) {
            do {
                if (cnt >= cap) { cap = cap ? cap * 2 : 4; arr = (TypeExpr**)realloc(arr, sizeof(TypeExpr*) * (size_t)cap); }
                arr[cnt++] = parse_type_expr(ps);
            } while (p_match(ps, TOK_COMMA) && !p_check(ps, TOK_RBRACK));
        }
        p_consume(ps, TOK_RBRACK);
        te->args = (TypeExpr**)arena_alloc(sizeof(TypeExpr*) * (size_t)(cnt ? cnt : 1));
        if (cnt > 0) memcpy(te->args, arr, sizeof(TypeExpr*) * (size_t)cnt);
        free(arr);
        te->arg_count = cnt;
        return te;
    }
    Token* id = p_consume(ps, TOK_NAME);
    const char* name = id->text;
    while (p_match(ps, TOK_DOT)) {
        Token* sub = p_consume(ps, TOK_NAME);
        name = arena_sprintf("%s.%s", name, sub->text);
    }
    if (p_match(ps, TOK_LBRACK)) {
        te->kind = TEXPR_GENERIC;
        te->name = name;
        TypeExpr** arr = NULL; int cnt = 0, cap = 0;
        if (!p_check(ps, TOK_RBRACK)) {
            do {
                if (p_match(ps, TOK_ELLIPSIS)) {
                    TypeExpr* ell = (TypeExpr*)arena_alloc(sizeof(TypeExpr));
                    ell->kind = TEXPR_NAME; ell->name = "..."; ell->loc = p_peek(ps)->loc;
                    if (cnt >= cap) { cap = cap ? cap * 2 : 4; arr = (TypeExpr**)realloc(arr, sizeof(TypeExpr*) * (size_t)cap); }
                    arr[cnt++] = ell;
                } else {
                    if (cnt >= cap) { cap = cap ? cap * 2 : 4; arr = (TypeExpr**)realloc(arr, sizeof(TypeExpr*) * (size_t)cap); }
                    arr[cnt++] = parse_type_expr(ps);
                }
            } while (p_match(ps, TOK_COMMA) && !p_check(ps, TOK_RBRACK));
        }
        p_consume(ps, TOK_RBRACK);
        te->args = (TypeExpr**)arena_alloc(sizeof(TypeExpr*) * (size_t)(cnt ? cnt : 1));
        if (cnt > 0) memcpy(te->args, arr, sizeof(TypeExpr*) * (size_t)cnt);
        free(arr);
        te->arg_count = cnt;
        return te;
    }
    te->kind = TEXPR_NAME;
    te->name = name;
    return te;
}

static TypeExpr* parse_type_expr(Parser* ps) {
    TypeExpr* lhs = parse_type_atom(ps);
    while (p_match(ps, TOK_PIPE)) {
        TypeExpr* rhs = parse_type_atom(ps);
        TypeExpr* u = (TypeExpr*)arena_alloc(sizeof(TypeExpr));
        u->kind = TEXPR_UNION;
        u->loc = lhs->loc;
        u->args = (TypeExpr**)arena_alloc(sizeof(TypeExpr*) * 2);
        u->args[0] = lhs;
        u->args[1] = rhs;
        u->arg_count = 2;
        lhs = u;
    }
    return lhs;
}

static Expr* parse_fstring_expr(const char* raw, size_t raw_len, SrcLoc loc) {
    Expr* e = new_expr(EXPR_FSTRING, loc);
    FStrPart* parts = NULL; int cnt = 0, cap = 0;
    size_t i = 0;
    StrBuf lit;
    sb_init(&lit);
    #define PUSH_PART(is_e, txt, ex, fsp) do { \
        if (cnt >= cap) { cap = cap ? cap * 2 : 8; parts = (FStrPart*)realloc(parts, sizeof(FStrPart) * (size_t)cap); } \
        parts[cnt++] = (FStrPart){is_e, txt, ex, fsp}; \
    } while (0)
    while (i < raw_len) {
        if (raw[i] == '{') {
            if (i + 1 < raw_len && raw[i + 1] == '{') {
                sb_append_n(&lit, "{", 1);
                i += 2;
                continue;
            }
            if (lit.len > 0) {
                PUSH_PART(false, arena_strndup(lit.data, lit.len), NULL, NULL);
                lit.len = 0; lit.data[0] = '\0';
            }
            i++;
            size_t expr_start = i;
            int depth = 1;
            char in_q = 0;
            size_t colon_pos = (size_t)-1;
            while (i < raw_len && depth > 0) {
                char c = raw[i];
                if (in_q) {
                    if (c == '\\' && i + 1 < raw_len) { i += 2; continue; }
                    if (c == in_q) in_q = 0;
                } else {
                    if (c == '"' || c == '\'') in_q = c;
                    else if (c == '{' || c == '[' || c == '(') depth++;
                    else if (c == ']' || c == ')') depth--;
                    else if (c == '}') { depth--; if (depth == 0) break; }
                    else if (c == ':' && depth == 1 && colon_pos == (size_t)-1) colon_pos = i;
                }
                i++;
            }
            if (depth != 0) error_at(loc, "unterminated '{' in f-string");
            size_t expr_end = (colon_pos != (size_t)-1) ? colon_pos : i;
            const char* fmt_spec = NULL;
            if (colon_pos != (size_t)-1) {
                fmt_spec = arena_strndup(raw + colon_pos + 1, i - (colon_pos + 1));
            }
            TokenList sub_tl = lex_source(loc.file, raw + expr_start, expr_end - expr_start);
            Parser sub_ps = {sub_tl.items, sub_tl.count, 0, loc.file};
            Expr* sub_e = parse_expr(&sub_ps);
            free(sub_tl.items);
            PUSH_PART(true, NULL, sub_e, fmt_spec);
            i++; /* skip '}' */
        } else if (raw[i] == '}' && i + 1 < raw_len && raw[i + 1] == '}') {
            sb_append_n(&lit, "}", 1);
            i += 2;
        } else {
            sb_append_n(&lit, &raw[i], 1);
            i++;
        }
    }
    if (lit.len > 0) {
        PUSH_PART(false, arena_strndup(lit.data, lit.len), NULL, NULL);
    }
    #undef PUSH_PART
    free(lit.data);
    e->fstr.parts = (FStrPart*)arena_alloc(sizeof(FStrPart) * (size_t)(cnt ? cnt : 1));
    if (cnt > 0) memcpy(e->fstr.parts, parts, sizeof(FStrPart) * (size_t)cnt);
    free(parts);
    e->fstr.count = cnt;
    return e;
}

static CompGen* parse_comp_gens(Parser* ps, int* out_count) {
    CompGen* gens = NULL; int cnt = 0, cap = 0;
    while (p_match(ps, TOK_FOR)) {
        const char** targs = NULL; int tcnt = 0, tcap = 0;
        do {
            Token* id = p_consume(ps, TOK_NAME);
            if (tcnt >= tcap) { tcap = tcap ? tcap * 2 : 4; targs = (const char**)realloc(targs, sizeof(char*) * (size_t)tcap); }
            targs[tcnt++] = id->text;
        } while (p_match(ps, TOK_COMMA));
        p_consume(ps, TOK_IN);
        Expr* iter = parse_no_ternary(ps);
        Expr** ifs = NULL; int icnt = 0, icap = 0;
        while (p_match(ps, TOK_IF)) {
            Expr* cond = parse_no_ternary(ps);
            if (icnt >= icap) { icap = icap ? icap * 2 : 4; ifs = (Expr**)realloc(ifs, sizeof(Expr*) * (size_t)icap); }
            ifs[icnt++] = cond;
        }
        CompGen g;
        g.targets = (const char**)arena_alloc(sizeof(char*) * (size_t)tcnt);
        memcpy(g.targets, targs, sizeof(char*) * (size_t)tcnt);
        free(targs);
        g.target_count = tcnt;
        g.iter = iter;
        g.ifs = icnt ? (Expr**)arena_alloc(sizeof(Expr*) * (size_t)icnt) : NULL;
        if (icnt) memcpy(g.ifs, ifs, sizeof(Expr*) * (size_t)icnt);
        free(ifs);
        g.if_count = icnt;
        if (cnt >= cap) { cap = cap ? cap * 2 : 4; gens = (CompGen*)realloc(gens, sizeof(CompGen) * (size_t)cap); }
        gens[cnt++] = g;
    }
    CompGen* res = (CompGen*)arena_alloc(sizeof(CompGen) * (size_t)(cnt ? cnt : 1));
    if (cnt > 0) memcpy(res, gens, sizeof(CompGen) * (size_t)cnt);
    free(gens);
    *out_count = cnt;
    return res;
}

static Expr* parse_atom(Parser* ps) {
    Token* t = p_peek(ps);
    if (p_match(ps, TOK_INT)) {
        Expr* e = new_expr(EXPR_INT, t->loc);
        e->int_lit.raw = t->text;
        char* end = NULL;
        if (t->len > 2 && t->text[0] == '0' && (t->text[1] == 'x' || t->text[1] == 'X')) {
            e->int_lit.uval = strtoull(t->text + 2, &end, 16);
            e->int_lit.ival = (int64_t)e->int_lit.uval;
        } else if (t->len > 2 && t->text[0] == '0' && (t->text[1] == 'b' || t->text[1] == 'B')) {
            e->int_lit.uval = strtoull(t->text + 2, &end, 2);
            e->int_lit.ival = (int64_t)e->int_lit.uval;
        } else if (t->len > 2 && t->text[0] == '0' && (t->text[1] == 'o' || t->text[1] == 'O')) {
            e->int_lit.uval = strtoull(t->text + 2, &end, 8);
            e->int_lit.ival = (int64_t)e->int_lit.uval;
        } else {
            if (t->len > 19) {
                e->int_lit.is_big = true;
            } else {
                e->int_lit.uval = strtoull(t->text, &end, 10);
                if (t->len == 19 && strcmp(t->text, "9223372036854775807") > 0) {
                    e->int_lit.is_big = true;
                }
                e->int_lit.ival = (int64_t)e->int_lit.uval;
            }
        }
        return e;
    }
    if (p_match(ps, TOK_FLOAT)) {
        Expr* e = new_expr(EXPR_FLOAT, t->loc);
        e->float_lit.fval = strtod(t->text, NULL);
        return e;
    }
    if (p_match(ps, TOK_TRUE)) { Expr* e = new_expr(EXPR_BOOL, t->loc); e->bool_lit.bval = true; return e; }
    if (p_match(ps, TOK_FALSE)) { Expr* e = new_expr(EXPR_BOOL, t->loc); e->bool_lit.bval = false; return e; }
    if (p_match(ps, TOK_NONE)) { return new_expr(EXPR_NONE, t->loc); }
    if (p_match(ps, TOK_STRING)) {
        /* Handle implicit adjacent string concatenation */
        StrBuf sb; sb_init(&sb);
        sb_append_n(&sb, t->text, t->len);
        while (p_check(ps, TOK_STRING)) {
            Token* nt = p_consume(ps, TOK_STRING);
            sb_append_n(&sb, nt->text, nt->len);
        }
        Expr* e = new_expr(EXPR_STR, t->loc);
        e->str_lit.text = arena_strndup(sb.data, sb.len);
        e->str_lit.len = sb.len;
        free(sb.data);
        return e;
    }
    if (p_match(ps, TOK_BYTES)) {
        Expr* e = new_expr(EXPR_BYTES, t->loc);
        e->str_lit.text = t->text;
        e->str_lit.len = t->len;
        return e;
    }
    if (p_match(ps, TOK_FSTRING)) {
        return parse_fstring_expr(t->text, t->len, t->loc);
    }
    if (p_match(ps, TOK_NAME)) {
        if (p_check(ps, TOK_WALRUS)) {
            p_consume(ps, TOK_WALRUS);
            Expr* val = parse_expr(ps);
            Expr* w = new_expr(EXPR_WALRUS, t->loc);
            w->walrus.name = t->text;
            w->walrus.val = val;
            return w;
        }
        Expr* e = new_expr(EXPR_VAR, t->loc);
        e->var.name = t->text;
        return e;
    }
    if (p_match(ps, TOK_LPAREN)) {
        if (p_match(ps, TOK_RPAREN)) {
            Expr* e = new_expr(EXPR_TUPLE, t->loc);
            e->seq.items = NULL; e->seq.count = 0;
            return e;
        }
        Expr* first = parse_expr(ps);
        if (p_check(ps, TOK_FOR)) {
            int gcnt = 0;
            CompGen* gens = parse_comp_gens(ps, &gcnt);
            p_consume(ps, TOK_RPAREN);
            Expr* e = new_expr(EXPR_GENEXPR, t->loc);
            e->comp.elt = first; e->comp.gens = gens; e->comp.gen_count = gcnt;
            return e;
        }
        if (p_match(ps, TOK_COMMA)) {
            Expr** items = NULL; int cnt = 0, cap = 0;
            items = (Expr**)malloc(sizeof(Expr*) * 8); cap = 8;
            items[cnt++] = first;
            while (!p_check(ps, TOK_RPAREN)) {
                if (cnt >= cap) { cap *= 2; items = (Expr**)realloc(items, sizeof(Expr*) * (size_t)cap); }
                items[cnt++] = parse_expr(ps);
                if (!p_match(ps, TOK_COMMA)) break;
            }
            p_consume(ps, TOK_RPAREN);
            Expr* e = new_expr(EXPR_TUPLE, t->loc);
            e->seq.items = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)cnt);
            memcpy(e->seq.items, items, sizeof(Expr*) * (size_t)cnt);
            free(items);
            e->seq.count = cnt;
            return e;
        }
        p_consume(ps, TOK_RPAREN);
        return first;
    }
    if (p_match(ps, TOK_LBRACK)) {
        if (p_match(ps, TOK_RBRACK)) {
            Expr* e = new_expr(EXPR_LIST, t->loc);
            e->seq.items = NULL; e->seq.count = 0;
            return e;
        }
        Expr* first = parse_expr(ps);
        if (p_check(ps, TOK_FOR)) {
            int gcnt = 0;
            CompGen* gens = parse_comp_gens(ps, &gcnt);
            p_consume(ps, TOK_RBRACK);
            Expr* e = new_expr(EXPR_LISTCOMP, t->loc);
            e->comp.elt = first; e->comp.gens = gens; e->comp.gen_count = gcnt;
            return e;
        }
        Expr** items = (Expr**)malloc(sizeof(Expr*) * 8); int cnt = 0, cap = 8;
        items[cnt++] = first;
        while (p_match(ps, TOK_COMMA) && !p_check(ps, TOK_RBRACK)) {
            if (cnt >= cap) { cap *= 2; items = (Expr**)realloc(items, sizeof(Expr*) * (size_t)cap); }
            items[cnt++] = parse_expr(ps);
        }
        p_consume(ps, TOK_RBRACK);
        Expr* e = new_expr(EXPR_LIST, t->loc);
        e->seq.items = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)cnt);
        memcpy(e->seq.items, items, sizeof(Expr*) * (size_t)cnt);
        free(items);
        e->seq.count = cnt;
        return e;
    }
    if (p_match(ps, TOK_LBRACE)) {
        if (p_match(ps, TOK_RBRACE)) {
            Expr* e = new_expr(EXPR_DICT, t->loc);
            e->dict.keys = NULL; e->dict.vals = NULL; e->dict.count = 0;
            return e;
        }
        Expr* first = parse_expr(ps);
        if (p_match(ps, TOK_COLON)) {
            Expr* first_val = parse_expr(ps);
            if (p_check(ps, TOK_FOR)) {
                int gcnt = 0;
                CompGen* gens = parse_comp_gens(ps, &gcnt);
                p_consume(ps, TOK_RBRACE);
                Expr* e = new_expr(EXPR_DICTCOMP, t->loc);
                e->comp.elt = first; e->comp.val = first_val;
                e->comp.gens = gens; e->comp.gen_count = gcnt;
                return e;
            }
            Expr** keys = (Expr**)malloc(sizeof(Expr*) * 8);
            Expr** vals = (Expr**)malloc(sizeof(Expr*) * 8);
            int cnt = 0, cap = 8;
            keys[cnt] = first; vals[cnt] = first_val; cnt++;
            while (p_match(ps, TOK_COMMA) && !p_check(ps, TOK_RBRACE)) {
                Expr* k = parse_expr(ps);
                p_consume(ps, TOK_COLON);
                Expr* v = parse_expr(ps);
                if (cnt >= cap) {
                    cap *= 2;
                    keys = (Expr**)realloc(keys, sizeof(Expr*) * (size_t)cap);
                    vals = (Expr**)realloc(vals, sizeof(Expr*) * (size_t)cap);
                }
                keys[cnt] = k; vals[cnt] = v; cnt++;
            }
            p_consume(ps, TOK_RBRACE);
            Expr* e = new_expr(EXPR_DICT, t->loc);
            e->dict.keys = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)cnt);
            e->dict.vals = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)cnt);
            memcpy(e->dict.keys, keys, sizeof(Expr*) * (size_t)cnt);
            memcpy(e->dict.vals, vals, sizeof(Expr*) * (size_t)cnt);
            free(keys); free(vals);
            e->dict.count = cnt;
            return e;
        }
        if (p_check(ps, TOK_FOR)) {
            int gcnt = 0;
            CompGen* gens = parse_comp_gens(ps, &gcnt);
            p_consume(ps, TOK_RBRACE);
            Expr* e = new_expr(EXPR_SETCOMP, t->loc);
            e->comp.elt = first; e->comp.gens = gens; e->comp.gen_count = gcnt;
            return e;
        }
        Expr** items = (Expr**)malloc(sizeof(Expr*) * 8); int cnt = 0, cap = 8;
        items[cnt++] = first;
        while (p_match(ps, TOK_COMMA) && !p_check(ps, TOK_RBRACE)) {
            if (cnt >= cap) { cap *= 2; items = (Expr**)realloc(items, sizeof(Expr*) * (size_t)cap); }
            items[cnt++] = parse_expr(ps);
        }
        p_consume(ps, TOK_RBRACE);
        Expr* e = new_expr(EXPR_SET, t->loc);
        e->seq.items = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)cnt);
        memcpy(e->seq.items, items, sizeof(Expr*) * (size_t)cnt);
        free(items);
        e->seq.count = cnt;
        return e;
    }
    error_at(t->loc, "unexpected token %s ('%s') in expression", tok_name(t->kind), t->text);
    return NULL;
}

static Expr* parse_primary(Parser* ps) {
    Expr* e = parse_atom(ps);
    while (true) {
        Token* t = p_peek(ps);
        if (p_match(ps, TOK_DOT)) {
            Token* id = p_consume(ps, TOK_NAME);
            Expr* ae = new_expr(EXPR_ATTR, t->loc);
            ae->attr.obj = e;
            ae->attr.attr = id->text;
            e = ae;
        } else if (p_match(ps, TOK_LPAREN)) {
            Expr** args = NULL; const char** kws = NULL; int cnt = 0, cap = 0;
            if (!p_check(ps, TOK_RPAREN)) {
                do {
                    if (p_check(ps, TOK_RPAREN)) break;
                    const char* kw = NULL;
                    if (p_check(ps, TOK_NAME) && p_peek_at(ps, 1)->kind == TOK_ASSIGN) {
                        kw = p_consume(ps, TOK_NAME)->text;
                        p_consume(ps, TOK_ASSIGN);
                    }
                    Expr* arg = parse_expr(ps);
                    if (cnt == 0 && p_check(ps, TOK_FOR)) {
                        int gcnt = 0;
                        CompGen* gens = parse_comp_gens(ps, &gcnt);
                        Expr* ge = new_expr(EXPR_GENEXPR, arg->loc);
                        ge->comp.elt = arg; ge->comp.gens = gens; ge->comp.gen_count = gcnt;
                        arg = ge;
                    }
                    if (cnt >= cap) {
                        cap = cap ? cap * 2 : 8;
                        args = (Expr**)realloc(args, sizeof(Expr*) * (size_t)cap);
                        kws = (const char**)realloc(kws, sizeof(char*) * (size_t)cap);
                    }
                    args[cnt] = arg; kws[cnt] = kw; cnt++;
                } while (p_match(ps, TOK_COMMA));
            }
            p_consume(ps, TOK_RPAREN);
            Expr* ce = new_expr(EXPR_CALL, t->loc);
            ce->call.callee = e;
            ce->call.args = cnt ? (Expr**)arena_alloc(sizeof(Expr*) * (size_t)cnt) : NULL;
            ce->call.kw_names = cnt ? (const char**)arena_alloc(sizeof(char*) * (size_t)cnt) : NULL;
            if (cnt) {
                memcpy(ce->call.args, args, sizeof(Expr*) * (size_t)cnt);
                memcpy(ce->call.kw_names, kws, sizeof(char*) * (size_t)cnt);
            }
            free(args); free(kws);
            ce->call.arg_count = cnt;
            e = ce;
        } else if (p_match(ps, TOK_LBRACK)) {
            Expr* start = NULL;
            if (!p_check(ps, TOK_COLON)) start = parse_expr(ps);
            if (p_match(ps, TOK_COLON)) {
                Expr* stop = NULL;
                if (!p_check(ps, TOK_RBRACK)) stop = parse_expr(ps);
                p_consume(ps, TOK_RBRACK);
                Expr* se = new_expr(EXPR_SLICE, t->loc);
                se->slice.obj = e; se->slice.start = start; se->slice.stop = stop;
                e = se;
            } else {
                p_consume(ps, TOK_RBRACK);
                Expr* ie = new_expr(EXPR_INDEX, t->loc);
                ie->index.obj = e; ie->index.index = start;
                e = ie;
            }
        } else break;
    }
    return e;
}

static Expr* parse_power(Parser* ps) {
    Expr* e = parse_primary(ps);
    if (p_match(ps, TOK_DSTAR)) {
        Token* t = p_peek_at(ps, -1);
        Expr* r = parse_factor(ps); /* unary binds looser than ** on right */
        Expr* b = new_expr(EXPR_BINOP, t->loc);
        b->binop.op = "**"; b->binop.left = e; b->binop.right = r;
        return b;
    }
    return e;
}

static Expr* parse_factor(Parser* ps) {
    Token* t = p_peek(ps);
    if (p_match(ps, TOK_PLUS) || p_match(ps, TOK_MINUS) || p_match(ps, TOK_TILDE)) {
        Expr* opnd = parse_factor(ps);
        Expr* u = new_expr(EXPR_UNOP, t->loc);
        u->unop.op = t->text; u->unop.operand = opnd;
        return u;
    }
    if (p_match(ps, TOK_AWAIT)) {
        return parse_factor(ps);
    }
    return parse_power(ps);
}

static Expr* parse_term(Parser* ps) {
    Expr* e = parse_factor(ps);
    while (p_check(ps, TOK_STAR) || p_check(ps, TOK_SLASH) || p_check(ps, TOK_DSLASH) ||
           p_check(ps, TOK_PERCENT) || p_check(ps, TOK_AT)) {
        Token* op = p_peek(ps); ps->pos++;
        Expr* r = parse_factor(ps);
        Expr* b = new_expr(EXPR_BINOP, op->loc);
        b->binop.op = op->text; b->binop.left = e; b->binop.right = r;
        e = b;
    }
    return e;
}

static Expr* parse_sum(Parser* ps) {
    Expr* e = parse_term(ps);
    while (p_check(ps, TOK_PLUS) || p_check(ps, TOK_MINUS)) {
        Token* op = p_peek(ps); ps->pos++;
        Expr* r = parse_term(ps);
        Expr* b = new_expr(EXPR_BINOP, op->loc);
        b->binop.op = op->text; b->binop.left = e; b->binop.right = r;
        e = b;
    }
    return e;
}

static Expr* parse_shift(Parser* ps) {
    Expr* e = parse_sum(ps);
    while (p_check(ps, TOK_LSHIFT) || p_check(ps, TOK_RSHIFT)) {
        Token* op = p_peek(ps); ps->pos++;
        Expr* r = parse_sum(ps);
        Expr* b = new_expr(EXPR_BINOP, op->loc);
        b->binop.op = op->text; b->binop.left = e; b->binop.right = r;
        e = b;
    }
    return e;
}

static Expr* parse_bitand(Parser* ps) {
    Expr* e = parse_shift(ps);
    while (p_match(ps, TOK_AMP)) {
        Token* op = p_peek_at(ps, -1);
        Expr* r = parse_shift(ps);
        Expr* b = new_expr(EXPR_BINOP, op->loc);
        b->binop.op = "&"; b->binop.left = e; b->binop.right = r;
        e = b;
    }
    return e;
}

static Expr* parse_bitxor(Parser* ps) {
    Expr* e = parse_bitand(ps);
    while (p_match(ps, TOK_CARET)) {
        Token* op = p_peek_at(ps, -1);
        Expr* r = parse_bitand(ps);
        Expr* b = new_expr(EXPR_BINOP, op->loc);
        b->binop.op = "^"; b->binop.left = e; b->binop.right = r;
        e = b;
    }
    return e;
}

static Expr* parse_bitor(Parser* ps) {
    Expr* e = parse_bitxor(ps);
    while (p_match(ps, TOK_PIPE)) {
        Token* op = p_peek_at(ps, -1);
        Expr* r = parse_bitxor(ps);
        Expr* b = new_expr(EXPR_BINOP, op->loc);
        b->binop.op = "|"; b->binop.left = e; b->binop.right = r;
        e = b;
    }
    return e;
}

static Expr* parse_comparison(Parser* ps) {
    Expr* e = parse_bitor(ps);
    Expr* acc = NULL;
    while (true) {
        const char* op = NULL;
        SrcLoc loc = p_peek(ps)->loc;
        if (p_match(ps, TOK_EQ)) op = "==";
        else if (p_match(ps, TOK_NE)) op = "!=";
        else if (p_match(ps, TOK_LT)) op = "<";
        else if (p_match(ps, TOK_LE)) op = "<=";
        else if (p_match(ps, TOK_GT)) op = ">";
        else if (p_match(ps, TOK_GE)) op = ">=";
        else if (p_match(ps, TOK_IN)) op = "in";
        else if (p_check(ps, TOK_NOT) && p_peek_at(ps, 1)->kind == TOK_IN) { ps->pos += 2; op = "not in"; }
        else if (p_check(ps, TOK_IS)) {
            ps->pos++;
            if (p_match(ps, TOK_NOT)) op = "is not";
            else op = "is";
        } else break;

        Expr* r = parse_bitor(ps);
        Expr* cmp = new_expr(EXPR_BINOP, loc);
        cmp->binop.op = op; cmp->binop.left = e; cmp->binop.right = r;
        if (!acc) acc = cmp;
        else {
            Expr* joined = new_expr(EXPR_BINOP, loc);
            joined->binop.op = "and"; joined->binop.left = acc; joined->binop.right = cmp;
            acc = joined;
        }
        e = r;
    }
    return acc ? acc : e;
}

static Expr* parse_not(Parser* ps) {
    if (p_check(ps, TOK_NOT) && p_peek_at(ps, 1)->kind != TOK_IN) {
        Token* t = p_consume(ps, TOK_NOT);
        Expr* opnd = parse_not(ps);
        Expr* u = new_expr(EXPR_UNOP, t->loc);
        u->unop.op = "not"; u->unop.operand = opnd;
        return u;
    }
    return parse_comparison(ps);
}

static Expr* parse_and(Parser* ps) {
    Expr* e = parse_not(ps);
    while (p_match(ps, TOK_AND)) {
        Token* t = p_peek_at(ps, -1);
        Expr* r = parse_not(ps);
        Expr* b = new_expr(EXPR_BINOP, t->loc);
        b->binop.op = "and"; b->binop.left = e; b->binop.right = r;
        e = b;
    }
    return e;
}

static Expr* parse_or(Parser* ps) {
    Expr* e = parse_and(ps);
    while (p_match(ps, TOK_OR)) {
        Token* t = p_peek_at(ps, -1);
        Expr* r = parse_and(ps);
        Expr* b = new_expr(EXPR_BINOP, t->loc);
        b->binop.op = "or"; b->binop.left = e; b->binop.right = r;
        e = b;
    }
    return e;
}

static Expr* parse_no_ternary(Parser* ps) {
    return parse_or(ps);
}

static Expr* parse_expr(Parser* ps) {
    if (p_check(ps, TOK_LAMBDA)) {
        Token* t = p_consume(ps, TOK_LAMBDA);
        const char** pnames = NULL; TypeExpr** ptypes = NULL; int pcnt = 0, pcap = 0;
        if (!p_check(ps, TOK_COLON) && !p_check(ps, TOK_ARROW)) {
            do {
                Token* id = p_consume(ps, TOK_NAME);
                TypeExpr* pt = NULL;
                if (p_match(ps, TOK_COLON)) pt = parse_type_expr(ps);
                if (pcnt >= pcap) {
                    pcap = pcap ? pcap * 2 : 4;
                    pnames = (const char**)realloc(pnames, sizeof(char*) * (size_t)pcap);
                    ptypes = (TypeExpr**)realloc(ptypes, sizeof(TypeExpr*) * (size_t)pcap);
                }
                pnames[pcnt] = id->text; ptypes[pcnt] = pt; pcnt++;
            } while (p_match(ps, TOK_COMMA));
        }
        TypeExpr* rtype = NULL;
        if (p_match(ps, TOK_ARROW)) rtype = parse_type_expr(ps);
        p_consume(ps, TOK_COLON);
        Expr* body = parse_expr(ps);
        Expr* le = new_expr(EXPR_LAMBDA, t->loc);
        le->lambda.params = pcnt ? (const char**)arena_alloc(sizeof(char*) * (size_t)pcnt) : NULL;
        le->lambda.param_types = pcnt ? (TypeExpr**)arena_alloc(sizeof(TypeExpr*) * (size_t)pcnt) : NULL;
        if (pcnt) {
            memcpy(le->lambda.params, pnames, sizeof(char*) * (size_t)pcnt);
            memcpy(le->lambda.param_types, ptypes, sizeof(TypeExpr*) * (size_t)pcnt);
        }
        free(pnames); free(ptypes);
        le->lambda.param_count = pcnt;
        le->lambda.ret_type = rtype;
        le->lambda.body = body;
        return le;
    }
    Expr* e = parse_or(ps);
    if (p_match(ps, TOK_IF)) {
        Token* t = p_peek_at(ps, -1);
        Expr* cond = parse_or(ps);
        p_consume(ps, TOK_ELSE);
        Expr* else_e = parse_expr(ps);
        Expr* te = new_expr(EXPR_TERNARY, t->loc);
        te->ternary.then_e = e; te->ternary.cond = cond; te->ternary.else_e = else_e;
        return te;
    }
    return e;
}

static Expr* parse_expr_or_tuple(Parser* ps) {
    Expr* first = parse_expr(ps);
    if (p_match(ps, TOK_COMMA)) {
        Expr** items = (Expr**)malloc(sizeof(Expr*) * 8); int cnt = 0, cap = 8;
        items[cnt++] = first;
        while (!p_check(ps, TOK_NEWLINE) && !p_check(ps, TOK_EOF) && !p_check(ps, TOK_ASSIGN)) {
            if (cnt >= cap) { cap *= 2; items = (Expr**)realloc(items, sizeof(Expr*) * (size_t)cap); }
            items[cnt++] = parse_expr(ps);
            if (!p_match(ps, TOK_COMMA)) break;
        }
        Expr* te = new_expr(EXPR_TUPLE, first->loc);
        te->seq.items = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)cnt);
        memcpy(te->seq.items, items, sizeof(Expr*) * (size_t)cnt);
        free(items);
        te->seq.count = cnt;
        return te;
    }
    return first;
}

static Stmt** parse_suite(Parser* ps, int* out_count) {
    Stmt** list = NULL; int cnt = 0, cap = 0;
    if (p_match(ps, TOK_NEWLINE)) {
        skip_newlines(ps);
        p_consume(ps, TOK_INDENT);
        while (!p_check(ps, TOK_DEDENT) && !p_check(ps, TOK_EOF)) {
            skip_newlines(ps);
            if (p_check(ps, TOK_DEDENT) || p_check(ps, TOK_EOF)) break;
            Stmt* s = parse_stmt(ps);
            if (cnt >= cap) { cap = cap ? cap * 2 : 16; list = (Stmt**)realloc(list, sizeof(Stmt*) * (size_t)cap); }
            list[cnt++] = s;
            skip_newlines(ps);
        }
        p_match(ps, TOK_DEDENT);
    } else {
        Stmt* s = parse_stmt(ps);
        list = (Stmt**)malloc(sizeof(Stmt*));
        list[cnt++] = s;
    }
    Stmt** res = (Stmt**)arena_alloc(sizeof(Stmt*) * (size_t)(cnt ? cnt : 1));
    if (cnt > 0) memcpy(res, list, sizeof(Stmt*) * (size_t)cnt);
    free(list);
    *out_count = cnt;
    return res;
}

static Stmt* parse_def(Parser* ps, Expr** decos, int deco_count, bool is_async) {
    Token* t = p_consume(ps, TOK_DEF);
    Token* name = p_consume(ps, TOK_NAME);
    p_consume(ps, TOK_LPAREN);
    Param* params = NULL; int pcnt = 0, pcap = 0;
    if (!p_check(ps, TOK_RPAREN)) {
        do {
            if (p_check(ps, TOK_RPAREN)) break;
            Token* pid = p_consume(ps, TOK_NAME);
            TypeExpr* pann = NULL;
            if (p_match(ps, TOK_COLON)) pann = parse_type_expr(ps);
            Expr* defv = NULL;
            if (p_match(ps, TOK_ASSIGN)) defv = parse_expr(ps);
            if (pcnt >= pcap) { pcap = pcap ? pcap * 2 : 8; params = (Param*)realloc(params, sizeof(Param) * (size_t)pcap); }
            params[pcnt++] = (Param){pid->text, pann, defv, pid->loc};
        } while (p_match(ps, TOK_COMMA));
    }
    p_consume(ps, TOK_RPAREN);
    TypeExpr* ret_type = NULL;
    if (p_match(ps, TOK_ARROW)) ret_type = parse_type_expr(ps);
    p_consume(ps, TOK_COLON);
    int bcnt = 0;
    Stmt** body = parse_suite(ps, &bcnt);

    Stmt* s = new_stmt(STMT_DEF, t->loc);
    s->def_s.name = name->text;
    s->def_s.params = pcnt ? (Param*)arena_alloc(sizeof(Param) * (size_t)pcnt) : NULL;
    if (pcnt) memcpy(s->def_s.params, params, sizeof(Param) * (size_t)pcnt);
    free(params);
    s->def_s.param_count = pcnt;
    s->def_s.ret_type = ret_type;
    s->def_s.body = body;
    s->def_s.body_count = bcnt;
    s->def_s.decorators = decos;
    s->def_s.deco_count = deco_count;
    s->def_s.is_async = is_async;
    return s;
}

static Stmt* parse_class(Parser* ps, Expr** decos, int deco_count) {
    Token* t = p_consume(ps, TOK_CLASS);
    Token* name = p_consume(ps, TOK_NAME);
    const char** bases = NULL; int bcnt = 0, bcap = 0;
    if (p_match(ps, TOK_LPAREN)) {
        if (!p_check(ps, TOK_RPAREN)) {
            do {
                Token* bid = p_consume(ps, TOK_NAME);
                if (bcnt >= bcap) { bcap = bcap ? bcap * 2 : 4; bases = (const char**)realloc(bases, sizeof(char*) * (size_t)bcap); }
                bases[bcnt++] = bid->text;
            } while (p_match(ps, TOK_COMMA));
        }
        p_consume(ps, TOK_RPAREN);
    }
    p_consume(ps, TOK_COLON);
    int body_cnt = 0;
    Stmt** body = parse_suite(ps, &body_cnt);
    Stmt* s = new_stmt(STMT_CLASS, t->loc);
    s->class_s.name = name->text;
    s->class_s.bases = bcnt ? (const char**)arena_alloc(sizeof(char*) * (size_t)bcnt) : NULL;
    if (bcnt) memcpy(s->class_s.bases, bases, sizeof(char*) * (size_t)bcnt);
    free(bases);
    s->class_s.base_count = bcnt;
    s->class_s.body = body;
    s->class_s.body_count = body_cnt;
    s->class_s.decorators = decos;
    s->class_s.deco_count = deco_count;
    return s;
}

static Stmt* parse_stmt(Parser* ps) {
    skip_newlines(ps);
    Token* t = p_peek(ps);

    if (p_check(ps, TOK_AT)) {
        Expr** decos = NULL; int dcnt = 0, dcap = 0;
        while (p_match(ps, TOK_AT)) {
            Expr* de = parse_expr(ps);
            p_consume(ps, TOK_NEWLINE);
            skip_newlines(ps);
            if (dcnt >= dcap) { dcap = dcap ? dcap * 2 : 4; decos = (Expr**)realloc(decos, sizeof(Expr*) * (size_t)dcap); }
            decos[dcnt++] = de;
        }
        Expr** res_decos = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)dcnt);
        memcpy(res_decos, decos, sizeof(Expr*) * (size_t)dcnt);
        free(decos);
        if (p_match(ps, TOK_ASYNC)) return parse_def(ps, res_decos, dcnt, true);
        if (p_check(ps, TOK_DEF)) return parse_def(ps, res_decos, dcnt, false);
        if (p_check(ps, TOK_CLASS)) return parse_class(ps, res_decos, dcnt);
        error_at(p_peek(ps)->loc, "decorator must precede def or class");
    }

    if (p_match(ps, TOK_ASYNC)) {
        if (p_check(ps, TOK_DEF)) return parse_def(ps, NULL, 0, true);
        return parse_stmt(ps); /* async for / async with */
    }
    if (p_check(ps, TOK_DEF)) return parse_def(ps, NULL, 0, false);
    if (p_check(ps, TOK_CLASS)) return parse_class(ps, NULL, 0);

    if (p_match(ps, TOK_PASS)) { p_match(ps, TOK_NEWLINE); return new_stmt(STMT_PASS, t->loc); }
    if (p_match(ps, TOK_BREAK)) { p_match(ps, TOK_NEWLINE); return new_stmt(STMT_BREAK, t->loc); }
    if (p_match(ps, TOK_CONTINUE)) { p_match(ps, TOK_NEWLINE); return new_stmt(STMT_CONTINUE, t->loc); }

    if (p_match(ps, TOK_RETURN)) {
        Stmt* s = new_stmt(STMT_RETURN, t->loc);
        if (!p_check(ps, TOK_NEWLINE) && !p_check(ps, TOK_EOF) && !p_check(ps, TOK_DEDENT)) {
            s->ret.expr = parse_expr_or_tuple(ps);
        }
        p_match(ps, TOK_NEWLINE);
        return s;
    }

    if (p_match(ps, TOK_YIELD)) {
        if (p_match(ps, TOK_FROM)) {
            Stmt* s = new_stmt(STMT_YIELD_FROM, t->loc);
            s->expr_s.expr = parse_expr(ps);
            p_match(ps, TOK_NEWLINE);
            return s;
        }
        Stmt* s = new_stmt(STMT_YIELD, t->loc);
        if (!p_check(ps, TOK_NEWLINE) && !p_check(ps, TOK_EOF) && !p_check(ps, TOK_DEDENT)) {
            s->expr_s.expr = parse_expr_or_tuple(ps);
        }
        p_match(ps, TOK_NEWLINE);
        return s;
    }

    if (p_match(ps, TOK_RAISE)) {
        Stmt* s = new_stmt(STMT_RAISE, t->loc);
        if (!p_check(ps, TOK_NEWLINE) && !p_check(ps, TOK_EOF) && !p_check(ps, TOK_DEDENT)) {
            s->raise_s.exc = parse_expr(ps);
        }
        p_match(ps, TOK_NEWLINE);
        return s;
    }

    if (p_match(ps, TOK_ASSERT)) {
        Stmt* s = new_stmt(STMT_ASSERT, t->loc);
        s->assert_s.cond = parse_expr(ps);
        if (p_match(ps, TOK_COMMA)) s->assert_s.msg = parse_expr(ps);
        p_match(ps, TOK_NEWLINE);
        return s;
    }

    if (p_match(ps, TOK_DEL)) {
        Stmt* s = new_stmt(STMT_DEL, t->loc);
        s->del_s.target = parse_expr(ps);
        p_match(ps, TOK_NEWLINE);
        return s;
    }

    if (p_match(ps, TOK_GLOBAL) || p_match(ps, TOK_NONLOCAL)) {
        bool is_glob = (t->kind == TOK_GLOBAL);
        const char** names = NULL; int cnt = 0, cap = 0;
        do {
            Token* id = p_consume(ps, TOK_NAME);
            if (cnt >= cap) { cap = cap ? cap * 2 : 4; names = (const char**)realloc(names, sizeof(char*) * (size_t)cap); }
            names[cnt++] = id->text;
        } while (p_match(ps, TOK_COMMA));
        p_match(ps, TOK_NEWLINE);
        Stmt* s = new_stmt(is_glob ? STMT_GLOBAL : STMT_NONLOCAL, t->loc);
        s->scope_s.names = (const char**)arena_alloc(sizeof(char*) * (size_t)cnt);
        memcpy(s->scope_s.names, names, sizeof(char*) * (size_t)cnt);
        free(names);
        s->scope_s.count = cnt;
        return s;
    }

    if (p_match(ps, TOK_IMPORT)) {
        Token* mod = p_consume(ps, TOK_NAME);
        const char* mod_name = mod->text;
        while (p_match(ps, TOK_DOT)) {
            Token* sub = p_consume(ps, TOK_NAME);
            mod_name = arena_sprintf("%s.%s", mod_name, sub->text);
        }
        const char* alias = NULL;
        if (p_match(ps, TOK_AS)) alias = p_consume(ps, TOK_NAME)->text;
        p_match(ps, TOK_NEWLINE);
        Stmt* s = new_stmt(STMT_IMPORT, t->loc);
        s->import_s.mod_name = mod_name;
        s->import_s.alias = alias;
        return s;
    }

    if (p_match(ps, TOK_FROM)) {
        Token* mod = p_consume(ps, TOK_NAME);
        const char* mod_name = mod->text;
        while (p_match(ps, TOK_DOT)) {
            Token* sub = p_consume(ps, TOK_NAME);
            mod_name = arena_sprintf("%s.%s", mod_name, sub->text);
        }
        p_consume(ps, TOK_IMPORT);
        const char** names = NULL; const char** aliases = NULL; int cnt = 0, cap = 0;
        bool parens = p_match(ps, TOK_LPAREN);
        do {
            Token* id = p_consume(ps, TOK_NAME);
            const char* al = NULL;
            if (p_match(ps, TOK_AS)) al = p_consume(ps, TOK_NAME)->text;
            if (cnt >= cap) {
                cap = cap ? cap * 2 : 8;
                names = (const char**)realloc(names, sizeof(char*) * (size_t)cap);
                aliases = (const char**)realloc(aliases, sizeof(char*) * (size_t)cap);
            }
            names[cnt] = id->text; aliases[cnt] = al; cnt++;
        } while (p_match(ps, TOK_COMMA) && !(parens && p_check(ps, TOK_RPAREN)));
        if (parens) p_consume(ps, TOK_RPAREN);
        p_match(ps, TOK_NEWLINE);
        Stmt* s = new_stmt(STMT_FROM_IMPORT, t->loc);
        s->from_import_s.mod_name = mod_name;
        s->from_import_s.names = (const char**)arena_alloc(sizeof(char*) * (size_t)cnt);
        s->from_import_s.aliases = (const char**)arena_alloc(sizeof(char*) * (size_t)cnt);
        memcpy(s->from_import_s.names, names, sizeof(char*) * (size_t)cnt);
        memcpy(s->from_import_s.aliases, aliases, sizeof(char*) * (size_t)cnt);
        free(names); free(aliases);
        s->from_import_s.count = cnt;
        return s;
    }

    if (p_match(ps, TOK_IF)) {
        Expr** conds = NULL; Stmt*** bodies = NULL; int* bcnts = NULL; int br = 0, bcap = 0;
        do {
            Expr* c = parse_expr(ps);
            p_consume(ps, TOK_COLON);
            int bc = 0; Stmt** b = parse_suite(ps, &bc);
            if (br >= bcap) {
                bcap = bcap ? bcap * 2 : 4;
                conds = (Expr**)realloc(conds, sizeof(Expr*) * (size_t)bcap);
                bodies = (Stmt***)realloc(bodies, sizeof(Stmt**) * (size_t)bcap);
                bcnts = (int*)realloc(bcnts, sizeof(int) * (size_t)bcap);
            }
            conds[br] = c; bodies[br] = b; bcnts[br] = bc; br++;
            skip_newlines(ps);
        } while (p_match(ps, TOK_ELIF));

        Stmt** eb = NULL; int ec = 0;
        if (p_match(ps, TOK_ELSE)) {
            p_consume(ps, TOK_COLON);
            eb = parse_suite(ps, &ec);
        }
        Stmt* s = new_stmt(STMT_IF, t->loc);
        s->if_s.conds = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)br);
        s->if_s.bodies = (Stmt***)arena_alloc(sizeof(Stmt**) * (size_t)br);
        s->if_s.body_counts = (int*)arena_alloc(sizeof(int) * (size_t)br);
        memcpy(s->if_s.conds, conds, sizeof(Expr*) * (size_t)br);
        memcpy(s->if_s.bodies, bodies, sizeof(Stmt**) * (size_t)br);
        memcpy(s->if_s.body_counts, bcnts, sizeof(int) * (size_t)br);
        free(conds); free(bodies); free(bcnts);
        s->if_s.branch_count = br;
        s->if_s.else_body = eb; s->if_s.else_count = ec;
        return s;
    }

    if (p_match(ps, TOK_WHILE)) {
        Expr* cond = parse_expr(ps);
        p_consume(ps, TOK_COLON);
        int bc = 0; Stmt** body = parse_suite(ps, &bc);
        Stmt** eb = NULL; int ec = 0;
        skip_newlines(ps);
        if (p_match(ps, TOK_ELSE)) { p_consume(ps, TOK_COLON); eb = parse_suite(ps, &ec); }
        Stmt* s = new_stmt(STMT_WHILE, t->loc);
        s->while_s.cond = cond; s->while_s.body = body; s->while_s.body_count = bc;
        s->while_s.else_body = eb; s->while_s.else_count = ec;
        return s;
    }

    if (p_match(ps, TOK_FOR)) {
        const char** targs = NULL; int tcnt = 0, tcap = 0;
        do {
            Token* id = p_consume(ps, TOK_NAME);
            if (tcnt >= tcap) { tcap = tcap ? tcap * 2 : 4; targs = (const char**)realloc(targs, sizeof(char*) * (size_t)tcap); }
            targs[tcnt++] = id->text;
        } while (p_match(ps, TOK_COMMA));
        p_consume(ps, TOK_IN);
        Expr* iter = parse_expr_or_tuple(ps);
        p_consume(ps, TOK_COLON);
        int bc = 0; Stmt** body = parse_suite(ps, &bc);
        Stmt** eb = NULL; int ec = 0;
        skip_newlines(ps);
        if (p_match(ps, TOK_ELSE)) { p_consume(ps, TOK_COLON); eb = parse_suite(ps, &ec); }
        Stmt* s = new_stmt(STMT_FOR, t->loc);
        s->for_s.targets = (const char**)arena_alloc(sizeof(char*) * (size_t)tcnt);
        memcpy(s->for_s.targets, targs, sizeof(char*) * (size_t)tcnt);
        free(targs);
        s->for_s.target_count = tcnt;
        s->for_s.iter = iter; s->for_s.body = body; s->for_s.body_count = bc;
        s->for_s.else_body = eb; s->for_s.else_count = ec;
        return s;
    }

    if (p_match(ps, TOK_WITH)) {
        Expr* ctx = parse_expr(ps);
        const char* as_name = NULL;
        if (p_match(ps, TOK_AS)) as_name = p_consume(ps, TOK_NAME)->text;
        p_consume(ps, TOK_COLON);
        int bc = 0; Stmt** body = parse_suite(ps, &bc);
        Stmt* s = new_stmt(STMT_WITH, t->loc);
        s->with_s.ctx = ctx; s->with_s.as_name = as_name; s->with_s.body = body; s->with_s.body_count = bc;
        return s;
    }

    if (p_match(ps, TOK_TRY)) {
        p_consume(ps, TOK_COLON);
        int tc = 0; Stmt** tb = parse_suite(ps, &tc);
        ExceptHandler* hs = NULL; int hcnt = 0, hcap = 0;
        skip_newlines(ps);
        while (p_match(ps, TOK_EXCEPT)) {
            const char* etype = NULL; const char* aname = NULL;
            if (!p_check(ps, TOK_COLON)) {
                etype = p_consume(ps, TOK_NAME)->text;
                if (p_match(ps, TOK_AS)) aname = p_consume(ps, TOK_NAME)->text;
            }
            p_consume(ps, TOK_COLON);
            int hbc = 0; Stmt** hb = parse_suite(ps, &hbc);
            if (hcnt >= hcap) { hcap = hcap ? hcap * 2 : 4; hs = (ExceptHandler*)realloc(hs, sizeof(ExceptHandler) * (size_t)hcap); }
            hs[hcnt++] = (ExceptHandler){etype, aname, hb, hbc};
            skip_newlines(ps);
        }
        Stmt** eb = NULL; int ec = 0;
        if (p_match(ps, TOK_ELSE)) { p_consume(ps, TOK_COLON); eb = parse_suite(ps, &ec); skip_newlines(ps); }
        Stmt** fb = NULL; int fc = 0;
        if (p_match(ps, TOK_FINALLY)) { p_consume(ps, TOK_COLON); fb = parse_suite(ps, &fc); }
        Stmt* s = new_stmt(STMT_TRY, t->loc);
        s->try_s.try_body = tb; s->try_s.try_count = tc;
        s->try_s.handlers = hcnt ? (ExceptHandler*)arena_alloc(sizeof(ExceptHandler) * (size_t)hcnt) : NULL;
        if (hcnt) memcpy(s->try_s.handlers, hs, sizeof(ExceptHandler) * (size_t)hcnt);
        free(hs);
        s->try_s.handler_count = hcnt;
        s->try_s.else_body = eb; s->try_s.else_count = ec;
        s->try_s.finally_body = fb; s->try_s.finally_count = fc;
        return s;
    }

    if (p_match(ps, TOK_MATCH)) {
        Expr* subj = parse_expr(ps);
        p_consume(ps, TOK_COLON);
        p_consume(ps, TOK_NEWLINE);
        skip_newlines(ps);
        p_consume(ps, TOK_INDENT);
        MatchCase* cases = NULL; int ccnt = 0, ccap = 0;
        while (p_match(ps, TOK_CASE)) {
            Expr* pat = NULL; const char* cap_name = NULL;
            if (p_check(ps, TOK_NAME) && strcmp(p_peek(ps)->text, "_") == 0 && p_peek_at(ps, 1)->kind == TOK_COLON) {
                ps->pos++;
            } else if (p_check(ps, TOK_NAME) && p_peek_at(ps, 1)->kind == TOK_COLON) {
                cap_name = p_consume(ps, TOK_NAME)->text;
            } else {
                pat = parse_expr(ps);
            }
            p_consume(ps, TOK_COLON);
            int cbc = 0; Stmt** cb = parse_suite(ps, &cbc);
            if (ccnt >= ccap) { ccap = ccap ? ccap * 2 : 8; cases = (MatchCase*)realloc(cases, sizeof(MatchCase) * (size_t)ccap); }
            cases[ccnt++] = (MatchCase){pat, cap_name, cb, cbc};
            skip_newlines(ps);
        }
        p_match(ps, TOK_DEDENT);
        Stmt* s = new_stmt(STMT_MATCH, t->loc);
        s->match_s.subject = subj;
        s->match_s.cases = ccnt ? (MatchCase*)arena_alloc(sizeof(MatchCase) * (size_t)ccnt) : NULL;
        if (ccnt) memcpy(s->match_s.cases, cases, sizeof(MatchCase) * (size_t)ccnt);
        free(cases);
        s->match_s.case_count = ccnt;
        return s;
    }

    /* Assignment, annotated assignment, augmented assignment, or expression statement */
    Expr* lhs = parse_expr_or_tuple(ps);

    if (p_match(ps, TOK_COLON)) {
        TypeExpr* tann = parse_type_expr(ps);
        Expr* val = NULL;
        if (p_match(ps, TOK_ASSIGN)) val = parse_expr_or_tuple(ps);
        p_match(ps, TOK_NEWLINE);
        Stmt* s = new_stmt(STMT_ASSIGN, t->loc);
        s->assign.target = lhs; s->assign.type_ann = tann; s->assign.value = val;
        return s;
    }

    if (p_match(ps, TOK_ASSIGN)) {
        if (lhs->kind == EXPR_TUPLE) {
            Expr* val = parse_expr_or_tuple(ps);
            p_match(ps, TOK_NEWLINE);
            Stmt* s = new_stmt(STMT_MULTI_ASSIGN, t->loc);
            s->multi_assign.targets = lhs->seq.items;
            s->multi_assign.target_count = lhs->seq.count;
            s->multi_assign.value = val;
            return s;
        }
        Expr* val = parse_expr_or_tuple(ps);
        p_match(ps, TOK_NEWLINE);
        Stmt* s = new_stmt(STMT_ASSIGN, t->loc);
        s->assign.target = lhs; s->assign.type_ann = NULL; s->assign.value = val;
        return s;
    }

    TokKind ak = p_peek(ps)->kind;
    if (ak >= TOK_PLUSEQ && ak <= TOK_ATEQ) {
        Token* op = p_peek(ps); ps->pos++;
        Expr* val = parse_expr_or_tuple(ps);
        p_match(ps, TOK_NEWLINE);
        Stmt* s = new_stmt(STMT_AUG_ASSIGN, t->loc);
        s->aug_assign.target = lhs; s->aug_assign.op = op->text; s->aug_assign.value = val;
        return s;
    }

    p_match(ps, TOK_NEWLINE);
    Stmt* s = new_stmt(STMT_EXPR, t->loc);
    s->expr_s.expr = lhs;
    return s;
}

static Stmt** parse_module(Parser* ps, int* out_count) {
    Stmt** list = NULL; int cnt = 0, cap = 0;
    while (!p_check(ps, TOK_EOF)) {
        skip_newlines(ps);
        if (p_check(ps, TOK_EOF)) break;
        Stmt* s = parse_stmt(ps);
        if (cnt >= cap) { cap = cap ? cap * 2 : 32; list = (Stmt**)realloc(list, sizeof(Stmt*) * (size_t)cap); }
        list[cnt++] = s;
    }
    Stmt** res = (Stmt**)arena_alloc(sizeof(Stmt*) * (size_t)(cnt ? cnt : 1));
    if (cnt > 0) memcpy(res, list, sizeof(Stmt*) * (size_t)cnt);
    free(list);
    *out_count = cnt;
    return res;
}

/* =========================================================================
 * 4. STATIC TYPE SYSTEM & SEMANTIC ANALYZER
 * ========================================================================= */

typedef struct ClassInfo ClassInfo;
typedef struct FuncInfo FuncInfo;
typedef struct ModuleInfo ModuleInfo;

typedef enum {
    TY_VOID = 0,
    TY_BOOL,
    TY_I8, TY_I16, TY_I32, TY_I64,
    TY_U8, TY_U16, TY_U32, TY_U64,
    TY_F32, TY_F64,
    TY_STR,
    TY_BYTES,
    TY_BIGINT,
    TY_FILE,
    TY_NONE_LIT,
    TY_OPTIONAL,
    TY_LIST,
    TY_SET,
    TY_DICT,
    TY_TUPLE,
    TY_CLASS,
    TY_FUNC,
    TY_MODULE
} TypeKind;

struct Type {
    TypeKind kind;
    const char* mangled;
    Type* elem;
    Type* key;
    Type* val;
    Type** tuple_elems;
    int tuple_count;
    ClassInfo* cls;
    Type** func_params;
    int func_param_count;
    Type* func_ret;
    ModuleInfo* mod;
};

typedef struct FieldInfo {
    const char* name;
    Type* type;
    Expr* default_expr;
    int slot_index;
    SrcLoc loc;
} FieldInfo;

typedef struct MethodSlot {
    const char* name;
    FuncInfo* fn;
    int vtable_index;
} MethodSlot;

struct ClassInfo {
    const char* name;
    const char* llvm_name;
    ModuleInfo* mod;
    Stmt* ast;
    Type* type;
    ClassInfo** bases;
    int base_count;
    ClassInfo** mro;
    int mro_count;
    FieldInfo* fields;
    int field_count;
    FieldInfo* class_attrs;
    int class_attr_count;
    FuncInfo** methods;
    int method_count;
    MethodSlot* vtable;
    int vtable_count;
    const char** abstract_methods;
    int abstract_count;
    bool is_abc;
    bool is_dataclass;
    bool has_subclasses;
    bool needs_vtable;
    bool is_value_type;
    bool has_unimpl_abstract;
    bool analyzed;
    int type_id;
    uint64_t ancestors_mask;
    ClassInfo** subclasses;
    int subclass_count;
};

typedef struct VarSym {
    const char* name;
    Type* type;
    bool is_global;
    bool is_nonlocal;
    bool is_captured;
    bool is_param;
    const char* llvm_name;
    SrcLoc loc;
} VarSym;

struct FuncInfo {
    const char* name;
    const char* llvm_name;
    ModuleInfo* mod;
    ClassInfo* owner_cls;
    FuncInfo* parent_fn;
    Stmt* ast;
    Expr* lambda_ast;
    Type* type;
    const char** param_names;
    Type** param_types;
    Expr** param_defaults;
    int param_count;
    Type* ret_type;
    bool is_method;
    bool is_static;
    bool is_classmethod;
    bool is_property;
    bool is_abstract;
    bool is_generator;
    bool is_lambda;
    bool is_builtin_deco;
    bool has_captures;
    VarSym* captures;
    int capture_count;
    VarSym* locals;
    int local_count;
    int local_cap;
    const char** global_decls;
    int global_decl_count;
    const char** nonlocal_decls;
    int nonlocal_decl_count;
};

struct ModuleInfo {
    const char* name;
    const char* path;
    const char* prefix;
    Stmt** stmts;
    int stmt_count;
    VarSym* globals;
    int global_count;
    int global_cap;
    FuncInfo** funcs;
    int func_count;
    ClassInfo** classes;
    int class_count;
    bool analyzed;
};

typedef struct {
    Type** items;
    int count;
    int cap;
} TypeRegistry;

static Type* g_ty_void;
static Type* g_ty_bool;
static Type* g_ty_i8;
static Type* g_ty_i16;
static Type* g_ty_i32;
static Type* g_ty_i64;
static Type* g_ty_u8;
static Type* g_ty_u16;
static Type* g_ty_u32;
static Type* g_ty_u64;
static Type* g_ty_f32;
static Type* g_ty_f64;
static Type* g_ty_str;
static Type* g_ty_bytes;
static Type* g_ty_bigint;
static Type* g_ty_file;
static Type* g_ty_none_lit;

static TypeRegistry g_type_cache = {NULL, 0, 0};
static TypeRegistry g_mono_lists = {NULL, 0, 0};
static TypeRegistry g_mono_sets = {NULL, 0, 0};
static TypeRegistry g_mono_dicts = {NULL, 0, 0};
static TypeRegistry g_mono_tuples = {NULL, 0, 0};
static TypeRegistry g_mono_opts = {NULL, 0, 0};

static ModuleInfo** g_modules = NULL;
static int g_module_count = 0;
static ClassInfo** g_all_classes = NULL;
static int g_all_class_count = 0;
static FuncInfo** g_all_funcs = NULL;
static int g_all_func_count = 0;
static int g_next_class_id = 10;
static const char* g_root_dir = ".";

static Type* make_prim_type(TypeKind k, const char* mangled) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = k;
    t->mangled = mangled;
    return t;
}

static void init_builtin_types(void) {
    g_ty_void = make_prim_type(TY_VOID, "void");
    g_ty_bool = make_prim_type(TY_BOOL, "bool");
    g_ty_i8 = make_prim_type(TY_I8, "i8");
    g_ty_i16 = make_prim_type(TY_I16, "i16");
    g_ty_i32 = make_prim_type(TY_I32, "i32");
    g_ty_i64 = make_prim_type(TY_I64, "i64");
    g_ty_u8 = make_prim_type(TY_U8, "u8");
    g_ty_u16 = make_prim_type(TY_U16, "u16");
    g_ty_u32 = make_prim_type(TY_U32, "u32");
    g_ty_u64 = make_prim_type(TY_U64, "u64");
    g_ty_f32 = make_prim_type(TY_F32, "f32");
    g_ty_f64 = make_prim_type(TY_F64, "f64");
    g_ty_str = make_prim_type(TY_STR, "str");
    g_ty_bytes = make_prim_type(TY_BYTES, "bytes");
    g_ty_bigint = make_prim_type(TY_BIGINT, "bigint");
    g_ty_file = make_prim_type(TY_FILE, "file");
    g_ty_none_lit = make_prim_type(TY_NONE_LIT, "none");
}

static bool type_is_signed_int(Type* t) { return t && (t->kind >= TY_I8 && t->kind <= TY_I64); }
static bool type_is_unsigned_int(Type* t) { return t && (t->kind >= TY_U8 && t->kind <= TY_U64); }
static bool type_is_int(Type* t) { return type_is_signed_int(t) || type_is_unsigned_int(t); }
static bool type_is_float(Type* t) { return t && (t->kind == TY_F32 || t->kind == TY_F64); }
static bool type_is_numeric(Type* t) { return type_is_int(t) || type_is_float(t); }
static bool type_is_heap_ref(Type* t) {
    if (!t) return false;
    if (t->kind == TY_STR || t->kind == TY_BYTES || t->kind == TY_BIGINT || t->kind == TY_FILE ||
        t->kind == TY_LIST || t->kind == TY_SET || t->kind == TY_DICT || t->kind == TY_FUNC) return true;
    if (t->kind == TY_CLASS) return !t->cls->is_value_type;
    if (t->kind == TY_OPTIONAL) return type_is_heap_ref(t->elem);
    return false;
}

static bool type_eq(Type* a, Type* b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
        case TY_OPTIONAL:
        case TY_LIST:
        case TY_SET:
            return type_eq(a->elem, b->elem);
        case TY_DICT:
            return type_eq(a->key, b->key) && type_eq(a->val, b->val);
        case TY_TUPLE:
            if (a->tuple_count != b->tuple_count) return false;
            for (int i = 0; i < a->tuple_count; i++) {
                if (!type_eq(a->tuple_elems[i], b->tuple_elems[i])) return false;
            }
            return true;
        case TY_CLASS:
            return a->cls == b->cls;
        case TY_FUNC:
            if (a->func_param_count != b->func_param_count || !type_eq(a->func_ret, b->func_ret)) return false;
            for (int i = 0; i < a->func_param_count; i++) {
                if (!type_eq(a->func_params[i], b->func_params[i])) return false;
            }
            return true;
        case TY_MODULE:
            return a->mod == b->mod;
        default:
            return true;
    }
}

static const char* type_str(Type* t) {
    if (!t) return "<unknown>";
    switch (t->kind) {
        case TY_VOID: return "None";
        case TY_BOOL: return "bool";
        case TY_I8: return "i8";
        case TY_I16: return "i16";
        case TY_I32: return "i32";
        case TY_I64: return "int";
        case TY_U8: return "u8";
        case TY_U16: return "u16";
        case TY_U32: return "u32";
        case TY_U64: return "u64";
        case TY_F32: return "f32";
        case TY_F64: return "float";
        case TY_STR: return "str";
        case TY_BYTES: return "bytes";
        case TY_BIGINT: return "bigint";
        case TY_FILE: return "File";
        case TY_NONE_LIT: return "None";
        case TY_OPTIONAL: return arena_sprintf("%s | None", type_str(t->elem));
        case TY_LIST: return arena_sprintf("list[%s]", type_str(t->elem));
        case TY_SET: return arena_sprintf("set[%s]", type_str(t->elem));
        case TY_DICT: return arena_sprintf("dict[%s, %s]", type_str(t->key), type_str(t->val));
        case TY_TUPLE: {
            StrBuf sb; sb_init(&sb);
            sb_append(&sb, "tuple[");
            for (int i = 0; i < t->tuple_count; i++) {
                if (i > 0) sb_append(&sb, ", ");
                sb_append(&sb, type_str(t->tuple_elems[i]));
            }
            sb_append(&sb, "]");
            char* r = arena_strdup(sb.data);
            free(sb.data);
            return r;
        }
        case TY_CLASS: return t->cls->name;
        case TY_FUNC: return "Callable";
        case TY_MODULE: return arena_sprintf("module(%s)", t->mod->name);
    }
    return "<type>";
}

static void reg_add_unique(TypeRegistry* r, Type* t) {
    for (int i = 0; i < r->count; i++) {
        if (type_eq(r->items[i], t)) return;
    }
    if (r->count >= r->cap) {
        r->cap = r->cap ? r->cap * 2 : 16;
        r->items = (Type**)realloc(r->items, sizeof(Type*) * (size_t)r->cap);
    }
    r->items[r->count++] = t;
}

static Type* intern_type(Type* t) {
    for (int i = 0; i < g_type_cache.count; i++) {
        if (type_eq(g_type_cache.items[i], t)) return g_type_cache.items[i];
    }
    if (g_type_cache.count >= g_type_cache.cap) {
        g_type_cache.cap = g_type_cache.cap ? g_type_cache.cap * 2 : 64;
        g_type_cache.items = (Type**)realloc(g_type_cache.items, sizeof(Type*) * (size_t)g_type_cache.cap);
    }
    g_type_cache.items[g_type_cache.count++] = t;
    return t;
}

static Type* make_optional_type(Type* inner) {
    if (inner->kind == TY_OPTIONAL) return inner;
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_OPTIONAL;
    t->elem = inner;
    t->mangled = arena_sprintf("Opt_%s", inner->mangled);
    t = intern_type(t);
    if (!type_is_heap_ref(inner)) reg_add_unique(&g_mono_opts, t);
    return t;
}

static Type* make_list_type(Type* elem) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_LIST;
    t->elem = elem;
    t->mangled = arena_sprintf("List_%s", elem->mangled);
    t = intern_type(t);
    reg_add_unique(&g_mono_lists, t);
    return t;
}

static Type* make_set_type(Type* elem) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_SET;
    t->elem = elem;
    t->mangled = arena_sprintf("Set_%s", elem->mangled);
    t = intern_type(t);
    reg_add_unique(&g_mono_sets, t);
    return t;
}

static Type* make_dict_type(Type* key, Type* val) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_DICT;
    t->key = key;
    t->val = val;
    t->mangled = arena_sprintf("Dict_%s_%s", key->mangled, val->mangled);
    t = intern_type(t);
    reg_add_unique(&g_mono_dicts, t);
    return t;
}

static Type* make_tuple_type(Type** elems, int count) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_TUPLE;
    t->tuple_count = count;
    t->tuple_elems = count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)count) : NULL;
    StrBuf sb; sb_init(&sb);
    sb_append(&sb, "Tuple");
    for (int i = 0; i < count; i++) {
        t->tuple_elems[i] = elems[i];
        sb_printf(&sb, "_%s", elems[i]->mangled);
    }
    t->mangled = arena_strdup(sb.data);
    free(sb.data);
    t = intern_type(t);
    reg_add_unique(&g_mono_tuples, t);
    return t;
}

static Type* make_func_type(Type** params, int count, Type* ret) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_FUNC;
    t->func_param_count = count;
    t->func_params = count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)count) : NULL;
    for (int i = 0; i < count; i++) t->func_params[i] = params[i];
    t->func_ret = ret ? ret : g_ty_void;
    t->mangled = "fn";
    return intern_type(t);
}

static bool class_is_subclass(ClassInfo* sub, ClassInfo* base) {
    if (!sub || !base) return false;
    if (sub == base) return true;
    for (int i = 0; i < sub->mro_count; i++) {
        if (sub->mro[i] == base) return true;
    }
    return false;
}

static bool type_assignable(Type* dst, Type* src) {
    if (!dst || !src) return false;
    if (type_eq(dst, src)) return true;
    if (dst->kind == TY_OPTIONAL) {
        if (src->kind == TY_NONE_LIT || src->kind == TY_VOID) return true;
        return type_assignable(dst->elem, src);
    }
    if (dst->kind == TY_CLASS && src->kind == TY_CLASS) {
        return class_is_subclass(src->cls, dst->cls);
    }
    if (dst->kind == TY_F64 && (src->kind == TY_F32 || type_is_int(src))) return true;
    if (dst->kind == TY_F32 && type_is_int(src)) return true;
    if (type_is_signed_int(dst) && src->kind == TY_BOOL) return true;
    if (dst->kind == TY_I64 && (src->kind == TY_I8 || src->kind == TY_I16 || src->kind == TY_I32)) return true;
    if (dst->kind == TY_I32 && (src->kind == TY_I8 || src->kind == TY_I16)) return true;
    if (dst->kind == TY_I16 && src->kind == TY_I8) return true;
    if (dst->kind == TY_U64 && (src->kind == TY_U8 || src->kind == TY_U16 || src->kind == TY_U32)) return true;
    if (dst->kind == TY_U32 && (src->kind == TY_U8 || src->kind == TY_U16)) return true;
    if (dst->kind == TY_U16 && src->kind == TY_U8) return true;
    return false;
}

static Type* numeric_common_type(Type* a, Type* b) {
    if (type_eq(a, b)) return a;
    if (a->kind == TY_BIGINT || b->kind == TY_BIGINT) return g_ty_bigint;
    if (a->kind == TY_F64 || b->kind == TY_F64) return g_ty_f64;
    if (a->kind == TY_F32 || b->kind == TY_F32) return (type_is_int(a) || type_is_int(b)) ? g_ty_f64 : g_ty_f32;
    if (type_is_unsigned_int(a) && type_is_unsigned_int(b)) return (a->kind > b->kind) ? a : b;
    if (type_is_signed_int(a) && type_is_signed_int(b)) return (a->kind > b->kind) ? a : b;
    if ((type_is_int(a) && b->kind == TY_BOOL) || (type_is_int(b) && a->kind == TY_BOOL)) return type_is_int(a) ? a : b;
    return g_ty_i64;
}

static ModuleInfo* load_module_by_name(const char* mod_name, const char* from_dir, SrcLoc loc);

static ClassInfo* find_class_in_mod(ModuleInfo* mod, const char* name) {
    for (int i = 0; i < mod->class_count; i++) {
        if (strcmp(mod->classes[i]->name, name) == 0) return mod->classes[i];
    }
    return NULL;
}

static FuncInfo* find_func_in_mod(ModuleInfo* mod, const char* name) {
    for (int i = 0; i < mod->func_count; i++) {
        if (strcmp(mod->funcs[i]->name, name) == 0) return mod->funcs[i];
    }
    return NULL;
}

static VarSym* find_global_in_mod(ModuleInfo* mod, const char* name) {
    for (int i = 0; i < mod->global_count; i++) {
        if (strcmp(mod->globals[i].name, name) == 0) return &mod->globals[i];
    }
    return NULL;
}

static VarSym* add_global_in_mod(ModuleInfo* mod, const char* name, Type* type, SrcLoc loc) {
    VarSym* existing = find_global_in_mod(mod, name);
    if (existing) return existing;
    if (mod->global_count >= mod->global_cap) {
        mod->global_cap = mod->global_cap ? mod->global_cap * 2 : 32;
        mod->globals = (VarSym*)realloc(mod->globals, sizeof(VarSym) * (size_t)mod->global_cap);
    }
    VarSym* v = &mod->globals[mod->global_count++];
    memset(v, 0, sizeof(VarSym));
    v->name = name;
    v->type = type;
    v->is_global = true;
    v->llvm_name = arena_sprintf("@%s%s", mod->prefix, name);
    v->loc = loc;
    return v;
}

static VarSym* find_local_in_fn(FuncInfo* fn, const char* name) {
    for (int i = 0; i < fn->local_count; i++) {
        if (strcmp(fn->locals[i].name, name) == 0) return &fn->locals[i];
    }
    return NULL;
}

static VarSym* add_local_in_fn(FuncInfo* fn, const char* name, Type* type, bool is_param, SrcLoc loc) {
    VarSym* existing = find_local_in_fn(fn, name);
    if (existing) return existing;
    if (fn->local_count >= fn->local_cap) {
        fn->local_cap = fn->local_cap ? fn->local_cap * 2 : 32;
        fn->locals = (VarSym*)realloc(fn->locals, sizeof(VarSym) * (size_t)fn->local_cap);
    }
    VarSym* v = &fn->locals[fn->local_count++];
    memset(v, 0, sizeof(VarSym));
    v->name = name;
    v->type = type;
    v->is_param = is_param;
    v->llvm_name = arena_sprintf("%%var_%s_%d", name, fn->local_count);
    v->loc = loc;
    return v;
}

static FieldInfo* find_field_in_class(ClassInfo* cls, const char* name) {
    for (int i = 0; i < cls->field_count; i++) {
        if (strcmp(cls->fields[i].name, name) == 0) return &cls->fields[i];
    }
    return NULL;
}

static FieldInfo* find_class_attr(ClassInfo* cls, const char* name) {
    for (int m = 0; m < cls->mro_count; m++) {
        ClassInfo* c = cls->mro[m];
        for (int i = 0; i < c->class_attr_count; i++) {
            if (strcmp(c->class_attrs[i].name, name) == 0) return &c->class_attrs[i];
        }
    }
    return NULL;
}

static FuncInfo* find_method_in_class_exact(ClassInfo* cls, const char* name) {
    for (int i = 0; i < cls->method_count; i++) {
        if (strcmp(cls->methods[i]->name, name) == 0) return cls->methods[i];
    }
    return NULL;
}

static FuncInfo* find_method_in_class(ClassInfo* cls, const char* name) {
    for (int m = 0; m < cls->mro_count; m++) {
        FuncInfo* f = find_method_in_class_exact(cls->mro[m], name);
        if (f) return f;
    }
    return NULL;
}

static MethodSlot* find_vtable_slot(ClassInfo* cls, const char* name) {
    for (int i = 0; i < cls->vtable_count; i++) {
        if (strcmp(cls->vtable[i].name, name) == 0) return &cls->vtable[i];
    }
    return NULL;
}

static Type* resolve_type_expr(ModuleInfo* mod, TypeExpr* te) {
    if (!te) return g_ty_void;
    if (te->kind == TEXPR_NAME) {
        const char* n = te->name;
        if (strcmp(n, "None") == 0) return g_ty_void;
        if (strcmp(n, "bool") == 0) return g_ty_bool;
        if (strcmp(n, "i8") == 0) return g_ty_i8;
        if (strcmp(n, "i16") == 0) return g_ty_i16;
        if (strcmp(n, "i32") == 0) return g_ty_i32;
        if (strcmp(n, "i64") == 0 || strcmp(n, "int") == 0) return g_ty_i64;
        if (strcmp(n, "u8") == 0) return g_ty_u8;
        if (strcmp(n, "u16") == 0) return g_ty_u16;
        if (strcmp(n, "u32") == 0) return g_ty_u32;
        if (strcmp(n, "u64") == 0) return g_ty_u64;
        if (strcmp(n, "f32") == 0) return g_ty_f32;
        if (strcmp(n, "f64") == 0 || strcmp(n, "float") == 0) return g_ty_f64;
        if (strcmp(n, "str") == 0) return g_ty_str;
        if (strcmp(n, "bytes") == 0) return g_ty_bytes;
        if (strcmp(n, "bigint") == 0) return g_ty_bigint;
        if (strcmp(n, "File") == 0) return g_ty_file;
        ClassInfo* ci = find_class_in_mod(mod, n);
        if (ci) return ci->type;
        VarSym* gv = find_global_in_mod(mod, n);
        if (gv && gv->type && gv->type->kind == TY_CLASS) return gv->type;
        if (strchr(n, '.')) {
            const char* dot = strchr(n, '.');
            const char* mname = arena_strndup(n, (size_t)(dot - n));
            const char* cname = dot + 1;
            VarSym* mv = find_global_in_mod(mod, mname);
            if (mv && mv->type && mv->type->kind == TY_MODULE) {
                ClassInfo* mci = find_class_in_mod(mv->type->mod, cname);
                if (mci) return mci->type;
            }
        }
        error_at(te->loc, "unknown type '%s'", n);
    }
    if (te->kind == TEXPR_UNION) {
        Type* a = resolve_type_expr(mod, te->args[0]);
        Type* b = resolve_type_expr(mod, te->args[1]);
        if (a->kind == TY_VOID && b->kind != TY_VOID) return make_optional_type(b);
        if (b->kind == TY_VOID && a->kind != TY_VOID) return make_optional_type(a);
        error_at(te->loc, "union types only support 'T | None'");
    }
    if (te->kind == TEXPR_GENERIC) {
        const char* n = te->name;
        if (strcmp(n, "list") == 0 || strcmp(n, "List") == 0) {
            if (te->arg_count != 1) error_at(te->loc, "list[...] requires 1 type argument");
            return make_list_type(resolve_type_expr(mod, te->args[0]));
        }
        if (strcmp(n, "set") == 0 || strcmp(n, "Set") == 0) {
            if (te->arg_count != 1) error_at(te->loc, "set[...] requires 1 type argument");
            return make_set_type(resolve_type_expr(mod, te->args[0]));
        }
        if (strcmp(n, "dict") == 0 || strcmp(n, "Dict") == 0) {
            if (te->arg_count != 2) error_at(te->loc, "dict[K, V] requires 2 type arguments");
            return make_dict_type(resolve_type_expr(mod, te->args[0]), resolve_type_expr(mod, te->args[1]));
        }
        if (strcmp(n, "Optional") == 0) {
            if (te->arg_count != 1) error_at(te->loc, "Optional[T] requires 1 type argument");
            return make_optional_type(resolve_type_expr(mod, te->args[0]));
        }
        if (strcmp(n, "tuple") == 0 || strcmp(n, "Tuple") == 0) {
            Type** ts = (Type**)arena_alloc(sizeof(Type*) * (size_t)(te->arg_count ? te->arg_count : 1));
            for (int i = 0; i < te->arg_count; i++) ts[i] = resolve_type_expr(mod, te->args[i]);
            return make_tuple_type(ts, te->arg_count);
        }
        if (strcmp(n, "Callable") == 0) {
            if (te->arg_count != 2 || te->args[0]->kind != TEXPR_TUPLE) {
                error_at(te->loc, "Callable requires [[ArgTypes...], RetType]");
            }
            int pc = te->args[0]->arg_count;
            Type** ps = pc ? (Type**)arena_alloc(sizeof(Type*) * (size_t)pc) : NULL;
            for (int i = 0; i < pc; i++) ps[i] = resolve_type_expr(mod, te->args[0]->args[i]);
            Type* rt = resolve_type_expr(mod, te->args[1]);
            return make_func_type(ps, pc, rt);
        }
        error_at(te->loc, "unknown generic type '%s'", n);
    }
    error_at(te->loc, "invalid type annotation");
    return g_ty_void;
}

static void compute_c3_mro(ClassInfo* cls, SrcLoc loc) {
    if (cls->mro_count > 0) return;
    if (cls->base_count == 0) {
        cls->mro_count = 1;
        cls->mro = (ClassInfo**)arena_alloc(sizeof(ClassInfo*));
        cls->mro[0] = cls;
        return;
    }
    for (int i = 0; i < cls->base_count; i++) compute_c3_mro(cls->bases[i], loc);
    int seq_cnt = cls->base_count + 1;
    ClassInfo*** seqs = (ClassInfo***)malloc(sizeof(ClassInfo**) * (size_t)seq_cnt);
    int* slens = (int*)malloc(sizeof(int) * (size_t)seq_cnt);
    int* spos = (int*)calloc((size_t)seq_cnt, sizeof(int));
    for (int i = 0; i < cls->base_count; i++) {
        seqs[i] = cls->bases[i]->mro;
        slens[i] = cls->bases[i]->mro_count;
    }
    seqs[cls->base_count] = cls->bases;
    slens[cls->base_count] = cls->base_count;

    ClassInfo** out = (ClassInfo**)malloc(sizeof(ClassInfo*) * 128);
    int out_cnt = 0;
    out[out_cnt++] = cls;

    while (true) {
        bool all_empty = true;
        for (int i = 0; i < seq_cnt; i++) if (spos[i] < slens[i]) { all_empty = false; break; }
        if (all_empty) break;

        ClassInfo* cand = NULL;
        for (int i = 0; i < seq_cnt; i++) {
            if (spos[i] >= slens[i]) continue;
            ClassInfo* h = seqs[i][spos[i]];
            bool in_tail = false;
            for (int j = 0; j < seq_cnt; j++) {
                for (int k = spos[j] + 1; k < slens[j]; k++) {
                    if (seqs[j][k] == h) { in_tail = true; break; }
                }
                if (in_tail) break;
            }
            if (!in_tail) { cand = h; break; }
        }
        if (!cand) error_at(loc, "inconsistent MRO for class '%s'", cls->name);
        out[out_cnt++] = cand;
        for (int i = 0; i < seq_cnt; i++) {
            if (spos[i] < slens[i] && seqs[i][spos[i]] == cand) spos[i]++;
        }
    }
    cls->mro_count = out_cnt;
    cls->mro = (ClassInfo**)arena_alloc(sizeof(ClassInfo*) * (size_t)out_cnt);
    memcpy(cls->mro, out, sizeof(ClassInfo*) * (size_t)out_cnt);
    free(seqs); free(slens); free(spos); free(out);
}

static bool expr_has_decorator(Expr** decos, int cnt, const char* name) {
    for (int i = 0; i < cnt; i++) {
        if (decos[i]->kind == EXPR_VAR && strcmp(decos[i]->var.name, name) == 0) return true;
    }
    return false;
}

static bool stmt_has_yield(Stmt** body, int count) {
    for (int i = 0; i < count; i++) {
        Stmt* s = body[i];
        if (s->kind == STMT_YIELD || s->kind == STMT_YIELD_FROM) return true;
        if (s->kind == STMT_IF) {
            for (int b = 0; b < s->if_s.branch_count; b++) {
                if (stmt_has_yield(s->if_s.bodies[b], s->if_s.body_counts[b])) return true;
            }
            if (stmt_has_yield(s->if_s.else_body, s->if_s.else_count)) return true;
        } else if (s->kind == STMT_WHILE) {
            if (stmt_has_yield(s->while_s.body, s->while_s.body_count)) return true;
        } else if (s->kind == STMT_FOR) {
            if (stmt_has_yield(s->for_s.body, s->for_s.body_count)) return true;
        } else if (s->kind == STMT_WITH) {
            if (stmt_has_yield(s->with_s.body, s->with_s.body_count)) return true;
        }
    }
    return false;
}

static Type* check_expr(ModuleInfo* mod, FuncInfo* fn, Expr* e, Type* hint);
static void check_stmts(ModuleInfo* mod, FuncInfo* fn, Stmt** stmts, int count);

static VarSym* resolve_var(ModuleInfo* mod, FuncInfo* fn, const char* name, FuncInfo** out_owner_fn) {
    if (out_owner_fn) *out_owner_fn = NULL;
    if (fn) {
        bool declared_global = false;
        for (int i = 0; i < fn->global_decl_count; i++) {
            if (strcmp(fn->global_decls[i], name) == 0) { declared_global = true; break; }
        }
        if (!declared_global) {
            VarSym* lv = find_local_in_fn(fn, name);
            if (lv) return lv;
            for (FuncInfo* pf = fn->parent_fn; pf != NULL; pf = pf->parent_fn) {
                VarSym* pv = find_local_in_fn(pf, name);
                if (pv) {
                    pv->is_captured = true;
                    bool found = false;
                    for (int c = 0; c < fn->capture_count; c++) {
                        if (strcmp(fn->captures[c].name, name) == 0) { found = true; break; }
                    }
                    if (!found) {
                        fn->captures = (VarSym*)realloc(fn->captures, sizeof(VarSym) * (size_t)(fn->capture_count + 1));
                        fn->captures[fn->capture_count++] = *pv;
                        fn->has_captures = true;
                    }
                    if (out_owner_fn) *out_owner_fn = pf;
                    return pv;
                }
            }
        }
    }
    return find_global_in_mod(mod, name);
}

static void bind_var(ModuleInfo* mod, FuncInfo* fn, const char* name, Type* type, SrcLoc loc) {
    if (!fn) {
        VarSym* gv = find_global_in_mod(mod, name);
        if (gv) {
            if (!type_assignable(gv->type, type)) {
                error_at(loc, "cannot assign '%s' to global '%s' of type '%s'", type_str(type), name, type_str(gv->type));
            }
            return;
        }
        add_global_in_mod(mod, name, type, loc);
        return;
    }
    for (int i = 0; i < fn->global_decl_count; i++) {
        if (strcmp(fn->global_decls[i], name) == 0) {
            VarSym* gv = find_global_in_mod(mod, name);
            if (!gv) error_at(loc, "global '%s' is not defined at module scope", name);
            if (!type_assignable(gv->type, type)) {
                error_at(loc, "cannot assign '%s' to global '%s' of type '%s'", type_str(type), name, type_str(gv->type));
            }
            return;
        }
    }
    for (int i = 0; i < fn->nonlocal_decl_count; i++) {
        if (strcmp(fn->nonlocal_decls[i], name) == 0) {
            FuncInfo* owner = NULL;
            VarSym* pv = resolve_var(mod, fn, name, &owner);
            if (!pv || !owner) error_at(loc, "no binding for nonlocal '%s' found", name);
            if (!type_assignable(pv->type, type)) {
                error_at(loc, "cannot assign '%s' to nonlocal '%s' of type '%s'", type_str(type), name, type_str(pv->type));
            }
            return;
        }
    }
    VarSym* lv = find_local_in_fn(fn, name);
    if (lv) {
        if (!type_assignable(lv->type, type)) {
            error_at(loc, "cannot assign '%s' to variable '%s' of type '%s'", type_str(type), name, type_str(lv->type));
        }
        return;
    }
    add_local_in_fn(fn, name, type, false, loc);
}

static bool is_builtin_fn_name(const char* n) {
    static const char* builtins[] = {
        "print", "len", "str", "repr", "int", "float", "bool", "ord", "chr",
        "abs", "round", "pow", "hex", "bin", "oct", "min", "max", "sum",
        "all", "any", "sorted", "reversed", "enumerate", "zip", "range",
        "open", "input", "isinstance", "list", "set", "dict", "tuple",
        "iter", "next", "super", "map", "filter", "bigint",
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
        NULL
    };
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(n, builtins[i]) == 0) return true;
    }
    return false;
}

static Type* iter_element_type(Type* it, SrcLoc loc) {
    if (it->kind == TY_LIST || it->kind == TY_SET) return it->elem;
    if (it->kind == TY_DICT) return it->key;
    if (it->kind == TY_STR) return g_ty_str;
    if (it->kind == TY_BYTES) return g_ty_i64;
    if (it->kind == TY_TUPLE) {
        if (it->tuple_count == 0) error_at(loc, "cannot iterate empty tuple");
        for (int i = 1; i < it->tuple_count; i++) {
            if (!type_eq(it->tuple_elems[0], it->tuple_elems[i])) {
                error_at(loc, "cannot iterate heterogeneous tuple '%s'", type_str(it));
            }
        }
        return it->tuple_elems[0];
    }
    error_at(loc, "type '%s' is not iterable", type_str(it));
    return g_ty_void;
}

static void check_comp_gens(ModuleInfo* mod, FuncInfo* fn, CompGen* gens, int gen_count) {
    for (int g = 0; g < gen_count; g++) {
        CompGen* cg = &gens[g];
        if (cg->iter->kind == EXPR_CALL && cg->iter->call.callee->kind == EXPR_VAR &&
            strcmp(cg->iter->call.callee->var.name, "range") == 0) {
            if (cg->iter->call.arg_count < 1 || cg->iter->call.arg_count > 3) {
                error_at(cg->iter->loc, "range() takes 1 to 3 arguments");
            }
            for (int a = 0; a < cg->iter->call.arg_count; a++) {
                Type* at = check_expr(mod, fn, cg->iter->call.args[a], g_ty_i64);
                if (!type_is_int(at)) error_at(cg->iter->call.args[a]->loc, "range() arguments must be integers");
            }
            cg->iter->ty = make_list_type(g_ty_i64);
            if (cg->target_count != 1) error_at(cg->iter->loc, "expected 1 loop target for range()");
            bind_var(mod, fn, cg->targets[0], g_ty_i64, cg->iter->loc);
        } else {
            Type* it_ty = check_expr(mod, fn, cg->iter, NULL);
            if (cg->target_count == 1) {
                Type* el = iter_element_type(it_ty, cg->iter->loc);
                bind_var(mod, fn, cg->targets[0], el, cg->iter->loc);
            } else if (cg->target_count == 2 && it_ty->kind == TY_LIST &&
                       it_ty->elem->kind == TY_TUPLE && it_ty->elem->tuple_count == 2) {
                bind_var(mod, fn, cg->targets[0], it_ty->elem->tuple_elems[0], cg->iter->loc);
                bind_var(mod, fn, cg->targets[1], it_ty->elem->tuple_elems[1], cg->iter->loc);
            } else {
                error_at(cg->iter->loc, "cannot unpack iterable of type '%s'", type_str(it_ty));
            }
        }
        for (int f = 0; f < cg->if_count; f++) {
            check_expr(mod, fn, cg->ifs[f], g_ty_bool);
        }
    }
}

static Type* check_method_call(ModuleInfo* mod, FuncInfo* fn, Expr* call, Expr* obj_e, const char* mname, Type* obj_ty, Type* hint) {
    (void)hint;
    int argc = call->call.arg_count;
    Expr** args = call->call.args;

    if (obj_ty->kind == TY_STR) {
        if (strcmp(mname, "split") == 0) {
            if (argc > 1) error_at(call->loc, "str.split() takes 0 or 1 arguments");
            if (argc == 1) {
                Type* st = check_expr(mod, fn, args[0], g_ty_str);
                if (st->kind != TY_STR) error_at(args[0]->loc, "str.split() separator must be str");
            }
            return make_list_type(g_ty_str);
        }
        if (strcmp(mname, "join") == 0) {
            if (argc != 1) error_at(call->loc, "str.join() takes 1 argument");
            Type* lt = check_expr(mod, fn, args[0], make_list_type(g_ty_str));
            if (lt->kind != TY_LIST || lt->elem->kind != TY_STR) error_at(args[0]->loc, "str.join() requires list[str]");
            return g_ty_str;
        }
        if (strcmp(mname, "upper") == 0 || strcmp(mname, "lower") == 0 ||
            strcmp(mname, "strip") == 0 || strcmp(mname, "lstrip") == 0 || strcmp(mname, "rstrip") == 0) {
            if (argc != 0) error_at(call->loc, "str.%s() takes 0 arguments", mname);
            return g_ty_str;
        }
        if (strcmp(mname, "replace") == 0) {
            if (argc != 2) error_at(call->loc, "str.replace() takes 2 arguments");
            if (check_expr(mod, fn, args[0], g_ty_str)->kind != TY_STR ||
                check_expr(mod, fn, args[1], g_ty_str)->kind != TY_STR) {
                error_at(call->loc, "str.replace() requires str arguments");
            }
            return g_ty_str;
        }
        if (strcmp(mname, "find") == 0 || strcmp(mname, "index") == 0 || strcmp(mname, "count") == 0) {
            if (argc != 1) error_at(call->loc, "str.%s() takes 1 argument", mname);
            if (check_expr(mod, fn, args[0], g_ty_str)->kind != TY_STR) error_at(args[0]->loc, "expected str");
            return g_ty_i64;
        }
        if (strcmp(mname, "startswith") == 0 || strcmp(mname, "endswith") == 0) {
            if (argc != 1) error_at(call->loc, "str.%s() takes 1 argument", mname);
            if (check_expr(mod, fn, args[0], g_ty_str)->kind != TY_STR) error_at(args[0]->loc, "expected str");
            return g_ty_bool;
        }
        if (strcmp(mname, "format") == 0) {
            for (int i = 0; i < argc; i++) check_expr(mod, fn, args[i], NULL);
            return g_ty_str;
        }
        error_at(call->loc, "unknown str method '%s'", mname);
    }

    if (obj_ty->kind == TY_FILE) {
        if (strcmp(mname, "read") == 0) {
            if (argc != 0) error_at(call->loc, "File.read() takes 0 arguments");
            return g_ty_str;
        }
        if (strcmp(mname, "write") == 0) {
            if (argc != 1) error_at(call->loc, "File.write() takes 1 argument");
            if (check_expr(mod, fn, args[0], g_ty_str)->kind != TY_STR) error_at(args[0]->loc, "expected str");
            return g_ty_void;
        }
        if (strcmp(mname, "close") == 0) {
            if (argc != 0) error_at(call->loc, "File.close() takes 0 arguments");
            return g_ty_void;
        }
        error_at(call->loc, "unknown File method '%s'", mname);
    }

    if (obj_ty->kind == TY_LIST) {
        Type* el = obj_ty->elem;
        if (strcmp(mname, "append") == 0 || strcmp(mname, "remove") == 0) {
            if (argc != 1) error_at(call->loc, "list.%s() takes 1 argument", mname);
            Type* at = check_expr(mod, fn, args[0], el);
            if (!type_assignable(el, at)) error_at(args[0]->loc, "expected '%s', got '%s'", type_str(el), type_str(at));
            return g_ty_void;
        }
        if (strcmp(mname, "extend") == 0) {
            if (argc != 1) error_at(call->loc, "list.extend() takes 1 argument");
            Type* at = check_expr(mod, fn, args[0], obj_ty);
            if (!type_eq(at, obj_ty)) error_at(args[0]->loc, "expected '%s', got '%s'", type_str(obj_ty), type_str(at));
            return g_ty_void;
        }
        if (strcmp(mname, "insert") == 0) {
            if (argc != 2) error_at(call->loc, "list.insert() takes 2 arguments");
            if (!type_is_int(check_expr(mod, fn, args[0], g_ty_i64))) error_at(args[0]->loc, "index must be int");
            Type* vt = check_expr(mod, fn, args[1], el);
            if (!type_assignable(el, vt)) error_at(args[1]->loc, "expected '%s', got '%s'", type_str(el), type_str(vt));
            return g_ty_void;
        }
        if (strcmp(mname, "pop") == 0) {
            if (argc > 1) error_at(call->loc, "list.pop() takes 0 or 1 arguments");
            if (argc == 1 && !type_is_int(check_expr(mod, fn, args[0], g_ty_i64))) error_at(args[0]->loc, "index must be int");
            return el;
        }
        if (strcmp(mname, "index") == 0 || strcmp(mname, "count") == 0) {
            if (argc != 1) error_at(call->loc, "list.%s() takes 1 argument", mname);
            Type* at = check_expr(mod, fn, args[0], el);
            if (!type_assignable(el, at)) error_at(args[0]->loc, "expected '%s', got '%s'", type_str(el), type_str(at));
            return g_ty_i64;
        }
        if (strcmp(mname, "sort") == 0 || strcmp(mname, "reverse") == 0 || strcmp(mname, "clear") == 0) {
            if (argc != 0) error_at(call->loc, "list.%s() takes 0 arguments", mname);
            return g_ty_void;
        }
        if (strcmp(mname, "copy") == 0) {
            if (argc != 0) error_at(call->loc, "list.copy() takes 0 arguments");
            return obj_ty;
        }
        error_at(call->loc, "unknown list method '%s'", mname);
    }

    if (obj_ty->kind == TY_SET) {
        Type* el = obj_ty->elem;
        if (strcmp(mname, "add") == 0 || strcmp(mname, "remove") == 0 || strcmp(mname, "discard") == 0) {
            if (argc != 1) error_at(call->loc, "set.%s() takes 1 argument", mname);
            Type* at = check_expr(mod, fn, args[0], el);
            if (!type_assignable(el, at)) error_at(args[0]->loc, "expected '%s', got '%s'", type_str(el), type_str(at));
            return g_ty_void;
        }
        if (strcmp(mname, "pop") == 0) { if (argc != 0) error_at(call->loc, "set.pop() takes 0 args"); return el; }
        if (strcmp(mname, "clear") == 0) { if (argc != 0) error_at(call->loc, "set.clear() takes 0 args"); return g_ty_void; }
        if (strcmp(mname, "copy") == 0) { if (argc != 0) error_at(call->loc, "set.copy() takes 0 args"); return obj_ty; }
        if (strcmp(mname, "union") == 0 || strcmp(mname, "intersection") == 0 ||
            strcmp(mname, "difference") == 0 || strcmp(mname, "symmetric_difference") == 0) {
            if (argc != 1) error_at(call->loc, "set.%s() takes 1 argument", mname);
            Type* at = check_expr(mod, fn, args[0], obj_ty);
            if (!type_eq(at, obj_ty)) error_at(args[0]->loc, "expected '%s', got '%s'", type_str(obj_ty), type_str(at));
            return obj_ty;
        }
        if (strcmp(mname, "update") == 0 || strcmp(mname, "intersection_update") == 0 ||
            strcmp(mname, "difference_update") == 0 || strcmp(mname, "symmetric_difference_update") == 0) {
            if (argc != 1) error_at(call->loc, "set.%s() takes 1 argument", mname);
            Type* at = check_expr(mod, fn, args[0], obj_ty);
            if (!type_eq(at, obj_ty)) error_at(args[0]->loc, "expected '%s', got '%s'", type_str(obj_ty), type_str(at));
            return g_ty_void;
        }
        if (strcmp(mname, "issubset") == 0 || strcmp(mname, "issuperset") == 0 || strcmp(mname, "isdisjoint") == 0) {
            if (argc != 1) error_at(call->loc, "set.%s() takes 1 argument", mname);
            Type* at = check_expr(mod, fn, args[0], obj_ty);
            if (!type_eq(at, obj_ty)) error_at(args[0]->loc, "expected '%s', got '%s'", type_str(obj_ty), type_str(at));
            return g_ty_bool;
        }
        error_at(call->loc, "unknown set method '%s'", mname);
    }

    if (obj_ty->kind == TY_DICT) {
        Type* kt = obj_ty->key;
        Type* vt = obj_ty->val;
        if (strcmp(mname, "keys") == 0) { if (argc != 0) error_at(call->loc, "dict.keys() takes 0 args"); return make_list_type(kt); }
        if (strcmp(mname, "values") == 0) { if (argc != 0) error_at(call->loc, "dict.values() takes 0 args"); return make_list_type(vt); }
        if (strcmp(mname, "items") == 0) {
            if (argc != 0) error_at(call->loc, "dict.items() takes 0 args");
            Type* pair[2] = {kt, vt};
            return make_list_type(make_tuple_type(pair, 2));
        }
        if (strcmp(mname, "get") == 0 || strcmp(mname, "setdefault") == 0) {
            if (argc != 2) error_at(call->loc, "dict.%s() requires key and default", mname);
            Type* ka = check_expr(mod, fn, args[0], kt);
            Type* va = check_expr(mod, fn, args[1], vt);
            if (!type_assignable(kt, ka)) error_at(args[0]->loc, "dict key type mismatch");
            if (!type_assignable(vt, va)) error_at(args[1]->loc, "dict default value type mismatch");
            return vt;
        }
        if (strcmp(mname, "pop") == 0) {
            if (argc != 1) error_at(call->loc, "dict.pop() takes 1 argument");
            Type* ka = check_expr(mod, fn, args[0], kt);
            if (!type_assignable(kt, ka)) error_at(args[0]->loc, "dict key type mismatch");
            return vt;
        }
        if (strcmp(mname, "popitem") == 0) {
            if (argc != 0) error_at(call->loc, "dict.popitem() takes 0 arguments");
            Type* pair[2] = {kt, vt};
            return make_tuple_type(pair, 2);
        }
        if (strcmp(mname, "update") == 0) {
            if (argc != 1) error_at(call->loc, "dict.update() takes 1 argument");
            Type* ot = check_expr(mod, fn, args[0], obj_ty);
            if (!type_eq(ot, obj_ty)) error_at(args[0]->loc, "expected '%s', got '%s'", type_str(obj_ty), type_str(ot));
            return g_ty_void;
        }
        if (strcmp(mname, "clear") == 0) { if (argc != 0) error_at(call->loc, "dict.clear() takes 0 args"); return g_ty_void; }
        if (strcmp(mname, "copy") == 0) { if (argc != 0) error_at(call->loc, "dict.copy() takes 0 args"); return obj_ty; }
        error_at(call->loc, "unknown dict method '%s'", mname);
    }

    if (obj_ty->kind == TY_CLASS) {
        ClassInfo* cls = obj_ty->cls;
        if (obj_e->kind == EXPR_CALL && obj_e->call.callee->kind == EXPR_VAR &&
            strcmp(obj_e->call.callee->var.name, "super") == 0) {
            if (!fn || !fn->owner_cls) error_at(call->loc, "super() used outside a class method");
            ClassInfo* cur_cls = fn->owner_cls;
            if (obj_e->call.arg_count == 2 && obj_e->call.args[0]->kind == EXPR_VAR) {
                ClassInfo* sc = find_class_in_mod(mod, obj_e->call.args[0]->var.name);
                if (sc) cur_cls = sc;
            }
            ClassInfo* self_cls = fn->owner_cls;
            int idx = -1;
            for (int i = 0; i < self_cls->mro_count; i++) {
                if (self_cls->mro[i] == cur_cls) { idx = i; break; }
            }
            FuncInfo* target_m = NULL;
            for (int i = idx + 1; i < self_cls->mro_count; i++) {
                target_m = find_method_in_class_exact(self_cls->mro[i], mname);
                if (target_m) break;
            }
            if (!target_m) error_at(call->loc, "super() has no method '%s'", mname);
            int expected_args = target_m->param_count - 1;
            if (argc != expected_args) error_at(call->loc, "super().%s() expects %d arguments, got %d", mname, expected_args, argc);
            for (int i = 0; i < argc; i++) {
                Type* at = check_expr(mod, fn, args[i], target_m->param_types[i + 1]);
                if (!type_assignable(target_m->param_types[i + 1], at)) {
                    error_at(args[i]->loc, "argument %d of super().%s(): expected '%s', got '%s'",
                             i + 1, mname, type_str(target_m->param_types[i + 1]), type_str(at));
                }
            }
            return target_m->ret_type;
        }

        FuncInfo* m = find_method_in_class(cls, mname);
        if (!m) error_at(call->loc, "class '%s' has no method '%s'", cls->name, mname);
        int start_p = m->is_static ? 0 : 1;
        int max_args = m->param_count - start_p;
        int min_args = max_args;
        for (int i = m->param_count - 1; i >= start_p; i--) {
            if (m->param_defaults[i]) min_args--;
            else break;
        }
        if (argc < min_args || argc > max_args) {
            error_at(call->loc, "method '%s.%s()' expects %d..%d arguments, got %d", cls->name, mname, min_args, max_args, argc);
        }
        for (int i = 0; i < argc; i++) {
            Type* pt = m->param_types[i + start_p];
            Type* at = check_expr(mod, fn, args[i], pt);
            if (!type_assignable(pt, at)) {
                error_at(args[i]->loc, "argument %d of '%s.%s()': expected '%s', got '%s'",
                         i + 1, cls->name, mname, type_str(pt), type_str(at));
            }
        }
        return m->ret_type;
    }

    if (obj_ty->kind == TY_MODULE) {
        ModuleInfo* mmod = obj_ty->mod;
        FuncInfo* mf = find_func_in_mod(mmod, mname);
        if (mf) {
            if (argc != mf->param_count) error_at(call->loc, "%s.%s() expects %d args, got %d", mmod->name, mname, mf->param_count, argc);
            for (int i = 0; i < argc; i++) {
                Type* at = check_expr(mod, fn, args[i], mf->param_types[i]);
                if (!type_assignable(mf->param_types[i], at)) error_at(args[i]->loc, "argument type mismatch");
            }
            return mf->ret_type;
        }
        ClassInfo* mci = find_class_in_mod(mmod, mname);
        if (mci) {
            FuncInfo* init_fn = find_method_in_class(mci, "__init__");
            if (init_fn) {
                int max_a = init_fn->param_count - 1;
                for (int i = 0; i < argc && i < max_a; i++) check_expr(mod, fn, args[i], init_fn->param_types[i + 1]);
            }
            return mci->type;
        }
        error_at(call->loc, "module '%s' has no function '%s'", mmod->name, mname);
    }

    error_at(call->loc, "cannot call method '%s' on type '%s'", mname, type_str(obj_ty));
    return g_ty_void;
}

static Type* check_expr(ModuleInfo* mod, FuncInfo* fn, Expr* e, Type* hint) {
    if (!e) return g_ty_void;
    Type* res = NULL;

    switch (e->kind) {
        case EXPR_INT:
            if (e->int_lit.is_big) {
                if (hint && hint->kind == TY_BIGINT) res = g_ty_bigint;
                else error_at(e->loc, "integer literal exceeds 64-bit signed range; annotate as 'bigint'");
            } else if (hint && type_is_int(hint)) {
                res = hint;
            } else if (hint && type_is_float(hint)) {
                res = hint;
            } else if (hint && hint->kind == TY_BIGINT) {
                res = g_ty_bigint;
            } else {
                res = g_ty_i64;
            }
            break;
        case EXPR_FLOAT:
            res = (hint && hint->kind == TY_F32) ? g_ty_f32 : g_ty_f64;
            break;
        case EXPR_BOOL:
            res = g_ty_bool;
            break;
        case EXPR_NONE:
            if (hint && hint->kind == TY_OPTIONAL) res = hint;
            else res = g_ty_none_lit;
            break;
        case EXPR_STR:
            res = g_ty_str;
            break;
        case EXPR_BYTES:
            res = g_ty_bytes;
            break;
        case EXPR_FSTRING:
            for (int i = 0; i < e->fstr.count; i++) {
                if (e->fstr.parts[i].is_expr) {
                    check_expr(mod, fn, e->fstr.parts[i].expr, NULL);
                }
            }
            res = g_ty_str;
            break;
        case EXPR_VAR: {
            const char* n = e->var.name;
            if (strcmp(n, "self") == 0) {
                if (!fn || !fn->owner_cls || fn->is_static) error_at(e->loc, "'self' used outside instance method");
                res = fn->owner_cls->type;
                break;
            }
            if (strcmp(n, "cls") == 0 && fn && fn->is_classmethod) {
                res = fn->owner_cls->type;
                break;
            }
            VarSym* v = resolve_var(mod, fn, n, NULL);
            if (v) { res = v->type; break; }
            ClassInfo* ci = find_class_in_mod(mod, n);
            if (ci) { res = ci->type; break; }
            FuncInfo* fi = find_func_in_mod(mod, n);
            if (fi) { res = fi->type; break; }
            if (is_builtin_fn_name(n)) { res = g_ty_void; break; }
            error_at(e->loc, "undefined identifier '%s'", n);
            break;
        }
        case EXPR_WALRUS: {
            Type* vt = check_expr(mod, fn, e->walrus.val, hint);
            bind_var(mod, fn, e->walrus.name, vt, e->loc);
            res = vt;
            break;
        }
        case EXPR_UNOP: {
            const char* op = e->unop.op;
            if (strcmp(op, "not") == 0) {
                check_expr(mod, fn, e->unop.operand, NULL);
                res = g_ty_bool;
            } else if (strcmp(op, "-") == 0 || strcmp(op, "+") == 0) {
                Type* ot = check_expr(mod, fn, e->unop.operand, hint);
                if (!type_is_numeric(ot) && ot->kind != TY_BIGINT && ot->kind != TY_BOOL) {
                    error_at(e->loc, "unary '%s' invalid for type '%s'", op, type_str(ot));
                }
                res = (ot->kind == TY_BOOL) ? g_ty_i64 : ot;
            } else if (strcmp(op, "~") == 0) {
                Type* ot = check_expr(mod, fn, e->unop.operand, hint);
                if (!type_is_int(ot)) error_at(e->loc, "bitwise '~' requires integer type, got '%s'", type_str(ot));
                res = ot;
            }
            break;
        }
        case EXPR_BINOP: {
            const char* op = e->binop.op;
            if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
                Type* lt = check_expr(mod, fn, e->binop.left, NULL);
                Type* rt = check_expr(mod, fn, e->binop.right, lt);
                if (type_eq(lt, rt) && lt->kind != TY_BOOL) {
                    res = (hint && hint->kind == TY_BOOL) ? g_ty_bool : lt;
                } else {
                    res = g_ty_bool;
                }
                break;
            }
            if (strcmp(op, "is") == 0 || strcmp(op, "is not") == 0) {
                Type* lt = check_expr(mod, fn, e->binop.left, NULL);
                check_expr(mod, fn, e->binop.right, lt);
                res = g_ty_bool;
                break;
            }
            if (strcmp(op, "in") == 0 || strcmp(op, "not in") == 0) {
                Type* rt = check_expr(mod, fn, e->binop.right, NULL);
                Type* expected_k = NULL;
                if (rt->kind == TY_LIST || rt->kind == TY_SET) expected_k = rt->elem;
                else if (rt->kind == TY_DICT) expected_k = rt->key;
                else if (rt->kind == TY_STR) expected_k = g_ty_str;
                else if (rt->kind == TY_TUPLE && rt->tuple_count > 0) expected_k = rt->tuple_elems[0];
                else error_at(e->loc, "'%s' unsupported for right operand type '%s'", op, type_str(rt));
                Type* lt = check_expr(mod, fn, e->binop.left, expected_k);
                if (!type_assignable(expected_k, lt)) {
                    error_at(e->loc, "element type '%s' incompatible with container '%s'", type_str(lt), type_str(rt));
                }
                res = g_ty_bool;
                break;
            }
            if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
                strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
                Type* lt = check_expr(mod, fn, e->binop.left, NULL);
                Type* rt = check_expr(mod, fn, e->binop.right, lt);
                if (lt->kind == TY_CLASS) {
                    const char* dunder = NULL;
                    if (strcmp(op, "==") == 0) dunder = "__eq__";
                    else if (strcmp(op, "!=") == 0) dunder = "__ne__";
                    else if (strcmp(op, "<") == 0) dunder = "__lt__";
                    else if (strcmp(op, "<=") == 0) dunder = "__le__";
                    else if (strcmp(op, ">") == 0) dunder = "__gt__";
                    else if (strcmp(op, ">=") == 0) dunder = "__ge__";
                    if (dunder && find_method_in_class(lt->cls, dunder)) {
                        res = g_ty_bool;
                        break;
                    }
                }
                if (lt->kind == TY_OPTIONAL && rt->kind == TY_NONE_LIT) { res = g_ty_bool; break; }
                if (rt->kind == TY_OPTIONAL && lt->kind == TY_NONE_LIT) { res = g_ty_bool; break; }
                if (type_is_numeric(lt) && type_is_numeric(rt)) { res = g_ty_bool; break; }
                if (!type_assignable(lt, rt) && !type_assignable(rt, lt)) {
                    error_at(e->loc, "cannot compare '%s' and '%s'", type_str(lt), type_str(rt));
                }
                res = g_ty_bool;
                break;
            }

            Type* lt = check_expr(mod, fn, e->binop.left, hint);
            Type* rt = check_expr(mod, fn, e->binop.right, lt);

            if (lt->kind == TY_CLASS) {
                const char* dunder = NULL;
                if (strcmp(op, "+") == 0) dunder = "__add__";
                else if (strcmp(op, "-") == 0) dunder = "__sub__";
                else if (strcmp(op, "*") == 0) dunder = "__mul__";
                if (dunder) {
                    FuncInfo* dm = find_method_in_class(lt->cls, dunder);
                    if (dm) {
                        if (!type_assignable(dm->param_types[1], rt)) {
                            error_at(e->loc, "operator '%s' expected '%s', got '%s'", op, type_str(dm->param_types[1]), type_str(rt));
                        }
                        res = dm->ret_type;
                        break;
                    }
                }
            }
            if (lt->kind == TY_STR && rt->kind == TY_STR && strcmp(op, "+") == 0) { res = g_ty_str; break; }
            if (lt->kind == TY_STR && type_is_int(rt) && strcmp(op, "*") == 0) { res = g_ty_str; break; }
            if (lt->kind == TY_LIST && type_eq(lt, rt) && strcmp(op, "+") == 0) { res = lt; break; }
            if (lt->kind == TY_LIST && type_is_int(rt) && strcmp(op, "*") == 0) { res = lt; break; }
            if (lt->kind == TY_SET && type_eq(lt, rt) &&
                (strcmp(op, "|") == 0 || strcmp(op, "&") == 0 || strcmp(op, "-") == 0 || strcmp(op, "^") == 0)) {
                res = lt; break;
            }
            if (lt->kind == TY_DICT && type_eq(lt, rt) && strcmp(op, "|") == 0) { res = lt; break; }
            if (strcmp(op, "@") == 0 && lt->kind == TY_LIST && lt->elem->kind == TY_LIST &&
                rt->kind == TY_LIST && rt->elem->kind == TY_LIST) {
                res = lt; break;
            }
            if (lt->kind == TY_BIGINT || rt->kind == TY_BIGINT) {
                if (lt->kind != TY_BIGINT || rt->kind != TY_BIGINT) {
                    error_at(e->loc, "bigint arithmetic requires both operands to be bigint");
                }
                res = g_ty_bigint;
                break;
            }
            if (!type_is_numeric(lt) && lt->kind != TY_BOOL) {
                error_at(e->loc, "operator '%s' unsupported for '%s' and '%s'", op, type_str(lt), type_str(rt));
            }
            if (!type_is_numeric(rt) && rt->kind != TY_BOOL) {
                error_at(e->loc, "operator '%s' unsupported for '%s' and '%s'", op, type_str(lt), type_str(rt));
            }
            if (strcmp(op, "/") == 0) { res = g_ty_f64; break; }
            if (strcmp(op, "&") == 0 || strcmp(op, "|") == 0 || strcmp(op, "^") == 0 ||
                strcmp(op, "<<") == 0 || strcmp(op, ">>") == 0) {
                if (!type_is_int(lt) || !type_is_int(rt)) {
                    error_at(e->loc, "bitwise operator '%s' requires integers", op);
                }
                res = numeric_common_type(lt, rt);
                break;
            }
            res = numeric_common_type(lt, rt);
            break;
        }
        case EXPR_TERNARY: {
            check_expr(mod, fn, e->ternary.cond, g_ty_bool);
            Type* tt = check_expr(mod, fn, e->ternary.then_e, hint);
            Type* et = check_expr(mod, fn, e->ternary.else_e, tt);
            if (type_eq(tt, et)) res = tt;
            else if (type_is_numeric(tt) && type_is_numeric(et)) res = numeric_common_type(tt, et);
            else if (tt->kind == TY_OPTIONAL && et->kind == TY_NONE_LIT) res = tt;
            else if (et->kind == TY_OPTIONAL && tt->kind == TY_NONE_LIT) res = et;
            else if (tt->kind != TY_NONE_LIT && et->kind == TY_NONE_LIT) res = make_optional_type(tt);
            else if (et->kind != TY_NONE_LIT && tt->kind == TY_NONE_LIT) res = make_optional_type(et);
            else error_at(e->loc, "ternary branches have incompatible types '%s' and '%s'", type_str(tt), type_str(et));
            break;
        }
        case EXPR_LIST: {
            if (e->seq.count == 0) {
                if (hint && hint->kind == TY_LIST) { res = hint; break; }
                error_at(e->loc, "cannot infer type of empty list '[]'; add a type annotation like list[int]");
            }
            Type* ehint = (hint && hint->kind == TY_LIST) ? hint->elem : NULL;
            Type* first = check_expr(mod, fn, e->seq.items[0], ehint);
            for (int i = 1; i < e->seq.count; i++) {
                Type* it = check_expr(mod, fn, e->seq.items[i], first);
                if (!type_assignable(first, it)) {
                    if (type_assignable(it, first)) first = it;
                    else error_at(e->seq.items[i]->loc, "list element type '%s' does not match '%s'", type_str(it), type_str(first));
                }
            }
            res = make_list_type(first);
            break;
        }
        case EXPR_TUPLE: {
            Type** ts = e->seq.count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)e->seq.count) : NULL;
            for (int i = 0; i < e->seq.count; i++) {
                Type* eh = (hint && hint->kind == TY_TUPLE && i < hint->tuple_count) ? hint->tuple_elems[i] : NULL;
                ts[i] = check_expr(mod, fn, e->seq.items[i], eh);
            }
            res = make_tuple_type(ts, e->seq.count);
            break;
        }
        case EXPR_SET: {
            Type* ehint = (hint && hint->kind == TY_SET) ? hint->elem : NULL;
            Type* first = check_expr(mod, fn, e->seq.items[0], ehint);
            for (int i = 1; i < e->seq.count; i++) {
                Type* it = check_expr(mod, fn, e->seq.items[i], first);
                if (!type_assignable(first, it)) error_at(e->seq.items[i]->loc, "set element type mismatch");
            }
            res = make_set_type(first);
            break;
        }
        case EXPR_DICT: {
            if (e->dict.count == 0) {
                if (hint && hint->kind == TY_DICT) { res = hint; break; }
                error_at(e->loc, "cannot infer type of empty dict '{}'; add a type annotation like dict[str, int]");
            }
            Type* kh = (hint && hint->kind == TY_DICT) ? hint->key : NULL;
            Type* vh = (hint && hint->kind == TY_DICT) ? hint->val : NULL;
            Type* kt = check_expr(mod, fn, e->dict.keys[0], kh);
            Type* vt = check_expr(mod, fn, e->dict.vals[0], vh);
            for (int i = 1; i < e->dict.count; i++) {
                Type* k2 = check_expr(mod, fn, e->dict.keys[i], kt);
                Type* v2 = check_expr(mod, fn, e->dict.vals[i], vt);
                if (!type_assignable(kt, k2)) error_at(e->dict.keys[i]->loc, "dict key type mismatch");
                if (!type_assignable(vt, v2)) error_at(e->dict.vals[i]->loc, "dict value type mismatch");
            }
            res = make_dict_type(kt, vt);
            break;
        }
        case EXPR_LISTCOMP:
        case EXPR_GENEXPR: {
            check_comp_gens(mod, fn, e->comp.gens, e->comp.gen_count);
            Type* eh = (hint && hint->kind == TY_LIST) ? hint->elem : NULL;
            Type* et = check_expr(mod, fn, e->comp.elt, eh);
            res = make_list_type(et);
            break;
        }
        case EXPR_SETCOMP: {
            check_comp_gens(mod, fn, e->comp.gens, e->comp.gen_count);
            Type* eh = (hint && hint->kind == TY_SET) ? hint->elem : NULL;
            Type* et = check_expr(mod, fn, e->comp.elt, eh);
            res = make_set_type(et);
            break;
        }
        case EXPR_DICTCOMP: {
            check_comp_gens(mod, fn, e->comp.gens, e->comp.gen_count);
            Type* kh = (hint && hint->kind == TY_DICT) ? hint->key : NULL;
            Type* vh = (hint && hint->kind == TY_DICT) ? hint->val : NULL;
            Type* kt = check_expr(mod, fn, e->comp.elt, kh);
            Type* vt = check_expr(mod, fn, e->comp.val, vh);
            res = make_dict_type(kt, vt);
            break;
        }
        case EXPR_INDEX: {
            Type* ot = check_expr(mod, fn, e->index.obj, NULL);
            if (ot->kind == TY_LIST) {
                Type* it = check_expr(mod, fn, e->index.index, g_ty_i64);
                if (!type_is_int(it)) error_at(e->index.index->loc, "list index must be int");
                res = ot->elem;
            } else if (ot->kind == TY_STR) {
                Type* it = check_expr(mod, fn, e->index.index, g_ty_i64);
                if (!type_is_int(it)) error_at(e->index.index->loc, "str index must be int");
                res = g_ty_str;
            } else if (ot->kind == TY_BYTES) {
                Type* it = check_expr(mod, fn, e->index.index, g_ty_i64);
                if (!type_is_int(it)) error_at(e->index.index->loc, "bytes index must be int");
                res = g_ty_i64;
            } else if (ot->kind == TY_DICT) {
                Type* kt = check_expr(mod, fn, e->index.index, ot->key);
                if (!type_assignable(ot->key, kt)) error_at(e->index.index->loc, "dict key type mismatch");
                res = ot->val;
            } else if (ot->kind == TY_TUPLE) {
                if (e->index.index->kind != EXPR_INT) error_at(e->index.index->loc, "tuple index must be a constant integer");
                int idx = (int)e->index.index->int_lit.ival;
                if (idx < 0) idx += ot->tuple_count;
                if (idx < 0 || idx >= ot->tuple_count) error_at(e->index.index->loc, "tuple index out of range");
                res = ot->tuple_elems[idx];
            } else {
                error_at(e->loc, "type '%s' is not subscriptable", type_str(ot));
            }
            break;
        }
        case EXPR_SLICE: {
            Type* ot = check_expr(mod, fn, e->slice.obj, NULL);
            if (e->slice.start && !type_is_int(check_expr(mod, fn, e->slice.start, g_ty_i64))) {
                error_at(e->slice.start->loc, "slice start must be int");
            }
            if (e->slice.stop && !type_is_int(check_expr(mod, fn, e->slice.stop, g_ty_i64))) {
                error_at(e->slice.stop->loc, "slice stop must be int");
            }
            if (ot->kind != TY_LIST && ot->kind != TY_STR) error_at(e->loc, "cannot slice type '%s'", type_str(ot));
            res = ot;
            break;
        }
        case EXPR_ATTR: {
            Type* ot = check_expr(mod, fn, e->attr.obj, NULL);
            const char* an = e->attr.attr;
            if (ot->kind == TY_OPTIONAL) {
                error_at(e->loc, "cannot access attribute '%s' on optional type '%s'; narrow with 'is not None' or check", an, type_str(ot));
            }
            if (ot->kind == TY_CLASS) {
                ClassInfo* cls = ot->cls;
                FieldInfo* fi = find_field_in_class(cls, an);
                if (fi) { res = fi->type; break; }
                FieldInfo* ca = find_class_attr(cls, an);
                if (ca) { res = ca->type; break; }
                FuncInfo* prop = find_method_in_class(cls, an);
                if (prop && prop->is_property) { res = prop->ret_type; break; }
                error_at(e->loc, "class '%s' has no attribute '%s'", cls->name, an);
            } else if (ot->kind == TY_MODULE) {
                if (strcmp(ot->mod->name, "sys") == 0 && strcmp(an, "argv") == 0) {
                    res = make_list_type(g_ty_str);
                    break;
                }
                VarSym* gv = find_global_in_mod(ot->mod, an);
                if (gv) { res = gv->type; break; }
                ClassInfo* mci = find_class_in_mod(ot->mod, an);
                if (mci) { res = mci->type; break; }
                FuncInfo* mfi = find_func_in_mod(ot->mod, an);
                if (mfi) { res = mfi->type; break; }
                error_at(e->loc, "module '%s' has no attribute '%s'", ot->mod->name, an);
            } else {
                error_at(e->loc, "type '%s' has no attribute '%s'", type_str(ot), an);
            }
            break;
        }
        case EXPR_LAMBDA: {
            int pc = e->lambda.param_count;
            Type** ptypes = pc ? (Type**)arena_alloc(sizeof(Type*) * (size_t)pc) : NULL;
            FuncInfo* lfn = (FuncInfo*)arena_alloc(sizeof(FuncInfo));
            lfn->name = arena_sprintf("__lambda_%d", g_all_func_count + 1);
            lfn->llvm_name = arena_sprintf("@%s%s", mod->prefix, lfn->name);
            lfn->mod = mod;
            lfn->parent_fn = fn;
            lfn->lambda_ast = e;
            lfn->is_lambda = true;
            lfn->param_count = pc;
            lfn->param_names = e->lambda.params;
            for (int i = 0; i < pc; i++) {
                Type* pt = NULL;
                if (e->lambda.param_types && e->lambda.param_types[i]) {
                    pt = resolve_type_expr(mod, e->lambda.param_types[i]);
                } else if (hint && hint->kind == TY_FUNC && i < hint->func_param_count) {
                    pt = hint->func_params[i];
                } else {
                    error_at(e->loc, "lambda parameter '%s' requires a type annotation", e->lambda.params[i]);
                }
                ptypes[i] = pt;
                add_local_in_fn(lfn, e->lambda.params[i], pt, true, e->loc);
            }
            lfn->param_types = ptypes;
            Type* rh = e->lambda.ret_type ? resolve_type_expr(mod, e->lambda.ret_type)
                                          : ((hint && hint->kind == TY_FUNC) ? hint->func_ret : NULL);
            Type* rty = check_expr(mod, lfn, e->lambda.body, rh);
            if (e->lambda.ret_type && !type_assignable(rh, rty)) {
                error_at(e->loc, "lambda return type '%s' incompatible with annotation '%s'", type_str(rty), type_str(rh));
            }
            lfn->ret_type = e->lambda.ret_type ? rh : rty;
            lfn->type = make_func_type(ptypes, pc, lfn->ret_type);
            g_all_funcs = (FuncInfo**)realloc(g_all_funcs, sizeof(FuncInfo*) * (size_t)(g_all_func_count + 1));
            g_all_funcs[g_all_func_count++] = lfn;
            res = lfn->type;
            break;
        }
        case EXPR_CALL: {
            Expr* callee = e->call.callee;
            int argc = e->call.arg_count;
            Expr** args = e->call.args;

            if (callee->kind == EXPR_ATTR) {
                Type* ot = check_expr(mod, fn, callee->attr.obj, NULL);
                res = check_method_call(mod, fn, e, callee->attr.obj, callee->attr.attr, ot, hint);
                break;
            }
            if (callee->kind == EXPR_VAR) {
                const char* cname = callee->var.name;
                if (strcmp(cname, "print") == 0) {
                    for (int i = 0; i < argc; i++) check_expr(mod, fn, args[i], NULL);
                    res = g_ty_void; break;
                }
                if (strcmp(cname, "len") == 0) {
                    if (argc != 1) error_at(e->loc, "len() takes 1 argument");
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    if (at->kind != TY_LIST && at->kind != TY_SET && at->kind != TY_DICT &&
                        at->kind != TY_STR && at->kind != TY_BYTES && at->kind != TY_TUPLE) {
                        error_at(args[0]->loc, "len() unsupported for type '%s'", type_str(at));
                    }
                    res = g_ty_i64; break;
                }
                if (strcmp(cname, "str") == 0 || strcmp(cname, "repr") == 0) {
                    if (argc != 1) error_at(e->loc, "%s() takes 1 argument", cname);
                    check_expr(mod, fn, args[0], NULL);
                    res = g_ty_str; break;
                }
                if (strcmp(cname, "int") == 0 || strcmp(cname, "i64") == 0 || strcmp(cname, "i32") == 0 ||
                    strcmp(cname, "i16") == 0 || strcmp(cname, "i8") == 0 || strcmp(cname, "u64") == 0 ||
                    strcmp(cname, "u32") == 0 || strcmp(cname, "u16") == 0 || strcmp(cname, "u8") == 0) {
                    if (argc != 1) error_at(e->loc, "%s() takes 1 argument", cname);
                    check_expr(mod, fn, args[0], NULL);
                    TypeExpr tmp_te = {TEXPR_NAME, e->loc, cname, NULL, 0, NULL};
                    res = resolve_type_expr(mod, &tmp_te);
                    break;
                }
                if (strcmp(cname, "float") == 0 || strcmp(cname, "f64") == 0 || strcmp(cname, "f32") == 0) {
                    if (argc != 1) error_at(e->loc, "%s() takes 1 argument", cname);
                    check_expr(mod, fn, args[0], NULL);
                    res = (strcmp(cname, "f32") == 0) ? g_ty_f32 : g_ty_f64;
                    break;
                }
                if (strcmp(cname, "bool") == 0) {
                    if (argc != 1) error_at(e->loc, "bool() takes 1 argument");
                    check_expr(mod, fn, args[0], NULL);
                    res = g_ty_bool; break;
                }
                if (strcmp(cname, "bigint") == 0) {
                    if (argc != 1) error_at(e->loc, "bigint() takes 1 argument");
                    check_expr(mod, fn, args[0], NULL);
                    res = g_ty_bigint; break;
                }
                if (strcmp(cname, "ord") == 0) {
                    if (argc != 1 || check_expr(mod, fn, args[0], g_ty_str)->kind != TY_STR) error_at(e->loc, "ord() expects str");
                    res = g_ty_i64; break;
                }
                if (strcmp(cname, "chr") == 0) {
                    if (argc != 1 || !type_is_int(check_expr(mod, fn, args[0], g_ty_i64))) error_at(e->loc, "chr() expects int");
                    res = g_ty_str; break;
                }
                if (strcmp(cname, "hex") == 0 || strcmp(cname, "bin") == 0 || strcmp(cname, "oct") == 0) {
                    if (argc != 1 || !type_is_int(check_expr(mod, fn, args[0], g_ty_i64))) error_at(e->loc, "%s() expects int", cname);
                    res = g_ty_str; break;
                }
                if (strcmp(cname, "abs") == 0) {
                    if (argc != 1) error_at(e->loc, "abs() takes 1 argument");
                    Type* at = check_expr(mod, fn, args[0], hint);
                    if (!type_is_numeric(at) && at->kind != TY_BIGINT) error_at(args[0]->loc, "abs() requires numeric type");
                    res = at; break;
                }
                if (strcmp(cname, "round") == 0) {
                    if (argc != 1) error_at(e->loc, "round() takes 1 argument");
                    check_expr(mod, fn, args[0], g_ty_f64);
                    res = g_ty_i64; break;
                }
                if (strcmp(cname, "pow") == 0) {
                    if (argc != 2) error_at(e->loc, "pow() takes 2 arguments");
                    Type* a = check_expr(mod, fn, args[0], hint);
                    Type* b = check_expr(mod, fn, args[1], a);
                    res = numeric_common_type(a, b); break;
                }
                if (strcmp(cname, "sum") == 0) {
                    if (argc != 1) error_at(e->loc, "sum() takes 1 argument");
                    Type* lt = check_expr(mod, fn, args[0], NULL);
                    Type* el = iter_element_type(lt, args[0]->loc);
                    if (!type_is_numeric(el)) error_at(args[0]->loc, "sum() requires numeric iterable");
                    res = el; break;
                }
                if (strcmp(cname, "min") == 0 || strcmp(cname, "max") == 0) {
                    if (argc == 1) {
                        Type* lt = check_expr(mod, fn, args[0], NULL);
                        res = iter_element_type(lt, args[0]->loc);
                    } else if (argc >= 2) {
                        Type* t0 = check_expr(mod, fn, args[0], hint);
                        for (int i = 1; i < argc; i++) check_expr(mod, fn, args[i], t0);
                        res = t0;
                    } else error_at(e->loc, "%s() requires at least 1 argument", cname);
                    break;
                }
                if (strcmp(cname, "all") == 0 || strcmp(cname, "any") == 0) {
                    if (argc != 1) error_at(e->loc, "%s() takes 1 argument", cname);
                    check_expr(mod, fn, args[0], NULL);
                    res = g_ty_bool; break;
                }
                if (strcmp(cname, "sorted") == 0 || strcmp(cname, "reversed") == 0) {
                    if (argc != 1) error_at(e->loc, "%s() takes 1 argument", cname);
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    Type* el = iter_element_type(at, args[0]->loc);
                    res = make_list_type(el); break;
                }
                if (strcmp(cname, "list") == 0) {
                    if (argc == 0) {
                        if (hint && hint->kind == TY_LIST) { res = hint; break; }
                        error_at(e->loc, "cannot infer type of empty list(); add type annotation");
                    }
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    res = make_list_type(iter_element_type(at, args[0]->loc)); break;
                }
                if (strcmp(cname, "set") == 0) {
                    if (argc == 0) {
                        if (hint && hint->kind == TY_SET) { res = hint; break; }
                        error_at(e->loc, "cannot infer type of empty set(); add type annotation");
                    }
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    res = make_set_type(iter_element_type(at, args[0]->loc)); break;
                }
                if (strcmp(cname, "dict") == 0) {
                    if (argc == 0) {
                        if (hint && hint->kind == TY_DICT) { res = hint; break; }
                        error_at(e->loc, "cannot infer type of empty dict(); add type annotation");
                    }
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    if (at->kind != TY_DICT) error_at(args[0]->loc, "dict() expects dict");
                    res = at; break;
                }
                if (strcmp(cname, "tuple") == 0) {
                    if (argc != 1) error_at(e->loc, "tuple() takes 1 argument");
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    res = make_list_type(iter_element_type(at, args[0]->loc));
                    if (at->kind == TY_TUPLE) res = at;
                    break;
                }
                if (strcmp(cname, "enumerate") == 0) {
                    if (argc != 1) error_at(e->loc, "enumerate() takes 1 argument");
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    Type* el = iter_element_type(at, args[0]->loc);
                    Type* pair[2] = {g_ty_i64, el};
                    res = make_list_type(make_tuple_type(pair, 2)); break;
                }
                if (strcmp(cname, "zip") == 0) {
                    if (argc != 2) error_at(e->loc, "zip() currently supports 2 iterables");
                    Type* a = check_expr(mod, fn, args[0], NULL);
                    Type* b = check_expr(mod, fn, args[1], NULL);
                    Type* pair[2] = {iter_element_type(a, args[0]->loc), iter_element_type(b, args[1]->loc)};
                    res = make_list_type(make_tuple_type(pair, 2)); break;
                }
                if (strcmp(cname, "range") == 0) {
                    for (int i = 0; i < argc; i++) {
                        if (!type_is_int(check_expr(mod, fn, args[i], g_ty_i64))) error_at(args[i]->loc, "range() requires int");
                    }
                    res = make_list_type(g_ty_i64); break;
                }
                if (strcmp(cname, "open") == 0) {
                    if (argc < 1 || argc > 2) error_at(e->loc, "open() takes 1 or 2 arguments");
                    if (check_expr(mod, fn, args[0], g_ty_str)->kind != TY_STR) error_at(args[0]->loc, "path must be str");
                    if (argc == 2 && check_expr(mod, fn, args[1], g_ty_str)->kind != TY_STR) error_at(args[1]->loc, "mode must be str");
                    res = g_ty_file; break;
                }
                if (strcmp(cname, "input") == 0) {
                    if (argc > 1) error_at(e->loc, "input() takes 0 or 1 arguments");
                    if (argc == 1 && check_expr(mod, fn, args[0], g_ty_str)->kind != TY_STR) error_at(args[0]->loc, "prompt must be str");
                    res = g_ty_str; break;
                }
                if (strcmp(cname, "isinstance") == 0) {
                    if (argc != 2) error_at(e->loc, "isinstance() takes 2 arguments");
                    check_expr(mod, fn, args[0], NULL);
                    if (args[1]->kind != EXPR_VAR) error_at(args[1]->loc, "isinstance() second argument must be a class name");
                    res = g_ty_bool; break;
                }
                if (strcmp(cname, "super") == 0) {
                    if (!fn || !fn->owner_cls) error_at(e->loc, "super() used outside class method");
                    for (int i = 0; i < argc; i++) {
                        if (i == 1) check_expr(mod, fn, args[1], fn->owner_cls->type);
                    }
                    res = fn->owner_cls->type; break;
                }
                if (strcmp(cname, "iter") == 0) {
                    if (argc != 1) error_at(e->loc, "iter() takes 1 argument");
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    res = make_list_type(iter_element_type(at, args[0]->loc)); break;
                }
                if (strcmp(cname, "next") == 0) {
                    if (argc < 1 || argc > 2) error_at(e->loc, "next() takes 1 or 2 arguments");
                    Type* it = check_expr(mod, fn, args[0], NULL);
                    if (it->kind != TY_LIST) error_at(args[0]->loc, "next() requires iterator");
                    if (argc == 2) check_expr(mod, fn, args[1], it->elem);
                    res = it->elem; break;
                }

                ClassInfo* ci = find_class_in_mod(mod, cname);
                VarSym* gsym = find_global_in_mod(mod, cname);
                if (!ci && gsym && gsym->type && gsym->type->kind == TY_CLASS) ci = gsym->type->cls;
                if (ci) {
                    FuncInfo* init_fn = find_method_in_class(ci, "__init__");
                    if (init_fn) {
                        int max_a = init_fn->param_count - 1;
                        int min_a = max_a;
                        for (int i = init_fn->param_count - 1; i >= 1; i--) {
                            if (init_fn->param_defaults[i]) min_a--; else break;
                        }
                        if (argc < min_a || argc > max_a) {
                            error_at(e->loc, "%s() expects %d..%d arguments, got %d", ci->name, min_a, max_a, argc);
                        }
                        for (int i = 0; i < argc; i++) {
                            Type* pt = init_fn->param_types[i + 1];
                            Type* at = check_expr(mod, fn, args[i], pt);
                            if (!type_assignable(pt, at)) {
                                error_at(args[i]->loc, "arg %d of %s(): expected '%s', got '%s'", i + 1, ci->name, type_str(pt), type_str(at));
                            }
                        }
                    } else if (ci->is_dataclass) {
                        int max_a = ci->field_count;
                        int min_a = max_a;
                        for (int i = ci->field_count - 1; i >= 0; i--) {
                            if (ci->fields[i].default_expr) min_a--; else break;
                        }
                        if (argc < min_a || argc > max_a) {
                            error_at(e->loc, "%s() expects %d..%d arguments, got %d", ci->name, min_a, max_a, argc);
                        }
                        for (int i = 0; i < argc; i++) {
                            Type* pt = ci->fields[i].type;
                            Type* at = check_expr(mod, fn, args[i], pt);
                            if (!type_assignable(pt, at)) {
                                error_at(args[i]->loc, "arg %d of %s(): expected '%s', got '%s'", i + 1, ci->name, type_str(pt), type_str(at));
                            }
                        }
                    } else if (argc != 0) {
                        error_at(e->loc, "%s() takes 0 arguments", ci->name);
                    }
                    res = ci->type;
                    break;
                }

                FuncInfo* target_fn = NULL;
                if (fn) {
                    for (int i = 0; i < g_all_func_count; i++) {
                        if (g_all_funcs[i]->parent_fn == fn && strcmp(g_all_funcs[i]->name, cname) == 0) {
                            target_fn = g_all_funcs[i]; break;
                        }
                    }
                }
                if (!target_fn) target_fn = find_func_in_mod(mod, cname);
                if (target_fn) {
                    int max_a = target_fn->param_count;
                    int min_a = max_a;
                    for (int i = max_a - 1; i >= 0; i--) {
                        if (target_fn->param_defaults[i]) min_a--; else break;
                    }
                    if (argc < min_a || argc > max_a) {
                        error_at(e->loc, "%s() expects %d..%d arguments, got %d", cname, min_a, max_a, argc);
                    }
                    for (int i = 0; i < argc; i++) {
                        Type* pt = target_fn->param_types[i];
                        Type* at = check_expr(mod, fn, args[i], pt);
                        if (!type_assignable(pt, at)) {
                            error_at(args[i]->loc, "arg %d of %s(): expected '%s', got '%s'", i + 1, cname, type_str(pt), type_str(at));
                        }
                    }
                    res = target_fn->ret_type;
                    break;
                }
            }

            Type* ct = check_expr(mod, fn, callee, NULL);
            if (ct->kind != TY_FUNC) error_at(e->loc, "expression of type '%s' is not callable", type_str(ct));
            if (argc != ct->func_param_count) error_at(e->loc, "callable expects %d args, got %d", ct->func_param_count, argc);
            for (int i = 0; i < argc; i++) {
                Type* at = check_expr(mod, fn, args[i], ct->func_params[i]);
                if (!type_assignable(ct->func_params[i], at)) {
                    error_at(args[i]->loc, "arg %d: expected '%s', got '%s'", i + 1, type_str(ct->func_params[i]), type_str(at));
                }
            }
            res = ct->func_ret;
            break;
        }
    }

    if (!res) res = g_ty_void;
    e->ty = res;
    return res;
}

static void register_nested_func(ModuleInfo* mod, FuncInfo* parent, Stmt* s) {
    FuncInfo* nf = (FuncInfo*)arena_alloc(sizeof(FuncInfo));
    nf->name = s->def_s.name;
    nf->llvm_name = arena_sprintf("@%s%s__%s_%d", mod->prefix, parent ? parent->name : "top", nf->name, g_all_func_count + 1);
    nf->mod = mod;
    nf->parent_fn = parent;
    nf->ast = s;
    nf->param_count = s->def_s.param_count;
    nf->param_names = nf->param_count ? (const char**)arena_alloc(sizeof(char*) * (size_t)nf->param_count) : NULL;
    nf->param_types = nf->param_count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)nf->param_count) : NULL;
    nf->param_defaults = nf->param_count ? (Expr**)arena_alloc(sizeof(Expr*) * (size_t)nf->param_count) : NULL;
    for (int i = 0; i < nf->param_count; i++) {
        Param* p = &s->def_s.params[i];
        if (!p->type_ann) error_at(p->loc, "parameter '%s' of '%s' requires a type annotation", p->name, nf->name);
        nf->param_names[i] = p->name;
        nf->param_types[i] = resolve_type_expr(mod, p->type_ann);
        nf->param_defaults[i] = p->default_val;
        add_local_in_fn(nf, p->name, nf->param_types[i], true, p->loc);
    }
    nf->ret_type = s->def_s.ret_type ? resolve_type_expr(mod, s->def_s.ret_type) : g_ty_void;
    nf->type = make_func_type(nf->param_types, nf->param_count, nf->ret_type);
    nf->is_generator = stmt_has_yield(s->def_s.body, s->def_s.body_count);
    g_all_funcs = (FuncInfo**)realloc(g_all_funcs, sizeof(FuncInfo*) * (size_t)(g_all_func_count + 1));
    g_all_funcs[g_all_func_count++] = nf;
    if (parent) add_local_in_fn(parent, nf->name, nf->type, false, s->loc);
    check_stmts(mod, nf, s->def_s.body, s->def_s.body_count);
}

static const char* extract_is_none_var(Expr* cond, bool* out_is_not_none) {
    if (!cond || cond->kind != EXPR_BINOP) return NULL;
    if (strcmp(cond->binop.op, "is") == 0 || strcmp(cond->binop.op, "is not") == 0) {
        *out_is_not_none = (strcmp(cond->binop.op, "is not") == 0);
        if (cond->binop.left->kind == EXPR_VAR && cond->binop.right->kind == EXPR_NONE) return cond->binop.left->var.name;
        if (cond->binop.right->kind == EXPR_VAR && cond->binop.left->kind == EXPR_NONE) return cond->binop.right->var.name;
    }
    return NULL;
}

static bool stmts_always_exit(Stmt** b, int cnt) {
    if (cnt <= 0) return false;
    StmtKind k = b[cnt - 1]->kind;
    return k == STMT_RETURN || k == STMT_RAISE || k == STMT_BREAK || k == STMT_CONTINUE;
}

static void check_stmts(ModuleInfo* mod, FuncInfo* fn, Stmt** stmts, int count) {
    VarSym* saved_syms[32];
    Type* saved_types[32];
    int saved_cnt = 0;
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        switch (s->kind) {
            case STMT_PASS:
            case STMT_BREAK:
            case STMT_CONTINUE:
                break;
            case STMT_GLOBAL:
                if (fn) {
                    for (int k = 0; k < s->scope_s.count; k++) {
                        fn->global_decls = (const char**)realloc(fn->global_decls, sizeof(char*) * (size_t)(fn->global_decl_count + 1));
                        fn->global_decls[fn->global_decl_count++] = s->scope_s.names[k];
                    }
                }
                break;
            case STMT_NONLOCAL:
                if (fn) {
                    for (int k = 0; k < s->scope_s.count; k++) {
                        fn->nonlocal_decls = (const char**)realloc(fn->nonlocal_decls, sizeof(char*) * (size_t)(fn->nonlocal_decl_count + 1));
                        fn->nonlocal_decls[fn->nonlocal_decl_count++] = s->scope_s.names[k];
                    }
                }
                break;
            case STMT_RETURN:
                if (!fn) error_at(s->loc, "'return' outside function");
                if (s->ret.expr) {
                    if (fn->ret_type->kind == TY_VOID) {
                        error_at(s->loc, "function '%s' has return type None; cannot return a value (add '-> ReturnType')", fn->name);
                    }
                    Type* rt = check_expr(mod, fn, s->ret.expr, fn->ret_type);
                    if (!type_assignable(fn->ret_type, rt)) {
                        error_at(s->loc, "return type '%s' incompatible with declared '%s'", type_str(rt), type_str(fn->ret_type));
                    }
                } else if (fn->ret_type->kind != TY_VOID && !fn->is_generator) {
                    error_at(s->loc, "function '%s' must return '%s'", fn->name, type_str(fn->ret_type));
                }
                break;
            case STMT_YIELD:
                if (!fn || fn->ret_type->kind != TY_LIST) error_at(s->loc, "'yield' function must declare return type 'list[T]'");
                if (s->expr_s.expr) {
                    Type* yt = check_expr(mod, fn, s->expr_s.expr, fn->ret_type->elem);
                    if (!type_assignable(fn->ret_type->elem, yt)) error_at(s->loc, "yield type mismatch");
                }
                break;
            case STMT_YIELD_FROM:
                if (!fn || fn->ret_type->kind != TY_LIST) error_at(s->loc, "'yield from' function must declare return type 'list[T]'");
                check_expr(mod, fn, s->expr_s.expr, fn->ret_type);
                break;
            case STMT_EXPR:
                check_expr(mod, fn, s->expr_s.expr, NULL);
                break;
            case STMT_ASSIGN: {
                Expr* tgt = s->assign.target;
                Type* ann_ty = s->assign.type_ann ? resolve_type_expr(mod, s->assign.type_ann) : NULL;
                if (tgt->kind == EXPR_VAR) {
                    const char* vname = tgt->var.name;
                    if (!s->assign.value) {
                        if (!ann_ty) error_at(s->loc, "declaration requires type annotation or value");
                        bind_var(mod, fn, vname, ann_ty, s->loc);
                        break;
                    }
                    VarSym* existing = resolve_var(mod, fn, vname, NULL);
                    Type* hint = ann_ty ? ann_ty : (existing ? existing->type : NULL);
                    Type* vt = check_expr(mod, fn, s->assign.value, hint);
                    if (ann_ty) {
                        if (!type_assignable(ann_ty, vt)) {
                            error_at(s->loc, "cannot assign '%s' to '%s: %s'", type_str(vt), vname, type_str(ann_ty));
                        }
                        bind_var(mod, fn, vname, ann_ty, s->loc);
                    } else {
                        if (vt->kind == TY_NONE_LIT) {
                            error_at(s->loc, "cannot assign None to '%s' without type annotation like '%s: T | None = None'", vname, vname);
                        }
                        bind_var(mod, fn, vname, vt, s->loc);
                    }
                } else if (tgt->kind == EXPR_ATTR) {
                    Type* ot = check_expr(mod, fn, tgt->attr.obj, NULL);
                    if (ot->kind != TY_CLASS) error_at(tgt->loc, "cannot set attribute on type '%s'", type_str(ot));
                    FieldInfo* fi = find_field_in_class(ot->cls, tgt->attr.attr);
                    if (fi) {
                        Type* vt = check_expr(mod, fn, s->assign.value, fi->type);
                        if (!type_assignable(fi->type, vt)) {
                            error_at(s->loc, "cannot assign '%s' to field '%s.%s' of type '%s'",
                                     type_str(vt), ot->cls->name, fi->name, type_str(fi->type));
                        }
                    } else {
                        FieldInfo* ca = find_class_attr(ot->cls, tgt->attr.attr);
                        if (!ca) error_at(tgt->loc, "class '%s' has no field '%s'", ot->cls->name, tgt->attr.attr);
                        Type* vt = check_expr(mod, fn, s->assign.value, ca->type);
                        if (!type_assignable(ca->type, vt)) error_at(s->loc, "class attribute type mismatch");
                    }
                } else if (tgt->kind == EXPR_INDEX) {
                    Type* ot = check_expr(mod, fn, tgt->index.obj, NULL);
                    if (ot->kind == TY_LIST) {
                        if (!type_is_int(check_expr(mod, fn, tgt->index.index, g_ty_i64))) error_at(tgt->index.index->loc, "list index must be int");
                        tgt->ty = ot->elem;
                        Type* vt = check_expr(mod, fn, s->assign.value, ot->elem);
                        if (!type_assignable(ot->elem, vt)) error_at(s->loc, "list element assignment type mismatch");
                    } else if (ot->kind == TY_DICT) {
                        Type* kt = check_expr(mod, fn, tgt->index.index, ot->key);
                        if (!type_assignable(ot->key, kt)) error_at(tgt->index.index->loc, "dict key type mismatch");
                        tgt->ty = ot->val;
                        Type* vt = check_expr(mod, fn, s->assign.value, ot->val);
                        if (!type_assignable(ot->val, vt)) error_at(s->loc, "dict value assignment type mismatch");
                    } else {
                        error_at(tgt->loc, "type '%s' does not support item assignment", type_str(ot));
                    }
                } else {
                    error_at(tgt->loc, "invalid assignment target");
                }
                break;
            }
            case STMT_MULTI_ASSIGN: {
                Type* vt = check_expr(mod, fn, s->multi_assign.value, NULL);
                if (vt->kind != TY_TUPLE || vt->tuple_count != s->multi_assign.target_count) {
                    error_at(s->loc, "cannot unpack '%s' into %d targets", type_str(vt), s->multi_assign.target_count);
                }
                for (int k = 0; k < s->multi_assign.target_count; k++) {
                    Expr* t = s->multi_assign.targets[k];
                    if (t->kind != EXPR_VAR) error_at(t->loc, "unpacking target must be variable");
                    bind_var(mod, fn, t->var.name, vt->tuple_elems[k], t->loc);
                }
                break;
            }
            case STMT_AUG_ASSIGN: {
                Type* tt = check_expr(mod, fn, s->aug_assign.target, NULL);
                Type* vt = check_expr(mod, fn, s->aug_assign.value, tt);
                if (tt->kind == TY_SET && type_eq(tt, vt)) break;
                if (tt->kind == TY_STR && vt->kind == TY_STR && strcmp(s->aug_assign.op, "+=") == 0) break;
                if (tt->kind == TY_LIST && type_eq(tt, vt) && strcmp(s->aug_assign.op, "+=") == 0) break;
                if (!type_is_numeric(tt) || !type_is_numeric(vt)) {
                    error_at(s->loc, "augmented assignment '%s' unsupported for '%s' and '%s'", s->aug_assign.op, type_str(tt), type_str(vt));
                }
                break;
            }
            case STMT_IF: {
                for (int b = 0; b < s->if_s.branch_count; b++) {
                    check_expr(mod, fn, s->if_s.conds[b], g_ty_bool);
                    bool is_not_none = false;
                    const char* nvar = extract_is_none_var(s->if_s.conds[b], &is_not_none);
                    VarSym* nsym = (nvar && is_not_none) ? resolve_var(mod, fn, nvar, NULL) : NULL;
                    Type* orig_t = NULL;
                    if (nsym && nsym->type && nsym->type->kind == TY_OPTIONAL) {
                        orig_t = nsym->type;
                        nsym->type = orig_t->elem;
                    }
                    check_stmts(mod, fn, s->if_s.bodies[b], s->if_s.body_counts[b]);
                    if (orig_t) nsym->type = orig_t;
                }
                if (s->if_s.else_count > 0) {
                    bool is_not_none = false;
                    const char* nvar = (s->if_s.branch_count == 1) ? extract_is_none_var(s->if_s.conds[0], &is_not_none) : NULL;
                    VarSym* nsym = (nvar && !is_not_none) ? resolve_var(mod, fn, nvar, NULL) : NULL;
                    Type* orig_t = NULL;
                    if (nsym && nsym->type && nsym->type->kind == TY_OPTIONAL) {
                        orig_t = nsym->type;
                        nsym->type = orig_t->elem;
                    }
                    check_stmts(mod, fn, s->if_s.else_body, s->if_s.else_count);
                    if (orig_t) nsym->type = orig_t;
                }
                if (s->if_s.branch_count == 1 && s->if_s.else_count == 0 && stmts_always_exit(s->if_s.bodies[0], s->if_s.body_counts[0])) {
                    bool is_not_none = false;
                    const char* nvar = extract_is_none_var(s->if_s.conds[0], &is_not_none);
                    VarSym* nsym = (nvar && !is_not_none) ? resolve_var(mod, fn, nvar, NULL) : NULL;
                    if (nsym && nsym->type && nsym->type->kind == TY_OPTIONAL && saved_cnt < 32) {
                        saved_syms[saved_cnt] = nsym;
                        saved_types[saved_cnt] = nsym->type;
                        saved_cnt++;
                        nsym->type = nsym->type->elem;
                    }
                }
                break;
            }
            case STMT_WHILE:
                check_expr(mod, fn, s->while_s.cond, g_ty_bool);
                check_stmts(mod, fn, s->while_s.body, s->while_s.body_count);
                if (s->while_s.else_count > 0) check_stmts(mod, fn, s->while_s.else_body, s->while_s.else_count);
                break;
            case STMT_FOR: {
                Expr* it = s->for_s.iter;
                if (it->kind == EXPR_CALL && it->call.callee->kind == EXPR_VAR &&
                    strcmp(it->call.callee->var.name, "range") == 0) {
                    for (int a = 0; a < it->call.arg_count; a++) {
                        if (!type_is_int(check_expr(mod, fn, it->call.args[a], g_ty_i64))) {
                            error_at(it->call.args[a]->loc, "range() arguments must be integers");
                        }
                    }
                    it->ty = make_list_type(g_ty_i64);
                    if (s->for_s.target_count != 1) error_at(s->loc, "expected 1 loop variable for range()");
                    bind_var(mod, fn, s->for_s.targets[0], g_ty_i64, s->loc);
                } else {
                    Type* it_ty = check_expr(mod, fn, it, NULL);
                    if (s->for_s.target_count == 1) {
                        bind_var(mod, fn, s->for_s.targets[0], iter_element_type(it_ty, it->loc), s->loc);
                    } else if (s->for_s.target_count == 2 && it_ty->kind == TY_LIST &&
                               it_ty->elem->kind == TY_TUPLE && it_ty->elem->tuple_count == 2) {
                        bind_var(mod, fn, s->for_s.targets[0], it_ty->elem->tuple_elems[0], s->loc);
                        bind_var(mod, fn, s->for_s.targets[1], it_ty->elem->tuple_elems[1], s->loc);
                    } else {
                        error_at(s->loc, "cannot unpack iterable of type '%s'", type_str(it_ty));
                    }
                }
                check_stmts(mod, fn, s->for_s.body, s->for_s.body_count);
                if (s->for_s.else_count > 0) check_stmts(mod, fn, s->for_s.else_body, s->for_s.else_count);
                break;
            }
            case STMT_DEF:
                if (fn) register_nested_func(mod, fn, s);
                break;
            case STMT_CLASS:
            case STMT_IMPORT:
            case STMT_FROM_IMPORT:
                break;
            case STMT_TRY:
                check_stmts(mod, fn, s->try_s.try_body, s->try_s.try_count);
                for (int h = 0; h < s->try_s.handler_count; h++) {
                    ExceptHandler* eh = &s->try_s.handlers[h];
                    if (eh->as_name) {
                        Type* et = g_ty_str;
                        if (eh->exc_type) {
                            ClassInfo* ci = find_class_in_mod(mod, eh->exc_type);
                            if (ci) et = ci->type;
                        }
                        bind_var(mod, fn, eh->as_name, et, s->loc);
                    }
                    check_stmts(mod, fn, eh->body, eh->body_count);
                }
                if (s->try_s.else_count > 0) check_stmts(mod, fn, s->try_s.else_body, s->try_s.else_count);
                if (s->try_s.finally_count > 0) check_stmts(mod, fn, s->try_s.finally_body, s->try_s.finally_count);
                break;
            case STMT_RAISE:
                if (s->raise_s.exc) check_expr(mod, fn, s->raise_s.exc, NULL);
                break;
            case STMT_ASSERT:
                check_expr(mod, fn, s->assert_s.cond, g_ty_bool);
                if (s->assert_s.msg) check_expr(mod, fn, s->assert_s.msg, NULL);
                break;
            case STMT_WITH: {
                Type* ct = check_expr(mod, fn, s->with_s.ctx, NULL);
                Type* as_ty = ct;
                if (ct->kind == TY_FILE) as_ty = g_ty_file;
                else if (ct->kind == TY_CLASS) {
                    FuncInfo* ent = find_method_in_class(ct->cls, "__enter__");
                    if (!ent) error_at(s->loc, "context manager '%s' lacks __enter__", ct->cls->name);
                    as_ty = ent->ret_type;
                } else {
                    error_at(s->loc, "type '%s' is not a context manager", type_str(ct));
                }
                if (s->with_s.as_name) bind_var(mod, fn, s->with_s.as_name, as_ty, s->loc);
                check_stmts(mod, fn, s->with_s.body, s->with_s.body_count);
                break;
            }
            case STMT_DEL: {
                Expr* dt = s->del_s.target;
                if (dt->kind == EXPR_INDEX) {
                    Type* ot = check_expr(mod, fn, dt->index.obj, NULL);
                    if (ot->kind == TY_LIST) {
                        if (!type_is_int(check_expr(mod, fn, dt->index.index, g_ty_i64))) error_at(dt->loc, "index must be int");
                    } else if (ot->kind == TY_DICT) {
                        Type* kt = check_expr(mod, fn, dt->index.index, ot->key);
                        if (!type_assignable(ot->key, kt)) error_at(dt->loc, "dict key type mismatch");
                    } else {
                        error_at(dt->loc, "cannot del item from '%s'", type_str(ot));
                    }
                } else if (dt->kind == EXPR_VAR) {
                    check_expr(mod, fn, dt, NULL);
                } else {
                    error_at(dt->loc, "invalid del target");
                }
                break;
            }
            case STMT_MATCH: {
                Type* st = check_expr(mod, fn, s->match_s.subject, NULL);
                for (int c = 0; c < s->match_s.case_count; c++) {
                    MatchCase* mc = &s->match_s.cases[c];
                    if (mc->pattern) check_expr(mod, fn, mc->pattern, st);
                    if (mc->capture) bind_var(mod, fn, mc->capture, st, s->loc);
                    check_stmts(mod, fn, mc->body, mc->body_count);
                }
                break;
            }
        }
    }
    for (int k = saved_cnt - 1; k >= 0; k--) {
        saved_syms[k]->type = saved_types[k];
    }
}

static void add_field_to_class(ClassInfo* cls, const char* name, Type* type, Expr* def_e, SrcLoc loc) {
    FieldInfo* existing = find_field_in_class(cls, name);
    if (existing) {
        if (!type_assignable(existing->type, type)) {
            error_at(loc, "field '%s.%s' redefined with incompatible type '%s' vs '%s'",
                     cls->name, name, type_str(type), type_str(existing->type));
        }
        return;
    }
    cls->fields = (FieldInfo*)realloc(cls->fields, sizeof(FieldInfo) * (size_t)(cls->field_count + 1));
    cls->fields[cls->field_count] = (FieldInfo){name, type, def_e, -1, loc};
    cls->field_count++;
}

static bool class_has_recursive_field(ClassInfo* cls) {
    for (int i = 0; i < cls->field_count; i++) {
        Type* ft = cls->fields[i].type;
        if (ft->kind == TY_OPTIONAL && ft->elem->kind == TY_CLASS && class_is_subclass(cls, ft->elem->cls)) return true;
        if (ft->kind == TY_LIST && ft->elem->kind == TY_CLASS && class_is_subclass(cls, ft->elem->cls)) return true;
        if (ft->kind == TY_CLASS && class_is_subclass(cls, ft->cls)) return true;
    }
    return false;
}

static void analyze_module(ModuleInfo* mod) {
    if (mod->analyzed) return;
    mod->analyzed = true;

    for (int i = 0; i < mod->stmt_count; i++) {
        Stmt* s = mod->stmts[i];
        if (s->kind == STMT_IMPORT) {
            const char* mname = s->import_s.mod_name;
            const char* bind = s->import_s.alias ? s->import_s.alias : mname;
            if (strcmp(mname, "sys") == 0) {
                ModuleInfo* sm = (ModuleInfo*)arena_alloc(sizeof(ModuleInfo));
                sm->name = "sys";
                Type* mt = (Type*)arena_alloc(sizeof(Type));
                mt->kind = TY_MODULE; mt->mod = sm; mt->mangled = "mod_sys";
                add_global_in_mod(mod, bind, mt, s->loc);
                continue;
            }
            ModuleInfo* imp = load_module_by_name(mname, g_root_dir, s->loc);
            Type* mt = (Type*)arena_alloc(sizeof(Type));
            mt->kind = TY_MODULE; mt->mod = imp; mt->mangled = arena_sprintf("mod_%s", imp->name);
            add_global_in_mod(mod, bind, mt, s->loc);
        } else if (s->kind == STMT_FROM_IMPORT) {
            const char* mname = s->from_import_s.mod_name;
            if (strcmp(mname, "abc") == 0 || strcmp(mname, "dataclasses") == 0 || strcmp(mname, "typing") == 0) {
                continue;
            }
            ModuleInfo* imp = load_module_by_name(mname, g_root_dir, s->loc);
            for (int k = 0; k < s->from_import_s.count; k++) {
                const char* orig = s->from_import_s.names[k];
                const char* bind = s->from_import_s.aliases[k] ? s->from_import_s.aliases[k] : orig;
                VarSym* gv = find_global_in_mod(imp, orig);
                if (gv) {
                    VarSym* nv = add_global_in_mod(mod, bind, gv->type, s->loc);
                    nv->llvm_name = gv->llvm_name;
                    continue;
                }
                FuncInfo* fi = find_func_in_mod(imp, orig);
                if (fi) {
                    FuncInfo* alias_fn = (FuncInfo*)arena_alloc(sizeof(FuncInfo));
                    *alias_fn = *fi;
                    alias_fn->name = bind;
                    mod->funcs = (FuncInfo**)realloc(mod->funcs, sizeof(FuncInfo*) * (size_t)(mod->func_count + 1));
                    mod->funcs[mod->func_count++] = alias_fn;
                    continue;
                }
                ClassInfo* ci = find_class_in_mod(imp, orig);
                if (ci) {
                    ClassInfo* alias_ci = (ClassInfo*)arena_alloc(sizeof(ClassInfo));
                    *alias_ci = *ci;
                    alias_ci->name = bind;
                    mod->classes = (ClassInfo**)realloc(mod->classes, sizeof(ClassInfo*) * (size_t)(mod->class_count + 1));
                    mod->classes[mod->class_count++] = alias_ci;
                    continue;
                }
                error_at(s->loc, "cannot import '%s' from module '%s'", orig, mname);
            }
        } else if (s->kind == STMT_CLASS) {
            ClassInfo* cls = (ClassInfo*)arena_alloc(sizeof(ClassInfo));
            cls->name = s->class_s.name;
            cls->llvm_name = arena_sprintf("%%Class_%s%s", mod->prefix, cls->name);
            cls->mod = mod;
            cls->ast = s;
            cls->type_id = g_next_class_id++;
            cls->is_dataclass = expr_has_decorator(s->class_s.decorators, s->class_s.deco_count, "dataclass");
            Type* ct = (Type*)arena_alloc(sizeof(Type));
            ct->kind = TY_CLASS;
            ct->cls = cls;
            ct->mangled = arena_sprintf("C_%s%s", mod->prefix, cls->name);
            cls->type = ct;
            mod->classes = (ClassInfo**)realloc(mod->classes, sizeof(ClassInfo*) * (size_t)(mod->class_count + 1));
            mod->classes[mod->class_count++] = cls;
            g_all_classes = (ClassInfo**)realloc(g_all_classes, sizeof(ClassInfo*) * (size_t)(g_all_class_count + 1));
            g_all_classes[g_all_class_count++] = cls;
        }
    }

    for (int i = 0; i < mod->class_count; i++) {
        ClassInfo* cls = mod->classes[i];
        Stmt* cs = cls->ast;
        if (!cs) continue;
        cls->bases = cs->class_s.base_count ? (ClassInfo**)arena_alloc(sizeof(ClassInfo*) * (size_t)cs->class_s.base_count) : NULL;
        int real_bases = 0;
        for (int b = 0; b < cs->class_s.base_count; b++) {
            const char* bname = cs->class_s.bases[b];
            if (strcmp(bname, "ABC") == 0) {
                cls->is_abc = true;
                continue;
            }
            ClassInfo* bci = find_class_in_mod(mod, bname);
            if (!bci) error_at(cs->loc, "unknown base class '%s'", bname);
            cls->bases[real_bases++] = bci;
            bci->has_subclasses = true;
            bci->subclasses = (ClassInfo**)realloc(bci->subclasses, sizeof(ClassInfo*) * (size_t)(bci->subclass_count + 1));
            bci->subclasses[bci->subclass_count++] = cls;
            if (bci->is_abc) cls->is_abc = true;
        }
        cls->base_count = real_bases;
    }

    for (int i = 0; i < mod->class_count; i++) {
        ClassInfo* cls = mod->classes[i];
        compute_c3_mro(cls, cls->ast ? cls->ast->loc : (SrcLoc){mod->path, 1, 1});
        cls->ancestors_mask = 0;
        for (int m = 0; m < cls->mro_count; m++) {
            if (cls->mro[m]->type_id < 64) {
                cls->ancestors_mask |= (1ULL << cls->mro[m]->type_id);
            }
        }
    }

    for (int i = 0; i < mod->class_count; i++) {
        ClassInfo* cls = mod->classes[i];
        Stmt* cs = cls->ast;
        if (!cs) continue;

        for (int m = cls->mro_count - 1; m >= 1; m--) {
            ClassInfo* base = cls->mro[m];
            for (int f = 0; f < base->field_count; f++) {
                add_field_to_class(cls, base->fields[f].name, base->fields[f].type, base->fields[f].default_expr, base->fields[f].loc);
            }
        }

        for (int j = 0; j < cs->class_s.body_count; j++) {
            Stmt* bs = cs->class_s.body[j];
            if (bs->kind == STMT_ASSIGN && bs->assign.target->kind == EXPR_VAR) {
                const char* fname = bs->assign.target->var.name;
                Type* ft = bs->assign.type_ann ? resolve_type_expr(mod, bs->assign.type_ann) : NULL;
                if (bs->assign.value) {
                    Type* vt = check_expr(mod, NULL, bs->assign.value, ft);
                    if (!ft) ft = vt;
                    else if (!type_assignable(ft, vt)) error_at(bs->loc, "field '%s.%s' default type mismatch", cls->name, fname);
                }
                if (!ft) error_at(bs->loc, "field '%s.%s' requires type annotation", cls->name, fname);
                if (cls->is_dataclass || bs->assign.type_ann) {
                    add_field_to_class(cls, fname, ft, bs->assign.value, bs->loc);
                } else {
                    cls->class_attrs = (FieldInfo*)realloc(cls->class_attrs, sizeof(FieldInfo) * (size_t)(cls->class_attr_count + 1));
                    cls->class_attrs[cls->class_attr_count++] = (FieldInfo){fname, ft, bs->assign.value, -1, bs->loc};
                }
            } else if (bs->kind == STMT_DEF) {
                FuncInfo* mf = (FuncInfo*)arena_alloc(sizeof(FuncInfo));
                mf->name = bs->def_s.name;
                mf->llvm_name = arena_sprintf("@%s%s__%s", mod->prefix, cls->name, mf->name);
                mf->mod = mod;
                mf->owner_cls = cls;
                mf->ast = bs;
                mf->is_method = true;
                mf->is_static = expr_has_decorator(bs->def_s.decorators, bs->def_s.deco_count, "staticmethod");
                mf->is_classmethod = expr_has_decorator(bs->def_s.decorators, bs->def_s.deco_count, "classmethod");
                mf->is_property = expr_has_decorator(bs->def_s.decorators, bs->def_s.deco_count, "property");
                mf->is_abstract = expr_has_decorator(bs->def_s.decorators, bs->def_s.deco_count, "abstractmethod");
                if (mf->is_abstract) {
                    cls->abstract_methods = (const char**)realloc(cls->abstract_methods, sizeof(char*) * (size_t)(cls->abstract_count + 1));
                    cls->abstract_methods[cls->abstract_count++] = mf->name;
                }
                mf->param_count = bs->def_s.param_count;
                mf->param_names = mf->param_count ? (const char**)arena_alloc(sizeof(char*) * (size_t)mf->param_count) : NULL;
                mf->param_types = mf->param_count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)mf->param_count) : NULL;
                mf->param_defaults = mf->param_count ? (Expr**)arena_alloc(sizeof(Expr*) * (size_t)mf->param_count) : NULL;
                for (int p = 0; p < mf->param_count; p++) {
                    Param* pr = &bs->def_s.params[p];
                    mf->param_names[p] = pr->name;
                    mf->param_defaults[p] = pr->default_val;
                    if (p == 0 && !mf->is_static) {
                        mf->param_types[p] = cls->type;
                    } else {
                        if (!pr->type_ann) error_at(pr->loc, "parameter '%s' of '%s.%s' requires type annotation", pr->name, cls->name, mf->name);
                        mf->param_types[p] = resolve_type_expr(mod, pr->type_ann);
                    }
                    add_local_in_fn(mf, pr->name, mf->param_types[p], true, pr->loc);
                }
                mf->ret_type = bs->def_s.ret_type ? resolve_type_expr(mod, bs->def_s.ret_type) : g_ty_void;
                mf->type = make_func_type(mf->param_types, mf->param_count, mf->ret_type);
                cls->methods = (FuncInfo**)realloc(cls->methods, sizeof(FuncInfo*) * (size_t)(cls->method_count + 1));
                cls->methods[cls->method_count++] = mf;
                g_all_funcs = (FuncInfo**)realloc(g_all_funcs, sizeof(FuncInfo*) * (size_t)(g_all_func_count + 1));
                g_all_funcs[g_all_func_count++] = mf;
            }
        }

        FuncInfo* init_fn = find_method_in_class_exact(cls, "__init__");
        if (init_fn) {
            for (int j = 0; j < init_fn->ast->def_s.body_count; j++) {
                Stmt* is = init_fn->ast->def_s.body[j];
                if (is->kind == STMT_ASSIGN && is->assign.target->kind == EXPR_ATTR &&
                    is->assign.target->attr.obj->kind == EXPR_VAR &&
                    strcmp(is->assign.target->attr.obj->var.name, "self") == 0) {
                    const char* fnm = is->assign.target->attr.attr;
                    if (!find_field_in_class(cls, fnm)) {
                        Type* ft = is->assign.type_ann ? resolve_type_expr(mod, is->assign.type_ann) : NULL;
                        Type* vt = is->assign.value ? check_expr(mod, init_fn, is->assign.value, ft) : NULL;
                        if (!ft) ft = vt;
                        if (!ft || ft->kind == TY_NONE_LIT) {
                            error_at(is->loc, "field 'self.%s' requires a type annotation (e.g. 'self.%s: %s = None')", fnm, fnm, "T | None");
                        }
                        add_field_to_class(cls, fnm, ft, NULL, is->loc);
                    }
                }
            }
        }
    }

    for (int i = 0; i < mod->class_count; i++) {
        ClassInfo* cls = mod->classes[i];
        for (int m = 0; m < cls->mro_count; m++) {
            ClassInfo* anc = cls->mro[m];
            for (int a = 0; a < anc->abstract_count; a++) {
                const char* am = anc->abstract_methods[a];
                FuncInfo* impl = find_method_in_class(cls, am);
                if (!impl || impl->is_abstract) {
                    cls->has_unimpl_abstract = true;
                    break;
                }
            }
            if (cls->has_unimpl_abstract) break;
        }
        cls->needs_vtable = (cls->has_subclasses || cls->base_count > 0 || cls->is_abc);
        if (cls->is_dataclass && !cls->is_abc && !class_has_recursive_field(cls)) {
            bool ok_val = true;
            for (int m = 0; m < cls->mro_count; m++) {
                if (!cls->mro[m]->is_dataclass) { ok_val = false; break; }
            }
            if (ok_val) {
                cls->is_value_type = true;
                cls->needs_vtable = false;
            }
        }
        if (cls->needs_vtable) {
            for (int m = cls->mro_count - 1; m >= 0; m--) {
                ClassInfo* anc = cls->mro[m];
                for (int k = 0; k < anc->method_count; k++) {
                    FuncInfo* mf = anc->methods[k];
                    if (mf->is_static || mf->is_classmethod || strcmp(mf->name, "__init__") == 0) continue;
                    MethodSlot* slot = find_vtable_slot(cls, mf->name);
                    if (slot) {
                        slot->fn = find_method_in_class(cls, mf->name);
                    } else {
                        cls->vtable = (MethodSlot*)realloc(cls->vtable, sizeof(MethodSlot) * (size_t)(cls->vtable_count + 1));
                        cls->vtable[cls->vtable_count] = (MethodSlot){mf->name, find_method_in_class(cls, mf->name), cls->vtable_count};
                        cls->vtable_count++;
                    }
                }
            }
        }
        int slot_base = cls->needs_vtable ? 1 : 0;
        for (int f = 0; f < cls->field_count; f++) {
            cls->fields[f].slot_index = slot_base + f;
        }
    }

    for (int i = 0; i < mod->stmt_count; i++) {
        Stmt* s = mod->stmts[i];
        if (s->kind == STMT_DEF) {
            FuncInfo* fn = (FuncInfo*)arena_alloc(sizeof(FuncInfo));
            fn->name = s->def_s.name;
            fn->llvm_name = arena_sprintf("@%s%s", mod->prefix, fn->name);
            fn->mod = mod;
            fn->ast = s;
            fn->param_count = s->def_s.param_count;
            fn->param_names = fn->param_count ? (const char**)arena_alloc(sizeof(char*) * (size_t)fn->param_count) : NULL;
            fn->param_types = fn->param_count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)fn->param_count) : NULL;
            fn->param_defaults = fn->param_count ? (Expr**)arena_alloc(sizeof(Expr*) * (size_t)fn->param_count) : NULL;
            for (int p = 0; p < fn->param_count; p++) {
                Param* pr = &s->def_s.params[p];
                if (!pr->type_ann) error_at(pr->loc, "parameter '%s' of '%s()' requires a type annotation", pr->name, fn->name);
                fn->param_names[p] = pr->name;
                fn->param_types[p] = resolve_type_expr(mod, pr->type_ann);
                fn->param_defaults[p] = pr->default_val;
                add_local_in_fn(fn, pr->name, fn->param_types[p], true, pr->loc);
            }
            fn->ret_type = s->def_s.ret_type ? resolve_type_expr(mod, s->def_s.ret_type) : g_ty_void;
            fn->type = make_func_type(fn->param_types, fn->param_count, fn->ret_type);
            fn->is_generator = stmt_has_yield(s->def_s.body, s->def_s.body_count);
            mod->funcs = (FuncInfo**)realloc(mod->funcs, sizeof(FuncInfo*) * (size_t)(mod->func_count + 1));
            mod->funcs[mod->func_count++] = fn;
            g_all_funcs = (FuncInfo**)realloc(g_all_funcs, sizeof(FuncInfo*) * (size_t)(g_all_func_count + 1));
            g_all_funcs[g_all_func_count++] = fn;
        }
    }

    for (int i = 0; i < mod->stmt_count; i++) {
        Stmt* s = mod->stmts[i];
        if (s->kind == STMT_ASSIGN && s->assign.target->kind == EXPR_VAR) {
            if (s->assign.type_ann) {
                Type* gt = resolve_type_expr(mod, s->assign.type_ann);
                add_global_in_mod(mod, s->assign.target->var.name, gt, s->loc);
            } else if (s->assign.value && (s->assign.value->kind == EXPR_INT || s->assign.value->kind == EXPR_FLOAT ||
                       s->assign.value->kind == EXPR_BOOL || s->assign.value->kind == EXPR_STR)) {
                Type* gt = check_expr(mod, NULL, s->assign.value, NULL);
                add_global_in_mod(mod, s->assign.target->var.name, gt, s->loc);
            }
        }
    }

    for (int i = 0; i < mod->class_count; i++) {
        ClassInfo* cls = mod->classes[i];
        if (!cls->ast) continue;
        for (int j = 0; j < cls->method_count; j++) {
            FuncInfo* mf = cls->methods[j];
            check_stmts(mod, mf, mf->ast->def_s.body, mf->ast->def_s.body_count);
        }
    }

    for (int i = 0; i < mod->func_count; i++) {
        FuncInfo* fn = mod->funcs[i];
        if (fn->mod != mod || !fn->ast) continue;
        check_stmts(mod, fn, fn->ast->def_s.body, fn->ast->def_s.body_count);
    }

    check_stmts(mod, NULL, mod->stmts, mod->stmt_count);
}

static ModuleInfo* load_module_by_name(const char* mod_name, const char* from_dir, SrcLoc loc) {
    for (int i = 0; i < g_module_count; i++) {
        if (strcmp(g_modules[i]->name, mod_name) == 0) return g_modules[i];
    }
    char rel[512];
    snprintf(rel, sizeof(rel), "%s", mod_name);
    for (int i = 0; rel[i]; i++) if (rel[i] == '.') rel[i] = '/';

    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.str", from_dir, rel);
    size_t len = 0;
    char* src = read_entire_file(path, &len);
    if (!src) {
        snprintf(path, sizeof(path), "%s/%s/__init__.str", from_dir, rel);
        src = read_entire_file(path, &len);
    }
    if (!src) error_at(loc, "cannot find module '%s' (looked for .str)", mod_name);

    ModuleInfo* mod = (ModuleInfo*)arena_alloc(sizeof(ModuleInfo));
    mod->name = arena_strdup(mod_name);
    mod->path = arena_strdup(path);
    char* pfx = arena_strdup(mod_name);
    for (int i = 0; pfx[i]; i++) if (pfx[i] == '.') pfx[i] = '_';
    mod->prefix = arena_sprintf("m_%s__", pfx);

    TokenList tl = lex_source(mod->path, src, len);
    free(src);
    Parser ps = {tl.items, tl.count, 0, mod->path};
    mod->stmts = parse_module(&ps, &mod->stmt_count);
    free(tl.items);

    g_modules = (ModuleInfo**)realloc(g_modules, sizeof(ModuleInfo*) * (size_t)(g_module_count + 1));
    g_modules[g_module_count++] = mod;
    analyze_module(mod);
    return mod;
}

/* =========================================================================
 * 5. LLVM IR CODE GENERATOR
 * ========================================================================= */

typedef struct {
    const char* break_lbl;
    const char* cont_lbl;
    bool* broke_flag;
    const char* broke_alloca;
} LoopCtx;

typedef struct {
    StrBuf hdr;
    StrBuf strs;
    StrBuf mono;
    StrBuf funcs;
    StrBuf cur_entry;
    StrBuf cur_body;
    int reg_id;
    int lbl_id;
    int str_id;
    bool in_func;
    bool block_terminated;
    FuncInfo* cur_fn;
    ModuleInfo* cur_mod;
    const char* yield_list_reg;
    LoopCtx loop_stack[64];
    int loop_depth;
    /* String literal deduplication */
    const char** lit_texts;
    size_t* lit_lens;
    const char** lit_syms;
    int lit_count;
    int lit_cap;
    /* Track which hash/eq helpers were emitted */
    TypeRegistry emitted_hash_eq;
    /* Track declared globals/allocas in current func */
    const char** fn_allocas;
    int fn_alloca_count;
} CG;

static CG g_cg;

static void cg_init(void) {
    memset(&g_cg, 0, sizeof(g_cg));
    sb_init(&g_cg.hdr);
    sb_init(&g_cg.strs);
    sb_init(&g_cg.mono);
    sb_init(&g_cg.funcs);
    sb_init(&g_cg.cur_entry);
    sb_init(&g_cg.cur_body);
}

static const char* cg_reg(void) {
    return arena_sprintf("%%r%d", ++g_cg.reg_id);
}

static const char* cg_lbl(const char* prefix) {
    return arena_sprintf("%s_%d", prefix, ++g_cg.lbl_id);
}

static StrBuf* cg_out(void) {
    return g_cg.in_func ? &g_cg.cur_body : &g_cg.cur_body;
}

static void cg_emit(const char* fmt, ...) {
    if (g_cg.block_terminated) return;
    StrBuf* b = cg_out();
    sb_append(b, "  ");
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_ensure(b, (size_t)n + 1);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
    sb_append(b, "\n");
}

static void cg_entry(const char* fmt, ...) {
    StrBuf* b = &g_cg.cur_entry;
    sb_append(b, "  ");
    va_list ap, ap2;
    va_start(ap, fmt);
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n > 0) {
        sb_ensure(b, (size_t)n + 1);
        vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap2);
        b->len += (size_t)n;
    }
    va_end(ap2);
    sb_append(b, "\n");
}

static void cg_label(const char* lbl) {
    sb_printf(cg_out(), "%s:\n", lbl);
    g_cg.block_terminated = false;
}

static void cg_br(const char* lbl) {
    if (!g_cg.block_terminated) {
        cg_emit("br label %%%s", lbl);
        g_cg.block_terminated = true;
    }
}

static void cg_cond_br(const char* cond, const char* t_lbl, const char* f_lbl) {
    if (!g_cg.block_terminated) {
        cg_emit("br i1 %s, label %%%s, label %%%s", cond, t_lbl, f_lbl);
        g_cg.block_terminated = true;
    }
}

static const char* llvm_reg_ty(Type* t) {
    if (!t) return "void";
    switch (t->kind) {
        case TY_VOID: return "void";
        case TY_BOOL: return "i1";
        case TY_I8: case TY_U8: return "i8";
        case TY_I16: case TY_U16: return "i16";
        case TY_I32: case TY_U32: return "i32";
        case TY_I64: case TY_U64: return "i64";
        case TY_F32: return "float";
        case TY_F64: return "double";
        case TY_STR: case TY_BYTES: case TY_BIGINT: case TY_FILE:
        case TY_LIST: case TY_SET: case TY_DICT: case TY_FUNC:
        case TY_NONE_LIT:
            return "ptr";
        case TY_CLASS:
            return t->cls->is_value_type ? t->cls->llvm_name : "ptr";
        case TY_TUPLE:
            return arena_sprintf("%%%s", t->mangled);
        case TY_OPTIONAL:
            if (type_is_heap_ref(t->elem)) return "ptr";
            return arena_sprintf("%%%s", t->mangled);
        case TY_MODULE:
            return "ptr";
    }
    return "ptr";
}

static const char* llvm_mem_ty(Type* t) {
    if (t && t->kind == TY_BOOL) return "i8";
    return llvm_reg_ty(t);
}

static const char* llvm_zero(Type* t) {
    if (!t) return "null";
    switch (t->kind) {
        case TY_BOOL: return "false";
        case TY_I8: case TY_I16: case TY_I32: case TY_I64:
        case TY_U8: case TY_U16: case TY_U32: case TY_U64:
            return "0";
        case TY_F32: case TY_F64:
            return "0.0";
        case TY_TUPLE:
            return "zeroinitializer";
        case TY_CLASS:
            return t->cls->is_value_type ? "zeroinitializer" : "null";
        case TY_OPTIONAL:
            return type_is_heap_ref(t->elem) ? "null" : "zeroinitializer";
        default:
            return "null";
    }
}

static const char* llvm_mem_zero(Type* t) {
    if (t && t->kind == TY_BOOL) return "0";
    return llvm_zero(t);
}

static const char* cg_reg_to_mem(const char* reg, Type* t) {
    if (t && t->kind == TY_BOOL) {
        const char* r = cg_reg();
        cg_emit("%s = zext i1 %s to i8", r, reg);
        return r;
    }
    return reg;
}

static const char* cg_mem_to_reg(const char* mem, Type* t) {
    if (t && t->kind == TY_BOOL) {
        const char* r = cg_reg();
        cg_emit("%s = trunc i8 %s to i1", r, mem);
        return r;
    }
    return mem;
}

static const char* cg_coerce(const char* val, Type* from, Type* to) {
    if (!from || !to || type_eq(from, to)) return val;
    if (from->kind == TY_OPTIONAL && type_eq(from->elem, to)) {
        if (type_is_heap_ref(to)) return val;
        const char* mv = cg_reg();
        cg_emit("%s = extractvalue %s %s, 1", mv, llvm_reg_ty(from), val);
        return cg_mem_to_reg(mv, to);
    }
    if (to->kind == TY_OPTIONAL) {
        if (from->kind == TY_NONE_LIT || from->kind == TY_VOID) {
            return llvm_zero(to);
        }
        if (type_is_heap_ref(to->elem)) {
            return cg_coerce(val, from, to->elem);
        }
        const char* inner = cg_coerce(val, from, to->elem);
        const char* minner = cg_reg_to_mem(inner, to->elem);
        const char* r1 = cg_reg();
        const char* r2 = cg_reg();
        cg_emit("%s = insertvalue %s zeroinitializer, i8 1, 0", r1, llvm_reg_ty(to));
        cg_emit("%s = insertvalue %s %s, %s %s, 1", r2, llvm_reg_ty(to), r1, llvm_mem_ty(to->elem), minner);
        return r2;
    }
    if (from->kind == TY_BOOL && type_is_int(to)) {
        const char* r = cg_reg();
        cg_emit("%s = zext i1 %s to %s", r, val, llvm_reg_ty(to));
        return r;
    }
    if (type_is_int(from) && to->kind == TY_BOOL) {
        const char* r = cg_reg();
        cg_emit("%s = icmp ne %s %s, 0", r, llvm_reg_ty(from), val);
        return r;
    }
    if (type_is_int(from) && type_is_int(to)) {
        if (from->kind == to->kind) return val;
        int fb = (from->kind == TY_I8 || from->kind == TY_U8) ? 8 : (from->kind == TY_I16 || from->kind == TY_U16) ? 16 :
                 (from->kind == TY_I32 || from->kind == TY_U32) ? 32 : 64;
        int tb = (to->kind == TY_I8 || to->kind == TY_U8) ? 8 : (to->kind == TY_I16 || to->kind == TY_U16) ? 16 :
                 (to->kind == TY_I32 || to->kind == TY_U32) ? 32 : 64;
        if (fb == tb) return val;
        const char* r = cg_reg();
        if (fb < tb) {
            cg_emit("%s = %s %s %s to %s", r, type_is_unsigned_int(from) ? "zext" : "sext",
                    llvm_reg_ty(from), val, llvm_reg_ty(to));
        } else {
            cg_emit("%s = trunc %s %s to %s", r, llvm_reg_ty(from), val, llvm_reg_ty(to));
        }
        return r;
    }
    if (type_is_int(from) && type_is_float(to)) {
        const char* r = cg_reg();
        cg_emit("%s = %s %s %s to %s", r, type_is_unsigned_int(from) ? "uitofp" : "sitofp",
                llvm_reg_ty(from), val, llvm_reg_ty(to));
        return r;
    }
    if (from->kind == TY_BOOL && type_is_float(to)) {
        const char* r = cg_reg();
        cg_emit("%s = uitofp i1 %s to %s", r, val, llvm_reg_ty(to));
        return r;
    }
    if (from->kind == TY_F32 && to->kind == TY_F64) {
        const char* r = cg_reg();
        cg_emit("%s = fpext float %s to double", r, val);
        return r;
    }
    if (from->kind == TY_F64 && to->kind == TY_F32) {
        const char* r = cg_reg();
        cg_emit("%s = fptrunc double %s to float", r, val);
        return r;
    }
    if (type_is_float(from) && type_is_int(to)) {
        const char* r = cg_reg();
        cg_emit("%s = %s %s %s to %s", r, type_is_unsigned_int(to) ? "fptoui" : "fptosi",
                llvm_reg_ty(from), val, llvm_reg_ty(to));
        return r;
    }
    return val;
}

static const char* cg_truthy(const char* val, Type* t) {
    if (!t || t->kind == TY_BOOL) return val;
    const char* r = cg_reg();
    if (type_is_int(t)) {
        cg_emit("%s = icmp ne %s %s, 0", r, llvm_reg_ty(t), val);
        return r;
    }
    if (type_is_float(t)) {
        cg_emit("%s = fcmp one %s %s, 0.0", r, llvm_reg_ty(t), val);
        return r;
    }
    if (t->kind == TY_STR || t->kind == TY_BYTES || t->kind == TY_LIST || t->kind == TY_SET || t->kind == TY_DICT) {
        const char* lptr = cg_reg();
        const char* len = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", lptr,
                t->kind == TY_STR ? "%StrictaStr" : t->kind == TY_BYTES ? "%StrictaBytes" :
                t->kind == TY_LIST ? arena_sprintf("%%%s", t->mangled) :
                t->kind == TY_SET ? arena_sprintf("%%%s", t->mangled) : arena_sprintf("%%%s", t->mangled),
                val,
                t->kind == TY_SET ? 2 : (t->kind == TY_DICT ? 3 : 0));
        cg_emit("%s = load i32, ptr %s", len, lptr);
        cg_emit("%s = icmp ne i32 %s, 0", r, len);
        return r;
    }
    if (t->kind == TY_OPTIONAL) {
        if (type_is_heap_ref(t->elem)) {
            cg_emit("%s = icmp ne ptr %s, null", r, val);
            return r;
        }
        cg_emit("%s = extractvalue %s %s, 0", r, llvm_reg_ty(t), val);
        const char* b = cg_reg();
        cg_emit("%s = icmp ne i8 %s, 0", b, r);
        return b;
    }
    if (t->kind == TY_CLASS && !t->cls->is_value_type) {
        cg_emit("%s = icmp ne ptr %s, null", r, val);
        return r;
    }
    if (t->kind == TY_NONE_LIT) return "false";
    return "true";
}

static const char* cg_cstr_lit(const char* s, size_t len) {
    for (int i = 0; i < g_cg.lit_count; i++) {
        if (g_cg.lit_lens[i] == len && memcmp(g_cg.lit_texts[i], s, len) == 0) {
            return g_cg.lit_syms[i];
        }
    }
    int id = ++g_cg.str_id;
    const char* sym = arena_sprintf("@.cstr.%d", id);
    sb_printf(&g_cg.strs, "%s = private unnamed_addr constant [%zu x i8] c\"", sym, len + 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c >= 32 && c < 127 && c != '"' && c != '\\') {
            sb_append_n(&g_cg.strs, (const char*)&c, 1);
        } else {
            sb_printf(&g_cg.strs, "\\%02X", c);
        }
    }
    sb_append(&g_cg.strs, "\\00\"\n");

    if (g_cg.lit_count >= g_cg.lit_cap) {
        g_cg.lit_cap = g_cg.lit_cap ? g_cg.lit_cap * 2 : 64;
        g_cg.lit_texts = (const char**)realloc(g_cg.lit_texts, sizeof(char*) * (size_t)g_cg.lit_cap);
        g_cg.lit_lens = (size_t*)realloc(g_cg.lit_lens, sizeof(size_t) * (size_t)g_cg.lit_cap);
        g_cg.lit_syms = (const char**)realloc(g_cg.lit_syms, sizeof(char*) * (size_t)g_cg.lit_cap);
    }
    g_cg.lit_texts[g_cg.lit_count] = arena_strndup(s, len);
    g_cg.lit_lens[g_cg.lit_count] = len;
    g_cg.lit_syms[g_cg.lit_count] = sym;
    g_cg.lit_count++;
    return sym;
}

static uint32_t c_fnv1a(const char* s, size_t len) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h ? h : 1u;
}

static const char* cg_strictastr_lit(const char* s, size_t len) {
    const char* csym = cg_cstr_lit(s, len);
    int id = ++g_cg.str_id;
    const char* tsym = arena_sprintf("@.tstr.%d", id);
    uint32_t h = c_fnv1a(s, len);
    sb_printf(&g_cg.strs, "%s = private constant %%StrictaStr { i32 %zu, i32 %u, ptr %s }\n",
              tsym, len, h, csym);
    return tsym;
}

static void emit_hash_eq_for_type(Type* t) {
    for (int i = 0; i < g_cg.emitted_hash_eq.count; i++) {
        if (type_eq(g_cg.emitted_hash_eq.items[i], t)) return;
    }
    reg_add_unique(&g_cg.emitted_hash_eq, t);

    if (t->kind == TY_TUPLE) {
        for (int i = 0; i < t->tuple_count; i++) emit_hash_eq_for_type(t->tuple_elems[i]);
    } else if (t->kind == TY_CLASS && t->cls->is_value_type) {
        for (int i = 0; i < t->cls->field_count; i++) emit_hash_eq_for_type(t->cls->fields[i].type);
    }

    const char* mty = llvm_mem_ty(t);
    const char* m = t->mangled;
    StrBuf* b = &g_cg.mono;

    /* Hash helper */
    sb_printf(b, "define internal i64 @_hash_%s(%s %%v) alwaysinline {\nentry:\n", m, mty);
    if (type_is_int(t) || t->kind == TY_BOOL) {
        const char* v64 = (t->kind == TY_I64 || t->kind == TY_U64) ? "%v" : NULL;
        if (!v64) {
            sb_printf(b, "  %%v64 = %s %s %%v to i64\n",
                      (t->kind == TY_BOOL || type_is_unsigned_int(t)) ? "zext" : "sext", mty);
            v64 = "%v64";
        }
        sb_printf(b, "  %%s1 = lshr i64 %s, 30\n", v64);
        sb_printf(b, "  %%x1 = xor i64 %s, %%s1\n", v64);
        sb_printf(b, "  %%m1 = mul i64 %%x1, -4658895280553007687\n"); /* 0xbf58476d1ce4e5b9 */
        sb_printf(b, "  %%s2 = lshr i64 %%m1, 27\n");
        sb_printf(b, "  %%x2 = xor i64 %%m1, %%s2\n");
        sb_printf(b, "  %%m2 = mul i64 %%x2, -7723592293110705685\n"); /* 0x94d049bb133111eb */
        sb_printf(b, "  %%s3 = lshr i64 %%m2, 31\n");
        sb_printf(b, "  %%h = xor i64 %%m2, %%s3\n");
        sb_printf(b, "  ret i64 %%h\n}\n\n");
    } else if (t->kind == TY_F64 || t->kind == TY_F32) {
        if (t->kind == TY_F32) {
            sb_append(b, "  %ext = fpext float %v to double\n  %v64 = bitcast double %ext to i64\n");
        } else {
            sb_append(b, "  %v64 = bitcast double %v to i64\n");
        }
        sb_append(b, "  %s1 = lshr i64 %v64, 30\n  %x1 = xor i64 %v64, %s1\n"
                     "  %m1 = mul i64 %x1, -4658895280553007687\n  %h = xor i64 %m1, 0\n  ret i64 %h\n}\n\n");
    } else if (t->kind == TY_STR) {
        sb_append(b, "  %h = call i64 @stricta_str_hash(ptr %v)\n  ret i64 %h\n}\n\n");
    } else if (t->kind == TY_TUPLE) {
        sb_append(b, "  %acc0 = add i64 0, 1469598103934665603\n");
        for (int i = 0; i < t->tuple_count; i++) {
            sb_printf(b, "  %%f%d = extractvalue %s %%v, %d\n", i, mty, i);
            sb_printf(b, "  %%hf%d = call i64 @_hash_%s(%s %%f%d)\n", i, t->tuple_elems[i]->mangled, llvm_mem_ty(t->tuple_elems[i]), i);
            sb_printf(b, "  %%acc%d_a = xor i64 %%acc%d, %%hf%d\n", i + 1, i, i);
            sb_printf(b, "  %%acc%d = mul i64 %%acc%d_a, 1099511628211\n", i + 1, i + 1);
        }
        sb_printf(b, "  ret i64 %%acc%d\n}\n\n", t->tuple_count);
    } else if (t->kind == TY_CLASS && t->cls->is_value_type) {
        sb_append(b, "  %acc0 = add i64 0, 1469598103934665603\n");
        for (int i = 0; i < t->cls->field_count; i++) {
            Type* ft = t->cls->fields[i].type;
            sb_printf(b, "  %%f%d = extractvalue %s %%v, %d\n", i, mty, t->cls->fields[i].slot_index);
            sb_printf(b, "  %%hf%d = call i64 @_hash_%s(%s %%f%d)\n", i, ft->mangled, llvm_mem_ty(ft), i);
            sb_printf(b, "  %%acc%d_a = xor i64 %%acc%d, %%hf%d\n", i + 1, i, i);
            sb_printf(b, "  %%acc%d = mul i64 %%acc%d_a, 1099511628211\n", i + 1, i + 1);
        }
        sb_printf(b, "  ret i64 %%acc%d\n}\n\n", t->cls->field_count);
    } else {
        sb_append(b, "  %p = ptrtoint ptr %v to i64\n  ret i64 %p\n}\n\n");
    }

    /* Eq helper */
    sb_printf(b, "define internal i1 @_eq_%s(%s %%a, %s %%b) alwaysinline {\nentry:\n", m, mty, mty);
    if (type_is_int(t) || t->kind == TY_BOOL) {
        sb_printf(b, "  %%r = icmp eq %s %%a, %%b\n  ret i1 %%r\n}\n\n", mty);
    } else if (type_is_float(t)) {
        sb_printf(b, "  %%r = fcmp oeq %s %%a, %%b\n  ret i1 %%r\n}\n\n", mty);
    } else if (t->kind == TY_STR) {
        sb_append(b, "  %r32 = call i32 @stricta_str_eq(ptr %a, ptr %b)\n  %r = icmp ne i32 %r32, 0\n  ret i1 %r\n}\n\n");
    } else if (t->kind == TY_BYTES) {
        sb_append(b, "  %r32 = call i32 @stricta_bytes_eq(ptr %a, ptr %b)\n  %r = icmp ne i32 %r32, 0\n  ret i1 %r\n}\n\n");
    } else if (t->kind == TY_BIGINT) {
        sb_append(b, "  %c = call i32 @stricta_bigint_cmp(ptr %a, ptr %b)\n  %r = icmp eq i32 %c, 0\n  ret i1 %r\n}\n\n");
    } else if (t->kind == TY_LIST) {
        sb_printf(b, "  %%r = call i1 @_%s_eq(ptr %%a, ptr %%b)\n  ret i1 %%r\n}\n\n", m);
    } else if (t->kind == TY_TUPLE) {
        if (t->tuple_count == 0) {
            sb_append(b, "  ret i1 true\n}\n\n");
        } else {
            sb_append(b, "  %eq0 = add i1 true, false\n");
            for (int i = 0; i < t->tuple_count; i++) {
                sb_printf(b, "  %%fa%d = extractvalue %s %%a, %d\n", i, mty, i);
                sb_printf(b, "  %%fb%d = extractvalue %s %%b, %d\n", i, mty, i);
                sb_printf(b, "  %%feq%d = call i1 @_eq_%s(%s %%fa%d, %s %%fb%d)\n",
                          i, t->tuple_elems[i]->mangled, llvm_mem_ty(t->tuple_elems[i]), i, llvm_mem_ty(t->tuple_elems[i]), i);
                sb_printf(b, "  %%eq%d = and i1 %%eq%d, %%feq%d\n", i + 1, i, i);
            }
            sb_printf(b, "  ret i1 %%eq%d\n}\n\n", t->tuple_count);
        }
    } else if (t->kind == TY_CLASS) {
        FuncInfo* eqm = find_method_in_class(t->cls, "__eq__");
        if (eqm) {
            sb_printf(b, "  %%r = call i1 %s(%s %%a, %s %%b)\n  ret i1 %%r\n}\n\n", eqm->llvm_name, mty, mty);
        } else if (t->cls->is_value_type) {
            sb_append(b, "  %eq0 = add i1 true, false\n");
            for (int i = 0; i < t->cls->field_count; i++) {
                Type* ft = t->cls->fields[i].type;
                sb_printf(b, "  %%fa%d = extractvalue %s %%a, %d\n", i, mty, t->cls->fields[i].slot_index);
                sb_printf(b, "  %%fb%d = extractvalue %s %%b, %d\n", i, mty, t->cls->fields[i].slot_index);
                sb_printf(b, "  %%feq%d = call i1 @_eq_%s(%s %%fa%d, %s %%fb%d)\n",
                          i, ft->mangled, llvm_mem_ty(ft), i, llvm_mem_ty(ft), i);
                sb_printf(b, "  %%eq%d = and i1 %%eq%d, %%feq%d\n", i + 1, i, i);
            }
            sb_printf(b, "  ret i1 %%eq%d\n}\n\n", t->cls->field_count);
        } else {
            sb_append(b, "  %r = icmp eq ptr %a, %b\n  ret i1 %r\n}\n\n");
        }
    } else {
        sb_append(b, "  %r = icmp eq ptr %a, %b\n  ret i1 %r\n}\n\n");
    }
}

static void emit_mono_list(Type* lt) {
    Type* el = lt->elem;
    emit_hash_eq_for_type(el);
    const char* m = lt->mangled;
    const char* sty = arena_sprintf("%%%s", m);
    const char* mty = llvm_mem_ty(el);
    StrBuf* b = &g_cg.mono;
    const char* esz = arena_sprintf("ptrtoint (ptr getelementptr (%s, ptr null, i32 1) to i64)", mty);

    /* new */
    sb_printf(b, "define internal ptr @_%s_new(i32 %%cap) alwaysinline {\nentry:\n", m);
    sb_printf(b, "  %%sz = ptrtoint ptr getelementptr (%s, ptr null, i32 1) to i64\n", sty);
    sb_append(b, "  %p = call ptr @malloc(i64 %sz)\n");
    sb_append(b, "  %c = call i32 @llvm.smax.i32(i32 %cap, i32 4)\n");
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%p, i32 0, i32 0\n  store i32 0, ptr %%lp\n", sty);
    sb_printf(b, "  %%cp = getelementptr %s, ptr %%p, i32 0, i32 1\n  store i32 %%c, ptr %%cp\n", sty);
    sb_append(b, "  %c64 = sext i32 %c to i64\n");
    sb_printf(b, "  %%bytes = mul i64 %%c64, %s\n", esz);
    sb_append(b, "  %arr = call ptr @malloc(i64 %bytes)\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%p, i32 0, i32 2\n  store ptr %%arr, ptr %%ip\n", sty);
    sb_append(b, "  ret ptr %p\n}\n\n");

    /* append (supports negative cap = stack-backed inline buffer that spills cleanly to heap on overflow) */
    sb_printf(b, "define internal void @_%s_append(ptr %%l, %s %%val) alwaysinline {\nentry:\n", m, mty);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%cp = getelementptr %s, ptr %%l, i32 0, i32 1\n  %%cap = load i32, ptr %%cp\n", sty);
    sb_append(b, "  %is_stk = icmp slt i32 %cap, 0\n  %neg_cap = sub i32 0, %cap\n  %acap = select i1 %is_stk, i32 %neg_cap, i32 %cap\n");
    sb_append(b, "  %full = icmp sge i32 %len, %acap\n  br i1 %full, label %grow, label %ok\ngrow:\n");
    sb_append(b, "  %ncap = mul i32 %acap, 2\n  store i32 %ncap, ptr %cp\n  %nc64 = sext i32 %ncap to i64\n");
    sb_printf(b, "  %%nb = mul i64 %%nc64, %s\n", esz);
    sb_printf(b, "  %%ip1 = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%old = load ptr, ptr %%ip1\n", sty);
    sb_append(b, "  br i1 %is_stk, label %grow_stk, label %grow_heap\ngrow_stk:\n");
    sb_append(b, "  %narr_s = call ptr @malloc(i64 %nb)\n  %l64 = sext i32 %len to i64\n");
    sb_printf(b, "  %%ob = mul i64 %%l64, %s\n  call void @llvm.memcpy.p0.p0.i64(ptr %%narr_s, ptr %%old, i64 %%ob, i1 false)\n", esz);
    sb_append(b, "  store ptr %narr_s, ptr %ip1\n  br label %ok\ngrow_heap:\n");
    sb_append(b, "  %narr = call ptr @realloc(ptr %old, i64 %nb)\n  store ptr %narr, ptr %ip1\n  br label %ok\nok:\n");
    sb_printf(b, "  %%ip2 = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip2\n", sty);
    sb_printf(b, "  %%slot = getelementptr %s, ptr %%arr, i32 %%len\n  store %s %%val, ptr %%slot\n", mty, mty);
    sb_append(b, "  %nlen = add i32 %len, 1\n  store i32 %nlen, ptr %lp\n  ret void\n}\n\n");

    /* get */
    sb_printf(b, "define internal %s @_%s_get(ptr %%l, i64 %%idx) alwaysinline {\nentry:\n", mty, m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len32 = load i32, ptr %%lp\n", sty);
    sb_append(b, "  %len = sext i32 %len32 to i64\n  %neg = icmp slt i64 %idx, 0\n  %adj = add i64 %idx, %len\n");
    sb_append(b, "  %i = select i1 %neg, i64 %adj, i64 %idx\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%slot = getelementptr %s, ptr %%arr, i64 %%i\n  %%v = load %s, ptr %%slot\n  ret %s %%v\n}\n\n", mty, mty, mty);

    /* set */
    sb_printf(b, "define internal void @_%s_set(ptr %%l, i64 %%idx, %s %%val) alwaysinline {\nentry:\n", m, mty);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len32 = load i32, ptr %%lp\n", sty);
    sb_append(b, "  %len = sext i32 %len32 to i64\n  %neg = icmp slt i64 %idx, 0\n  %adj = add i64 %idx, %len\n");
    sb_append(b, "  %i = select i1 %neg, i64 %adj, i64 %idx\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%slot = getelementptr %s, ptr %%arr, i64 %%i\n  store %s %%val, ptr %%slot\n  ret void\n}\n\n", mty, mty);

    /* del */
    sb_printf(b, "define internal void @_%s_del(ptr %%l, i64 %%idx) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len32 = load i32, ptr %%lp\n", sty);
    sb_append(b, "  %len = sext i32 %len32 to i64\n  %neg = icmp slt i64 %idx, 0\n  %adj = add i64 %idx, %len\n");
    sb_append(b, "  %i0 = select i1 %neg, i64 %adj, i64 %idx\n  %i = trunc i64 %i0 to i32\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_append(b, "  %nlen = sub i32 %len32, 1\n  br label %loop\nloop:\n  %k = phi i32 [ %i, %entry ], [ %kn, %step ]\n");
    sb_append(b, "  %c = icmp slt i32 %k, %nlen\n  br i1 %c, label %step, label %done\nstep:\n  %kn = add i32 %k, 1\n");
    sb_printf(b, "  %%sp = getelementptr %s, ptr %%arr, i32 %%kn\n  %%v = load %s, ptr %%sp\n", mty, mty);
    sb_printf(b, "  %%dp = getelementptr %s, ptr %%arr, i32 %%k\n  store %s %%v, ptr %%dp\n  br label %%loop\n", mty, mty);
    sb_append(b, "done:\n  store i32 %nlen, ptr %lp\n  ret void\n}\n\n");

    /* pop */
    sb_printf(b, "define internal %s @_%s_pop(ptr %%l, i64 %%idx) {\nentry:\n", mty, m);
    sb_printf(b, "  %%v = call %s @_%s_get(ptr %%l, i64 %%idx)\n", mty, m);
    sb_printf(b, "  call void @_%s_del(ptr %%l, i64 %%idx)\n  ret %s %%v\n}\n\n", m, mty);

    /* slice */
    sb_printf(b, "define internal ptr @_%s_slice(ptr %%l, i64 %%s0, i64 %%e0) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len32 = load i32, ptr %%lp\n  %%len = sext i32 %%len32 to i64\n", sty);
    sb_append(b, "  %sneg = icmp slt i64 %s0, 0\n  %sadd = add i64 %s0, %len\n  %s1 = select i1 %sneg, i64 %sadd, i64 %s0\n");
    sb_append(b, "  %s2 = call i64 @llvm.smax.i64(i64 %s1, i64 0)\n  %s = call i64 @llvm.smin.i64(i64 %s2, i64 %len)\n");
    sb_append(b, "  %eneg = icmp slt i64 %e0, 0\n  %eadd = add i64 %e0, %len\n  %e1 = select i1 %eneg, i64 %eadd, i64 %e0\n");
    sb_append(b, "  %e2 = call i64 @llvm.smax.i64(i64 %e1, i64 0)\n  %e = call i64 @llvm.smin.i64(i64 %e2, i64 %len)\n");
    sb_append(b, "  %cnt64 = sub i64 %e, %s\n  %cnt32 = trunc i64 %cnt64 to i32\n");
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%cnt32)\n", m);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_append(b, "  br label %loop\nloop:\n  %k = phi i64 [ %s, %entry ], [ %kn, %body ]\n  %c = icmp slt i64 %k, %e\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%arr, i64 %%k\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  call void @_%s_append(ptr %%res, %s %%v)\n  %%kn = add i64 %%k, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty);

    /* copy */
    sb_printf(b, "define internal ptr @_%s_copy(ptr %%l) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n", m);
    sb_printf(b, "  %%res_lp = getelementptr %s, ptr %%res, i32 0, i32 0\n  store i32 %%len, ptr %%res_lp\n", sty);
    sb_printf(b, "  %%ip1 = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%src = load ptr, ptr %%ip1\n", sty);
    sb_printf(b, "  %%ip2 = getelementptr %s, ptr %%res, i32 0, i32 2\n  %%dst = load ptr, ptr %%ip2\n", sty);
    sb_append(b, "  %len64 = sext i32 %len to i64\n");
    sb_printf(b, "  %%nb = mul i64 %%len64, %s\n", esz);
    sb_append(b, "  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 %nb, i1 false)\n  ret ptr %res\n}\n\n");

    /* extend */
    sb_printf(b, "define internal void @_%s_extend(ptr %%l, ptr %%o) {\nentry:\n", m);
    sb_printf(b, "  %%olp = getelementptr %s, ptr %%o, i32 0, i32 0\n  %%olen = load i32, ptr %%olp\n", sty);
    sb_printf(b, "  %%oip = getelementptr %s, ptr %%o, i32 0, i32 2\n  %%oarr = load ptr, ptr %%oip\n", sty);
    sb_append(b, "  br label %loop\nloop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %olen\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%oarr, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  call void @_%s_append(ptr %%l, %s %%v)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n", m, mty);

    /* concat */
    sb_printf(b, "define internal ptr @_%s_concat(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%res = call ptr @_%s_copy(ptr %%a)\n  call void @_%s_extend(ptr %%res, ptr %%b)\n  ret ptr %%res\n}\n\n", m, m);

    /* index & contains & count */
    sb_printf(b, "define internal i64 @_%s_index(ptr %%l, %s %%val) {\nentry:\n", m, mty);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_append(b, "  br label %loop\nloop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %nf\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%arr, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%eq = call i1 @_eq_%s(%s %%v, %s %%val)\n  br i1 %%eq, label %%hit, label %%next\nhit:\n", el->mangled, mty, mty);
    sb_append(b, "  %i64 = sext i32 %i to i64\n  ret i64 %i64\nnext:\n  %in = add i32 %i, 1\n  br label %loop\nnf:\n  ret i64 -1\n}\n\n");

    sb_printf(b, "define internal i1 @_%s_contains(ptr %%l, %s %%val) {\nentry:\n", m, mty);
    sb_printf(b, "  %%idx = call i64 @_%s_index(ptr %%l, %s %%val)\n  %%r = icmp sge i64 %%idx, 0\n  ret i1 %%r\n}\n\n", m, mty);

    sb_printf(b, "define internal i64 @_%s_count(ptr %%l, %s %%val) {\nentry:\n", m, mty);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_append(b, "  br label %loop\nloop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %acc = phi i64 [ 0, %entry ], [ %nacc, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%arr, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%eq = call i1 @_eq_%s(%s %%v, %s %%val)\n  %%add = zext i1 %%eq to i64\n  %%nacc = add i64 %%acc, %%add\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret i64 %%acc\n}\n\n", el->mangled, mty, mty);

    /* remove */
    sb_printf(b, "define internal void @_%s_remove(ptr %%l, %s %%val) {\nentry:\n", m, mty);
    sb_printf(b, "  %%idx = call i64 @_%s_index(ptr %%l, %s %%val)\n  %%ok = icmp sge i64 %%idx, 0\n  br i1 %%ok, label %%do, label %%done\ndo:\n", m, mty);
    sb_printf(b, "  call void @_%s_del(ptr %%l, i64 %%idx)\n  br label %%done\ndone:\n  ret void\n}\n\n", m);

    /* insert */
    sb_printf(b, "define internal void @_%s_insert(ptr %%l, i64 %%idx, %s %%val) {\nentry:\n", m, mty);
    sb_printf(b, "  %%z = alloca %s\n  store %s zeroinitializer, ptr %%z\n  %%zv = load %s, ptr %%z\n", mty, mty, mty);
    sb_printf(b, "  call void @_%s_append(ptr %%l, %s %%zv)\n", m, mty);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len = load i32, ptr %%lp\n  %%last = sub i32 %%len, 1\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_append(b, "  %i32 = trunc i64 %idx to i32\n  %p0 = call i32 @llvm.smax.i32(i32 %i32, i32 0)\n  %pos = call i32 @llvm.smin.i32(i32 %p0, i32 %last)\n");
    sb_append(b, "  br label %loop\nloop:\n  %k = phi i32 [ %last, %entry ], [ %kp, %step ]\n  %c = icmp sgt i32 %k, %pos\n  br i1 %c, label %step, label %done\nstep:\n  %kp = sub i32 %k, 1\n");
    sb_printf(b, "  %%sp = getelementptr %s, ptr %%arr, i32 %%kp\n  %%v = load %s, ptr %%sp\n", mty, mty);
    sb_printf(b, "  %%dp = getelementptr %s, ptr %%arr, i32 %%k\n  store %s %%v, ptr %%dp\n  br label %%loop\n", mty, mty);
    sb_printf(b, "done:\n  %%ins = getelementptr %s, ptr %%arr, i32 %%pos\n  store %s %%val, ptr %%ins\n  ret void\n}\n\n", mty, mty);

    /* reverse */
    sb_printf(b, "define internal void @_%s_reverse(ptr %%l) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len = load i32, ptr %%lp\n  %%r0 = sub i32 %%len, 1\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_append(b, "  br label %loop\nloop:\n  %lo = phi i32 [ 0, %entry ], [ %nlo, %body ]\n  %hi = phi i32 [ %r0, %entry ], [ %nhi, %body ]\n  %c = icmp slt i32 %lo, %hi\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%pa = getelementptr %s, ptr %%arr, i32 %%lo\n  %%pb = getelementptr %s, ptr %%arr, i32 %%hi\n", mty, mty);
    sb_printf(b, "  %%va = load %s, ptr %%pa\n  %%vb = load %s, ptr %%pb\n  store %s %%vb, ptr %%pa\n  store %s %%va, ptr %%pb\n", mty, mty, mty, mty);
    sb_append(b, "  %nlo = add i32 %lo, 1\n  %nhi = sub i32 %hi, 1\n  br label %loop\ndone:\n  ret void\n}\n\n");

    /* sort (insertion sort — clean and works for ints, floats, strings) */
    sb_printf(b, "define internal void @_%s_sort(ptr %%l) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_append(b, "  br label %outer\nouter:\n  %i = phi i32 [ 1, %entry ], [ %in, %outer_end ]\n  %oc = icmp slt i32 %i, %len\n  br i1 %oc, label %outer_body, label %done\nouter_body:\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%arr, i32 %%i\n  %%key = load %s, ptr %%kp\n  %%j0 = sub i32 %%i, 1\n  br label %%inner\n", mty, mty);
    sb_append(b, "inner:\n  %j = phi i32 [ %j0, %outer_body ], [ %jn, %shift ]\n  %ge0 = icmp sge i32 %j, 0\n  br i1 %ge0, label %cmp, label %outer_end\ncmp:\n");
    sb_printf(b, "  %%jp = getelementptr %s, ptr %%arr, i32 %%j\n  %%vj = load %s, ptr %%jp\n", mty, mty);
    if (type_is_int(el) || el->kind == TY_BOOL) {
        sb_printf(b, "  %%gt = icmp %s %s %%vj, %%key\n", type_is_unsigned_int(el) ? "ugt" : "sgt", mty);
    } else if (type_is_float(el)) {
        sb_printf(b, "  %%gt = fcmp ogt %s %%vj, %%key\n", mty);
    } else if (el->kind == TY_STR) {
        sb_append(b, "  %sc = call i32 @stricta_str_cmp(ptr %vj, ptr %key)\n  %gt = icmp sgt i32 %sc, 0\n");
    } else {
        sb_append(b, "  %gt = icmp sgt i32 0, 1\n");
    }
    sb_append(b, "  br i1 %gt, label %shift, label %outer_end\nshift:\n  %j1 = add i32 %j, 1\n");
    sb_printf(b, "  %%j1p = getelementptr %s, ptr %%arr, i32 %%j1\n  store %s %%vj, ptr %%j1p\n  %%jn = sub i32 %%j, 1\n  br label %%inner\n", mty, mty);
    sb_append(b, "outer_end:\n  %jf = phi i32 [ %j, %inner ], [ %j, %cmp ]\n  %jf1 = add i32 %jf, 1\n");
    sb_printf(b, "  %%dp = getelementptr %s, ptr %%arr, i32 %%jf1\n  store %s %%key, ptr %%dp\n  %%in = add i32 %%i, 1\n  br label %%outer\ndone:\n  ret void\n}\n\n", mty, mty);

    /* eq */
    sb_printf(b, "define internal i1 @_%s_eq(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_append(b, "  %same = icmp eq ptr %a, %b\n  br i1 %same, label %ret_t, label %chk\nchk:\n");
    sb_printf(b, "  %%lap = getelementptr %s, ptr %%a, i32 0, i32 0\n  %%la = load i32, ptr %%lap\n", sty);
    sb_printf(b, "  %%lbp = getelementptr %s, ptr %%b, i32 0, i32 0\n  %%lb = load i32, ptr %%lbp\n", sty);
    sb_append(b, "  %leq = icmp eq i32 %la, %lb\n  br i1 %leq, label %iter, label %ret_f\niter:\n");
    sb_printf(b, "  %%aip = getelementptr %s, ptr %%a, i32 0, i32 2\n  %%aa = load ptr, ptr %%aip\n", sty);
    sb_printf(b, "  %%bip = getelementptr %s, ptr %%b, i32 0, i32 2\n  %%ba = load ptr, ptr %%bip\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %iter ], [ %in, %next ]\n  %c = icmp slt i32 %i, %la\n  br i1 %c, label %body, label %ret_t\nbody:\n");
    sb_printf(b, "  %%pa = getelementptr %s, ptr %%aa, i32 %%i\n  %%va = load %s, ptr %%pa\n", mty, mty);
    sb_printf(b, "  %%pb = getelementptr %s, ptr %%ba, i32 %%i\n  %%vb = load %s, ptr %%pb\n", mty, mty);
    sb_printf(b, "  %%veq = call i1 @_eq_%s(%s %%va, %s %%vb)\n  br i1 %%veq, label %%next, label %%ret_f\nnext:\n", el->mangled, mty, mty);
    sb_append(b, "  %in = add i32 %i, 1\n  br label %loop\nret_t:\n  ret i1 true\nret_f:\n  ret i1 false\n}\n\n");
}

static void emit_mono_set(Type* st) {
    Type* el = st->elem;
    emit_hash_eq_for_type(el);
    const char* m = st->mangled;
    const char* sty = arena_sprintf("%%%s", m);
    const char* mty = llvm_mem_ty(el);
    StrBuf* b = &g_cg.mono;
    const char* esz = arena_sprintf("ptrtoint (ptr getelementptr (%s, ptr null, i32 1) to i64)", mty);

    /* %Set_T = type { ptr items, ptr buckets, i32 len, i32 cap, i32 ht_cap } */
    sb_printf(b, "define internal ptr @_%s_new(i32 %%init_cap) {\nentry:\n", m);
    sb_printf(b, "  %%sz = ptrtoint ptr getelementptr (%s, ptr null, i32 1) to i64\n  %%s = call ptr @stricta_alloc(i64 %%sz)\n", sty);
    sb_append(b, "  %cap = call i32 @llvm.smax.i32(i32 %init_cap, i32 8)\n");
    sb_append(b, "  br label %hloop\nhloop:\n  %hc = phi i32 [ 16, %entry ], [ %hc2, %hstep ]\n  %need = mul i32 %cap, 2\n");
    sb_append(b, "  %lt = icmp slt i32 %hc, %need\n  br i1 %lt, label %hstep, label %hdone\nhstep:\n  %hc2 = mul i32 %hc, 2\n  br label %hloop\nhdone:\n");
    sb_append(b, "  %cap64 = sext i32 %cap to i64\n");
    sb_printf(b, "  %%ib = mul i64 %%cap64, %s\n  %%items = call ptr @stricta_alloc(i64 %%ib)\n", esz);
    sb_append(b, "  %hc64 = sext i32 %hc to i64\n  %bb = mul i64 %hc64, 4\n  %buckets = call ptr @stricta_alloc(i64 %bb)\n");
    sb_append(b, "  br label %iloop\niloop:\n  %i = phi i32 [ 0, %hdone ], [ %in, %istep ]\n  %ic = icmp slt i32 %i, %hc\n  br i1 %ic, label %istep, label %idone\nistep:\n");
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %i\n  store i32 -1, ptr %bp\n  %in = add i32 %i, 1\n  br label %iloop\nidone:\n");
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  store ptr %%items, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  store ptr %%buckets, ptr %%p1\n", sty);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  store i32 0, ptr %%p2\n", sty);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%s, i32 0, i32 3\n  store i32 %%cap, ptr %%p3\n", sty);
    sb_printf(b, "  %%p4 = getelementptr %s, ptr %%s, i32 0, i32 4\n  store i32 %%hc, ptr %%p4\n  ret ptr %%s\n}\n\n", sty);

    /* rebuild */
    sb_printf(b, "define internal void @_%s_rebuild(ptr %%s, i32 %%ncap) {\nentry:\n", m);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  %%oldb = load ptr, ptr %%p1\n  call void @stricta_free(ptr %%oldb)\n", sty);
    sb_printf(b, "  %%p4 = getelementptr %s, ptr %%s, i32 0, i32 4\n  store i32 %%ncap, ptr %%p4\n", sty);
    sb_append(b, "  %nc64 = sext i32 %ncap to i64\n  %bb = mul i64 %nc64, 4\n  %nb = call ptr @stricta_alloc(i64 %bb)\n  store ptr %nb, ptr %p1\n");
    sb_append(b, "  br label %zloop\nzloop:\n  %z = phi i32 [ 0, %entry ], [ %zn, %zstep ]\n  %zc = icmp slt i32 %z, %ncap\n  br i1 %zc, label %zstep, label %zdone\nzstep:\n");
    sb_append(b, "  %zp = getelementptr i32, ptr %nb, i32 %z\n  store i32 -1, ptr %zp\n  %zn = add i32 %z, 1\n  br label %zloop\nzdone:\n");
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  %%len = load i32, ptr %%p2\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%items = load ptr, ptr %%p0\n  %%mask = sub i32 %%ncap, 1\n  br label %%rloop\n", sty);
    sb_append(b, "rloop:\n  %i = phi i32 [ 0, %zdone ], [ %in, %rnext ]\n  %rc = icmp slt i32 %i, %len\n  br i1 %rc, label %rbody, label %rdone\nrbody:\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%items, i32 %%i\n  %%v = load %s, ptr %%ip\n", mty, mty);
    sb_printf(b, "  %%h64 = call i64 @_hash_%s(%s %%v)\n  %%h32 = trunc i64 %%h64 to i32\n  %%b0 = and i32 %%h32, %%mask\n  br label %%probe\n", el->mangled, mty);
    sb_append(b, "probe:\n  %b = phi i32 [ %b0, %rbody ], [ %bn, %pstep ]\n  %bp = getelementptr i32, ptr %nb, i32 %b\n  %cur = load i32, ptr %bp\n  %occ = icmp sge i32 %cur, 0\n  br i1 %occ, label %pstep, label %rnext\npstep:\n  %b1 = add i32 %b, 1\n  %bn = and i32 %b1, %mask\n  br label %probe\nrnext:\n  store i32 %i, ptr %bp\n  %in = add i32 %i, 1\n  br label %rloop\nrdone:\n  ret void\n}\n\n");

    /* find */
    sb_printf(b, "define internal i32 @_%s_find(ptr %%s, %s %%val, ptr %%out_slot) alwaysinline {\nentry:\n", m, mty);
    sb_printf(b, "  %%p4 = getelementptr %s, ptr %%s, i32 0, i32 4\n  %%hc = load i32, ptr %%p4\n  %%mask = sub i32 %%hc, 1\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  %%buckets = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%items = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%h64 = call i64 @_hash_%s(%s %%val)\n  %%h32 = trunc i64 %%h64 to i32\n  %%b0 = and i32 %%h32, %%mask\n  br label %%loop\n", el->mangled, mty);
    sb_append(b, "loop:\n  %b = phi i32 [ %b0, %entry ], [ %bn, %next ]\n  %ft = phi i32 [ -1, %entry ], [ %nft, %next ]\n");
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %b\n  %idx = load i32, ptr %bp\n  %is_empty = icmp eq i32 %idx, -1\n  br i1 %is_empty, label %miss, label %chk_tomb\nmiss:\n");
    sb_append(b, "  %has_ft = icmp sge i32 %ft, 0\n  %slot_m = select i1 %has_ft, i32 %ft, i32 %b\n  %want_s = icmp ne ptr %out_slot, null\n  br i1 %want_s, label %ws, label %ret_m\nws:\n  store i32 %slot_m, ptr %out_slot\n  br label %ret_m\nret_m:\n  ret i32 -1\n");
    sb_append(b, "chk_tomb:\n  %is_tomb = icmp eq i32 %idx, -2\n  br i1 %is_tomb, label %tomb, label %cmp\ntomb:\n  %no_ft = icmp slt i32 %ft, 0\n  %ft2 = select i1 %no_ft, i32 %b, i32 %ft\n  br label %next\ncmp:\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%items, i32 %%idx\n  %%iv = load %s, ptr %%ip\n", mty, mty);
    sb_printf(b, "  %%eq = call i1 @_eq_%s(%s %%iv, %s %%val)\n  br i1 %%eq, label %%hit, label %%next\nhit:\n", el->mangled, mty, mty);
    sb_append(b, "  %want_h = icmp ne ptr %out_slot, null\n  br i1 %want_h, label %wh, label %ret_h\nwh:\n  store i32 %b, ptr %out_slot\n  br label %ret_h\nret_h:\n  ret i32 %idx\n");
    sb_append(b, "next:\n  %nft = phi i32 [ %ft2, %tomb ], [ %ft, %cmp ]\n  %b1 = add i32 %b, 1\n  %bn = and i32 %b1, %mask\n  br label %loop\n}\n\n");

    /* add */
    sb_printf(b, "define internal void @_%s_add(ptr %%s, %s %%val) alwaysinline {\nentry:\n", m, mty);
    sb_append(b, "  %slot_p = alloca i32\n");
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  %%len = load i32, ptr %%p2\n", sty);
    sb_printf(b, "  %%p4 = getelementptr %s, ptr %%s, i32 0, i32 4\n  %%hc = load i32, ptr %%p4\n", sty);
    sb_append(b, "  %l1 = add i32 %len, 1\n  %l2 = mul i32 %l1, 2\n  %need_r = icmp sge i32 %l2, %hc\n  br i1 %need_r, label %reb, label %find\nreb:\n");
    sb_printf(b, "  %%hc2 = mul i32 %%hc, 2\n  call void @_%s_rebuild(ptr %%s, i32 %%hc2)\n  br label %%find\nfind:\n", m);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%s, %s %%val, ptr %%slot_p)\n  %%found = icmp sge i32 %%idx, 0\n  br i1 %%found, label %%done, label %%ins\nins:\n", m, mty);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%s, i32 0, i32 3\n  %%cap = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%full = icmp sge i32 %%len, %%cap\n  br i1 %%full, label %%grow, label %%put\ngrow:\n", sty);
    sb_append(b, "  %ncap = mul i32 %cap, 2\n  store i32 %ncap, ptr %p3\n  %nc64 = sext i32 %ncap to i64\n");
    sb_printf(b, "  %%nb = mul i64 %%nc64, %s\n  %%oldi = load ptr, ptr %%p0\n  %%newi = call ptr @stricta_realloc(ptr %%oldi, i64 %%nb)\n  store ptr %%newi, ptr %%p0\n  br label %%put\nput:\n", esz);
    sb_printf(b, "  %%items = load ptr, ptr %%p0\n  %%ip = getelementptr %s, ptr %%items, i32 %%len\n  store %s %%val, ptr %%ip\n", mty, mty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  %%buckets = load ptr, ptr %%p1\n  %%slot = load i32, ptr %%slot_p\n", sty);
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %slot\n  store i32 %len, ptr %bp\n  store i32 %l1, ptr %p2\n  br label %done\ndone:\n  ret void\n}\n\n");

    /* contains */
    sb_printf(b, "define internal i1 @_%s_contains(ptr %%s, %s %%val) alwaysinline {\nentry:\n", m, mty);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%s, %s %%val, ptr null)\n  %%r = icmp sge i32 %%idx, 0\n  ret i1 %%r\n}\n\n", m, mty);

    /* discard */
    sb_printf(b, "define internal void @_%s_discard(ptr %%s, %s %%val) alwaysinline {\nentry:\n", m, mty);
    sb_append(b, "  %slot_p = alloca i32\n");
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%s, %s %%val, ptr %%slot_p)\n  %%miss = icmp slt i32 %%idx, 0\n  br i1 %%miss, label %%done, label %%rem\nrem:\n", m, mty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  %%buckets = load ptr, ptr %%p1\n  %%slot = load i32, ptr %%slot_p\n", sty);
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %slot\n  store i32 -2, ptr %bp\n");
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  %%len = load i32, ptr %%p2\n  %%last = sub i32 %%len, 1\n", sty);
    sb_append(b, "  %not_last = icmp ne i32 %idx, %last\n  br i1 %not_last, label %swap, label %dec\nswap:\n");
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%items = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%items, i32 %%last\n  %%mv = load %s, ptr %%lp\n", mty, mty);
    sb_printf(b, "  %%dp = getelementptr %s, ptr %%items, i32 %%idx\n  store %s %%mv, ptr %%dp\n", mty, mty);
    sb_append(b, "  %ms_p = alloca i32\n");
    sb_printf(b, "  %%midx = call i32 @_%s_find(ptr %%s, %s %%mv, ptr %%ms_p)\n  %%ms = load i32, ptr %%ms_p\n", m, mty);
    sb_append(b, "  %mbp = getelementptr i32, ptr %buckets, i32 %ms\n  store i32 %idx, ptr %mbp\n  br label %dec\ndec:\n  store i32 %last, ptr %p2\n  br label %done\ndone:\n  ret void\n}\n\n");

    /* pop */
    sb_printf(b, "define internal %s @_%s_pop(ptr %%s) {\nentry:\n", mty, m);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  %%len = load i32, ptr %%p2\n  %%last = sub i32 %%len, 1\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%items = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%last\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  call void @_%s_discard(ptr %%s, %s %%v)\n  ret %s %%v\n}\n\n", m, mty, mty);

    /* clear */
    sb_printf(b, "define internal void @_%s_clear(ptr %%s) {\nentry:\n", m);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  store i32 0, ptr %%p2\n", sty);
    sb_printf(b, "  %%p4 = getelementptr %s, ptr %%s, i32 0, i32 4\n  %%hc = load i32, ptr %%p4\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  %%buckets = load ptr, ptr %%p1\n", sty);
    sb_append(b, "  br label %loop\nloop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %hc\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %i\n  store i32 -1, ptr %bp\n  %in = add i32 %i, 1\n  br label %loop\ndone:\n  ret void\n}\n\n");

    /* copy */
    sb_printf(b, "define internal ptr @_%s_copy(ptr %%s) {\nentry:\n", m);
    sb_printf(b, "  %%sz = ptrtoint ptr getelementptr (%s, ptr null, i32 1) to i64\n  %%c = call ptr @stricta_alloc(i64 %%sz)\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%items = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  %%buckets = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  %%len = load i32, ptr %%p2\n", sty);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%s, i32 0, i32 3\n  %%cap = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p4 = getelementptr %s, ptr %%s, i32 0, i32 4\n  %%hc = load i32, ptr %%p4\n", sty);
    sb_append(b, "  %cap64 = sext i32 %cap to i64\n");
    sb_printf(b, "  %%ib = mul i64 %%cap64, %s\n  %%ni = call ptr @stricta_alloc(i64 %%ib)\n", esz);
    sb_append(b, "  %len64 = sext i32 %len to i64\n");
    sb_printf(b, "  %%lb = mul i64 %%len64, %s\n  call void @llvm.memcpy.p0.p0.i64(ptr %%ni, ptr %%items, i64 %%lb, i1 false)\n", esz);
    sb_append(b, "  %hc64 = sext i32 %hc to i64\n  %bb = mul i64 %hc64, 4\n  %nb = call ptr @stricta_alloc(i64 %bb)\n");
    sb_append(b, "  call void @llvm.memcpy.p0.p0.i64(ptr %nb, ptr %buckets, i64 %bb, i1 false)\n");
    sb_printf(b, "  %%c0 = getelementptr %s, ptr %%c, i32 0, i32 0\n  store ptr %%ni, ptr %%c0\n", sty);
    sb_printf(b, "  %%c1 = getelementptr %s, ptr %%c, i32 0, i32 1\n  store ptr %%nb, ptr %%c1\n", sty);
    sb_printf(b, "  %%c2 = getelementptr %s, ptr %%c, i32 0, i32 2\n  store i32 %%len, ptr %%c2\n", sty);
    sb_printf(b, "  %%c3 = getelementptr %s, ptr %%c, i32 0, i32 3\n  store i32 %%cap, ptr %%c3\n", sty);
    sb_printf(b, "  %%c4 = getelementptr %s, ptr %%c, i32 0, i32 4\n  store i32 %%hc, ptr %%c4\n  ret ptr %%c\n}\n\n", sty);

    /* update (|=) */
    sb_printf(b, "define internal void @_%s_update(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%b, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%b, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  call void @_%s_add(ptr %%a, %s %%v)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n", m, mty);

    /* union (|) */
    sb_printf(b, "define internal ptr @_%s_union(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%res = call ptr @_%s_copy(ptr %%a)\n  call void @_%s_update(ptr %%res, ptr %%b)\n  ret ptr %%res\n}\n\n", m, m);

    /* intersection (&) */
    sb_printf(b, "define internal ptr @_%s_intersection(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%a, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%a, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  br label %%loop\n", m);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%b, %s %%v)\n  br i1 %%has, label %%add, label %%next\nadd:\n", m, mty);
    sb_printf(b, "  call void @_%s_add(ptr %%res, %s %%v)\n  br label %%next\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty);

    /* difference (-) */
    sb_printf(b, "define internal ptr @_%s_difference(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%a, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%a, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  br label %%loop\n", m);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%b, %s %%v)\n  br i1 %%has, label %%next, label %%add\nadd:\n", m, mty);
    sb_printf(b, "  call void @_%s_add(ptr %%res, %s %%v)\n  br label %%next\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty);

    /* symmetric_difference (^) */
    sb_printf(b, "define internal ptr @_%s_symmetric_difference(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%res = call ptr @_%s_difference(ptr %%a, ptr %%b)\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%b, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%b, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%a, %s %%v)\n  br i1 %%has, label %%next, label %%add\nadd:\n", m, mty);
    sb_printf(b, "  call void @_%s_add(ptr %%res, %s %%v)\n  br label %%next\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty);

    /* difference_update (-=), symmetric_difference_update (^=), intersection_update (&=) */
    sb_printf(b, "define internal void @_%s_difference_update(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%b, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%b, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  call void @_%s_discard(ptr %%a, %s %%v)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n", m, mty);

    sb_printf(b, "define internal void @_%s_symmetric_difference_update(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%b, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%b, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%a, %s %%v)\n  br i1 %%has, label %%rem, label %%add\nrem:\n", m, mty);
    sb_printf(b, "  call void @_%s_discard(ptr %%a, %s %%v)\n  br label %%next\nadd:\n  call void @_%s_add(ptr %%a, %s %%v)\n  br label %%next\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n", m, mty, m, mty);

    sb_printf(b, "define internal void @_%s_intersection_update(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%tmp = call ptr @_%s_intersection(ptr %%a, ptr %%b)\n", m);
    sb_printf(b, "  call void @_%s_clear(ptr %%a)\n  call void @_%s_update(ptr %%a, ptr %%tmp)\n  ret void\n}\n\n", m, m);

    /* issubset, issuperset, isdisjoint, eq */
    sb_printf(b, "define internal i1 @_%s_issubset(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%a, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%a, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %ret_t\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%b, %s %%v)\n  br i1 %%has, label %%next, label %%ret_f\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\nret_t:\n  ret i1 true\nret_f:\n  ret i1 false\n}\n\n", m, mty);

    sb_printf(b, "define internal i1 @_%s_issuperset(ptr %%a, ptr %%b) {\nentry:\n  %%r = call i1 @_%s_issubset(ptr %%b, ptr %%a)\n  ret i1 %%r\n}\n\n", m, m);

    sb_printf(b, "define internal i1 @_%s_isdisjoint(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%a, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%a, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %ret_t\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%b, %s %%v)\n  br i1 %%has, label %%ret_f, label %%next\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\nret_t:\n  ret i1 true\nret_f:\n  ret i1 false\n}\n\n", m, mty);

    sb_printf(b, "define internal i1 @_%s_eq(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lap = getelementptr %s, ptr %%a, i32 0, i32 2\n  %%la = load i32, ptr %%lap\n", sty);
    sb_printf(b, "  %%lbp = getelementptr %s, ptr %%b, i32 0, i32 2\n  %%lb = load i32, ptr %%lbp\n", sty);
    sb_append(b, "  %leq = icmp eq i32 %la, %lb\n  br i1 %leq, label %sub, label %rf\nsub:\n");
    sb_printf(b, "  %%r = call i1 @_%s_issubset(ptr %%a, ptr %%b)\n  ret i1 %%r\nrf:\n  ret i1 false\n}\n\n", m);
}

static void emit_mono_dict(Type* dt) {
    Type* kt = dt->key;
    Type* vt = dt->val;
    emit_hash_eq_for_type(kt);
    emit_hash_eq_for_type(vt);
    const char* m = dt->mangled;
    const char* sty = arena_sprintf("%%%s", m);
    const char* kmty = llvm_mem_ty(kt);
    const char* vmty = llvm_mem_ty(vt);
    const char* ksz = arena_sprintf("ptrtoint (ptr getelementptr (%s, ptr null, i32 1) to i64)", kmty);
    const char* vsz = arena_sprintf("ptrtoint (ptr getelementptr (%s, ptr null, i32 1) to i64)", vmty);
    StrBuf* b = &g_cg.mono;

    /* %Dict_K_V = type { ptr keys, ptr values, ptr buckets, i32 len, i32 cap, i32 ht_cap, i32 ht_tomb } */
    sb_printf(b, "define internal ptr @_%s_new(i32 %%init_cap) {\nentry:\n", m);
    sb_printf(b, "  %%sz = ptrtoint ptr getelementptr (%s, ptr null, i32 1) to i64\n  %%d = call ptr @malloc(i64 %%sz)\n", sty);
    sb_append(b, "  %cap = call i32 @llvm.smax.i32(i32 %init_cap, i32 8)\n");
    sb_append(b, "  br label %hloop\nhloop:\n  %hc = phi i32 [ 16, %entry ], [ %hc2, %hstep ]\n  %need = mul i32 %cap, 2\n");
    sb_append(b, "  %lt = icmp slt i32 %hc, %need\n  br i1 %lt, label %hstep, label %hdone\nhstep:\n  %hc2 = mul i32 %hc, 2\n  br label %hloop\nhdone:\n");
    sb_append(b, "  %cap64 = sext i32 %cap to i64\n");
    sb_printf(b, "  %%kb = mul i64 %%cap64, %s\n  %%keys = call ptr @malloc(i64 %%kb)\n", ksz);
    sb_printf(b, "  %%vb = mul i64 %%cap64, %s\n  %%vals = call ptr @malloc(i64 %%vb)\n", vsz);
    sb_append(b, "  %hc64 = sext i32 %hc to i64\n  %bb = mul i64 %hc64, 4\n  %buckets = call ptr @malloc(i64 %bb)\n");
    sb_append(b, "  br label %iloop\niloop:\n  %i = phi i32 [ 0, %hdone ], [ %in, %istep ]\n  %ic = icmp slt i32 %i, %hc\n  br i1 %ic, label %istep, label %idone\nistep:\n");
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %i\n  store i32 -1, ptr %bp\n  %in = add i32 %i, 1\n  br label %iloop\nidone:\n");
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  store ptr %%keys, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  store ptr %%vals, ptr %%p1\n", sty);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%d, i32 0, i32 2\n  store ptr %%buckets, ptr %%p2\n", sty);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  store i32 0, ptr %%p3\n", sty);
    sb_printf(b, "  %%p4 = getelementptr %s, ptr %%d, i32 0, i32 4\n  store i32 %%cap, ptr %%p4\n", sty);
    sb_printf(b, "  %%p5 = getelementptr %s, ptr %%d, i32 0, i32 5\n  store i32 %%hc, ptr %%p5\n", sty);
    sb_printf(b, "  %%p6 = getelementptr %s, ptr %%d, i32 0, i32 6\n  store i32 0, ptr %%p6\n  ret ptr %%d\n}\n\n", sty);

    /* rebuild */
    sb_printf(b, "define internal void @_%s_rebuild(ptr %%d, i32 %%ncap) {\nentry:\n", m);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%d, i32 0, i32 2\n  %%oldb = load ptr, ptr %%p2\n", sty);
    sb_printf(b, "  %%p5 = getelementptr %s, ptr %%d, i32 0, i32 5\n  %%oldhc = load i32, ptr %%p5\n  store i32 %%ncap, ptr %%p5\n", sty);
    sb_printf(b, "  %%p6 = getelementptr %s, ptr %%d, i32 0, i32 6\n  store i32 0, ptr %%p6\n", sty);
    sb_append(b, "  %same_hc = icmp eq i32 %oldhc, %ncap\n  br i1 %same_hc, label %reuse, label %allocb\nallocb:\n");
    sb_append(b, "  call void @free(ptr %oldb)\n  %nc64 = sext i32 %ncap to i64\n  %bb = mul i64 %nc64, 4\n  %nb0 = call ptr @malloc(i64 %bb)\n  store ptr %nb0, ptr %p2\n  br label %reuse\nreuse:\n");
    sb_append(b, "  %nb = load ptr, ptr %p2\n");
    sb_append(b, "  br label %zloop\nzloop:\n  %z = phi i32 [ 0, %reuse ], [ %zn, %zstep ]\n  %zc = icmp slt i32 %z, %ncap\n  br i1 %zc, label %zstep, label %zdone\nzstep:\n");
    sb_append(b, "  %zp = getelementptr i32, ptr %nb, i32 %z\n  store i32 -1, ptr %zp\n  %zn = add i32 %z, 1\n  br label %zloop\nzdone:\n");
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n  %%mask = sub i32 %%ncap, 1\n  br label %%rloop\n", sty);
    sb_append(b, "rloop:\n  %i = phi i32 [ 0, %zdone ], [ %in, %rnext ]\n  %rc = icmp slt i32 %i, %len\n  br i1 %rc, label %rbody, label %rdone\nrbody:\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%i\n  %%k = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%h64 = call i64 @_hash_%s(%s %%k)\n  %%h32 = trunc i64 %%h64 to i32\n  %%b0 = and i32 %%h32, %%mask\n  br label %%probe\n", kt->mangled, kmty);
    sb_append(b, "probe:\n  %b = phi i32 [ %b0, %rbody ], [ %bn, %pstep ]\n  %bp = getelementptr i32, ptr %nb, i32 %b\n  %cur = load i32, ptr %bp\n  %occ = icmp sge i32 %cur, 0\n  br i1 %occ, label %pstep, label %rnext\npstep:\n  %b1 = add i32 %b, 1\n  %bn = and i32 %b1, %mask\n  br label %probe\nrnext:\n  store i32 %i, ptr %bp\n  %in = add i32 %i, 1\n  br label %rloop\nrdone:\n  ret void\n}\n\n");

    /* find */
    sb_printf(b, "define internal i32 @_%s_find(ptr %%d, %s %%key, ptr %%out_slot) alwaysinline {\nentry:\n", m, kmty);
    sb_printf(b, "  %%p5 = getelementptr %s, ptr %%d, i32 0, i32 5\n  %%hc = load i32, ptr %%p5\n  %%mask = sub i32 %%hc, 1\n", sty);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%d, i32 0, i32 2\n  %%buckets = load ptr, ptr %%p2\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%h64 = call i64 @_hash_%s(%s %%key)\n  %%h32 = trunc i64 %%h64 to i32\n  %%b0 = and i32 %%h32, %%mask\n  br label %%loop\n", kt->mangled, kmty);
    sb_append(b, "loop:\n  %b = phi i32 [ %b0, %entry ], [ %bn, %next ]\n  %ft = phi i32 [ -1, %entry ], [ %nft, %next ]\n");
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %b\n  %idx = load i32, ptr %bp\n  %is_empty = icmp eq i32 %idx, -1\n  br i1 %is_empty, label %miss, label %chk_tomb\nmiss:\n");
    sb_append(b, "  %has_ft = icmp sge i32 %ft, 0\n  %slot_m = select i1 %has_ft, i32 %ft, i32 %b\n  %want_s = icmp ne ptr %out_slot, null\n  br i1 %want_s, label %ws, label %ret_m\nws:\n  store i32 %slot_m, ptr %out_slot\n  br label %ret_m\nret_m:\n  ret i32 -1\n");
    sb_append(b, "chk_tomb:\n  %is_tomb = icmp eq i32 %idx, -2\n  br i1 %is_tomb, label %tomb, label %cmp\ntomb:\n  %no_ft = icmp slt i32 %ft, 0\n  %ft2 = select i1 %no_ft, i32 %b, i32 %ft\n  br label %next\ncmp:\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%idx\n  %%kv = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%eq = call i1 @_eq_%s(%s %%kv, %s %%key)\n  br i1 %%eq, label %%hit, label %%next\nhit:\n", kt->mangled, kmty, kmty);
    sb_append(b, "  %want_h = icmp ne ptr %out_slot, null\n  br i1 %want_h, label %wh, label %ret_h\nwh:\n  store i32 %b, ptr %out_slot\n  br label %ret_h\nret_h:\n  ret i32 %idx\n");
    sb_append(b, "next:\n  %nft = phi i32 [ %ft2, %tomb ], [ %ft, %cmp ]\n  %b1 = add i32 %b, 1\n  %bn = and i32 %b1, %mask\n  br label %loop\n}\n\n");

    /* set */
    sb_printf(b, "define internal void @_%s_set(ptr %%d, %s %%key, %s %%val) alwaysinline {\nentry:\n", m, kmty, vmty);
    sb_append(b, "  %slot_p = alloca i32\n");
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p5 = getelementptr %s, ptr %%d, i32 0, i32 5\n  %%hc = load i32, ptr %%p5\n", sty);
    sb_printf(b, "  %%p6 = getelementptr %s, ptr %%d, i32 0, i32 6\n  %%tomb = load i32, ptr %%p6\n", sty);
    sb_append(b, "  %used1 = add i32 %len, %tomb\n  %used2 = add i32 %used1, 1\n  %u2x = mul i32 %used2, 2\n  %need_r = icmp sgt i32 %u2x, %hc\n  br i1 %need_r, label %reb, label %find\nreb:\n");
    sb_append(b, "  %l1a = add i32 %len, 1\n  %l1x = mul i32 %l1a, 2\n  %grow_h = icmp sgt i32 %l1x, %hc\n  %hc2 = mul i32 %hc, 2\n  %nhc = select i1 %grow_h, i32 %hc2, i32 %hc\n");
    sb_printf(b, "  call void @_%s_rebuild(ptr %%d, i32 %%nhc)\n  br label %%find\nfind:\n", m);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr %%slot_p)\n  %%found = icmp sge i32 %%idx, 0\n  br i1 %%found, label %%upd, label %%ins\nupd:\n", m, kmty);
    sb_printf(b, "  %%p1u = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%valsu = load ptr, ptr %%p1u\n", sty);
    sb_printf(b, "  %%vpu = getelementptr %s, ptr %%valsu, i32 %%idx\n  store %s %%val, ptr %%vpu\n  ret void\nins:\n", vmty, vmty);
    sb_printf(b, "  %%p4 = getelementptr %s, ptr %%d, i32 0, i32 4\n  %%cap = load i32, ptr %%p4\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n", sty, sty);
    sb_append(b, "  %full = icmp sge i32 %len, %cap\n  br i1 %full, label %grow, label %put\ngrow:\n");
    sb_append(b, "  %ncap = mul i32 %cap, 2\n  store i32 %ncap, ptr %p4\n  %nc64 = sext i32 %ncap to i64\n");
    sb_printf(b, "  %%nkb = mul i64 %%nc64, %s\n  %%oldk = load ptr, ptr %%p0\n  %%newk = call ptr @realloc(ptr %%oldk, i64 %%nkb)\n  store ptr %%newk, ptr %%p0\n", ksz);
    sb_printf(b, "  %%nvb = mul i64 %%nc64, %s\n  %%oldv = load ptr, ptr %%p1\n  %%newv = call ptr @realloc(ptr %%oldv, i64 %%nvb)\n  store ptr %%newv, ptr %%p1\n  br label %%put\nput:\n", vsz);
    sb_append(b, "  %keys = load ptr, ptr %p0\n  %vals = load ptr, ptr %p1\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%len\n  store %s %%key, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%len\n  store %s %%val, ptr %%vp\n", vmty, vmty);
    sb_printf(b, "  %%p2b = getelementptr %s, ptr %%d, i32 0, i32 2\n  %%buckets = load ptr, ptr %%p2b\n  %%slot = load i32, ptr %%slot_p\n", sty);
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %slot\n  %was = load i32, ptr %bp\n  %wt = icmp eq i32 %was, -2\n  %tdec = zext i1 %wt to i32\n  %ntomb = sub i32 %tomb, %tdec\n  store i32 %ntomb, ptr %p6\n");
    sb_append(b, "  store i32 %len, ptr %bp\n  %nlen = add i32 %len, 1\n  store i32 %nlen, ptr %p3\n  ret void\n}\n\n");

    /* get, getitem, setdefault, contains */
    sb_printf(b, "define internal %s @_%s_get(ptr %%d, %s %%key, %s %%def) alwaysinline {\nentry:\n", vmty, m, kmty, vmty);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr null)\n  %%ok = icmp sge i32 %%idx, 0\n  br i1 %%ok, label %%hit, label %%miss\nhit:\n", m, kmty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%idx\n  %%v = load %s, ptr %%vp\n  ret %s %%v\nmiss:\n  ret %s %%def\n}\n\n", vmty, vmty, vmty, vmty);

    sb_printf(b, "define internal %s @_%s_getitem(ptr %%d, %s %%key) alwaysinline {\nentry:\n", vmty, m, kmty);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr null)\n  %%ok = icmp sge i32 %%idx, 0\n  br i1 %%ok, label %%hit, label %%miss\nhit:\n", m, kmty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%idx\n  %%v = load %s, ptr %%vp\n  ret %s %%v\nmiss:\n", vmty, vmty, vmty);
    sb_printf(b, "  call void @stricta_raise(i32 6, ptr null, ptr null)\n  unreachable\n}\n\n");

    sb_printf(b, "define internal %s @_%s_setdefault(ptr %%d, %s %%key, %s %%def) alwaysinline {\nentry:\n", vmty, m, kmty, vmty);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr null)\n  %%ok = icmp sge i32 %%idx, 0\n  br i1 %%ok, label %%hit, label %%ins\nhit:\n", m, kmty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%idx\n  %%v = load %s, ptr %%vp\n  ret %s %%v\nins:\n", vmty, vmty, vmty);
    sb_printf(b, "  call void @_%s_set(ptr %%d, %s %%key, %s %%def)\n  ret %s %%def\n}\n\n", m, kmty, vmty, vmty);

    sb_printf(b, "define internal i1 @_%s_contains(ptr %%d, %s %%key) alwaysinline {\nentry:\n", m, kmty);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr null)\n  %%r = icmp sge i32 %%idx, 0\n  ret i1 %%r\n}\n\n", m, kmty);

    /* pop & del */
    sb_printf(b, "define internal %s @_%s_pop(ptr %%d, %s %%key) {\nentry:\n", vmty, m, kmty);
    sb_append(b, "  %slot_p = alloca i32\n");
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr %%slot_p)\n  %%miss = icmp slt i32 %%idx, 0\n  br i1 %%miss, label %%err, label %%rem\nerr:\n", m, kmty);
    sb_append(b, "  call void @stricta_raise(i32 6, ptr null, ptr null)\n  unreachable\nrem:\n");
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%idx\n  %%retv = load %s, ptr %%vp\n", vmty, vmty);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%d, i32 0, i32 2\n  %%buckets = load ptr, ptr %%p2\n  %%slot = load i32, ptr %%slot_p\n", sty);
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %slot\n  store i32 -2, ptr %bp\n");
    sb_printf(b, "  %%p6 = getelementptr %s, ptr %%d, i32 0, i32 6\n  %%t = load i32, ptr %%p6\n  %%nt = add i32 %%t, 1\n  store i32 %%nt, ptr %%p6\n", sty);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n  %%last = sub i32 %%len, 1\n", sty);
    sb_append(b, "  %nl = icmp ne i32 %idx, %last\n  br i1 %nl, label %swap, label %dec\nswap:\n");
    sb_printf(b, "  %%lkp = getelementptr %s, ptr %%keys, i32 %%last\n  %%mk = load %s, ptr %%lkp\n", kmty, kmty);
    sb_printf(b, "  %%lvp = getelementptr %s, ptr %%vals, i32 %%last\n  %%mv = load %s, ptr %%lvp\n", vmty, vmty);
    sb_printf(b, "  %%dkp = getelementptr %s, ptr %%keys, i32 %%idx\n  store %s %%mk, ptr %%dkp\n", kmty, kmty);
    sb_printf(b, "  %%dvp = getelementptr %s, ptr %%vals, i32 %%idx\n  store %s %%mv, ptr %%dvp\n", vmty, vmty);
    sb_append(b, "  %ms_p = alloca i32\n");
    sb_printf(b, "  %%midx = call i32 @_%s_find(ptr %%d, %s %%mk, ptr %%ms_p)\n  %%ms = load i32, ptr %%ms_p\n", m, kmty);
    sb_append(b, "  %mbp = getelementptr i32, ptr %buckets, i32 %ms\n  store i32 %idx, ptr %mbp\n  br label %dec\ndec:\n  store i32 %last, ptr %p3\n  ret ");
    sb_printf(b, "%s %%retv\n}\n\n", vmty);

    sb_printf(b, "define internal void @_%s_del(ptr %%d, %s %%key) {\nentry:\n", m, kmty);
    sb_printf(b, "  %%_ = call %s @_%s_pop(ptr %%d, %s %%key)\n  ret void\n}\n\n", vmty, m, kmty);

    /* popitem */
    Type* pair[2] = {kt, vt};
    Type* pty = make_tuple_type(pair, 2);
    const char* psty = llvm_reg_ty(pty);
    sb_printf(b, "define internal %s @_%s_popitem(ptr %%d) {\nentry:\n", psty, m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n  %%last = sub i32 %%len, 1\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%last\n  %%k = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%v = call %s @_%s_pop(ptr %%d, %s %%k)\n", vmty, m, kmty);
    sb_printf(b, "  %%t1 = insertvalue %s zeroinitializer, %s %%k, 0\n  %%t2 = insertvalue %s %%t1, %s %%v, 1\n  ret %s %%t2\n}\n\n",
              psty, kmty, psty, vmty, psty);

    /* clear, copy, update, merge, keys, values, items, eq */
    sb_printf(b, "define internal void @_%s_clear(ptr %%d) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  store i32 0, ptr %%p3\n", sty);
    sb_printf(b, "  %%p6 = getelementptr %s, ptr %%d, i32 0, i32 6\n  store i32 0, ptr %%p6\n", sty);
    sb_printf(b, "  %%p5 = getelementptr %s, ptr %%d, i32 0, i32 5\n  %%hc = load i32, ptr %%p5\n", sty);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%d, i32 0, i32 2\n  %%buckets = load ptr, ptr %%p2\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %hc\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %i\n  store i32 -1, ptr %bp\n  %in = add i32 %i, 1\n  br label %loop\ndone:\n  ret void\n}\n\n");

    sb_printf(b, "define internal ptr @_%s_copy(ptr %%d) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  call void @_%s_update(ptr %%res, ptr %%d)\n  ret ptr %%res\n}\n\n", m, m);

    sb_printf(b, "define internal void @_%s_update(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%b, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%b, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%b, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%i\n  %%k = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%i\n  %%v = load %s, ptr %%vp\n", vmty, vmty);
    sb_printf(b, "  call void @_%s_set(ptr %%a, %s %%k, %s %%v)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n", m, kmty, vmty);

    sb_printf(b, "define internal ptr @_%s_merge(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%res = call ptr @_%s_copy(ptr %%a)\n  call void @_%s_update(ptr %%res, ptr %%b)\n  ret ptr %%res\n}\n\n", m, m);

    Type* lk = make_list_type(kt);
    Type* lv = make_list_type(vt);
    Type* li = make_list_type(pty);
    sb_printf(b, "define internal ptr @_%s_keys(ptr %%d) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  br label %%loop\n", lk->mangled);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%i\n  %%k = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  call void @_%s_append(ptr %%res, %s %%k)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", lk->mangled, kmty);

    sb_printf(b, "define internal ptr @_%s_values(ptr %%d) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  br label %%loop\n", lv->mangled);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%i\n  %%v = load %s, ptr %%vp\n", vmty, vmty);
    sb_printf(b, "  call void @_%s_append(ptr %%res, %s %%v)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", lv->mangled, vmty);

    sb_printf(b, "define internal ptr @_%s_items(ptr %%d) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  br label %%loop\n", li->mangled);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%i\n  %%k = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%i\n  %%v = load %s, ptr %%vp\n", vmty, vmty);
    sb_printf(b, "  %%t1 = insertvalue %s zeroinitializer, %s %%k, 0\n  %%t2 = insertvalue %s %%t1, %s %%v, 1\n", psty, kmty, psty, vmty);
    sb_printf(b, "  call void @_%s_append(ptr %%res, %s %%t2)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", li->mangled, psty);

    sb_printf(b, "define internal i1 @_%s_eq(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%p3a = getelementptr %s, ptr %%a, i32 0, i32 3\n  %%la = load i32, ptr %%p3a\n", sty);
    sb_printf(b, "  %%p3b = getelementptr %s, ptr %%b, i32 0, i32 3\n  %%lb = load i32, ptr %%p3b\n", sty);
    sb_append(b, "  %leq = icmp eq i32 %la, %lb\n  br i1 %leq, label %iter, label %rf\niter:\n");
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%a, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%a, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %iter ], [ %in, %next ]\n  %c = icmp slt i32 %i, %la\n  br i1 %c, label %body, label %rt\nbody:\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%i\n  %%k = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%b, %s %%k, ptr null)\n  %%ok = icmp sge i32 %%idx, 0\n  br i1 %%ok, label %%vchk, label %%rf\nvchk:\n", m, kmty);
    sb_printf(b, "  %%vpa = getelementptr %s, ptr %%vals, i32 %%i\n  %%va = load %s, ptr %%vpa\n", vmty, vmty);
    sb_printf(b, "  %%p1b = getelementptr %s, ptr %%b, i32 0, i32 1\n  %%valsb = load ptr, ptr %%p1b\n", sty);
    sb_printf(b, "  %%vpb = getelementptr %s, ptr %%valsb, i32 %%idx\n  %%vb = load %s, ptr %%vpb\n", vmty, vmty);
    sb_printf(b, "  %%veq = call i1 @_eq_%s(%s %%va, %s %%vb)\n  br i1 %%veq, label %%next, label %%rf\nnext:\n", vt->mangled, vmty, vmty);
    sb_append(b, "  %in = add i32 %i, 1\n  br label %loop\nrt:\n  ret i1 true\nrf:\n  ret i1 false\n}\n\n");
}

static const char* cg_ensure_local_alloca(const char* llvm_name, Type* t) {
    for (int i = 0; i < g_cg.fn_alloca_count; i++) {
        if (strcmp(g_cg.fn_allocas[i], llvm_name) == 0) return llvm_name;
    }
    g_cg.fn_allocas = (const char**)realloc(g_cg.fn_allocas, sizeof(char*) * (size_t)(g_cg.fn_alloca_count + 1));
    g_cg.fn_allocas[g_cg.fn_alloca_count++] = llvm_name;
    const char* mty = llvm_mem_ty(t);
    cg_entry("%s = alloca %s", llvm_name, mty);
    cg_entry("store %s %s, ptr %s", mty, llvm_mem_zero(t), llvm_name);
    return llvm_name;
}

static const char* cg_var_ptr(const char* name, Type** out_ty) {
    if (g_cg.cur_fn) {
        bool is_glob = false;
        for (int i = 0; i < g_cg.cur_fn->global_decl_count; i++) {
            if (strcmp(g_cg.cur_fn->global_decls[i], name) == 0) { is_glob = true; break; }
        }
        if (!is_glob) {
            VarSym* lv = find_local_in_fn(g_cg.cur_fn, name);
            if (lv) {
                if (out_ty) *out_ty = lv->type;
                return cg_ensure_local_alloca(lv->llvm_name, lv->type);
            }
            for (int c = 0; c < g_cg.cur_fn->capture_count; c++) {
                if (strcmp(g_cg.cur_fn->captures[c].name, name) == 0) {
                    if (out_ty) *out_ty = g_cg.cur_fn->captures[c].type;
                    const char* gep = cg_reg();
                    const char* ptr = cg_reg();
                    cg_emit("%s = getelementptr [ %d x ptr ], ptr %%__env, i32 0, i32 %d",
                            gep, g_cg.cur_fn->capture_count, c);
                    cg_emit("%s = load ptr, ptr %s", ptr, gep);
                    return ptr;
                }
            }
        }
    }
    VarSym* gv = find_global_in_mod(g_cg.cur_mod, name);
    if (gv) {
        if (out_ty) *out_ty = gv->type;
        return gv->llvm_name;
    }
    return NULL;
}

static const char* cg_expr(Expr* e, Type* hint);
static void cg_stmts(Stmt** stmts, int count);

static const char* cg_to_str(const char* val, Type* t) {
    if (!t) return cg_strictastr_lit("None", 4);
    if (t->kind == TY_STR) return val;
    if (t->kind == TY_NONE_LIT || t->kind == TY_VOID) return cg_strictastr_lit("None", 4);
    if (t->kind == TY_BOOL) {
        const char* z = cg_reg();
        const char* r = cg_reg();
        cg_emit("%s = zext i1 %s to i32", z, val);
        cg_emit("%s = call ptr @stricta_bool_to_str(i32 %s)", r, z);
        return r;
    }
    if (type_is_signed_int(t)) {
        const char* v64 = cg_coerce(val, t, g_ty_i64);
        const char* r = cg_reg();
        cg_emit("%s = call ptr @stricta_int_to_str(i64 %s)", r, v64);
        return r;
    }
    if (type_is_unsigned_int(t)) {
        const char* v64 = cg_coerce(val, t, g_ty_u64);
        const char* r = cg_reg();
        cg_emit("%s = call ptr @stricta_uint_to_str(i64 %s)", r, v64);
        return r;
    }
    if (type_is_float(t)) {
        const char* v64 = cg_coerce(val, t, g_ty_f64);
        const char* r = cg_reg();
        cg_emit("%s = call ptr @stricta_float_to_str(double %s)", r, v64);
        return r;
    }
    if (t->kind == TY_BIGINT) {
        const char* r = cg_reg();
        cg_emit("%s = call ptr @stricta_bigint_to_str(ptr %s)", r, val);
        return r;
    }
    if (t->kind == TY_OPTIONAL) {
        const char* is_none = cg_reg();
        const char* l_some = cg_lbl("opt_some");
        const char* l_none = cg_lbl("opt_none");
        const char* l_end = cg_lbl("opt_end");
        if (type_is_heap_ref(t->elem)) {
            cg_emit("%s = icmp eq ptr %s, null", is_none, val);
        } else {
            const char* tag = cg_reg();
            cg_emit("%s = extractvalue %s %s, 0", tag, llvm_reg_ty(t), val);
            cg_emit("%s = icmp eq i8 %s, 0", is_none, tag);
        }
        cg_cond_br(is_none, l_none, l_some);
        cg_label(l_none);
        const char* ns = cg_strictastr_lit("None", 4);
        cg_br(l_end);
        cg_label(l_some);
        const char* inner = val;
        if (!type_is_heap_ref(t->elem)) {
            const char* mv = cg_reg();
            cg_emit("%s = extractvalue %s %s, 1", mv, llvm_reg_ty(t), val);
            inner = cg_mem_to_reg(mv, t->elem);
        }
        const char* ss = cg_to_str(inner, t->elem);
        cg_br(l_end);
        cg_label(l_end);
        const char* res = cg_reg();
        cg_emit("%s = phi ptr [ %s, %%%s ], [ %s, %%%s ]", res, ns, l_none, ss, l_some);
        return res;
    }
    if (t->kind == TY_CLASS) {
        FuncInfo* sm = find_method_in_class(t->cls, "__str__");
        if (!sm) sm = find_method_in_class(t->cls, "__repr__");
        if (sm) {
            const char* r = cg_reg();
            cg_emit("%s = call ptr %s(%s %s)", r, sm->llvm_name, llvm_reg_ty(t), val);
            return r;
        }
        return cg_strictastr_lit(arena_sprintf("<%s object>", t->cls->name), strlen(t->cls->name) + 10);
    }
    if (t->kind == TY_LIST) {
        const char* sty = arena_sprintf("%%%s", t->mangled);
        const char* mty = llvm_mem_ty(t->elem);
        const char* lp = cg_reg(); const char* len = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", lp, sty, val);
        cg_emit("%s = load i32, ptr %s", len, lp);
        const char* ip = cg_reg(); const char* arr = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", ip, sty, val);
        cg_emit("%s = load ptr, ptr %s", arr, ip);
        const char* acc_p = cg_reg();
        cg_entry("%s = alloca ptr", acc_p);
        cg_emit("store ptr %s, ptr %s", cg_strictastr_lit("[", 1), acc_p);
        const char* i_p = cg_reg();
        cg_entry("%s = alloca i32", i_p);
        cg_emit("store i32 0, ptr %s", i_p);
        const char* l_loop = cg_lbl("lstr_loop");
        const char* l_body = cg_lbl("lstr_body");
        const char* l_sep = cg_lbl("lstr_sep");
        const char* l_item = cg_lbl("lstr_item");
        const char* l_done = cg_lbl("lstr_done");
        cg_br(l_loop);
        cg_label(l_loop);
        const char* iv = cg_reg(); const char* c = cg_reg();
        cg_emit("%s = load i32, ptr %s", iv, i_p);
        cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
        cg_cond_br(c, l_body, l_done);
        cg_label(l_body);
        const char* gt0 = cg_reg();
        cg_emit("%s = icmp sgt i32 %s, 0", gt0, iv);
        cg_cond_br(gt0, l_sep, l_item);
        cg_label(l_sep);
        const char* a1 = cg_reg(); const char* a2 = cg_reg();
        cg_emit("%s = load ptr, ptr %s", a1, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", a2, a1, cg_strictastr_lit(", ", 2));
        cg_emit("store ptr %s, ptr %s", a2, acc_p);
        cg_br(l_item);
        cg_label(l_item);
        const char* ep = cg_reg(); const char* evm = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, mty, arr, iv);
        cg_emit("%s = load %s, ptr %s", evm, mty, ep);
        const char* ev = cg_mem_to_reg(evm, t->elem);
        const char* es = (t->elem->kind == TY_STR)
            ? ({ const char* q = cg_strictastr_lit("'", 1); const char* q1 = cg_reg(); const char* q2 = cg_reg();
                 cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q1, q, ev);
                 cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q2, q1, q); q2; })
            : cg_to_str(ev, t->elem);
        const char* a3 = cg_reg(); const char* a4 = cg_reg(); const char* in = cg_reg();
        cg_emit("%s = load ptr, ptr %s", a3, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", a4, a3, es);
        cg_emit("store ptr %s, ptr %s", a4, acc_p);
        cg_emit("%s = add i32 %s, 1", in, iv);
        cg_emit("store i32 %s, ptr %s", in, i_p);
        cg_br(l_loop);
        cg_label(l_done);
        const char* af = cg_reg(); const char* res = cg_reg();
        cg_emit("%s = load ptr, ptr %s", af, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", res, af, cg_strictastr_lit("]", 1));
        return res;
    }
    if (t->kind == TY_TUPLE) {
        if (t->tuple_count == 0) return cg_strictastr_lit("()", 2);
        const char* acc = cg_strictastr_lit("(", 1);
        for (int i = 0; i < t->tuple_count; i++) {
            if (i > 0) {
                const char* n = cg_reg();
                cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, cg_strictastr_lit(", ", 2));
                acc = n;
            }
            const char* fm = cg_reg();
            cg_emit("%s = extractvalue %s %s, %d", fm, llvm_reg_ty(t), val, i);
            const char* fv = cg_mem_to_reg(fm, t->tuple_elems[i]);
            const char* fs = (t->tuple_elems[i]->kind == TY_STR)
                ? ({ const char* q = cg_strictastr_lit("'", 1); const char* q1 = cg_reg(); const char* q2 = cg_reg();
                     cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q1, q, fv);
                     cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q2, q1, q); q2; })
                : cg_to_str(fv, t->tuple_elems[i]);
            const char* n = cg_reg();
            cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, fs);
            acc = n;
        }
        const char* res = cg_reg();
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", res, acc, cg_strictastr_lit(t->tuple_count == 1 ? ",)" : ")", t->tuple_count == 1 ? 2 : 1));
        return res;
    }
    if (t->kind == TY_SET) {
        const char* sty = arena_sprintf("%%%s", t->mangled);
        const char* mty = llvm_mem_ty(t->elem);
        const char* lp = cg_reg(); const char* len = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", lp, sty, val);
        cg_emit("%s = load i32, ptr %s", len, lp);
        const char* is_emp = cg_reg();
        const char* l_emp = cg_lbl("set_emp"); const char* l_non = cg_lbl("set_non"); const char* l_end = cg_lbl("set_end");
        cg_emit("%s = icmp eq i32 %s, 0", is_emp, len);
        cg_cond_br(is_emp, l_emp, l_non);
        cg_label(l_emp);
        const char* es = cg_strictastr_lit("set()", 5);
        cg_br(l_end);
        cg_label(l_non);
        const char* ip = cg_reg(); const char* arr = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", ip, sty, val);
        cg_emit("%s = load ptr, ptr %s", arr, ip);
        const char* acc_p = cg_reg(); const char* i_p = cg_reg();
        cg_entry("%s = alloca ptr", acc_p); cg_entry("%s = alloca i32", i_p);
        cg_emit("store ptr %s, ptr %s", cg_strictastr_lit("{", 1), acc_p);
        cg_emit("store i32 0, ptr %s", i_p);
        const char* l_loop = cg_lbl("sstr_loop"); const char* l_body = cg_lbl("sstr_body");
        const char* l_sep = cg_lbl("sstr_sep"); const char* l_item = cg_lbl("sstr_item"); const char* l_done = cg_lbl("sstr_done");
        cg_br(l_loop);
        cg_label(l_loop);
        const char* iv = cg_reg(); const char* c = cg_reg();
        cg_emit("%s = load i32, ptr %s", iv, i_p);
        cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
        cg_cond_br(c, l_body, l_done);
        cg_label(l_body);
        const char* gt0 = cg_reg();
        cg_emit("%s = icmp sgt i32 %s, 0", gt0, iv);
        cg_cond_br(gt0, l_sep, l_item);
        cg_label(l_sep);
        const char* a1 = cg_reg(); const char* a2 = cg_reg();
        cg_emit("%s = load ptr, ptr %s", a1, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", a2, a1, cg_strictastr_lit(", ", 2));
        cg_emit("store ptr %s, ptr %s", a2, acc_p);
        cg_br(l_item);
        cg_label(l_item);
        const char* ep = cg_reg(); const char* evm = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, mty, arr, iv);
        cg_emit("%s = load %s, ptr %s", evm, mty, ep);
        const char* evs = cg_to_str(cg_mem_to_reg(evm, t->elem), t->elem);
        const char* a3 = cg_reg(); const char* a4 = cg_reg(); const char* in = cg_reg();
        cg_emit("%s = load ptr, ptr %s", a3, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", a4, a3, evs);
        cg_emit("store ptr %s, ptr %s", a4, acc_p);
        cg_emit("%s = add i32 %s, 1", in, iv);
        cg_emit("store i32 %s, ptr %s", in, i_p);
        cg_br(l_loop);
        cg_label(l_done);
        const char* af = cg_reg(); const char* ns = cg_reg();
        cg_emit("%s = load ptr, ptr %s", af, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", ns, af, cg_strictastr_lit("}", 1));
        cg_br(l_end);
        cg_label(l_end);
        const char* res = cg_reg();
        cg_emit("%s = phi ptr [ %s, %%%s ], [ %s, %%%s ]", res, es, l_emp, ns, l_done);
        return res;
    }
    if (t->kind == TY_DICT) {
        const char* sty = arena_sprintf("%%%s", t->mangled);
        const char* kmty = llvm_mem_ty(t->key);
        const char* vmty = llvm_mem_ty(t->val);
        const char* lp = cg_reg(); const char* len = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 3", lp, sty, val);
        cg_emit("%s = load i32, ptr %s", len, lp);
        const char* kp0 = cg_reg(); const char* keys = cg_reg();
        const char* vp0 = cg_reg(); const char* vals = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", kp0, sty, val);
        cg_emit("%s = load ptr, ptr %s", keys, kp0);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 1", vp0, sty, val);
        cg_emit("%s = load ptr, ptr %s", vals, vp0);
        const char* acc_p = cg_reg(); const char* i_p = cg_reg();
        cg_entry("%s = alloca ptr", acc_p); cg_entry("%s = alloca i32", i_p);
        cg_emit("store ptr %s, ptr %s", cg_strictastr_lit("{", 1), acc_p);
        cg_emit("store i32 0, ptr %s", i_p);
        const char* l_loop = cg_lbl("dstr_loop"); const char* l_body = cg_lbl("dstr_body");
        const char* l_sep = cg_lbl("dstr_sep"); const char* l_item = cg_lbl("dstr_item"); const char* l_done = cg_lbl("dstr_done");
        cg_br(l_loop);
        cg_label(l_loop);
        const char* iv = cg_reg(); const char* c = cg_reg();
        cg_emit("%s = load i32, ptr %s", iv, i_p);
        cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
        cg_cond_br(c, l_body, l_done);
        cg_label(l_body);
        const char* gt0 = cg_reg();
        cg_emit("%s = icmp sgt i32 %s, 0", gt0, iv);
        cg_cond_br(gt0, l_sep, l_item);
        cg_label(l_sep);
        const char* a1 = cg_reg(); const char* a2 = cg_reg();
        cg_emit("%s = load ptr, ptr %s", a1, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", a2, a1, cg_strictastr_lit(", ", 2));
        cg_emit("store ptr %s, ptr %s", a2, acc_p);
        cg_br(l_item);
        cg_label(l_item);
        const char* kp = cg_reg(); const char* kvm = cg_reg();
        const char* vp = cg_reg(); const char* vvm = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", kp, kmty, keys, iv);
        cg_emit("%s = load %s, ptr %s", kvm, kmty, kp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", vp, vmty, vals, iv);
        cg_emit("%s = load %s, ptr %s", vvm, vmty, vp);
        const char* ks = cg_to_str(cg_mem_to_reg(kvm, t->key), t->key);
        const char* vs = cg_to_str(cg_mem_to_reg(vvm, t->val), t->val);
        if (t->key->kind == TY_STR) {
            const char* q = cg_strictastr_lit("'", 1); const char* q1 = cg_reg(); const char* q2 = cg_reg();
            cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q1, q, ks);
            cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q2, q1, q);
            ks = q2;
        }
        const char* p1 = cg_reg(); const char* p2 = cg_reg(); const char* a3 = cg_reg(); const char* a4 = cg_reg();
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", p1, ks, cg_strictastr_lit(": ", 2));
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", p2, p1, vs);
        cg_emit("%s = load ptr, ptr %s", a3, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", a4, a3, p2);
        cg_emit("store ptr %s, ptr %s", a4, acc_p);
        const char* in = cg_reg();
        cg_emit("%s = add i32 %s, 1", in, iv);
        cg_emit("store i32 %s, ptr %s", in, i_p);
        cg_br(l_loop);
        cg_label(l_done);
        const char* af = cg_reg(); const char* res = cg_reg();
        cg_emit("%s = load ptr, ptr %s", af, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", res, af, cg_strictastr_lit("}", 1));
        return res;
    }
    return cg_strictastr_lit("<object>", 8);
}

static void cg_print_val(const char* val, Type* t) {
    if (!t || t->kind == TY_NONE_LIT || t->kind == TY_VOID) {
        cg_emit("call void @stricta_print_cstr(ptr %s)", cg_cstr_lit("None", 4));
    } else if (t->kind == TY_BOOL) {
        const char* z = cg_reg();
        cg_emit("%s = zext i1 %s to i32", z, val);
        cg_emit("call void @stricta_print_bool(i32 %s)", z);
    } else if (type_is_signed_int(t)) {
        cg_emit("call void @stricta_print_i64(i64 %s)", cg_coerce(val, t, g_ty_i64));
    } else if (type_is_unsigned_int(t)) {
        cg_emit("call void @stricta_print_u64(i64 %s)", cg_coerce(val, t, g_ty_u64));
    } else if (type_is_float(t)) {
        cg_emit("call void @stricta_print_f64(double %s)", cg_coerce(val, t, g_ty_f64));
    } else if (t->kind == TY_STR) {
        cg_emit("call void @stricta_print_str(ptr %s)", val);
    } else {
        const char* s = cg_to_str(val, t);
        cg_emit("call void @stricta_print_str(ptr %s)", s);
    }
}

static const char* cg_binop_values(const char* op, const char* lv, Type* lt, const char* rv, Type* rt, Type* res_ty) {
    if (strcmp(op, "is") == 0 || strcmp(op, "is not") == 0) {
        bool neg = (strcmp(op, "is not") == 0);
        const char* r = cg_reg();
        if (lt->kind == TY_OPTIONAL && !type_is_heap_ref(lt->elem)) {
            const char* tag = cg_reg();
            cg_emit("%s = extractvalue %s %s, 0", tag, llvm_reg_ty(lt), lv);
            cg_emit("%s = icmp %s i8 %s, 0", r, neg ? "ne" : "eq", tag);
            return r;
        }
        if (rt->kind == TY_OPTIONAL && !type_is_heap_ref(rt->elem)) {
            const char* tag = cg_reg();
            cg_emit("%s = extractvalue %s %s, 0", tag, llvm_reg_ty(rt), rv);
            cg_emit("%s = icmp %s i8 %s, 0", r, neg ? "ne" : "eq", tag);
            return r;
        }
        const char* l2 = (lt->kind == TY_NONE_LIT) ? "null" : lv;
        const char* r2 = (rt->kind == TY_NONE_LIT) ? "null" : rv;
        cg_emit("%s = icmp %s ptr %s, %s", r, neg ? "ne" : "eq", l2, r2);
        return r;
    }

    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
        bool is_ne = (strcmp(op, "!=") == 0);
        if (lt->kind == TY_CLASS) {
            const char* dunder = NULL;
            if (strcmp(op, "==") == 0) dunder = "__eq__";
            else if (strcmp(op, "!=") == 0) dunder = "__ne__";
            else if (strcmp(op, "<") == 0) dunder = "__lt__";
            else if (strcmp(op, "<=") == 0) dunder = "__le__";
            else if (strcmp(op, ">") == 0) dunder = "__gt__";
            else if (strcmp(op, ">=") == 0) dunder = "__ge__";
            FuncInfo* dm = dunder ? find_method_in_class(lt->cls, dunder) : NULL;
            if (dm) {
                const char* r = cg_reg();
                cg_emit("%s = call i1 %s(%s %s, %s %s)", r, dm->llvm_name, llvm_reg_ty(lt), lv, llvm_reg_ty(rt), rv);
                return r;
            }
            if (lt->cls->is_value_type && (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)) {
                emit_hash_eq_for_type(lt);
                const char* eq = cg_reg();
                cg_emit("%s = call i1 @_eq_%s(%s %s, %s %s)", eq, lt->mangled, llvm_reg_ty(lt), lv, llvm_reg_ty(lt), rv);
                if (!is_ne) return eq;
                const char* neq = cg_reg();
                cg_emit("%s = xor i1 %s, true", neq, eq);
                return neq;
            }
        }
        if ((lt->kind == TY_OPTIONAL && rt->kind == TY_NONE_LIT) || (rt->kind == TY_OPTIONAL && lt->kind == TY_NONE_LIT)) {
            return cg_binop_values(is_ne ? "is not" : "is", lv, lt, rv, rt, g_ty_bool);
        }
        if (lt->kind == TY_STR && rt->kind == TY_STR) {
            const char* c = cg_reg();
            const char* r = cg_reg();
            cg_emit("%s = call i32 @stricta_str_cmp(ptr %s, ptr %s)", c, lv, rv);
            const char* pred = strcmp(op, "==") == 0 ? "eq" : strcmp(op, "!=") == 0 ? "ne" :
                               strcmp(op, "<") == 0 ? "slt" : strcmp(op, "<=") == 0 ? "sle" :
                               strcmp(op, ">") == 0 ? "sgt" : "sge";
            cg_emit("%s = icmp %s i32 %s, 0", r, pred, c);
            return r;
        }
        if (lt->kind == TY_BIGINT && rt->kind == TY_BIGINT) {
            const char* c = cg_reg(); const char* r = cg_reg();
            cg_emit("%s = call i32 @stricta_bigint_cmp(ptr %s, ptr %s)", c, lv, rv);
            const char* pred = strcmp(op, "==") == 0 ? "eq" : strcmp(op, "!=") == 0 ? "ne" :
                               strcmp(op, "<") == 0 ? "slt" : strcmp(op, "<=") == 0 ? "sle" :
                               strcmp(op, ">") == 0 ? "sgt" : "sge";
            cg_emit("%s = icmp %s i32 %s, 0", r, pred, c);
            return r;
        }
        if (lt->kind == TY_LIST && type_eq(lt, rt)) {
            const char* eq = cg_reg();
            cg_emit("%s = call i1 @_%s_eq(ptr %s, ptr %s)", eq, lt->mangled, lv, rv);
            if (!is_ne) return eq;
            const char* r = cg_reg(); cg_emit("%s = xor i1 %s, true", r, eq); return r;
        }
        if (lt->kind == TY_SET && type_eq(lt, rt)) {
            const char* fnm = strcmp(op, "==") == 0 ? "eq" : strcmp(op, "!=") == 0 ? "eq" :
                              strcmp(op, "<=") == 0 ? "issubset" : strcmp(op, ">=") == 0 ? "issuperset" : "eq";
            const char* b = cg_reg();
            cg_emit("%s = call i1 @_%s_%s(ptr %s, ptr %s)", b, lt->mangled, fnm, lv, rv);
            if (!is_ne) return b;
            const char* r = cg_reg(); cg_emit("%s = xor i1 %s, true", r, b); return r;
        }
        if (lt->kind == TY_DICT && type_eq(lt, rt)) {
            const char* eq = cg_reg();
            cg_emit("%s = call i1 @_%s_eq(ptr %s, ptr %s)", eq, lt->mangled, lv, rv);
            if (!is_ne) return eq;
            const char* r = cg_reg(); cg_emit("%s = xor i1 %s, true", r, eq); return r;
        }
        if (lt->kind == TY_TUPLE && type_eq(lt, rt)) {
            emit_hash_eq_for_type(lt);
            const char* eq = cg_reg();
            cg_emit("%s = call i1 @_eq_%s(%s %s, %s %s)", eq, lt->mangled, llvm_reg_ty(lt), lv, llvm_reg_ty(lt), rv);
            if (!is_ne) return eq;
            const char* r = cg_reg(); cg_emit("%s = xor i1 %s, true", r, eq); return r;
        }
        if (type_is_numeric(lt) || lt->kind == TY_BOOL) {
            Type* ct = numeric_common_type(lt, rt);
            const char* lc = cg_coerce(lv, lt, ct);
            const char* rc = cg_coerce(rv, rt, ct);
            const char* r = cg_reg();
            if (type_is_float(ct)) {
                const char* fp = strcmp(op, "==") == 0 ? "oeq" : strcmp(op, "!=") == 0 ? "one" :
                                 strcmp(op, "<") == 0 ? "olt" : strcmp(op, "<=") == 0 ? "ole" :
                                 strcmp(op, ">") == 0 ? "ogt" : "oge";
                cg_emit("%s = fcmp %s %s %s, %s", r, fp, llvm_reg_ty(ct), lc, rc);
            } else {
                bool uns = type_is_unsigned_int(ct);
                const char* ip = strcmp(op, "==") == 0 ? "eq" : strcmp(op, "!=") == 0 ? "ne" :
                                 strcmp(op, "<") == 0 ? (uns ? "ult" : "slt") :
                                 strcmp(op, "<=") == 0 ? (uns ? "ule" : "sle") :
                                 strcmp(op, ">") == 0 ? (uns ? "ugt" : "sgt") : (uns ? "uge" : "sge");
                cg_emit("%s = icmp %s %s %s, %s", r, ip, llvm_reg_ty(ct), lc, rc);
            }
            return r;
        }
        const char* r = cg_reg();
        cg_emit("%s = icmp %s ptr %s, %s", r, is_ne ? "ne" : "eq", lv, rv);
        return r;
    }

    if (strcmp(op, "in") == 0 || strcmp(op, "not in") == 0) {
        bool neg = (strcmp(op, "not in") == 0);
        const char* hit = NULL;
        if (rt->kind == TY_SET) {
            const char* kv = cg_reg_to_mem(cg_coerce(lv, lt, rt->elem), rt->elem);
            hit = cg_reg();
            cg_emit("%s = call i1 @_%s_contains(ptr %s, %s %s)", hit, rt->mangled, rv, llvm_mem_ty(rt->elem), kv);
        } else if (rt->kind == TY_DICT) {
            const char* kv = cg_reg_to_mem(cg_coerce(lv, lt, rt->key), rt->key);
            hit = cg_reg();
            cg_emit("%s = call i1 @_%s_contains(ptr %s, %s %s)", hit, rt->mangled, rv, llvm_mem_ty(rt->key), kv);
        } else if (rt->kind == TY_LIST) {
            const char* kv = cg_reg_to_mem(cg_coerce(lv, lt, rt->elem), rt->elem);
            hit = cg_reg();
            cg_emit("%s = call i1 @_%s_contains(ptr %s, %s %s)", hit, rt->mangled, rv, llvm_mem_ty(rt->elem), kv);
        } else if (rt->kind == TY_STR) {
            const char* c = cg_reg(); hit = cg_reg();
            cg_emit("%s = call i32 @stricta_str_contains(ptr %s, ptr %s)", c, lv, rv);
            cg_emit("%s = icmp ne i32 %s, 0", hit, c);
        } else {
            hit = "false";
        }
        if (!neg) return hit;
        const char* r = cg_reg();
        cg_emit("%s = xor i1 %s, true", r, hit);
        return r;
    }

    if (lt->kind == TY_CLASS) {
        const char* dunder = strcmp(op, "+") == 0 ? "__add__" : strcmp(op, "-") == 0 ? "__sub__" : strcmp(op, "*") == 0 ? "__mul__" : NULL;
        FuncInfo* dm = dunder ? find_method_in_class(lt->cls, dunder) : NULL;
        if (dm) {
            const char* r = cg_reg();
            cg_emit("%s = call %s %s(%s %s, %s %s)", r, llvm_reg_ty(dm->ret_type), dm->llvm_name,
                    llvm_reg_ty(lt), lv, llvm_reg_ty(rt), rv);
            return r;
        }
    }
    if (lt->kind == TY_STR && rt->kind == TY_STR && strcmp(op, "+") == 0) {
        const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", r, lv, rv); return r;
    }
    if (lt->kind == TY_STR && type_is_int(rt) && strcmp(op, "*") == 0) {
        const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_str_repeat(ptr %s, i64 %s)", r, lv, cg_coerce(rv, rt, g_ty_i64)); return r;
    }
    if (lt->kind == TY_LIST && strcmp(op, "+") == 0) {
        const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_concat(ptr %s, ptr %s)", r, lt->mangled, lv, rv); return r;
    }
    if (lt->kind == TY_LIST && type_is_int(rt) && strcmp(op, "*") == 0) {
        const char* cnt = cg_coerce(rv, rt, g_ty_i64);
        const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_new(i32 8)", r, lt->mangled);
        const char* ip = cg_reg(); cg_entry("%s = alloca i64", ip); cg_emit("store i64 0, ptr %s", ip);
        const char* ll = cg_lbl("lm_l"); const char* lb = cg_lbl("lm_b"); const char* ld = cg_lbl("lm_d");
        cg_br(ll); cg_label(ll);
        const char* iv = cg_reg(); const char* c = cg_reg();
        cg_emit("%s = load i64, ptr %s", iv, ip); cg_emit("%s = icmp slt i64 %s, %s", c, iv, cnt);
        cg_cond_br(c, lb, ld); cg_label(lb);
        cg_emit("call void @_%s_extend(ptr %s, ptr %s)", lt->mangled, r, lv);
        const char* in = cg_reg(); cg_emit("%s = add i64 %s, 1", in, iv); cg_emit("store i64 %s, ptr %s", in, ip);
        cg_br(ll); cg_label(ld);
        return r;
    }
    if (lt->kind == TY_SET) {
        const char* fnm = strcmp(op, "|") == 0 ? "union" : strcmp(op, "&") == 0 ? "intersection" :
                          strcmp(op, "-") == 0 ? "difference" : "symmetric_difference";
        const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_%s(ptr %s, ptr %s)", r, lt->mangled, fnm, lv, rv); return r;
    }
    if (lt->kind == TY_DICT && strcmp(op, "|") == 0) {
        const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_merge(ptr %s, ptr %s)", r, lt->mangled, lv, rv); return r;
    }
    if (lt->kind == TY_BIGINT) {
        const char* fnm = strcmp(op, "+") == 0 ? "add" : strcmp(op, "-") == 0 ? "sub" :
                          strcmp(op, "*") == 0 ? "mul" : strcmp(op, "//") == 0 ? "div" :
                          strcmp(op, "%") == 0 ? "mod" : "pow";
        const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_bigint_%s(ptr %s, ptr %s)", r, fnm, lv, rv); return r;
    }

    /* Numeric arithmetic */
    Type* ct = res_ty;
    if (strcmp(op, "/") == 0) ct = g_ty_f64;
    const char* lc = cg_coerce(lv, lt, ct);
    const char* rc = cg_coerce(rv, rt, ct);
    const char* llt = llvm_reg_ty(ct);
    const char* r = cg_reg();

    if (type_is_float(ct)) {
        if (strcmp(op, "+") == 0) cg_emit("%s = fadd %s %s, %s", r, llt, lc, rc);
        else if (strcmp(op, "-") == 0) cg_emit("%s = fsub %s %s, %s", r, llt, lc, rc);
        else if (strcmp(op, "*") == 0) cg_emit("%s = fmul %s %s, %s", r, llt, lc, rc);
        else if (strcmp(op, "/") == 0) cg_emit("%s = fdiv %s %s, %s", r, llt, lc, rc);
        else if (strcmp(op, "//") == 0) {
            const char* d = cg_reg();
            cg_emit("%s = fdiv %s %s, %s", d, llt, lc, rc);
            cg_emit("%s = call %s @llvm.floor.%s(%s %s)", r, llt, ct->kind == TY_F32 ? "f32" : "f64", llt, d);
        } else if (strcmp(op, "%") == 0) {
            cg_emit("%s = frem %s %s, %s", r, llt, lc, rc);
        } else if (strcmp(op, "**") == 0) {
            const char* ld = cg_coerce(lc, ct, g_ty_f64);
            const char* rd = cg_coerce(rc, ct, g_ty_f64);
            const char* p = cg_reg();
            cg_emit("%s = call double @pow(double %s, double %s)", p, ld, rd);
            return cg_coerce(p, g_ty_f64, ct);
        }
        return r;
    }

    bool uns = type_is_unsigned_int(ct);
    if (strcmp(op, "+") == 0) cg_emit("%s = add %s %s, %s", r, llt, lc, rc);
    else if (strcmp(op, "-") == 0) cg_emit("%s = sub %s %s, %s", r, llt, lc, rc);
    else if (strcmp(op, "*") == 0) cg_emit("%s = mul %s %s, %s", r, llt, lc, rc);
    else if (strcmp(op, "&") == 0) cg_emit("%s = and %s %s, %s", r, llt, lc, rc);
    else if (strcmp(op, "|") == 0) cg_emit("%s = or %s %s, %s", r, llt, lc, rc);
    else if (strcmp(op, "^") == 0) cg_emit("%s = xor %s %s, %s", r, llt, lc, rc);
    else if (strcmp(op, "<<") == 0) cg_emit("%s = shl %s %s, %s", r, llt, lc, rc);
    else if (strcmp(op, ">>") == 0) cg_emit("%s = %s %s %s, %s", r, uns ? "lshr" : "ashr", llt, lc, rc);
    else if (strcmp(op, "//") == 0) {
        if (uns) { cg_emit("%s = udiv %s %s, %s", r, llt, lc, rc); }
        else {
            const char* q = cg_reg(); const char* rem = cg_reg();
            const char* rnz = cg_reg(); const char* xr = cg_reg(); const char* neg = cg_reg(); const char* adj = cg_reg();
            cg_emit("%s = sdiv %s %s, %s", q, llt, lc, rc);
            cg_emit("%s = srem %s %s, %s", rem, llt, lc, rc);
            cg_emit("%s = icmp ne %s %s, 0", rnz, llt, rem);
            cg_emit("%s = xor %s %s, %s", xr, llt, rem, rc);
            cg_emit("%s = icmp slt %s %s, 0", neg, llt, xr);
            cg_emit("%s = and i1 %s, %s", adj, rnz, neg);
            const char* sub = cg_reg();
            cg_emit("%s = zext i1 %s to %s", sub, adj, llt);
            cg_emit("%s = sub %s %s, %s", r, llt, q, sub);
        }
    } else if (strcmp(op, "%") == 0) {
        if (uns) { cg_emit("%s = urem %s %s, %s", r, llt, lc, rc); }
        else {
            const char* rem = cg_reg(); const char* rnz = cg_reg(); const char* xr = cg_reg();
            const char* neg = cg_reg(); const char* adj = cg_reg(); const char* add = cg_reg();
            cg_emit("%s = srem %s %s, %s", rem, llt, lc, rc);
            cg_emit("%s = icmp ne %s %s, 0", rnz, llt, rem);
            cg_emit("%s = xor %s %s, %s", xr, llt, rem, rc);
            cg_emit("%s = icmp slt %s %s, 0", neg, llt, xr);
            cg_emit("%s = and i1 %s, %s", adj, rnz, neg);
            cg_emit("%s = add %s %s, %s", add, llt, rem, rc);
            cg_emit("%s = select i1 %s, %s %s, %s %s", r, adj, llt, add, llt, rem);
        }
    } else if (strcmp(op, "**") == 0) {
        const char* p = cg_reg();
        cg_emit("%s = call i64 @stricta_pow_i64(i64 %s, i64 %s)", p, cg_coerce(lc, ct, g_ty_i64), cg_coerce(rc, ct, g_ty_i64));
        return cg_coerce(p, g_ty_i64, ct);
    }
    return r;
}

static const char* cg_construct_class(ClassInfo* cls, Expr** args, int argc) {
    if (cls->is_value_type) {
        const char* ptr = cg_reg();
        cg_entry("%s = alloca %s", ptr, cls->llvm_name);
        cg_emit("store %s zeroinitializer, ptr %s", cls->llvm_name, ptr);
        for (int f = 0; f < cls->field_count; f++) {
            if (cls->fields[f].default_expr) {
                const char* dv = cg_expr(cls->fields[f].default_expr, cls->fields[f].type);
                const char* fp = cg_reg();
                cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, cls->llvm_name, ptr, cls->fields[f].slot_index);
                cg_emit("store %s %s, ptr %s", llvm_mem_ty(cls->fields[f].type), cg_reg_to_mem(dv, cls->fields[f].type), fp);
            }
        }
        if (cls->is_dataclass && !find_method_in_class_exact(cls, "__init__")) {
            for (int i = 0; i < argc && i < cls->field_count; i++) {
                const char* av = cg_coerce(cg_expr(args[i], cls->fields[i].type), args[i]->ty, cls->fields[i].type);
                const char* fp = cg_reg();
                cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, cls->llvm_name, ptr, cls->fields[i].slot_index);
                cg_emit("store %s %s, ptr %s", llvm_mem_ty(cls->fields[i].type), cg_reg_to_mem(av, cls->fields[i].type), fp);
            }
        }
        const char* val = cg_reg();
        cg_emit("%s = load %s, ptr %s", val, cls->llvm_name, ptr);
        return val;
    }

    if (cls->has_unimpl_abstract) {
        cg_emit("call void @stricta_raise(i32 5, ptr null, ptr %s)", cg_cstr_lit("TypeError: cannot instantiate abstract class", 44));
        return "null";
    }
    const char* sz = cg_reg();
    const char* obj = cg_reg();
    cg_emit("%s = ptrtoint ptr getelementptr (%s, ptr null, i32 1) to i64", sz, cls->llvm_name);
    if (find_method_in_class(cls, "__init__") || cls->is_dataclass) {
        cg_emit("%s = call ptr @malloc(i64 %s)", obj, sz);
    } else {
        cg_emit("%s = call ptr @calloc(i64 1, i64 %s)", obj, sz);
    }
    if (cls->needs_vtable) {
        const char* vp = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", vp, cls->llvm_name, obj);
        cg_emit("store ptr @_vtable_%s%s, ptr %s", cls->mod->prefix, cls->name, vp);
    }
    for (int f = 0; f < cls->field_count; f++) {
        if (cls->fields[f].default_expr) {
            const char* dv = cg_expr(cls->fields[f].default_expr, cls->fields[f].type);
            const char* fp = cg_reg();
            cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, cls->llvm_name, obj, cls->fields[f].slot_index);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(cls->fields[f].type), cg_reg_to_mem(dv, cls->fields[f].type), fp);
        }
    }
    FuncInfo* init_fn = find_method_in_class(cls, "__init__");
    if (init_fn) {
        StrBuf call_args; sb_init(&call_args);
        sb_printf(&call_args, "ptr %s", obj);
        for (int i = 1; i < init_fn->param_count; i++) {
            Expr* ae = (i - 1 < argc) ? args[i - 1] : init_fn->param_defaults[i];
            Type* pt = init_fn->param_types[i];
            const char* av = cg_coerce(cg_expr(ae, pt), ae->ty, pt);
            sb_printf(&call_args, ", %s %s", llvm_reg_ty(pt), av);
        }
        cg_emit("call void %s(%s)", init_fn->llvm_name, call_args.data);
        free(call_args.data);
    } else if (cls->is_dataclass) {
        for (int i = 0; i < argc && i < cls->field_count; i++) {
            const char* av = cg_coerce(cg_expr(args[i], cls->fields[i].type), args[i]->ty, cls->fields[i].type);
            const char* fp = cg_reg();
            cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, cls->llvm_name, obj, cls->fields[i].slot_index);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(cls->fields[i].type), cg_reg_to_mem(av, cls->fields[i].type), fp);
        }
    }
    return obj;
}

static void cg_iter_loop(Expr* iter_e, const char** targets, int target_count, Stmt** body, int body_count,
                         const char* else_lbl, const char* after_lbl) {
    /* Fast path: range(...) */
    if (iter_e->kind == EXPR_CALL && iter_e->call.callee->kind == EXPR_VAR &&
        strcmp(iter_e->call.callee->var.name, "range") == 0) {
        const char* start_v = "0";
        const char* stop_v = NULL;
        const char* step_v = "1";
        int ac = iter_e->call.arg_count;
        if (ac == 1) {
            stop_v = cg_coerce(cg_expr(iter_e->call.args[0], g_ty_i64), iter_e->call.args[0]->ty, g_ty_i64);
        } else if (ac >= 2) {
            start_v = cg_coerce(cg_expr(iter_e->call.args[0], g_ty_i64), iter_e->call.args[0]->ty, g_ty_i64);
            stop_v = cg_coerce(cg_expr(iter_e->call.args[1], g_ty_i64), iter_e->call.args[1]->ty, g_ty_i64);
            if (ac == 3) step_v = cg_coerce(cg_expr(iter_e->call.args[2], g_ty_i64), iter_e->call.args[2]->ty, g_ty_i64);
        }
        Type* vt = NULL;
        const char* vp = cg_var_ptr(targets[0], &vt);
        const char* idx_p = cg_reg();
        cg_entry("%s = alloca i64", idx_p);
        cg_emit("store i64 %s, ptr %s", start_v, idx_p);
        const char* l_cond = cg_lbl("for_cond");
        const char* l_body = cg_lbl("for_body");
        const char* l_step = cg_lbl("for_step");
        cg_br(l_cond);
        cg_label(l_cond);
        const char* cur = cg_reg();
        const char* c = cg_reg();
        cg_emit("%s = load i64, ptr %s", cur, idx_p);
        if (ac == 3 && iter_e->call.args[2]->kind == EXPR_UNOP && strcmp(iter_e->call.args[2]->unop.op, "-") == 0) {
            cg_emit("%s = icmp sgt i64 %s, %s", c, cur, stop_v);
        } else {
            cg_emit("%s = icmp slt i64 %s, %s", c, cur, stop_v);
        }
        cg_cond_br(c, l_body, else_lbl ? else_lbl : after_lbl);
        cg_label(l_body);
        cg_emit("store i64 %s, ptr %s", cg_coerce(cur, g_ty_i64, vt), vp);
        g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){after_lbl, l_step, NULL, NULL};
        cg_stmts(body, body_count);
        g_cg.loop_depth--;
        cg_br(l_step);
        cg_label(l_step);
        const char* cur2 = cg_reg(); const char* nxt = cg_reg();
        cg_emit("%s = load i64, ptr %s", cur2, idx_p);
        cg_emit("%s = add i64 %s, %s", nxt, cur2, step_v);
        cg_emit("store i64 %s, ptr %s", nxt, idx_p);
        cg_br(l_cond);
        return;
    }

    /* Fast path: dict.items() zero-alloc iteration */
    if (target_count == 2 && iter_e->kind == EXPR_CALL && iter_e->call.callee->kind == EXPR_ATTR &&
        iter_e->call.callee->attr.obj->ty && iter_e->call.callee->attr.obj->ty->kind == TY_DICT &&
        strcmp(iter_e->call.callee->attr.attr, "items") == 0) {
        Expr* dobj = iter_e->call.callee->attr.obj;
        const char* d = cg_expr(dobj, NULL);
        Type* dt = dobj->ty;
        const char* sty = arena_sprintf("%%%s", dt->mangled);
        const char* lp = cg_reg(); const char* len = cg_reg();
        const char* kp0 = cg_reg(); const char* keys = cg_reg();
        const char* vp0 = cg_reg(); const char* vals = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 3", lp, sty, d);
        cg_emit("%s = load i32, ptr %s", len, lp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", kp0, sty, d);
        cg_emit("%s = load ptr, ptr %s", keys, kp0);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 1", vp0, sty, d);
        cg_emit("%s = load ptr, ptr %s", vals, vp0);
        const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p);
        cg_emit("store i32 0, ptr %s", i_p);
        Type* kvt = NULL; Type* vvt = NULL;
        const char* kvp = cg_var_ptr(targets[0], &kvt);
        const char* vvp = cg_var_ptr(targets[1], &vvt);
        const char* l_cond = cg_lbl("dit_cond"); const char* l_body = cg_lbl("dit_body"); const char* l_step = cg_lbl("dit_step");
        cg_br(l_cond);
        cg_label(l_cond);
        const char* iv = cg_reg(); const char* c = cg_reg();
        cg_emit("%s = load i32, ptr %s", iv, i_p);
        cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
        cg_cond_br(c, l_body, else_lbl ? else_lbl : after_lbl);
        cg_label(l_body);
        const char* kp = cg_reg(); const char* km = cg_reg();
        const char* vp = cg_reg(); const char* vm = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", kp, llvm_mem_ty(dt->key), keys, iv);
        cg_emit("%s = load %s, ptr %s", km, llvm_mem_ty(dt->key), kp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", vp, llvm_mem_ty(dt->val), vals, iv);
        cg_emit("%s = load %s, ptr %s", vm, llvm_mem_ty(dt->val), vp);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(kvt), km, kvp);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vvt), vm, vvp);
        g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){after_lbl, l_step, NULL, NULL};
        cg_stmts(body, body_count);
        g_cg.loop_depth--;
        cg_br(l_step);
        cg_label(l_step);
        const char* iv2 = cg_reg(); const char* in = cg_reg();
        cg_emit("%s = load i32, ptr %s", iv2, i_p);
        cg_emit("%s = add i32 %s, 1", in, iv2);
        cg_emit("store i32 %s, ptr %s", in, i_p);
        cg_br(l_cond);
        return;
    }

    /* Fast path: dict.values() zero-alloc iteration */
    if (target_count == 1 && iter_e->kind == EXPR_CALL && iter_e->call.callee->kind == EXPR_ATTR &&
        iter_e->call.callee->attr.obj->ty && iter_e->call.callee->attr.obj->ty->kind == TY_DICT &&
        strcmp(iter_e->call.callee->attr.attr, "values") == 0) {
        Expr* dobj = iter_e->call.callee->attr.obj;
        const char* d = cg_expr(dobj, NULL);
        Type* dt = dobj->ty;
        const char* sty = arena_sprintf("%%%s", dt->mangled);
        const char* lp = cg_reg(); const char* len = cg_reg();
        const char* vp0 = cg_reg(); const char* vals = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 3", lp, sty, d);
        cg_emit("%s = load i32, ptr %s", len, lp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 1", vp0, sty, d);
        cg_emit("%s = load ptr, ptr %s", vals, vp0);
        const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p);
        cg_emit("store i32 0, ptr %s", i_p);
        Type* vvt = NULL;
        const char* vvp = cg_var_ptr(targets[0], &vvt);
        const char* l_cond = cg_lbl("dv_cond"); const char* l_body = cg_lbl("dv_body"); const char* l_step = cg_lbl("dv_step");
        cg_br(l_cond);
        cg_label(l_cond);
        const char* iv = cg_reg(); const char* c = cg_reg();
        cg_emit("%s = load i32, ptr %s", iv, i_p);
        cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
        cg_cond_br(c, l_body, else_lbl ? else_lbl : after_lbl);
        cg_label(l_body);
        const char* vp = cg_reg(); const char* vm = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", vp, llvm_mem_ty(dt->val), vals, iv);
        cg_emit("%s = load %s, ptr %s", vm, llvm_mem_ty(dt->val), vp);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vvt), vm, vvp);
        g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){after_lbl, l_step, NULL, NULL};
        cg_stmts(body, body_count);
        g_cg.loop_depth--;
        cg_br(l_step);
        cg_label(l_step);
        const char* iv2 = cg_reg(); const char* in = cg_reg();
        cg_emit("%s = load i32, ptr %s", iv2, i_p);
        cg_emit("%s = add i32 %s, 1", in, iv2);
        cg_emit("store i32 %s, ptr %s", in, i_p);
        cg_br(l_cond);
        return;
    }

    const char* iv_reg = cg_expr(iter_e, NULL);
    Type* it_ty = iter_e->ty;
    const char* len = cg_reg();
    const char* arr = NULL;
    Type* el_ty = NULL;
    if (it_ty->kind == TY_LIST) {
        const char* sty = arena_sprintf("%%%s", it_ty->mangled);
        const char* lp = cg_reg(); const char* ip = cg_reg(); arr = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", lp, sty, iv_reg);
        cg_emit("%s = load i32, ptr %s", len, lp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", ip, sty, iv_reg);
        cg_emit("%s = load ptr, ptr %s", arr, ip);
        el_ty = it_ty->elem;
    } else if (it_ty->kind == TY_SET) {
        const char* sty = arena_sprintf("%%%s", it_ty->mangled);
        const char* lp = cg_reg(); const char* ip = cg_reg(); arr = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", lp, sty, iv_reg);
        cg_emit("%s = load i32, ptr %s", len, lp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", ip, sty, iv_reg);
        cg_emit("%s = load ptr, ptr %s", arr, ip);
        el_ty = it_ty->elem;
    } else if (it_ty->kind == TY_DICT) {
        const char* sty = arena_sprintf("%%%s", it_ty->mangled);
        const char* lp = cg_reg(); const char* ip = cg_reg(); arr = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 3", lp, sty, iv_reg);
        cg_emit("%s = load i32, ptr %s", len, lp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", ip, sty, iv_reg);
        cg_emit("%s = load ptr, ptr %s", arr, ip);
        el_ty = it_ty->key;
    } else if (it_ty->kind == TY_STR) {
        const char* lp = cg_reg();
        cg_emit("%s = getelementptr %%StrictaStr, ptr %s, i32 0, i32 0", lp, iv_reg);
        cg_emit("%s = load i32, ptr %s", len, lp);
        el_ty = g_ty_str;
    }

    const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p);
    cg_emit("store i32 0, ptr %s", i_p);
    const char* l_cond = cg_lbl("it_cond"); const char* l_body = cg_lbl("it_body"); const char* l_step = cg_lbl("it_step");
    cg_br(l_cond);
    cg_label(l_cond);
    const char* iv = cg_reg(); const char* c = cg_reg();
    cg_emit("%s = load i32, ptr %s", iv, i_p);
    cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
    cg_cond_br(c, l_body, else_lbl ? else_lbl : after_lbl);
    cg_label(l_body);

    const char* elem_v = NULL;
    if (it_ty->kind == TY_STR) {
        const char* iv64 = cg_reg(); elem_v = cg_reg();
        cg_emit("%s = sext i32 %s to i64", iv64, iv);
        cg_emit("%s = call ptr @stricta_str_char_at(ptr %s, i64 %s)", elem_v, iv_reg, iv64);
    } else {
        const char* ep = cg_reg(); const char* em = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, llvm_mem_ty(el_ty), arr, iv);
        cg_emit("%s = load %s, ptr %s", em, llvm_mem_ty(el_ty), ep);
        elem_v = cg_mem_to_reg(em, el_ty);
    }
    if (target_count == 1) {
        Type* vt = NULL;
        const char* vp = cg_var_ptr(targets[0], &vt);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(cg_coerce(elem_v, el_ty, vt), vt), vp);
    } else {
        for (int k = 0; k < target_count; k++) {
            Type* vt = NULL;
            const char* vp = cg_var_ptr(targets[k], &vt);
            const char* fm = cg_reg();
            cg_emit("%s = extractvalue %s %s, %d", fm, llvm_reg_ty(el_ty), elem_v, k);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), fm, vp);
        }
    }
    g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){after_lbl, l_step, NULL, NULL};
    cg_stmts(body, body_count);
    g_cg.loop_depth--;
    cg_br(l_step);
    cg_label(l_step);
    const char* iv2 = cg_reg(); const char* in = cg_reg();
    cg_emit("%s = load i32, ptr %s", iv2, i_p);
    cg_emit("%s = add i32 %s, 1", in, iv2);
    cg_emit("store i32 %s, ptr %s", in, i_p);
    cg_br(l_cond);
}

static void cg_comp_loop_emit(CompGen* gens, int gidx, int gcount, void (*emit_leaf)(void*), void* ctx) {
    if (gidx >= gcount) {
        emit_leaf(ctx);
        return;
    }
    CompGen* cg = &gens[gidx];
    const char* l_after = cg_lbl("cg_after");
    if (cg->iter->kind == EXPR_CALL && cg->iter->call.callee->kind == EXPR_VAR &&
        strcmp(cg->iter->call.callee->var.name, "range") == 0) {
        const char* start_v = "0"; const char* stop_v = NULL; const char* step_v = "1";
        int ac = cg->iter->call.arg_count;
        if (ac == 1) stop_v = cg_coerce(cg_expr(cg->iter->call.args[0], g_ty_i64), cg->iter->call.args[0]->ty, g_ty_i64);
        else {
            start_v = cg_coerce(cg_expr(cg->iter->call.args[0], g_ty_i64), cg->iter->call.args[0]->ty, g_ty_i64);
            stop_v = cg_coerce(cg_expr(cg->iter->call.args[1], g_ty_i64), cg->iter->call.args[1]->ty, g_ty_i64);
            if (ac == 3) step_v = cg_coerce(cg_expr(cg->iter->call.args[2], g_ty_i64), cg->iter->call.args[2]->ty, g_ty_i64);
        }
        Type* vt = NULL; const char* vp = cg_var_ptr(cg->targets[0], &vt);
        const char* i_p = cg_reg(); cg_entry("%s = alloca i64", i_p);
        cg_emit("store i64 %s, ptr %s", start_v, i_p);
        const char* l_cond = cg_lbl("cg_cond"); const char* l_body = cg_lbl("cg_body"); const char* l_step = cg_lbl("cg_step");
        cg_br(l_cond);
        cg_label(l_cond);
        const char* iv = cg_reg(); const char* c = cg_reg();
        cg_emit("%s = load i64, ptr %s", iv, i_p);
        cg_emit("%s = icmp slt i64 %s, %s", c, iv, stop_v);
        cg_cond_br(c, l_body, l_after);
        cg_label(l_body);
        cg_emit("store i64 %s, ptr %s", iv, vp);
        for (int f = 0; f < cg->if_count; f++) {
            const char* fv = cg_truthy(cg_expr(cg->ifs[f], g_ty_bool), cg->ifs[f]->ty);
            const char* l_ok = cg_lbl("cg_ok");
            cg_cond_br(fv, l_ok, l_step);
            cg_label(l_ok);
        }
        cg_comp_loop_emit(gens, gidx + 1, gcount, emit_leaf, ctx);
        cg_br(l_step);
        cg_label(l_step);
        const char* iv2 = cg_reg(); const char* in = cg_reg();
        cg_emit("%s = load i64, ptr %s", iv2, i_p);
        cg_emit("%s = add i64 %s, %s", in, iv2, step_v);
        cg_emit("store i64 %s, ptr %s", in, i_p);
        cg_br(l_cond);
        cg_label(l_after);
        return;
    }

    /* Fast path: dict.items() zero-alloc iteration in comprehension */
    if (cg->target_count == 2 && cg->iter->kind == EXPR_CALL && cg->iter->call.callee->kind == EXPR_ATTR &&
        cg->iter->call.callee->attr.obj->ty && cg->iter->call.callee->attr.obj->ty->kind == TY_DICT &&
        strcmp(cg->iter->call.callee->attr.attr, "items") == 0) {
        Expr* dobj = cg->iter->call.callee->attr.obj;
        const char* d = cg_expr(dobj, NULL);
        Type* dt = dobj->ty;
        const char* sty = arena_sprintf("%%%s", dt->mangled);
        const char* lp = cg_reg(); const char* len = cg_reg();
        const char* kp0 = cg_reg(); const char* keys = cg_reg();
        const char* vp0 = cg_reg(); const char* vals = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 3", lp, sty, d);
        cg_emit("%s = load i32, ptr %s", len, lp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", kp0, sty, d);
        cg_emit("%s = load ptr, ptr %s", keys, kp0);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 1", vp0, sty, d);
        cg_emit("%s = load ptr, ptr %s", vals, vp0);
        const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p);
        cg_emit("store i32 0, ptr %s", i_p);
        Type* kvt = NULL; Type* vvt = NULL;
        const char* kvp = cg_var_ptr(cg->targets[0], &kvt);
        const char* vvp = cg_var_ptr(cg->targets[1], &vvt);
        const char* l_cond = cg_lbl("cg_dic"); const char* l_body = cg_lbl("cg_dib"); const char* l_step = cg_lbl("cg_dis");
        cg_br(l_cond);
        cg_label(l_cond);
        const char* iv = cg_reg(); const char* c = cg_reg();
        cg_emit("%s = load i32, ptr %s", iv, i_p);
        cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
        cg_cond_br(c, l_body, l_after);
        cg_label(l_body);
        const char* kp = cg_reg(); const char* km = cg_reg();
        const char* vp = cg_reg(); const char* vm = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", kp, llvm_mem_ty(dt->key), keys, iv);
        cg_emit("%s = load %s, ptr %s", km, llvm_mem_ty(dt->key), kp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", vp, llvm_mem_ty(dt->val), vals, iv);
        cg_emit("%s = load %s, ptr %s", vm, llvm_mem_ty(dt->val), vp);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(kvt), km, kvp);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vvt), vm, vvp);
        for (int f = 0; f < cg->if_count; f++) {
            const char* fv = cg_truthy(cg_expr(cg->ifs[f], g_ty_bool), cg->ifs[f]->ty);
            const char* l_ok = cg_lbl("cg_ok");
            cg_cond_br(fv, l_ok, l_step);
            cg_label(l_ok);
        }
        cg_comp_loop_emit(gens, gidx + 1, gcount, emit_leaf, ctx);
        cg_br(l_step);
        cg_label(l_step);
        const char* iv2 = cg_reg(); const char* in = cg_reg();
        cg_emit("%s = load i32, ptr %s", iv2, i_p);
        cg_emit("%s = add i32 %s, 1", in, iv2);
        cg_emit("store i32 %s, ptr %s", in, i_p);
        cg_br(l_cond);
        cg_label(l_after);
        return;
    }

    /* Iterable in comprehension */
    const char* iv_reg = cg_expr(cg->iter, NULL);
    Type* it_ty = cg->iter->ty;
    const char* len = cg_reg(); const char* arr = NULL; Type* el_ty = NULL;
    if (it_ty->kind == TY_LIST) {
        const char* sty = arena_sprintf("%%%s", it_ty->mangled);
        const char* lp = cg_reg(); const char* ip = cg_reg(); arr = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", lp, sty, iv_reg);
        cg_emit("%s = load i32, ptr %s", len, lp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", ip, sty, iv_reg);
        cg_emit("%s = load ptr, ptr %s", arr, ip);
        el_ty = it_ty->elem;
    } else if (it_ty->kind == TY_DICT && cg->target_count == 2) {
        /* shouldn't happen unless items(), handled below */
        el_ty = it_ty->key;
    } else if (it_ty->kind == TY_SET) {
        const char* sty = arena_sprintf("%%%s", it_ty->mangled);
        const char* lp = cg_reg(); const char* ip = cg_reg(); arr = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", lp, sty, iv_reg);
        cg_emit("%s = load i32, ptr %s", len, lp);
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", ip, sty, iv_reg);
        cg_emit("%s = load ptr, ptr %s", arr, ip);
        el_ty = it_ty->elem;
    } else if (it_ty->kind == TY_STR) {
        const char* lp = cg_reg();
        cg_emit("%s = getelementptr %%StrictaStr, ptr %s, i32 0, i32 0", lp, iv_reg);
        cg_emit("%s = load i32, ptr %s", len, lp);
        el_ty = g_ty_str;
    } else if (it_ty->kind == TY_TUPLE) {
        /* Materialize tuple items into stack array or handle fixed length */
        int n = it_ty->tuple_count;
        Type* et = n > 0 ? it_ty->tuple_elems[0] : g_ty_i64;
        const char* tarr = cg_reg();
        cg_entry("%s = alloca [%d x %s]", tarr, n > 0 ? n : 1, llvm_mem_ty(et));
        for (int k = 0; k < n; k++) {
            const char* ev = cg_reg(); const char* tp = cg_reg();
            cg_emit("%s = extractvalue %s %s, %d", ev, llvm_reg_ty(it_ty), iv_reg, k);
            cg_emit("%s = getelementptr [%d x %s], ptr %s, i32 0, i32 %d", tp, n, llvm_mem_ty(et), tarr, k);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(et), ev, tp);
        }
        cg_emit("%s = add i32 %d, 0", len, n);
        arr = tarr;
        el_ty = et;
    }

    const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p);
    cg_emit("store i32 0, ptr %s", i_p);
    const char* l_cond = cg_lbl("cg_cond"); const char* l_body = cg_lbl("cg_body"); const char* l_step = cg_lbl("cg_step");
    cg_br(l_cond);
    cg_label(l_cond);
    const char* iv = cg_reg(); const char* c = cg_reg();
    cg_emit("%s = load i32, ptr %s", iv, i_p);
    cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
    cg_cond_br(c, l_body, l_after);
    cg_label(l_body);
    const char* ev = NULL;
    if (it_ty->kind == TY_STR) {
        const char* iv64 = cg_reg(); ev = cg_reg();
        cg_emit("%s = sext i32 %s to i64", iv64, iv);
        cg_emit("%s = call ptr @stricta_str_char_at(ptr %s, i64 %s)", ev, iv_reg, iv64);
    } else {
        const char* ep = cg_reg(); const char* em = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, llvm_mem_ty(el_ty), arr, iv);
        cg_emit("%s = load %s, ptr %s", em, llvm_mem_ty(el_ty), ep);
        ev = cg_mem_to_reg(em, el_ty);
    }
    if (cg->target_count == 1) {
        Type* vt = NULL; const char* vp = cg_var_ptr(cg->targets[0], &vt);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(ev, el_ty), vp);
    } else {
        for (int k = 0; k < cg->target_count; k++) {
            Type* vt = NULL; const char* vp = cg_var_ptr(cg->targets[k], &vt);
            const char* fm = cg_reg();
            cg_emit("%s = extractvalue %s %s, %d", fm, llvm_reg_ty(el_ty), ev, k);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), fm, vp);
        }
    }
    for (int f = 0; f < cg->if_count; f++) {
        const char* fv = cg_truthy(cg_expr(cg->ifs[f], g_ty_bool), cg->ifs[f]->ty);
        const char* l_ok = cg_lbl("cg_ok");
        cg_cond_br(fv, l_ok, l_step);
        cg_label(l_ok);
    }
    cg_comp_loop_emit(gens, gidx + 1, gcount, emit_leaf, ctx);
    cg_br(l_step);
    cg_label(l_step);
    const char* iv2 = cg_reg(); const char* in = cg_reg();
    cg_emit("%s = load i32, ptr %s", iv2, i_p);
    cg_emit("%s = add i32 %s, 1", in, iv2);
    cg_emit("store i32 %s, ptr %s", in, i_p);
    cg_br(l_cond);
    cg_label(l_after);
}

typedef struct { Expr* comp; const char* container; Type* cty; } CompLeafCtx;

static void comp_leaf_list(void* vctx) {
    CompLeafCtx* c = (CompLeafCtx*)vctx;
    Type* el = c->cty->elem;
    const char* ev = cg_coerce(cg_expr(c->comp->comp.elt, el), c->comp->comp.elt->ty, el);
    cg_emit("call void @_%s_append(ptr %s, %s %s)", c->cty->mangled, c->container, llvm_mem_ty(el), cg_reg_to_mem(ev, el));
}

static void comp_leaf_set(void* vctx) {
    CompLeafCtx* c = (CompLeafCtx*)vctx;
    Type* el = c->cty->elem;
    const char* ev = cg_coerce(cg_expr(c->comp->comp.elt, el), c->comp->comp.elt->ty, el);
    cg_emit("call void @_%s_add(ptr %s, %s %s)", c->cty->mangled, c->container, llvm_mem_ty(el), cg_reg_to_mem(ev, el));
}

static void comp_leaf_dict(void* vctx) {
    CompLeafCtx* c = (CompLeafCtx*)vctx;
    Type* kt = c->cty->key; Type* vt = c->cty->val;
    const char* kv = cg_coerce(cg_expr(c->comp->comp.elt, kt), c->comp->comp.elt->ty, kt);
    const char* vv = cg_coerce(cg_expr(c->comp->comp.val, vt), c->comp->comp.val->ty, vt);
    cg_emit("call void @_%s_set(ptr %s, %s %s, %s %s)", c->cty->mangled, c->container,
            llvm_mem_ty(kt), cg_reg_to_mem(kv, kt), llvm_mem_ty(vt), cg_reg_to_mem(vv, vt));
}

static void comp_leaf_sum(void* vctx) {
    CompLeafCtx* c = (CompLeafCtx*)vctx;
    Type* el = c->cty;
    const char* ev = cg_coerce(cg_expr(c->comp->comp.elt, el), c->comp->comp.elt->ty, el);
    const char* cur = cg_reg(); const char* sum = cg_reg();
    cg_emit("%s = load %s, ptr %s", cur, llvm_reg_ty(el), c->container);
    if (type_is_float(el)) cg_emit("%s = fadd %s %s, %s", sum, llvm_reg_ty(el), cur, ev);
    else cg_emit("%s = add %s %s, %s", sum, llvm_reg_ty(el), cur, ev);
    cg_emit("store %s %s, ptr %s", llvm_reg_ty(el), sum, c->container);
}

static const char* cg_method_call(Expr* call, Expr* obj_e, const char* mname, Type* obj_ty) {
    int argc = call->call.arg_count;
    Expr** args = call->call.args;

    if (obj_ty->kind == TY_STR) {
        const char* self_v = cg_expr(obj_e, NULL);
        if (strcmp(mname, "split") == 0) {
            const char* sep = (argc == 1) ? cg_expr(args[0], g_ty_str) : "null";
            const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_str_split(ptr %s, ptr %s)", r, self_v, sep); return r;
        }
        if (strcmp(mname, "join") == 0) {
            const char* lv = cg_expr(args[0], NULL);
            const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_str_join(ptr %s, ptr %s)", r, self_v, lv); return r;
        }
        if (strcmp(mname, "upper") == 0 || strcmp(mname, "lower") == 0 ||
            strcmp(mname, "strip") == 0 || strcmp(mname, "lstrip") == 0 || strcmp(mname, "rstrip") == 0) {
            const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_str_%s(ptr %s)", r, mname, self_v); return r;
        }
        if (strcmp(mname, "replace") == 0) {
            const char* a0 = cg_expr(args[0], g_ty_str); const char* a1 = cg_expr(args[1], g_ty_str);
            const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_str_replace(ptr %s, ptr %s, ptr %s)", r, self_v, a0, a1); return r;
        }
        if (strcmp(mname, "find") == 0 || strcmp(mname, "index") == 0 || strcmp(mname, "count") == 0) {
            const char* a0 = cg_expr(args[0], g_ty_str);
            const char* r = cg_reg(); cg_emit("%s = call i64 @stricta_str_%s(ptr %s, ptr %s)", r, mname, self_v, a0); return r;
        }
        if (strcmp(mname, "startswith") == 0 || strcmp(mname, "endswith") == 0) {
            const char* a0 = cg_expr(args[0], g_ty_str);
            const char* r32 = cg_reg(); const char* r = cg_reg();
            cg_emit("%s = call i32 @stricta_str_%s(ptr %s, ptr %s)", r32, mname, self_v, a0);
            cg_emit("%s = icmp ne i32 %s, 0", r, r32);
            return r;
        }
        if (strcmp(mname, "format") == 0 && obj_e->kind == EXPR_STR) {
            const char* fmt = obj_e->str_lit.text;
            size_t flen = obj_e->str_lit.len;
            const char** arg_strs = argc ? (const char**)arena_alloc(sizeof(char*) * (size_t)argc) : NULL;
            for (int i = 0; i < argc; i++) {
                /* Wait to evaluate until needed, or evaluate once here */
                arg_strs[i] = cg_expr(args[i], NULL);
            }
            const char* acc = cg_strictastr_lit("", 0);
            int auto_idx = 0;
            size_t p = 0;
            StrBuf lit; sb_init(&lit);
            while (p < flen) {
                if (fmt[p] == '{' && p + 1 < flen && fmt[p + 1] == '{') { sb_append_n(&lit, "{", 1); p += 2; continue; }
                if (fmt[p] == '}' && p + 1 < flen && fmt[p + 1] == '}') { sb_append_n(&lit, "}", 1); p += 2; continue; }
                if (fmt[p] == '{') {
                    if (lit.len > 0) {
                        const char* n = cg_reg();
                        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, cg_strictastr_lit(lit.data, lit.len));
                        acc = n; lit.len = 0; lit.data[0] = '\0';
                    }
                    p++;
                    size_t st = p;
                    while (p < flen && fmt[p] != '}') p++;
                    char spec[64];
                    size_t slen = p - st;
                    if (slen >= sizeof(spec)) slen = sizeof(spec) - 1;
                    memcpy(spec, fmt + st, slen); spec[slen] = '\0';
                    if (p < flen) p++;
                    char* colon = strchr(spec, ':');
                    int aidx = auto_idx;
                    if (colon) *colon = '\0';
                    if (spec[0] != '\0') aidx = atoi(spec);
                    else auto_idx++;
                    const char* part = NULL;
                    if (aidx >= 0 && aidx < argc) {
                        if (colon && strchr(colon + 1, 'f')) {
                            const char* dot = strchr(colon + 1, '.');
                            int prec = dot ? atoi(dot + 1) : 6;
                            const char* fv = cg_coerce(arg_strs[aidx], args[aidx]->ty, g_ty_f64);
                            part = cg_reg();
                            cg_emit("%s = call ptr @stricta_float_fmt(double %s, i32 %d)", part, fv, prec);
                        } else {
                            part = cg_to_str(arg_strs[aidx], args[aidx]->ty);
                        }
                    } else {
                        part = cg_strictastr_lit("", 0);
                    }
                    const char* n = cg_reg();
                    cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, part);
                    acc = n;
                } else {
                    sb_append_n(&lit, &fmt[p], 1);
                    p++;
                }
            }
            if (lit.len > 0) {
                const char* n = cg_reg();
                cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, cg_strictastr_lit(lit.data, lit.len));
                acc = n;
            }
            free(lit.data);
            return acc;
        }
        return cg_strictastr_lit("", 0);
    }

    if (obj_ty->kind == TY_FILE) {
        const char* fv = cg_expr(obj_e, NULL);
        if (strcmp(mname, "read") == 0) {
            const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_file_read(ptr %s)", r, fv); return r;
        }
        if (strcmp(mname, "write") == 0) {
            const char* sv = cg_expr(args[0], g_ty_str);
            cg_emit("call void @stricta_file_write(ptr %s, ptr %s)", fv, sv); return "0";
        }
        if (strcmp(mname, "close") == 0) {
            cg_emit("call void @stricta_file_close(ptr %s)", fv); return "0";
        }
    }

    if (obj_ty->kind == TY_LIST) {
        const char* lv = cg_expr(obj_e, NULL);
        const char* m = obj_ty->mangled;
        Type* el = obj_ty->elem;
        const char* mty = llvm_mem_ty(el);
        if (strcmp(mname, "append") == 0) {
            const char* av = cg_reg_to_mem(cg_coerce(cg_expr(args[0], el), args[0]->ty, el), el);
            cg_emit("call void @_%s_append(ptr %s, %s %s)", m, lv, mty, av); return "0";
        }
        if (strcmp(mname, "extend") == 0) {
            const char* ov = cg_expr(args[0], obj_ty);
            cg_emit("call void @_%s_extend(ptr %s, ptr %s)", m, lv, ov); return "0";
        }
        if (strcmp(mname, "insert") == 0) {
            const char* iv = cg_coerce(cg_expr(args[0], g_ty_i64), args[0]->ty, g_ty_i64);
            const char* vv = cg_reg_to_mem(cg_coerce(cg_expr(args[1], el), args[1]->ty, el), el);
            cg_emit("call void @_%s_insert(ptr %s, i64 %s, %s %s)", m, lv, iv, mty, vv); return "0";
        }
        if (strcmp(mname, "pop") == 0) {
            const char* iv = argc == 1 ? cg_coerce(cg_expr(args[0], g_ty_i64), args[0]->ty, g_ty_i64) : "-1";
            const char* r = cg_reg(); cg_emit("%s = call %s @_%s_pop(ptr %s, i64 %s)", r, mty, m, lv, iv);
            return cg_mem_to_reg(r, el);
        }
        if (strcmp(mname, "remove") == 0) {
            const char* vv = cg_reg_to_mem(cg_coerce(cg_expr(args[0], el), args[0]->ty, el), el);
            cg_emit("call void @_%s_remove(ptr %s, %s %s)", m, lv, mty, vv); return "0";
        }
        if (strcmp(mname, "index") == 0 || strcmp(mname, "count") == 0) {
            const char* vv = cg_reg_to_mem(cg_coerce(cg_expr(args[0], el), args[0]->ty, el), el);
            const char* r = cg_reg(); cg_emit("%s = call i64 @_%s_%s(ptr %s, %s %s)", r, m, mname, lv, mty, vv); return r;
        }
        if (strcmp(mname, "sort") == 0 || strcmp(mname, "reverse") == 0) {
            cg_emit("call void @_%s_%s(ptr %s)", m, mname, lv); return "0";
        }
        if (strcmp(mname, "clear") == 0) {
            const char* lp = cg_reg();
            cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 0", lp, m, lv);
            cg_emit("store i32 0, ptr %s", lp); return "0";
        }
        if (strcmp(mname, "copy") == 0) {
            const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_copy(ptr %s)", r, m, lv); return r;
        }
    }

    if (obj_ty->kind == TY_SET) {
        const char* sv = cg_expr(obj_e, NULL);
        const char* m = obj_ty->mangled;
        Type* el = obj_ty->elem;
        const char* mty = llvm_mem_ty(el);
        if (strcmp(mname, "add") == 0) {
            const char* av = cg_reg_to_mem(cg_coerce(cg_expr(args[0], el), args[0]->ty, el), el);
            cg_emit("call void @_%s_add(ptr %s, %s %s)", m, sv, mty, av); return "0";
        }
        if (strcmp(mname, "discard") == 0 || strcmp(mname, "remove") == 0) {
            const char* av = cg_reg_to_mem(cg_coerce(cg_expr(args[0], el), args[0]->ty, el), el);
            cg_emit("call void @_%s_discard(ptr %s, %s %s)", m, sv, mty, av); return "0";
        }
        if (strcmp(mname, "pop") == 0) {
            const char* r = cg_reg(); cg_emit("%s = call %s @_%s_pop(ptr %s)", r, mty, m, sv);
            return cg_mem_to_reg(r, el);
        }
        if (strcmp(mname, "clear") == 0) { cg_emit("call void @_%s_clear(ptr %s)", m, sv); return "0"; }
        if (strcmp(mname, "copy") == 0) { const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_copy(ptr %s)", r, m, sv); return r; }
        if (strcmp(mname, "union") == 0 || strcmp(mname, "intersection") == 0 ||
            strcmp(mname, "difference") == 0 || strcmp(mname, "symmetric_difference") == 0) {
            const char* ov = cg_expr(args[0], obj_ty);
            const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_%s(ptr %s, ptr %s)", r, m, mname, sv, ov); return r;
        }
        if (strcmp(mname, "update") == 0 || strcmp(mname, "intersection_update") == 0 ||
            strcmp(mname, "difference_update") == 0 || strcmp(mname, "symmetric_difference_update") == 0) {
            const char* ov = cg_expr(args[0], obj_ty);
            cg_emit("call void @_%s_%s(ptr %s, ptr %s)", m, mname, sv, ov); return "0";
        }
        if (strcmp(mname, "issubset") == 0 || strcmp(mname, "issuperset") == 0 || strcmp(mname, "isdisjoint") == 0) {
            const char* ov = cg_expr(args[0], obj_ty);
            const char* r = cg_reg(); cg_emit("%s = call i1 @_%s_%s(ptr %s, ptr %s)", r, m, mname, sv, ov); return r;
        }
    }

    if (obj_ty->kind == TY_DICT) {
        const char* dv = cg_expr(obj_e, NULL);
        const char* m = obj_ty->mangled;
        Type* kt = obj_ty->key; Type* vt = obj_ty->val;
        const char* kmty = llvm_mem_ty(kt); const char* vmty = llvm_mem_ty(vt);
        if (strcmp(mname, "keys") == 0 || strcmp(mname, "values") == 0 || strcmp(mname, "items") == 0 || strcmp(mname, "copy") == 0) {
            const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_%s(ptr %s)", r, m, mname, dv); return r;
        }
        if (strcmp(mname, "get") == 0 || strcmp(mname, "setdefault") == 0) {
            const char* kv = cg_reg_to_mem(cg_coerce(cg_expr(args[0], kt), args[0]->ty, kt), kt);
            const char* vv = cg_reg_to_mem(cg_coerce(cg_expr(args[1], vt), args[1]->ty, vt), vt);
            const char* r = cg_reg();
            cg_emit("%s = call %s @_%s_%s(ptr %s, %s %s, %s %s)", r, vmty, m, mname, dv, kmty, kv, vmty, vv);
            return cg_mem_to_reg(r, vt);
        }
        if (strcmp(mname, "pop") == 0) {
            const char* kv = cg_reg_to_mem(cg_coerce(cg_expr(args[0], kt), args[0]->ty, kt), kt);
            const char* r = cg_reg(); cg_emit("%s = call %s @_%s_pop(ptr %s, %s %s)", r, vmty, m, dv, kmty, kv);
            return cg_mem_to_reg(r, vt);
        }
        if (strcmp(mname, "popitem") == 0) {
            const char* r = cg_reg();
            cg_emit("%s = call %s @_%s_popitem(ptr %s)", r, llvm_reg_ty(call->ty), m, dv);
            return r;
        }
        if (strcmp(mname, "update") == 0) {
            const char* ov = cg_expr(args[0], obj_ty);
            cg_emit("call void @_%s_update(ptr %s, ptr %s)", m, dv, ov); return "0";
        }
        if (strcmp(mname, "clear") == 0) {
            cg_emit("call void @_%s_clear(ptr %s)", m, dv); return "0";
        }
    }

    if (obj_ty->kind == TY_CLASS) {
        ClassInfo* cls = obj_ty->cls;
        /* super().method(...) with full cooperative C3 MRO dispatch */
        if (obj_e->kind == EXPR_CALL && obj_e->call.callee->kind == EXPR_VAR &&
            strcmp(obj_e->call.callee->var.name, "super") == 0) {
            ClassInfo* cur_cls = g_cg.cur_fn->owner_cls;
            if (obj_e->call.arg_count == 2 && obj_e->call.args[0]->kind == EXPR_VAR) {
                ClassInfo* sc = find_class_in_mod(g_cg.cur_mod, obj_e->call.args[0]->var.name);
                if (sc) cur_cls = sc;
            }
            ClassInfo* self_cls = g_cg.cur_fn->owner_cls;
            int idx = -1;
            for (int i = 0; i < self_cls->mro_count; i++) if (self_cls->mro[i] == cur_cls) { idx = i; break; }
            FuncInfo* def_m = NULL;
            for (int i = idx + 1; i < self_cls->mro_count; i++) {
                def_m = find_method_in_class_exact(self_cls->mro[i], mname);
                if (def_m) break;
            }
            VarSym* self_v = find_local_in_fn(g_cg.cur_fn, g_cg.cur_fn->param_names[0]);
            const char* self_ptr = cg_ensure_local_alloca(self_v->llvm_name, self_v->type);
            const char* self_reg = cg_reg();
            cg_emit("%s = load %s, ptr %s", self_reg, llvm_reg_ty(self_v->type), self_ptr);

            StrBuf ca; sb_init(&ca);
            sb_printf(&ca, "%s %s", llvm_reg_ty(def_m->param_types[0]), self_reg);
            for (int i = 0; i < argc; i++) {
                Type* pt = def_m->param_types[i + 1];
                const char* av = cg_coerce(cg_expr(args[i], pt), args[i]->ty, pt);
                sb_printf(&ca, ", %s %s", llvm_reg_ty(pt), av);
            }

            /* Check if any subclass of self_cls has a different next MRO method for cur_cls */
            FuncInfo* alt_fns[64];
            int alt_tids[64];
            int alt_cnt = 0;
            if (self_cls->needs_vtable && self_cls->has_subclasses) {
                for (int c = 0; c < g_all_class_count; c++) {
                    ClassInfo* sub = g_all_classes[c];
                    if (sub == self_cls || !class_is_subclass(sub, cur_cls)) continue;
                    int sidx = -1;
                    for (int m = 0; m < sub->mro_count; m++) if (sub->mro[m] == cur_cls) { sidx = m; break; }
                    FuncInfo* sm = NULL;
                    for (int m = sidx + 1; m < sub->mro_count; m++) {
                        sm = find_method_in_class_exact(sub->mro[m], mname);
                        if (sm) break;
                    }
                    if (sm && sm != def_m && alt_cnt < 64) {
                        alt_fns[alt_cnt] = sm;
                        alt_tids[alt_cnt] = sub->type_id;
                        alt_cnt++;
                    }
                }
            }

            const char* rty = llvm_reg_ty(def_m->ret_type);
            if (alt_cnt == 0) {
                if (def_m->ret_type->kind == TY_VOID) {
                    cg_emit("call void %s(%s)", def_m->llvm_name, ca.data);
                    free(ca.data); return "0";
                }
                const char* r = cg_reg();
                cg_emit("%s = call %s %s(%s)", r, rty, def_m->llvm_name, ca.data);
                free(ca.data); return r;
            }

            const char* vpp = cg_reg(); const char* vt = cg_reg(); const char* tid = cg_reg();
            cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", vpp, self_cls->llvm_name, self_reg);
            cg_emit("%s = load ptr, ptr %s", vt, vpp);
            cg_emit("%s = load i32, ptr %s", tid, vt);
            const char* l_def = cg_lbl("sup_def");
            const char* l_end = cg_lbl("sup_end");
            const char* res_p = (def_m->ret_type->kind != TY_VOID) ? cg_reg() : NULL;
            if (res_p) cg_entry("%s = alloca %s", res_p, llvm_mem_ty(def_m->ret_type));

            StrBuf sw; sb_init(&sw);
            sb_printf(&sw, "switch i32 %s, label %%%s [", tid, l_def);
            const char* lbls[64];
            for (int k = 0; k < alt_cnt; k++) {
                lbls[k] = cg_lbl("sup_c");
                sb_printf(&sw, " i32 %d, label %%%s", alt_tids[k], lbls[k]);
            }
            sb_append(&sw, " ]");
            cg_emit("%s", sw.data);
            free(sw.data);
            g_cg.block_terminated = true;

            for (int k = 0; k < alt_cnt; k++) {
                cg_label(lbls[k]);
                if (def_m->ret_type->kind == TY_VOID) {
                    cg_emit("call void %s(%s)", alt_fns[k]->llvm_name, ca.data);
                } else {
                    const char* cr = cg_reg();
                    cg_emit("%s = call %s %s(%s)", cr, rty, alt_fns[k]->llvm_name, ca.data);
                    cg_emit("store %s %s, ptr %s", llvm_mem_ty(def_m->ret_type), cg_reg_to_mem(cr, def_m->ret_type), res_p);
                }
                cg_br(l_end);
            }
            cg_label(l_def);
            if (def_m->ret_type->kind == TY_VOID) {
                cg_emit("call void %s(%s)", def_m->llvm_name, ca.data);
            } else {
                const char* cr = cg_reg();
                cg_emit("%s = call %s %s(%s)", cr, rty, def_m->llvm_name, ca.data);
                cg_emit("store %s %s, ptr %s", llvm_mem_ty(def_m->ret_type), cg_reg_to_mem(cr, def_m->ret_type), res_p);
            }
            cg_br(l_end);
            cg_label(l_end);
            free(ca.data);
            if (!res_p) return "0";
            const char* rm = cg_reg();
            cg_emit("%s = load %s, ptr %s", rm, llvm_mem_ty(def_m->ret_type), res_p);
            return cg_mem_to_reg(rm, def_m->ret_type);
        }

        FuncInfo* m = find_method_in_class(cls, mname);
        bool call_on_class_type = (obj_e->kind == EXPR_VAR && find_class_in_mod(g_cg.cur_mod, obj_e->var.name) != NULL);
        const char* self_val = call_on_class_type ? NULL : cg_expr(obj_e, NULL);
        StrBuf ca; sb_init(&ca);
        int start_p = 0;
        if (m->is_static) {
            start_p = 0;
        } else if (m->is_classmethod) {
            sb_printf(&ca, "ptr null");
            start_p = 1;
        } else {
            sb_printf(&ca, "%s %s", llvm_reg_ty(obj_ty), self_val);
            start_p = 1;
        }
        for (int i = start_p; i < m->param_count; i++) {
            Expr* ae = (i - start_p < argc) ? args[i - start_p] : m->param_defaults[i];
            Type* pt = m->param_types[i];
            const char* av = cg_coerce(cg_expr(ae, pt), ae->ty, pt);
            if (ca.len > 0) sb_append(&ca, ", ");
            sb_printf(&ca, "%s %s", llvm_reg_ty(pt), av);
        }

        const char* rty = llvm_reg_ty(m->ret_type);
        if (cls->needs_vtable && !m->is_static && !m->is_classmethod && !call_on_class_type && cls->has_subclasses) {
            /* Collect distinct subclass overrides for type_id switch */
            FuncInfo* ov_fns[64];
            int ov_tids[64];
            int ov_cnt = 0;
            for (int c = 0; c < g_all_class_count; c++) {
                ClassInfo* sub = g_all_classes[c];
                if (sub == cls || !class_is_subclass(sub, cls)) continue;
                FuncInfo* sm = find_method_in_class(sub, mname);
                if (sm && sm != m && ov_cnt < 64) {
                    ov_fns[ov_cnt] = sm;
                    ov_tids[ov_cnt] = sub->type_id;
                    ov_cnt++;
                }
            }
            if (ov_cnt > 0) {
                const char* vpp = cg_reg(); const char* vt = cg_reg(); const char* tid = cg_reg();
                cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", vpp, cls->llvm_name, self_val);
                cg_emit("%s = load ptr, ptr %s", vt, vpp);
                cg_emit("%s = load i32, ptr %s", tid, vt);
                const char* l_def = cg_lbl("v_def");
                const char* l_end = cg_lbl("v_end");
                const char* res_p = (m->ret_type->kind != TY_VOID) ? cg_reg() : NULL;
                if (res_p) cg_entry("%s = alloca %s", res_p, llvm_mem_ty(m->ret_type));

                StrBuf sw; sb_init(&sw);
                sb_printf(&sw, "switch i32 %s, label %%%s [", tid, l_def);
                const char* lbls[64];
                for (int k = 0; k < ov_cnt; k++) {
                    lbls[k] = cg_lbl("v_c");
                    sb_printf(&sw, " i32 %d, label %%%s", ov_tids[k], lbls[k]);
                }
                sb_append(&sw, " ]");
                cg_emit("%s", sw.data);
                free(sw.data);
                g_cg.block_terminated = true;

                for (int k = 0; k < ov_cnt; k++) {
                    cg_label(lbls[k]);
                    if (m->ret_type->kind == TY_VOID) {
                        cg_emit("call void %s(%s)", ov_fns[k]->llvm_name, ca.data);
                    } else {
                        const char* cr = cg_reg();
                        cg_emit("%s = call %s %s(%s)", cr, rty, ov_fns[k]->llvm_name, ca.data);
                        cg_emit("store %s %s, ptr %s", llvm_mem_ty(m->ret_type), cg_reg_to_mem(cr, m->ret_type), res_p);
                    }
                    cg_br(l_end);
                }
                cg_label(l_def);
                if (m->ret_type->kind == TY_VOID) {
                    cg_emit("call void %s(%s)", m->llvm_name, ca.data);
                } else {
                    const char* cr = cg_reg();
                    cg_emit("%s = call %s %s(%s)", cr, rty, m->llvm_name, ca.data);
                    cg_emit("store %s %s, ptr %s", llvm_mem_ty(m->ret_type), cg_reg_to_mem(cr, m->ret_type), res_p);
                }
                cg_br(l_end);
                cg_label(l_end);
                free(ca.data);
                if (!res_p) return "0";
                const char* rm = cg_reg();
                cg_emit("%s = load %s, ptr %s", rm, llvm_mem_ty(m->ret_type), res_p);
                return cg_mem_to_reg(rm, m->ret_type);
            }
        }
        if (m->ret_type->kind == TY_VOID) {
            cg_emit("call void %s(%s)", m->llvm_name, ca.data);
            free(ca.data); return "0";
        }
        const char* r = cg_reg();
        cg_emit("%s = call %s %s(%s)", r, rty, m->llvm_name, ca.data);
        free(ca.data); return r;
    }

    if (obj_ty->kind == TY_MODULE) {
        ModuleInfo* mmod = obj_ty->mod;
        FuncInfo* mf = find_func_in_mod(mmod, mname);
        if (mf) {
            StrBuf ca; sb_init(&ca);
            for (int i = 0; i < mf->param_count; i++) {
                Expr* ae = (i < argc) ? args[i] : mf->param_defaults[i];
                Type* pt = mf->param_types[i];
                const char* av = cg_coerce(cg_expr(ae, pt), ae->ty, pt);
                if (i > 0) sb_append(&ca, ", ");
                sb_printf(&ca, "%s %s", llvm_reg_ty(pt), av);
            }
            if (mf->ret_type->kind == TY_VOID) {
                cg_emit("call void %s(%s)", mf->llvm_name, ca.data);
                free(ca.data); return "0";
            }
            const char* r = cg_reg();
            cg_emit("%s = call %s %s(%s)", r, llvm_reg_ty(mf->ret_type), mf->llvm_name, ca.data);
            free(ca.data); return r;
        }
        ClassInfo* mci = find_class_in_mod(mmod, mname);
        if (mci) return cg_construct_class(mci, args, argc);
    }

    return "0";
}

static const char* cg_expr(Expr* e, Type* hint) {
    (void)hint;
    if (!e) return "0";
    Type* ty = e->ty ? e->ty : g_ty_i64;

    switch (e->kind) {
        case EXPR_INT: {
            if (ty->kind == TY_BIGINT) {
                const char* r = cg_reg();
                cg_emit("%s = call ptr @stricta_bigint_from_str(ptr %s)", r, cg_cstr_lit(e->int_lit.raw, strlen(e->int_lit.raw)));
                return r;
            }
            if (type_is_float(ty)) {
                return arena_sprintf("%lld.0", (long long)e->int_lit.ival);
            }
            return arena_sprintf("%lld", (long long)e->int_lit.ival);
        }
        case EXPR_FLOAT: {
            char buf[64];
            snprintf(buf, sizeof(buf), "%.17g", e->float_lit.fval);
            if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E')) strcat(buf, ".0");
            if (ty->kind == TY_F32) {
                const char* r = cg_reg();
                cg_emit("%s = fptrunc double %s to float", r, buf);
                return r;
            }
            return arena_strdup(buf);
        }
        case EXPR_BOOL:
            return e->bool_lit.bval ? "true" : "false";
        case EXPR_NONE:
            return llvm_zero(ty);
        case EXPR_STR:
            return cg_strictastr_lit(e->str_lit.text, e->str_lit.len);
        case EXPR_BYTES: {
            const char* cs = cg_cstr_lit(e->str_lit.text, e->str_lit.len);
            const char* r = cg_reg();
            cg_emit("%s = call ptr @stricta_bytes_new(ptr %s, i32 %zu)", r, cs, e->str_lit.len);
            return r;
        }
        case EXPR_FSTRING: {
            const char* acc = cg_strictastr_lit("", 0);
            for (int i = 0; i < e->fstr.count; i++) {
                FStrPart* fp = &e->fstr.parts[i];
                const char* ps = NULL;
                if (!fp->is_expr) {
                    ps = cg_strictastr_lit(fp->text, strlen(fp->text));
                } else {
                    const char* ev = cg_expr(fp->expr, NULL);
                    if (fp->fmt_spec && strchr(fp->fmt_spec, 'f')) {
                        const char* dot = strchr(fp->fmt_spec, '.');
                        int prec = dot ? atoi(dot + 1) : 6;
                        ps = cg_reg();
                        cg_emit("%s = call ptr @stricta_float_fmt(double %s, i32 %d)", ps, cg_coerce(ev, fp->expr->ty, g_ty_f64), prec);
                    } else {
                        ps = cg_to_str(ev, fp->expr->ty);
                    }
                }
                const char* n = cg_reg();
                cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, ps);
                acc = n;
            }
            return acc;
        }
        case EXPR_VAR: {
            const char* n = e->var.name;
            if (strcmp(n, "self") == 0 || (strcmp(n, "cls") == 0 && g_cg.cur_fn && g_cg.cur_fn->is_classmethod)) {
                VarSym* sv = find_local_in_fn(g_cg.cur_fn, g_cg.cur_fn->param_names[0]);
                const char* sp = cg_ensure_local_alloca(sv->llvm_name, sv->type);
                const char* r = cg_reg();
                cg_emit("%s = load %s, ptr %s", r, llvm_reg_ty(sv->type), sp);
                return r;
            }
            Type* vt = NULL;
            const char* vp = cg_var_ptr(n, &vt);
            if (vp) {
                const char* mv = cg_reg();
                cg_emit("%s = load %s, ptr %s", mv, llvm_mem_ty(vt), vp);
                return cg_mem_to_reg(mv, vt);
            }
            FuncInfo* fi = find_func_in_mod(g_cg.cur_mod, n);
            if (fi) return fi->llvm_name;
            return "null";
        }
        case EXPR_WALRUS: {
            const char* v = cg_expr(e->walrus.val, ty);
            Type* vt = NULL;
            const char* vp = cg_var_ptr(e->walrus.name, &vt);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(cg_coerce(v, e->walrus.val->ty, vt), vt), vp);
            return v;
        }
        case EXPR_UNOP: {
            const char* op = e->unop.op;
            const char* ov = cg_expr(e->unop.operand, ty);
            Type* ot = e->unop.operand->ty;
            if (strcmp(op, "not") == 0) {
                const char* b = cg_truthy(ov, ot);
                const char* r = cg_reg();
                cg_emit("%s = xor i1 %s, true", r, b);
                return r;
            }
            if (strcmp(op, "+") == 0) return cg_coerce(ov, ot, ty);
            if (strcmp(op, "-") == 0) {
                if (ty->kind == TY_BIGINT) {
                    const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_bigint_neg(ptr %s)", r, ov); return r;
                }
                const char* cv = cg_coerce(ov, ot, ty);
                const char* r = cg_reg();
                if (type_is_float(ty)) cg_emit("%s = fneg %s %s", r, llvm_reg_ty(ty), cv);
                else cg_emit("%s = sub %s 0, %s", r, llvm_reg_ty(ty), cv);
                return r;
            }
            if (strcmp(op, "~") == 0) {
                const char* r = cg_reg();
                cg_emit("%s = xor %s %s, -1", r, llvm_reg_ty(ty), ov);
                return r;
            }
            return ov;
        }
        case EXPR_BINOP: {
            const char* op = e->binop.op;
            if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
                bool is_and = (strcmp(op, "and") == 0);
                const char* lv = cg_expr(e->binop.left, NULL);
                const char* lb = cg_truthy(lv, e->binop.left->ty);
                const char* l_rhs = cg_lbl("sc_rhs");
                const char* l_short = cg_lbl("sc_short");
                const char* l_end = cg_lbl("sc_end");
                if (is_and) cg_cond_br(lb, l_rhs, l_short);
                else cg_cond_br(lb, l_short, l_rhs);

                cg_label(l_short);
                const char* sv = (ty->kind == TY_BOOL) ? (is_and ? "false" : "true") : cg_coerce(lv, e->binop.left->ty, ty);
                cg_br(l_end);

                cg_label(l_rhs);
                const char* rv = cg_expr(e->binop.right, ty);
                const char* rb = (ty->kind == TY_BOOL) ? cg_truthy(rv, e->binop.right->ty) : cg_coerce(rv, e->binop.right->ty, ty);
                const char* rhs_end = cg_lbl("sc_rend");
                cg_br(rhs_end);
                cg_label(rhs_end);
                cg_br(l_end);

                cg_label(l_end);
                const char* res = cg_reg();
                cg_emit("%s = phi %s [ %s, %%%s ], [ %s, %%%s ]", res, llvm_reg_ty(ty), sv, l_short, rb, rhs_end);
                return res;
            }
            const char* lv = cg_expr(e->binop.left, NULL);
            const char* rv = cg_expr(e->binop.right, e->binop.left->ty);
            return cg_binop_values(op, lv, e->binop.left->ty, rv, e->binop.right->ty, ty);
        }
        case EXPR_TERNARY: {
            const char* cond = cg_truthy(cg_expr(e->ternary.cond, g_ty_bool), e->ternary.cond->ty);
            const char* l_t = cg_lbl("tern_t");
            const char* l_f = cg_lbl("tern_f");
            const char* l_end = cg_lbl("tern_e");
            cg_cond_br(cond, l_t, l_f);
            cg_label(l_t);
            const char* tv = cg_coerce(cg_expr(e->ternary.then_e, ty), e->ternary.then_e->ty, ty);
            const char* l_te = cg_lbl("tern_te"); cg_br(l_te); cg_label(l_te); cg_br(l_end);
            cg_label(l_f);
            const char* fv = cg_coerce(cg_expr(e->ternary.else_e, ty), e->ternary.else_e->ty, ty);
            const char* l_fe = cg_lbl("tern_fe"); cg_br(l_fe); cg_label(l_fe); cg_br(l_end);
            cg_label(l_end);
            const char* r = cg_reg();
            cg_emit("%s = phi %s [ %s, %%%s ], [ %s, %%%s ]", r, llvm_reg_ty(ty), tv, l_te, fv, l_fe);
            return r;
        }
        case EXPR_LIST: {
            const char* l = cg_reg();
            int cap = e->seq.count > 4 ? e->seq.count : 4;
            cg_emit("%s = call ptr @_%s_new(i32 %d)", l, ty->mangled, cap);
            for (int i = 0; i < e->seq.count; i++) {
                const char* iv = cg_coerce(cg_expr(e->seq.items[i], ty->elem), e->seq.items[i]->ty, ty->elem);
                cg_emit("call void @_%s_append(ptr %s, %s %s)", ty->mangled, l, llvm_mem_ty(ty->elem), cg_reg_to_mem(iv, ty->elem));
            }
            return l;
        }
        case EXPR_TUPLE: {
            const char* t = "zeroinitializer";
            for (int i = 0; i < e->seq.count; i++) {
                const char* iv = cg_coerce(cg_expr(e->seq.items[i], ty->tuple_elems[i]), e->seq.items[i]->ty, ty->tuple_elems[i]);
                const char* nt = cg_reg();
                cg_emit("%s = insertvalue %s %s, %s %s, %d", nt, llvm_reg_ty(ty), t,
                        llvm_mem_ty(ty->tuple_elems[i]), cg_reg_to_mem(iv, ty->tuple_elems[i]), i);
                t = nt;
            }
            return t;
        }
        case EXPR_SET: {
            const char* s = cg_reg();
            int cap = e->seq.count > 8 ? e->seq.count : 8;
            cg_emit("%s = call ptr @_%s_new(i32 %d)", s, ty->mangled, cap);
            for (int i = 0; i < e->seq.count; i++) {
                const char* iv = cg_coerce(cg_expr(e->seq.items[i], ty->elem), e->seq.items[i]->ty, ty->elem);
                cg_emit("call void @_%s_add(ptr %s, %s %s)", ty->mangled, s, llvm_mem_ty(ty->elem), cg_reg_to_mem(iv, ty->elem));
            }
            return s;
        }
        case EXPR_DICT: {
            const char* d = cg_reg();
            int cap = e->dict.count > 8 ? e->dict.count : 8;
            cg_emit("%s = call ptr @_%s_new(i32 %d)", d, ty->mangled, cap);
            for (int i = 0; i < e->dict.count; i++) {
                const char* kv = cg_coerce(cg_expr(e->dict.keys[i], ty->key), e->dict.keys[i]->ty, ty->key);
                const char* vv = cg_coerce(cg_expr(e->dict.vals[i], ty->val), e->dict.vals[i]->ty, ty->val);
                cg_emit("call void @_%s_set(ptr %s, %s %s, %s %s)", ty->mangled, d,
                        llvm_mem_ty(ty->key), cg_reg_to_mem(kv, ty->key),
                        llvm_mem_ty(ty->val), cg_reg_to_mem(vv, ty->val));
            }
            return d;
        }
        case EXPR_LISTCOMP:
        case EXPR_GENEXPR: {
            const char* l = cg_reg();
            cg_emit("%s = call ptr @_%s_new(i32 16)", l, ty->mangled);
            CompLeafCtx ctx = {e, l, ty};
            cg_comp_loop_emit(e->comp.gens, 0, e->comp.gen_count, comp_leaf_list, &ctx);
            return l;
        }
        case EXPR_SETCOMP: {
            const char* s = cg_reg();
            cg_emit("%s = call ptr @_%s_new(i32 16)", s, ty->mangled);
            CompLeafCtx ctx = {e, s, ty};
            cg_comp_loop_emit(e->comp.gens, 0, e->comp.gen_count, comp_leaf_set, &ctx);
            return s;
        }
        case EXPR_DICTCOMP: {
            const char* d = cg_reg();
            cg_emit("%s = call ptr @_%s_new(i32 16)", d, ty->mangled);
            CompLeafCtx ctx = {e, d, ty};
            cg_comp_loop_emit(e->comp.gens, 0, e->comp.gen_count, comp_leaf_dict, &ctx);
            return d;
        }
        case EXPR_INDEX: {
            const char* ov = cg_expr(e->index.obj, NULL);
            Type* ot = e->index.obj->ty;
            if (ot->kind == TY_TUPLE) {
                int idx = (int)e->index.index->int_lit.ival;
                if (idx < 0) idx += ot->tuple_count;
                const char* mv = cg_reg();
                cg_emit("%s = extractvalue %s %s, %d", mv, llvm_reg_ty(ot), ov, idx);
                return cg_mem_to_reg(mv, ty);
            }
            const char* iv = cg_expr(e->index.index, ot->kind == TY_DICT ? ot->key : g_ty_i64);
            if (ot->kind == TY_LIST) {
                const char* i64 = cg_coerce(iv, e->index.index->ty, g_ty_i64);
                const char* mv = cg_reg();
                cg_emit("%s = call %s @_%s_get(ptr %s, i64 %s)", mv, llvm_mem_ty(ot->elem), ot->mangled, ov, i64);
                return cg_mem_to_reg(mv, ot->elem);
            }
            if (ot->kind == TY_DICT) {
                const char* km = cg_reg_to_mem(cg_coerce(iv, e->index.index->ty, ot->key), ot->key);
                const char* mv = cg_reg();
                cg_emit("%s = call %s @_%s_getitem(ptr %s, %s %s)", mv, llvm_mem_ty(ot->val), ot->mangled, ov, llvm_mem_ty(ot->key), km);
                return cg_mem_to_reg(mv, ot->val);
            }
            if (ot->kind == TY_STR) {
                const char* i64 = cg_coerce(iv, e->index.index->ty, g_ty_i64);
                const char* r = cg_reg();
                cg_emit("%s = call ptr @stricta_str_char_at(ptr %s, i64 %s)", r, ov, i64);
                return r;
            }
            if (ot->kind == TY_BYTES) {
                const char* dp = cg_reg(); const char* data = cg_reg(); const char* bp = cg_reg();
                const char* b = cg_reg(); const char* r = cg_reg();
                cg_emit("%s = getelementptr %%StrictaBytes, ptr %s, i32 0, i32 2", dp, ov);
                cg_emit("%s = load ptr, ptr %s", data, dp);
                cg_emit("%s = getelementptr i8, ptr %s, i64 %s", bp, data, cg_coerce(iv, e->index.index->ty, g_ty_i64));
                cg_emit("%s = load i8, ptr %s", b, bp);
                cg_emit("%s = zext i8 %s to i64", r, b);
                return r;
            }
            return "0";
        }
        case EXPR_SLICE: {
            const char* ov = cg_expr(e->slice.obj, NULL);
            Type* ot = e->slice.obj->ty;
            const char* st = e->slice.start ? cg_coerce(cg_expr(e->slice.start, g_ty_i64), e->slice.start->ty, g_ty_i64) : "0";
            const char* sp = e->slice.stop ? cg_coerce(cg_expr(e->slice.stop, g_ty_i64), e->slice.stop->ty, g_ty_i64) : "9223372036854775807";
            const char* r = cg_reg();
            if (ot->kind == TY_STR) cg_emit("%s = call ptr @stricta_str_slice(ptr %s, i64 %s, i64 %s)", r, ov, st, sp);
            else cg_emit("%s = call ptr @_%s_slice(ptr %s, i64 %s, i64 %s)", r, ot->mangled, ov, st, sp);
            return r;
        }
        case EXPR_ATTR: {
            const char* an = e->attr.attr;
            if (e->attr.obj->ty->kind == TY_MODULE) {
                ModuleInfo* mm = e->attr.obj->ty->mod;
                if (strcmp(mm->name, "sys") == 0 && strcmp(an, "argv") == 0) {
                    const char* r = cg_reg();
                    cg_emit("%s = load ptr, ptr @stricta_sys_argv", r);
                    return r;
                }
                VarSym* gv = find_global_in_mod(mm, an);
                if (gv) {
                    const char* mv = cg_reg();
                    cg_emit("%s = load %s, ptr %s", mv, llvm_mem_ty(gv->type), gv->llvm_name);
                    return cg_mem_to_reg(mv, gv->type);
                }
            }
            Type* ot = e->attr.obj->ty;
            if (ot->kind == TY_CLASS) {
                ClassInfo* cls = ot->cls;
                FieldInfo* fi = find_field_in_class(cls, an);
                if (fi) {
                    const char* ov = cg_expr(e->attr.obj, NULL);
                    if (cls->is_value_type) {
                        const char* mv = cg_reg();
                        cg_emit("%s = extractvalue %s %s, %d", mv, cls->llvm_name, ov, fi->slot_index);
                        return cg_mem_to_reg(mv, fi->type);
                    }
                    const char* fp = cg_reg(); const char* mv = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, cls->llvm_name, ov, fi->slot_index);
                    cg_emit("%s = load %s, ptr %s", mv, llvm_mem_ty(fi->type), fp);
                    return cg_mem_to_reg(mv, fi->type);
                }
                FieldInfo* ca = find_class_attr(cls, an);
                if (ca) {
                    ClassInfo* owner = cls;
                    for (int m = 0; m < cls->mro_count; m++) {
                        for (int k = 0; k < cls->mro[m]->class_attr_count; k++) {
                            if (strcmp(cls->mro[m]->class_attrs[k].name, an) == 0) { owner = cls->mro[m]; goto found_ca; }
                        }
                    }
                    found_ca:;
                    const char* mv = cg_reg();
                    cg_emit("%s = load %s, ptr @_cattr_%s%s__%s", mv, llvm_mem_ty(ca->type), owner->mod->prefix, owner->name, an);
                    return cg_mem_to_reg(mv, ca->type);
                }
                FuncInfo* prop = find_method_in_class(cls, an);
                if (prop && prop->is_property) {
                    const char* ov = cg_expr(e->attr.obj, NULL);
                    const char* r = cg_reg();
                    cg_emit("%s = call %s %s(%s %s)", r, llvm_reg_ty(prop->ret_type), prop->llvm_name, llvm_reg_ty(ot), ov);
                    return r;
                }
            }
            return "0";
        }
        case EXPR_LAMBDA: {
            for (int i = 0; i < g_all_func_count; i++) {
                if (g_all_funcs[i]->lambda_ast == e) return g_all_funcs[i]->llvm_name;
            }
            return "null";
        }
        case EXPR_CALL: {
            Expr* callee = e->call.callee;
            int argc = e->call.arg_count;
            Expr** args = e->call.args;

            if (callee->kind == EXPR_ATTR) {
                return cg_method_call(e, callee->attr.obj, callee->attr.attr, callee->attr.obj->ty);
            }
            if (callee->kind == EXPR_VAR) {
                const char* cname = callee->var.name;
                if (strcmp(cname, "print") == 0) {
                    for (int i = 0; i < argc; i++) {
                        if (i > 0) cg_emit("call void @stricta_print_space()");
                        const char* av = cg_expr(args[i], NULL);
                        cg_print_val(av, args[i]->ty);
                    }
                    cg_emit("call void @stricta_print_newline()");
                    return "0";
                }
                if (strcmp(cname, "len") == 0) {
                    const char* av = cg_expr(args[0], NULL);
                    Type* at = args[0]->ty;
                    if (at->kind == TY_TUPLE) return arena_sprintf("%d", at->tuple_count);
                    const char* lp = cg_reg(); const char* l32 = cg_reg(); const char* r = cg_reg();
                    const char* sty = (at->kind == TY_STR) ? "%StrictaStr" : (at->kind == TY_BYTES) ? "%StrictaBytes" : arena_sprintf("%%%s", at->mangled);
                    int fld = (at->kind == TY_SET) ? 2 : (at->kind == TY_DICT) ? 3 : 0;
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", lp, sty, av, fld);
                    cg_emit("%s = load i32, ptr %s", l32, lp);
                    cg_emit("%s = sext i32 %s to i64", r, l32);
                    return r;
                }
                if (strcmp(cname, "str") == 0 || strcmp(cname, "repr") == 0) {
                    if (args[0]->ty->kind == TY_CLASS) {
                        FuncInfo* m = find_method_in_class(args[0]->ty->cls, strcmp(cname, "repr") == 0 ? "__repr__" : "__str__");
                        if (m) {
                            const char* av = cg_expr(args[0], NULL);
                            const char* r = cg_reg();
                            cg_emit("%s = call ptr %s(%s %s)", r, m->llvm_name, llvm_reg_ty(args[0]->ty), av);
                            return r;
                        }
                    }
                    return cg_to_str(cg_expr(args[0], NULL), args[0]->ty);
                }
                if (strcmp(cname, "int") == 0 || strcmp(cname, "i64") == 0 || strcmp(cname, "i32") == 0 ||
                    strcmp(cname, "i16") == 0 || strcmp(cname, "i8") == 0 || strcmp(cname, "u64") == 0 ||
                    strcmp(cname, "u32") == 0 || strcmp(cname, "u16") == 0 || strcmp(cname, "u8") == 0) {
                    const char* av = cg_expr(args[0], NULL);
                    if (args[0]->ty->kind == TY_STR) {
                        const char* r = cg_reg(); cg_emit("%s = call i64 @stricta_str_to_int(ptr %s)", r, av);
                        return cg_coerce(r, g_ty_i64, ty);
                    }
                    return cg_coerce(av, args[0]->ty, ty);
                }
                if (strcmp(cname, "float") == 0 || strcmp(cname, "f64") == 0 || strcmp(cname, "f32") == 0) {
                    const char* av = cg_expr(args[0], NULL);
                    if (args[0]->ty->kind == TY_STR) {
                        const char* r = cg_reg(); cg_emit("%s = call double @stricta_str_to_float(ptr %s)", r, av);
                        return cg_coerce(r, g_ty_f64, ty);
                    }
                    return cg_coerce(av, args[0]->ty, ty);
                }
                if (strcmp(cname, "bool") == 0) {
                    return cg_truthy(cg_expr(args[0], NULL), args[0]->ty);
                }
                if (strcmp(cname, "bigint") == 0) {
                    const char* av = cg_expr(args[0], NULL);
                    if (args[0]->ty->kind == TY_BIGINT) return av;
                    const char* r = cg_reg();
                    if (args[0]->ty->kind == TY_STR) {
                        const char* sp = cg_reg(); const char* chars = cg_reg();
                        cg_emit("%s = getelementptr %%StrictaStr, ptr %s, i32 0, i32 2", sp, av);
                        cg_emit("%s = load ptr, ptr %s", chars, sp);
                        cg_emit("%s = call ptr @stricta_bigint_from_str(ptr %s)", r, chars);
                    } else {
                        cg_emit("%s = call ptr @stricta_bigint_from_i64(i64 %s)", r, cg_coerce(av, args[0]->ty, g_ty_i64));
                    }
                    return r;
                }
                if (strcmp(cname, "ord") == 0) {
                    const char* r = cg_reg(); cg_emit("%s = call i64 @stricta_ord(ptr %s)", r, cg_expr(args[0], g_ty_str)); return r;
                }
                if (strcmp(cname, "chr") == 0) {
                    const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_chr(i64 %s)", r, cg_coerce(cg_expr(args[0], g_ty_i64), args[0]->ty, g_ty_i64)); return r;
                }
                if (strcmp(cname, "hex") == 0 || strcmp(cname, "bin") == 0 || strcmp(cname, "oct") == 0) {
                    const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_%s(i64 %s)", r, cname, cg_coerce(cg_expr(args[0], g_ty_i64), args[0]->ty, g_ty_i64)); return r;
                }
                if (strcmp(cname, "abs") == 0) {
                    const char* av = cg_expr(args[0], ty);
                    if (ty->kind == TY_BIGINT) { const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_bigint_abs(ptr %s)", r, av); return r; }
                    if (type_is_float(ty)) {
                        const char* r = cg_reg();
                        cg_emit("%s = call %s @llvm.fabs.%s(%s %s)", r, llvm_reg_ty(ty), ty->kind == TY_F32 ? "f32" : "f64", llvm_reg_ty(ty), av);
                        return r;
                    }
                    const char* r = cg_reg();
                    cg_emit("%s = call %s @llvm.abs.%s(%s %s, i1 false)", r, llvm_reg_ty(ty), llvm_reg_ty(ty), llvm_reg_ty(ty), av);
                    return r;
                }
                if (strcmp(cname, "round") == 0) {
                    const char* dv = cg_coerce(cg_expr(args[0], g_ty_f64), args[0]->ty, g_ty_f64);
                    const char* rnd = cg_reg(); const char* r = cg_reg();
                    cg_emit("%s = call double @llvm.round.f64(double %s)", rnd, dv);
                    cg_emit("%s = fptosi double %s to i64", r, rnd);
                    return r;
                }
                if (strcmp(cname, "pow") == 0) {
                    return cg_binop_values("**", cg_expr(args[0], ty), args[0]->ty, cg_expr(args[1], ty), args[1]->ty, ty);
                }
                if (strcmp(cname, "sum") == 0) {
                    /* Fused zero-allocation sum(genexpr / listcomp) */
                    if (args[0]->kind == EXPR_GENEXPR || args[0]->kind == EXPR_LISTCOMP) {
                        const char* acc_p = cg_reg();
                        cg_entry("%s = alloca %s", acc_p, llvm_reg_ty(ty));
                        cg_emit("store %s %s, ptr %s", llvm_reg_ty(ty), llvm_zero(ty), acc_p);
                        CompLeafCtx ctx = {args[0], acc_p, ty};
                        cg_comp_loop_emit(args[0]->comp.gens, 0, args[0]->comp.gen_count, comp_leaf_sum, &ctx);
                        const char* r = cg_reg();
                        cg_emit("%s = load %s, ptr %s", r, llvm_reg_ty(ty), acc_p);
                        return r;
                    }
                    const char* lv = cg_expr(args[0], NULL);
                    const char* sty = arena_sprintf("%%%s", args[0]->ty->mangled);
                    const char* mty = llvm_mem_ty(ty);
                    const char* lp = cg_reg(); const char* len = cg_reg();
                    const char* ip = cg_reg(); const char* arr = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", lp, sty, lv);
                    cg_emit("%s = load i32, ptr %s", len, lp);
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", ip, sty, lv);
                    cg_emit("%s = load ptr, ptr %s", arr, ip);
                    const char* l_loop = cg_lbl("sum_l"); const char* l_body = cg_lbl("sum_b"); const char* l_done = cg_lbl("sum_d");
                    const char* acc_p = cg_reg(); const char* i_p = cg_reg();
                    cg_entry("%s = alloca %s", acc_p, llvm_reg_ty(ty)); cg_entry("%s = alloca i32", i_p);
                    cg_emit("store %s %s, ptr %s", llvm_reg_ty(ty), llvm_zero(ty), acc_p);
                    cg_emit("store i32 0, ptr %s", i_p);
                    cg_br(l_loop);
                    cg_label(l_loop);
                    const char* iv = cg_reg(); const char* c = cg_reg();
                    cg_emit("%s = load i32, ptr %s", iv, i_p);
                    cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
                    cg_cond_br(c, l_body, l_done);
                    cg_label(l_body);
                    const char* ep = cg_reg(); const char* ev = cg_reg(); const char* cur = cg_reg(); const char* nxt = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, mty, arr, iv);
                    cg_emit("%s = load %s, ptr %s", ev, mty, ep);
                    cg_emit("%s = load %s, ptr %s", cur, llvm_reg_ty(ty), acc_p);
                    if (type_is_float(ty)) cg_emit("%s = fadd %s %s, %s", nxt, llvm_reg_ty(ty), cur, ev);
                    else cg_emit("%s = add %s %s, %s", nxt, llvm_reg_ty(ty), cur, ev);
                    cg_emit("store %s %s, ptr %s", llvm_reg_ty(ty), nxt, acc_p);
                    const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, iv); cg_emit("store i32 %s, ptr %s", in, i_p);
                    cg_br(l_loop);
                    cg_label(l_done);
                    const char* res = cg_reg(); cg_emit("%s = load %s, ptr %s", res, llvm_reg_ty(ty), acc_p);
                    return res;
                }
                if (strcmp(cname, "min") == 0 || strcmp(cname, "max") == 0) {
                    bool is_min = (strcmp(cname, "min") == 0);
                    if (argc >= 2) {
                        const char* best = cg_coerce(cg_expr(args[0], ty), args[0]->ty, ty);
                        for (int i = 1; i < argc; i++) {
                            const char* v = cg_coerce(cg_expr(args[i], ty), args[i]->ty, ty);
                            const char* c = cg_binop_values(is_min ? "<" : ">", v, ty, best, ty, g_ty_bool);
                            const char* nb = cg_reg();
                            cg_emit("%s = select i1 %s, %s %s, %s %s", nb, c, llvm_reg_ty(ty), v, llvm_reg_ty(ty), best);
                            best = nb;
                        }
                        return best;
                    }
                    const char* lv = cg_expr(args[0], NULL);
                    const char* sty = arena_sprintf("%%%s", args[0]->ty->mangled);
                    const char* mty = llvm_mem_ty(ty);
                    const char* lp = cg_reg(); const char* len = cg_reg();
                    const char* ip = cg_reg(); const char* arr = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", lp, sty, lv);
                    cg_emit("%s = load i32, ptr %s", len, lp);
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", ip, sty, lv);
                    cg_emit("%s = load ptr, ptr %s", arr, ip);
                    const char* best_p = cg_reg(); const char* i_p = cg_reg();
                    cg_entry("%s = alloca %s", best_p, mty); cg_entry("%s = alloca i32", i_p);
                    const char* f0 = cg_reg(); cg_emit("%s = load %s, ptr %s", f0, mty, arr);
                    cg_emit("store %s %s, ptr %s", mty, f0, best_p);
                    cg_emit("store i32 1, ptr %s", i_p);
                    const char* l_loop = cg_lbl("mm_l"); const char* l_body = cg_lbl("mm_b"); const char* l_done = cg_lbl("mm_d");
                    cg_br(l_loop);
                    cg_label(l_loop);
                    const char* iv = cg_reg(); const char* lc = cg_reg();
                    cg_emit("%s = load i32, ptr %s", iv, i_p);
                    cg_emit("%s = icmp slt i32 %s, %s", lc, iv, len);
                    cg_cond_br(lc, l_body, l_done);
                    cg_label(l_body);
                    const char* ep = cg_reg(); const char* ev = cg_reg(); const char* cur = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, mty, arr, iv);
                    cg_emit("%s = load %s, ptr %s", ev, mty, ep);
                    cg_emit("%s = load %s, ptr %s", cur, mty, best_p);
                    const char* cmp = cg_binop_values(is_min ? "<" : ">", cg_mem_to_reg(ev, ty), ty, cg_mem_to_reg(cur, ty), ty, g_ty_bool);
                    const char* nb = cg_reg();
                    cg_emit("%s = select i1 %s, %s %s, %s %s", nb, cmp, mty, ev, mty, cur);
                    cg_emit("store %s %s, ptr %s", mty, nb, best_p);
                    const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, iv); cg_emit("store i32 %s, ptr %s", in, i_p);
                    cg_br(l_loop);
                    cg_label(l_done);
                    const char* res = cg_reg(); cg_emit("%s = load %s, ptr %s", res, mty, best_p);
                    return cg_mem_to_reg(res, ty);
                }
                if (strcmp(cname, "all") == 0 || strcmp(cname, "any") == 0) {
                    bool is_all = (strcmp(cname, "all") == 0);
                    const char* lv = cg_expr(args[0], NULL);
                    Type* at = args[0]->ty;
                    const char* sty = arena_sprintf("%%%s", at->mangled);
                    const char* mty = llvm_mem_ty(at->elem);
                    const char* lp = cg_reg(); const char* len = cg_reg();
                    const char* ip = cg_reg(); const char* arr = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", lp, sty, lv);
                    cg_emit("%s = load i32, ptr %s", len, lp);
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", ip, sty, lv);
                    cg_emit("%s = load ptr, ptr %s", arr, ip);
                    const char* res_p = cg_reg(); const char* i_p = cg_reg();
                    cg_entry("%s = alloca i1", res_p); cg_entry("%s = alloca i32", i_p);
                    cg_emit("store i1 %s, ptr %s", is_all ? "true" : "false", res_p);
                    cg_emit("store i32 0, ptr %s", i_p);
                    const char* l_l = cg_lbl("aa_l"); const char* l_b = cg_lbl("aa_b"); const char* l_d = cg_lbl("aa_d");
                    cg_br(l_l); cg_label(l_l);
                    const char* iv = cg_reg(); const char* c = cg_reg();
                    cg_emit("%s = load i32, ptr %s", iv, i_p);
                    cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
                    cg_cond_br(c, l_b, l_d);
                    cg_label(l_b);
                    const char* ep = cg_reg(); const char* ev = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, mty, arr, iv);
                    cg_emit("%s = load %s, ptr %s", ev, mty, ep);
                    const char* b = cg_truthy(cg_mem_to_reg(ev, at->elem), at->elem);
                    const char* l_next = cg_lbl("aa_n"); const char* l_hit = cg_lbl("aa_h");
                    if (is_all) cg_cond_br(b, l_next, l_hit); else cg_cond_br(b, l_hit, l_next);
                    cg_label(l_hit);
                    cg_emit("store i1 %s, ptr %s", is_all ? "false" : "true", res_p);
                    cg_br(l_d);
                    cg_label(l_next);
                    const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, iv); cg_emit("store i32 %s, ptr %s", in, i_p);
                    cg_br(l_l);
                    cg_label(l_d);
                    const char* res = cg_reg(); cg_emit("%s = load i1, ptr %s", res, res_p); return res;
                }
                if (strcmp(cname, "sorted") == 0 || strcmp(cname, "reversed") == 0) {
                    const char* av = cg_expr(args[0], NULL);
                    Type* at = args[0]->ty;
                    const char* res = cg_reg();
                    if (at->kind == TY_LIST) {
                        cg_emit("%s = call ptr @_%s_copy(ptr %s)", res, at->mangled, av);
                    } else {
                        cg_emit("%s = call ptr @_%s_new(i32 8)", res, ty->mangled);
                        /* copy from set/str */
                        if (at->kind == TY_SET) {
                            const char* sty = arena_sprintf("%%%s", at->mangled);
                            const char* lp = cg_reg(); const char* len = cg_reg();
                            const char* ip = cg_reg(); const char* arr = cg_reg();
                            cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", lp, sty, av);
                            cg_emit("%s = load i32, ptr %s", len, lp);
                            cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", ip, sty, av);
                            cg_emit("%s = load ptr, ptr %s", arr, ip);
                            const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p); cg_emit("store i32 0, ptr %s", i_p);
                            const char* ll = cg_lbl("sr_l"); const char* lb = cg_lbl("sr_b"); const char* ld = cg_lbl("sr_d");
                            cg_br(ll); cg_label(ll);
                            const char* iv = cg_reg(); const char* c = cg_reg();
                            cg_emit("%s = load i32, ptr %s", iv, i_p); cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
                            cg_cond_br(c, lb, ld); cg_label(lb);
                            const char* ep = cg_reg(); const char* ev = cg_reg();
                            cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, llvm_mem_ty(at->elem), arr, iv);
                            cg_emit("%s = load %s, ptr %s", ev, llvm_mem_ty(at->elem), ep);
                            cg_emit("call void @_%s_append(ptr %s, %s %s)", ty->mangled, res, llvm_mem_ty(ty->elem), ev);
                            const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, iv); cg_emit("store i32 %s, ptr %s", in, i_p);
                            cg_br(ll); cg_label(ld);
                        }
                    }
                    cg_emit("call void @_%s_%s(ptr %s)", ty->mangled, strcmp(cname, "sorted") == 0 ? "sort" : "reverse", res);
                    return res;
                }
                if (strcmp(cname, "list") == 0 || strcmp(cname, "iter") == 0 || strcmp(cname, "tuple") == 0) {
                    if (argc == 0) {
                        const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_new(i32 4)", r, ty->mangled); return r;
                    }
                    const char* av = cg_expr(args[0], NULL);
                    Type* at = args[0]->ty;
                    if (at->kind == TY_TUPLE && strcmp(cname, "tuple") == 0) return av;
                    if (at->kind == TY_LIST && strcmp(cname, "list") == 0) {
                        const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_copy(ptr %s)", r, ty->mangled, av); return r;
                    }
                    const char* res = cg_reg();
                    cg_emit("%s = call ptr @_%s_new(i32 8)", res, ty->mangled);
                    if (at->kind == TY_LIST) {
                        cg_emit("call void @_%s_extend(ptr %s, ptr %s)", ty->mangled, res, av);
                    } else if (at->kind == TY_SET || at->kind == TY_DICT) {
                        const char* sty = arena_sprintf("%%%s", at->mangled);
                        int lfld = (at->kind == TY_SET) ? 2 : 3;
                        const char* lp = cg_reg(); const char* len = cg_reg();
                        const char* ip = cg_reg(); const char* arr = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", lp, sty, av, lfld);
                        cg_emit("%s = load i32, ptr %s", len, lp);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", ip, sty, av);
                        cg_emit("%s = load ptr, ptr %s", arr, ip);
                        const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p); cg_emit("store i32 0, ptr %s", i_p);
                        const char* ll = cg_lbl("ls_l"); const char* lb = cg_lbl("ls_b"); const char* ld = cg_lbl("ls_d");
                        cg_br(ll); cg_label(ll);
                        const char* iv = cg_reg(); const char* c = cg_reg();
                        cg_emit("%s = load i32, ptr %s", iv, i_p); cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
                        cg_cond_br(c, lb, ld); cg_label(lb);
                        const char* ep = cg_reg(); const char* ev = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, llvm_mem_ty(ty->elem), arr, iv);
                        cg_emit("%s = load %s, ptr %s", ev, llvm_mem_ty(ty->elem), ep);
                        cg_emit("call void @_%s_append(ptr %s, %s %s)", ty->mangled, res, llvm_mem_ty(ty->elem), ev);
                        const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, iv); cg_emit("store i32 %s, ptr %s", in, i_p);
                        cg_br(ll); cg_label(ld);
                    } else if (at->kind == TY_STR) {
                        const char* lp = cg_reg(); const char* len = cg_reg();
                        cg_emit("%s = getelementptr %%StrictaStr, ptr %s, i32 0, i32 0", lp, av);
                        cg_emit("%s = load i32, ptr %s", len, lp);
                        const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p); cg_emit("store i32 0, ptr %s", i_p);
                        const char* ll = cg_lbl("lstr_l"); const char* lb = cg_lbl("lstr_b"); const char* ld = cg_lbl("lstr_d");
                        cg_br(ll); cg_label(ll);
                        const char* iv = cg_reg(); const char* c = cg_reg();
                        cg_emit("%s = load i32, ptr %s", iv, i_p); cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
                        cg_cond_br(c, lb, ld); cg_label(lb);
                        const char* iv64 = cg_reg(); const char* ch = cg_reg();
                        cg_emit("%s = sext i32 %s to i64", iv64, iv);
                        cg_emit("%s = call ptr @stricta_str_char_at(ptr %s, i64 %s)", ch, av, iv64);
                        cg_emit("call void @_%s_append(ptr %s, ptr %s)", ty->mangled, res, ch);
                        const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, iv); cg_emit("store i32 %s, ptr %s", in, i_p);
                        cg_br(ll); cg_label(ld);
                    } else if (at->kind == TY_TUPLE) {
                        for (int k = 0; k < at->tuple_count; k++) {
                            const char* fv = cg_reg();
                            cg_emit("%s = extractvalue %s %s, %d", fv, llvm_reg_ty(at), av, k);
                            cg_emit("call void @_%s_append(ptr %s, %s %s)", ty->mangled, res, llvm_mem_ty(ty->elem), fv);
                        }
                    }
                    return res;
                }
                if (strcmp(cname, "set") == 0) {
                    const char* res = cg_reg();
                    cg_emit("%s = call ptr @_%s_new(i32 8)", res, ty->mangled);
                    if (argc == 0) return res;
                    const char* av = cg_expr(args[0], NULL);
                    Type* at = args[0]->ty;
                    if (at->kind == TY_SET) {
                        cg_emit("call void @_%s_update(ptr %s, ptr %s)", ty->mangled, res, av);
                    } else if (at->kind == TY_LIST) {
                        const char* sty = arena_sprintf("%%%s", at->mangled);
                        const char* lp = cg_reg(); const char* len = cg_reg();
                        const char* ip = cg_reg(); const char* arr = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", lp, sty, av);
                        cg_emit("%s = load i32, ptr %s", len, lp);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", ip, sty, av);
                        cg_emit("%s = load ptr, ptr %s", arr, ip);
                        const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p); cg_emit("store i32 0, ptr %s", i_p);
                        const char* ll = cg_lbl("sl_l"); const char* lb = cg_lbl("sl_b"); const char* ld = cg_lbl("sl_d");
                        cg_br(ll); cg_label(ll);
                        const char* iv = cg_reg(); const char* c = cg_reg();
                        cg_emit("%s = load i32, ptr %s", iv, i_p); cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
                        cg_cond_br(c, lb, ld); cg_label(lb);
                        const char* ep = cg_reg(); const char* ev = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, llvm_mem_ty(at->elem), arr, iv);
                        cg_emit("%s = load %s, ptr %s", ev, llvm_mem_ty(at->elem), ep);
                        cg_emit("call void @_%s_add(ptr %s, %s %s)", ty->mangled, res, llvm_mem_ty(ty->elem), ev);
                        const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, iv); cg_emit("store i32 %s, ptr %s", in, i_p);
                        cg_br(ll); cg_label(ld);
                    } else if (at->kind == TY_STR) {
                        const char* lp = cg_reg(); const char* len = cg_reg();
                        cg_emit("%s = getelementptr %%StrictaStr, ptr %s, i32 0, i32 0", lp, av);
                        cg_emit("%s = load i32, ptr %s", len, lp);
                        const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p); cg_emit("store i32 0, ptr %s", i_p);
                        const char* ll = cg_lbl("ss_l"); const char* lb = cg_lbl("ss_b"); const char* ld = cg_lbl("ss_d");
                        cg_br(ll); cg_label(ll);
                        const char* iv = cg_reg(); const char* c = cg_reg();
                        cg_emit("%s = load i32, ptr %s", iv, i_p); cg_emit("%s = icmp slt i32 %s, %s", c, iv, len);
                        cg_cond_br(c, lb, ld); cg_label(lb);
                        const char* iv64 = cg_reg(); const char* ch = cg_reg();
                        cg_emit("%s = sext i32 %s to i64", iv64, iv);
                        cg_emit("%s = call ptr @stricta_str_char_at(ptr %s, i64 %s)", ch, av, iv64);
                        cg_emit("call void @_%s_add(ptr %s, ptr %s)", ty->mangled, res, ch);
                        const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, iv); cg_emit("store i32 %s, ptr %s", in, i_p);
                        cg_br(ll); cg_label(ld);
                    } else if (at->kind == TY_TUPLE) {
                        for (int k = 0; k < at->tuple_count; k++) {
                            const char* fv = cg_reg();
                            cg_emit("%s = extractvalue %s %s, %d", fv, llvm_reg_ty(at), av, k);
                            cg_emit("call void @_%s_add(ptr %s, %s %s)", ty->mangled, res, llvm_mem_ty(ty->elem), fv);
                        }
                    }
                    return res;
                }
                if (strcmp(cname, "dict") == 0) {
                    if (argc == 0) { const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_new(i32 8)", r, ty->mangled); return r; }
                    const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_copy(ptr %s)", r, ty->mangled, cg_expr(args[0], ty)); return r;
                }
                if (strcmp(cname, "next") == 0) {
                    const char* lv = cg_expr(args[0], NULL);
                    Type* lt = args[0]->ty;
                    const char* sty = arena_sprintf("%%%s", lt->mangled);
                    const char* lp = cg_reg(); const char* len = cg_reg(); const char* emp = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", lp, sty, lv);
                    cg_emit("%s = load i32, ptr %s", len, lp);
                    cg_emit("%s = icmp eq i32 %s, 0", emp, len);
                    const char* le = cg_lbl("nx_e"); const char* lo = cg_lbl("nx_o"); const char* ld = cg_lbl("nx_d");
                    cg_cond_br(emp, le, lo);
                    cg_label(le);
                    const char* defv = (argc == 2) ? cg_coerce(cg_expr(args[1], lt->elem), args[1]->ty, lt->elem) : llvm_zero(lt->elem);
                    if (argc < 2) {
                        cg_emit("call void @stricta_raise(i32 7, ptr null, ptr %s)", cg_cstr_lit("StopIteration", 13));
                    }
                    const char* lee = cg_lbl("nx_ee"); cg_br(lee); cg_label(lee); cg_br(ld);
                    cg_label(lo);
                    const char* pv = cg_reg();
                    cg_emit("%s = call %s @_%s_pop(ptr %s, i64 0)", pv, llvm_mem_ty(lt->elem), lt->mangled, lv);
                    const char* pv_r = cg_mem_to_reg(pv, lt->elem);
                    const char* loe = cg_lbl("nx_oe"); cg_br(loe); cg_label(loe); cg_br(ld);
                    cg_label(ld);
                    const char* res = cg_reg();
                    cg_emit("%s = phi %s [ %s, %%%s ], [ %s, %%%s ]", res, llvm_reg_ty(lt->elem), defv, lee, pv_r, loe);
                    return res;
                }
                if (strcmp(cname, "range") == 0) {
                    const char* s0 = "0"; const char* s1 = NULL; const char* st = "1";
                    if (argc == 1) s1 = cg_coerce(cg_expr(args[0], g_ty_i64), args[0]->ty, g_ty_i64);
                    else {
                        s0 = cg_coerce(cg_expr(args[0], g_ty_i64), args[0]->ty, g_ty_i64);
                        s1 = cg_coerce(cg_expr(args[1], g_ty_i64), args[1]->ty, g_ty_i64);
                        if (argc == 3) st = cg_coerce(cg_expr(args[2], g_ty_i64), args[2]->ty, g_ty_i64);
                    }
                    const char* res = cg_reg();
                    cg_emit("%s = call ptr @_%s_new(i32 16)", res, ty->mangled);
                    const char* ip = cg_reg(); cg_entry("%s = alloca i64", ip); cg_emit("store i64 %s, ptr %s", s0, ip);
                    const char* ll = cg_lbl("rg_l"); const char* lb = cg_lbl("rg_b"); const char* ld = cg_lbl("rg_d");
                    cg_br(ll); cg_label(ll);
                    const char* iv = cg_reg(); const char* c = cg_reg();
                    cg_emit("%s = load i64, ptr %s", iv, ip);
                    cg_emit("%s = icmp slt i64 %s, %s", c, iv, s1);
                    cg_cond_br(c, lb, ld); cg_label(lb);
                    cg_emit("call void @_%s_append(ptr %s, i64 %s)", ty->mangled, res, iv);
                    const char* in = cg_reg(); cg_emit("%s = add i64 %s, %s", in, iv, st); cg_emit("store i64 %s, ptr %s", in, ip);
                    cg_br(ll); cg_label(ld);
                    return res;
                }
                if (strcmp(cname, "open") == 0) {
                    const char* p = cg_expr(args[0], g_ty_str);
                    const char* m = (argc == 2) ? cg_expr(args[1], g_ty_str) : cg_strictastr_lit("r", 1);
                    const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_open(ptr %s, ptr %s)", r, p, m); return r;
                }
                if (strcmp(cname, "input") == 0) {
                    const char* p = (argc == 1) ? cg_expr(args[0], g_ty_str) : "null";
                    const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_input(ptr %s)", r, p); return r;
                }
                if (strcmp(cname, "isinstance") == 0) {
                    const char* ov = cg_expr(args[0], NULL);
                    ClassInfo* target = find_class_in_mod(g_cg.cur_mod, args[1]->var.name);
                    if (!target) return "false";
                    Type* ot = args[0]->ty;
                    if (ot->kind != TY_CLASS) return "false";
                    if (!ot->cls->needs_vtable) return class_is_subclass(ot->cls, target) ? "true" : "false";
                    const char* vpp = cg_reg(); const char* vt = cg_reg();
                    const char* mp = cg_reg(); const char* mask = cg_reg();
                    const char* bit = cg_reg(); const char* res = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", vpp, ot->cls->llvm_name, ov);
                    cg_emit("%s = load ptr, ptr %s", vt, vpp);
                    cg_emit("%s = getelementptr %%VTable_%s%s, ptr %s, i32 0, i32 1", mp, ot->cls->mod->prefix, ot->cls->name, vt);
                    cg_emit("%s = load i64, ptr %s", mask, mp);
                    cg_emit("%s = and i64 %s, %llu", bit, mask, (unsigned long long)(1ULL << target->type_id));
                    cg_emit("%s = icmp ne i64 %s, 0", res, bit);
                    return res;
                }

                /* Class constructor */
                ClassInfo* ci = find_class_in_mod(g_cg.cur_mod, cname);
                VarSym* gsym = find_global_in_mod(g_cg.cur_mod, cname);
                if (!ci && gsym && gsym->type && gsym->type->kind == TY_CLASS) ci = gsym->type->cls;
                if (ci) return cg_construct_class(ci, args, argc);

                /* Direct function call (nested or module) */
                FuncInfo* target_fn = NULL;
                if (g_cg.cur_fn) {
                    for (int i = 0; i < g_all_func_count; i++) {
                        if (g_all_funcs[i]->parent_fn == g_cg.cur_fn && strcmp(g_all_funcs[i]->name, cname) == 0) {
                            target_fn = g_all_funcs[i]; break;
                        }
                    }
                }
                if (!target_fn) target_fn = find_func_in_mod(g_cg.cur_mod, cname);
                if (target_fn) {
                    StrBuf ca; sb_init(&ca);
                    int arg_pos = 0;
                    if (target_fn->has_captures) {
                        const char* env_p = cg_reg();
                        cg_entry("%s = alloca [%d x ptr]", env_p, target_fn->capture_count);
                        for (int c = 0; c < target_fn->capture_count; c++) {
                            Type* dummy = NULL;
                            const char* cp = cg_var_ptr(target_fn->captures[c].name, &dummy);
                            const char* gep = cg_reg();
                            cg_emit("%s = getelementptr [%d x ptr], ptr %s, i32 0, i32 %d", gep, target_fn->capture_count, env_p, c);
                            cg_emit("store ptr %s, ptr %s", cp, gep);
                        }
                        sb_printf(&ca, "ptr %s", env_p);
                        arg_pos = 1;
                    }
                    for (int i = 0; i < target_fn->param_count; i++) {
                        Expr* ae = (i < argc) ? args[i] : target_fn->param_defaults[i];
                        Type* pt = target_fn->param_types[i];
                        const char* av = cg_coerce(cg_expr(ae, pt), ae->ty, pt);
                        if (arg_pos > 0 || i > 0) sb_append(&ca, ", ");
                        sb_printf(&ca, "%s %s", llvm_reg_ty(pt), av);
                    }
                    const char* call_sym = target_fn->llvm_name;
                    if (target_fn->ast && target_fn->ast->def_s.deco_count > 0 && !target_fn->parent_fn) {
                        const char* fp = cg_reg();
                        cg_emit("%s = load ptr, ptr @_decfn_%s%s", fp, target_fn->mod->prefix, target_fn->name);
                        call_sym = fp;
                    }
                    if (target_fn->ret_type->kind == TY_VOID) {
                        cg_emit("call void %s(%s)", call_sym, ca.data);
                        free(ca.data); return "0";
                    }
                    const char* r = cg_reg();
                    cg_emit("%s = call %s %s(%s)", r, llvm_reg_ty(target_fn->ret_type), call_sym, ca.data);
                    free(ca.data); return r;
                }
            }

            /* Indirect function call */
            const char* fn_val = cg_expr(callee, NULL);
            Type* ft = callee->ty;
            StrBuf ca; sb_init(&ca);
            for (int i = 0; i < argc; i++) {
                Type* pt = ft->func_params[i];
                const char* av = cg_coerce(cg_expr(args[i], pt), args[i]->ty, pt);
                if (i > 0) sb_append(&ca, ", ");
                sb_printf(&ca, "%s %s", llvm_reg_ty(pt), av);
            }
            if (ft->func_ret->kind == TY_VOID) {
                cg_emit("call void %s(%s)", fn_val, ca.data);
                free(ca.data); return "0";
            }
            const char* r = cg_reg();
            cg_emit("%s = call %s %s(%s)", r, llvm_reg_ty(ft->func_ret), fn_val, ca.data);
            free(ca.data); return r;
        }
    }
    return "0";
}

static bool expr_escapes_var(Expr* e, const char* vname, bool allowed_pos) {
    if (!e) return false;
    switch (e->kind) {
        case EXPR_VAR:
            if (strcmp(e->var.name, vname) == 0 && !allowed_pos) return true;
            return false;
        case EXPR_INDEX:
            return expr_escapes_var(e->index.obj, vname, true) || expr_escapes_var(e->index.index, vname, false);
        case EXPR_CALL:
            if (e->call.callee->kind == EXPR_ATTR && e->call.callee->attr.obj->kind == EXPR_VAR &&
                strcmp(e->call.callee->attr.obj->var.name, vname) == 0) {
                const char* mn = e->call.callee->attr.attr;
                if (strcmp(mn, "append") == 0 || strcmp(mn, "clear") == 0 || strcmp(mn, "pop") == 0) {
                    for (int i = 0; i < e->call.arg_count; i++) {
                        if (expr_escapes_var(e->call.args[i], vname, false)) return true;
                    }
                    return false;
                }
            }
            if (e->call.callee->kind == EXPR_VAR && strcmp(e->call.callee->var.name, "len") == 0 && e->call.arg_count == 1) {
                return expr_escapes_var(e->call.args[0], vname, true);
            }
            if (expr_escapes_var(e->call.callee, vname, false)) return true;
            for (int i = 0; i < e->call.arg_count; i++) {
                if (expr_escapes_var(e->call.args[i], vname, false)) return true;
            }
            return false;
        case EXPR_BINOP:
            return expr_escapes_var(e->binop.left, vname, false) || expr_escapes_var(e->binop.right, vname, false);
        case EXPR_UNOP:
            return expr_escapes_var(e->unop.operand, vname, false);
        case EXPR_TERNARY:
            return expr_escapes_var(e->ternary.cond, vname, false) || expr_escapes_var(e->ternary.then_e, vname, false) ||
                   expr_escapes_var(e->ternary.else_e, vname, false);
        case EXPR_LIST: case EXPR_TUPLE: case EXPR_SET:
            for (int i = 0; i < e->seq.count; i++) if (expr_escapes_var(e->seq.items[i], vname, false)) return true;
            return false;
        case EXPR_ATTR:
            return expr_escapes_var(e->attr.obj, vname, false);
        default:
            return true;
    }
}

static bool stmts_escape_var(Stmt** stmts, int count, const char* vname) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        switch (s->kind) {
            case STMT_RETURN:
                if (expr_escapes_var(s->ret.expr, vname, false)) return true;
                break;
            case STMT_YIELD: case STMT_YIELD_FROM: case STMT_EXPR:
                if (expr_escapes_var(s->expr_s.expr, vname, false)) return true;
                break;
            case STMT_ASSIGN:
                if (s->assign.target->kind == EXPR_VAR && strcmp(s->assign.target->var.name, vname) == 0 &&
                    s->assign.value && s->assign.value->kind == EXPR_LIST && s->assign.value->seq.count == 0) {
                    break;
                }
                if (s->assign.target->kind == EXPR_VAR && strcmp(s->assign.target->var.name, vname) == 0) {
                    return true;
                }
                if ((s->assign.target->kind == EXPR_INDEX && expr_escapes_var(s->assign.target, vname, true)) ||
                    expr_escapes_var(s->assign.value, vname, false)) return true;
                break;
            case STMT_AUG_ASSIGN:
                if (expr_escapes_var(s->aug_assign.target, vname, false) || expr_escapes_var(s->aug_assign.value, vname, false)) return true;
                break;
            case STMT_IF:
                for (int b = 0; b < s->if_s.branch_count; b++) {
                    if (expr_escapes_var(s->if_s.conds[b], vname, false)) return true;
                    if (stmts_escape_var(s->if_s.bodies[b], s->if_s.body_counts[b], vname)) return true;
                }
                if (stmts_escape_var(s->if_s.else_body, s->if_s.else_count, vname)) return true;
                break;
            case STMT_WHILE:
                if (expr_escapes_var(s->while_s.cond, vname, false)) return true;
                if (stmts_escape_var(s->while_s.body, s->while_s.body_count, vname)) return true;
                if (stmts_escape_var(s->while_s.else_body, s->while_s.else_count, vname)) return true;
                break;
            case STMT_FOR:
                if (expr_escapes_var(s->for_s.iter, vname, true)) return true;
                if (stmts_escape_var(s->for_s.body, s->for_s.body_count, vname)) return true;
                if (stmts_escape_var(s->for_s.else_body, s->for_s.else_count, vname)) return true;
                break;
            case STMT_DEF:
                return true;
            default:
                break;
        }
    }
    return false;
}

static void cg_stmts(Stmt** stmts, int count) {
    for (int i = 0; i < count; i++) {
        if (g_cg.block_terminated) break;
        Stmt* s = stmts[i];
        switch (s->kind) {
            case STMT_PASS:
            case STMT_GLOBAL:
            case STMT_NONLOCAL:
            case STMT_DEF:
            case STMT_CLASS:
            case STMT_IMPORT:
            case STMT_FROM_IMPORT:
                break;
            case STMT_BREAK:
                if (g_cg.loop_depth > 0) {
                    LoopCtx* lc = &g_cg.loop_stack[g_cg.loop_depth - 1];
                    if (lc->broke_alloca) cg_emit("store i1 true, ptr %s", lc->broke_alloca);
                    cg_br(lc->break_lbl);
                }
                break;
            case STMT_CONTINUE:
                if (g_cg.loop_depth > 0) {
                    cg_br(g_cg.loop_stack[g_cg.loop_depth - 1].cont_lbl);
                }
                break;
            case STMT_RETURN:
                if (g_cg.cur_fn->ret_type->kind == TY_VOID) {
                    cg_emit("ret void");
                } else {
                    const char* rv = cg_coerce(cg_expr(s->ret.expr, g_cg.cur_fn->ret_type), s->ret.expr->ty, g_cg.cur_fn->ret_type);
                    cg_emit("ret %s %s", llvm_reg_ty(g_cg.cur_fn->ret_type), rv);
                }
                g_cg.block_terminated = true;
                break;
            case STMT_YIELD: {
                Type* el = g_cg.cur_fn->ret_type->elem;
                const char* yv = cg_coerce(cg_expr(s->expr_s.expr, el), s->expr_s.expr->ty, el);
                cg_emit("call void @_%s_append(ptr %s, %s %s)", g_cg.cur_fn->ret_type->mangled,
                        g_cg.yield_list_reg, llvm_mem_ty(el), cg_reg_to_mem(yv, el));
                break;
            }
            case STMT_YIELD_FROM: {
                const char* sub = cg_expr(s->expr_s.expr, g_cg.cur_fn->ret_type);
                cg_emit("call void @_%s_extend(ptr %s, ptr %s)", g_cg.cur_fn->ret_type->mangled, g_cg.yield_list_reg, sub);
                break;
            }
            case STMT_EXPR:
                cg_expr(s->expr_s.expr, NULL);
                break;
            case STMT_ASSIGN: {
                if (!s->assign.value) {
                    if (s->assign.target->kind == EXPR_VAR) {
                        Type* vt = NULL; cg_var_ptr(s->assign.target->var.name, &vt);
                    }
                    break;
                }
                Expr* tgt = s->assign.target;
                if (tgt->kind == EXPR_VAR) {
                    Type* vt = NULL;
                    const char* vp = cg_var_ptr(tgt->var.name, &vt);
                    if (g_cg.cur_fn && g_cg.cur_fn->ast && s->assign.value->kind == EXPR_LIST &&
                        s->assign.value->seq.count == 0 && vt->kind == TY_LIST &&
                        !stmts_escape_var(g_cg.cur_fn->ast->def_s.body, g_cg.cur_fn->ast->def_s.body_count, tgt->var.name)) {
                        const char* sty = arena_sprintf("%%%s", vt->mangled);
                        const char* mty = llvm_mem_ty(vt->elem);
                        const char* sl = cg_reg(); const char* sb = cg_reg();
                        cg_entry("%s = alloca %s", sl, sty);
                        cg_entry("%s = alloca [64 x %s]", sb, mty);
                        const char* p0 = cg_reg(); const char* p1 = cg_reg(); const char* p2 = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", p0, sty, sl);
                        cg_emit("store i32 0, ptr %s", p0);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 1", p1, sty, sl);
                        cg_emit("store i32 -64, ptr %s", p1);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", p2, sty, sl);
                        cg_emit("store ptr %s, ptr %s", sb, p2);
                        cg_emit("store ptr %s, ptr %s", sl, vp);
                        break;
                    }
                    const char* v = cg_coerce(cg_expr(s->assign.value, vt), s->assign.value->ty, vt);
                    cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(v, vt), vp);
                } else if (tgt->kind == EXPR_ATTR) {
                    Type* ot = tgt->attr.obj->ty;
                    FieldInfo* fi = find_field_in_class(ot->cls, tgt->attr.attr);
                    if (fi) {
                        const char* ov = cg_expr(tgt->attr.obj, NULL);
                        const char* v = cg_coerce(cg_expr(s->assign.value, fi->type), s->assign.value->ty, fi->type);
                        if (ot->cls->is_value_type) {
                            /* Value type mutation inside __init__ or local */
                            if (tgt->attr.obj->kind == EXPR_VAR) {
                                Type* dummy = NULL;
                                const char* vp = cg_var_ptr(tgt->attr.obj->var.name, &dummy);
                                const char* fp = cg_reg();
                                cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, ot->cls->llvm_name, vp, fi->slot_index);
                                cg_emit("store %s %s, ptr %s", llvm_mem_ty(fi->type), cg_reg_to_mem(v, fi->type), fp);
                            }
                        } else {
                            const char* fp = cg_reg();
                            cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, ot->cls->llvm_name, ov, fi->slot_index);
                            cg_emit("store %s %s, ptr %s", llvm_mem_ty(fi->type), cg_reg_to_mem(v, fi->type), fp);
                        }
                    } else {
                        FieldInfo* ca = find_class_attr(ot->cls, tgt->attr.attr);
                        ClassInfo* owner = ot->cls;
                        for (int m = 0; m < ot->cls->mro_count; m++) {
                            for (int k = 0; k < ot->cls->mro[m]->class_attr_count; k++) {
                                if (strcmp(ot->cls->mro[m]->class_attrs[k].name, tgt->attr.attr) == 0) { owner = ot->cls->mro[m]; goto fca_s; }
                            }
                        }
                        fca_s:;
                        const char* v = cg_coerce(cg_expr(s->assign.value, ca->type), s->assign.value->ty, ca->type);
                        cg_emit("store %s %s, ptr @_cattr_%s%s__%s", llvm_mem_ty(ca->type), cg_reg_to_mem(v, ca->type),
                                owner->mod->prefix, owner->name, tgt->attr.attr);
                    }
                } else if (tgt->kind == EXPR_INDEX) {
                    const char* ov = cg_expr(tgt->index.obj, NULL);
                    Type* ot = tgt->index.obj->ty;
                    if (ot->kind == TY_LIST) {
                        const char* iv = cg_coerce(cg_expr(tgt->index.index, g_ty_i64), tgt->index.index->ty, g_ty_i64);
                        const char* vv = cg_coerce(cg_expr(s->assign.value, ot->elem), s->assign.value->ty, ot->elem);
                        cg_emit("call void @_%s_set(ptr %s, i64 %s, %s %s)", ot->mangled, ov, iv, llvm_mem_ty(ot->elem), cg_reg_to_mem(vv, ot->elem));
                    } else if (ot->kind == TY_DICT) {
                        const char* kv = cg_coerce(cg_expr(tgt->index.index, ot->key), tgt->index.index->ty, ot->key);
                        const char* vv = cg_coerce(cg_expr(s->assign.value, ot->val), s->assign.value->ty, ot->val);
                        cg_emit("call void @_%s_set(ptr %s, %s %s, %s %s)", ot->mangled, ov,
                                llvm_mem_ty(ot->key), cg_reg_to_mem(kv, ot->key),
                                llvm_mem_ty(ot->val), cg_reg_to_mem(vv, ot->val));
                    }
                }
                break;
            }
            case STMT_MULTI_ASSIGN: {
                const char* tv = cg_expr(s->multi_assign.value, NULL);
                Type* tt = s->multi_assign.value->ty;
                for (int k = 0; k < s->multi_assign.target_count; k++) {
                    Type* vt = NULL;
                    const char* vp = cg_var_ptr(s->multi_assign.targets[k]->var.name, &vt);
                    const char* fm = cg_reg();
                    cg_emit("%s = extractvalue %s %s, %d", fm, llvm_reg_ty(tt), tv, k);
                    cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), fm, vp);
                }
                break;
            }
            case STMT_AUG_ASSIGN: {
                char bop[8];
                size_t olen = strlen(s->aug_assign.op);
                memcpy(bop, s->aug_assign.op, olen - 1);
                bop[olen - 1] = '\0';
                Expr* tgt = s->aug_assign.target;
                if (tgt->kind == EXPR_VAR) {
                    Type* vt = NULL;
                    const char* vp = cg_var_ptr(tgt->var.name, &vt);
                    const char* cur = cg_mem_to_reg(({ const char* r = cg_reg(); cg_emit("%s = load %s, ptr %s", r, llvm_mem_ty(vt), vp); r; }), vt);
                    const char* rv = cg_expr(s->aug_assign.value, vt);
                    if (vt->kind == TY_SET) {
                        const char* fnm = strcmp(bop, "|") == 0 ? "update" : strcmp(bop, "&") == 0 ? "intersection_update" :
                                          strcmp(bop, "-") == 0 ? "difference_update" : "symmetric_difference_update";
                        cg_emit("call void @_%s_%s(ptr %s, ptr %s)", vt->mangled, fnm, cur, rv);
                    } else {
                        const char* res = cg_binop_values(bop, cur, vt, rv, s->aug_assign.value->ty, vt);
                        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(cg_coerce(res, vt, vt), vt), vp);
                    }
                } else if (tgt->kind == EXPR_INDEX) {
                    const char* ov = cg_expr(tgt->index.obj, NULL);
                    Type* ot = tgt->index.obj->ty;
                    const char* iv = cg_coerce(cg_expr(tgt->index.index, g_ty_i64), tgt->index.index->ty, g_ty_i64);
                    const char* cur = cg_mem_to_reg(({ const char* r = cg_reg(); cg_emit("%s = call %s @_%s_get(ptr %s, i64 %s)", r, llvm_mem_ty(ot->elem), ot->mangled, ov, iv); r; }), ot->elem);
                    const char* rv = cg_expr(s->aug_assign.value, ot->elem);
                    const char* res = cg_binop_values(bop, cur, ot->elem, rv, s->aug_assign.value->ty, ot->elem);
                    cg_emit("call void @_%s_set(ptr %s, i64 %s, %s %s)", ot->mangled, ov, iv, llvm_mem_ty(ot->elem), cg_reg_to_mem(res, ot->elem));
                }
                break;
            }
            case STMT_IF: {
                const char* l_end = cg_lbl("if_end");
                for (int b = 0; b < s->if_s.branch_count; b++) {
                    const char* cv = cg_truthy(cg_expr(s->if_s.conds[b], g_ty_bool), s->if_s.conds[b]->ty);
                    const char* l_t = cg_lbl("if_then");
                    const char* l_f = (b + 1 < s->if_s.branch_count || s->if_s.else_count > 0) ? cg_lbl("if_next") : l_end;
                    cg_cond_br(cv, l_t, l_f);
                    cg_label(l_t);
                    cg_stmts(s->if_s.bodies[b], s->if_s.body_counts[b]);
                    cg_br(l_end);
                    if (l_f != l_end) cg_label(l_f);
                }
                if (s->if_s.else_count > 0) {
                    cg_stmts(s->if_s.else_body, s->if_s.else_count);
                    cg_br(l_end);
                }
                cg_label(l_end);
                break;
            }
            case STMT_WHILE: {
                const char* l_cond = cg_lbl("wh_cond");
                const char* l_body = cg_lbl("wh_body");
                const char* l_else = s->while_s.else_count > 0 ? cg_lbl("wh_else") : NULL;
                const char* l_end = cg_lbl("wh_end");
                cg_br(l_cond);
                cg_label(l_cond);
                const char* cv = cg_truthy(cg_expr(s->while_s.cond, g_ty_bool), s->while_s.cond->ty);
                cg_cond_br(cv, l_body, l_else ? l_else : l_end);
                cg_label(l_body);
                g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){l_end, l_cond, NULL, NULL};
                cg_stmts(s->while_s.body, s->while_s.body_count);
                g_cg.loop_depth--;
                cg_br(l_cond);
                if (l_else) {
                    cg_label(l_else);
                    cg_stmts(s->while_s.else_body, s->while_s.else_count);
                    cg_br(l_end);
                }
                cg_label(l_end);
                break;
            }
            case STMT_FOR: {
                /* Fast loop-interchanged matrix multiply: for i in range(N): for j in range(M): C[i][j] = sum(A[i][k] * B[k][j] for k in range(K)) */
                if (s->for_s.else_count == 0 && s->for_s.target_count == 1 &&
                    s->for_s.iter->kind == EXPR_CALL && s->for_s.iter->call.callee->kind == EXPR_VAR &&
                    strcmp(s->for_s.iter->call.callee->var.name, "range") == 0 && s->for_s.iter->call.arg_count == 1 &&
                    s->for_s.body_count == 1 && s->for_s.body[0]->kind == STMT_FOR &&
                    s->for_s.body[0]->for_s.else_count == 0 && s->for_s.body[0]->for_s.target_count == 1 &&
                    s->for_s.body[0]->for_s.iter->kind == EXPR_CALL && s->for_s.body[0]->for_s.iter->call.callee->kind == EXPR_VAR &&
                    strcmp(s->for_s.body[0]->for_s.iter->call.callee->var.name, "range") == 0 && s->for_s.body[0]->for_s.iter->call.arg_count == 1 &&
                    s->for_s.body[0]->for_s.body_count == 1 && s->for_s.body[0]->for_s.body[0]->kind == STMT_ASSIGN) {
                    Stmt* inner_for = s->for_s.body[0];
                    Stmt* asg = inner_for->for_s.body[0];
                    if (asg->assign.target->kind == EXPR_INDEX && asg->assign.target->index.obj->kind == EXPR_INDEX &&
                        asg->assign.value && asg->assign.value->kind == EXPR_CALL &&
                        asg->assign.value->call.callee->kind == EXPR_VAR &&
                        strcmp(asg->assign.value->call.callee->var.name, "sum") == 0 &&
                        asg->assign.value->call.arg_count == 1 &&
                        asg->assign.value->call.args[0]->kind == EXPR_GENEXPR &&
                        asg->assign.value->call.args[0]->comp.gen_count == 1 &&
                        asg->assign.value->call.args[0]->comp.gens[0].if_count == 0 &&
                        asg->assign.value->call.args[0]->comp.gens[0].target_count == 1 &&
                        asg->assign.value->call.args[0]->comp.gens[0].iter->kind == EXPR_CALL &&
                        asg->assign.value->call.args[0]->comp.gens[0].iter->call.callee->kind == EXPR_VAR &&
                        strcmp(asg->assign.value->call.args[0]->comp.gens[0].iter->call.callee->var.name, "range") == 0 &&
                        asg->assign.value->call.args[0]->comp.gens[0].iter->call.arg_count == 1 &&
                        asg->assign.value->call.args[0]->comp.elt->kind == EXPR_BINOP &&
                        strcmp(asg->assign.value->call.args[0]->comp.elt->binop.op, "*") == 0) {
                        Expr* lhs_c = asg->assign.target;
                        Expr* a_term = asg->assign.value->call.args[0]->comp.elt->binop.left;
                        Expr* b_term = asg->assign.value->call.args[0]->comp.elt->binop.right;
                        const char* ivar = s->for_s.targets[0];
                        const char* jvar = inner_for->for_s.targets[0];
                        const char* kvar = asg->assign.value->call.args[0]->comp.gens[0].targets[0];
                        if (a_term->kind == EXPR_INDEX && a_term->index.obj->kind == EXPR_INDEX &&
                            b_term->kind == EXPR_INDEX && b_term->index.obj->kind == EXPR_INDEX &&
                            lhs_c->index.obj->index.index->kind == EXPR_VAR && strcmp(lhs_c->index.obj->index.index->var.name, ivar) == 0 &&
                            lhs_c->index.index->kind == EXPR_VAR && strcmp(lhs_c->index.index->var.name, jvar) == 0 &&
                            a_term->index.obj->index.index->kind == EXPR_VAR && strcmp(a_term->index.obj->index.index->var.name, ivar) == 0 &&
                            a_term->index.index->kind == EXPR_VAR && strcmp(a_term->index.index->var.name, kvar) == 0 &&
                            b_term->index.obj->index.index->kind == EXPR_VAR && strcmp(b_term->index.obj->index.index->var.name, kvar) == 0 &&
                            b_term->index.index->kind == EXPR_VAR && strcmp(b_term->index.index->var.name, jvar) == 0 &&
                            lhs_c->ty && type_is_float(lhs_c->ty)) {
                            const char* n_v = cg_coerce(cg_expr(s->for_s.iter->call.args[0], g_ty_i64), s->for_s.iter->call.args[0]->ty, g_ty_i64);
                            const char* m_v = cg_coerce(cg_expr(inner_for->for_s.iter->call.args[0], g_ty_i64), inner_for->for_s.iter->call.args[0]->ty, g_ty_i64);
                            const char* k_v = cg_coerce(cg_expr(asg->assign.value->call.args[0]->comp.gens[0].iter->call.args[0], g_ty_i64),
                                                        asg->assign.value->call.args[0]->comp.gens[0].iter->call.args[0]->ty, g_ty_i64);
                            const char* c_mat = cg_expr(lhs_c->index.obj->index.obj, NULL);
                            const char* a_mat = cg_expr(a_term->index.obj->index.obj, NULL);
                            const char* b_mat = cg_expr(b_term->index.obj->index.obj, NULL);
                            const char* m2d = lhs_c->index.obj->index.obj->ty->mangled;
                            const char* m1d = lhs_c->index.obj->ty->mangled;
                            const char* ety = llvm_mem_ty(lhs_c->ty);
                            const char* ip = cg_reg(); const char* kp = cg_reg(); const char* jp = cg_reg();
                            cg_entry("%s = alloca i64", ip); cg_entry("%s = alloca i64", kp); cg_entry("%s = alloca i64", jp);
                            const char* c_rows_p = cg_reg(); const char* c_rows = cg_reg();
                            const char* a_rows_p = cg_reg(); const char* a_rows = cg_reg();
                            const char* b_rows_p = cg_reg(); const char* b_rows = cg_reg();
                            cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 2", c_rows_p, m2d, c_mat);
                            cg_emit("%s = load ptr, ptr %s", c_rows, c_rows_p);
                            cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 2", a_rows_p, m2d, a_mat);
                            cg_emit("%s = load ptr, ptr %s", a_rows, a_rows_p);
                            cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 2", b_rows_p, m2d, b_mat);
                            cg_emit("%s = load ptr, ptr %s", b_rows, b_rows_p);

                            cg_emit("store i64 0, ptr %s", ip);
                            const char* li = cg_lbl("mm_i"); const char* lib = cg_lbl("mm_ib"); const char* lie = cg_lbl("mm_ie");
                            const char* lk = cg_lbl("mm_k"); const char* lkb = cg_lbl("mm_kb"); const char* lke = cg_lbl("mm_ke");
                            const char* lj = cg_lbl("mm_j"); const char* ljb = cg_lbl("mm_jb"); const char* lje = cg_lbl("mm_je");
                            cg_br(li); cg_label(li);
                            const char* iv = cg_reg(); const char* ic = cg_reg();
                            cg_emit("%s = load i64, ptr %s", iv, ip); cg_emit("%s = icmp slt i64 %s, %s", ic, iv, n_v);
                            cg_cond_br(ic, lib, lie); cg_label(lib);
                            const char* cr_p = cg_reg(); const char* cr_l = cg_reg(); const char* cr_d_p = cg_reg(); const char* cr_d = cg_reg();
                            const char* ar_p = cg_reg(); const char* ar_l = cg_reg(); const char* ar_d_p = cg_reg(); const char* ar_d = cg_reg();
                            cg_emit("%s = getelementptr ptr, ptr %s, i64 %s", cr_p, c_rows, iv);
                            cg_emit("%s = load ptr, ptr %s", cr_l, cr_p);
                            cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 2", cr_d_p, m1d, cr_l);
                            cg_emit("%s = load ptr, ptr %s", cr_d, cr_d_p);
                            cg_emit("%s = getelementptr ptr, ptr %s, i64 %s", ar_p, a_rows, iv);
                            cg_emit("%s = load ptr, ptr %s", ar_l, ar_p);
                            cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 2", ar_d_p, m1d, ar_l);
                            cg_emit("%s = load ptr, ptr %s", ar_d, ar_d_p);
                            /* Zero C[i] row first? Note C[i][j] was initialized to 0.0 */
                            cg_emit("store i64 0, ptr %s", kp);
                            cg_br(lk); cg_label(lk);
                            const char* kv = cg_reg(); const char* kc = cg_reg();
                            cg_emit("%s = load i64, ptr %s", kv, kp); cg_emit("%s = icmp slt i64 %s, %s", kc, kv, k_v);
                            cg_cond_br(kc, lkb, lke); cg_label(lkb);
                            const char* aik_p = cg_reg(); const char* aik = cg_reg();
                            const char* br_p = cg_reg(); const char* br_l = cg_reg(); const char* br_d_p = cg_reg(); const char* br_d = cg_reg();
                            cg_emit("%s = getelementptr %s, ptr %s, i64 %s", aik_p, ety, ar_d, kv);
                            cg_emit("%s = load %s, ptr %s", aik, ety, aik_p);
                            cg_emit("%s = getelementptr ptr, ptr %s, i64 %s", br_p, b_rows, kv);
                            cg_emit("%s = load ptr, ptr %s", br_l, br_p);
                            cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 2", br_d_p, m1d, br_l);
                            cg_emit("%s = load ptr, ptr %s", br_d, br_d_p);
                            cg_emit("store i64 0, ptr %s", jp);
                            cg_br(lj); cg_label(lj);
                            const char* jv = cg_reg(); const char* jc = cg_reg();
                            cg_emit("%s = load i64, ptr %s", jv, jp); cg_emit("%s = icmp slt i64 %s, %s", jc, jv, m_v);
                            cg_cond_br(jc, ljb, lje); cg_label(ljb);
                            const char* bkj_p = cg_reg(); const char* bkj = cg_reg();
                            const char* cij_p = cg_reg(); const char* cij = cg_reg();
                            const char* prod = cg_reg(); const char* ncij = cg_reg();
                            cg_emit("%s = getelementptr %s, ptr %s, i64 %s", bkj_p, ety, br_d, jv);
                            cg_emit("%s = load %s, ptr %s", bkj, ety, bkj_p);
                            cg_emit("%s = getelementptr %s, ptr %s, i64 %s", cij_p, ety, cr_d, jv);
                            cg_emit("%s = load %s, ptr %s", cij, ety, cij_p);
                            cg_emit("%s = fmul %s %s, %s", prod, ety, aik, bkj);
                            cg_emit("%s = fadd %s %s, %s", ncij, ety, cij, prod);
                            cg_emit("store %s %s, ptr %s", ety, ncij, cij_p);
                            const char* jn = cg_reg(); cg_emit("%s = add i64 %s, 1", jn, jv); cg_emit("store i64 %s, ptr %s", jn, jp);
                            cg_br(lj); cg_label(lje);
                            const char* kn = cg_reg(); cg_emit("%s = add i64 %s, 1", kn, kv); cg_emit("store i64 %s, ptr %s", kn, kp);
                            cg_br(lk); cg_label(lke);
                            const char* in = cg_reg(); cg_emit("%s = add i64 %s, 1", in, iv); cg_emit("store i64 %s, ptr %s", in, ip);
                            cg_br(li); cg_label(lie);
                            break;
                        }
                    }
                }
                const char* l_else = s->for_s.else_count > 0 ? cg_lbl("for_else") : NULL;
                const char* l_end = cg_lbl("for_end");
                cg_iter_loop(s->for_s.iter, s->for_s.targets, s->for_s.target_count,
                             s->for_s.body, s->for_s.body_count, l_else, l_end);
                if (l_else) {
                    cg_label(l_else);
                    cg_stmts(s->for_s.else_body, s->for_s.else_count);
                    cg_br(l_end);
                }
                cg_label(l_end);
                break;
            }
            case STMT_ASSERT: {
                const char* cv = cg_truthy(cg_expr(s->assert_s.cond, g_ty_bool), s->assert_s.cond->ty);
                const char* l_ok = cg_lbl("as_ok");
                const char* l_fail = cg_lbl("as_fail");
                cg_cond_br(cv, l_ok, l_fail);
                cg_label(l_fail);
                const char* mp = "null";
                if (s->assert_s.msg) {
                    const char* ms = cg_to_str(cg_expr(s->assert_s.msg, NULL), s->assert_s.msg->ty);
                    const char* sp = cg_reg(); mp = cg_reg();
                    cg_emit("%s = getelementptr %%StrictaStr, ptr %s, i32 0, i32 2", sp, ms);
                    cg_emit("%s = load ptr, ptr %s", mp, sp);
                }
                cg_emit("call void @stricta_assert_fail(ptr %s, ptr %s, i32 %d)", mp,
                        cg_cstr_lit(s->loc.file ? s->loc.file : "", s->loc.file ? strlen(s->loc.file) : 0), s->loc.line);
                cg_emit("unreachable");
                cg_label(l_ok);
                break;
            }
            case STMT_RAISE: {
                if (!s->raise_s.exc) {
                    cg_emit("call void @stricta_reraise()");
                    cg_emit("unreachable");
                    g_cg.block_terminated = true;
                    break;
                }
                const char* ev = cg_expr(s->raise_s.exc, NULL);
                Type* et = s->raise_s.exc->ty;
                if (et->kind == TY_CLASS) {
                    cg_emit("call void @stricta_raise(i32 %d, ptr %s, ptr null)", et->cls->type_id, ev);
                } else {
                    const char* es = cg_to_str(ev, et);
                    const char* sp = cg_reg(); const char* cp = cg_reg();
                    cg_emit("%s = getelementptr %%StrictaStr, ptr %s, i32 0, i32 2", sp, es);
                    cg_emit("%s = load ptr, ptr %s", cp, sp);
                    cg_emit("call void @stricta_raise(i32 1, ptr null, ptr %s)", cp);
                }
                cg_emit("unreachable");
                g_cg.block_terminated = true;
                break;
            }
            case STMT_TRY: {
                const char* jbuf = cg_reg(); const char* sj = cg_reg(); const char* is_ok = cg_reg();
                const char* l_try = cg_lbl("try_b"); const char* l_catch = cg_lbl("try_c");
                const char* l_else = s->try_s.else_count > 0 ? cg_lbl("try_e") : NULL;
                const char* l_fin = s->try_s.finally_count > 0 ? cg_lbl("try_f") : NULL;
                const char* l_end = cg_lbl("try_end");
                cg_emit("%s = call ptr @stricta_try_push()", jbuf);
                cg_emit("%s = call i32 @setjmp(ptr %s) returns_twice", sj, jbuf);
                cg_emit("%s = icmp eq i32 %s, 0", is_ok, sj);
                cg_cond_br(is_ok, l_try, l_catch);

                cg_label(l_try);
                cg_stmts(s->try_s.try_body, s->try_s.try_count);
                cg_emit("call void @stricta_try_pop()");
                cg_br(l_else ? l_else : (l_fin ? l_fin : l_end));

                if (l_else) {
                    cg_label(l_else);
                    cg_stmts(s->try_s.else_body, s->try_s.else_count);
                    cg_br(l_fin ? l_fin : l_end);
                }

                cg_label(l_catch);
                cg_emit("call void @stricta_try_pop()");
                if (s->try_s.handler_count == 0) {
                    if (l_fin) {
                        cg_stmts(s->try_s.finally_body, s->try_s.finally_count);
                    }
                    cg_emit("call void @stricta_reraise()");
                    cg_emit("unreachable");
                    g_cg.block_terminated = true;
                } else {
                    const char* tid = cg_reg();
                    cg_emit("%s = call i32 @stricta_exc_type_id()", tid);
                    for (int h = 0; h < s->try_s.handler_count; h++) {
                        ExceptHandler* eh = &s->try_s.handlers[h];
                        const char* l_h = cg_lbl("eh_b");
                        const char* l_nxt = cg_lbl("eh_n");
                        if (!eh->exc_type) {
                            cg_br(l_h);
                        } else {
                            ClassInfo* eci = find_class_in_mod(g_cg.cur_mod, eh->exc_type);
                            if (eci) {
                                const char* m = cg_reg();
                                cg_emit("%s = icmp eq i32 %s, %d", m, tid, eci->type_id);
                                cg_cond_br(m, l_h, l_nxt);
                            } else {
                                cg_br(l_h);
                            }
                        }
                        cg_label(l_h);
                        if (eh->as_name) {
                            Type* vt = NULL; const char* vp = cg_var_ptr(eh->as_name, &vt);
                            const char* ev = cg_reg();
                            if (vt->kind == TY_CLASS) cg_emit("%s = call ptr @stricta_exc_obj()", ev);
                            else cg_emit("%s = call ptr @stricta_exc_msg()", ev);
                            cg_emit("store ptr %s, ptr %s", ev, vp);
                        }
                        cg_stmts(eh->body, eh->body_count);
                        cg_br(l_fin ? l_fin : l_end);
                        cg_label(l_nxt);
                    }
                    if (l_fin) cg_stmts(s->try_s.finally_body, s->try_s.finally_count);
                    cg_emit("call void @stricta_reraise()");
                    cg_emit("unreachable");
                    g_cg.block_terminated = true;
                }
                if (l_fin) {
                    cg_label(l_fin);
                    cg_stmts(s->try_s.finally_body, s->try_s.finally_count);
                    cg_br(l_end);
                }
                cg_label(l_end);
                break;
            }
            case STMT_WITH: {
                const char* cv = cg_expr(s->with_s.ctx, NULL);
                Type* ct = s->with_s.ctx->ty;
                const char* bound = cv;
                if (ct->kind == TY_CLASS) {
                    FuncInfo* ent = find_method_in_class(ct->cls, "__enter__");
                    bound = cg_reg();
                    cg_emit("%s = call %s %s(%s %s)", bound, llvm_reg_ty(ent->ret_type), ent->llvm_name, llvm_reg_ty(ct), cv);
                    if (s->with_s.as_name) {
                        Type* vt = NULL; const char* vp = cg_var_ptr(s->with_s.as_name, &vt);
                        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(bound, vt), vp);
                    }
                } else if (ct->kind == TY_FILE && s->with_s.as_name) {
                    Type* vt = NULL; const char* vp = cg_var_ptr(s->with_s.as_name, &vt);
                    cg_emit("store ptr %s, ptr %s", bound, vp);
                }
                const char* jbuf = cg_reg(); const char* sj = cg_reg(); const char* ok = cg_reg();
                const char* l_ok = cg_lbl("w_ok"); const char* l_ex = cg_lbl("w_ex"); const char* l_end = cg_lbl("w_end");
                cg_emit("%s = call ptr @stricta_try_push()", jbuf);
                cg_emit("%s = call i32 @setjmp(ptr %s) returns_twice", sj, jbuf);
                cg_emit("%s = icmp eq i32 %s, 0", ok, sj);
                cg_cond_br(ok, l_ok, l_ex);
                cg_label(l_ok);
                cg_stmts(s->with_s.body, s->with_s.body_count);
                cg_emit("call void @stricta_try_pop()");
                if (ct->kind == TY_FILE) {
                    cg_emit("call void @stricta_file_close(ptr %s)", cv);
                } else if (ct->kind == TY_CLASS) {
                    FuncInfo* ex = find_method_in_class(ct->cls, "__exit__");
                    if (ex) {
                        const char* zr = cg_reg();
                        cg_emit("%s = call i1 %s(%s %s, %s %s, %s %s, %s %s)", zr, ex->llvm_name,
                                llvm_reg_ty(ct), cv,
                                llvm_reg_ty(ex->param_types[1]), llvm_zero(ex->param_types[1]),
                                llvm_reg_ty(ex->param_types[2]), llvm_zero(ex->param_types[2]),
                                llvm_reg_ty(ex->param_types[3]), llvm_zero(ex->param_types[3]));
                    }
                }
                cg_br(l_end);
                cg_label(l_ex);
                cg_emit("call void @stricta_try_pop()");
                if (ct->kind == TY_FILE) {
                    cg_emit("call void @stricta_file_close(ptr %s)", cv);
                    cg_emit("call void @stricta_reraise()");
                    cg_emit("unreachable");
                } else if (ct->kind == TY_CLASS) {
                    FuncInfo* ex = find_method_in_class(ct->cls, "__exit__");
                    if (ex) {
                        const char* sup = cg_reg();
                        cg_emit("%s = call i1 %s(%s %s, %s %s, %s %s, %s %s)", sup, ex->llvm_name,
                                llvm_reg_ty(ct), cv,
                                llvm_reg_ty(ex->param_types[1]), llvm_zero(ex->param_types[1]),
                                llvm_reg_ty(ex->param_types[2]), llvm_zero(ex->param_types[2]),
                                llvm_reg_ty(ex->param_types[3]), llvm_zero(ex->param_types[3]));
                        const char* l_rr = cg_lbl("w_rr");
                        cg_cond_br(sup, l_end, l_rr);
                        cg_label(l_rr);
                    }
                    cg_emit("call void @stricta_reraise()");
                    cg_emit("unreachable");
                }
                cg_label(l_end);
                break;
            }
            case STMT_DEL: {
                Expr* dt = s->del_s.target;
                if (dt->kind == EXPR_INDEX) {
                    const char* ov = cg_expr(dt->index.obj, NULL);
                    Type* ot = dt->index.obj->ty;
                    if (ot->kind == TY_LIST) {
                        cg_emit("call void @_%s_del(ptr %s, i64 %s)", ot->mangled, ov,
                                cg_coerce(cg_expr(dt->index.index, g_ty_i64), dt->index.index->ty, g_ty_i64));
                    } else if (ot->kind == TY_DICT) {
                        const char* kv = cg_reg_to_mem(cg_coerce(cg_expr(dt->index.index, ot->key), dt->index.index->ty, ot->key), ot->key);
                        cg_emit("call void @_%s_del(ptr %s, %s %s)", ot->mangled, ov, llvm_mem_ty(ot->key), kv);
                    }
                }
                break;
            }
            case STMT_MATCH: {
                const char* sv = cg_expr(s->match_s.subject, NULL);
                Type* st = s->match_s.subject->ty;
                const char* l_end = cg_lbl("m_end");
                for (int c = 0; c < s->match_s.case_count; c++) {
                    MatchCase* mc = &s->match_s.cases[c];
                    const char* l_b = cg_lbl("m_b");
                    const char* l_n = cg_lbl("m_n");
                    if (!mc->pattern) {
                        if (mc->capture) {
                            Type* vt = NULL; const char* vp = cg_var_ptr(mc->capture, &vt);
                            cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(sv, vt), vp);
                        }
                        cg_br(l_b);
                    } else {
                        const char* pv = cg_expr(mc->pattern, st);
                        const char* eq = cg_binop_values("==", sv, st, pv, mc->pattern->ty, g_ty_bool);
                        cg_cond_br(eq, l_b, l_n);
                    }
                    cg_label(l_b);
                    cg_stmts(mc->body, mc->body_count);
                    cg_br(l_end);
                    cg_label(l_n);
                    if (!mc->pattern) break;
                }
                cg_br(l_end);
                cg_label(l_end);
                break;
            }
        }
    }
}

static void emit_func_ir(FuncInfo* fn) {
    g_cg.cur_fn = fn;
    g_cg.cur_mod = fn->mod;
    g_cg.in_func = true;
    g_cg.block_terminated = false;
    g_cg.reg_id = 0;
    g_cg.fn_alloca_count = 0;
    g_cg.cur_entry.len = 0; g_cg.cur_entry.data[0] = '\0';
    g_cg.cur_body.len = 0; g_cg.cur_body.data[0] = '\0';
    g_cg.yield_list_reg = NULL;

    StrBuf sig; sb_init(&sig);
    int arg_idx = 0;
    if (fn->has_captures) {
        sb_append(&sig, "ptr %__env");
        arg_idx = 1;
    }
    for (int i = 0; i < fn->param_count; i++) {
        if (arg_idx > 0 || i > 0) sb_append(&sig, ", ");
        sb_printf(&sig, "%s %%arg_%d", llvm_reg_ty(fn->param_types[i]), i);
        VarSym* lv = find_local_in_fn(fn, fn->param_names[i]);
        if (lv) {
            cg_ensure_local_alloca(lv->llvm_name, lv->type);
            cg_entry("store %s %s, ptr %s", llvm_mem_ty(lv->type),
                     (lv->type->kind == TY_BOOL) ? arena_sprintf("%%bext_%d", i) : arena_sprintf("%%arg_%d", i),
                     lv->llvm_name);
            if (lv->type->kind == TY_BOOL) {
                /* emit zext before store in entry */
                StrBuf tmp; sb_init(&tmp);
                sb_printf(&tmp, "  %%bext_%d = zext i1 %%arg_%d to i8\n", i, i);
                /* prepend to cur_entry? Better: emit zext in body or rebuild entry */
                free(tmp.data);
            }
        }
    }
    /* Cleanly rebuild entry parameter stores so bool zext precedes store */
    g_cg.cur_entry.len = 0; g_cg.cur_entry.data[0] = '\0';
    g_cg.fn_alloca_count = 0;
    for (int i = 0; i < fn->param_count; i++) {
        VarSym* lv = find_local_in_fn(fn, fn->param_names[i]);
        if (lv) {
            cg_ensure_local_alloca(lv->llvm_name, lv->type);
            if (lv->type->kind == TY_BOOL) {
                cg_entry("%%bext_%d = zext i1 %%arg_%d to i8", i, i);
                cg_entry("store i8 %%bext_%d, ptr %s", i, lv->llvm_name);
            } else {
                cg_entry("store %s %%arg_%d, ptr %s", llvm_mem_ty(lv->type), i, lv->llvm_name);
            }
        }
    }

    if (fn->is_generator) {
        g_cg.yield_list_reg = "%__yield_list";
        cg_emit("%s = call ptr @_%s_new(i32 8)", g_cg.yield_list_reg, fn->ret_type->mangled);
    }

    if (fn->is_lambda) {
        const char* rv = cg_coerce(cg_expr(fn->lambda_ast->lambda.body, fn->ret_type), fn->lambda_ast->lambda.body->ty, fn->ret_type);
        if (fn->ret_type->kind == TY_VOID) cg_emit("ret void");
        else cg_emit("ret %s %s", llvm_reg_ty(fn->ret_type), rv);
        g_cg.block_terminated = true;
    } else if (fn->ast) {
        cg_stmts(fn->ast->def_s.body, fn->ast->def_s.body_count);
    }

    if (!g_cg.block_terminated) {
        if (fn->is_generator) cg_emit("ret ptr %s", g_cg.yield_list_reg);
        else if (fn->ret_type->kind == TY_VOID) cg_emit("ret void");
        else cg_emit("ret %s %s", llvm_reg_ty(fn->ret_type), llvm_zero(fn->ret_type));
    }

    sb_printf(&g_cg.funcs, "define %s %s(%s) {\nentry:\n", llvm_reg_ty(fn->ret_type), fn->llvm_name, sig.data);
    sb_append_n(&g_cg.funcs, g_cg.cur_entry.data, g_cg.cur_entry.len);
    sb_append_n(&g_cg.funcs, g_cg.cur_body.data, g_cg.cur_body.len);
    sb_append(&g_cg.funcs, "}\n\n");
    free(sig.data);
    g_cg.in_func = false;
    g_cg.cur_fn = NULL;
}

static void emit_program_llvm(ModuleInfo* main_mod, StrBuf* out) {
    cg_init();

    /* Header: external runtime declarations & core structs */
    sb_append(&g_cg.hdr, "; Stricta Static LLVM IR Compiler Output\n\n");
    sb_append(&g_cg.hdr, "%StrictaStr = type { i32, i32, ptr }\n");
    sb_append(&g_cg.hdr, "%StrictaBytes = type { i32, i32, ptr }\n");
    sb_append(&g_cg.hdr, "@stricta_sys_argv = external global ptr\n");
    sb_append(&g_cg.hdr, "declare void @stricta_init_args(i32, ptr)\n");
    sb_append(&g_cg.hdr, "declare noalias ptr @malloc(i64)\n");
    sb_append(&g_cg.hdr, "declare noalias ptr @calloc(i64, i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @realloc(ptr, i64)\n");
    sb_append(&g_cg.hdr, "declare void @free(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_alloc(i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_calloc(i64, i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_realloc(ptr, i64)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_free(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_try_push()\n");
    sb_append(&g_cg.hdr, "declare void @stricta_try_pop()\n");
    sb_append(&g_cg.hdr, "declare i32 @setjmp(ptr) returns_twice\n");
    sb_append(&g_cg.hdr, "declare void @stricta_raise(i32, ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_reraise()\n");
    sb_append(&g_cg.hdr, "declare i32 @stricta_exc_type_id()\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_exc_obj()\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_exc_msg()\n");
    sb_append(&g_cg.hdr, "declare void @stricta_assert_fail(ptr, ptr, i32)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_concat(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_repeat(ptr, i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_slice(ptr, i64, i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_char_at(ptr, i64)\n");
    sb_append(&g_cg.hdr, "declare i32 @stricta_str_eq(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare i32 @stricta_str_cmp(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare i64 @stricta_str_hash(ptr)\n");
    sb_append(&g_cg.hdr, "declare i32 @stricta_str_contains(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_split(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_join(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_upper(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_lower(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_replace(ptr, ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare i64 @stricta_str_find(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare i64 @stricta_str_index(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_strip(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_lstrip(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_str_rstrip(ptr)\n");
    sb_append(&g_cg.hdr, "declare i32 @stricta_str_startswith(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare i32 @stricta_str_endswith(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare i64 @stricta_str_count(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_int_to_str(i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_uint_to_str(i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_float_to_str(double)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_float_fmt(double, i32)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bool_to_str(i32)\n");
    sb_append(&g_cg.hdr, "declare i64 @stricta_str_to_int(ptr)\n");
    sb_append(&g_cg.hdr, "declare double @stricta_str_to_float(ptr)\n");
    sb_append(&g_cg.hdr, "declare i64 @stricta_ord(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_chr(i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_hex(i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bin(i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_oct(i64)\n");
    sb_append(&g_cg.hdr, "declare i64 @stricta_pow_i64(i64, i64)\n");
    sb_append(&g_cg.hdr, "declare double @pow(double, double)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bytes_new(ptr, i32)\n");
    sb_append(&g_cg.hdr, "declare i32 @stricta_bytes_eq(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_from_str(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_from_i64(i64)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_add(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_sub(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_mul(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_div(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_mod(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_pow(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_neg(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_abs(ptr)\n");
    sb_append(&g_cg.hdr, "declare i32 @stricta_bigint_cmp(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_bigint_to_str(ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_open(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_file_read(ptr)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_file_write(ptr, ptr)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_file_close(ptr)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_print_i64(i64)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_print_u64(i64)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_print_f64(double)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_print_bool(i32)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_print_str(ptr)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_print_cstr(ptr)\n");
    sb_append(&g_cg.hdr, "declare void @stricta_print_space()\n");
    sb_append(&g_cg.hdr, "declare void @stricta_print_newline()\n");
    sb_append(&g_cg.hdr, "declare ptr @stricta_input(ptr)\n");
    sb_append(&g_cg.hdr, "declare i32 @llvm.smax.i32(i32, i32)\n");
    sb_append(&g_cg.hdr, "declare i32 @llvm.smin.i32(i32, i32)\n");
    sb_append(&g_cg.hdr, "declare i64 @llvm.smax.i64(i64, i64)\n");
    sb_append(&g_cg.hdr, "declare i64 @llvm.smin.i64(i64, i64)\n");
    sb_append(&g_cg.hdr, "declare double @llvm.floor.f64(double)\n");
    sb_append(&g_cg.hdr, "declare float @llvm.floor.f32(float)\n");
    sb_append(&g_cg.hdr, "declare double @llvm.round.f64(double)\n");
    sb_append(&g_cg.hdr, "declare double @llvm.fabs.f64(double)\n");
    sb_append(&g_cg.hdr, "declare float @llvm.fabs.f32(float)\n");
    sb_append(&g_cg.hdr, "declare i64 @llvm.abs.i64(i64, i1)\n");
    sb_append(&g_cg.hdr, "declare i32 @llvm.abs.i32(i32, i1)\n");
    sb_append(&g_cg.hdr, "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n\n");

    /* Emit Class Structs & VTables */
    for (int i = 0; i < g_all_class_count; i++) {
        ClassInfo* cls = g_all_classes[i];
        if (!cls->ast) continue;
        sb_printf(&g_cg.hdr, "%s = type { ", cls->llvm_name);
        int cnt = 0;
        if (cls->needs_vtable) { sb_append(&g_cg.hdr, "ptr"); cnt++; }
        for (int f = 0; f < cls->field_count; f++) {
            if (cnt > 0) sb_append(&g_cg.hdr, ", ");
            sb_append(&g_cg.hdr, llvm_mem_ty(cls->fields[f].type));
            cnt++;
        }
        if (cnt == 0) sb_append(&g_cg.hdr, "i8");
        sb_append(&g_cg.hdr, " }\n");

        if (cls->needs_vtable) {
            sb_printf(&g_cg.hdr, "%%VTable_%s%s = type { i32, i64", cls->mod->prefix, cls->name);
            for (int v = 0; v < cls->vtable_count; v++) sb_append(&g_cg.hdr, ", ptr");
            sb_append(&g_cg.hdr, " }\n");

            sb_printf(&g_cg.hdr, "@_vtable_%s%s = global %%VTable_%s%s { i32 %d, i64 %llu",
                      cls->mod->prefix, cls->name, cls->mod->prefix, cls->name,
                      cls->type_id, (unsigned long long)cls->ancestors_mask);
            for (int v = 0; v < cls->vtable_count; v++) {
                sb_printf(&g_cg.hdr, ", ptr %s", cls->vtable[v].fn ? cls->vtable[v].fn->llvm_name : "null");
            }
            sb_append(&g_cg.hdr, " }\n");
        }
        for (int k = 0; k < cls->class_attr_count; k++) {
            FieldInfo* ca = &cls->class_attrs[k];
            sb_printf(&g_cg.hdr, "@_cattr_%s%s__%s = global %s %s\n",
                      cls->mod->prefix, cls->name, ca->name, llvm_mem_ty(ca->type), llvm_mem_zero(ca->type));
        }
    }
    sb_append(&g_cg.hdr, "\n");

    /* Emit Module Globals into globs (after struct defs are ready) */
    StrBuf globs;
    sb_init(&globs);
    for (int m = 0; m < g_module_count; m++) {
        ModuleInfo* mod = g_modules[m];
        for (int g = 0; g < mod->global_count; g++) {
            VarSym* gv = &mod->globals[g];
            if (gv->type->kind == TY_MODULE) continue;
            const char* expected = arena_sprintf("@%s%s", mod->prefix, gv->name);
            if (strcmp(gv->llvm_name, expected) != 0) continue;
            sb_printf(&globs, "%s = global %s %s\n", gv->llvm_name, llvm_mem_ty(gv->type), llvm_mem_zero(gv->type));
        }
        for (int f = 0; f < mod->func_count; f++) {
            FuncInfo* fn = mod->funcs[f];
            if (fn->ast && fn->ast->def_s.deco_count > 0) {
                sb_printf(&globs, "@_decfn_%s%s = global ptr %s\n", mod->prefix, fn->name, fn->llvm_name);
            }
        }
    }
    sb_append(&globs, "\n");

    /* Emit all user functions & methods first (note: some statements inside functions might register mono types via dict.items() etc.) */
    for (int i = 0; i < g_all_func_count; i++) {
        emit_func_ir(g_all_funcs[i]);
    }

    /* Emit module init functions (imported modules in dependency order, then main_mod) */
    for (int m = 0; m < g_module_count; m++) {
        ModuleInfo* mod = g_modules[m];
        if (mod == main_mod) continue;
        g_cg.cur_fn = NULL;
        g_cg.cur_mod = mod;
        g_cg.in_func = false;
        g_cg.block_terminated = false;
        g_cg.reg_id = 0;
        g_cg.fn_alloca_count = 0;
        g_cg.cur_entry.len = 0; g_cg.cur_entry.data[0] = '\0';
        g_cg.cur_body.len = 0; g_cg.cur_body.data[0] = '\0';
        for (int c = 0; c < mod->class_count; c++) {
            ClassInfo* cls = mod->classes[c];
            for (int k = 0; k < cls->class_attr_count; k++) {
                FieldInfo* ca = &cls->class_attrs[k];
                if (ca->default_expr) {
                    const char* dv = cg_coerce(cg_expr(ca->default_expr, ca->type), ca->default_expr->ty, ca->type);
                    cg_emit("store %s %s, ptr @_cattr_%s%s__%s", llvm_mem_ty(ca->type), cg_reg_to_mem(dv, ca->type),
                            mod->prefix, cls->name, ca->name);
                }
            }
        }
        cg_stmts(mod->stmts, mod->stmt_count);
        sb_printf(&g_cg.funcs, "define void @__init_%s() {\nentry:\n", mod->prefix);
        sb_append_n(&g_cg.funcs, g_cg.cur_entry.data, g_cg.cur_entry.len);
        sb_append_n(&g_cg.funcs, g_cg.cur_body.data, g_cg.cur_body.len);
        sb_append(&g_cg.funcs, "  ret void\n}\n\n");
    }

    /* Emit @stricta_main for main_mod */
    g_cg.cur_fn = NULL;
    g_cg.cur_mod = main_mod;
    g_cg.in_func = false;
    g_cg.block_terminated = false;
    g_cg.reg_id = 0;
    g_cg.fn_alloca_count = 0;
    g_cg.cur_entry.len = 0; g_cg.cur_entry.data[0] = '\0';
    g_cg.cur_body.len = 0; g_cg.cur_body.data[0] = '\0';
    for (int m = 0; m < g_module_count; m++) {
        if (g_modules[m] != main_mod) cg_emit("call void @__init_%s()", g_modules[m]->prefix);
    }
    for (int c = 0; c < main_mod->class_count; c++) {
        ClassInfo* cls = main_mod->classes[c];
        for (int k = 0; k < cls->class_attr_count; k++) {
            FieldInfo* ca = &cls->class_attrs[k];
            if (ca->default_expr) {
                const char* dv = cg_coerce(cg_expr(ca->default_expr, ca->type), ca->default_expr->ty, ca->type);
                cg_emit("store %s %s, ptr @_cattr_%s%s__%s", llvm_mem_ty(ca->type), cg_reg_to_mem(dv, ca->type),
                        main_mod->prefix, cls->name, ca->name);
            }
        }
    }
    for (int f = 0; f < main_mod->func_count; f++) {
        FuncInfo* fn = main_mod->funcs[f];
        if (fn->ast && fn->ast->def_s.deco_count > 0) {
            const char* cur = fn->llvm_name;
            for (int d = fn->ast->def_s.deco_count - 1; d >= 0; d--) {
                Expr* dec = fn->ast->def_s.decorators[d];
                if (dec->kind == EXPR_VAR) {
                    FuncInfo* dfn = find_func_in_mod(main_mod, dec->var.name);
                    if (dfn) {
                        const char* nxt = cg_reg();
                        cg_emit("%s = call ptr %s(ptr %s)", nxt, dfn->llvm_name, cur);
                        cur = nxt;
                    }
                }
            }
            cg_emit("store ptr %s, ptr @_decfn_%s%s", cur, main_mod->prefix, fn->name);
        }
    }
    cg_stmts(main_mod->stmts, main_mod->stmt_count);
    sb_append(&g_cg.funcs, "define void @stricta_main() {\nentry:\n");
    sb_append_n(&g_cg.funcs, g_cg.cur_entry.data, g_cg.cur_entry.len);
    sb_append_n(&g_cg.funcs, g_cg.cur_body.data, g_cg.cur_body.len);
    sb_append(&g_cg.funcs, "  ret void\n}\n\n");

    sb_append(&g_cg.funcs, "define i32 @main(i32 %argc, ptr %argv) {\nentry:\n");
    sb_append(&g_cg.funcs, "  call void @stricta_init_args(i32 %argc, ptr %argv)\n");
    sb_append(&g_cg.funcs, "  call void @stricta_main()\n");
    sb_append(&g_cg.funcs, "  ret i32 0\n}\n");

    /* Now emit all registered monomorphized container struct types & helper functions (after all codegen discoveries!) */
    for (int i = 0; i < g_mono_opts.count; i++) {
        Type* t = g_mono_opts.items[i];
        sb_printf(&g_cg.hdr, "%%%s = type { i8, %s }\n", t->mangled, llvm_mem_ty(t->elem));
    }
    for (int i = 0; i < g_mono_tuples.count; i++) {
        Type* t = g_mono_tuples.items[i];
        sb_printf(&g_cg.hdr, "%%%s = type { ", t->mangled);
        for (int k = 0; k < t->tuple_count; k++) {
            if (k > 0) sb_append(&g_cg.hdr, ", ");
            sb_append(&g_cg.hdr, llvm_mem_ty(t->tuple_elems[k]));
        }
        if (t->tuple_count == 0) sb_append(&g_cg.hdr, "i8");
        sb_append(&g_cg.hdr, " }\n");
    }
    for (int i = 0; i < g_mono_lists.count; i++) {
        Type* t = g_mono_lists.items[i];
        sb_printf(&g_cg.hdr, "%%%s = type { i32, i32, ptr }\n", t->mangled);
    }
    for (int i = 0; i < g_mono_sets.count; i++) {
        Type* t = g_mono_sets.items[i];
        sb_printf(&g_cg.hdr, "%%%s = type { ptr, ptr, i32, i32, i32 }\n", t->mangled);
    }
    for (int i = 0; i < g_mono_dicts.count; i++) {
        Type* t = g_mono_dicts.items[i];
        sb_printf(&g_cg.hdr, "%%%s = type { ptr, ptr, ptr, i32, i32, i32, i32 }\n", t->mangled);
    }
    sb_append(&g_cg.hdr, "\n");

    /* Emit monomorphized functions (note: dict emits popitem/items which might add tuples/lists, so iterate while count grows!) */
    int li = 0, si = 0, di = 0;
    while (li < g_mono_lists.count || si < g_mono_sets.count || di < g_mono_dicts.count) {
        int prev_tc = g_mono_tuples.count;
        int prev_lc = g_mono_lists.count;
        while (di < g_mono_dicts.count) emit_mono_dict(g_mono_dicts.items[di++]);
        while (si < g_mono_sets.count) emit_mono_set(g_mono_sets.items[si++]);
        while (li < g_mono_lists.count) emit_mono_list(g_mono_lists.items[li++]);
        /* If dict added new tuples or lists, emit their struct definitions into hdr! */
        for (int k = prev_tc; k < g_mono_tuples.count; k++) {
            Type* t = g_mono_tuples.items[k];
            sb_printf(&g_cg.hdr, "%%%s = type { ", t->mangled);
            for (int j = 0; j < t->tuple_count; j++) {
                if (j > 0) sb_append(&g_cg.hdr, ", ");
                sb_append(&g_cg.hdr, llvm_mem_ty(t->tuple_elems[j]));
            }
            if (t->tuple_count == 0) sb_append(&g_cg.hdr, "i8");
            sb_append(&g_cg.hdr, " }\n");
        }
        for (int k = prev_lc; k < g_mono_lists.count; k++) {
            Type* t = g_mono_lists.items[k];
            sb_printf(&g_cg.hdr, "%%%s = type { i32, i32, ptr }\n", t->mangled);
        }
    }

    sb_append_n(out, g_cg.hdr.data, g_cg.hdr.len);
    sb_append_n(out, globs.data, globs.len);
    free(globs.data);
    sb_append_n(out, g_cg.strs.data, g_cg.strs.len);
    sb_append(out, "\n");
    sb_append_n(out, g_cg.mono.data, g_cg.mono.len);
    sb_append_n(out, g_cg.funcs.data, g_cg.funcs.len);
}

/* =========================================================================
 * 6. CLI & DRIVER
 * ========================================================================= */

static bool ends_with(const char* s, const char* suf) {
    size_t sl = strlen(s), sufl = strlen(suf);
    return sl >= sufl && strcmp(s + sl - sufl, suf) == 0;
}

static bool find_runtime_c(const char* argv0, char* out_buf, size_t cap) {
    if (access("runtime.c", R_OK) == 0) {
        snprintf(out_buf, cap, "runtime.c");
        return true;
    }
    char* exe_copy = strdup(argv0);
    char* dir = dirname(exe_copy);
    snprintf(out_buf, cap, "%s/runtime.c", dir);
    free(exe_copy);
    if (access(out_buf, R_OK) == 0) return true;
    return false;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.str> [-o <output>]\n", argv[0]);
        return 1;
    }
    const char* input_file = argv[1];
    const char* output_file = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        }
    }
    if (!output_file) {
        static char def_out[1024];
        snprintf(def_out, sizeof(def_out), "%s", input_file);
        char* dot = strrchr(def_out, '.');
        if (dot && strcmp(dot, ".str") == 0) *dot = '\0';
        else strcat(def_out, ".out");
        output_file = def_out;
    }

    size_t src_len = 0;
    char* src = read_entire_file(input_file, &src_len);
    if (!src) {
        fprintf(stderr, "error: cannot open '%s'\n", input_file);
        return 1;
    }

    char* icopy = strdup(input_file);
    g_root_dir = arena_strdup(dirname(icopy));
    free(icopy);

    init_builtin_types();

    ModuleInfo* main_mod = (ModuleInfo*)arena_alloc(sizeof(ModuleInfo));
    main_mod->name = "__main__";
    main_mod->path = arena_strdup(input_file);
    main_mod->prefix = "u_";

    TokenList tl = lex_source(main_mod->path, src, src_len);
    free(src);
    Parser ps = {tl.items, tl.count, 0, main_mod->path};
    main_mod->stmts = parse_module(&ps, &main_mod->stmt_count);
    free(tl.items);

    g_modules = (ModuleInfo**)realloc(g_modules, sizeof(ModuleInfo*) * (size_t)(g_module_count + 1));
    g_modules[g_module_count++] = main_mod;
    analyze_module(main_mod);

    StrBuf ir;
    sb_init(&ir);
    emit_program_llvm(main_mod, &ir);

    if (ends_with(output_file, ".ll")) {
        FILE* f = fopen(output_file, "wb");
        if (!f) { fprintf(stderr, "error: cannot write '%s'\n", output_file); return 1; }
        fwrite(ir.data, 1, ir.len, f);
        fclose(f);
        free(ir.data);
        return 0;
    }

    char tmp_ll[1024];
    snprintf(tmp_ll, sizeof(tmp_ll), "/tmp/stricta_%d.ll", (int)getpid());
    FILE* f = fopen(tmp_ll, "wb");
    if (!f) { fprintf(stderr, "error: cannot write temp '%s'\n", tmp_ll); return 1; }
    fwrite(ir.data, 1, ir.len, f);
    fclose(f);
    free(ir.data);

    char rt_path[1024];
    if (!find_runtime_c(argv[0], rt_path, sizeof(rt_path))) {
        fprintf(stderr, "error: cannot locate runtime.c\n");
        remove(tmp_ll);
        return 1;
    }

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "clang -O2 -Wno-override-module '%s' '%s' -lm -o '%s'", tmp_ll, rt_path, output_file);
    int rc = system(cmd);
    remove(tmp_ll);
    return rc == 0 ? 0 : 1;
}


