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
    TOK_IMPORT, TOK_FROM, TOK_YIELD, TOK_LAMBDA,
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
    KW("yield", TOK_YIELD);
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
typedef struct FuncInfo FuncInfo;

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
    EXPR_TERNARY, EXPR_WALRUS, EXPR_LAMBDA, EXPR_AWAIT
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
    bool is_nonneg;
    bool is_inbounds;
    bool is_move;
    bool is_ptr_take;
    union {
        struct { const char* raw; int64_t ival; uint64_t uval; bool is_big; } int_lit;
        struct { double fval; } float_lit;
        struct { bool bval; } bool_lit;
        struct { const char* text; size_t len; } str_lit;
        struct { FStrPart* parts; int count; } fstr;
        struct { const char* name; } var;
        struct { const char* op; Expr* left; Expr* right; } binop;
        struct { const char* op; Expr* operand; } unop;
        struct { Expr* callee; Expr** args; const char** kw_names; int arg_count; FuncInfo* resolved_fn; } call;
        struct { Expr* obj; const char* attr; } attr;
        struct { Expr* obj; Expr* index; } index;
        struct { Expr* obj; Expr* start; Expr* stop; Expr* step; } slice;
        struct { Expr** items; int count; } seq;
        struct { Expr** keys; Expr** vals; int count; } dict;
        struct { Expr* elt; Expr* val; CompGen* gens; int gen_count; } comp;
        struct { Expr* cond; Expr* then_e; Expr* else_e; } ternary;
        struct { const char* name; Expr* val; } walrus;
        struct {
            const char** params; TypeExpr** param_types; int param_count;
            TypeExpr* ret_type; Expr* body;
        } lambda;
        struct { Expr* expr; } await_e;
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
        if ((strcmp(name, "ref") == 0 || strcmp(name, "addr") == 0) &&
            p_check(ps, TOK_NAME) &&
            (strcmp(p_peek(ps)->text, "mut") == 0 || strcmp(p_peek(ps)->text, "const") == 0)) {
            TokKind nk = (ps->pos + 1 < ps->count) ? ps->toks[ps->pos + 1].kind : TOK_EOF;
            if (nk != TOK_RBRACK && nk != TOK_COMMA && nk != TOK_PIPE) {
                Token* mod_tok = p_consume(ps, TOK_NAME);
                if (strcmp(name, "ref") == 0 && strcmp(mod_tok->text, "mut") == 0) name = "mut_ref";
            }
        }
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
                if (!p_check(ps, TOK_COLON) && !p_check(ps, TOK_RBRACK)) stop = parse_expr(ps);
                Expr* step = NULL;
                if (p_match(ps, TOK_COLON)) {
                    if (!p_check(ps, TOK_RBRACK)) step = parse_expr(ps);
                }
                p_consume(ps, TOK_RBRACK);
                Expr* se = new_expr(EXPR_SLICE, t->loc);
                se->slice.obj = e; se->slice.start = start; se->slice.stop = stop; se->slice.step = step;
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
        Expr* opnd = parse_factor(ps);
        Expr* ae = new_expr(EXPR_AWAIT, t->loc);
        ae->await_e.expr = opnd;
        return ae;
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

static int skip_lambda_type_ann(Parser* ps, int pos) {
    if (pos >= ps->count || ps->toks[pos].kind != TOK_NAME) return -1;
    pos++;
    while (pos + 1 < ps->count && ps->toks[pos].kind == TOK_DOT && ps->toks[pos + 1].kind == TOK_NAME) {
        pos += 2;
    }
    if (pos < ps->count && ps->toks[pos].kind == TOK_LBRACK) {
        int d = 1; pos++;
        while (pos < ps->count && d > 0) {
            if (ps->toks[pos].kind == TOK_LBRACK) d++;
            else if (ps->toks[pos].kind == TOK_RBRACK) d--;
            pos++;
        }
        if (d != 0) return -1;
    }
    while (pos < ps->count && ps->toks[pos].kind == TOK_PIPE) {
        pos = skip_lambda_type_ann(ps, pos + 1);
        if (pos < 0) return -1;
    }
    return pos;
}

static bool lambda_colon_is_type_ann(Parser* ps) {
    int pos = skip_lambda_type_ann(ps, ps->pos + 1);
    if (pos < 0 || pos >= ps->count) return false;
    while (pos < ps->count && ps->toks[pos].kind == TOK_COMMA) {
        pos++;
        if (pos >= ps->count || ps->toks[pos].kind != TOK_NAME) return false;
        pos++;
        if (pos < ps->count && ps->toks[pos].kind == TOK_COLON) {
            pos = skip_lambda_type_ann(ps, pos + 1);
            if (pos < 0) return false;
        }
    }
    return pos < ps->count && (ps->toks[pos].kind == TOK_COLON || ps->toks[pos].kind == TOK_ARROW);
}

static Expr* parse_expr(Parser* ps) {
    if (p_check(ps, TOK_LAMBDA)) {
        Token* t = p_consume(ps, TOK_LAMBDA);
        const char** pnames = NULL; TypeExpr** ptypes = NULL; int pcnt = 0, pcap = 0;
        if (!p_check(ps, TOK_COLON) && !p_check(ps, TOK_ARROW)) {
            do {
                Token* id = p_consume(ps, TOK_NAME);
                TypeExpr* pt = NULL;
                if (p_check(ps, TOK_COLON) && lambda_colon_is_type_ann(ps)) {
                    p_consume(ps, TOK_COLON);
                    pt = parse_type_expr(ps);
                }
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
        while (p_match(ps, TOK_MINUS)) {
            Token* s2 = p_consume(ps, TOK_NAME);
            mod_name = arena_sprintf("%s-%s", mod_name, s2->text);
        }
        while (p_check(ps, TOK_DOT) || p_check(ps, TOK_SLASH)) {
            char sep = p_match(ps, TOK_SLASH) ? '/' : '.';
            if (sep == '.') p_match(ps, TOK_DOT);
            Token* sub = p_consume(ps, TOK_NAME);
            const char* seg = sub->text;
            while (p_match(ps, TOK_MINUS)) {
                Token* s2 = p_consume(ps, TOK_NAME);
                seg = arena_sprintf("%s-%s", seg, s2->text);
            }
            mod_name = arena_sprintf("%s%c%s", mod_name, sep, seg);
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
        while (p_match(ps, TOK_MINUS)) {
            Token* s2 = p_consume(ps, TOK_NAME);
            mod_name = arena_sprintf("%s-%s", mod_name, s2->text);
        }
        while (p_check(ps, TOK_DOT) || p_check(ps, TOK_SLASH)) {
            char sep = p_match(ps, TOK_SLASH) ? '/' : '.';
            if (sep == '.') p_match(ps, TOK_DOT);
            Token* sub = p_consume(ps, TOK_NAME);
            const char* seg = sub->text;
            while (p_match(ps, TOK_MINUS)) {
                Token* s2 = p_consume(ps, TOK_NAME);
                seg = arena_sprintf("%s-%s", seg, s2->text);
            }
            mod_name = arena_sprintf("%s%c%s", mod_name, sep, seg);
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

    if (p_check(ps, TOK_NAME) && strcmp(p_peek(ps)->text, "match") == 0) {
        int look = 1, depth = 0;
        bool ends_colon = false;
        while (true) {
            Token* lt = p_peek_at(ps, look);
            if (!lt || lt->kind == TOK_EOF || lt->kind == TOK_NEWLINE) break;
            if (lt->kind == TOK_LPAREN || lt->kind == TOK_LBRACK || lt->kind == TOK_LBRACE) depth++;
            else if ((lt->kind == TOK_RPAREN || lt->kind == TOK_RBRACK || lt->kind == TOK_RBRACE) && depth > 0) depth--;
            ends_colon = (depth == 0 && lt->kind == TOK_COLON);
            look++;
        }
        TokKind nk = p_peek_at(ps, 1) ? p_peek_at(ps, 1)->kind : TOK_EOF;
        if (ends_colon && nk != TOK_ASSIGN && nk != TOK_COLON && nk != TOK_DOT) {
            ps->pos++;
            Expr* subj = parse_expr(ps);
            p_consume(ps, TOK_COLON);
            p_consume(ps, TOK_NEWLINE);
            skip_newlines(ps);
            p_consume(ps, TOK_INDENT);
            MatchCase* cases = NULL; int ccnt = 0, ccap = 0;
            while (p_check(ps, TOK_NAME) && strcmp(p_peek(ps)->text, "case") == 0) {
                ps->pos++;
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
    TY_TASK,
    TY_REF,
    TY_MUT_REF,
    TY_ADDR,
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

typedef enum { PM_BORROW = 0, PM_MUT_BORROW = 1, PM_MOVE = 2 } ParamMode;
typedef enum { OWN_UNINIT = 0, OWN_ALIVE = 1, OWN_MOVED = 2, OWN_MAYBE_MOVED = 3, OWN_BORROWED = 4 } OwnState;

typedef struct VarSym {
    const char* name;
    Type* type;
    bool is_global;
    bool is_nonlocal;
    bool is_captured;
    bool is_param;
    bool is_nonneg;
    const char* bound_arrs[4];
    int bound_arr_count;
    OwnState own_state;
    bool is_borrowed_view;
    const char* borrow_owner;
    const char* drop_flag_llvm;
    bool is_stack_owned;
    const char* llvm_name;
    SrcLoc loc;
} VarSym;

struct FuncInfo {
    const char* name;
    const char* llvm_name;
    ModuleInfo* mod;
    ClassInfo* owner_cls;
    FuncInfo* parent_fn;
    FuncInfo* orig_fn;
    Stmt* ast;
    Expr* lambda_ast;
    Type* type;
    const char** param_names;
    Type** param_types;
    Expr** param_defaults;
    ParamMode* param_modes;
    int param_count;
    Type* ret_type;
    bool infer_ret;
    bool can_raise;
    bool is_generic;
    FuncInfo** mono_specs;
    int mono_spec_count;
    bool checking_body;
    bool body_checked;
    Type* inferred_ret_type;
    bool saw_void_return;
    bool is_method;
    bool is_static;
    bool is_classmethod;
    bool is_property;
    bool is_abstract;
    bool is_async;
    bool is_generator;
    bool is_lambda;
    bool is_extern;
    const char* extern_sym;
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
    const char* real_path;
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
static const char* g_exe_dir = ".";
static const char* g_import_dirs[64];
static int g_import_dir_count = 0;

typedef struct {
    const char* scope_rp; /* NULL for root/global */
    const char* alias;
    const char* target_dir;
} ModMapEntry;

static ModMapEntry g_mod_maps[512];
static int g_mod_map_count = 0;

static void add_mod_map(const char* spec) {
    if (g_mod_map_count >= 512) return;
    const char* eq = strchr(spec, '=');
    if (!eq || eq == spec) return;
    const char* left = arena_strndup(spec, (size_t)(eq - spec));
    const char* target = eq + 1;
    const char* col = strchr(left, ':');
    const char* scope = NULL;
    const char* alias = left;
    if (col) {
        scope = arena_strndup(left, (size_t)(col - left));
        alias = col + 1;
        char rp[4096];
        if (realpath(scope, rp)) scope = arena_strdup(rp);
    }
    char trp[4096];
    if (realpath(target, trp)) target = arena_strdup(trp);
    g_mod_maps[g_mod_map_count++] = (ModMapEntry){scope, alias, target};
}

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
static bool type_is_ptr(Type* t) {
    return t && (t->kind == TY_REF || t->kind == TY_MUT_REF || t->kind == TY_ADDR);
}

static Type* type_deref_all(Type* t) {
    while (type_is_ptr(t)) t = t->elem;
    return t;
}

static bool type_ptr_is_const(Type* t) {
    while (type_is_ptr(t)) {
        if (t->kind == TY_REF) return true;
        t = t->elem;
    }
    return false;
}

static bool type_has_ref_d(Type* t, int depth) {
    if (!t || depth > 16) return false;
    if (t->kind == TY_REF || t->kind == TY_MUT_REF) return true;
    if (t->kind == TY_OPTIONAL || t->kind == TY_LIST || t->kind == TY_SET ||
        t->kind == TY_TASK || t->kind == TY_ADDR) {
        return type_has_ref_d(t->elem, depth + 1);
    }
    if (t->kind == TY_DICT) return type_has_ref_d(t->key, depth + 1) || type_has_ref_d(t->val, depth + 1);
    if (t->kind == TY_TUPLE) {
        for (int i = 0; i < t->tuple_count; i++) {
            if (type_has_ref_d(t->tuple_elems[i], depth + 1)) return true;
        }
    }
    return false;
}

static bool type_has_ref(Type* t) {
    return type_has_ref_d(t, 0);
}

static bool type_is_heap_ref(Type* t) {
    if (!t) return false;
    if (t->kind == TY_STR || t->kind == TY_BYTES || t->kind == TY_BIGINT || t->kind == TY_FILE ||
        t->kind == TY_LIST || t->kind == TY_SET || t->kind == TY_DICT || t->kind == TY_TASK ||
        t->kind == TY_REF || t->kind == TY_MUT_REF || t->kind == TY_ADDR ||
        t->kind == TY_FUNC) return true;
    if (t->kind == TY_CLASS) return !t->cls->is_value_type;
    if (t->kind == TY_OPTIONAL) return type_is_heap_ref(t->elem);
    return false;
}

static bool type_is_owned_d(Type* t, int depth) {
    if (!t || depth > 16) return false;
    switch (t->kind) {
        case TY_STR: case TY_BYTES: case TY_BIGINT: case TY_FILE:
        case TY_LIST: case TY_SET: case TY_DICT: case TY_TASK:
            return true;
        case TY_OPTIONAL:
            return type_is_owned_d(t->elem, depth + 1);
        case TY_TUPLE:
            for (int i = 0; i < t->tuple_count; i++) {
                if (type_is_owned_d(t->tuple_elems[i], depth + 1)) return true;
            }
            return false;
        case TY_CLASS:
            if (!t->cls->is_value_type) return true;
            for (int i = 0; i < t->cls->field_count; i++) {
                if (type_is_owned_d(t->cls->fields[i].type, depth + 1)) return true;
            }
            return false;
        default:
            return false;
    }
}

static bool type_is_owned(Type* t) {
    return type_is_owned_d(t, 0);
}

static bool type_eq(Type* a, Type* b) {
    if (a == b) return true;
    if (!a || !b || a->kind != b->kind) return false;
    switch (a->kind) {
        case TY_OPTIONAL:
        case TY_LIST:
        case TY_SET:
        case TY_TASK:
        case TY_REF:
        case TY_MUT_REF:
        case TY_ADDR:
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
        case TY_TASK: return arena_sprintf("Task[%s]", type_str(t->elem));
        case TY_REF: return arena_sprintf("ref[%s]", type_str(t->elem));
        case TY_MUT_REF: return arena_sprintf("mut_ref[%s]", type_str(t->elem));
        case TY_ADDR: return arena_sprintf("addr[%s]", type_str(t->elem));
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

static Type* make_task_type(Type* elem) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_TASK;
    t->elem = elem ? elem : g_ty_void;
    t->mangled = arena_sprintf("Task_%s", t->elem->mangled);
    return intern_type(t);
}

static Type* make_ref_type(Type* elem) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_REF;
    t->elem = elem;
    t->mangled = arena_sprintf("Ref_%s", elem->mangled);
    return intern_type(t);
}

static Type* make_mut_ref_type(Type* elem) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_MUT_REF;
    t->elem = elem;
    t->mangled = arena_sprintf("MutRef_%s", elem->mangled);
    return intern_type(t);
}

static Type* make_addr_type(Type* elem) {
    Type* t = (Type*)arena_alloc(sizeof(Type));
    t->kind = TY_ADDR;
    t->elem = elem;
    t->mangled = arena_sprintf("Addr_%s", elem->mangled);
    return intern_type(t);
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
    if (dst->kind == TY_REF && src->kind == TY_MUT_REF && type_eq(dst->elem, src->elem)) return true;
    if (dst->kind == TY_ADDR && (src->kind == TY_REF || src->kind == TY_MUT_REF || src->kind == TY_ADDR) &&
        type_eq(dst->elem, src->elem)) return true;
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

static ModuleInfo* try_load_module_by_name(const char* mod_name, const char* from_dir);
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
            Type* el = resolve_type_expr(mod, te->args[0]);
            if (type_has_ref(el)) error_at(te->loc, "cannot store borrowed reference '%s' in list; use 'addr[...]' for raw pointers", type_str(el));
            return make_list_type(el);
        }
        if (strcmp(n, "set") == 0 || strcmp(n, "Set") == 0) {
            if (te->arg_count != 1) error_at(te->loc, "set[...] requires 1 type argument");
            Type* el = resolve_type_expr(mod, te->args[0]);
            if (type_has_ref(el)) error_at(te->loc, "cannot store borrowed reference '%s' in set; use 'addr[...]' for raw pointers", type_str(el));
            return make_set_type(el);
        }
        if (strcmp(n, "Task") == 0) {
            if (te->arg_count != 1) error_at(te->loc, "Task[...] requires 1 type argument");
            return make_task_type(resolve_type_expr(mod, te->args[0]));
        }
        if (strcmp(n, "ref") == 0) {
            if (te->arg_count != 1) error_at(te->loc, "ref[T] requires 1 type argument");
            return make_ref_type(resolve_type_expr(mod, te->args[0]));
        }
        if (strcmp(n, "mut_ref") == 0) {
            if (te->arg_count != 1) error_at(te->loc, "mut_ref[T] requires 1 type argument");
            return make_mut_ref_type(resolve_type_expr(mod, te->args[0]));
        }
        if (strcmp(n, "addr") == 0) {
            if (te->arg_count != 1) error_at(te->loc, "addr[T] requires 1 type argument");
            return make_addr_type(resolve_type_expr(mod, te->args[0]));
        }
        if (strcmp(n, "dict") == 0 || strcmp(n, "Dict") == 0) {
            if (te->arg_count != 2) error_at(te->loc, "dict[K, V] requires 2 type arguments");
            Type* kt = resolve_type_expr(mod, te->args[0]);
            Type* vt = resolve_type_expr(mod, te->args[1]);
            if (type_has_ref(kt) || type_has_ref(vt)) error_at(te->loc, "cannot store borrowed reference in dict; use 'addr[...]' for raw pointers");
            return make_dict_type(kt, vt);
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

static const char* expr_extern_sym(Expr** decos, int cnt, const char* def_name) {
    for (int i = 0; i < cnt; i++) {
        Expr* d = decos[i];
        if (d->kind == EXPR_VAR && strcmp(d->var.name, "extern") == 0) return def_name;
        if (d->kind == EXPR_CALL && d->call.callee->kind == EXPR_VAR &&
            strcmp(d->call.callee->var.name, "extern") == 0) {
            if (d->call.arg_count == 1 && d->call.args[0]->kind == EXPR_STR) {
                return d->call.args[0]->str_lit.text;
            }
            return def_name;
        }
    }
    return NULL;
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

typedef struct {
    VarSym* sym;
    Type* orig_type;
    bool orig_nonneg;
    int orig_bound_count;
} NarrowEntry;

static Type* check_expr(ModuleInfo* mod, FuncInfo* fn, Expr* e, Type* hint);
static void check_stmts(ModuleInfo* mod, FuncInfo* fn, Stmt** stmts, int count);
static void check_func_body(ModuleInfo* mod, FuncInfo* fn);
static VarSym* resolve_var(ModuleInfo* mod, FuncInfo* fn, const char* name, FuncInfo** out_owner_fn);
static int empty_container_kind(Expr* rhs);
static void apply_cond_narrowing(ModuleInfo* mod, FuncInfo* fn, Expr* cond, bool truthy,
                                 Stmt** body, int body_cnt, NarrowEntry* saved, int* saved_cnt, int max_saved);
static void restore_narrowing(NarrowEntry* saved, int saved_cnt);
static FuncInfo* instantiate_generic_fn(FuncInfo* tmpl, Type** arg_types, int argc, SrcLoc call_loc);

static Expr* clone_expr(Expr* e);
static Stmt* clone_stmt(Stmt* s);

static Stmt** clone_stmts(Stmt** stmts, int count) {
    if (!stmts || count <= 0) return NULL;
    Stmt** out = (Stmt**)arena_alloc(sizeof(Stmt*) * (size_t)count);
    for (int i = 0; i < count; i++) out[i] = clone_stmt(stmts[i]);
    return out;
}

static Expr* clone_expr(Expr* e) {
    if (!e) return NULL;
    Expr* c = (Expr*)arena_alloc(sizeof(Expr));
    *c = *e;
    c->ty = NULL;
    c->is_nonneg = false;
    c->is_inbounds = false;
    switch (e->kind) {
        case EXPR_FSTRING:
            if (e->fstr.count > 0) {
                c->fstr.parts = (FStrPart*)arena_alloc(sizeof(FStrPart) * (size_t)e->fstr.count);
                for (int i = 0; i < e->fstr.count; i++) {
                    c->fstr.parts[i] = e->fstr.parts[i];
                    c->fstr.parts[i].expr = clone_expr(e->fstr.parts[i].expr);
                }
            }
            break;
        case EXPR_BINOP:
            c->binop.left = clone_expr(e->binop.left);
            c->binop.right = clone_expr(e->binop.right);
            break;
        case EXPR_UNOP:
            c->unop.operand = clone_expr(e->unop.operand);
            break;
        case EXPR_AWAIT:
            c->await_e.expr = clone_expr(e->await_e.expr);
            break;
        case EXPR_CALL:
            c->call.callee = clone_expr(e->call.callee);
            c->call.resolved_fn = NULL;
            if (e->call.arg_count > 0) {
                c->call.args = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)e->call.arg_count);
                for (int i = 0; i < e->call.arg_count; i++) c->call.args[i] = clone_expr(e->call.args[i]);
                if (e->call.kw_names) {
                    c->call.kw_names = (const char**)arena_alloc(sizeof(char*) * (size_t)e->call.arg_count);
                    memcpy(c->call.kw_names, e->call.kw_names, sizeof(char*) * (size_t)e->call.arg_count);
                }
            }
            break;
        case EXPR_ATTR:
            c->attr.obj = clone_expr(e->attr.obj);
            break;
        case EXPR_INDEX:
            c->index.obj = clone_expr(e->index.obj);
            c->index.index = clone_expr(e->index.index);
            break;
        case EXPR_SLICE:
            c->slice.obj = clone_expr(e->slice.obj);
            c->slice.start = clone_expr(e->slice.start);
            c->slice.stop = clone_expr(e->slice.stop);
            c->slice.step = clone_expr(e->slice.step);
            break;
        case EXPR_LIST:
        case EXPR_TUPLE:
        case EXPR_SET:
            if (e->seq.count > 0) {
                c->seq.items = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)e->seq.count);
                for (int i = 0; i < e->seq.count; i++) c->seq.items[i] = clone_expr(e->seq.items[i]);
            }
            break;
        case EXPR_DICT:
            if (e->dict.count > 0) {
                c->dict.keys = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)e->dict.count);
                c->dict.vals = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)e->dict.count);
                for (int i = 0; i < e->dict.count; i++) {
                    c->dict.keys[i] = clone_expr(e->dict.keys[i]);
                    c->dict.vals[i] = clone_expr(e->dict.vals[i]);
                }
            }
            break;
        case EXPR_LISTCOMP:
        case EXPR_SETCOMP:
        case EXPR_DICTCOMP:
        case EXPR_GENEXPR:
            c->comp.elt = clone_expr(e->comp.elt);
            c->comp.val = clone_expr(e->comp.val);
            if (e->comp.gen_count > 0) {
                c->comp.gens = (CompGen*)arena_alloc(sizeof(CompGen) * (size_t)e->comp.gen_count);
                for (int i = 0; i < e->comp.gen_count; i++) {
                    c->comp.gens[i] = e->comp.gens[i];
                    c->comp.gens[i].iter = clone_expr(e->comp.gens[i].iter);
                    if (e->comp.gens[i].if_count > 0) {
                        c->comp.gens[i].ifs = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)e->comp.gens[i].if_count);
                        for (int f = 0; f < e->comp.gens[i].if_count; f++) {
                            c->comp.gens[i].ifs[f] = clone_expr(e->comp.gens[i].ifs[f]);
                        }
                    }
                }
            }
            break;
        case EXPR_TERNARY:
            c->ternary.cond = clone_expr(e->ternary.cond);
            c->ternary.then_e = clone_expr(e->ternary.then_e);
            c->ternary.else_e = clone_expr(e->ternary.else_e);
            break;
        case EXPR_WALRUS:
            c->walrus.val = clone_expr(e->walrus.val);
            break;
        case EXPR_LAMBDA:
            c->lambda.body = clone_expr(e->lambda.body);
            break;
        default:
            break;
    }
    return c;
}

static Stmt* clone_stmt(Stmt* s) {
    if (!s) return NULL;
    Stmt* c = (Stmt*)arena_alloc(sizeof(Stmt));
    *c = *s;
    switch (s->kind) {
        case STMT_RETURN:
            c->ret.expr = clone_expr(s->ret.expr);
            break;
        case STMT_EXPR:
        case STMT_YIELD:
        case STMT_YIELD_FROM:
            c->expr_s.expr = clone_expr(s->expr_s.expr);
            break;
        case STMT_ASSIGN:
            c->assign.target = clone_expr(s->assign.target);
            c->assign.value = clone_expr(s->assign.value);
            break;
        case STMT_MULTI_ASSIGN:
            if (s->multi_assign.target_count > 0) {
                c->multi_assign.targets = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)s->multi_assign.target_count);
                for (int i = 0; i < s->multi_assign.target_count; i++) c->multi_assign.targets[i] = clone_expr(s->multi_assign.targets[i]);
            }
            c->multi_assign.value = clone_expr(s->multi_assign.value);
            break;
        case STMT_AUG_ASSIGN:
            c->aug_assign.target = clone_expr(s->aug_assign.target);
            c->aug_assign.value = clone_expr(s->aug_assign.value);
            break;
        case STMT_IF: {
            int bc = s->if_s.branch_count;
            c->if_s.conds = (Expr**)arena_alloc(sizeof(Expr*) * (size_t)bc);
            c->if_s.bodies = (Stmt***)arena_alloc(sizeof(Stmt**) * (size_t)bc);
            c->if_s.body_counts = (int*)arena_alloc(sizeof(int) * (size_t)bc);
            for (int b = 0; b < bc; b++) {
                c->if_s.conds[b] = clone_expr(s->if_s.conds[b]);
                c->if_s.body_counts[b] = s->if_s.body_counts[b];
                c->if_s.bodies[b] = clone_stmts(s->if_s.bodies[b], s->if_s.body_counts[b]);
            }
            c->if_s.else_body = clone_stmts(s->if_s.else_body, s->if_s.else_count);
            break;
        }
        case STMT_WHILE:
            c->while_s.cond = clone_expr(s->while_s.cond);
            c->while_s.body = clone_stmts(s->while_s.body, s->while_s.body_count);
            c->while_s.else_body = clone_stmts(s->while_s.else_body, s->while_s.else_count);
            break;
        case STMT_FOR:
            c->for_s.iter = clone_expr(s->for_s.iter);
            c->for_s.body = clone_stmts(s->for_s.body, s->for_s.body_count);
            c->for_s.else_body = clone_stmts(s->for_s.else_body, s->for_s.else_count);
            break;
        case STMT_DEF:
            if (s->def_s.param_count > 0) {
                c->def_s.params = (Param*)arena_alloc(sizeof(Param) * (size_t)s->def_s.param_count);
                for (int i = 0; i < s->def_s.param_count; i++) {
                    c->def_s.params[i] = s->def_s.params[i];
                    c->def_s.params[i].default_val = clone_expr(s->def_s.params[i].default_val);
                }
            }
            c->def_s.body = clone_stmts(s->def_s.body, s->def_s.body_count);
            break;
        case STMT_TRY:
            c->try_s.try_body = clone_stmts(s->try_s.try_body, s->try_s.try_count);
            if (s->try_s.handler_count > 0) {
                c->try_s.handlers = (ExceptHandler*)arena_alloc(sizeof(ExceptHandler) * (size_t)s->try_s.handler_count);
                for (int h = 0; h < s->try_s.handler_count; h++) {
                    c->try_s.handlers[h] = s->try_s.handlers[h];
                    c->try_s.handlers[h].body = clone_stmts(s->try_s.handlers[h].body, s->try_s.handlers[h].body_count);
                }
            }
            c->try_s.else_body = clone_stmts(s->try_s.else_body, s->try_s.else_count);
            c->try_s.finally_body = clone_stmts(s->try_s.finally_body, s->try_s.finally_count);
            break;
        case STMT_RAISE:
            c->raise_s.exc = clone_expr(s->raise_s.exc);
            break;
        case STMT_ASSERT:
            c->assert_s.cond = clone_expr(s->assert_s.cond);
            c->assert_s.msg = clone_expr(s->assert_s.msg);
            break;
        case STMT_WITH:
            c->with_s.ctx = clone_expr(s->with_s.ctx);
            c->with_s.body = clone_stmts(s->with_s.body, s->with_s.body_count);
            break;
        case STMT_DEL:
            c->del_s.target = clone_expr(s->del_s.target);
            break;
        case STMT_MATCH:
            c->match_s.subject = clone_expr(s->match_s.subject);
            if (s->match_s.case_count > 0) {
                c->match_s.cases = (MatchCase*)arena_alloc(sizeof(MatchCase) * (size_t)s->match_s.case_count);
                for (int k = 0; k < s->match_s.case_count; k++) {
                    c->match_s.cases[k] = s->match_s.cases[k];
                    c->match_s.cases[k].pattern = clone_expr(s->match_s.cases[k].pattern);
                    c->match_s.cases[k].body = clone_stmts(s->match_s.cases[k].body, s->match_s.cases[k].body_count);
                }
            }
            break;
        default:
            break;
    }
    return c;
}

static void var_add_bound(VarSym* v, const char* arr_name) {
    if (!v || !arr_name) return;
    for (int i = 0; i < v->bound_arr_count; i++) {
        if (strcmp(v->bound_arrs[i], arr_name) == 0) return;
    }
    if (v->bound_arr_count < 4) v->bound_arrs[v->bound_arr_count++] = arr_name;
}

static bool var_is_bound(VarSym* v, const char* arr_name) {
    if (!v || !arr_name) return false;
    for (int i = 0; i < v->bound_arr_count; i++) {
        if (strcmp(v->bound_arrs[i], arr_name) == 0) return true;
    }
    return false;
}

static const char* expr_len_var(Expr* e) {
    if (!e || e->kind != EXPR_CALL || e->call.callee->kind != EXPR_VAR) return NULL;
    if (strcmp(e->call.callee->var.name, "len") != 0 || e->call.arg_count != 1) return NULL;
    if (e->call.args[0]->kind == EXPR_VAR) return e->call.args[0]->var.name;
    return NULL;
}

static bool stmts_mutate_seq_len(Stmt** stmts, int count, const char* vname) {
    if (!vname) return true;
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        switch (s->kind) {
            case STMT_ASSIGN:
                if (s->assign.target->kind == EXPR_VAR && strcmp(s->assign.target->var.name, vname) == 0) return true;
                break;
            case STMT_MULTI_ASSIGN:
                for (int k = 0; k < s->multi_assign.target_count; k++) {
                    if (s->multi_assign.targets[k]->kind == EXPR_VAR &&
                        strcmp(s->multi_assign.targets[k]->var.name, vname) == 0) return true;
                }
                break;
            case STMT_AUG_ASSIGN:
                if (s->aug_assign.target->kind == EXPR_VAR && strcmp(s->aug_assign.target->var.name, vname) == 0) return true;
                break;
            case STMT_DEL:
                if (s->del_s.target->kind == EXPR_INDEX && s->del_s.target->index.obj->kind == EXPR_VAR &&
                    strcmp(s->del_s.target->index.obj->var.name, vname) == 0) return true;
                break;
            case STMT_EXPR:
                if (s->expr_s.expr && s->expr_s.expr->kind == EXPR_CALL &&
                    s->expr_s.expr->call.callee->kind == EXPR_ATTR &&
                    s->expr_s.expr->call.callee->attr.obj->kind == EXPR_VAR &&
                    strcmp(s->expr_s.expr->call.callee->attr.obj->var.name, vname) == 0) {
                    const char* mn = s->expr_s.expr->call.callee->attr.attr;
                    if (strcmp(mn, "append") == 0 || strcmp(mn, "extend") == 0 ||
                        strcmp(mn, "insert") == 0 || strcmp(mn, "pop") == 0 ||
                        strcmp(mn, "remove") == 0 || strcmp(mn, "clear") == 0) return true;
                }
                break;
            case STMT_IF:
                for (int b = 0; b < s->if_s.branch_count; b++) {
                    if (stmts_mutate_seq_len(s->if_s.bodies[b], s->if_s.body_counts[b], vname)) return true;
                }
                if (stmts_mutate_seq_len(s->if_s.else_body, s->if_s.else_count, vname)) return true;
                break;
            case STMT_WHILE:
                if (stmts_mutate_seq_len(s->while_s.body, s->while_s.body_count, vname)) return true;
                if (stmts_mutate_seq_len(s->while_s.else_body, s->while_s.else_count, vname)) return true;
                break;
            case STMT_FOR:
                if (stmts_mutate_seq_len(s->for_s.body, s->for_s.body_count, vname)) return true;
                if (stmts_mutate_seq_len(s->for_s.else_body, s->for_s.else_count, vname)) return true;
                break;
            case STMT_TRY:
                if (stmts_mutate_seq_len(s->try_s.try_body, s->try_s.try_count, vname)) return true;
                for (int h = 0; h < s->try_s.handler_count; h++) {
                    if (stmts_mutate_seq_len(s->try_s.handlers[h].body, s->try_s.handlers[h].body_count, vname)) return true;
                }
                if (stmts_mutate_seq_len(s->try_s.else_body, s->try_s.else_count, vname)) return true;
                if (stmts_mutate_seq_len(s->try_s.finally_body, s->try_s.finally_count, vname)) return true;
                break;
            case STMT_WITH:
                if (stmts_mutate_seq_len(s->with_s.body, s->with_s.body_count, vname)) return true;
                break;
            case STMT_MATCH:
                for (int k = 0; k < s->match_s.case_count; k++) {
                    if (stmts_mutate_seq_len(s->match_s.cases[k].body, s->match_s.cases[k].body_count, vname)) return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

static bool expr_is_nonneg(ModuleInfo* mod, FuncInfo* fn, Expr* e) {
    if (!e) return false;
    if (e->ty && type_is_unsigned_int(e->ty)) return true;
    switch (e->kind) {
        case EXPR_INT:
            return !e->int_lit.is_big && e->int_lit.ival >= 0;
        case EXPR_VAR: {
            VarSym* v = resolve_var(mod, fn, e->var.name, NULL);
            return v && v->is_nonneg;
        }
        case EXPR_CALL:
            return e->call.callee->kind == EXPR_VAR && strcmp(e->call.callee->var.name, "len") == 0;
        case EXPR_BINOP:
            if (strcmp(e->binop.op, "+") == 0 || strcmp(e->binop.op, "*") == 0 ||
                strcmp(e->binop.op, "//") == 0 || strcmp(e->binop.op, "%") == 0) {
                return expr_is_nonneg(mod, fn, e->binop.left) && expr_is_nonneg(mod, fn, e->binop.right);
            }
            return false;
        default:
            return false;
    }
}

static Type* ensure_func_ret_type(FuncInfo* callee, SrcLoc loc) {
    if (!callee) return g_ty_void;
    if (callee->infer_ret && !callee->body_checked) {
        if (callee->checking_body) {
            if (callee->inferred_ret_type) return callee->is_async ? make_task_type(callee->inferred_ret_type) : callee->inferred_ret_type;
            error_at(loc, "cannot infer return type of recursive function '%s' before a return statement; add '-> ReturnType'", callee->name);
        }
        check_func_body(callee->mod, callee);
    }
    return callee->is_async ? make_task_type(callee->ret_type) : callee->ret_type;
}

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
        "iter", "next", "super", "map", "filter", "bigint", "val",
        "unbin", "sys_argv",
        "i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64",
        NULL
    };
    for (int i = 0; builtins[i]; i++) {
        if (strcmp(n, builtins[i]) == 0) return true;
    }
    return false;
}

static bool expr_is_ptr_attr(const char* an) {
    return an && (strcmp(an, "ref") == 0 || strcmp(an, "mut_ref") == 0 || strcmp(an, "addr") == 0);
}

static bool is_val_builtin_call(ModuleInfo* mod, FuncInfo* fn, Expr* e) {
    if (!e || e->kind != EXPR_CALL || !e->call.callee || e->call.callee->kind != EXPR_VAR) return false;
    if (strcmp(e->call.callee->var.name, "val") != 0) return false;
    if (fn) {
        for (int i = 0; i < g_all_func_count; i++) {
            if (g_all_funcs[i]->parent_fn == fn && strcmp(g_all_funcs[i]->name, "val") == 0) return false;
        }
    }
    if (mod && find_func_in_mod(mod, "val")) return false;
    return true;
}

static bool expr_is_addressable(ModuleInfo* mod, FuncInfo* fn, Expr* e) {
    if (!e) return false;
    if (e->kind == EXPR_VAR) {
        if (resolve_var(mod, fn, e->var.name, NULL) != NULL) return true;
        if (strcmp(e->var.name, "self") == 0 && fn && fn->owner_cls && !fn->is_static) return true;
        return false;
    }
    if (e->kind == EXPR_ATTR) {
        if (e->is_ptr_take) return false;
        Type* ot = e->attr.obj ? e->attr.obj->ty : NULL;
        if (!ot) return false;
        if (ot->kind == TY_MODULE) return find_global_in_mod(ot->mod, e->attr.attr) != NULL;
        Type* base = type_deref_all(ot);
        if (base->kind == TY_CLASS) {
            if (find_field_in_class(base->cls, e->attr.attr) != NULL) {
                if (!type_is_ptr(ot) && base->cls->is_value_type) {
                    return expr_is_addressable(mod, fn, e->attr.obj);
                }
                return true;
            }
            return find_class_attr(base->cls, e->attr.attr) != NULL;
        }
        return false;
    }
    if (e->kind == EXPR_INDEX) {
        Type* ot = e->index.obj ? type_deref_all(e->index.obj->ty) : NULL;
        return ot && ot->kind == TY_LIST;
    }
    if (e->kind == EXPR_CALL && (e->is_ptr_take || is_val_builtin_call(mod, fn, e))) {
        return true;
    }
    return false;
}

static bool expr_is_mutable_place(ModuleInfo* mod, FuncInfo* fn, Expr* e) {
    if (!e) return true;
    if (e->kind == EXPR_VAR) {
        VarSym* v = resolve_var(mod, fn, e->var.name, NULL);
        if (v && v->type && type_ptr_is_const(v->type)) return false;
        return true;
    }
    if (e->kind == EXPR_ATTR) {
        if (e->is_ptr_take) return false;
        Type* ot = e->attr.obj ? e->attr.obj->ty : NULL;
        if (ot && type_ptr_is_const(ot)) return false;
        if (ot && !type_is_ptr(ot) && ot->kind == TY_CLASS && ot->cls->is_value_type) {
            return expr_is_mutable_place(mod, fn, e->attr.obj);
        }
        return true;
    }
    if (e->kind == EXPR_INDEX) {
        Type* ot = e->index.obj ? e->index.obj->ty : NULL;
        if (ot && type_ptr_is_const(ot)) return false;
        return true;
    }
    if (e->kind == EXPR_CALL && (e->is_ptr_take || is_val_builtin_call(mod, fn, e))) {
        int pidx = (e->call.arg_count == 2) ? 1 : 0;
        if (pidx >= e->call.arg_count) return false;
        Expr* pe = e->call.args[pidx];
        Type* pt = pe->ty;
        int lvl = (e->call.arg_count == 2 && e->call.args[0]->kind == EXPR_INT) ? (int)e->call.args[0]->int_lit.ival : 1;
        for (int k = 0; k < lvl && pt; k++) {
            if (pt->kind == TY_REF) return false;
            if (!type_is_ptr(pt)) return false;
            pt = pt->elem;
        }
        return true;
    }
    return true;
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
            VarSym* lv = resolve_var(mod, fn, cg->targets[0], NULL);
            if (lv) {
                int ac = cg->iter->call.arg_count;
                bool nn = (ac == 1) ||
                          (expr_is_nonneg(mod, fn, cg->iter->call.args[0]) &&
                           (ac == 2 || expr_is_nonneg(mod, fn, cg->iter->call.args[2])));
                lv->is_nonneg = nn;
                const char* b_arr = NULL;
                if (ac == 1) b_arr = expr_len_var(cg->iter->call.args[0]);
                else if (ac == 2 && cg->iter->call.args[0]->kind == EXPR_INT && cg->iter->call.args[0]->int_lit.ival >= 0) {
                    b_arr = expr_len_var(cg->iter->call.args[1]);
                }
                if (b_arr) var_add_bound(lv, b_arr);
            }
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

static bool call_has_kwargs(Expr* call) {
    if (!call || call->kind != EXPR_CALL || !call->call.kw_names) return false;
    for (int i = 0; i < call->call.arg_count; i++) {
        if (call->call.kw_names[i]) return true;
    }
    return false;
}

static void reject_unexpected_kwargs(Expr* call, const char* fn_desc) {
    if (!call || call->kind != EXPR_CALL || !call->call.kw_names) return;
    for (int i = 0; i < call->call.arg_count; i++) {
        if (call->call.kw_names[i]) {
            error_at(call->call.args[i]->loc, "%s got an unexpected keyword argument '%s'", fn_desc, call->call.kw_names[i]);
        }
    }
}

static void normalize_call_args(Expr* call, const char** pnames, Expr** pdefaults,
                                int param_count, int start_p, const char* fn_desc) {
    if (!call_has_kwargs(call)) return;
    int max_args = param_count - start_p;
    if (max_args > 64) error_at(call->loc, "%s has too many parameters for keyword call", fn_desc);
    Expr* slots[64];
    for (int i = 0; i < max_args; i++) slots[i] = NULL;

    bool seen_kw = false;
    int argc = call->call.arg_count;
    for (int a = 0; a < argc; a++) {
        const char* kw = call->call.kw_names[a];
        if (!kw) {
            if (seen_kw) error_at(call->call.args[a]->loc, "positional argument follows keyword argument");
            if (a >= max_args) error_at(call->call.args[a]->loc, "%s takes at most %d positional arguments", fn_desc, max_args);
            slots[a] = call->call.args[a];
        } else {
            seen_kw = true;
            int found = -1;
            for (int p = start_p; p < param_count; p++) {
                if (pnames[p] && strcmp(pnames[p], kw) == 0) { found = p - start_p; break; }
            }
            if (found < 0) {
                error_at(call->call.args[a]->loc, "%s got an unexpected keyword argument '%s'", fn_desc, kw);
            }
            if (slots[found] != NULL) {
                error_at(call->call.args[a]->loc, "%s got multiple values for argument '%s'", fn_desc, kw);
            }
            slots[found] = call->call.args[a];
        }
    }
    for (int i = 0; i < max_args; i++) {
        if (!slots[i]) {
            Expr* def_e = pdefaults ? pdefaults[start_p + i] : NULL;
            if (!def_e) {
                error_at(call->loc, "%s missing required argument '%s'", fn_desc, pnames[start_p + i]);
            }
            slots[i] = clone_expr(def_e);
        }
    }
    call->call.args = max_args ? (Expr**)arena_alloc(sizeof(Expr*) * (size_t)max_args) : NULL;
    if (max_args) memcpy(call->call.args, slots, sizeof(Expr*) * (size_t)max_args);
    call->call.arg_count = max_args;
    call->call.kw_names = NULL;
}

static void normalize_dataclass_args(Expr* call, ClassInfo* ci) {
    if (!call_has_kwargs(call)) return;
    const char* fnames[64];
    Expr* fdefs[64];
    int fc = ci->field_count < 64 ? ci->field_count : 64;
    for (int i = 0; i < fc; i++) {
        fnames[i] = ci->fields[i].name;
        fdefs[i] = ci->fields[i].default_expr;
    }
    normalize_call_args(call, fnames, fdefs, fc, 0, arena_sprintf("%s()", ci->name));
}

static bool is_mutating_container_method(Type* ot, const char* mname);

static Type* check_method_call(ModuleInfo* mod, FuncInfo* fn, Expr* call, Expr* obj_e, const char* mname, Type* obj_ty, Type* hint) {
    (void)hint;
    int argc = call->call.arg_count;
    Expr** args = call->call.args;

    bool recv_is_const_ptr = type_ptr_is_const(obj_ty);
    if (type_is_ptr(obj_ty)) {
        if (obj_ty->kind == TY_OPTIONAL) {
            error_at(call->loc, "cannot call method '%s' on optional pointer '%s'; narrow with 'is not None' first", mname, type_str(obj_ty));
        }
        obj_ty = type_deref_all(obj_ty);
        if (obj_ty->kind == TY_OPTIONAL) {
            error_at(call->loc, "cannot call method '%s' on optional type '%s'; narrow with 'is not None' first", mname, type_str(obj_ty));
        }
    }

    if (obj_ty->kind != TY_CLASS && obj_ty->kind != TY_MODULE) {
        reject_unexpected_kwargs(call, arena_sprintf("%s.%s()", type_str(obj_ty), mname));
    }
    if (recv_is_const_ptr && (obj_ty->kind == TY_LIST || obj_ty->kind == TY_SET || obj_ty->kind == TY_DICT || obj_ty->kind == TY_FILE)) {
        if (is_mutating_container_method(obj_ty, mname)) {
            error_at(call->loc, "cannot call mutating method '%s.%s()' through immutable reference", type_str(obj_ty), mname);
        }
    }

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
        if (strcmp(mname, "copy") == 0) {
            if (argc != 0) error_at(call->loc, "str.copy() takes 0 arguments");
            return g_ty_str;
        }
        error_at(call->loc, "unknown str method '%s'", mname);
    }

    if (obj_ty->kind == TY_BYTES || obj_ty->kind == TY_BIGINT || obj_ty->kind == TY_TUPLE) {
        if (strcmp(mname, "copy") == 0) {
            if (argc != 0) error_at(call->loc, "%s.copy() takes 0 arguments", type_str(obj_ty));
            return obj_ty;
        }
        error_at(call->loc, "unknown %s method '%s'", type_str(obj_ty), mname);
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
            call->call.resolved_fn = target_m;
            normalize_call_args(call, target_m->param_names, target_m->param_defaults, target_m->param_count, 1, arena_sprintf("super().%s()", mname));
            args = call->call.args; argc = call->call.arg_count;
            int expected_args = target_m->param_count - 1;
            int min_sa = expected_args;
            for (int i = target_m->param_count - 1; i >= 1; i--) {
                if (target_m->param_defaults[i]) min_sa--; else break;
            }
            if (argc < min_sa || argc > expected_args) error_at(call->loc, "super().%s() expects %d..%d arguments, got %d", mname, min_sa, expected_args, argc);
            for (int i = 0; i < argc; i++) {
                Type* at = check_expr(mod, fn, args[i], target_m->param_types[i + 1]);
                if (!type_assignable(target_m->param_types[i + 1], at)) {
                    error_at(args[i]->loc, "argument %d of super().%s(): expected '%s', got '%s'",
                             i + 1, mname, type_str(target_m->param_types[i + 1]), type_str(at));
                }
            }
            return ensure_func_ret_type(target_m, call->loc);
        }

        FuncInfo* m = find_method_in_class(cls, mname);
        if (!m && strcmp(mname, "copy") == 0 && argc == 0) {
            reject_unexpected_kwargs(call, arena_sprintf("%s.copy()", cls->name));
            return obj_ty;
        }
        if (!m) error_at(call->loc, "class '%s' has no method '%s'", cls->name, mname);
        int start_p = m->is_static ? 0 : 1;
        normalize_call_args(call, m->param_names, m->param_defaults, m->param_count, start_p, arena_sprintf("%s.%s()", cls->name, mname));
        args = call->call.args; argc = call->call.arg_count;
        int max_args = m->param_count - start_p;
        int min_args = max_args;
        for (int i = m->param_count - 1; i >= start_p; i--) {
            if (m->param_defaults[i]) min_args--;
            else break;
        }
        if (argc < min_args || argc > max_args) {
            error_at(call->loc, "method '%s.%s()' expects %d..%d arguments, got %d", cls->name, mname, min_args, max_args, argc);
        }
        if (m->is_generic) {
            Type* arg_tys[64];
            for (int i = 0; i < argc && i < 64; i++) {
                Param* pr = &m->ast->def_s.params[i + start_p];
                Type* ph = pr->type_ann ? resolve_type_expr(m->mod, pr->type_ann) : NULL;
                arg_tys[i] = check_expr(mod, fn, args[i], ph);
            }
            FuncInfo* spec = instantiate_generic_fn(m, arg_tys, argc, call->loc);
            call->call.resolved_fn = spec;
            return ensure_func_ret_type(spec, call->loc);
        }
        for (int i = 0; i < argc; i++) {
            Type* pt = m->param_types[i + start_p];
            Type* at = check_expr(mod, fn, args[i], pt);
            if (!type_assignable(pt, at)) {
                error_at(args[i]->loc, "argument %d of '%s.%s()': expected '%s', got '%s'",
                         i + 1, cls->name, mname, type_str(pt), type_str(at));
            }
        }
        return ensure_func_ret_type(m, call->loc);
    }

    if (obj_ty->kind == TY_MODULE) {
        ModuleInfo* mmod = obj_ty->mod;
        FuncInfo* mf = find_func_in_mod(mmod, mname);
        if (mf) {
            normalize_call_args(call, mf->param_names, mf->param_defaults, mf->param_count, 0, arena_sprintf("%s.%s()", mmod->name, mname));
            args = call->call.args; argc = call->call.arg_count;
            if (mf->is_generic) {
                int max_a = mf->param_count;
                int min_a = max_a;
                for (int i = max_a - 1; i >= 0; i--) {
                    if (mf->param_defaults[i]) min_a--; else break;
                }
                if (argc < min_a || argc > max_a) {
                    error_at(call->loc, "%s.%s() expects %d..%d args, got %d", mmod->name, mname, min_a, max_a, argc);
                }
                Type* arg_tys[64];
                for (int i = 0; i < argc && i < 64; i++) {
                    Param* pr = &mf->ast->def_s.params[i];
                    Type* ph = pr->type_ann ? resolve_type_expr(mf->mod, pr->type_ann) : NULL;
                    arg_tys[i] = check_expr(mod, fn, args[i], ph);
                }
                FuncInfo* spec = instantiate_generic_fn(mf, arg_tys, argc, call->loc);
                call->call.resolved_fn = spec;
                return ensure_func_ret_type(spec, call->loc);
            }
            int max_a = mf->param_count;
            int min_a = max_a;
            for (int i = max_a - 1; i >= 0; i--) {
                if (mf->param_defaults[i]) min_a--; else break;
            }
            if (argc < min_a || argc > max_a) {
                error_at(call->loc, "%s.%s() expects %d..%d args, got %d", mmod->name, mname, min_a, max_a, argc);
            }
            for (int i = 0; i < argc; i++) {
                Type* at = check_expr(mod, fn, args[i], mf->param_types[i]);
                if (!type_assignable(mf->param_types[i], at)) error_at(args[i]->loc, "argument type mismatch");
            }
            return ensure_func_ret_type(mf, call->loc);
        }
        ClassInfo* mci = find_class_in_mod(mmod, mname);
        if (mci) {
            FuncInfo* init_fn = find_method_in_class(mci, "__init__");
            if (init_fn) {
                normalize_call_args(call, init_fn->param_names, init_fn->param_defaults, init_fn->param_count, 1, arena_sprintf("%s.%s()", mmod->name, mname));
                args = call->call.args; argc = call->call.arg_count;
                int max_a = init_fn->param_count - 1;
                int min_a = max_a;
                for (int i = init_fn->param_count - 1; i >= 1; i--) {
                    if (init_fn->param_defaults[i]) min_a--; else break;
                }
                if (argc < min_a || argc > max_a) error_at(call->loc, "%s.%s() expects %d..%d arguments, got %d", mmod->name, mname, min_a, max_a, argc);
                for (int i = 0; i < argc; i++) check_expr(mod, fn, args[i], init_fn->param_types[i + 1]);
            } else if (mci->is_dataclass) {
                normalize_dataclass_args(call, mci);
                args = call->call.args; argc = call->call.arg_count;
                for (int i = 0; i < argc && i < mci->field_count; i++) check_expr(mod, fn, args[i], mci->fields[i].type);
            } else {
                reject_unexpected_kwargs(call, arena_sprintf("%s.%s()", mmod->name, mname));
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
            if (fi) {
                if (fi->is_generic) error_at(e->loc, "generic function '%s' cannot be used as a first-class value without parameter annotations", n);
                ensure_func_ret_type(fi, e->loc); res = fi->type; break;
            }
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
        case EXPR_AWAIT: {
            Type* th = hint ? make_task_type(hint) : NULL;
            Type* et = check_expr(mod, fn, e->await_e.expr, th);
            if (et->kind != TY_TASK) error_at(e->loc, "await requires Task[T], got '%s'", type_str(et));
            res = et->elem;
            break;
        }
        case EXPR_BINOP: {
            const char* op = e->binop.op;
            if (strcmp(op, "and") == 0 || strcmp(op, "or") == 0) {
                Type* lt = check_expr(mod, fn, e->binop.left, NULL);
                NarrowEntry nsaved[16];
                int ncnt = 0;
                apply_cond_narrowing(mod, fn, e->binop.left, strcmp(op, "and") == 0, NULL, 0, nsaved, &ncnt, 16);
                Type* rt = check_expr(mod, fn, e->binop.right, lt);
                restore_narrowing(nsaved, ncnt);
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
            if (lt->kind == TY_ADDR && type_is_int(rt) && (strcmp(op, "+") == 0 || strcmp(op, "-") == 0)) {
                res = lt; break;
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
            NarrowEntry nsaved[16];
            int ncnt = 0;
            apply_cond_narrowing(mod, fn, e->ternary.cond, true, NULL, 0, nsaved, &ncnt, 16);
            Type* branch_hint = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
            Type* tt = NULL;
            Type* et = NULL;
            if ((!branch_hint) && (empty_container_kind(e->ternary.then_e) != 0 || e->ternary.then_e->kind == EXPR_NONE)) {
                restore_narrowing(nsaved, ncnt);
                et = check_expr(mod, fn, e->ternary.else_e, NULL);
                apply_cond_narrowing(mod, fn, e->ternary.cond, true, NULL, 0, nsaved, &ncnt, 16);
                tt = check_expr(mod, fn, e->ternary.then_e, (et->kind == TY_NONE_LIT) ? NULL : et);
                restore_narrowing(nsaved, ncnt);
            } else {
                tt = check_expr(mod, fn, e->ternary.then_e, branch_hint);
                restore_narrowing(nsaved, ncnt);
                ncnt = 0;
                apply_cond_narrowing(mod, fn, e->ternary.cond, false, NULL, 0, nsaved, &ncnt, 16);
                Type* e_hint = (tt->kind == TY_NONE_LIT) ? branch_hint : tt;
                et = check_expr(mod, fn, e->ternary.else_e, e_hint);
                restore_narrowing(nsaved, ncnt);
            }
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
            Type* lhint = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
            if (e->seq.count == 0) {
                if (lhint && lhint->kind == TY_LIST) { res = lhint; break; }
                error_at(e->loc, "cannot infer type of empty list '[]'; add a type annotation like list[int]");
            }
            Type* ehint = (lhint && lhint->kind == TY_LIST) ? lhint->elem : NULL;
            Type* first = check_expr(mod, fn, e->seq.items[0], ehint);
            if (ehint && ehint->kind == TY_OPTIONAL && first->kind == TY_NONE_LIT) first = ehint;
            for (int i = 1; i < e->seq.count; i++) {
                Type* it = check_expr(mod, fn, e->seq.items[i], first);
                if (first->kind == TY_NONE_LIT && it->kind != TY_NONE_LIT) {
                    first = make_optional_type(it);
                } else if (first->kind != TY_NONE_LIT && first->kind != TY_OPTIONAL && it->kind == TY_NONE_LIT) {
                    first = make_optional_type(first);
                } else if (!type_assignable(first, it)) {
                    if (type_assignable(it, first)) first = it;
                    else error_at(e->seq.items[i]->loc, "list element type '%s' does not match '%s'", type_str(it), type_str(first));
                }
            }
            if (first->kind == TY_NONE_LIT) error_at(e->loc, "cannot infer element type of list containing only None; add a type annotation");
            if (type_has_ref(first)) error_at(e->loc, "cannot store borrowed reference '%s' in list; use 'addr[...]' for raw pointers", type_str(first));
            res = make_list_type(first);
            break;
        }
        case EXPR_TUPLE: {
            Type* thint = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
            Type** ts = e->seq.count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)e->seq.count) : NULL;
            for (int i = 0; i < e->seq.count; i++) {
                Type* eh = (thint && thint->kind == TY_TUPLE && i < thint->tuple_count) ? thint->tuple_elems[i] : NULL;
                ts[i] = check_expr(mod, fn, e->seq.items[i], eh);
            }
            res = make_tuple_type(ts, e->seq.count);
            break;
        }
        case EXPR_SET: {
            Type* shint = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
            Type* ehint = (shint && shint->kind == TY_SET) ? shint->elem : NULL;
            Type* first = check_expr(mod, fn, e->seq.items[0], ehint);
            for (int i = 1; i < e->seq.count; i++) {
                Type* it = check_expr(mod, fn, e->seq.items[i], first);
                if (!type_assignable(first, it)) error_at(e->seq.items[i]->loc, "set element type mismatch");
            }
            if (type_has_ref(first)) error_at(e->loc, "cannot store borrowed reference '%s' in set; use 'addr[...]' for raw pointers", type_str(first));
            res = make_set_type(first);
            break;
        }
        case EXPR_DICT: {
            Type* dhint = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
            if (e->dict.count == 0) {
                if (dhint && dhint->kind == TY_DICT) { res = dhint; break; }
                error_at(e->loc, "cannot infer type of empty dict '{}'; add a type annotation like dict[str, int]");
            }
            Type* kh = (dhint && dhint->kind == TY_DICT) ? dhint->key : NULL;
            Type* vh = (dhint && dhint->kind == TY_DICT) ? dhint->val : NULL;
            Type* kt = check_expr(mod, fn, e->dict.keys[0], kh);
            Type* vt = check_expr(mod, fn, e->dict.vals[0], vh);
            if (vh && vh->kind == TY_OPTIONAL && vt->kind == TY_NONE_LIT) vt = vh;
            for (int i = 1; i < e->dict.count; i++) {
                Type* k2 = check_expr(mod, fn, e->dict.keys[i], kt);
                Type* v2 = check_expr(mod, fn, e->dict.vals[i], vt);
                if (!type_assignable(kt, k2)) error_at(e->dict.keys[i]->loc, "dict key type mismatch");
                if (vt->kind == TY_NONE_LIT && v2->kind != TY_NONE_LIT) vt = make_optional_type(v2);
                else if (vt->kind != TY_NONE_LIT && vt->kind != TY_OPTIONAL && v2->kind == TY_NONE_LIT) vt = make_optional_type(vt);
                else if (!type_assignable(vt, v2)) error_at(e->dict.vals[i]->loc, "dict value type mismatch");
            }
            if (vt->kind == TY_NONE_LIT) error_at(e->loc, "cannot infer dict value type from None; add a type annotation");
            if (type_has_ref(kt) || type_has_ref(vt)) error_at(e->loc, "cannot store borrowed reference in dict; use 'addr[...]' for raw pointers");
            res = make_dict_type(kt, vt);
            break;
        }
        case EXPR_LISTCOMP:
        case EXPR_GENEXPR: {
            Type* lhint = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
            check_comp_gens(mod, fn, e->comp.gens, e->comp.gen_count);
            Type* eh = (lhint && lhint->kind == TY_LIST) ? lhint->elem : NULL;
            Type* et = check_expr(mod, fn, e->comp.elt, eh);
            if (eh && eh->kind == TY_OPTIONAL && et->kind == TY_NONE_LIT) et = eh;
            res = make_list_type(et);
            break;
        }
        case EXPR_SETCOMP: {
            Type* shint = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
            check_comp_gens(mod, fn, e->comp.gens, e->comp.gen_count);
            Type* eh = (shint && shint->kind == TY_SET) ? shint->elem : NULL;
            Type* et = check_expr(mod, fn, e->comp.elt, eh);
            res = make_set_type(et);
            break;
        }
        case EXPR_DICTCOMP: {
            Type* dhint = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
            check_comp_gens(mod, fn, e->comp.gens, e->comp.gen_count);
            Type* kh = (dhint && dhint->kind == TY_DICT) ? dhint->key : NULL;
            Type* vh = (dhint && dhint->kind == TY_DICT) ? dhint->val : NULL;
            Type* kt = check_expr(mod, fn, e->comp.elt, kh);
            Type* vt = check_expr(mod, fn, e->comp.val, vh);
            if (vh && vh->kind == TY_OPTIONAL && vt->kind == TY_NONE_LIT) vt = vh;
            res = make_dict_type(kt, vt);
            break;
        }
        case EXPR_INDEX: {
            Type* raw_ot = check_expr(mod, fn, e->index.obj, NULL);
            Type* ptr_ot = raw_ot;
            while (ptr_ot->kind == TY_REF || ptr_ot->kind == TY_MUT_REF) ptr_ot = ptr_ot->elem;
            Type* ot = type_deref_all(raw_ot);
            if (ptr_ot->kind == TY_ADDR) {
                Type* it = check_expr(mod, fn, e->index.index, g_ty_i64);
                if (!type_is_int(it)) error_at(e->index.index->loc, "pointer index must be int");
                res = ptr_ot->elem;
            } else if (ot->kind == TY_LIST) {
                Type* it = check_expr(mod, fn, e->index.index, g_ty_i64);
                if (!type_is_int(it)) error_at(e->index.index->loc, "list index must be int");
                e->index.index->is_nonneg = expr_is_nonneg(mod, fn, e->index.index);
                if (e->index.obj->kind == EXPR_VAR && e->index.index->kind == EXPR_VAR &&
                    var_is_bound(resolve_var(mod, fn, e->index.index->var.name, NULL), e->index.obj->var.name)) {
                    e->is_inbounds = true;
                    e->index.index->is_nonneg = true;
                }
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
                error_at(e->loc, "type '%s' is not subscriptable", type_str(raw_ot));
            }
            break;
        }
        case EXPR_SLICE: {
            Type* ot = type_deref_all(check_expr(mod, fn, e->slice.obj, NULL));
            if (e->slice.start && !type_is_int(check_expr(mod, fn, e->slice.start, g_ty_i64))) {
                error_at(e->slice.start->loc, "slice start must be int");
            }
            if (e->slice.stop && !type_is_int(check_expr(mod, fn, e->slice.stop, g_ty_i64))) {
                error_at(e->slice.stop->loc, "slice stop must be int");
            }
            if (e->slice.step && !type_is_int(check_expr(mod, fn, e->slice.step, g_ty_i64))) {
                error_at(e->slice.step->loc, "slice step must be int");
            }
            if (ot->kind != TY_LIST && ot->kind != TY_STR && ot->kind != TY_BYTES) error_at(e->loc, "cannot slice type '%s'", type_str(ot));
            res = ot;
            break;
        }
        case EXPR_ATTR: {
            Type* ot = check_expr(mod, fn, e->attr.obj, NULL);
            const char* an = e->attr.attr;
            bool has_real_attr = false;
            Type* base_ot = type_deref_all(ot);
            if (base_ot->kind == TY_CLASS) {
                ClassInfo* cls = base_ot->cls;
                if (find_field_in_class(cls, an) || find_class_attr(cls, an)) has_real_attr = true;
                else {
                    FuncInfo* prop = find_method_in_class(cls, an);
                    if (prop && prop->is_property) has_real_attr = true;
                }
            } else if (ot->kind == TY_MODULE) {
                if (find_global_in_mod(ot->mod, an) || find_class_in_mod(ot->mod, an) || find_func_in_mod(ot->mod, an)) {
                    has_real_attr = true;
                }
            }
            if (!has_real_attr && expr_is_ptr_attr(an)) {
                if (!expr_is_addressable(mod, fn, e->attr.obj)) {
                    error_at(e->loc, "cannot take '.%s' of non-addressable expression", an);
                }
                if (strcmp(an, "mut_ref") == 0 && !expr_is_mutable_place(mod, fn, e->attr.obj)) {
                    error_at(e->loc, "cannot take '.mut_ref' through immutable reference");
                }
                e->is_ptr_take = true;
                if (strcmp(an, "ref") == 0) res = make_ref_type(ot);
                else if (strcmp(an, "mut_ref") == 0) res = make_mut_ref_type(ot);
                else res = make_addr_type(ot);
                break;
            }
            if (ot->kind == TY_OPTIONAL) {
                error_at(e->loc, "cannot access attribute '%s' on optional type '%s'; narrow with 'is not None' or check", an, type_str(ot));
            }
            if (base_ot->kind == TY_OPTIONAL) {
                error_at(e->loc, "cannot access attribute '%s' on optional type '%s'; narrow with 'is not None' or check", an, type_str(base_ot));
            }
            if ((base_ot->kind == TY_STR || base_ot->kind == TY_BYTES) && strcmp(an, "bin") == 0) {
                res = make_addr_type(g_ty_u8);
                break;
            }
            if (base_ot->kind == TY_CLASS) {
                ClassInfo* cls = base_ot->cls;
                FieldInfo* fi = find_field_in_class(cls, an);
                if (fi) { res = fi->type; break; }
                FieldInfo* ca = find_class_attr(cls, an);
                if (ca) { res = ca->type; break; }
                FuncInfo* prop = find_method_in_class(cls, an);
                if (prop && prop->is_property) { res = ensure_func_ret_type(prop, e->loc); break; }
                error_at(e->loc, "class '%s' has no attribute '%s'", cls->name, an);
            } else if (ot->kind == TY_MODULE) {
                VarSym* gv = find_global_in_mod(ot->mod, an);
                if (gv) { res = gv->type; break; }
                ClassInfo* mci = find_class_in_mod(ot->mod, an);
                if (mci) { res = mci->type; break; }
                FuncInfo* mfi = find_func_in_mod(ot->mod, an);
                if (mfi) { ensure_func_ret_type(mfi, e->loc); res = mfi->type; break; }
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
            if (rh && !type_assignable(rh, rty)) {
                error_at(e->loc, "lambda return type '%s' incompatible with expected '%s'", type_str(rty), type_str(rh));
            }
            lfn->ret_type = rh ? rh : rty;
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
                    Type* raw_at = check_expr(mod, fn, args[0], NULL);
                    Type* at = type_deref_all(raw_at);
                    if (at->kind != TY_LIST && at->kind != TY_SET && at->kind != TY_DICT &&
                        at->kind != TY_STR && at->kind != TY_BYTES && at->kind != TY_TUPLE) {
                        error_at(args[0]->loc, "len() unsupported for type '%s'", type_str(raw_at));
                    }
                    res = g_ty_i64; break;
                }
                if (is_val_builtin_call(mod, fn, e)) {
                    reject_unexpected_kwargs(e, "val()");
                    if (argc < 1 || argc > 2) error_at(e->loc, "val() takes 1 or 2 arguments");
                    int lvl = 1;
                    Expr* ptr_arg = args[0];
                    if (argc == 2) {
                        if (args[0]->kind != EXPR_INT || args[0]->int_lit.is_big || args[0]->int_lit.ival < 1) {
                            error_at(args[0]->loc, "val() level must be a positive integer literal");
                        }
                        check_expr(mod, fn, args[0], g_ty_i64);
                        lvl = (int)args[0]->int_lit.ival;
                        ptr_arg = args[1];
                    }
                    Type* pt = check_expr(mod, fn, ptr_arg, NULL);
                    for (int k = 0; k < lvl; k++) {
                        if (pt->kind == TY_OPTIONAL) {
                            error_at(ptr_arg->loc, "cannot dereference optional pointer '%s'; narrow with 'is not None' first", type_str(pt));
                        }
                        if (!type_is_ptr(pt)) {
                            error_at(ptr_arg->loc, "cannot dereference non-pointer type '%s'", type_str(pt));
                        }
                        pt = pt->elem;
                    }
                    e->is_ptr_take = true;
                    res = pt;
                    break;
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
                if (strcmp(cname, "unbin") == 0) {
                    if (argc != 1) error_at(e->loc, "unbin() takes 1 argument");
                    Type* pt = check_expr(mod, fn, args[0], NULL);
                    if (!type_is_ptr(pt) || (pt->elem->kind != TY_U8 && pt->elem->kind != TY_I8)) {
                        error_at(args[0]->loc, "unbin() expects addr[u8] or addr[i8], got '%s'", type_str(pt));
                    }
                    res = g_ty_str; break;
                }
                if (strcmp(cname, "sys_argv") == 0) {
                    if (argc != 0) error_at(e->loc, "sys_argv() takes 0 arguments");
                    res = make_list_type(g_ty_str); break;
                }
                if (strcmp(cname, "hex") == 0 || strcmp(cname, "bin") == 0 || strcmp(cname, "oct") == 0) {
                    if (argc != 1) error_at(e->loc, "%s() takes 1 argument", cname);
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    if (!type_is_int(at) && !(strcmp(cname, "hex") == 0 && type_is_ptr(at))) {
                        error_at(args[0]->loc, "%s() expects int", cname);
                    }
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
                if (strcmp(cname, "sorted") == 0) {
                    if (argc < 1 || argc > 2) error_at(e->loc, "sorted() takes 1 or 2 arguments");
                    if (e->call.kw_names && e->call.kw_names[0]) {
                        error_at(args[0]->loc, "sorted() first argument must be positional");
                    }
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    Type* el = iter_element_type(at, args[0]->loc);
                    if (argc == 2) {
                        if (e->call.kw_names && e->call.kw_names[1] && strcmp(e->call.kw_names[1], "key") != 0) {
                            error_at(args[1]->loc, "sorted() unsupported keyword argument '%s'", e->call.kw_names[1]);
                        }
                        Type* kh_params[1] = {el};
                        Type khint = {0}; khint.kind = TY_FUNC; khint.func_param_count = 1; khint.func_params = kh_params; khint.func_ret = NULL;
                        Type* kf = check_expr(mod, fn, args[1], &khint);
                        if (kf->kind != TY_FUNC || kf->func_param_count != 1 || !type_assignable(kf->func_params[0], el)) {
                            error_at(args[1]->loc, "sorted() key function must accept '%s'", type_str(el));
                        }
                    }
                    e->call.kw_names = NULL;
                    res = make_list_type(el); break;
                }
                if (strcmp(cname, "reversed") == 0) {
                    if (argc != 1) error_at(e->loc, "reversed() takes 1 argument");
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    Type* el = iter_element_type(at, args[0]->loc);
                    res = make_list_type(el); break;
                }
                if (strcmp(cname, "map") == 0) {
                    if (argc != 2) error_at(e->loc, "map() takes 2 arguments");
                    Type* ch = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
                    Type* at = check_expr(mod, fn, args[1], NULL);
                    Type* el = iter_element_type(at, args[1]->loc);
                    Type* fh_params[1] = {el};
                    Type fhint = {0}; fhint.kind = TY_FUNC; fhint.func_param_count = 1; fhint.func_params = fh_params;
                    fhint.func_ret = (ch && ch->kind == TY_LIST) ? ch->elem : NULL;
                    Type* fty = check_expr(mod, fn, args[0], &fhint);
                    if (fty->kind != TY_FUNC || fty->func_param_count != 1 || !type_assignable(fty->func_params[0], el)) {
                        error_at(args[0]->loc, "map() function must accept '%s'", type_str(el));
                    }
                    res = make_list_type(fty->func_ret); break;
                }
                if (strcmp(cname, "filter") == 0) {
                    if (argc != 2) error_at(e->loc, "filter() takes 2 arguments");
                    Type* at = check_expr(mod, fn, args[1], NULL);
                    Type* el = iter_element_type(at, args[1]->loc);
                    Type* fhint = make_func_type(&el, 1, g_ty_bool);
                    Type* fty = check_expr(mod, fn, args[0], fhint);
                    if (fty->kind != TY_FUNC || fty->func_param_count != 1 || !type_assignable(fty->func_params[0], el)) {
                        error_at(args[0]->loc, "filter() function must accept '%s'", type_str(el));
                    }
                    res = make_list_type(el); break;
                }
                if (strcmp(cname, "list") == 0) {
                    Type* ch = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
                    if (argc == 0) {
                        if (ch && ch->kind == TY_LIST) { res = ch; break; }
                        error_at(e->loc, "cannot infer type of empty list(); add type annotation");
                    }
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    res = make_list_type(iter_element_type(at, args[0]->loc)); break;
                }
                if (strcmp(cname, "set") == 0) {
                    Type* ch = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
                    if (argc == 0) {
                        if (ch && ch->kind == TY_SET) { res = ch; break; }
                        error_at(e->loc, "cannot infer type of empty set(); add type annotation");
                    }
                    Type* at = check_expr(mod, fn, args[0], NULL);
                    res = make_set_type(iter_element_type(at, args[0]->loc)); break;
                }
                if (strcmp(cname, "dict") == 0) {
                    Type* ch = (hint && hint->kind == TY_OPTIONAL) ? hint->elem : hint;
                    if (argc == 0) {
                        if (ch && ch->kind == TY_DICT) { res = ch; break; }
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
                        normalize_call_args(e, init_fn->param_names, init_fn->param_defaults, init_fn->param_count, 1, arena_sprintf("%s()", ci->name));
                        args = e->call.args; argc = e->call.arg_count;
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
                        normalize_dataclass_args(e, ci);
                        args = e->call.args; argc = e->call.arg_count;
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
                    normalize_call_args(e, target_fn->param_names, target_fn->param_defaults, target_fn->param_count, 0, arena_sprintf("%s()", cname));
                    args = e->call.args; argc = e->call.arg_count;
                    int max_a = target_fn->param_count;
                    int min_a = max_a;
                    for (int i = max_a - 1; i >= 0; i--) {
                        if (target_fn->param_defaults[i]) min_a--; else break;
                    }
                    if (argc < min_a || argc > max_a) {
                        error_at(e->loc, "%s() expects %d..%d arguments, got %d", cname, min_a, max_a, argc);
                    }
                    if (target_fn->is_generic) {
                        Type* arg_tys[64];
                        for (int i = 0; i < argc && i < 64; i++) {
                            Param* pr = &target_fn->ast->def_s.params[i];
                            Type* ph = pr->type_ann ? resolve_type_expr(target_fn->mod, pr->type_ann) : NULL;
                            arg_tys[i] = check_expr(mod, fn, args[i], ph);
                        }
                        FuncInfo* spec = instantiate_generic_fn(target_fn, arg_tys, argc, e->loc);
                        e->call.resolved_fn = spec;
                        res = ensure_func_ret_type(spec, e->loc);
                        break;
                    }
                    for (int i = 0; i < argc; i++) {
                        Type* pt = target_fn->param_types[i];
                        Type* at = check_expr(mod, fn, args[i], pt);
                        if (!type_assignable(pt, at)) {
                            error_at(args[i]->loc, "arg %d of %s(): expected '%s', got '%s'", i + 1, cname, type_str(pt), type_str(at));
                        }
                    }
                    e->call.resolved_fn = target_fn;
                    res = ensure_func_ret_type(target_fn, e->loc);
                    break;
                }
            }

            Type* ct = check_expr(mod, fn, callee, NULL);
            if (ct->kind != TY_FUNC) error_at(e->loc, "expression of type '%s' is not callable", type_str(ct));
            reject_unexpected_kwargs(e, "callable");
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

    if (e->kind == EXPR_CALL) {
        reject_unexpected_kwargs(e, (e->call.callee->kind == EXPR_VAR) ? arena_sprintf("%s()", e->call.callee->var.name) : "call");
    }

    if (!res) res = g_ty_void;
    e->ty = res;
    return res;
}

static Type* resolve_param_type(ModuleInfo* mod, FuncInfo* ctx_fn, Param* pr, const char* fn_desc) {
    if (pr->type_ann) {
        Type* pt = resolve_type_expr(mod, pr->type_ann);
        if (pr->default_val) {
            Type* dt = check_expr(mod, ctx_fn, pr->default_val, pt);
            if (!type_assignable(pt, dt)) {
                error_at(pr->loc, "default value for parameter '%s' of %s has type '%s', expected '%s'",
                         pr->name, fn_desc, type_str(dt), type_str(pt));
            }
        }
        return pt;
    }
    if (pr->default_val) {
        Type* dt = check_expr(mod, ctx_fn, pr->default_val, NULL);
        if (!dt || dt->kind == TY_NONE_LIT) {
            error_at(pr->loc, "parameter '%s' of %s with default None requires a type annotation (e.g. '%s: T | None = None')",
                     pr->name, fn_desc, pr->name);
        }
        return dt;
    }
    error_at(pr->loc, "parameter '%s' of %s requires a type annotation or default value", pr->name, fn_desc);
    return NULL;
}

static bool def_has_unannotated_params(Stmt* def_s, bool is_method, bool is_static) {
    int start_p = (is_method && !is_static) ? 1 : 0;
    for (int i = start_p; i < def_s->def_s.param_count; i++) {
        Param* pr = &def_s->def_s.params[i];
        if (!pr->type_ann && !pr->default_val) return true;
    }
    return false;
}

static bool stmts_has_break(Stmt** b, int cnt) {
    for (int i = 0; i < cnt; i++) {
        Stmt* s = b[i];
        if (s->kind == STMT_BREAK) return true;
        if (s->kind == STMT_IF) {
            for (int k = 0; k < s->if_s.branch_count; k++) {
                if (stmts_has_break(s->if_s.bodies[k], s->if_s.body_counts[k])) return true;
            }
            if (stmts_has_break(s->if_s.else_body, s->if_s.else_count)) return true;
        } else if (s->kind == STMT_TRY) {
            if (stmts_has_break(s->try_s.try_body, s->try_s.try_count)) return true;
            for (int h = 0; h < s->try_s.handler_count; h++) {
                if (stmts_has_break(s->try_s.handlers[h].body, s->try_s.handlers[h].body_count)) return true;
            }
            if (stmts_has_break(s->try_s.else_body, s->try_s.else_count)) return true;
            if (stmts_has_break(s->try_s.finally_body, s->try_s.finally_count)) return true;
        } else if (s->kind == STMT_WITH) {
            if (stmts_has_break(s->with_s.body, s->with_s.body_count)) return true;
        } else if (s->kind == STMT_MATCH) {
            for (int k = 0; k < s->match_s.case_count; k++) {
                if (stmts_has_break(s->match_s.cases[k].body, s->match_s.cases[k].body_count)) return true;
            }
        }
    }
    return false;
}

static bool stmts_definitely_returns(Stmt** b, int cnt) {
    for (int i = 0; i < cnt; i++) {
        Stmt* s = b[i];
        switch (s->kind) {
            case STMT_RETURN:
            case STMT_RAISE:
                return true;
            case STMT_ASSERT:
                if (s->assert_s.cond && s->assert_s.cond->kind == EXPR_BOOL && !s->assert_s.cond->bool_lit.bval) {
                    return true;
                }
                break;
            case STMT_IF:
                if (s->if_s.else_count > 0) {
                    bool all = stmts_definitely_returns(s->if_s.else_body, s->if_s.else_count);
                    for (int k = 0; all && k < s->if_s.branch_count; k++) {
                        if (!stmts_definitely_returns(s->if_s.bodies[k], s->if_s.body_counts[k])) all = false;
                    }
                    if (all) return true;
                }
                break;
            case STMT_WHILE:
                if (s->while_s.cond && s->while_s.cond->kind == EXPR_BOOL && s->while_s.cond->bool_lit.bval &&
                    !stmts_has_break(s->while_s.body, s->while_s.body_count)) {
                    return true;
                }
                break;
            case STMT_WITH:
                if (stmts_definitely_returns(s->with_s.body, s->with_s.body_count)) return true;
                break;
            case STMT_TRY:
                if (s->try_s.finally_count > 0 && stmts_definitely_returns(s->try_s.finally_body, s->try_s.finally_count)) {
                    return true;
                }
                if (stmts_definitely_returns(s->try_s.try_body, s->try_s.try_count)) {
                    bool all_h = true;
                    for (int h = 0; h < s->try_s.handler_count; h++) {
                        if (!stmts_definitely_returns(s->try_s.handlers[h].body, s->try_s.handlers[h].body_count)) {
                            all_h = false; break;
                        }
                    }
                    if (all_h) return true;
                }
                break;
            case STMT_MATCH:
                if (s->match_s.case_count > 0 && !s->match_s.cases[s->match_s.case_count - 1].pattern) {
                    bool all_c = true;
                    for (int k = 0; k < s->match_s.case_count; k++) {
                        if (!stmts_definitely_returns(s->match_s.cases[k].body, s->match_s.cases[k].body_count)) {
                            all_c = false; break;
                        }
                    }
                    if (all_c) return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

static FuncInfo* instantiate_generic_fn(FuncInfo* tmpl, Type** arg_types, int argc, SrcLoc call_loc) {
    int start_p = (tmpl->is_method && !tmpl->is_static) ? 1 : 0;
    int max_a = tmpl->param_count - start_p;
    int min_a = max_a;
    for (int i = tmpl->param_count - 1; i >= start_p; i--) {
        if (tmpl->param_defaults[i]) min_a--;
        else break;
    }
    if (argc < min_a || argc > max_a) {
        error_at(call_loc, "%s() expects %d..%d arguments, got %d", tmpl->name, min_a, max_a, argc);
    }

    Type* concrete_params[64];
    if (tmpl->is_method && !tmpl->is_static) concrete_params[0] = tmpl->owner_cls->type;
    for (int i = start_p; i < tmpl->param_count && i < 64; i++) {
        Param* pr = &tmpl->ast->def_s.params[i];
        int aidx = i - start_p;
        if (pr->type_ann) {
            Type* pt = resolve_type_expr(tmpl->mod, pr->type_ann);
            if (aidx < argc && !type_assignable(pt, arg_types[aidx])) {
                error_at(call_loc, "arg %d of %s(): expected '%s', got '%s'",
                         aidx + 1, tmpl->name, type_str(pt), type_str(arg_types[aidx]));
            }
            concrete_params[i] = pt;
        } else if (aidx < argc) {
            if (arg_types[aidx]->kind == TY_NONE_LIT) {
                error_at(call_loc, "cannot pass None to unannotated parameter '%s' of '%s'", pr->name, tmpl->name);
            }
            concrete_params[i] = arg_types[aidx];
        } else {
            Type* dt = check_expr(tmpl->mod, tmpl->parent_fn, pr->default_val, NULL);
            if (!dt || dt->kind == TY_NONE_LIT) {
                error_at(pr->loc, "parameter '%s' of '%s' with default None requires a type annotation", pr->name, tmpl->name);
            }
            concrete_params[i] = dt;
        }
    }

    for (int s = 0; s < tmpl->mono_spec_count; s++) {
        FuncInfo* sp = tmpl->mono_specs[s];
        bool match = true;
        for (int i = 0; i < tmpl->param_count; i++) {
            if (!type_eq(sp->param_types[i], concrete_params[i])) { match = false; break; }
        }
        if (match) return sp;
    }

    FuncInfo* spec = (FuncInfo*)arena_alloc(sizeof(FuncInfo));
    spec->name = tmpl->name;
    spec->llvm_name = arena_sprintf("%s__m%d", tmpl->llvm_name, tmpl->mono_spec_count + 1);
    spec->mod = tmpl->mod;
    spec->owner_cls = tmpl->owner_cls;
    spec->parent_fn = tmpl->parent_fn;
    spec->ast = clone_stmt(tmpl->ast);
    spec->is_method = tmpl->is_method;
    spec->is_static = tmpl->is_static;
    spec->is_classmethod = tmpl->is_classmethod;
    spec->is_property = tmpl->is_property;
    spec->is_generator = tmpl->is_generator;
    spec->is_async = tmpl->is_async;
    spec->is_generic = false;
    spec->param_count = tmpl->param_count;
    spec->param_names = spec->param_count ? (const char**)arena_alloc(sizeof(char*) * (size_t)spec->param_count) : NULL;
    spec->param_types = spec->param_count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)spec->param_count) : NULL;
    spec->param_defaults = spec->param_count ? (Expr**)arena_alloc(sizeof(Expr*) * (size_t)spec->param_count) : NULL;
    for (int i = 0; i < spec->param_count; i++) {
        Param* pr = &spec->ast->def_s.params[i];
        spec->param_names[i] = pr->name;
        spec->param_types[i] = concrete_params[i];
        spec->param_defaults[i] = pr->default_val;
        add_local_in_fn(spec, pr->name, spec->param_types[i], true, pr->loc);
    }
    spec->ret_type = spec->ast->def_s.ret_type ? resolve_type_expr(spec->mod, spec->ast->def_s.ret_type) : g_ty_void;
    spec->infer_ret = (spec->ast->def_s.ret_type == NULL && strcmp(spec->name, "__init__") != 0);
    spec->type = make_func_type(spec->param_types, spec->param_count, spec->is_async ? make_task_type(spec->ret_type) : spec->ret_type);

    tmpl->mono_specs = (FuncInfo**)realloc(tmpl->mono_specs, sizeof(FuncInfo*) * (size_t)(tmpl->mono_spec_count + 1));
    tmpl->mono_specs[tmpl->mono_spec_count++] = spec;
    g_all_funcs = (FuncInfo**)realloc(g_all_funcs, sizeof(FuncInfo*) * (size_t)(g_all_func_count + 1));
    g_all_funcs[g_all_func_count++] = spec;

    check_func_body(spec->mod, spec);
    return spec;
}

static void check_func_body(ModuleInfo* mod, FuncInfo* fn) {
    if (!fn || fn->body_checked || fn->checking_body || !fn->ast || fn->is_generic || fn->is_extern) return;
    fn->checking_body = true;
    check_stmts(mod, fn, fn->ast->def_s.body, fn->ast->def_s.body_count);
    if (fn->infer_ret) {
        if (fn->inferred_ret_type) {
            fn->ret_type = fn->saw_void_return ? make_optional_type(fn->inferred_ret_type) : fn->inferred_ret_type;
        } else {
            fn->ret_type = g_ty_void;
        }
        fn->type = make_func_type(fn->param_types, fn->param_count, fn->ret_type);
    } else if (fn->ret_type->kind != TY_VOID && !fn->is_generator && !fn->is_abstract &&
               fn->ast->def_s.body_count > 0 &&
               !stmts_definitely_returns(fn->ast->def_s.body, fn->ast->def_s.body_count)) {
        error_at(fn->ast->loc, "function '%s' with return type '%s' does not return a value on all paths",
                 fn->name, type_str(fn->ret_type));
    }
    fn->checking_body = false;
    fn->body_checked = true;
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
    nf->is_generic = def_has_unannotated_params(s, false, false);
    const char* fdesc = arena_sprintf("'%s'", nf->name);
    if (!nf->is_generic) {
        for (int i = 0; i < nf->param_count; i++) {
            Param* p = &s->def_s.params[i];
            nf->param_names[i] = p->name;
            nf->param_types[i] = resolve_param_type(mod, parent, p, fdesc);
            nf->param_defaults[i] = p->default_val;
            add_local_in_fn(nf, p->name, nf->param_types[i], true, p->loc);
        }
    } else {
        for (int i = 0; i < nf->param_count; i++) {
            Param* p = &s->def_s.params[i];
            nf->param_names[i] = p->name;
            nf->param_defaults[i] = p->default_val;
        }
    }
    nf->ret_type = s->def_s.ret_type ? resolve_type_expr(mod, s->def_s.ret_type) : g_ty_void;
    nf->infer_ret = (s->def_s.ret_type == NULL);
    nf->type = nf->is_generic ? NULL : make_func_type(nf->param_types, nf->param_count, nf->ret_type);
    nf->is_generator = stmt_has_yield(s->def_s.body, s->def_s.body_count);
    g_all_funcs = (FuncInfo**)realloc(g_all_funcs, sizeof(FuncInfo*) * (size_t)(g_all_func_count + 1));
    g_all_funcs[g_all_func_count++] = nf;
    if (!nf->is_generic) {
        if (parent) add_local_in_fn(parent, nf->name, nf->type, false, s->loc);
        check_func_body(mod, nf);
        if (parent) {
            VarSym* pv = find_local_in_fn(parent, nf->name);
            if (pv) pv->type = nf->type;
        }
    }
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

static const char* extract_isinstance_var(ModuleInfo* mod, Expr* cond, ClassInfo** out_cls) {
    if (!cond || cond->kind != EXPR_CALL || cond->call.callee->kind != EXPR_VAR) return NULL;
    if (strcmp(cond->call.callee->var.name, "isinstance") != 0 || cond->call.arg_count != 2) return NULL;
    if (cond->call.args[0]->kind != EXPR_VAR || cond->call.args[1]->kind != EXPR_VAR) return NULL;
    ClassInfo* ci = find_class_in_mod(mod, cond->call.args[1]->var.name);
    if (!ci) return NULL;
    *out_cls = ci;
    return cond->call.args[0]->var.name;
}

static void save_narrow(VarSym* sym, NarrowEntry* saved, int* saved_cnt, int max_saved) {
    if (!sym || *saved_cnt >= max_saved) return;
    for (int i = 0; i < *saved_cnt; i++) {
        if (saved[i].sym == sym) return;
    }
    saved[*saved_cnt] = (NarrowEntry){sym, sym->type, sym->is_nonneg, sym->bound_arr_count};
    (*saved_cnt)++;
}

static void restore_narrowing(NarrowEntry* saved, int saved_cnt) {
    for (int i = saved_cnt - 1; i >= 0; i--) {
        saved[i].sym->type = saved[i].orig_type;
        saved[i].sym->is_nonneg = saved[i].orig_nonneg;
        saved[i].sym->bound_arr_count = saved[i].orig_bound_count;
    }
}

static void apply_cond_narrowing_pass(ModuleInfo* mod, FuncInfo* fn, Expr* cond, bool truthy,
                                      Stmt** body, int body_cnt, NarrowEntry* saved, int* saved_cnt, int max_saved) {
    if (!cond) return;
    if (cond->kind == EXPR_UNOP && strcmp(cond->unop.op, "not") == 0) {
        apply_cond_narrowing_pass(mod, fn, cond->unop.operand, !truthy, body, body_cnt, saved, saved_cnt, max_saved);
        return;
    }
    if (cond->kind == EXPR_BINOP) {
        const char* op = cond->binop.op;
        if (strcmp(op, "and") == 0 && truthy) {
            apply_cond_narrowing_pass(mod, fn, cond->binop.left, true, body, body_cnt, saved, saved_cnt, max_saved);
            apply_cond_narrowing_pass(mod, fn, cond->binop.right, true, body, body_cnt, saved, saved_cnt, max_saved);
            return;
        }
        if (strcmp(op, "or") == 0 && !truthy) {
            apply_cond_narrowing_pass(mod, fn, cond->binop.left, false, body, body_cnt, saved, saved_cnt, max_saved);
            apply_cond_narrowing_pass(mod, fn, cond->binop.right, false, body, body_cnt, saved, saved_cnt, max_saved);
            return;
        }
        bool is_not_none = false;
        const char* nvar = extract_is_none_var(cond, &is_not_none);
        if (nvar && ((is_not_none && truthy) || (!is_not_none && !truthy))) {
            VarSym* nsym = resolve_var(mod, fn, nvar, NULL);
            if (nsym && nsym->type && nsym->type->kind == TY_OPTIONAL) {
                save_narrow(nsym, saved, saved_cnt, max_saved);
                nsym->type = nsym->type->elem;
            }
            return;
        }
        if (truthy) {
            /* Check x >= 0, 0 <= x, x > -1, -1 < x */
            Expr* L = cond->binop.left;
            Expr* R = cond->binop.right;
            const char* nn_var = NULL;
            if (strcmp(op, ">=") == 0 && L->kind == EXPR_VAR && expr_is_nonneg(mod, fn, R)) nn_var = L->var.name;
            else if (strcmp(op, "<=") == 0 && R->kind == EXPR_VAR && expr_is_nonneg(mod, fn, L)) nn_var = R->var.name;
            else if (strcmp(op, ">") == 0 && L->kind == EXPR_VAR && R->kind == EXPR_INT && !R->int_lit.is_big && R->int_lit.ival >= -1) nn_var = L->var.name;
            else if (strcmp(op, "<") == 0 && R->kind == EXPR_VAR && L->kind == EXPR_INT && !L->int_lit.is_big && L->int_lit.ival >= -1) nn_var = R->var.name;
            if (nn_var) {
                VarSym* vs = resolve_var(mod, fn, nn_var, NULL);
                if (vs) {
                    save_narrow(vs, saved, saved_cnt, max_saved);
                    vs->is_nonneg = true;
                }
                return;
            }
            /* Check i < len(arr) or len(arr) > i when i is_nonneg */
            const char* idx_var = NULL;
            const char* arr_var = NULL;
            if (strcmp(op, "<") == 0 && L->kind == EXPR_VAR) {
                arr_var = expr_len_var(R);
                if (arr_var) idx_var = L->var.name;
            } else if (strcmp(op, ">") == 0 && R->kind == EXPR_VAR) {
                arr_var = expr_len_var(L);
                if (arr_var) idx_var = R->var.name;
            }
            if (idx_var && arr_var) {
                VarSym* vs = resolve_var(mod, fn, idx_var, NULL);
                if (vs && vs->is_nonneg && (!body || !stmts_mutate_seq_len(body, body_cnt, arr_var))) {
                    save_narrow(vs, saved, saved_cnt, max_saved);
                    var_add_bound(vs, arr_var);
                }
                return;
            }
        }
    }
    if (truthy) {
        ClassInfo* sub_cls = NULL;
        const char* ivar = extract_isinstance_var(mod, cond, &sub_cls);
        if (ivar && sub_cls) {
            VarSym* isym = resolve_var(mod, fn, ivar, NULL);
            if (isym && isym->type &&
                ((isym->type->kind == TY_CLASS && !isym->type->cls->is_value_type && !sub_cls->is_value_type &&
                  class_is_subclass(sub_cls, isym->type->cls)) ||
                 (isym->type->kind == TY_OPTIONAL && isym->type->elem->kind == TY_CLASS &&
                  !isym->type->elem->cls->is_value_type && !sub_cls->is_value_type &&
                  class_is_subclass(sub_cls, isym->type->elem->cls)))) {
                save_narrow(isym, saved, saved_cnt, max_saved);
                isym->type = sub_cls->type;
            }
        }
    }
}

static void apply_cond_narrowing(ModuleInfo* mod, FuncInfo* fn, Expr* cond, bool truthy,
                                 Stmt** body, int body_cnt, NarrowEntry* saved, int* saved_cnt, int max_saved) {
    /* Two passes handle reversed order like (i < len(arr) and i >= 0) */
    apply_cond_narrowing_pass(mod, fn, cond, truthy, body, body_cnt, saved, saved_cnt, max_saved);
    if (truthy) apply_cond_narrowing_pass(mod, fn, cond, truthy, body, body_cnt, saved, saved_cnt, max_saved);
}

static int empty_container_kind(Expr* rhs) {
    if (!rhs) return 0;
    if (rhs->kind == EXPR_LIST && rhs->seq.count == 0) return 1;
    if (rhs->kind == EXPR_DICT && rhs->dict.count == 0) return 3;
    if (rhs->kind == EXPR_CALL && rhs->call.callee->kind == EXPR_VAR && rhs->call.arg_count == 0) {
        const char* fn = rhs->call.callee->var.name;
        if (strcmp(fn, "list") == 0) return 1;
        if (strcmp(fn, "set") == 0) return 2;
        if (strcmp(fn, "dict") == 0) return 3;
    }
    return 0;
}

static Type* expr_match_container_hint(Type* t, int ckind) {
    if (!t) return NULL;
    if (t->kind == TY_OPTIONAL) t = t->elem;
    if (ckind == 1 && t->kind == TY_LIST) return t;
    if (ckind == 2 && t->kind == TY_SET) return t;
    if (ckind == 3 && t->kind == TY_DICT) return t;
    return NULL;
}

static Type* scan_expr_call_container_hint(ModuleInfo* mod, FuncInfo* fn, Expr* e, const char* vname, int ckind) {
    if (!e) return NULL;
    if (e->kind == EXPR_CALL) {
        if (e->call.callee->kind == EXPR_VAR) {
            FuncInfo* tf = find_func_in_mod(mod, e->call.callee->var.name);
            if (tf && !tf->is_generic) {
                for (int a = 0; a < e->call.arg_count && a < tf->param_count; a++) {
                    if (e->call.args[a]->kind == EXPR_VAR && strcmp(e->call.args[a]->var.name, vname) == 0) {
                        Type* m = expr_match_container_hint(tf->param_types[a], ckind);
                        if (m) return m;
                    }
                }
            }
        }
        for (int a = 0; a < e->call.arg_count; a++) {
            Type* r = scan_expr_call_container_hint(mod, fn, e->call.args[a], vname, ckind);
            if (r) return r;
        }
    } else if (e->kind == EXPR_TERNARY) {
        Type* r = scan_expr_call_container_hint(mod, fn, e->ternary.then_e, vname, ckind);
        if (r) return r;
        return scan_expr_call_container_hint(mod, fn, e->ternary.else_e, vname, ckind);
    } else if (e->kind == EXPR_BINOP) {
        Type* r = scan_expr_call_container_hint(mod, fn, e->binop.left, vname, ckind);
        if (r) return r;
        return scan_expr_call_container_hint(mod, fn, e->binop.right, vname, ckind);
    } else if (e->kind == EXPR_UNOP) {
        return scan_expr_call_container_hint(mod, fn, e->unop.operand, vname, ckind);
    }
    return NULL;
}

static Type* scan_empty_container_use(ModuleInfo* mod, FuncInfo* fn, const char* vname, int ckind, Stmt** stmts, int count) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        if (s->kind == STMT_RETURN && s->ret.expr && s->ret.expr->kind == EXPR_VAR &&
            strcmp(s->ret.expr->var.name, vname) == 0 && fn && !fn->infer_ret) {
            Type* m = expr_match_container_hint(fn->ret_type, ckind);
            if (m) return m;
        }
        if (s->kind == STMT_EXPR && s->expr_s.expr && s->expr_s.expr->kind == EXPR_CALL) {
            Expr* c = s->expr_s.expr;
            if (c->call.callee->kind == EXPR_ATTR && c->call.callee->attr.obj->kind == EXPR_VAR &&
                strcmp(c->call.callee->attr.obj->var.name, vname) == 0) {
                const char* mn = c->call.callee->attr.attr;
                if (ckind == 1) {
                    if (strcmp(mn, "append") == 0 && c->call.arg_count == 1) {
                        return make_list_type(check_expr(mod, fn, c->call.args[0], NULL));
                    }
                    if (strcmp(mn, "insert") == 0 && c->call.arg_count == 2) {
                        return make_list_type(check_expr(mod, fn, c->call.args[1], NULL));
                    }
                    if (strcmp(mn, "extend") == 0 && c->call.arg_count == 1) {
                        Type* it = check_expr(mod, fn, c->call.args[0], NULL);
                        return make_list_type(iter_element_type(it, c->call.args[0]->loc));
                    }
                } else if (ckind == 2) {
                    if (strcmp(mn, "add") == 0 && c->call.arg_count == 1) {
                        return make_set_type(check_expr(mod, fn, c->call.args[0], NULL));
                    }
                    if (strcmp(mn, "update") == 0 && c->call.arg_count == 1) {
                        Type* it = check_expr(mod, fn, c->call.args[0], NULL);
                        return make_set_type(iter_element_type(it, c->call.args[0]->loc));
                    }
                } else if (ckind == 3) {
                    if (strcmp(mn, "setdefault") == 0 && c->call.arg_count == 2) {
                        Type* kt = check_expr(mod, fn, c->call.args[0], NULL);
                        Type* vt = check_expr(mod, fn, c->call.args[1], NULL);
                        return make_dict_type(kt, vt);
                    }
                }
            }
            Type* r = scan_expr_call_container_hint(mod, fn, c, vname, ckind);
            if (r) return r;
        } else if (s->kind == STMT_ASSIGN) {
            if (ckind == 3 && s->assign.target->kind == EXPR_INDEX &&
                s->assign.target->index.obj->kind == EXPR_VAR &&
                strcmp(s->assign.target->index.obj->var.name, vname) == 0 && s->assign.value) {
                Type* kt = check_expr(mod, fn, s->assign.target->index.index, NULL);
                Type* vt = check_expr(mod, fn, s->assign.value, NULL);
                return make_dict_type(kt, vt);
            }
            Type* r = scan_expr_call_container_hint(mod, fn, s->assign.value, vname, ckind);
            if (r) return r;
        } else if (s->kind == STMT_RETURN && s->ret.expr) {
            Type* r = scan_expr_call_container_hint(mod, fn, s->ret.expr, vname, ckind);
            if (r) return r;
        } else if (s->kind == STMT_ASSERT && s->assert_s.cond) {
            Type* r = scan_expr_call_container_hint(mod, fn, s->assert_s.cond, vname, ckind);
            if (r) return r;
        } else if (s->kind == STMT_IF) {
            for (int b = 0; b < s->if_s.branch_count; b++) {
                Type* r = scan_empty_container_use(mod, fn, vname, ckind, s->if_s.bodies[b], s->if_s.body_counts[b]);
                if (r) return r;
            }
            Type* r = scan_empty_container_use(mod, fn, vname, ckind, s->if_s.else_body, s->if_s.else_count);
            if (r) return r;
        } else if (s->kind == STMT_WHILE) {
            Type* r = scan_empty_container_use(mod, fn, vname, ckind, s->while_s.body, s->while_s.body_count);
            if (r) return r;
        } else if (s->kind == STMT_FOR) {
            Type* r = scan_empty_container_use(mod, fn, vname, ckind, s->for_s.body, s->for_s.body_count);
            if (r) return r;
        } else if (s->kind == STMT_TRY) {
            Type* r = scan_empty_container_use(mod, fn, vname, ckind, s->try_s.try_body, s->try_s.try_count);
            if (r) return r;
        } else if (s->kind == STMT_WITH) {
            Type* r = scan_empty_container_use(mod, fn, vname, ckind, s->with_s.body, s->with_s.body_count);
            if (r) return r;
        }
    }
    return NULL;
}

static Type* scan_later_non_none_assign(ModuleInfo* mod, FuncInfo* fn, const char* vname, Stmt** stmts, int count) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        if (s->kind == STMT_ASSIGN && s->assign.target->kind == EXPR_VAR &&
            strcmp(s->assign.target->var.name, vname) == 0 &&
            s->assign.value && s->assign.value->kind != EXPR_NONE) {
            if (empty_container_kind(s->assign.value) == 0) {
                Type* vt = check_expr(mod, fn, s->assign.value, NULL);
                if (vt && vt->kind != TY_NONE_LIT) return make_optional_type(vt);
            }
        } else if (s->kind == STMT_IF) {
            for (int b = 0; b < s->if_s.branch_count; b++) {
                Type* r = scan_later_non_none_assign(mod, fn, vname, s->if_s.bodies[b], s->if_s.body_counts[b]);
                if (r) return r;
            }
            Type* r = scan_later_non_none_assign(mod, fn, vname, s->if_s.else_body, s->if_s.else_count);
            if (r) return r;
        } else if (s->kind == STMT_WHILE) {
            Type* r = scan_later_non_none_assign(mod, fn, vname, s->while_s.body, s->while_s.body_count);
            if (r) return r;
        } else if (s->kind == STMT_FOR) {
            Type* r = scan_later_non_none_assign(mod, fn, vname, s->for_s.body, s->for_s.body_count);
            if (r) return r;
        } else if (s->kind == STMT_TRY) {
            Type* r = scan_later_non_none_assign(mod, fn, vname, s->try_s.try_body, s->try_s.try_count);
            if (r) return r;
        } else if (s->kind == STMT_WITH) {
            Type* r = scan_later_non_none_assign(mod, fn, vname, s->with_s.body, s->with_s.body_count);
            if (r) return r;
        }
    }
    return NULL;
}

static bool stmts_always_exit(Stmt** b, int cnt) {
    if (cnt <= 0) return false;
    StmtKind k = b[cnt - 1]->kind;
    return k == STMT_RETURN || k == STMT_RAISE || k == STMT_BREAK || k == STMT_CONTINUE;
}

static void check_stmts(ModuleInfo* mod, FuncInfo* fn, Stmt** stmts, int count) {
    NarrowEntry blk_saved[32];
    int blk_saved_cnt = 0;
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
                if (fn->infer_ret) {
                    if (!s->ret.expr) {
                        fn->saw_void_return = true;
                    } else {
                        Type* rt = check_expr(mod, fn, s->ret.expr, fn->inferred_ret_type);
                        if (rt->kind == TY_NONE_LIT) {
                            fn->saw_void_return = true;
                        } else if (!fn->inferred_ret_type) {
                            fn->inferred_ret_type = rt;
                        } else if (type_assignable(fn->inferred_ret_type, rt)) {
                            /* compatible */
                        } else if (type_assignable(rt, fn->inferred_ret_type)) {
                            fn->inferred_ret_type = rt;
                        } else {
                            error_at(s->loc, "inconsistent return types '%s' and '%s' in '%s'",
                                     type_str(fn->inferred_ret_type), type_str(rt), fn->name);
                        }
                    }
                    break;
                }
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
                if (!fn) error_at(s->loc, "'yield' outside function");
                if (fn->infer_ret) {
                    Type* eh = (fn->inferred_ret_type && fn->inferred_ret_type->kind == TY_LIST) ? fn->inferred_ret_type->elem : NULL;
                    Type* yt = s->expr_s.expr ? check_expr(mod, fn, s->expr_s.expr, eh) : g_ty_void;
                    if (!fn->inferred_ret_type) fn->inferred_ret_type = make_list_type(yt);
                    else if (fn->inferred_ret_type->kind != TY_LIST || !type_assignable(fn->inferred_ret_type->elem, yt)) {
                        error_at(s->loc, "yield type mismatch");
                    }
                    break;
                }
                if (fn->ret_type->kind != TY_LIST) error_at(s->loc, "'yield' function must declare return type 'list[T]'");
                if (s->expr_s.expr) {
                    Type* yt = check_expr(mod, fn, s->expr_s.expr, fn->ret_type->elem);
                    if (!type_assignable(fn->ret_type->elem, yt)) error_at(s->loc, "yield type mismatch");
                }
                break;
            case STMT_YIELD_FROM:
                if (!fn) error_at(s->loc, "'yield from' outside function");
                if (fn->infer_ret) {
                    Type* st = check_expr(mod, fn, s->expr_s.expr, fn->inferred_ret_type);
                    Type* el = iter_element_type(st, s->expr_s.expr->loc);
                    Type* lt = make_list_type(el);
                    if (!fn->inferred_ret_type) fn->inferred_ret_type = lt;
                    else if (!type_eq(fn->inferred_ret_type, lt)) error_at(s->loc, "yield from type mismatch");
                    break;
                }
                if (fn->ret_type->kind != TY_LIST) error_at(s->loc, "'yield from' function must declare return type 'list[T]'");
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
                    if (!ann_ty && !existing) {
                        int ckind = empty_container_kind(s->assign.value);
                        if (ckind != 0) {
                            Type* inf = scan_empty_container_use(mod, fn, vname, ckind, stmts + i + 1, count - i - 1);
                            if (inf) {
                                s->assign.value->ty = inf;
                                bind_var(mod, fn, vname, inf, s->loc);
                                break;
                            }
                        }
                        if (s->assign.value->kind == EXPR_NONE) {
                            Type* inf = scan_later_non_none_assign(mod, fn, vname, stmts + i + 1, count - i - 1);
                            if (inf) {
                                s->assign.value->ty = inf;
                                bind_var(mod, fn, vname, inf, s->loc);
                                break;
                            }
                        }
                    }
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
                    VarSym* bv = resolve_var(mod, fn, vname, NULL);
                    if (bv) bv->is_nonneg = expr_is_nonneg(mod, fn, s->assign.value);
                } else if (is_val_builtin_call(mod, fn, tgt)) {
                    Type* tt = check_expr(mod, fn, tgt, NULL);
                    if (!expr_is_mutable_place(mod, fn, tgt)) {
                        error_at(tgt->loc, "cannot assign through immutable reference");
                    }
                    Type* vt = check_expr(mod, fn, s->assign.value, tt);
                    if (!type_assignable(tt, vt)) {
                        error_at(s->loc, "cannot assign '%s' to dereferenced target of type '%s'", type_str(vt), type_str(tt));
                    }
                } else if (tgt->kind == EXPR_ATTR) {
                    Type* raw_ot = check_expr(mod, fn, tgt->attr.obj, NULL);
                    Type* ot = type_deref_all(raw_ot);
                    if (ot->kind != TY_CLASS) error_at(tgt->loc, "cannot set attribute on type '%s'", type_str(raw_ot));
                    if (!expr_is_mutable_place(mod, fn, tgt->attr.obj)) {
                        error_at(tgt->loc, "cannot mutate field '%s' through immutable reference", tgt->attr.attr);
                    }
                    if (!type_is_ptr(raw_ot) && ot->cls->is_value_type && !expr_is_addressable(mod, fn, tgt->attr.obj)) {
                        error_at(tgt->loc, "cannot mutate field '%s' on temporary value", tgt->attr.attr);
                    }
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
                    Type* raw_ot = check_expr(mod, fn, tgt->index.obj, NULL);
                    if (!expr_is_mutable_place(mod, fn, tgt->index.obj)) {
                        error_at(tgt->loc, "cannot mutate element through immutable reference");
                    }
                    Type* ptr_ot = raw_ot;
                    while (ptr_ot->kind == TY_REF || ptr_ot->kind == TY_MUT_REF) ptr_ot = ptr_ot->elem;
                    Type* ot = type_deref_all(raw_ot);
                    if (ptr_ot->kind == TY_ADDR) {
                        if (type_ptr_is_const(ptr_ot)) error_at(tgt->loc, "cannot assign through immutable pointer '%s'", type_str(ptr_ot));
                        if (!type_is_int(check_expr(mod, fn, tgt->index.index, g_ty_i64))) error_at(tgt->index.index->loc, "pointer index must be int");
                        tgt->ty = ptr_ot->elem;
                        Type* vt = check_expr(mod, fn, s->assign.value, ptr_ot->elem);
                        if (!type_assignable(ptr_ot->elem, vt)) error_at(s->loc, "pointer element assignment type mismatch");
                    } else if (ot->kind == TY_LIST) {
                        if (!type_is_int(check_expr(mod, fn, tgt->index.index, g_ty_i64))) error_at(tgt->index.index->loc, "list index must be int");
                        tgt->index.index->is_nonneg = expr_is_nonneg(mod, fn, tgt->index.index);
                        if (tgt->index.obj->kind == EXPR_VAR && tgt->index.index->kind == EXPR_VAR &&
                            var_is_bound(resolve_var(mod, fn, tgt->index.index->var.name, NULL), tgt->index.obj->var.name)) {
                            tgt->is_inbounds = true;
                            tgt->index.index->is_nonneg = true;
                        }
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
                        error_at(tgt->loc, "type '%s' does not support item assignment", type_str(raw_ot));
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
                if (s->aug_assign.target->kind != EXPR_VAR && s->aug_assign.target->kind != EXPR_ATTR &&
                    s->aug_assign.target->kind != EXPR_INDEX && !is_val_builtin_call(mod, fn, s->aug_assign.target)) {
                    error_at(s->aug_assign.target->loc, "invalid augmented assignment target");
                }
                Type* tt = check_expr(mod, fn, s->aug_assign.target, NULL);
                if (!expr_is_mutable_place(mod, fn, s->aug_assign.target)) {
                    error_at(s->aug_assign.target->loc, "cannot mutate through immutable reference");
                }
                Type* vt = check_expr(mod, fn, s->aug_assign.value, tt);
                if (s->aug_assign.target->kind == EXPR_VAR) {
                    VarSym* bv = resolve_var(mod, fn, s->aug_assign.target->var.name, NULL);
                    if (bv && (strcmp(s->aug_assign.op, "+=") != 0 || !expr_is_nonneg(mod, fn, s->aug_assign.value))) {
                        bv->is_nonneg = false;
                    }
                }
                if (tt->kind == TY_SET && type_eq(tt, vt)) break;
                if (tt->kind == TY_STR && vt->kind == TY_STR && strcmp(s->aug_assign.op, "+=") == 0) break;
                if (tt->kind == TY_LIST && type_eq(tt, vt) && strcmp(s->aug_assign.op, "+=") == 0) break;
                if (!type_is_numeric(tt) || !type_is_numeric(vt)) {
                    error_at(s->loc, "augmented assignment '%s' unsupported for '%s' and '%s'", s->aug_assign.op, type_str(tt), type_str(vt));
                }
                break;
            }
            case STMT_IF: {
                bool all_exit = true;
                for (int b = 0; b < s->if_s.branch_count; b++) {
                    NarrowEntry br_saved[32];
                    int br_cnt = 0;
                    for (int prev = 0; prev < b; prev++) {
                        apply_cond_narrowing(mod, fn, s->if_s.conds[prev], false, s->if_s.bodies[b], s->if_s.body_counts[b], br_saved, &br_cnt, 32);
                    }
                    check_expr(mod, fn, s->if_s.conds[b], g_ty_bool);
                    apply_cond_narrowing(mod, fn, s->if_s.conds[b], true, s->if_s.bodies[b], s->if_s.body_counts[b], br_saved, &br_cnt, 32);
                    check_stmts(mod, fn, s->if_s.bodies[b], s->if_s.body_counts[b]);
                    restore_narrowing(br_saved, br_cnt);
                    if (!stmts_always_exit(s->if_s.bodies[b], s->if_s.body_counts[b])) all_exit = false;
                }
                if (s->if_s.else_count > 0) {
                    NarrowEntry el_saved[32];
                    int el_cnt = 0;
                    for (int b = 0; b < s->if_s.branch_count; b++) {
                        apply_cond_narrowing(mod, fn, s->if_s.conds[b], false, s->if_s.else_body, s->if_s.else_count, el_saved, &el_cnt, 32);
                    }
                    check_stmts(mod, fn, s->if_s.else_body, s->if_s.else_count);
                    restore_narrowing(el_saved, el_cnt);
                } else if (all_exit) {
                    for (int b = 0; b < s->if_s.branch_count; b++) {
                        apply_cond_narrowing(mod, fn, s->if_s.conds[b], false, stmts + i + 1, count - i - 1, blk_saved, &blk_saved_cnt, 32);
                    }
                }
                break;
            }
            case STMT_WHILE: {
                check_expr(mod, fn, s->while_s.cond, g_ty_bool);
                NarrowEntry w_saved[16];
                int w_cnt = 0;
                apply_cond_narrowing(mod, fn, s->while_s.cond, true, s->while_s.body, s->while_s.body_count, w_saved, &w_cnt, 16);
                check_stmts(mod, fn, s->while_s.body, s->while_s.body_count);
                restore_narrowing(w_saved, w_cnt);
                if (s->while_s.else_count > 0) check_stmts(mod, fn, s->while_s.else_body, s->while_s.else_count);
                break;
            }
            case STMT_FOR: {
                Expr* it = s->for_s.iter;
                bool set_nn = false;
                bool prev_nn = false;
                int prev_bound_cnt = 0;
                VarSym* loop_v = NULL;
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
                    loop_v = resolve_var(mod, fn, s->for_s.targets[0], NULL);
                    if (loop_v) {
                        prev_nn = loop_v->is_nonneg;
                        prev_bound_cnt = loop_v->bound_arr_count;
                        int ac = it->call.arg_count;
                        bool nn = (ac == 1) ||
                                  (expr_is_nonneg(mod, fn, it->call.args[0]) &&
                                   (ac == 2 || expr_is_nonneg(mod, fn, it->call.args[2])));
                        loop_v->is_nonneg = nn;
                        set_nn = true;
                        const char* b_arr = NULL;
                        if (ac == 1) b_arr = expr_len_var(it->call.args[0]);
                        else if (ac == 2 && it->call.args[0]->kind == EXPR_INT && !it->call.args[0]->int_lit.is_big &&
                                 it->call.args[0]->int_lit.ival >= 0) {
                            b_arr = expr_len_var(it->call.args[1]);
                        }
                        if (b_arr && !stmts_mutate_seq_len(s->for_s.body, s->for_s.body_count, b_arr)) {
                            var_add_bound(loop_v, b_arr);
                        }
                    }
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
                if (set_nn && loop_v) {
                    loop_v->is_nonneg = prev_nn;
                    loop_v->bound_arr_count = prev_bound_cnt;
                }
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
                apply_cond_narrowing(mod, fn, s->assert_s.cond, true, stmts + i + 1, count - i - 1, blk_saved, &blk_saved_cnt, 32);
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
                bool saw_none_case = false;
                VarSym* subj_sym = (s->match_s.subject->kind == EXPR_VAR && st->kind == TY_OPTIONAL)
                                   ? resolve_var(mod, fn, s->match_s.subject->var.name, NULL) : NULL;
                for (int c = 0; c < s->match_s.case_count; c++) {
                    MatchCase* mc = &s->match_s.cases[c];
                    if (mc->pattern) check_expr(mod, fn, mc->pattern, st);
                    Type* case_st = (saw_none_case && subj_sym) ? st->elem : st;
                    Type* orig_subj = NULL;
                    if (saw_none_case && subj_sym && subj_sym->type == st) {
                        orig_subj = subj_sym->type;
                        subj_sym->type = st->elem;
                    }
                    if (mc->capture) bind_var(mod, fn, mc->capture, case_st, s->loc);
                    check_stmts(mod, fn, mc->body, mc->body_count);
                    if (orig_subj) subj_sym->type = orig_subj;
                    if (mc->pattern && mc->pattern->kind == EXPR_NONE) saw_none_case = true;
                }
                break;
            }
        }
    }
    restore_narrowing(blk_saved, blk_saved_cnt);
}

static void add_field_to_class(ClassInfo* cls, const char* name, Type* type, Expr* def_e, SrcLoc loc) {
    if (type_has_ref(type)) {
        error_at(loc, "cannot store borrowed reference '%s' in class field '%s.%s'; use 'addr[...]' for raw pointers",
                 type_str(type), cls->name, name);
    }
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

static Type* scan_self_field_non_none(ModuleInfo* mod, FuncInfo* ctx_fn, const char* fnm, Stmt** stmts, int count) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        if (s->kind == STMT_ASSIGN && s->assign.target->kind == EXPR_ATTR &&
            s->assign.target->attr.obj->kind == EXPR_VAR &&
            strcmp(s->assign.target->attr.obj->var.name, "self") == 0 &&
            strcmp(s->assign.target->attr.attr, fnm) == 0 &&
            s->assign.value && s->assign.value->kind != EXPR_NONE &&
            empty_container_kind(s->assign.value) == 0) {
            Type* vt = check_expr(mod, ctx_fn, s->assign.value, NULL);
            if (vt && vt->kind != TY_NONE_LIT) return make_optional_type(vt);
        } else if (s->kind == STMT_IF) {
            for (int b = 0; b < s->if_s.branch_count; b++) {
                Type* r = scan_self_field_non_none(mod, ctx_fn, fnm, s->if_s.bodies[b], s->if_s.body_counts[b]);
                if (r) return r;
            }
            Type* r = scan_self_field_non_none(mod, ctx_fn, fnm, s->if_s.else_body, s->if_s.else_count);
            if (r) return r;
        } else if (s->kind == STMT_WHILE) {
            Type* r = scan_self_field_non_none(mod, ctx_fn, fnm, s->while_s.body, s->while_s.body_count);
            if (r) return r;
        } else if (s->kind == STMT_FOR) {
            Type* r = scan_self_field_non_none(mod, ctx_fn, fnm, s->for_s.body, s->for_s.body_count);
            if (r) return r;
        } else if (s->kind == STMT_TRY) {
            Type* r = scan_self_field_non_none(mod, ctx_fn, fnm, s->try_s.try_body, s->try_s.try_count);
            if (r) return r;
            for (int h = 0; h < s->try_s.handler_count; h++) {
                r = scan_self_field_non_none(mod, ctx_fn, fnm, s->try_s.handlers[h].body, s->try_s.handlers[h].body_count);
                if (r) return r;
            }
        } else if (s->kind == STMT_WITH) {
            Type* r = scan_self_field_non_none(mod, ctx_fn, fnm, s->with_s.body, s->with_s.body_count);
            if (r) return r;
        }
    }
    return NULL;
}

static bool scan_self_field_has_none(const char* fnm, Stmt** stmts, int count) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        if (s->kind == STMT_ASSIGN && s->assign.target->kind == EXPR_ATTR &&
            s->assign.target->attr.obj->kind == EXPR_VAR &&
            strcmp(s->assign.target->attr.obj->var.name, "self") == 0 &&
            strcmp(s->assign.target->attr.attr, fnm) == 0 &&
            s->assign.value && s->assign.value->kind == EXPR_NONE) {
            return true;
        } else if (s->kind == STMT_IF) {
            for (int b = 0; b < s->if_s.branch_count; b++) {
                if (scan_self_field_has_none(fnm, s->if_s.bodies[b], s->if_s.body_counts[b])) return true;
            }
            if (scan_self_field_has_none(fnm, s->if_s.else_body, s->if_s.else_count)) return true;
        } else if (s->kind == STMT_WHILE) {
            if (scan_self_field_has_none(fnm, s->while_s.body, s->while_s.body_count)) return true;
        } else if (s->kind == STMT_FOR) {
            if (scan_self_field_has_none(fnm, s->for_s.body, s->for_s.body_count)) return true;
        } else if (s->kind == STMT_TRY) {
            if (scan_self_field_has_none(fnm, s->try_s.try_body, s->try_s.try_count)) return true;
        } else if (s->kind == STMT_WITH) {
            if (scan_self_field_has_none(fnm, s->with_s.body, s->with_s.body_count)) return true;
        }
    }
    return false;
}

static Type* scan_self_field_container(ModuleInfo* mod, FuncInfo* ctx_fn, const char* fnm, int ckind, Stmt** stmts, int count) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        if (s->kind == STMT_EXPR && s->expr_s.expr && s->expr_s.expr->kind == EXPR_CALL) {
            Expr* c = s->expr_s.expr;
            if (c->call.callee->kind == EXPR_ATTR && c->call.callee->attr.obj->kind == EXPR_ATTR &&
                c->call.callee->attr.obj->attr.obj->kind == EXPR_VAR &&
                strcmp(c->call.callee->attr.obj->attr.obj->var.name, "self") == 0 &&
                strcmp(c->call.callee->attr.obj->attr.attr, fnm) == 0) {
                const char* mn = c->call.callee->attr.attr;
                if (ckind == 1) {
                    if (strcmp(mn, "append") == 0 && c->call.arg_count == 1) {
                        return make_list_type(check_expr(mod, ctx_fn, c->call.args[0], NULL));
                    }
                    if (strcmp(mn, "insert") == 0 && c->call.arg_count == 2) {
                        return make_list_type(check_expr(mod, ctx_fn, c->call.args[1], NULL));
                    }
                    if (strcmp(mn, "extend") == 0 && c->call.arg_count == 1) {
                        Type* it = check_expr(mod, ctx_fn, c->call.args[0], NULL);
                        return make_list_type(iter_element_type(it, c->call.args[0]->loc));
                    }
                } else if (ckind == 2) {
                    if (strcmp(mn, "add") == 0 && c->call.arg_count == 1) {
                        return make_set_type(check_expr(mod, ctx_fn, c->call.args[0], NULL));
                    }
                    if (strcmp(mn, "update") == 0 && c->call.arg_count == 1) {
                        Type* it = check_expr(mod, ctx_fn, c->call.args[0], NULL);
                        return make_set_type(iter_element_type(it, c->call.args[0]->loc));
                    }
                } else if (ckind == 3 && strcmp(mn, "setdefault") == 0 && c->call.arg_count == 2) {
                    Type* kt = check_expr(mod, ctx_fn, c->call.args[0], NULL);
                    Type* vt = check_expr(mod, ctx_fn, c->call.args[1], NULL);
                    return make_dict_type(kt, vt);
                }
            }
        } else if (s->kind == STMT_ASSIGN && ckind == 3 && s->assign.target->kind == EXPR_INDEX &&
                   s->assign.target->index.obj->kind == EXPR_ATTR &&
                   s->assign.target->index.obj->attr.obj->kind == EXPR_VAR &&
                   strcmp(s->assign.target->index.obj->attr.obj->var.name, "self") == 0 &&
                   strcmp(s->assign.target->index.obj->attr.attr, fnm) == 0 && s->assign.value) {
            Type* kt = check_expr(mod, ctx_fn, s->assign.target->index.index, NULL);
            Type* vt = check_expr(mod, ctx_fn, s->assign.value, NULL);
            return make_dict_type(kt, vt);
        } else if (s->kind == STMT_IF) {
            for (int b = 0; b < s->if_s.branch_count; b++) {
                Type* r = scan_self_field_container(mod, ctx_fn, fnm, ckind, s->if_s.bodies[b], s->if_s.body_counts[b]);
                if (r) return r;
            }
            Type* r = scan_self_field_container(mod, ctx_fn, fnm, ckind, s->if_s.else_body, s->if_s.else_count);
            if (r) return r;
        } else if (s->kind == STMT_WHILE) {
            Type* r = scan_self_field_container(mod, ctx_fn, fnm, ckind, s->while_s.body, s->while_s.body_count);
            if (r) return r;
        } else if (s->kind == STMT_FOR) {
            Type* r = scan_self_field_container(mod, ctx_fn, fnm, ckind, s->for_s.body, s->for_s.body_count);
            if (r) return r;
        } else if (s->kind == STMT_TRY) {
            Type* r = scan_self_field_container(mod, ctx_fn, fnm, ckind, s->try_s.try_body, s->try_s.try_count);
            if (r) return r;
        } else if (s->kind == STMT_WITH) {
            Type* r = scan_self_field_container(mod, ctx_fn, fnm, ckind, s->with_s.body, s->with_s.body_count);
            if (r) return r;
        }
    }
    return NULL;
}

static void infer_class_fields_from_stmts(ModuleInfo* mod, ClassInfo* cls, FuncInfo* mf, Stmt** stmts, int count) {
    for (int j = 0; j < count; j++) {
        Stmt* is = stmts[j];
        if (is->kind == STMT_ASSIGN && is->assign.target->kind == EXPR_ATTR &&
            is->assign.target->attr.obj->kind == EXPR_VAR &&
            strcmp(is->assign.target->attr.obj->var.name, "self") == 0) {
            const char* fnm = is->assign.target->attr.attr;
            if (!find_field_in_class(cls, fnm)) {
                Type* ft = is->assign.type_ann ? resolve_type_expr(mod, is->assign.type_ann) : NULL;
                if (!ft && is->assign.value) {
                    int ckind = empty_container_kind(is->assign.value);
                    if (ckind != 0) {
                        for (int m = 0; !ft && m < cls->method_count; m++) {
                            FuncInfo* cm = cls->methods[m];
                            if (!cm->is_generic && cm->ast) {
                                ft = scan_self_field_container(mod, cm, fnm, ckind, cm->ast->def_s.body, cm->ast->def_s.body_count);
                            }
                        }
                    } else if (is->assign.value->kind == EXPR_NONE) {
                        ft = scan_self_field_non_none(mod, mf, fnm, stmts + j + 1, count - j - 1);
                        for (int m = 0; !ft && m < cls->method_count; m++) {
                            FuncInfo* cm = cls->methods[m];
                            if (!cm->is_generic && cm->ast) {
                                ft = scan_self_field_non_none(mod, cm, fnm, cm->ast->def_s.body, cm->ast->def_s.body_count);
                            }
                        }
                    }
                }
                if (!ft && is->assign.value) {
                    Type* vt = check_expr(mod, mf, is->assign.value, ft);
                    ft = vt;
                    if (ft && ft->kind != TY_NONE_LIT && ft->kind != TY_OPTIONAL) {
                        for (int m = 0; m < cls->method_count; m++) {
                            FuncInfo* cm = cls->methods[m];
                            if (cm->ast && scan_self_field_has_none(fnm, cm->ast->def_s.body, cm->ast->def_s.body_count)) {
                                ft = make_optional_type(ft);
                                break;
                            }
                        }
                    }
                }
                if (!ft || ft->kind == TY_NONE_LIT) {
                    error_at(is->loc, "field 'self.%s' requires a type annotation (e.g. 'self.%s: %s = None')", fnm, fnm, "T | None");
                }
                add_field_to_class(cls, fnm, ft, NULL, is->loc);
            }
        } else if (is->kind == STMT_IF) {
            for (int b = 0; b < is->if_s.branch_count; b++) {
                infer_class_fields_from_stmts(mod, cls, mf, is->if_s.bodies[b], is->if_s.body_counts[b]);
            }
            infer_class_fields_from_stmts(mod, cls, mf, is->if_s.else_body, is->if_s.else_count);
        } else if (is->kind == STMT_WHILE) {
            infer_class_fields_from_stmts(mod, cls, mf, is->while_s.body, is->while_s.body_count);
        } else if (is->kind == STMT_FOR) {
            infer_class_fields_from_stmts(mod, cls, mf, is->for_s.body, is->for_s.body_count);
        } else if (is->kind == STMT_TRY) {
            infer_class_fields_from_stmts(mod, cls, mf, is->try_s.try_body, is->try_s.try_count);
        } else if (is->kind == STMT_WITH) {
            infer_class_fields_from_stmts(mod, cls, mf, is->with_s.body, is->with_s.body_count);
        }
    }
}

static void analyze_module(ModuleInfo* mod) {
    if (mod->analyzed) return;
    mod->analyzed = true;
    add_global_in_mod(mod, "__name__", g_ty_str, (SrcLoc){mod->path ? mod->path : "<module>", 1, 1});
    const char* dir_src = mod->real_path ? mod->real_path : mod->path;
    char* mpath_copy = dir_src ? strdup(dir_src) : NULL;
    const char* mod_dir = mpath_copy ? arena_strdup(dirname(mpath_copy)) : g_root_dir;
    if (mpath_copy) free(mpath_copy);

    for (int i = 0; i < mod->stmt_count; i++) {
        Stmt* s = mod->stmts[i];
        if (s->kind == STMT_IMPORT) {
            const char* mname = s->import_s.mod_name;
            const char* bind = s->import_s.alias;
            if (!bind) {
                const char* sl = strrchr(mname, '/');
                bind = sl ? sl + 1 : mname;
            }
            ModuleInfo* imp = load_module_by_name(mname, mod_dir, s->loc);
            Type* mt = (Type*)arena_alloc(sizeof(Type));
            mt->kind = TY_MODULE; mt->mod = imp; mt->mangled = arena_sprintf("mod_%s", imp->prefix);
            add_global_in_mod(mod, bind, mt, s->loc);
        } else if (s->kind == STMT_FROM_IMPORT) {
            const char* mname = s->from_import_s.mod_name;
            if (strcmp(mname, "abc") == 0 || strcmp(mname, "dataclasses") == 0 || strcmp(mname, "typing") == 0) {
                continue;
            }
            ModuleInfo* imp = try_load_module_by_name(mname, mod_dir);
            for (int k = 0; k < s->from_import_s.count; k++) {
                const char* orig = s->from_import_s.names[k];
                const char* bind = s->from_import_s.aliases[k] ? s->from_import_s.aliases[k] : orig;
                if (imp) {
                    VarSym* gv = find_global_in_mod(imp, orig);
                    if (gv) {
                        VarSym* nv = add_global_in_mod(mod, bind, gv->type, s->loc);
                        nv->llvm_name = gv->llvm_name;
                        continue;
                    }
                    FuncInfo* fi = find_func_in_mod(imp, orig);
                    if (fi) {
                        FuncInfo* add_f = fi;
                        if (strcmp(bind, orig) != 0) {
                            FuncInfo* alias_fn = (FuncInfo*)arena_alloc(sizeof(FuncInfo));
                            *alias_fn = *fi;
                            alias_fn->name = bind;
                            alias_fn->orig_fn = fi;
                            add_f = alias_fn;
                        }
                        mod->funcs = (FuncInfo**)realloc(mod->funcs, sizeof(FuncInfo*) * (size_t)(mod->func_count + 1));
                        mod->funcs[mod->func_count++] = add_f;
                        continue;
                    }
                    ClassInfo* ci = find_class_in_mod(imp, orig);
                    if (ci) {
                        ClassInfo* add_c = ci;
                        if (strcmp(bind, orig) != 0) {
                            ClassInfo* alias_ci = (ClassInfo*)arena_alloc(sizeof(ClassInfo));
                            *alias_ci = *ci;
                            alias_ci->name = bind;
                            add_c = alias_ci;
                        }
                        mod->classes = (ClassInfo**)realloc(mod->classes, sizeof(ClassInfo*) * (size_t)(mod->class_count + 1));
                        mod->classes[mod->class_count++] = add_c;
                        continue;
                    }
                }
                const char* sub_name = arena_sprintf("%s.%s", mname, orig);
                ModuleInfo* sub = try_load_module_by_name(sub_name, mod_dir);
                if (sub) {
                    Type* mt = (Type*)arena_alloc(sizeof(Type));
                    mt->kind = TY_MODULE; mt->mod = sub; mt->mangled = arena_sprintf("mod_%s", sub->prefix);
                    add_global_in_mod(mod, bind, mt, s->loc);
                    continue;
                }
                if (!imp) error_at(s->loc, "cannot find module '%s' (looked for .str)", mname);
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
        if (cls->mod != mod) continue;
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
        if (cls->mod != mod) continue;
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
        if (cls->mod != mod) continue;
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
                mf->is_async = bs->def_s.is_async;
                if (mf->is_abstract) {
                    cls->abstract_methods = (const char**)realloc(cls->abstract_methods, sizeof(char*) * (size_t)(cls->abstract_count + 1));
                    cls->abstract_methods[cls->abstract_count++] = mf->name;
                }
                mf->param_count = bs->def_s.param_count;
                mf->param_names = mf->param_count ? (const char**)arena_alloc(sizeof(char*) * (size_t)mf->param_count) : NULL;
                mf->param_types = mf->param_count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)mf->param_count) : NULL;
                mf->param_defaults = mf->param_count ? (Expr**)arena_alloc(sizeof(Expr*) * (size_t)mf->param_count) : NULL;
                mf->is_generic = def_has_unannotated_params(bs, !mf->is_static, mf->is_static);
                if (mf->is_generic && (cls->has_subclasses || cls->base_count > 0 || cls->is_abc ||
                                       mf->is_property || mf->is_abstract || strcmp(mf->name, "__init__") == 0)) {
                    error_at(bs->loc, "method '%s.%s' requires parameter type annotations", cls->name, mf->name);
                }
                const char* mdesc = arena_sprintf("'%s.%s'", cls->name, mf->name);
                if (!mf->is_generic) {
                    for (int p = 0; p < mf->param_count; p++) {
                        Param* pr = &bs->def_s.params[p];
                        mf->param_names[p] = pr->name;
                        mf->param_defaults[p] = pr->default_val;
                        if (p == 0 && !mf->is_static) {
                            mf->param_types[p] = cls->type;
                        } else {
                            mf->param_types[p] = resolve_param_type(mod, NULL, pr, mdesc);
                        }
                        add_local_in_fn(mf, pr->name, mf->param_types[p], true, pr->loc);
                    }
                } else {
                    for (int p = 0; p < mf->param_count; p++) {
                        Param* pr = &bs->def_s.params[p];
                        mf->param_names[p] = pr->name;
                        mf->param_defaults[p] = pr->default_val;
                    }
                }
                mf->ret_type = bs->def_s.ret_type ? resolve_type_expr(mod, bs->def_s.ret_type) : g_ty_void;
                mf->infer_ret = (bs->def_s.ret_type == NULL && strcmp(mf->name, "__init__") != 0);
                mf->type = mf->is_generic ? NULL : make_func_type(mf->param_types, mf->param_count, mf->is_async ? make_task_type(mf->ret_type) : mf->ret_type);
                cls->methods = (FuncInfo**)realloc(cls->methods, sizeof(FuncInfo*) * (size_t)(cls->method_count + 1));
                cls->methods[cls->method_count++] = mf;
                g_all_funcs = (FuncInfo**)realloc(g_all_funcs, sizeof(FuncInfo*) * (size_t)(g_all_func_count + 1));
                g_all_funcs[g_all_func_count++] = mf;
            }
        }

        FuncInfo* init_fn = find_method_in_class_exact(cls, "__init__");
        if (init_fn) {
            infer_class_fields_from_stmts(mod, cls, init_fn, init_fn->ast->def_s.body, init_fn->ast->def_s.body_count);
        }
        for (int m = 0; m < cls->method_count; m++) {
            FuncInfo* cm = cls->methods[m];
            if (cm != init_fn && !cm->is_generic && cm->ast) {
                infer_class_fields_from_stmts(mod, cls, cm, cm->ast->def_s.body, cm->ast->def_s.body_count);
            }
        }
    }

    for (int i = 0; i < mod->class_count; i++) {
        ClassInfo* cls = mod->classes[i];
        if (cls->mod != mod) continue;
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

    int mod_fn_start = mod->func_count;
    for (int i = 0; i < mod->stmt_count; i++) {
        Stmt* s = mod->stmts[i];
        if (s->kind == STMT_DEF) {
            FuncInfo* fn = (FuncInfo*)arena_alloc(sizeof(FuncInfo));
            fn->name = s->def_s.name;
            const char* ext_sym = expr_extern_sym(s->def_s.decorators, s->def_s.deco_count, fn->name);
            fn->is_extern = (ext_sym != NULL);
            fn->extern_sym = ext_sym;
            fn->llvm_name = ext_sym ? arena_sprintf("@%s", ext_sym) : arena_sprintf("@%s%s", mod->prefix, fn->name);
            fn->mod = mod;
            fn->ast = s;
            fn->param_count = s->def_s.param_count;
            fn->param_names = fn->param_count ? (const char**)arena_alloc(sizeof(char*) * (size_t)fn->param_count) : NULL;
            fn->param_types = fn->param_count ? (Type**)arena_alloc(sizeof(Type*) * (size_t)fn->param_count) : NULL;
            fn->param_defaults = fn->param_count ? (Expr**)arena_alloc(sizeof(Expr*) * (size_t)fn->param_count) : NULL;
            fn->ret_type = s->def_s.ret_type ? resolve_type_expr(mod, s->def_s.ret_type) : g_ty_void;
            fn->infer_ret = (!fn->is_extern && s->def_s.ret_type == NULL);
            fn->is_async = s->def_s.is_async;
            fn->is_generator = !fn->is_extern && stmt_has_yield(s->def_s.body, s->def_s.body_count);
            if (fn->is_extern) fn->body_checked = true;
            mod->funcs = (FuncInfo**)realloc(mod->funcs, sizeof(FuncInfo*) * (size_t)(mod->func_count + 1));
            mod->funcs[mod->func_count++] = fn;
            g_all_funcs = (FuncInfo**)realloc(g_all_funcs, sizeof(FuncInfo*) * (size_t)(g_all_func_count + 1));
            g_all_funcs[g_all_func_count++] = fn;
        }
    }

    for (int i = mod_fn_start; i < mod->func_count; i++) {
        FuncInfo* fn = mod->funcs[i];
        Stmt* s = fn->ast;
        fn->is_generic = def_has_unannotated_params(s, false, false);
        const char* fdesc = arena_sprintf("'%s()'", fn->name);
        if (!fn->is_generic) {
            for (int p = 0; p < fn->param_count; p++) {
                Param* pr = &s->def_s.params[p];
                fn->param_names[p] = pr->name;
                fn->param_types[p] = resolve_param_type(mod, NULL, pr, fdesc);
                fn->param_defaults[p] = pr->default_val;
                add_local_in_fn(fn, pr->name, fn->param_types[p], true, pr->loc);
            }
            fn->type = make_func_type(fn->param_types, fn->param_count, fn->is_async ? make_task_type(fn->ret_type) : fn->ret_type);
        } else {
            for (int p = 0; p < fn->param_count; p++) {
                Param* pr = &s->def_s.params[p];
                fn->param_names[p] = pr->name;
                fn->param_defaults[p] = pr->default_val;
            }
        }
    }

    /* Pre-infer general non-annotated module globals (e.g. ORIGIN = Vec2D(0, 0), PRIMES = [2, 3, 5]) */
    for (int i = 0; i < mod->stmt_count; i++) {
        Stmt* s = mod->stmts[i];
        if (s->kind == STMT_ASSIGN && s->assign.target->kind == EXPR_VAR && !s->assign.type_ann && s->assign.value) {
            const char* gname = s->assign.target->var.name;
            if (find_global_in_mod(mod, gname)) continue;
            if (empty_container_kind(s->assign.value) == 0 && s->assign.value->kind != EXPR_NONE) {
                Type* gt = check_expr(mod, NULL, s->assign.value, NULL);
                if (gt && gt->kind != TY_NONE_LIT) {
                    add_global_in_mod(mod, gname, gt, s->loc);
                }
            }
        }
    }

    for (int i = 0; i < mod->class_count; i++) {
        ClassInfo* cls = mod->classes[i];
        if (!cls->ast) continue;
        for (int j = 0; j < cls->method_count; j++) {
            check_func_body(mod, cls->methods[j]);
        }
    }

    for (int i = mod_fn_start; i < mod->func_count; i++) {
        FuncInfo* fn = mod->funcs[i];
        if (fn->mod != mod || !fn->ast) continue;
        check_func_body(mod, fn);
    }

    check_stmts(mod, NULL, mod->stmts, mod->stmt_count);
}

static char* try_read_mod_in_dir(const char* dir, const char* rel, char* out_path, size_t cap, size_t* out_len) {
    if (!rel || rel[0] == '\0') {
        snprintf(out_path, cap, "%s/__init__.str", dir);
        return read_entire_file(out_path, out_len);
    }
    snprintf(out_path, cap, "%s/%s.str", dir, rel);
    char* src = read_entire_file(out_path, out_len);
    if (src) return src;
    snprintf(out_path, cap, "%s/%s/__init__.str", dir, rel);
    return read_entire_file(out_path, out_len);
}

static char* try_resolve_mod_maps(const char* from_dir_rp, const char* mod_name, char* out_path, size_t cap, size_t* out_len) {
    if (g_mod_map_count == 0) return NULL;
    char head[256] = "";
    const char* sub_rel = "";
    const char* sep = NULL;
    for (const char* p = mod_name; *p; p++) {
        if (*p == '.' || *p == '/') { sep = p; break; }
    }
    if (sep && (size_t)(sep - mod_name) < sizeof(head)) {
        memcpy(head, mod_name, (size_t)(sep - mod_name));
        head[sep - mod_name] = '\0';
        char* sb = arena_strdup(sep + 1);
        for (int i = 0; sb[i]; i++) if (sb[i] == '.') sb[i] = '/';
        sub_rel = sb;
    }

    int best_exact = -1;
    size_t best_exact_len = 0;
    bool best_exact_global = false;
    int best_prefix = -1;
    size_t best_prefix_len = 0;
    bool best_prefix_global = false;

    for (int i = 0; i < g_mod_map_count; i++) {
        ModMapEntry* e = &g_mod_maps[i];
        bool is_global = (e->scope_rp == NULL || e->scope_rp[0] == '\0');
        size_t slen = 0;
        if (!is_global) {
            slen = strlen(e->scope_rp);
            if (strncmp(from_dir_rp, e->scope_rp, slen) != 0) continue;
            if (from_dir_rp[slen] != '\0' && from_dir_rp[slen] != '/') continue;
        }
        if (strcmp(e->alias, mod_name) == 0) {
            if (best_exact < 0 || (!is_global && (best_exact_global || slen > best_exact_len))) {
                best_exact = i; best_exact_len = slen; best_exact_global = is_global;
            }
        } else if (head[0] && strcmp(e->alias, head) == 0) {
            if (best_prefix < 0 || (!is_global && (best_prefix_global || slen > best_prefix_len))) {
                best_prefix = i; best_prefix_len = slen; best_prefix_global = is_global;
            }
        }
    }
    if (best_exact >= 0) {
        char* s = try_read_mod_in_dir(g_mod_maps[best_exact].target_dir, "", out_path, cap, out_len);
        if (s) return s;
    }
    if (best_prefix >= 0) {
        return try_read_mod_in_dir(g_mod_maps[best_prefix].target_dir, sub_rel, out_path, cap, out_len);
    }
    return NULL;
}

static ModuleInfo* try_load_module_by_name(const char* mod_name, const char* from_dir) {
    char rel[512];
    snprintf(rel, sizeof(rel), "%s", mod_name);
    for (int i = 0; rel[i]; i++) if (rel[i] == '.') rel[i] = '/';

    char path[1024];
    size_t len = 0;
    char* src = try_read_mod_in_dir(from_dir, rel, path, sizeof(path), &len);
    if (!src) {
        char from_dir_rp[4096];
        if (!realpath(from_dir, from_dir_rp)) snprintf(from_dir_rp, sizeof(from_dir_rp), "%s", from_dir);
        src = try_resolve_mod_maps(from_dir_rp, mod_name, path, sizeof(path), &len);
    }
    if (!src) {
        char walk_dir[4096];
        if (!realpath(from_dir, walk_dir)) snprintf(walk_dir, sizeof(walk_dir), "%s", from_dir);
        for (int hop = 0; !src && hop < 64; hop++) {
            char pkg_dir[4128];
            snprintf(pkg_dir, sizeof(pkg_dir), "%s/.pkgs", walk_dir);
            src = try_read_mod_in_dir(pkg_dir, rel, path, sizeof(path), &len);
            if (!src) {
                snprintf(pkg_dir, sizeof(pkg_dir), "%s/.stricta/pkgs", walk_dir);
                src = try_read_mod_in_dir(pkg_dir, rel, path, sizeof(path), &len);
            }
            if (strcmp(walk_dir, "/") == 0 || strcmp(walk_dir, ".") == 0) break;
            char* slash = strrchr(walk_dir, '/');
            if (!slash) break;
            if (slash == walk_dir) walk_dir[1] = '\0';
            else *slash = '\0';
        }
    }
    for (int d = 0; !src && d < g_import_dir_count; d++) {
        src = try_read_mod_in_dir(g_import_dirs[d], rel, path, sizeof(path), &len);
    }
    if (!src) {
        char pkg_dir[1056];
        snprintf(pkg_dir, sizeof(pkg_dir), "%s/.pkgs", g_root_dir);
        src = try_read_mod_in_dir(pkg_dir, rel, path, sizeof(path), &len);
        if (!src) {
            snprintf(pkg_dir, sizeof(pkg_dir), "%s/.stricta/pkgs", g_root_dir);
            src = try_read_mod_in_dir(pkg_dir, rel, path, sizeof(path), &len);
        }
    }
    if (!src) {
        src = try_read_mod_in_dir(".pkgs", rel, path, sizeof(path), &len);
    }
    if (!src) {
        src = try_read_mod_in_dir(".stricta/pkgs", rel, path, sizeof(path), &len);
    }
    if (!src) {
        const char* sp = getenv("STRICTA_PATH");
        if (sp && *sp) {
            char* sp_copy = strdup(sp);
            char* tok = strtok(sp_copy, ":");
            while (tok && !src) {
                src = try_read_mod_in_dir(tok, rel, path, sizeof(path), &len);
                tok = strtok(NULL, ":");
            }
            free(sp_copy);
        }
    }
    if (!src) {
        src = try_read_mod_in_dir(g_exe_dir, rel, path, sizeof(path), &len);
    }
    if (!src) {
        char up_dir[1024];
        snprintf(up_dir, sizeof(up_dir), "%s/..", g_exe_dir);
        src = try_read_mod_in_dir(up_dir, rel, path, sizeof(path), &len);
    }
    if (!src) return NULL;

    char rp_buf[4096];
    const char* rp = realpath(path, rp_buf) ? arena_strdup(rp_buf) : arena_strdup(path);
    for (int i = 0; i < g_module_count; i++) {
        if (g_modules[i]->real_path && strcmp(g_modules[i]->real_path, rp) == 0) {
            free(src);
            return g_modules[i];
        }
    }

    ModuleInfo* mod = (ModuleInfo*)arena_alloc(sizeof(ModuleInfo));
    mod->name = arena_strdup(mod_name);
    mod->path = arena_strdup(path);
    mod->real_path = rp;
    char* pfx = arena_strdup(mod_name);
    for (int i = 0; pfx[i]; i++) if (pfx[i] == '.' || pfx[i] == '/' || pfx[i] == '-') pfx[i] = '_';
    mod->prefix = arena_sprintf("m%d_%s__", g_module_count, pfx);

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

static ModuleInfo* load_module_by_name(const char* mod_name, const char* from_dir, SrcLoc loc) {
    ModuleInfo* mod = try_load_module_by_name(mod_name, from_dir);
    if (!mod) error_at(loc, "cannot find module '%s' (looked for .str)", mod_name);
    return mod;
}

/* =========================================================================
 * 4B. OWNERSHIP, BORROWING & MOVE CHECKER
 * ========================================================================= */

static bool is_mutating_container_method(Type* ot, const char* mname) {
    if (!ot) return false;
    if (ot->kind == TY_LIST) {
        return strcmp(mname, "append") == 0 || strcmp(mname, "extend") == 0 ||
               strcmp(mname, "insert") == 0 || strcmp(mname, "pop") == 0 ||
               strcmp(mname, "remove") == 0 || strcmp(mname, "sort") == 0 ||
               strcmp(mname, "reverse") == 0 || strcmp(mname, "clear") == 0;
    }
    if (ot->kind == TY_SET) {
        return strcmp(mname, "add") == 0 || strcmp(mname, "discard") == 0 ||
               strcmp(mname, "remove") == 0 || strcmp(mname, "pop") == 0 ||
               strcmp(mname, "clear") == 0 || strcmp(mname, "update") == 0 ||
               strcmp(mname, "intersection_update") == 0 ||
               strcmp(mname, "difference_update") == 0 ||
               strcmp(mname, "symmetric_difference_update") == 0;
    }
    if (ot->kind == TY_DICT) {
        return strcmp(mname, "setdefault") == 0 || strcmp(mname, "pop") == 0 ||
               strcmp(mname, "popitem") == 0 || strcmp(mname, "update") == 0 ||
               strcmp(mname, "clear") == 0;
    }
    if (ot->kind == TY_FILE) {
        return strcmp(mname, "write") == 0 || strcmp(mname, "close") == 0 || strcmp(mname, "read") == 0;
    }
    return false;
}

static const char* expr_root_var(Expr* e) {
    while (e) {
        if (e->kind == EXPR_VAR) return e->var.name;
        if (e->kind == EXPR_ATTR) { e = e->attr.obj; continue; }
        if (e->kind == EXPR_INDEX) { e = e->index.obj; continue; }
        break;
    }
    return NULL;
}

static bool expr_is_borrow_source(Expr* e) {
    if (!e || !e->ty) return false;
    if (e->kind == EXPR_ATTR && e->is_ptr_take &&
        (strcmp(e->attr.attr, "ref") == 0 || strcmp(e->attr.attr, "mut_ref") == 0)) {
        return true;
    }
    if (!type_is_owned(e->ty)) return false;
    if (e->kind == EXPR_CALL && e->is_ptr_take) return true;
    if (e->kind == EXPR_ATTR) {
        if (!e->attr.obj->ty) return false;
        if (e->attr.obj->ty->kind == TY_MODULE) return true;
        Type* base = type_deref_all(e->attr.obj->ty);
        if (base->kind != TY_CLASS) return false;
        FuncInfo* prop = find_method_in_class(base->cls, e->attr.attr);
        if (prop && prop->is_property) return false;
        return true;
    }
    if (e->kind == EXPR_INDEX) {
        Type* ot = type_deref_all(e->index.obj->ty);
        return ot && (ot->kind == TY_LIST || ot->kind == TY_DICT || ot->kind == TY_TUPLE);
    }
    return false;
}

static bool expr_mentions_var(Expr* e, const char* vname) {
    if (!e) return false;
    switch (e->kind) {
        case EXPR_VAR: return strcmp(e->var.name, vname) == 0;
        case EXPR_FSTRING:
            for (int i = 0; i < e->fstr.count; i++) {
                if (e->fstr.parts[i].is_expr && expr_mentions_var(e->fstr.parts[i].expr, vname)) return true;
            }
            return false;
        case EXPR_BINOP: return expr_mentions_var(e->binop.left, vname) || expr_mentions_var(e->binop.right, vname);
        case EXPR_UNOP: return expr_mentions_var(e->unop.operand, vname);
        case EXPR_AWAIT: return expr_mentions_var(e->await_e.expr, vname);
        case EXPR_TERNARY:
            return expr_mentions_var(e->ternary.cond, vname) ||
                   expr_mentions_var(e->ternary.then_e, vname) ||
                   expr_mentions_var(e->ternary.else_e, vname);
        case EXPR_WALRUS: return strcmp(e->walrus.name, vname) == 0 || expr_mentions_var(e->walrus.val, vname);
        case EXPR_ATTR: return expr_mentions_var(e->attr.obj, vname);
        case EXPR_INDEX: return expr_mentions_var(e->index.obj, vname) || expr_mentions_var(e->index.index, vname);
        case EXPR_SLICE:
            return expr_mentions_var(e->slice.obj, vname) ||
                   expr_mentions_var(e->slice.start, vname) ||
                   expr_mentions_var(e->slice.stop, vname) ||
                   expr_mentions_var(e->slice.step, vname);
        case EXPR_LIST: case EXPR_TUPLE: case EXPR_SET:
            for (int i = 0; i < e->seq.count; i++) if (expr_mentions_var(e->seq.items[i], vname)) return true;
            return false;
        case EXPR_DICT:
            for (int i = 0; i < e->dict.count; i++) {
                if (expr_mentions_var(e->dict.keys[i], vname) || expr_mentions_var(e->dict.vals[i], vname)) return true;
            }
            return false;
        case EXPR_LISTCOMP: case EXPR_SETCOMP: case EXPR_DICTCOMP: case EXPR_GENEXPR:
            if (expr_mentions_var(e->comp.elt, vname) || expr_mentions_var(e->comp.val, vname)) return true;
            for (int i = 0; i < e->comp.gen_count; i++) {
                if (expr_mentions_var(e->comp.gens[i].iter, vname)) return true;
                for (int f = 0; f < e->comp.gens[i].if_count; f++) {
                    if (expr_mentions_var(e->comp.gens[i].ifs[f], vname)) return true;
                }
            }
            return false;
        case EXPR_CALL:
            if (expr_mentions_var(e->call.callee, vname)) return true;
            for (int i = 0; i < e->call.arg_count; i++) if (expr_mentions_var(e->call.args[i], vname)) return true;
            return false;
        case EXPR_LAMBDA:
            return expr_mentions_var(e->lambda.body, vname);
        default:
            return false;
    }
}

static bool stmts_mention_var(Stmt** stmts, int count, const char* vname) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        switch (s->kind) {
            case STMT_RETURN: if (expr_mentions_var(s->ret.expr, vname)) return true; break;
            case STMT_EXPR: case STMT_YIELD: case STMT_YIELD_FROM:
                if (expr_mentions_var(s->expr_s.expr, vname)) return true; break;
            case STMT_ASSIGN:
                if (expr_mentions_var(s->assign.target, vname) || expr_mentions_var(s->assign.value, vname)) return true; break;
            case STMT_MULTI_ASSIGN:
                for (int k = 0; k < s->multi_assign.target_count; k++) {
                    if (expr_mentions_var(s->multi_assign.targets[k], vname)) return true;
                }
                if (expr_mentions_var(s->multi_assign.value, vname)) return true;
                break;
            case STMT_AUG_ASSIGN:
                if (expr_mentions_var(s->aug_assign.target, vname) || expr_mentions_var(s->aug_assign.value, vname)) return true; break;
            case STMT_IF:
                for (int b = 0; b < s->if_s.branch_count; b++) {
                    if (expr_mentions_var(s->if_s.conds[b], vname) ||
                        stmts_mention_var(s->if_s.bodies[b], s->if_s.body_counts[b], vname)) return true;
                }
                if (stmts_mention_var(s->if_s.else_body, s->if_s.else_count, vname)) return true;
                break;
            case STMT_WHILE:
                if (expr_mentions_var(s->while_s.cond, vname) ||
                    stmts_mention_var(s->while_s.body, s->while_s.body_count, vname) ||
                    stmts_mention_var(s->while_s.else_body, s->while_s.else_count, vname)) return true;
                break;
            case STMT_FOR:
                if (expr_mentions_var(s->for_s.iter, vname) ||
                    stmts_mention_var(s->for_s.body, s->for_s.body_count, vname) ||
                    stmts_mention_var(s->for_s.else_body, s->for_s.else_count, vname)) return true;
                break;
            case STMT_TRY:
                if (stmts_mention_var(s->try_s.try_body, s->try_s.try_count, vname)) return true;
                for (int h = 0; h < s->try_s.handler_count; h++) {
                    if (stmts_mention_var(s->try_s.handlers[h].body, s->try_s.handlers[h].body_count, vname)) return true;
                }
                if (stmts_mention_var(s->try_s.else_body, s->try_s.else_count, vname) ||
                    stmts_mention_var(s->try_s.finally_body, s->try_s.finally_count, vname)) return true;
                break;
            case STMT_RAISE: if (expr_mentions_var(s->raise_s.exc, vname)) return true; break;
            case STMT_ASSERT:
                if (expr_mentions_var(s->assert_s.cond, vname) || expr_mentions_var(s->assert_s.msg, vname)) return true; break;
            case STMT_WITH:
                if (expr_mentions_var(s->with_s.ctx, vname) ||
                    stmts_mention_var(s->with_s.body, s->with_s.body_count, vname)) return true;
                break;
            case STMT_DEL: if (expr_mentions_var(s->del_s.target, vname)) return true; break;
            case STMT_MATCH:
                if (expr_mentions_var(s->match_s.subject, vname)) return true;
                for (int c = 0; c < s->match_s.case_count; c++) {
                    if (expr_mentions_var(s->match_s.cases[c].pattern, vname) ||
                        stmts_mention_var(s->match_s.cases[c].body, s->match_s.cases[c].body_count, vname)) return true;
                }
                break;
            default: break;
        }
    }
    return false;
}

static int fn_param_index(FuncInfo* fn, const char* name) {
    if (!fn) return -1;
    for (int i = 0; i < fn->param_count; i++) {
        if (strcmp(fn->param_names[i], name) == 0) return i;
    }
    return -1;
}

static void raise_param_mode(FuncInfo* fn, int p, ParamMode m, bool* changed) {
    if (p < 0 || p >= fn->param_count) return;
    if (!type_is_owned(fn->param_types[p])) return;
    if (fn->param_modes[p] < m) {
        fn->param_modes[p] = m;
        *changed = true;
    }
}

static void scan_expr_param_modes(FuncInfo* fn, Expr* e, bool moved_ctx, bool* changed) {
    if (!e) return;
    if (moved_ctx && e->kind == EXPR_VAR && type_is_owned(e->ty)) {
        int p = fn_param_index(fn, e->var.name);
        if (p >= 0) raise_param_mode(fn, p, PM_MOVE, changed);
        return;
    }
    switch (e->kind) {
        case EXPR_FSTRING:
            for (int i = 0; i < e->fstr.count; i++) {
                if (e->fstr.parts[i].is_expr) scan_expr_param_modes(fn, e->fstr.parts[i].expr, false, changed);
            }
            break;
        case EXPR_BINOP:
            scan_expr_param_modes(fn, e->binop.left, false, changed);
            scan_expr_param_modes(fn, e->binop.right, false, changed);
            break;
        case EXPR_UNOP:
            scan_expr_param_modes(fn, e->unop.operand, false, changed);
            break;
        case EXPR_AWAIT:
            scan_expr_param_modes(fn, e->await_e.expr, true, changed);
            break;
        case EXPR_TERNARY:
            scan_expr_param_modes(fn, e->ternary.cond, false, changed);
            scan_expr_param_modes(fn, e->ternary.then_e, moved_ctx, changed);
            scan_expr_param_modes(fn, e->ternary.else_e, moved_ctx, changed);
            break;
        case EXPR_WALRUS:
            scan_expr_param_modes(fn, e->walrus.val, true, changed);
            break;
        case EXPR_ATTR:
            if (e->is_ptr_take && strcmp(e->attr.attr, "mut_ref") == 0) {
                const char* rv = expr_root_var(e->attr.obj);
                if (rv) {
                    int pi = fn_param_index(fn, rv);
                    if (pi >= 0 && !type_is_ptr(fn->param_types[pi])) {
                        raise_param_mode(fn, pi, PM_MUT_BORROW, changed);
                    }
                }
            }
            scan_expr_param_modes(fn, e->attr.obj, false, changed);
            break;
        case EXPR_INDEX:
            scan_expr_param_modes(fn, e->index.obj, false, changed);
            scan_expr_param_modes(fn, e->index.index, false, changed);
            break;
        case EXPR_SLICE:
            scan_expr_param_modes(fn, e->slice.obj, false, changed);
            scan_expr_param_modes(fn, e->slice.start, false, changed);
            scan_expr_param_modes(fn, e->slice.stop, false, changed);
            scan_expr_param_modes(fn, e->slice.step, false, changed);
            break;
        case EXPR_LIST: case EXPR_TUPLE: case EXPR_SET:
            for (int i = 0; i < e->seq.count; i++) {
                scan_expr_param_modes(fn, e->seq.items[i], true, changed);
            }
            break;
        case EXPR_DICT:
            for (int i = 0; i < e->dict.count; i++) {
                scan_expr_param_modes(fn, e->dict.keys[i], true, changed);
                scan_expr_param_modes(fn, e->dict.vals[i], true, changed);
            }
            break;
        case EXPR_LISTCOMP: case EXPR_SETCOMP: case EXPR_GENEXPR:
            scan_expr_param_modes(fn, e->comp.elt, true, changed);
            for (int i = 0; i < e->comp.gen_count; i++) {
                scan_expr_param_modes(fn, e->comp.gens[i].iter, false, changed);
                for (int f = 0; f < e->comp.gens[i].if_count; f++) {
                    scan_expr_param_modes(fn, e->comp.gens[i].ifs[f], false, changed);
                }
            }
            break;
        case EXPR_DICTCOMP:
            scan_expr_param_modes(fn, e->comp.elt, true, changed);
            scan_expr_param_modes(fn, e->comp.val, true, changed);
            for (int i = 0; i < e->comp.gen_count; i++) {
                scan_expr_param_modes(fn, e->comp.gens[i].iter, false, changed);
                for (int f = 0; f < e->comp.gens[i].if_count; f++) {
                    scan_expr_param_modes(fn, e->comp.gens[i].ifs[f], false, changed);
                }
            }
            break;
        case EXPR_CALL: {
            Expr* ce = e->call.callee;
            if (ce->kind == EXPR_ATTR) {
                Type* ot = type_deref_all(ce->attr.obj->ty);
                const char* mn = ce->attr.attr;
                if (ot && (ot->kind == TY_LIST || ot->kind == TY_SET || ot->kind == TY_DICT || ot->kind == TY_FILE)) {
                    if (is_mutating_container_method(ot, mn)) {
                        const char* rv = expr_root_var(ce->attr.obj);
                        if (rv && !type_is_ptr(ce->attr.obj->ty)) raise_param_mode(fn, fn_param_index(fn, rv), PM_MUT_BORROW, changed);
                    }
                    scan_expr_param_modes(fn, ce->attr.obj, false, changed);
                    bool arg_move = (ot->kind == TY_LIST && (strcmp(mn, "append") == 0 || strcmp(mn, "insert") == 0)) ||
                                    (ot->kind == TY_SET && strcmp(mn, "add") == 0) ||
                                    (ot->kind == TY_DICT && strcmp(mn, "setdefault") == 0);
                    for (int i = 0; i < e->call.arg_count; i++) {
                        bool mv = arg_move && (ot->kind != TY_LIST || strcmp(mn, "insert") != 0 || i == 1);
                        scan_expr_param_modes(fn, e->call.args[i], mv, changed);
                    }
                    break;
                }
                if (ot && ot->kind == TY_CLASS) {
                    FuncInfo* m = e->call.resolved_fn ? e->call.resolved_fn : find_method_in_class(ot->cls, mn);
                    if (m && m->param_modes) {
                        int sp = m->is_static ? 0 : 1;
                        if (!m->is_static && !m->is_classmethod && m->param_count > 0) {
                            if (ce->attr.obj->kind == EXPR_VAR && !type_is_ptr(ce->attr.obj->ty)) {
                                int p = fn_param_index(fn, ce->attr.obj->var.name);
                                if (p >= 0 && m->param_modes[0] != PM_BORROW) {
                                    raise_param_mode(fn, p, m->param_modes[0], changed);
                                }
                            } else {
                                scan_expr_param_modes(fn, ce->attr.obj, false, changed);
                            }
                        }
                        for (int i = 0; i < e->call.arg_count && i + sp < m->param_count; i++) {
                            scan_expr_param_modes(fn, e->call.args[i], m->param_modes[i + sp] == PM_MOVE, changed);
                            if (m->param_modes[i + sp] == PM_MUT_BORROW && e->call.args[i]->kind == EXPR_VAR) {
                                raise_param_mode(fn, fn_param_index(fn, e->call.args[i]->var.name), PM_MUT_BORROW, changed);
                            }
                        }
                        break;
                    }
                }
                if (ot && ot->kind == TY_MODULE) {
                    ModuleInfo* mm = ot->mod;
                    FuncInfo* mf = e->call.resolved_fn ? e->call.resolved_fn : find_func_in_mod(mm, mn);
                    if (mf && mf->param_modes) {
                        for (int i = 0; i < e->call.arg_count && i < mf->param_count; i++) {
                            scan_expr_param_modes(fn, e->call.args[i], mf->param_modes[i] == PM_MOVE, changed);
                            if (mf->param_modes[i] == PM_MUT_BORROW && e->call.args[i]->kind == EXPR_VAR) {
                                raise_param_mode(fn, fn_param_index(fn, e->call.args[i]->var.name), PM_MUT_BORROW, changed);
                            }
                        }
                        break;
                    }
                    ClassInfo* mci = find_class_in_mod(mm, mn);
                    if (mci) {
                        FuncInfo* init_fn = find_method_in_class(mci, "__init__");
                        if (init_fn && init_fn->param_modes) {
                            for (int i = 0; i < e->call.arg_count && i + 1 < init_fn->param_count; i++) {
                                scan_expr_param_modes(fn, e->call.args[i], init_fn->param_modes[i + 1] == PM_MOVE, changed);
                            }
                        } else {
                            for (int i = 0; i < e->call.arg_count; i++) scan_expr_param_modes(fn, e->call.args[i], true, changed);
                        }
                        break;
                    }
                }
            }
            if (ce->kind == EXPR_VAR) {
                const char* cname = ce->var.name;
                ClassInfo* ci = fn ? find_class_in_mod(fn->mod, cname) : NULL;
                if (ci) {
                    FuncInfo* init_fn = find_method_in_class(ci, "__init__");
                    if (init_fn && init_fn->param_modes) {
                        for (int i = 0; i < e->call.arg_count && i + 1 < init_fn->param_count; i++) {
                            scan_expr_param_modes(fn, e->call.args[i], init_fn->param_modes[i + 1] == PM_MOVE, changed);
                        }
                    } else {
                        for (int i = 0; i < e->call.arg_count; i++) scan_expr_param_modes(fn, e->call.args[i], true, changed);
                    }
                    break;
                }
                FuncInfo* tf = e->call.resolved_fn ? e->call.resolved_fn : (fn ? find_func_in_mod(fn->mod, cname) : NULL);
                if (tf && tf->param_modes) {
                    for (int i = 0; i < e->call.arg_count && i < tf->param_count; i++) {
                        scan_expr_param_modes(fn, e->call.args[i], tf->param_modes[i] == PM_MOVE, changed);
                        if (tf->param_modes[i] == PM_MUT_BORROW && e->call.args[i]->kind == EXPR_VAR) {
                            raise_param_mode(fn, fn_param_index(fn, e->call.args[i]->var.name), PM_MUT_BORROW, changed);
                        }
                    }
                    break;
                }
            }
            for (int i = 0; i < e->call.arg_count; i++) scan_expr_param_modes(fn, e->call.args[i], false, changed);
            break;
        }
        default: break;
    }
}

static void scan_stmts_param_modes(FuncInfo* fn, Stmt** stmts, int count, bool* changed) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        switch (s->kind) {
            case STMT_RETURN:
                scan_expr_param_modes(fn, s->ret.expr, true, changed);
                break;
            case STMT_YIELD:
                scan_expr_param_modes(fn, s->expr_s.expr, true, changed);
                break;
            case STMT_YIELD_FROM:
                scan_expr_param_modes(fn, s->expr_s.expr, false, changed);
                break;
            case STMT_EXPR:
                scan_expr_param_modes(fn, s->expr_s.expr, false, changed);
                break;
            case STMT_ASSIGN: {
                Expr* tgt = s->assign.target;
                if (tgt->kind == EXPR_VAR) {
                    bool is_global_t = fn && find_global_in_mod(fn->mod, tgt->var.name) != NULL &&
                                       find_local_in_fn(fn, tgt->var.name) == NULL;
                    bool is_borrow = expr_is_borrow_source(s->assign.value) && !is_global_t;
                    scan_expr_param_modes(fn, s->assign.value, !is_borrow, changed);
                } else if (tgt->kind == EXPR_CALL && tgt->is_ptr_take) {
                    for (int a = 0; a < tgt->call.arg_count; a++) scan_expr_param_modes(fn, tgt->call.args[a], false, changed);
                    scan_expr_param_modes(fn, s->assign.value, true, changed);
                } else if (tgt->kind == EXPR_ATTR) {
                    const char* rv = expr_root_var(tgt->attr.obj);
                    if (rv && !type_is_ptr(tgt->attr.obj->ty)) raise_param_mode(fn, fn_param_index(fn, rv), PM_MUT_BORROW, changed);
                    scan_expr_param_modes(fn, s->assign.value, true, changed);
                } else if (tgt->kind == EXPR_INDEX) {
                    const char* rv = expr_root_var(tgt->index.obj);
                    if (rv && !type_is_ptr(tgt->index.obj->ty)) raise_param_mode(fn, fn_param_index(fn, rv), PM_MUT_BORROW, changed);
                    Type* it_ot = type_deref_all(tgt->index.obj->ty);
                    scan_expr_param_modes(fn, tgt->index.index,
                                          it_ot && it_ot->kind == TY_DICT, changed);
                    scan_expr_param_modes(fn, s->assign.value, true, changed);
                }
                break;
            }
            case STMT_MULTI_ASSIGN:
                scan_expr_param_modes(fn, s->multi_assign.value, false, changed);
                break;
            case STMT_AUG_ASSIGN: {
                const char* rv = expr_root_var(s->aug_assign.target);
                if (rv) raise_param_mode(fn, fn_param_index(fn, rv), PM_MUT_BORROW, changed);
                scan_expr_param_modes(fn, s->aug_assign.value, false, changed);
                break;
            }
            case STMT_IF:
                for (int b = 0; b < s->if_s.branch_count; b++) {
                    scan_expr_param_modes(fn, s->if_s.conds[b], false, changed);
                    scan_stmts_param_modes(fn, s->if_s.bodies[b], s->if_s.body_counts[b], changed);
                }
                scan_stmts_param_modes(fn, s->if_s.else_body, s->if_s.else_count, changed);
                break;
            case STMT_WHILE:
                scan_expr_param_modes(fn, s->while_s.cond, false, changed);
                scan_stmts_param_modes(fn, s->while_s.body, s->while_s.body_count, changed);
                scan_stmts_param_modes(fn, s->while_s.else_body, s->while_s.else_count, changed);
                break;
            case STMT_FOR:
                scan_expr_param_modes(fn, s->for_s.iter, false, changed);
                scan_stmts_param_modes(fn, s->for_s.body, s->for_s.body_count, changed);
                scan_stmts_param_modes(fn, s->for_s.else_body, s->for_s.else_count, changed);
                break;
            case STMT_TRY:
                scan_stmts_param_modes(fn, s->try_s.try_body, s->try_s.try_count, changed);
                for (int h = 0; h < s->try_s.handler_count; h++) {
                    scan_stmts_param_modes(fn, s->try_s.handlers[h].body, s->try_s.handlers[h].body_count, changed);
                }
                scan_stmts_param_modes(fn, s->try_s.else_body, s->try_s.else_count, changed);
                scan_stmts_param_modes(fn, s->try_s.finally_body, s->try_s.finally_count, changed);
                break;
            case STMT_WITH:
                scan_expr_param_modes(fn, s->with_s.ctx, false, changed);
                scan_stmts_param_modes(fn, s->with_s.body, s->with_s.body_count, changed);
                break;
            case STMT_DEL:
                if (s->del_s.target->kind == EXPR_VAR) {
                    int p = fn_param_index(fn, s->del_s.target->var.name);
                    if (p >= 0) raise_param_mode(fn, p, PM_MOVE, changed);
                } else if (s->del_s.target->kind == EXPR_INDEX) {
                    const char* rv = expr_root_var(s->del_s.target->index.obj);
                    if (rv) raise_param_mode(fn, fn_param_index(fn, rv), PM_MUT_BORROW, changed);
                }
                break;
            case STMT_ASSERT:
                scan_expr_param_modes(fn, s->assert_s.cond, false, changed);
                scan_expr_param_modes(fn, s->assert_s.msg, false, changed);
                break;
            case STMT_RAISE:
                scan_expr_param_modes(fn, s->raise_s.exc, true, changed);
                break;
            case STMT_MATCH:
                scan_expr_param_modes(fn, s->match_s.subject, false, changed);
                for (int c = 0; c < s->match_s.case_count; c++) {
                    scan_expr_param_modes(fn, s->match_s.cases[c].pattern, false, changed);
                    scan_stmts_param_modes(fn, s->match_s.cases[c].body, s->match_s.cases[c].body_count, changed);
                }
                break;
            default: break;
        }
    }
}

static void analyze_ownership_modes(void) {
    for (int i = 0; i < g_all_func_count; i++) {
        FuncInfo* fn = g_all_funcs[i];
        if (fn->is_generic) continue;
        fn->param_modes = fn->param_count ? (ParamMode*)arena_alloc(sizeof(ParamMode) * (size_t)fn->param_count) : NULL;
        for (int p = 0; p < fn->param_count; p++) {
            fn->param_modes[p] = PM_BORROW;
            if (fn->is_method && strcmp(fn->name, "__init__") == 0 && p == 0) {
                fn->param_modes[p] = PM_MUT_BORROW;
            }
            if (fn->is_async && type_is_owned(fn->param_types[p])) {
                fn->param_modes[p] = PM_MOVE;
            }
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < g_all_func_count; i++) {
            FuncInfo* fn = g_all_funcs[i];
            if (fn->is_generic || fn->is_extern) continue;
            if (fn->is_lambda && fn->lambda_ast) {
                scan_expr_param_modes(fn, fn->lambda_ast->lambda.body, true, &changed);
            } else if (fn->ast) {
                scan_stmts_param_modes(fn, fn->ast->def_s.body, fn->ast->def_s.body_count, &changed);
            }
        }
        /* Propagate parameter modes across vtable families */
        for (int c = 0; c < g_all_class_count; c++) {
            ClassInfo* cls = g_all_classes[c];
            for (int v = 0; v < cls->vtable_count; v++) {
                FuncInfo* vm = cls->vtable[v].fn;
                if (!vm || !vm->param_modes) continue;
                for (int m = 0; m < cls->mro_count; m++) {
                    FuncInfo* am = find_method_in_class_exact(cls->mro[m], cls->vtable[v].name);
                    if (!am || !am->param_modes || am->param_count != vm->param_count) continue;
                    for (int p = 0; p < vm->param_count; p++) {
                        if (am->param_modes[p] > vm->param_modes[p]) { vm->param_modes[p] = am->param_modes[p]; changed = true; }
                        else if (vm->param_modes[p] > am->param_modes[p]) { am->param_modes[p] = vm->param_modes[p]; changed = true; }
                    }
                }
            }
        }
    }
    /* Sync aliased imports */
    for (int m = 0; m < g_module_count; m++) {
        for (int f = 0; f < g_modules[m]->func_count; f++) {
            FuncInfo* fn = g_modules[m]->funcs[f];
            if (fn->orig_fn) fn->param_modes = fn->orig_fn->param_modes;
        }
    }
}

typedef struct {
    const char* name;
    Type* type;
    OwnState state;
    bool is_view;
    bool is_mut_borrow;
    bool borrows_stack;
    const char* borrow_owner;
    bool is_global;
    int imm_borrows;
    int mut_borrows;
    int decl_depth;
} OwnSlot;

typedef struct {
    OwnSlot slots[256];
    int count;
    int loop_depth;
} OwnCtx;

static int own_find(OwnCtx* ctx, const char* name) {
    for (int i = 0; i < ctx->count; i++) {
        if (strcmp(ctx->slots[i].name, name) == 0) return i;
    }
    return -1;
}

static int own_ensure(OwnCtx* ctx, const char* name, Type* ty, bool is_global) {
    int idx = own_find(ctx, name);
    if (idx >= 0) return idx;
    if (ctx->count >= 256) return -1;
    idx = ctx->count++;
    memset(&ctx->slots[idx], 0, sizeof(OwnSlot));
    ctx->slots[idx].name = name;
    ctx->slots[idx].type = ty;
    ctx->slots[idx].state = OWN_UNINIT;
    ctx->slots[idx].is_global = is_global;
    ctx->slots[idx].decl_depth = ctx->loop_depth;
    return idx;
}

static void own_release_view(OwnCtx* ctx, int idx) {
    if (idx < 0 || !ctx->slots[idx].is_view || ctx->slots[idx].state != OWN_ALIVE) return;
    if (ctx->slots[idx].borrow_owner) {
        int oi = own_find(ctx, ctx->slots[idx].borrow_owner);
        if (oi >= 0) {
            if (ctx->slots[idx].is_mut_borrow) {
                if (ctx->slots[oi].mut_borrows > 0) ctx->slots[oi].mut_borrows--;
            } else {
                if (ctx->slots[oi].imm_borrows > 0) ctx->slots[oi].imm_borrows--;
            }
        }
    }
    ctx->slots[idx].state = OWN_UNINIT;
    ctx->slots[idx].is_view = false;
    ctx->slots[idx].is_mut_borrow = false;
    ctx->slots[idx].borrows_stack = false;
    ctx->slots[idx].borrow_owner = NULL;
}

static void own_end_dead_views(OwnCtx* ctx, Stmt** rem, int rem_count) {
    for (int i = 0; i < ctx->count; i++) {
        if (ctx->slots[i].is_view && ctx->slots[i].state == OWN_ALIVE &&
            ctx->slots[i].decl_depth >= ctx->loop_depth &&
            !stmts_mention_var(rem, rem_count, ctx->slots[i].name)) {
            own_release_view(ctx, i);
        }
    }
}

static void own_sync_to_fn(FuncInfo* fn, OwnCtx* ctx) {
    if (!fn) return;
    for (int i = 0; i < fn->local_count; i++) {
        int idx = own_find(ctx, fn->locals[i].name);
        if (idx >= 0 && ctx->slots[idx].is_view) {
            fn->locals[i].is_borrowed_view = true;
            fn->locals[i].borrow_owner = ctx->slots[idx].borrow_owner;
        }
    }
}

static void own_check_expr(ModuleInfo* mod, FuncInfo* fn, OwnCtx* ctx, Expr* e, bool want_move);

static void own_check_mutate_var(OwnCtx* ctx, const char* name, SrcLoc loc) {
    int idx = own_find(ctx, name);
    if (idx < 0) return;
    if (ctx->slots[idx].is_view && !ctx->slots[idx].is_mut_borrow) {
        error_at(loc, "cannot mutate borrowed value '%s'", name);
    }
    if (type_is_owned(ctx->slots[idx].type) &&
        (ctx->slots[idx].state == OWN_MOVED || ctx->slots[idx].state == OWN_MAYBE_MOVED)) {
        error_at(loc, "use of moved value '%s'", name);
    }
    if (ctx->slots[idx].imm_borrows > 0 || ctx->slots[idx].mut_borrows > 0) {
        error_at(loc, "cannot mutate '%s' while it is borrowed", name);
    }
}

static bool var_is_fn_param(FuncInfo* fn, const char* name) {
    if (!fn || !name) return false;
    for (int i = 0; i < fn->param_count; i++) {
        if (strcmp(fn->param_names[i], name) == 0) return true;
    }
    return false;
}

static bool expr_borrows_stack(ModuleInfo* mod, FuncInfo* fn, OwnCtx* ctx, Expr* obj) {
    if (!obj) return false;
    if (obj->kind == EXPR_VAR) {
        bool is_glob = fn ? (find_global_in_mod(mod, obj->var.name) != NULL && find_local_in_fn(fn, obj->var.name) == NULL) : true;
        return !is_glob;
    }
    const char* rv = expr_root_var(obj);
    if (!rv) return false;
    bool is_glob = fn ? (find_global_in_mod(mod, rv) != NULL && find_local_in_fn(fn, rv) == NULL) : true;
    if (is_glob) return false;
    if (obj->kind == EXPR_ATTR || obj->kind == EXPR_INDEX) {
        Expr* root_e = obj;
        while (root_e->kind == EXPR_ATTR || root_e->kind == EXPR_INDEX) {
            root_e = (root_e->kind == EXPR_ATTR) ? root_e->attr.obj : root_e->index.obj;
        }
        if (root_e->ty && type_is_ptr(root_e->ty)) {
            int ridx = own_find(ctx, rv);
            if (ridx >= 0 && ctx->slots[ridx].is_view) return ctx->slots[ridx].borrows_stack;
            return !var_is_fn_param(fn, rv);
        }
        if (var_is_fn_param(fn, rv) && root_e->ty &&
            ((root_e->ty->kind == TY_CLASS && !root_e->ty->cls->is_value_type) ||
             root_e->ty->kind == TY_LIST || root_e->ty->kind == TY_SET || root_e->ty->kind == TY_DICT)) {
            return false;
        }
    }
    return true;
}

static void own_check_ret_ref(ModuleInfo* mod, FuncInfo* fn, OwnCtx* ctx, Expr* e, SrcLoc loc) {
    if (!e || !e->ty) return;
    if (e->kind == EXPR_TERNARY) {
        own_check_ret_ref(mod, fn, ctx, e->ternary.then_e, loc);
        own_check_ret_ref(mod, fn, ctx, e->ternary.else_e, loc);
        return;
    }
    if (e->kind == EXPR_ATTR && e->is_ptr_take &&
        (strcmp(e->attr.attr, "ref") == 0 || strcmp(e->attr.attr, "mut_ref") == 0)) {
        if (expr_borrows_stack(mod, fn, ctx, e->attr.obj)) {
            const char* rv = expr_root_var(e->attr.obj);
            error_at(loc, "cannot return reference to local '%s'", rv ? rv : "<local>");
        }
        return;
    }
    if (e->kind == EXPR_VAR && (e->ty->kind == TY_REF || e->ty->kind == TY_MUT_REF ||
        (e->ty->kind == TY_OPTIONAL && (e->ty->elem->kind == TY_REF || e->ty->elem->kind == TY_MUT_REF)))) {
        int idx = own_find(ctx, e->var.name);
        if (idx >= 0 && ctx->slots[idx].is_view && ctx->slots[idx].borrows_stack) {
            error_at(loc, "cannot return reference '%s' to local '%s'",
                     e->var.name, ctx->slots[idx].borrow_owner ? ctx->slots[idx].borrow_owner : "<local>");
        }
    }
}

static void own_check_expr(ModuleInfo* mod, FuncInfo* fn, OwnCtx* ctx, Expr* e, bool want_move) {
    if (!e) return;
    switch (e->kind) {
        case EXPR_VAR: {
            int idx = own_find(ctx, e->var.name);
            if (idx < 0) {
                VarSym* gv = find_global_in_mod(mod, e->var.name);
                if (gv && type_is_owned(gv->type) && want_move) {
                    error_at(e->loc, "cannot move out of global '%s'; use '.copy()' to clone", e->var.name);
                }
                return;
            }
            Type* sty = ctx->slots[idx].type;
            bool is_mut_ref_ty = (sty && (sty->kind == TY_MUT_REF || (sty->kind == TY_OPTIONAL && sty->elem->kind == TY_MUT_REF)));
            if (!type_is_owned(sty) && !is_mut_ref_ty) {
                if (ctx->slots[idx].mut_borrows > 0) {
                    error_at(e->loc, "cannot borrow '%s' while it is mutably borrowed", e->var.name);
                }
                return;
            }
            if (ctx->slots[idx].state == OWN_MOVED || ctx->slots[idx].state == OWN_MAYBE_MOVED) {
                error_at(e->loc, "use of moved value '%s'", e->var.name);
            }
            if (want_move) {
                if (ctx->slots[idx].is_global && !is_mut_ref_ty) {
                    error_at(e->loc, "cannot move out of global '%s'; use '.copy()' to clone", e->var.name);
                }
                if (ctx->slots[idx].is_view && !is_mut_ref_ty) {
                    error_at(e->loc, "cannot move out of borrowed value '%s'; use '.copy()' to clone", e->var.name);
                }
                if (ctx->slots[idx].imm_borrows > 0 || ctx->slots[idx].mut_borrows > 0) {
                    error_at(e->loc, "cannot move '%s' while it is borrowed", e->var.name);
                }
                if (type_is_owned(sty)) e->is_move = true;
                ctx->slots[idx].state = OWN_MOVED;
            } else {
                if (ctx->slots[idx].mut_borrows > 0) {
                    error_at(e->loc, "cannot borrow '%s' while it is mutably borrowed", e->var.name);
                }
            }
            break;
        }
        case EXPR_ATTR:
            if (e->is_ptr_take) {
                if (strcmp(e->attr.attr, "mut_ref") == 0) {
                    const char* rv = expr_root_var(e->attr.obj);
                    if (rv && !type_is_ptr(e->attr.obj->ty)) own_check_mutate_var(ctx, rv, e->loc);
                    if (e->attr.obj->kind == EXPR_VAR) {
                        int idx = own_find(ctx, e->attr.obj->var.name);
                        if (idx >= 0 && (ctx->slots[idx].state == OWN_MOVED || ctx->slots[idx].state == OWN_MAYBE_MOVED)) {
                            error_at(e->loc, "use of moved value '%s'", e->attr.obj->var.name);
                        }
                    } else {
                        own_check_expr(mod, fn, ctx, e->attr.obj, false);
                    }
                } else if (strcmp(e->attr.attr, "ref") == 0) {
                    own_check_expr(mod, fn, ctx, e->attr.obj, false);
                } else {
                    if (e->attr.obj->kind == EXPR_VAR) {
                        int idx = own_find(ctx, e->attr.obj->var.name);
                        if (idx >= 0 && (ctx->slots[idx].state == OWN_MOVED || ctx->slots[idx].state == OWN_MAYBE_MOVED)) {
                            error_at(e->loc, "use of moved value '%s'", e->attr.obj->var.name);
                        }
                    } else {
                        own_check_expr(mod, fn, ctx, e->attr.obj, false);
                    }
                }
                break;
            }
            if (want_move && type_is_owned(e->ty) && e->attr.obj->ty) {
                Type* base = type_deref_all(e->attr.obj->ty);
                if (e->attr.obj->ty->kind == TY_MODULE) {
                    error_at(e->loc, "cannot move out of module global '%s'; use '.copy()' to clone", e->attr.attr);
                }
                if (base->kind == TY_CLASS && expr_root_var(e->attr.obj)) {
                    error_at(e->loc, "cannot move out of borrowed field '%s'; use '.copy()' to clone", e->attr.attr);
                }
            }
            own_check_expr(mod, fn, ctx, e->attr.obj, false);
            break;
        case EXPR_INDEX: {
            Type* ot = e->index.obj->ty;
            if (want_move && type_is_owned(e->ty) && ot && ot->kind != TY_STR && ot->kind != TY_BYTES) {
                if (expr_root_var(e->index.obj)) {
                    error_at(e->loc, "cannot move out of indexed element; use '.copy()' to clone");
                }
            }
            own_check_expr(mod, fn, ctx, e->index.obj, false);
            own_check_expr(mod, fn, ctx, e->index.index, false);
            break;
        }
        case EXPR_SLICE:
            own_check_expr(mod, fn, ctx, e->slice.obj, false);
            own_check_expr(mod, fn, ctx, e->slice.start, false);
            own_check_expr(mod, fn, ctx, e->slice.stop, false);
            own_check_expr(mod, fn, ctx, e->slice.step, false);
            break;
        case EXPR_FSTRING:
            for (int i = 0; i < e->fstr.count; i++) {
                if (e->fstr.parts[i].is_expr) own_check_expr(mod, fn, ctx, e->fstr.parts[i].expr, false);
            }
            break;
        case EXPR_BINOP:
            own_check_expr(mod, fn, ctx, e->binop.left, false);
            own_check_expr(mod, fn, ctx, e->binop.right, false);
            break;
        case EXPR_UNOP:
            own_check_expr(mod, fn, ctx, e->unop.operand, false);
            break;
        case EXPR_AWAIT:
            own_check_expr(mod, fn, ctx, e->await_e.expr, true);
            break;
        case EXPR_TERNARY: {
            own_check_expr(mod, fn, ctx, e->ternary.cond, false);
            OwnCtx then_ctx = *ctx;
            OwnCtx else_ctx = *ctx;
            own_check_expr(mod, fn, &then_ctx, e->ternary.then_e, want_move);
            own_check_expr(mod, fn, &else_ctx, e->ternary.else_e, want_move);
            for (int i = 0; i < ctx->count; i++) {
                if (then_ctx.slots[i].state == else_ctx.slots[i].state) {
                    ctx->slots[i].state = then_ctx.slots[i].state;
                } else if (then_ctx.slots[i].state == OWN_MOVED || else_ctx.slots[i].state == OWN_MOVED ||
                           then_ctx.slots[i].state == OWN_MAYBE_MOVED || else_ctx.slots[i].state == OWN_MAYBE_MOVED) {
                    ctx->slots[i].state = OWN_MAYBE_MOVED;
                }
            }
            break;
        }
        case EXPR_WALRUS:
            own_check_expr(mod, fn, ctx, e->walrus.val, type_is_owned(e->ty));
            {
                int idx = own_ensure(ctx, e->walrus.name, e->ty, fn == NULL);
                if (idx >= 0) ctx->slots[idx].state = OWN_ALIVE;
            }
            break;
        case EXPR_LIST: case EXPR_TUPLE: case EXPR_SET:
            for (int i = 0; i < e->seq.count; i++) {
                own_check_expr(mod, fn, ctx, e->seq.items[i], type_is_owned(e->seq.items[i]->ty));
            }
            break;
        case EXPR_DICT:
            for (int i = 0; i < e->dict.count; i++) {
                own_check_expr(mod, fn, ctx, e->dict.keys[i], type_is_owned(e->dict.keys[i]->ty));
                own_check_expr(mod, fn, ctx, e->dict.vals[i], type_is_owned(e->dict.vals[i]->ty));
            }
            break;
        case EXPR_LISTCOMP: case EXPR_SETCOMP: case EXPR_GENEXPR: {
            OwnCtx comp_ctx = *ctx;
            for (int g = 0; g < e->comp.gen_count; g++) {
                CompGen* cg = &e->comp.gens[g];
                own_check_expr(mod, fn, &comp_ctx, cg->iter, false);
                for (int k = 0; k < cg->target_count; k++) {
                    int tidx = own_ensure(&comp_ctx, cg->targets[k], g_ty_i64, false);
                    if (tidx >= 0) {
                        comp_ctx.slots[tidx].state = OWN_ALIVE;
                        comp_ctx.slots[tidx].is_view = false;
                        Type* it = cg->iter->ty;
                        if (it && it->kind == TY_STR) {
                            comp_ctx.slots[tidx].type = g_ty_str;
                            comp_ctx.slots[tidx].is_view = false;
                        } else if (it && (it->kind == TY_LIST || it->kind == TY_SET) && type_is_owned(it->elem)) {
                            comp_ctx.slots[tidx].type = it->elem;
                            comp_ctx.slots[tidx].is_view = true;
                        } else if (it && it->kind == TY_DICT && type_is_owned(it->key)) {
                            comp_ctx.slots[tidx].type = it->key;
                            comp_ctx.slots[tidx].is_view = true;
                        }
                    }
                }
                for (int f = 0; f < cg->if_count; f++) own_check_expr(mod, fn, &comp_ctx, cg->ifs[f], false);
            }
            own_check_expr(mod, fn, &comp_ctx, e->comp.elt, type_is_owned(e->comp.elt->ty));
            break;
        }
        case EXPR_DICTCOMP: {
            OwnCtx comp_ctx = *ctx;
            for (int g = 0; g < e->comp.gen_count; g++) {
                CompGen* cg = &e->comp.gens[g];
                own_check_expr(mod, fn, &comp_ctx, cg->iter, false);
                for (int k = 0; k < cg->target_count; k++) {
                    int tidx = own_ensure(&comp_ctx, cg->targets[k], g_ty_i64, false);
                    if (tidx >= 0) {
                        comp_ctx.slots[tidx].state = OWN_ALIVE;
                        comp_ctx.slots[tidx].is_view = false;
                        Type* it = cg->iter->ty;
                        if (it && it->kind == TY_STR) {
                            comp_ctx.slots[tidx].type = g_ty_str;
                            comp_ctx.slots[tidx].is_view = false;
                        } else if (it && (it->kind == TY_LIST || it->kind == TY_SET) && type_is_owned(it->elem)) {
                            comp_ctx.slots[tidx].type = it->elem;
                            comp_ctx.slots[tidx].is_view = true;
                        } else if (it && it->kind == TY_DICT && type_is_owned(it->key)) {
                            comp_ctx.slots[tidx].type = it->key;
                            comp_ctx.slots[tidx].is_view = true;
                        }
                    }
                }
                for (int f = 0; f < cg->if_count; f++) own_check_expr(mod, fn, &comp_ctx, cg->ifs[f], false);
            }
            own_check_expr(mod, fn, &comp_ctx, e->comp.elt, type_is_owned(e->comp.elt->ty));
            own_check_expr(mod, fn, &comp_ctx, e->comp.val, type_is_owned(e->comp.val->ty));
            break;
        }
        case EXPR_CALL: {
            Expr* ce = e->call.callee;
            const char* tmp_bor_vars[32];
            bool tmp_bor_mut[32];
            int tmp_bor_cnt = 0;
            const char* mut_vars[32];
            int mut_cnt = 0;

            if (e->is_ptr_take) {
                if (want_move && type_is_owned(e->ty)) {
                    error_at(e->loc, "cannot move out of dereferenced pointer; use '.copy()' to clone");
                }
                if (want_move && e->ty && e->ty->kind == TY_MUT_REF && !expr_is_mutable_place(mod, fn, e)) {
                    error_at(e->loc, "cannot move mutable reference out of immutable reference");
                }
                for (int i = 0; i < e->call.arg_count; i++) own_check_expr(mod, fn, ctx, e->call.args[i], false);
                break;
            }

            #define CHECK_CALL_ARG(arg_expr, arg_want_mut, arg_want_move) do { \
                Expr* _ae = (arg_expr); \
                if (((arg_want_mut) || (_ae->ty && _ae->ty->kind == TY_MUT_REF)) && _ae->kind == EXPR_VAR) { \
                    const char* _av = _ae->var.name; \
                    for (int _k = 0; _k < mut_cnt; _k++) { \
                        if (strcmp(mut_vars[_k], _av) == 0) error_at(e->loc, "cannot borrow '%s' as mutable more than once", _av); \
                    } \
                    own_check_mutate_var(ctx, _av, _ae->loc); \
                    if (mut_cnt < 32) mut_vars[mut_cnt++] = _av; \
                } \
                own_check_expr(mod, fn, ctx, _ae, (arg_want_move)); \
                if (_ae->kind == EXPR_ATTR && _ae->is_ptr_take && strcmp(_ae->attr.attr, "addr") != 0) { \
                    const char* _rv = expr_root_var(_ae->attr.obj); \
                    int _oi = _rv ? own_find(ctx, _rv) : -1; \
                    if (_oi >= 0 && tmp_bor_cnt < 32) { \
                        bool _ism = (strcmp(_ae->attr.attr, "mut_ref") == 0); \
                        if (_ism) ctx->slots[_oi].mut_borrows++; else ctx->slots[_oi].imm_borrows++; \
                        tmp_bor_vars[tmp_bor_cnt] = _rv; tmp_bor_mut[tmp_bor_cnt] = _ism; tmp_bor_cnt++; \
                    } \
                } \
            } while (0)

            if (ce->kind == EXPR_ATTR) {
                Type* raw_ot = ce->attr.obj->ty;
                Type* ot = type_deref_all(raw_ot);
                const char* mn = ce->attr.attr;
                if (ot && (ot->kind == TY_LIST || ot->kind == TY_SET || ot->kind == TY_DICT || ot->kind == TY_FILE)) {
                    bool mut = is_mutating_container_method(ot, mn);
                    if (mut) {
                        if (type_ptr_is_const(raw_ot)) {
                            error_at(e->loc, "cannot call mutating method '%s' through immutable reference", mn);
                        }
                        const char* rv = expr_root_var(ce->attr.obj);
                        if (rv && !type_is_ptr(raw_ot)) own_check_mutate_var(ctx, rv, e->loc);
                    }
                    own_check_expr(mod, fn, ctx, ce->attr.obj, false);
                    bool arg_move = (ot->kind == TY_LIST && (strcmp(mn, "append") == 0 || strcmp(mn, "insert") == 0)) ||
                                    (ot->kind == TY_SET && strcmp(mn, "add") == 0) ||
                                    (ot->kind == TY_DICT && strcmp(mn, "setdefault") == 0);
                    for (int i = 0; i < e->call.arg_count; i++) {
                        bool mv = arg_move && (ot->kind != TY_LIST || strcmp(mn, "insert") != 0 || i == 1) &&
                                  type_is_owned(e->call.args[i]->ty);
                        CHECK_CALL_ARG(e->call.args[i], false, mv);
                    }
                    goto call_release_borrows;
                }
                if (ot && ot->kind == TY_CLASS) {
                    FuncInfo* m = e->call.resolved_fn ? e->call.resolved_fn : find_method_in_class(ot->cls, mn);
                    if (m && m->param_modes) {
                        int sp = m->is_static ? 0 : 1;
                        if (!m->is_static && !m->is_classmethod && m->param_count > 0 && m->param_modes[0] == PM_MUT_BORROW) {
                            if (type_ptr_is_const(raw_ot)) {
                                error_at(e->loc, "cannot call mutating method '%s.%s()' through immutable reference", ot->cls->name, mn);
                            }
                            const char* rv = expr_root_var(ce->attr.obj);
                            if (rv && !type_is_ptr(raw_ot)) {
                                own_check_mutate_var(ctx, rv, e->loc);
                                if (mut_cnt < 32) mut_vars[mut_cnt++] = rv;
                            }
                        }
                        if (!m->is_static && !m->is_classmethod && m->param_count > 0 &&
                            m->param_modes[0] == PM_MOVE && type_is_ptr(raw_ot)) {
                            error_at(e->loc, "cannot call consuming method '%s.%s()' through reference", ot->cls->name, mn);
                        }
                        bool self_move = !m->is_static && !m->is_classmethod && m->param_count > 0 &&
                                         m->param_modes[0] == PM_MOVE && type_is_owned(raw_ot);
                        own_check_expr(mod, fn, ctx, ce->attr.obj, self_move);
                        for (int i = 0; i < e->call.arg_count && i + sp < m->param_count; i++) {
                            ParamMode pm = m->param_modes[i + sp];
                            CHECK_CALL_ARG(e->call.args[i], pm == PM_MUT_BORROW, pm == PM_MOVE && type_is_owned(e->call.args[i]->ty));
                        }
                        goto call_release_borrows;
                    }
                }
                if (ot && ot->kind == TY_MODULE) {
                    ModuleInfo* mm = ot->mod;
                    FuncInfo* mf = e->call.resolved_fn ? e->call.resolved_fn : find_func_in_mod(mm, mn);
                    if (mf && mf->param_modes) {
                        for (int i = 0; i < e->call.arg_count && i < mf->param_count; i++) {
                            ParamMode pm = mf->param_modes[i];
                            CHECK_CALL_ARG(e->call.args[i], pm == PM_MUT_BORROW, pm == PM_MOVE && type_is_owned(e->call.args[i]->ty));
                        }
                        goto call_release_borrows;
                    }
                    ClassInfo* mci = find_class_in_mod(mm, mn);
                    if (mci) {
                        FuncInfo* init_fn = find_method_in_class(mci, "__init__");
                        for (int i = 0; i < e->call.arg_count; i++) {
                            bool mv = type_is_owned(e->call.args[i]->ty);
                            if (init_fn && init_fn->param_modes && i + 1 < init_fn->param_count) {
                                mv = (init_fn->param_modes[i + 1] == PM_MOVE) && type_is_owned(e->call.args[i]->ty);
                            }
                            CHECK_CALL_ARG(e->call.args[i], false, mv);
                        }
                        goto call_release_borrows;
                    }
                }
                own_check_expr(mod, fn, ctx, ce->attr.obj, false);
                for (int i = 0; i < e->call.arg_count; i++) CHECK_CALL_ARG(e->call.args[i], false, false);
                goto call_release_borrows;
            }
            if (ce->kind == EXPR_VAR) {
                const char* cname = ce->var.name;
                ClassInfo* ci = find_class_in_mod(mod, cname);
                if (ci) {
                    FuncInfo* init_fn = find_method_in_class(ci, "__init__");
                    for (int i = 0; i < e->call.arg_count; i++) {
                        bool mv = type_is_owned(e->call.args[i]->ty);
                        if (init_fn && init_fn->param_modes && i + 1 < init_fn->param_count) {
                            mv = (init_fn->param_modes[i + 1] == PM_MOVE) && type_is_owned(e->call.args[i]->ty);
                        }
                        CHECK_CALL_ARG(e->call.args[i], false, mv);
                    }
                    goto call_release_borrows;
                }
                FuncInfo* tf = e->call.resolved_fn ? e->call.resolved_fn : find_func_in_mod(mod, cname);
                if (!tf && fn) {
                    for (int i = 0; i < g_all_func_count; i++) {
                        if (g_all_funcs[i]->parent_fn == fn && strcmp(g_all_funcs[i]->name, cname) == 0) {
                            tf = g_all_funcs[i]; break;
                        }
                    }
                }
                if (tf && tf->param_modes) {
                    for (int i = 0; i < e->call.arg_count && i < tf->param_count; i++) {
                        ParamMode pm = tf->param_modes[i];
                        CHECK_CALL_ARG(e->call.args[i], pm == PM_MUT_BORROW, pm == PM_MOVE && type_is_owned(e->call.args[i]->ty));
                    }
                    goto call_release_borrows;
                }
            }
            for (int i = 0; i < e->call.arg_count; i++) CHECK_CALL_ARG(e->call.args[i], false, false);
        call_release_borrows:
            #undef CHECK_CALL_ARG
            for (int k = 0; k < tmp_bor_cnt; k++) {
                int oi = own_find(ctx, tmp_bor_vars[k]);
                if (oi >= 0) {
                    if (tmp_bor_mut[k]) {
                        if (ctx->slots[oi].mut_borrows > 0) ctx->slots[oi].mut_borrows--;
                    } else {
                        if (ctx->slots[oi].imm_borrows > 0) ctx->slots[oi].imm_borrows--;
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

static void own_merge_ctx(OwnCtx* dst, OwnCtx* brs, int br_count) {
    for (int i = 0; i < dst->count; i++) {
        OwnState st = brs[0].slots[i].state;
        bool diff = false;
        for (int b = 1; b < br_count; b++) {
            if (brs[b].slots[i].state != st) diff = true;
        }
        if (diff) {
            if (st == OWN_MOVED || st == OWN_MAYBE_MOVED) dst->slots[i].state = OWN_MAYBE_MOVED;
            else {
                bool any_moved = false;
                for (int b = 0; b < br_count; b++) {
                    if (brs[b].slots[i].state == OWN_MOVED || brs[b].slots[i].state == OWN_MAYBE_MOVED) any_moved = true;
                }
                dst->slots[i].state = any_moved ? OWN_MAYBE_MOVED : OWN_ALIVE;
            }
        } else {
            dst->slots[i].state = st;
        }
    }
}

static void own_check_stmts(ModuleInfo* mod, FuncInfo* fn, OwnCtx* ctx, Stmt** stmts, int count) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        switch (s->kind) {
            case STMT_RETURN:
                if (s->ret.expr) {
                    own_check_expr(mod, fn, ctx, s->ret.expr, type_is_owned(s->ret.expr->ty));
                    own_check_ret_ref(mod, fn, ctx, s->ret.expr, s->loc);
                }
                break;
            case STMT_YIELD:
                if (s->expr_s.expr) own_check_expr(mod, fn, ctx, s->expr_s.expr, type_is_owned(s->expr_s.expr->ty));
                break;
            case STMT_YIELD_FROM:
                own_check_expr(mod, fn, ctx, s->expr_s.expr, false);
                break;
            case STMT_EXPR:
                own_check_expr(mod, fn, ctx, s->expr_s.expr, false);
                break;
            case STMT_ASSIGN: {
                Expr* tgt = s->assign.target;
                if (tgt->kind == EXPR_VAR) {
                    const char* vname = tgt->var.name;
                    bool is_glob = fn ? (find_global_in_mod(mod, vname) != NULL && find_local_in_fn(fn, vname) == NULL) : true;
                    if (!s->assign.value) {
                        VarSym* vs = resolve_var(mod, fn, vname, NULL);
                        own_ensure(ctx, vname, vs ? vs->type : NULL, is_glob);
                        break;
                    }
                    VarSym* vs = resolve_var(mod, fn, vname, NULL);
                    Type* vty = vs ? vs->type : s->assign.value->ty;
                    bool can_ptr_view = (fn == NULL) || !is_glob;
                    bool is_ptr_take_bor = can_ptr_view && s->assign.value->kind == EXPR_ATTR && s->assign.value->is_ptr_take &&
                                           (strcmp(s->assign.value->attr.attr, "ref") == 0 || strcmp(s->assign.value->attr.attr, "mut_ref") == 0);
                    bool is_ref_var_copy = can_ptr_view && s->assign.value->kind == EXPR_VAR && s->assign.value->ty &&
                                           (s->assign.value->ty->kind == TY_REF ||
                                            (s->assign.value->ty->kind == TY_OPTIONAL && s->assign.value->ty->elem->kind == TY_REF));
                    bool is_mut_ref_var_move = can_ptr_view && s->assign.value->kind == EXPR_VAR && s->assign.value->ty &&
                                               (s->assign.value->ty->kind == TY_MUT_REF ||
                                                (s->assign.value->ty->kind == TY_OPTIONAL && s->assign.value->ty->elem->kind == TY_MUT_REF)) &&
                                               (!vty || vty->kind != TY_ADDR);
                    bool is_owned_bor = !is_glob && expr_is_borrow_source(s->assign.value) && type_is_owned(vty);
                    bool is_borrow = is_owned_bor || is_ptr_take_bor || is_ref_var_copy || is_mut_ref_var_move;

                    const char* xfer_owner = NULL;
                    bool xfer_mut = false;
                    bool xfer_stack = false;
                    if (is_mut_ref_var_move) {
                        int sidx = own_find(ctx, s->assign.value->var.name);
                        if (sidx >= 0 && ctx->slots[sidx].is_view) {
                            xfer_owner = ctx->slots[sidx].borrow_owner;
                            xfer_mut = ctx->slots[sidx].is_mut_borrow;
                            xfer_stack = ctx->slots[sidx].borrows_stack;
                        }
                        own_check_expr(mod, fn, ctx, s->assign.value, true);
                        if (sidx >= 0) {
                            ctx->slots[sidx].is_view = false;
                            ctx->slots[sidx].is_mut_borrow = false;
                            ctx->slots[sidx].borrows_stack = false;
                            ctx->slots[sidx].borrow_owner = NULL;
                        }
                    } else if (is_borrow) {
                        own_check_expr(mod, fn, ctx, s->assign.value, false);
                    } else {
                        own_check_expr(mod, fn, ctx, s->assign.value, type_is_owned(vty));
                    }

                    int idx = own_find(ctx, vname);
                    if (idx >= 0) {
                        if (ctx->slots[idx].imm_borrows > 0 || ctx->slots[idx].mut_borrows > 0) {
                            error_at(s->loc, "cannot reassign '%s' while it is borrowed", vname);
                        }
                        if (ctx->slots[idx].is_view) own_release_view(ctx, idx);
                    } else {
                        idx = own_ensure(ctx, vname, vty, is_glob);
                    }
                    if (idx >= 0) {
                        ctx->slots[idx].state = OWN_ALIVE;
                        ctx->slots[idx].is_view = is_borrow;
                        ctx->slots[idx].is_mut_borrow = false;
                        ctx->slots[idx].borrows_stack = false;
                        if (is_ptr_take_bor) {
                            const char* rv = expr_root_var(s->assign.value->attr.obj);
                            bool is_m = (strcmp(s->assign.value->attr.attr, "mut_ref") == 0);
                            ctx->slots[idx].is_mut_borrow = is_m;
                            ctx->slots[idx].borrows_stack = expr_borrows_stack(mod, fn, ctx, s->assign.value->attr.obj);
                            ctx->slots[idx].borrow_owner = rv;
                            if (rv) {
                                int oi = own_find(ctx, rv);
                                if (oi >= 0) {
                                    if (is_m) ctx->slots[oi].mut_borrows++;
                                    else ctx->slots[oi].imm_borrows++;
                                }
                            }
                        } else if (is_ref_var_copy) {
                            int sidx = own_find(ctx, s->assign.value->var.name);
                            if (sidx >= 0 && ctx->slots[sidx].is_view && ctx->slots[sidx].borrow_owner) {
                                const char* bo = ctx->slots[sidx].borrow_owner;
                                ctx->slots[idx].borrow_owner = bo;
                                ctx->slots[idx].borrows_stack = ctx->slots[sidx].borrows_stack;
                                int oi = own_find(ctx, bo);
                                if (oi >= 0) ctx->slots[oi].imm_borrows++;
                            } else {
                                ctx->slots[idx].is_view = false;
                                ctx->slots[idx].borrow_owner = NULL;
                            }
                        } else if (is_mut_ref_var_move) {
                            ctx->slots[idx].is_view = (xfer_owner != NULL);
                            ctx->slots[idx].is_mut_borrow = xfer_mut;
                            ctx->slots[idx].borrows_stack = xfer_stack;
                            ctx->slots[idx].borrow_owner = xfer_owner;
                        } else if (is_owned_bor) {
                            const char* rv = expr_root_var(s->assign.value);
                            ctx->slots[idx].borrow_owner = rv;
                            if (rv) {
                                int oi = own_find(ctx, rv);
                                if (oi >= 0) ctx->slots[oi].imm_borrows++;
                            }
                        } else {
                            ctx->slots[idx].borrow_owner = NULL;
                        }
                    }
                } else if (tgt->kind == EXPR_CALL && tgt->is_ptr_take) {
                    own_check_expr(mod, fn, ctx, tgt, false);
                    own_check_expr(mod, fn, ctx, s->assign.value, type_is_owned(s->assign.value->ty));
                } else if (tgt->kind == EXPR_ATTR) {
                    const char* rv = expr_root_var(tgt->attr.obj);
                    if (rv && !type_is_ptr(tgt->attr.obj->ty)) own_check_mutate_var(ctx, rv, s->loc);
                    own_check_expr(mod, fn, ctx, tgt->attr.obj, false);
                    own_check_expr(mod, fn, ctx, s->assign.value, type_is_owned(s->assign.value->ty));
                } else if (tgt->kind == EXPR_INDEX) {
                    const char* rv = expr_root_var(tgt->index.obj);
                    if (rv && !type_is_ptr(tgt->index.obj->ty)) own_check_mutate_var(ctx, rv, s->loc);
                    own_check_expr(mod, fn, ctx, tgt->index.obj, false);
                    Type* it_ot = type_deref_all(tgt->index.obj->ty);
                    bool move_key = it_ot && it_ot->kind == TY_DICT && type_is_owned(it_ot->key);
                    own_check_expr(mod, fn, ctx, tgt->index.index, move_key);
                    own_check_expr(mod, fn, ctx, s->assign.value, type_is_owned(s->assign.value->ty));
                }
                break;
            }
            case STMT_MULTI_ASSIGN:
                own_check_expr(mod, fn, ctx, s->multi_assign.value, false);
                for (int k = 0; k < s->multi_assign.target_count; k++) {
                    Expr* t = s->multi_assign.targets[k];
                    if (t->kind == EXPR_VAR) {
                        VarSym* vs = resolve_var(mod, fn, t->var.name, NULL);
                        int idx = own_ensure(ctx, t->var.name, vs ? vs->type : NULL, fn == NULL);
                        if (idx >= 0) ctx->slots[idx].state = OWN_ALIVE;
                    }
                }
                break;
            case STMT_AUG_ASSIGN: {
                Expr* tgt = s->aug_assign.target;
                const char* rv = expr_root_var(tgt);
                if (rv && !(tgt->kind == EXPR_ATTR && type_is_ptr(tgt->attr.obj->ty)) &&
                    !(tgt->kind == EXPR_INDEX && type_is_ptr(tgt->index.obj->ty))) {
                    own_check_mutate_var(ctx, rv, s->loc);
                }
                own_check_expr(mod, fn, ctx, tgt, false);
                own_check_expr(mod, fn, ctx, s->aug_assign.value, false);
                break;
            }
            case STMT_IF: {
                OwnCtx brs[32];
                int mcnt = 0;
                int bc = s->if_s.branch_count;
                for (int b = 0; b < bc && mcnt < 31; b++) {
                    own_check_expr(mod, fn, ctx, s->if_s.conds[b], false);
                    OwnCtx bctx = *ctx;
                    own_check_stmts(mod, fn, &bctx, s->if_s.bodies[b], s->if_s.body_counts[b]);
                    own_sync_to_fn(fn, &bctx);
                    if (!stmts_definitely_returns(s->if_s.bodies[b], s->if_s.body_counts[b])) {
                        brs[mcnt++] = bctx;
                    }
                }
                if (s->if_s.else_count > 0) {
                    OwnCtx ectx = *ctx;
                    own_check_stmts(mod, fn, &ectx, s->if_s.else_body, s->if_s.else_count);
                    own_sync_to_fn(fn, &ectx);
                    if (!stmts_definitely_returns(s->if_s.else_body, s->if_s.else_count) && mcnt < 32) {
                        brs[mcnt++] = ectx;
                    }
                } else if (mcnt < 32) {
                    brs[mcnt++] = *ctx;
                }
                if (mcnt > 0) own_merge_ctx(ctx, brs, mcnt);
                break;
            }
            case STMT_WHILE: {
                own_check_expr(mod, fn, ctx, s->while_s.cond, false);
                OwnCtx body_ctx = *ctx;
                body_ctx.loop_depth++;
                own_check_stmts(mod, fn, &body_ctx, s->while_s.body, s->while_s.body_count);
                own_sync_to_fn(fn, &body_ctx);
                for (int k = 0; k < ctx->count; k++) {
                    if (type_is_owned(ctx->slots[k].type) && ctx->slots[k].state == OWN_ALIVE &&
                        (body_ctx.slots[k].state == OWN_MOVED || body_ctx.slots[k].state == OWN_MAYBE_MOVED)) {
                        error_at(s->loc, "value '%s' moved inside loop without being reassigned", ctx->slots[k].name);
                    }
                }
                if (s->while_s.else_count > 0) {
                    own_check_stmts(mod, fn, ctx, s->while_s.else_body, s->while_s.else_count);
                }
                break;
            }
            case STMT_FOR: {
                own_check_expr(mod, fn, ctx, s->for_s.iter, false);
                const char* iter_owner = (s->for_s.iter->kind == EXPR_VAR) ? s->for_s.iter->var.name : NULL;
                int iter_idx = iter_owner ? own_find(ctx, iter_owner) : -1;
                if (iter_idx >= 0) ctx->slots[iter_idx].imm_borrows++;

                OwnCtx body_ctx = *ctx;
                body_ctx.loop_depth++;
                bool iter_allocs_str = (s->for_s.iter->ty && s->for_s.iter->ty->kind == TY_STR);
                for (int k = 0; k < s->for_s.target_count; k++) {
                    VarSym* tv = resolve_var(mod, fn, s->for_s.targets[k], NULL);
                    Type* tty = tv ? tv->type : NULL;
                    int tidx = own_ensure(&body_ctx, s->for_s.targets[k], tty, fn == NULL);
                    if (tidx >= 0) {
                        body_ctx.slots[tidx].state = OWN_ALIVE;
                        if (type_is_owned(tty) && !iter_allocs_str) {
                            body_ctx.slots[tidx].is_view = true;
                            body_ctx.slots[tidx].borrow_owner = iter_owner;
                        } else {
                            body_ctx.slots[tidx].is_view = false;
                            body_ctx.slots[tidx].borrow_owner = NULL;
                        }
                    }
                }
                own_check_stmts(mod, fn, &body_ctx, s->for_s.body, s->for_s.body_count);
                own_sync_to_fn(fn, &body_ctx);
                for (int k = 0; k < ctx->count; k++) {
                    if (type_is_owned(ctx->slots[k].type) && ctx->slots[k].state == OWN_ALIVE &&
                        (body_ctx.slots[k].state == OWN_MOVED || body_ctx.slots[k].state == OWN_MAYBE_MOVED)) {
                        error_at(s->loc, "value '%s' moved inside loop without being reassigned", ctx->slots[k].name);
                    }
                }
                if (iter_idx >= 0 && ctx->slots[iter_idx].imm_borrows > 0) ctx->slots[iter_idx].imm_borrows--;
                if (s->for_s.else_count > 0) own_check_stmts(mod, fn, ctx, s->for_s.else_body, s->for_s.else_count);
                break;
            }
            case STMT_TRY: {
                OwnCtx brs[32];
                brs[0] = *ctx;
                own_check_stmts(mod, fn, &brs[0], s->try_s.try_body, s->try_s.try_count);
                own_sync_to_fn(fn, &brs[0]);
                int bc = 1;
                for (int h = 0; h < s->try_s.handler_count && bc < 32; h++) {
                    brs[bc] = *ctx;
                    if (s->try_s.handlers[h].as_name) {
                        VarSym* ev = resolve_var(mod, fn, s->try_s.handlers[h].as_name, NULL);
                        int eidx = own_ensure(&brs[bc], s->try_s.handlers[h].as_name, ev ? ev->type : NULL, fn == NULL);
                        if (eidx >= 0) {
                            brs[bc].slots[eidx].state = OWN_ALIVE;
                            brs[bc].slots[eidx].is_view = true;
                        }
                    }
                    own_check_stmts(mod, fn, &brs[bc], s->try_s.handlers[h].body, s->try_s.handlers[h].body_count);
                    own_sync_to_fn(fn, &brs[bc]);
                    bc++;
                }
                own_merge_ctx(ctx, brs, bc);
                if (s->try_s.else_count > 0) own_check_stmts(mod, fn, ctx, s->try_s.else_body, s->try_s.else_count);
                if (s->try_s.finally_count > 0) own_check_stmts(mod, fn, ctx, s->try_s.finally_body, s->try_s.finally_count);
                break;
            }
            case STMT_WITH: {
                own_check_expr(mod, fn, ctx, s->with_s.ctx, false);
                if (s->with_s.as_name) {
                    VarSym* av = resolve_var(mod, fn, s->with_s.as_name, NULL);
                    int aidx = own_ensure(ctx, s->with_s.as_name, av ? av->type : NULL, fn == NULL);
                    if (aidx >= 0) {
                        ctx->slots[aidx].state = OWN_ALIVE;
                        ctx->slots[aidx].is_view = (s->with_s.ctx->ty && s->with_s.ctx->ty->kind == TY_FILE);
                        ctx->slots[aidx].is_mut_borrow = (s->with_s.ctx->ty && s->with_s.ctx->ty->kind == TY_FILE);
                    }
                }
                own_check_stmts(mod, fn, ctx, s->with_s.body, s->with_s.body_count);
                break;
            }
            case STMT_DEL:
                if (s->del_s.target->kind == EXPR_VAR) {
                    const char* vn = s->del_s.target->var.name;
                    own_check_mutate_var(ctx, vn, s->loc);
                    int idx = own_find(ctx, vn);
                    if (idx >= 0) ctx->slots[idx].state = OWN_MOVED;
                } else if (s->del_s.target->kind == EXPR_INDEX) {
                    const char* rv = expr_root_var(s->del_s.target->index.obj);
                    if (rv) own_check_mutate_var(ctx, rv, s->loc);
                    own_check_expr(mod, fn, ctx, s->del_s.target->index.obj, false);
                    own_check_expr(mod, fn, ctx, s->del_s.target->index.index, false);
                }
                break;
            case STMT_ASSERT:
                own_check_expr(mod, fn, ctx, s->assert_s.cond, false);
                own_check_expr(mod, fn, ctx, s->assert_s.msg, false);
                break;
            case STMT_RAISE:
                if (s->raise_s.exc) own_check_expr(mod, fn, ctx, s->raise_s.exc, type_is_owned(s->raise_s.exc->ty));
                break;
            case STMT_MATCH: {
                own_check_expr(mod, fn, ctx, s->match_s.subject, false);
                OwnCtx brs[32];
                int mcnt = 0;
                int cc = s->match_s.case_count;
                for (int c = 0; c < cc && c < 32; c++) {
                    own_check_expr(mod, fn, ctx, s->match_s.cases[c].pattern, false);
                    OwnCtx cctx = *ctx;
                    if (s->match_s.cases[c].capture) {
                        VarSym* cv = resolve_var(mod, fn, s->match_s.cases[c].capture, NULL);
                        int cidx = own_ensure(&cctx, s->match_s.cases[c].capture, cv ? cv->type : NULL, fn == NULL);
                        if (cidx >= 0) {
                            cctx.slots[cidx].state = OWN_ALIVE;
                            cctx.slots[cidx].is_view = true;
                        }
                    }
                    own_check_stmts(mod, fn, &cctx, s->match_s.cases[c].body, s->match_s.cases[c].body_count);
                    own_sync_to_fn(fn, &cctx);
                    if (!stmts_definitely_returns(s->match_s.cases[c].body, s->match_s.cases[c].body_count)) {
                        brs[mcnt++] = cctx;
                    }
                }
                if (mcnt > 0) own_merge_ctx(ctx, brs, mcnt);
                break;
            }
            default: break;
        }
        own_end_dead_views(ctx, stmts + i + 1, count - i - 1);
        own_sync_to_fn(fn, ctx);
    }
}

static void check_program_ownership(void) {
    analyze_ownership_modes();
    for (int i = 0; i < g_all_func_count; i++) {
        FuncInfo* fn = g_all_funcs[i];
        if (fn->is_generic || fn->is_extern) continue;
        OwnCtx ctx;
        memset(&ctx, 0, sizeof(ctx));
        for (int p = 0; p < fn->param_count; p++) {
            int idx = own_ensure(&ctx, fn->param_names[p], fn->param_types[p], false);
            if (idx >= 0) {
                ctx.slots[idx].state = OWN_ALIVE;
                bool is_owned_p = type_is_owned(fn->param_types[p]);
                ctx.slots[idx].is_view = (is_owned_p && fn->param_modes && fn->param_modes[p] != PM_MOVE);
                ctx.slots[idx].is_mut_borrow = (is_owned_p && fn->param_modes && fn->param_modes[p] == PM_MUT_BORROW);
            }
        }
        if (fn->is_lambda && fn->lambda_ast) {
            own_check_expr(fn->mod, fn, &ctx, fn->lambda_ast->lambda.body, type_is_owned(fn->ret_type));
        } else if (fn->ast) {
            own_check_stmts(fn->mod, fn, &ctx, fn->ast->def_s.body, fn->ast->def_s.body_count);
            own_sync_to_fn(fn, &ctx);
        }
    }
    for (int m = 0; m < g_module_count; m++) {
        ModuleInfo* mod = g_modules[m];
        OwnCtx mctx;
        memset(&mctx, 0, sizeof(mctx));
        own_check_stmts(mod, NULL, &mctx, mod->stmts, mod->stmt_count);
    }
}

/* =========================================================================
 * 5. LLVM IR CODE GENERATOR
 * ========================================================================= */

typedef struct {
    const char* break_lbl;
    const char* cont_lbl;
    bool* broke_flag;
    const char* broke_alloca;
    int body_temp_mark;
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
    const char* fn_unwind_lbl;
    const char* cur_unwind_lbl;
    LoopCtx loop_stack[64];
    int loop_depth;
    /* String literal deduplication */
    const char** lit_texts;
    size_t* lit_lens;
    const char** lit_syms;
    int lit_count;
    int lit_cap;
    /* Track which hash/eq/drop/clone helpers were emitted */
    TypeRegistry emitted_hash_eq;
    TypeRegistry emitted_drops;
    TypeRegistry emitted_clones;
    /* Track declared globals/allocas in current func */
    const char** fn_allocas;
    int fn_alloca_count;
    /* Track unconsumed heap temporaries in current statement */
    struct { const char* reg; Type* ty; bool active; } temps[128];
    int temp_count;
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
        case TY_LIST: case TY_SET: case TY_DICT: case TY_TASK: case TY_FUNC:
        case TY_REF: case TY_MUT_REF: case TY_ADDR:
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

static const char* llvm_fn_ret_ty(FuncInfo* fn) {
    if (!fn || !fn->can_raise) return llvm_reg_ty(fn ? fn->ret_type : NULL);
    if (!fn->ret_type || fn->ret_type->kind == TY_VOID) return "i1";
    return arena_sprintf("{ i1, %s }", llvm_reg_ty(fn->ret_type));
}

static bool cg_has_active_temps(int mark);
static void cg_flush_temps_preserve(int mark);

static void cg_raise_branch(void) {
    if (g_cg.block_terminated) return;
    if (g_cg.cur_unwind_lbl) {
        cg_flush_temps_preserve(0);
        cg_br(g_cg.cur_unwind_lbl);
    } else {
        cg_emit("call void @stricta_unhandled_exc()");
        cg_emit("unreachable");
        g_cg.block_terminated = true;
    }
}

static const char* cg_emit_call_check(FuncInfo* target_fn, const char* call_sym, const char* args_str) {
    if (target_fn->is_async) {
        const char* bare = (call_sym[0] == '@') ? call_sym + 1 : call_sym;
        const char* r = cg_reg();
        cg_emit("%s = call ptr @_async_spawn_%s(%s)", r, bare, args_str);
        return r;
    }
    if (!target_fn->can_raise) {
        if (target_fn->ret_type->kind == TY_VOID) {
            cg_emit("call void %s(%s)", call_sym, args_str);
            return "0";
        }
        const char* r = cg_reg();
        cg_emit("%s = call %s %s(%s)", r, llvm_reg_ty(target_fn->ret_type), call_sym, args_str);
        return r;
    }
    const char* rty = llvm_fn_ret_ty(target_fn);
    const char* raw = cg_reg();
    cg_emit("%s = call %s %s(%s)", raw, rty, call_sym, args_str);
    const char* is_exc = raw;
    if (target_fn->ret_type->kind != TY_VOID) {
        is_exc = cg_reg();
        cg_emit("%s = extractvalue %s %s, 0", is_exc, rty, raw);
    }
    const char* l_ok = cg_lbl("call_ok");
    if (g_cg.cur_unwind_lbl) {
        if (cg_has_active_temps(0)) {
            const char* l_ed = cg_lbl("call_ed");
            cg_cond_br(is_exc, l_ed, l_ok);
            cg_label(l_ed);
            cg_flush_temps_preserve(0);
            cg_br(g_cg.cur_unwind_lbl);
        } else {
            cg_cond_br(is_exc, g_cg.cur_unwind_lbl, l_ok);
        }
    } else {
        const char* l_unh = cg_lbl("call_unh");
        cg_cond_br(is_exc, l_unh, l_ok);
        cg_label(l_unh);
        cg_emit("call void @stricta_unhandled_exc()");
        cg_emit("unreachable");
        g_cg.block_terminated = true;
    }
    cg_label(l_ok);
    if (target_fn->ret_type->kind == TY_VOID) return "0";
    const char* val = cg_reg();
    cg_emit("%s = extractvalue %s %s, 1", val, rty, raw);
    return val;
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
    if (type_is_ptr(from) && type_is_int(to)) {
        const char* iv = cg_reg();
        cg_emit("%s = ptrtoint ptr %s to i64", iv, val);
        return cg_coerce(iv, g_ty_u64, to);
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
    if (type_is_ptr(t)) {
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
    sb_printf(&g_cg.strs, "%s = private constant %%StrictaStr { i32 %zu, i32 %u, ptr %s, i32 1 }\n",
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
    } else if (t->kind == TY_OPTIONAL && !type_is_heap_ref(t->elem)) {
        emit_hash_eq_for_type(t->elem);
    }

    const char* mty = llvm_mem_ty(t);
    const char* m = t->mangled;
    StrBuf* b = &g_cg.mono;

    /* Hash helper */
    sb_printf(b, "define internal i64 @_hash_%s(%s %%v) alwaysinline {\nentry:\n", m, mty);
    if (t->kind == TY_OPTIONAL && !type_is_heap_ref(t->elem)) {
        sb_printf(b, "  %%tag = extractvalue %s %%v, 0\n  %%is_n = icmp eq i8 %%tag, 0\n", mty);
        sb_append(b, "  br i1 %is_n, label %none, label %some\nnone:\n  ret i64 0\nsome:\n");
        sb_printf(b, "  %%ev = extractvalue %s %%v, 1\n  %%h = call i64 @_hash_%s(%s %%ev)\n  ret i64 %%h\n}\n\n",
                  mty, t->elem->mangled, llvm_mem_ty(t->elem));
    } else if (type_is_int(t) || t->kind == TY_BOOL) {
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
    if (t->kind == TY_OPTIONAL && !type_is_heap_ref(t->elem)) {
        sb_printf(b, "  %%ta = extractvalue %s %%a, 0\n  %%tb = extractvalue %s %%b, 0\n", mty, mty);
        sb_append(b, "  %teq = icmp eq i8 %ta, %tb\n  br i1 %teq, label %same_tag, label %diff\ndiff:\n  ret i1 false\nsame_tag:\n");
        sb_append(b, "  %is_n = icmp eq i8 %ta, 0\n  br i1 %is_n, label %both_n, label %both_s\nboth_n:\n  ret i1 true\nboth_s:\n");
        sb_printf(b, "  %%va = extractvalue %s %%a, 1\n  %%vb = extractvalue %s %%b, 1\n", mty, mty);
        sb_printf(b, "  %%r = call i1 @_eq_%s(%s %%va, %s %%vb)\n  ret i1 %%r\n}\n\n",
                  t->elem->mangled, llvm_mem_ty(t->elem), llvm_mem_ty(t->elem));
    } else if (type_is_int(t) || t->kind == TY_BOOL) {
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

static void emit_drop_for_type(Type* t);
static void emit_clone_for_type(Type* t);

static void emit_drop_for_type(Type* t) {
    if (!t || !type_is_owned(t)) return;
    for (int i = 0; i < g_cg.emitted_drops.count; i++) {
        if (type_eq(g_cg.emitted_drops.items[i], t)) return;
    }
    reg_add_unique(&g_cg.emitted_drops, t);

    if (t->kind == TY_OPTIONAL && !type_is_heap_ref(t->elem)) reg_add_unique(&g_mono_opts, t);
    else if (t->kind == TY_TUPLE) reg_add_unique(&g_mono_tuples, t);
    else if (t->kind == TY_LIST) reg_add_unique(&g_mono_lists, t);
    else if (t->kind == TY_SET) reg_add_unique(&g_mono_sets, t);
    else if (t->kind == TY_DICT) reg_add_unique(&g_mono_dicts, t);

    if (t->kind == TY_OPTIONAL) emit_drop_for_type(t->elem);
    else if (t->kind == TY_LIST || t->kind == TY_SET) emit_drop_for_type(t->elem);
    else if (t->kind == TY_DICT) { emit_drop_for_type(t->key); emit_drop_for_type(t->val); }
    else if (t->kind == TY_TUPLE) {
        for (int i = 0; i < t->tuple_count; i++) emit_drop_for_type(t->tuple_elems[i]);
    } else if (t->kind == TY_CLASS) {
        for (int i = 0; i < t->cls->field_count; i++) emit_drop_for_type(t->cls->fields[i].type);
        if (t->cls->has_subclasses) {
            for (int c = 0; c < g_all_class_count; c++) {
                ClassInfo* sub = g_all_classes[c];
                if (sub != t->cls && class_is_subclass(sub, t->cls)) emit_drop_for_type(sub->type);
            }
        }
    }

    const char* m = t->mangled;
    const char* mty = llvm_mem_ty(t);
    StrBuf* b = &g_cg.mono;

    if (t->kind == TY_STR) {
        sb_printf(b, "define internal void @_drop_%s(ptr %%v) alwaysinline {\nentry:\n  call void @stricta_str_free(ptr %%v)\n  ret void\n}\n\n", m);
        return;
    }
    if (t->kind == TY_BYTES) {
        sb_printf(b, "define internal void @_drop_%s(ptr %%v) alwaysinline {\nentry:\n  call void @stricta_bytes_free(ptr %%v)\n  ret void\n}\n\n", m);
        return;
    }
    if (t->kind == TY_BIGINT) {
        sb_printf(b, "define internal void @_drop_%s(ptr %%v) alwaysinline {\nentry:\n  call void @stricta_bigint_free(ptr %%v)\n  ret void\n}\n\n", m);
        return;
    }
    if (t->kind == TY_FILE) {
        sb_printf(b, "define internal void @_drop_%s(ptr %%v) alwaysinline {\nentry:\n  call void @stricta_file_free(ptr %%v)\n  ret void\n}\n\n", m);
        return;
    }
    if (t->kind == TY_TASK) {
        sb_printf(b, "define internal void @_drop_%s(ptr %%v) alwaysinline {\nentry:\n  call void @stricta_task_free(ptr %%v)\n  ret void\n}\n\n", m);
        return;
    }
    if (t->kind == TY_OPTIONAL) {
        if (type_is_heap_ref(t->elem)) {
            sb_printf(b, "define internal void @_drop_%s(ptr %%v) alwaysinline {\nentry:\n  %s", m, "");
            sb_printf(b, "call void @_drop_%s(ptr %%v)\n  ret void\n}\n\n", t->elem->mangled);
        } else {
            const char* emty = llvm_mem_ty(t->elem);
            sb_printf(b, "define internal void @_drop_%s(%s %%v) alwaysinline {\nentry:\n", m, mty);
            sb_printf(b, "  %%tag = extractvalue %s %%v, 0\n  %%ok = icmp ne i8 %%tag, 0\n  br i1 %%ok, label %%some, label %%done\nsome:\n", mty);
            sb_printf(b, "  %%ev = extractvalue %s %%v, 1\n  call void @_drop_%s(%s %%ev)\n  br label %%done\ndone:\n  ret void\n}\n\n",
                      mty, t->elem->mangled, emty);
        }
        return;
    }
    if (t->kind == TY_TUPLE) {
        sb_printf(b, "define internal void @_drop_%s(%s %%v) alwaysinline {\nentry:\n", m, mty);
        for (int i = 0; i < t->tuple_count; i++) {
            if (type_is_owned(t->tuple_elems[i])) {
                sb_printf(b, "  %%f%d = extractvalue %s %%v, %d\n  call void @_drop_%s(%s %%f%d)\n",
                          i, mty, i, t->tuple_elems[i]->mangled, llvm_mem_ty(t->tuple_elems[i]), i);
            }
        }
        sb_append(b, "  ret void\n}\n\n");
        return;
    }
    if (t->kind == TY_LIST) {
        const char* sty = arena_sprintf("%%%s", m);
        const char* emty = llvm_mem_ty(t->elem);
        sb_printf(b, "define internal void @_drop_fields_%s(ptr %%l) {\nentry:\n", m);
        sb_printf(b, "  %%chk = icmp eq ptr %%l, null\n  br i1 %%chk, label %%done, label %%go\ngo:\n");
        sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
        if (type_is_owned(t->elem)) {
            sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len = load i32, ptr %%lp\n  br label %%loop\n", sty);
            sb_append(b, "loop:\n  %i = phi i32 [ 0, %go ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %after_elems\nbody:\n");
            sb_printf(b, "  %%ep = getelementptr %s, ptr %%arr, i32 %%i\n  %%ev = load %s, ptr %%ep\n  call void @_drop_%s(%s %%ev)\n  %%in = add i32 %%i, 1\n  br label %%loop\nafter_elems:\n",
                      emty, emty, t->elem->mangled, emty);
        }
        sb_printf(b, "  %%cp = getelementptr %s, ptr %%l, i32 0, i32 1\n  %%cap = load i32, ptr %%cp\n  %%is_heap = icmp sgt i32 %%cap, 0\n  br i1 %%is_heap, label %%free_buf, label %%done\nfree_buf:\n  call void @free(ptr %%arr)\n  br label %%done\ndone:\n  ret void\n}\n\n", sty);
        sb_printf(b, "define internal void @_drop_%s(ptr %%l) {\nentry:\n  %%z = icmp eq ptr %%l, null\n  br i1 %%z, label %%done, label %%go\ngo:\n  call void @_drop_fields_%s(ptr %%l)\n  call void @free(ptr %%l)\n  br label %%done\ndone:\n  ret void\n}\n\n", m, m);
        return;
    }
    if (t->kind == TY_SET) {
        const char* sty = arena_sprintf("%%%s", m);
        const char* emty = llvm_mem_ty(t->elem);
        sb_printf(b, "define internal void @_drop_%s(ptr %%s) {\nentry:\n  %%z = icmp eq ptr %%s, null\n  br i1 %%z, label %%done, label %%go\ngo:\n", m);
        sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%items = load ptr, ptr %%p0\n", sty);
        sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  %%buckets = load ptr, ptr %%p1\n", sty);
        if (type_is_owned(t->elem)) {
            sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  %%len = load i32, ptr %%p2\n  br label %%loop\n", sty);
            sb_append(b, "loop:\n  %i = phi i32 [ 0, %go ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %after_elems\nbody:\n");
            sb_printf(b, "  %%ep = getelementptr %s, ptr %%items, i32 %%i\n  %%ev = load %s, ptr %%ep\n  call void @_drop_%s(%s %%ev)\n  %%in = add i32 %%i, 1\n  br label %%loop\nafter_elems:\n",
                      emty, emty, t->elem->mangled, emty);
        }
        sb_append(b, "  call void @stricta_free(ptr %items)\n  call void @stricta_free(ptr %buckets)\n  call void @stricta_free(ptr %s)\n  br label %done\ndone:\n  ret void\n}\n\n");
        return;
    }
    if (t->kind == TY_DICT) {
        const char* sty = arena_sprintf("%%%s", m);
        const char* kmty = llvm_mem_ty(t->key);
        const char* vmty = llvm_mem_ty(t->val);
        sb_printf(b, "define internal void @_drop_%s(ptr %%d) {\nentry:\n  %%z = icmp eq ptr %%d, null\n  br i1 %%z, label %%done, label %%go\ngo:\n", m);
        sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
        sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
        sb_printf(b, "  %%p2 = getelementptr %s, ptr %%d, i32 0, i32 2\n  %%buckets = load ptr, ptr %%p2\n", sty);
        if (type_is_owned(t->key) || type_is_owned(t->val)) {
            sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n  br label %%loop\n", sty);
            sb_append(b, "loop:\n  %i = phi i32 [ 0, %go ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %after_elems\nbody:\n");
            if (type_is_owned(t->key)) {
                sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%i\n  %%kv = load %s, ptr %%kp\n  call void @_drop_%s(%s %%kv)\n",
                          kmty, kmty, t->key->mangled, kmty);
            }
            if (type_is_owned(t->val)) {
                sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%i\n  %%vv = load %s, ptr %%vp\n  call void @_drop_%s(%s %%vv)\n",
                          vmty, vmty, t->val->mangled, vmty);
            }
            sb_append(b, "  %in = add i32 %i, 1\n  br label %loop\nafter_elems:\n");
        }
        sb_append(b, "  call void @stricta_free(ptr %keys)\n  call void @stricta_free(ptr %vals)\n  call void @stricta_free(ptr %buckets)\n  call void @stricta_free(ptr %d)\n  br label %done\ndone:\n  ret void\n}\n\n");
        return;
    }
    if (t->kind == TY_CLASS) {
        ClassInfo* cls = t->cls;
        if (cls->is_value_type) {
            sb_printf(b, "define internal void @_drop_%s(%s %%v) alwaysinline {\nentry:\n", m, mty);
            for (int f = 0; f < cls->field_count; f++) {
                if (type_is_owned(cls->fields[f].type)) {
                    sb_printf(b, "  %%f%d = extractvalue %s %%v, %d\n  call void @_drop_%s(%s %%f%d)\n",
                              f, mty, cls->fields[f].slot_index, cls->fields[f].type->mangled, llvm_mem_ty(cls->fields[f].type), f);
                }
            }
            sb_append(b, "  ret void\n}\n\n");
            return;
        }
        sb_printf(b, "define internal void @_drop_fields_%s(ptr %%p) {\nentry:\n  %%z = icmp eq ptr %%p, null\n  br i1 %%z, label %%done, label %%go\ngo:\n", m);
        for (int f = 0; f < cls->field_count; f++) {
            Type* ft = cls->fields[f].type;
            if (type_is_owned(ft)) {
                const char* fmty = llvm_mem_ty(ft);
                sb_printf(b, "  %%fp%d = getelementptr %s, ptr %%p, i32 0, i32 %d\n  %%fv%d = load %s, ptr %%fp%d\n  call void @_drop_%s(%s %%fv%d)\n",
                          f, cls->llvm_name, cls->fields[f].slot_index, f, fmty, f, ft->mangled, fmty, f);
            }
        }
        sb_append(b, "  br label %done\ndone:\n  ret void\n}\n\n");
        sb_printf(b, "define internal void @_drop_%s(ptr %%p) {\nentry:\n  %%z = icmp eq ptr %%p, null\n  br i1 %%z, label %%done, label %%go\ngo:\n", m);
        if (cls->needs_vtable && cls->has_subclasses) {
            sb_printf(b, "  %%vpp = getelementptr %s, ptr %%p, i32 0, i32 0\n  %%vt = load ptr, ptr %%vpp\n  %%tid = load i32, ptr %%vt\n", cls->llvm_name);
            sb_append(b, "  switch i32 %tid, label %def [");
            for (int c = 0; c < g_all_class_count; c++) {
                ClassInfo* sub = g_all_classes[c];
                if (sub != cls && class_is_subclass(sub, cls)) {
                    sb_printf(b, " i32 %d, label %%sub_%d", sub->type_id, sub->type_id);
                }
            }
            sb_append(b, " ]\n");
            for (int c = 0; c < g_all_class_count; c++) {
                ClassInfo* sub = g_all_classes[c];
                if (sub != cls && class_is_subclass(sub, cls)) {
                    sb_printf(b, "sub_%d:\n  call void @_drop_fields_%s(ptr %%p)\n  br label %%after_f\n",
                              sub->type_id, sub->type->mangled);
                }
            }
            sb_printf(b, "def:\n  call void @_drop_fields_%s(ptr %%p)\n  br label %%after_f\nafter_f:\n", m);
        } else {
            sb_printf(b, "  call void @_drop_fields_%s(ptr %%p)\n", m);
        }
        sb_append(b, "  call void @free(ptr %p)\n  br label %done\ndone:\n  ret void\n}\n\n");
    }
}

static void emit_clone_for_type(Type* t) {
    if (!t || !type_is_owned(t)) return;
    for (int i = 0; i < g_cg.emitted_clones.count; i++) {
        if (type_eq(g_cg.emitted_clones.items[i], t)) return;
    }
    reg_add_unique(&g_cg.emitted_clones, t);

    if (t->kind == TY_OPTIONAL && !type_is_heap_ref(t->elem)) reg_add_unique(&g_mono_opts, t);
    else if (t->kind == TY_TUPLE) reg_add_unique(&g_mono_tuples, t);
    else if (t->kind == TY_LIST) reg_add_unique(&g_mono_lists, t);
    else if (t->kind == TY_SET) reg_add_unique(&g_mono_sets, t);
    else if (t->kind == TY_DICT) reg_add_unique(&g_mono_dicts, t);

    if (t->kind == TY_OPTIONAL) emit_clone_for_type(t->elem);
    else if (t->kind == TY_TUPLE) {
        for (int i = 0; i < t->tuple_count; i++) emit_clone_for_type(t->tuple_elems[i]);
    } else if (t->kind == TY_CLASS) {
        for (int i = 0; i < t->cls->field_count; i++) emit_clone_for_type(t->cls->fields[i].type);
    }

    const char* m = t->mangled;
    const char* mty = llvm_mem_ty(t);
    StrBuf* b = &g_cg.mono;

    if (t->kind == TY_STR) {
        sb_printf(b, "define internal ptr @_clone_%s(ptr %%v) alwaysinline {\nentry:\n  %%r = call ptr @stricta_str_clone(ptr %%v)\n  ret ptr %%r\n}\n\n", m);
        return;
    }
    if (t->kind == TY_BYTES) {
        sb_printf(b, "define internal ptr @_clone_%s(ptr %%v) alwaysinline {\nentry:\n  %%r = call ptr @stricta_bytes_clone(ptr %%v)\n  ret ptr %%r\n}\n\n", m);
        return;
    }
    if (t->kind == TY_BIGINT) {
        sb_printf(b, "define internal ptr @_clone_%s(ptr %%v) alwaysinline {\nentry:\n  %%r = call ptr @stricta_bigint_clone(ptr %%v)\n  ret ptr %%r\n}\n\n", m);
        return;
    }
    if (t->kind == TY_FILE || t->kind == TY_TASK) {
        sb_printf(b, "define internal ptr @_clone_%s(ptr %%v) alwaysinline {\nentry:\n  ret ptr %%v\n}\n\n", m);
        return;
    }
    if (t->kind == TY_LIST || t->kind == TY_SET || t->kind == TY_DICT) {
        sb_printf(b, "define internal ptr @_clone_%s(ptr %%v) alwaysinline {\nentry:\n  %%z = icmp eq ptr %%v, null\n  br i1 %%z, label %%rn, label %%rc\nrn:\n  ret ptr null\nrc:\n  %%r = call ptr @_%s_copy(ptr %%v)\n  ret ptr %%r\n}\n\n", m, m);
        return;
    }
    if (t->kind == TY_OPTIONAL) {
        if (type_is_heap_ref(t->elem)) {
            sb_printf(b, "define internal ptr @_clone_%s(ptr %%v) alwaysinline {\nentry:\n  %%z = icmp eq ptr %%v, null\n  br i1 %%z, label %%rn, label %%rc\nrn:\n  ret ptr null\nrc:\n  %%r = call ptr @_clone_%s(ptr %%v)\n  ret ptr %%r\n}\n\n",
                      m, t->elem->mangled);
        } else {
            const char* emty = llvm_mem_ty(t->elem);
            sb_printf(b, "define internal %s @_clone_%s(%s %%v) alwaysinline {\nentry:\n", mty, m, mty);
            sb_printf(b, "  %%tag = extractvalue %s %%v, 0\n  %%ok = icmp ne i8 %%tag, 0\n  br i1 %%ok, label %%some, label %%none\nnone:\n  ret %s %%v\nsome:\n", mty, mty);
            sb_printf(b, "  %%ev = extractvalue %s %%v, 1\n  %%cv = call %s @_clone_%s(%s %%ev)\n",
                      mty, emty, t->elem->mangled, emty);
            sb_printf(b, "  %%res = insertvalue %s %%v, %s %%cv, 1\n  ret %s %%res\n}\n\n", mty, emty, mty);
        }
        return;
    }
    if (t->kind == TY_TUPLE) {
        sb_printf(b, "define internal %s @_clone_%s(%s %%v) alwaysinline {\nentry:\n", mty, m, mty);
        const char* cur = "%v";
        for (int i = 0; i < t->tuple_count; i++) {
            if (type_is_owned(t->tuple_elems[i])) {
                const char* emty = llvm_mem_ty(t->tuple_elems[i]);
                sb_printf(b, "  %%f%d = extractvalue %s %s, %d\n  %%cf%d = call %s @_clone_%s(%s %%f%d)\n  %%t%d = insertvalue %s %s, %s %%cf%d, %d\n",
                          i, mty, cur, i, i, emty, t->tuple_elems[i]->mangled, emty, i, i, mty, cur, emty, i, i);
                cur = arena_sprintf("%%t%d", i);
            }
        }
        sb_printf(b, "  ret %s %s\n}\n\n", mty, cur);
        return;
    }
    if (t->kind == TY_CLASS) {
        ClassInfo* cls = t->cls;
        if (cls->is_value_type) {
            sb_printf(b, "define internal %s @_clone_%s(%s %%v) alwaysinline {\nentry:\n", mty, m, mty);
            const char* cur = "%v";
            for (int f = 0; f < cls->field_count; f++) {
                Type* ft = cls->fields[f].type;
                if (type_is_owned(ft)) {
                    const char* fmty = llvm_mem_ty(ft);
                    sb_printf(b, "  %%f%d = extractvalue %s %s, %d\n  %%cf%d = call %s @_clone_%s(%s %%f%d)\n  %%t%d = insertvalue %s %s, %s %%cf%d, %d\n",
                              f, mty, cur, cls->fields[f].slot_index, f, fmty, ft->mangled, fmty, f, f, mty, cur, fmty, f, cls->fields[f].slot_index);
                    cur = arena_sprintf("%%t%d", f);
                }
            }
            sb_printf(b, "  ret %s %s\n}\n\n", mty, cur);
            return;
        }
        sb_printf(b, "define internal ptr @_clone_%s(ptr %%p) {\nentry:\n  %%z = icmp eq ptr %%p, null\n  br i1 %%z, label %%rn, label %%rc\nrn:\n  ret ptr null\nrc:\n", m);
        sb_printf(b, "  %%sz = ptrtoint ptr getelementptr (%s, ptr null, i32 1) to i64\n  %%np = call ptr @malloc(i64 %%sz)\n", cls->llvm_name);
        sb_append(b, "  call void @llvm.memcpy.p0.p0.i64(ptr %np, ptr %p, i64 %sz, i1 false)\n");
        for (int f = 0; f < cls->field_count; f++) {
            Type* ft = cls->fields[f].type;
            if (type_is_owned(ft)) {
                const char* fmty = llvm_mem_ty(ft);
                sb_printf(b, "  %%sp%d = getelementptr %s, ptr %%p, i32 0, i32 %d\n  %%sv%d = load %s, ptr %%sp%d\n",
                          f, cls->llvm_name, cls->fields[f].slot_index, f, fmty, f);
                sb_printf(b, "  %%cv%d = call %s @_clone_%s(%s %%sv%d)\n", f, fmty, ft->mangled, fmty, f);
                sb_printf(b, "  %%dp%d = getelementptr %s, ptr %%np, i32 0, i32 %d\n  store %s %%cv%d, ptr %%dp%d\n",
                          f, cls->llvm_name, cls->fields[f].slot_index, fmty, f, f);
            }
        }
        sb_append(b, "  ret ptr %np\n}\n\n");
    }
}

static void emit_mono_list(Type* lt) {
    Type* el = lt->elem;
    emit_hash_eq_for_type(el);
    if (type_is_owned(el)) {
        emit_drop_for_type(el);
        emit_clone_for_type(el);
    }
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
    sb_append(b, "  %ok = icmp ult i64 %i, %len\n  br i1 %ok, label %inb, label %oob\noob:\n");
    sb_append(b, "  call void @stricta_bounds_fail(i64 %idx, i64 %len, ptr null, i32 0)\n  unreachable\ninb:\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%slot = getelementptr %s, ptr %%arr, i64 %%i\n  %%v = load %s, ptr %%slot\n  ret %s %%v\n}\n\n", mty, mty, mty);

    /* get_nn (statically proven non-negative index; still bounds-checked for upper limit) */
    sb_printf(b, "define internal %s @_%s_get_nn(ptr %%l, i64 %%idx) alwaysinline {\nentry:\n", mty, m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len32 = load i32, ptr %%lp\n  %%len = sext i32 %%len32 to i64\n", sty);
    sb_append(b, "  %ok = icmp ult i64 %idx, %len\n  br i1 %ok, label %inb, label %oob\noob:\n");
    sb_append(b, "  call void @stricta_bounds_fail(i64 %idx, i64 %len, ptr null, i32 0)\n  unreachable\ninb:\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%slot = getelementptr %s, ptr %%arr, i64 %%idx\n  %%v = load %s, ptr %%slot\n  ret %s %%v\n}\n\n", mty, mty, mty);

    /* set */
    sb_printf(b, "define internal void @_%s_set(ptr %%l, i64 %%idx, %s %%val) alwaysinline {\nentry:\n", m, mty);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len32 = load i32, ptr %%lp\n", sty);
    sb_append(b, "  %len = sext i32 %len32 to i64\n  %neg = icmp slt i64 %idx, 0\n  %adj = add i64 %idx, %len\n");
    sb_append(b, "  %i = select i1 %neg, i64 %adj, i64 %idx\n");
    sb_append(b, "  %ok = icmp ult i64 %i, %len\n  br i1 %ok, label %inb, label %oob\noob:\n");
    sb_append(b, "  call void @stricta_bounds_fail(i64 %idx, i64 %len, ptr null, i32 0)\n  unreachable\ninb:\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%slot = getelementptr %s, ptr %%arr, i64 %%i\n", mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%old = load %s, ptr %%slot\n  call void @_drop_%s(%s %%old)\n", mty, el->mangled, mty);
    }
    sb_printf(b, "  store %s %%val, ptr %%slot\n  ret void\n}\n\n", mty);

    /* set_nn (statically proven non-negative index; still bounds-checked for upper limit) */
    sb_printf(b, "define internal void @_%s_set_nn(ptr %%l, i64 %%idx, %s %%val) alwaysinline {\nentry:\n", m, mty);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len32 = load i32, ptr %%lp\n  %%len = sext i32 %%len32 to i64\n", sty);
    sb_append(b, "  %ok = icmp ult i64 %idx, %len\n  br i1 %ok, label %inb, label %oob\noob:\n");
    sb_append(b, "  call void @stricta_bounds_fail(i64 %idx, i64 %len, ptr null, i32 0)\n  unreachable\ninb:\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%slot = getelementptr %s, ptr %%arr, i64 %%idx\n", mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%old = load %s, ptr %%slot\n  call void @_drop_%s(%s %%old)\n", mty, el->mangled, mty);
    }
    sb_printf(b, "  store %s %%val, ptr %%slot\n  ret void\n}\n\n", mty);

    /* del_raw (drops element iff %drop_it is true) */
    sb_printf(b, "define internal void @_%s_del_raw(ptr %%l, i64 %%idx, i1 %%drop_it) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len32 = load i32, ptr %%lp\n", sty);
    sb_append(b, "  %len = sext i32 %len32 to i64\n  %neg = icmp slt i64 %idx, 0\n  %adj = add i64 %idx, %len\n");
    sb_append(b, "  %i0 = select i1 %neg, i64 %adj, i64 %idx\n");
    sb_append(b, "  %ok = icmp ult i64 %i0, %len\n  br i1 %ok, label %inb, label %oob\noob:\n");
    sb_append(b, "  call void @stricta_bounds_fail(i64 %idx, i64 %len, ptr null, i32 0)\n  unreachable\ninb:\n  %i = trunc i64 %i0 to i32\n");
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    if (type_is_owned(el)) {
        sb_append(b, "  br i1 %drop_it, label %do_drop, label %after_drop\ndo_drop:\n");
        sb_printf(b, "  %%dp0 = getelementptr %s, ptr %%arr, i32 %%i\n  %%dv0 = load %s, ptr %%dp0\n  call void @_drop_%s(%s %%dv0)\n  br label %%after_drop\nafter_drop:\n",
                  mty, mty, el->mangled, mty);
    }
    sb_printf(b, "  %%nlen = sub i32 %%len32, 1\n  br label %%loop\nloop:\n  %%k = phi i32 [ %%i, %%%s ], [ %%kn, %%step ]\n",
              type_is_owned(el) ? "after_drop" : "inb");
    sb_append(b, "  %c = icmp slt i32 %k, %nlen\n  br i1 %c, label %step, label %done\nstep:\n  %kn = add i32 %k, 1\n");
    sb_printf(b, "  %%sp = getelementptr %s, ptr %%arr, i32 %%kn\n  %%v = load %s, ptr %%sp\n", mty, mty);
    sb_printf(b, "  %%dp = getelementptr %s, ptr %%arr, i32 %%k\n  store %s %%v, ptr %%dp\n  br label %%loop\n", mty, mty);
    sb_append(b, "done:\n  store i32 %nlen, ptr %lp\n  ret void\n}\n\n");

    sb_printf(b, "define internal void @_%s_del(ptr %%l, i64 %%idx) {\nentry:\n  call void @_%s_del_raw(ptr %%l, i64 %%idx, i1 true)\n  ret void\n}\n\n", m, m);

    /* pop */
    sb_printf(b, "define internal %s @_%s_pop(ptr %%l, i64 %%idx) {\nentry:\n", mty, m);
    sb_printf(b, "  %%v = call %s @_%s_get(ptr %%l, i64 %%idx)\n", mty, m);
    sb_printf(b, "  call void @_%s_del_raw(ptr %%l, i64 %%idx, i1 false)\n  ret %s %%v\n}\n\n", m, mty);

    /* clear */
    sb_printf(b, "define internal void @_%s_clear(ptr %%l) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n", sty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%len = load i32, ptr %%lp\n  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n  br label %%loop\n", sty);
        sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
        sb_printf(b, "  %%ep = getelementptr %s, ptr %%arr, i32 %%i\n  %%ev = load %s, ptr %%ep\n  call void @_drop_%s(%s %%ev)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n",
                  mty, mty, el->mangled, mty);
    }
    sb_append(b, "  store i32 0, ptr %lp\n  ret void\n}\n\n");

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
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%arr, i64 %%k\n  %%v0 = load %s, ptr %%vp\n", mty, mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", mty, el->mangled, mty);
        sb_printf(b, "  call void @_%s_append(ptr %%res, %s %%v)\n  %%kn = add i64 %%k, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty);
    } else {
        sb_printf(b, "  call void @_%s_append(ptr %%res, %s %%v0)\n  %%kn = add i64 %%k, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty);
    }

    /* slice_step */
    sb_printf(b, "define internal ptr @_%s_slice_step(ptr %%l, i64 %%s0, i32 %%hs, i64 %%e0, i32 %%he, i64 %%step) {\nentry:\n", m);
    sb_append(b, "  %st_p = alloca i64\n");
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len32 = load i32, ptr %%lp\n  %%len = sext i32 %%len32 to i64\n", sty);
    sb_append(b, "  %cnt = call i64 @stricta_slice_adjust(i64 %len, i64 %s0, i32 %hs, i64 %e0, i32 %he, i64 %step, ptr %st_p)\n");
    sb_append(b, "  %st = load i64, ptr %st_p\n");
    sb_append(b, "  %cnt_pos = call i64 @llvm.smax.i64(i64 %cnt, i64 0)\n  %cnt32 = trunc i64 %cnt_pos to i32\n");
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%cnt32)\n", m);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%arr = load ptr, ptr %%ip\n", sty);
    sb_append(b, "  br label %loop\nloop:\n  %i = phi i64 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i64 %i, %cnt\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_append(b, "  %off = mul i64 %i, %step\n  %idx = add i64 %st, %off\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%arr, i64 %%idx\n  %%v0 = load %s, ptr %%vp\n", mty, mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", mty, el->mangled, mty);
        sb_printf(b, "  call void @_%s_append(ptr %%res, %s %%v)\n  %%in = add i64 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty);
    } else {
        sb_printf(b, "  call void @_%s_append(ptr %%res, %s %%v0)\n  %%in = add i64 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty);
    }

    /* copy */
    sb_printf(b, "define internal ptr @_%s_copy(ptr %%l) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%l, i32 0, i32 0\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n", m);
    sb_printf(b, "  %%res_lp = getelementptr %s, ptr %%res, i32 0, i32 0\n  store i32 %%len, ptr %%res_lp\n", sty);
    sb_printf(b, "  %%ip1 = getelementptr %s, ptr %%l, i32 0, i32 2\n  %%src = load ptr, ptr %%ip1\n", sty);
    sb_printf(b, "  %%ip2 = getelementptr %s, ptr %%res, i32 0, i32 2\n  %%dst = load ptr, ptr %%ip2\n", sty);
    if (type_is_owned(el)) {
        sb_append(b, "  br label %loop\nloop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
        sb_printf(b, "  %%sp = getelementptr %s, ptr %%src, i32 %%i\n  %%sv = load %s, ptr %%sp\n", mty, mty);
        sb_printf(b, "  %%cv = call %s @_clone_%s(%s %%sv)\n", mty, el->mangled, mty);
        sb_printf(b, "  %%dp = getelementptr %s, ptr %%dst, i32 %%i\n  store %s %%cv, ptr %%dp\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", mty, mty);
    } else {
        sb_append(b, "  %len64 = sext i32 %len to i64\n");
        sb_printf(b, "  %%nb = mul i64 %%len64, %s\n", esz);
        sb_append(b, "  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %src, i64 %nb, i1 false)\n  ret ptr %res\n}\n\n");
    }

    /* extend */
    sb_printf(b, "define internal void @_%s_extend(ptr %%l, ptr %%o) {\nentry:\n", m);
    sb_printf(b, "  %%olp = getelementptr %s, ptr %%o, i32 0, i32 0\n  %%olen = load i32, ptr %%olp\n", sty);
    sb_printf(b, "  %%oip = getelementptr %s, ptr %%o, i32 0, i32 2\n  %%oarr = load ptr, ptr %%oip\n", sty);
    sb_append(b, "  br label %loop\nloop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %olen\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%oarr, i32 %%i\n  %%v0 = load %s, ptr %%vp\n", mty, mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", mty, el->mangled, mty);
        sb_printf(b, "  call void @_%s_append(ptr %%l, %s %%v)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n", m, mty);
    } else {
        sb_printf(b, "  call void @_%s_append(ptr %%l, %s %%v0)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n", m, mty);
    }

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
    if (type_is_owned(el)) {
        emit_drop_for_type(el);
        emit_clone_for_type(el);
    }
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
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%s, %s %%val, ptr %%slot_p)\n  %%found = icmp sge i32 %%idx, 0\n  br i1 %%found, label %%dup, label %%ins\ndup:\n", m, mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  call void @_drop_%s(%s %%val)\n", el->mangled, mty);
    }
    sb_append(b, "  br label %done\nins:\n");
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

    /* discard_raw (drops removed element iff %drop_it is true) */
    sb_printf(b, "define internal void @_%s_discard_raw(ptr %%s, %s %%val, i1 %%drop_it) alwaysinline {\nentry:\n", m, mty);
    sb_append(b, "  %slot_p = alloca i32\n");
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%s, %s %%val, ptr %%slot_p)\n  %%miss = icmp slt i32 %%idx, 0\n  br i1 %%miss, label %%done, label %%rem\nrem:\n", m, mty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  %%buckets = load ptr, ptr %%p1\n  %%slot = load i32, ptr %%slot_p\n", sty);
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %slot\n  store i32 -2, ptr %bp\n");
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%items = load ptr, ptr %%p0\n", sty);
    if (type_is_owned(el)) {
        sb_append(b, "  br i1 %drop_it, label %do_drop, label %after_drop\ndo_drop:\n");
        sb_printf(b, "  %%dp0 = getelementptr %s, ptr %%items, i32 %%idx\n  %%dv0 = load %s, ptr %%dp0\n  call void @_drop_%s(%s %%dv0)\n  br label %%after_drop\nafter_drop:\n",
                  mty, mty, el->mangled, mty);
    }
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  %%len = load i32, ptr %%p2\n  %%last = sub i32 %%len, 1\n", sty);
    sb_append(b, "  %not_last = icmp ne i32 %idx, %last\n  br i1 %not_last, label %swap, label %dec\nswap:\n");
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%items, i32 %%last\n  %%mv = load %s, ptr %%lp\n", mty, mty);
    sb_printf(b, "  %%dp = getelementptr %s, ptr %%items, i32 %%idx\n  store %s %%mv, ptr %%dp\n", mty, mty);
    sb_append(b, "  %ms_p = alloca i32\n");
    sb_printf(b, "  %%midx = call i32 @_%s_find(ptr %%s, %s %%mv, ptr %%ms_p)\n  %%ms = load i32, ptr %%ms_p\n", m, mty);
    sb_append(b, "  %mbp = getelementptr i32, ptr %buckets, i32 %ms\n  store i32 %idx, ptr %mbp\n  br label %dec\ndec:\n  store i32 %last, ptr %p2\n  br label %done\ndone:\n  ret void\n}\n\n");

    sb_printf(b, "define internal void @_%s_discard(ptr %%s, %s %%val) alwaysinline {\nentry:\n  call void @_%s_discard_raw(ptr %%s, %s %%val, i1 true)\n  ret void\n}\n\n", m, mty, m, mty);

    /* pop */
    sb_printf(b, "define internal %s @_%s_pop(ptr %%s) {\nentry:\n", mty, m);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n  %%len = load i32, ptr %%p2\n  %%last = sub i32 %%len, 1\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%items = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%last\n  %%v = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  call void @_%s_discard_raw(ptr %%s, %s %%v, i1 false)\n  ret %s %%v\n}\n\n", m, mty, mty);

    /* clear */
    sb_printf(b, "define internal void @_%s_clear(ptr %%s) {\nentry:\n", m);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%s, i32 0, i32 2\n", sty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%len = load i32, ptr %%p2\n  %%p0 = getelementptr %s, ptr %%s, i32 0, i32 0\n  %%items = load ptr, ptr %%p0\n  br label %%dloop\n", sty);
        sb_append(b, "dloop:\n  %di = phi i32 [ 0, %entry ], [ %din, %dbody ]\n  %dc = icmp slt i32 %di, %len\n  br i1 %dc, label %dbody, label %ddone\ndbody:\n");
        sb_printf(b, "  %%dip = getelementptr %s, ptr %%items, i32 %%di\n  %%dv = load %s, ptr %%dip\n  call void @_drop_%s(%s %%dv)\n  %%din = add i32 %%di, 1\n  br label %%dloop\nddone:\n",
                  mty, mty, el->mangled, mty);
    }
    sb_append(b, "  store i32 0, ptr %p2\n");
    sb_printf(b, "  %%p4 = getelementptr %s, ptr %%s, i32 0, i32 4\n  %%hc = load i32, ptr %%p4\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%s, i32 0, i32 1\n  %%buckets = load ptr, ptr %%p1\n", sty);
    sb_append(b, "  br label %loop\nloop:\n  %i = phi i32 [ 0, %");
    sb_append(b, type_is_owned(el) ? "ddone" : "entry");
    sb_append(b, " ], [ %in, %body ]\n  %c = icmp slt i32 %i, %hc\n  br i1 %c, label %body, label %done\nbody:\n");
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
    if (type_is_owned(el)) {
        sb_append(b, "  br label %cloop\ncloop:\n  %ci = phi i32 [ 0, %entry ], [ %cin, %cbody ]\n  %cc = icmp slt i32 %ci, %len\n  br i1 %cc, label %cbody, label %cdone\ncbody:\n");
        sb_printf(b, "  %%csp = getelementptr %s, ptr %%items, i32 %%ci\n  %%csv = load %s, ptr %%csp\n  %%ccv = call %s @_clone_%s(%s %%csv)\n",
                  mty, mty, mty, el->mangled, mty);
        sb_printf(b, "  %%cdp = getelementptr %s, ptr %%ni, i32 %%ci\n  store %s %%ccv, ptr %%cdp\n  %%cin = add i32 %%ci, 1\n  br label %%cloop\ncdone:\n",
                  mty, mty);
    } else {
        sb_append(b, "  %len64 = sext i32 %len to i64\n");
        sb_printf(b, "  %%lb = mul i64 %%len64, %s\n  call void @llvm.memcpy.p0.p0.i64(ptr %%ni, ptr %%items, i64 %%lb, i1 false)\n", esz);
    }
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
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v0 = load %s, ptr %%vp\n", mty, mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", mty, el->mangled, mty);
    }
    sb_printf(b, "  call void @_%s_add(ptr %%a, %s %s)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n", m, mty, type_is_owned(el) ? "%v" : "%v0");

    /* union (|) */
    sb_printf(b, "define internal ptr @_%s_union(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%res = call ptr @_%s_copy(ptr %%a)\n  call void @_%s_update(ptr %%res, ptr %%b)\n  ret ptr %%res\n}\n\n", m, m);

    /* intersection (&) */
    sb_printf(b, "define internal ptr @_%s_intersection(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%a, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%a, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  br label %%loop\n", m);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v0 = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%b, %s %%v0)\n  br i1 %%has, label %%add, label %%next\nadd:\n", m, mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", mty, el->mangled, mty);
    }
    sb_printf(b, "  call void @_%s_add(ptr %%res, %s %s)\n  br label %%next\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty, type_is_owned(el) ? "%v" : "%v0");

    /* difference (-) */
    sb_printf(b, "define internal ptr @_%s_difference(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%a, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%a, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  br label %%loop\n", m);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v0 = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%b, %s %%v0)\n  br i1 %%has, label %%next, label %%add\nadd:\n", m, mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", mty, el->mangled, mty);
    }
    sb_printf(b, "  call void @_%s_add(ptr %%res, %s %s)\n  br label %%next\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty, type_is_owned(el) ? "%v" : "%v0");

    /* symmetric_difference (^) */
    sb_printf(b, "define internal ptr @_%s_symmetric_difference(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%res = call ptr @_%s_difference(ptr %%a, ptr %%b)\n", m);
    sb_printf(b, "  %%lp = getelementptr %s, ptr %%b, i32 0, i32 2\n  %%len = load i32, ptr %%lp\n", sty);
    sb_printf(b, "  %%ip = getelementptr %s, ptr %%b, i32 0, i32 0\n  %%items = load ptr, ptr %%ip\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %next ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v0 = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%a, %s %%v0)\n  br i1 %%has, label %%next, label %%add\nadd:\n", m, mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", mty, el->mangled, mty);
    }
    sb_printf(b, "  call void @_%s_add(ptr %%res, %s %s)\n  br label %%next\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n", m, mty, type_is_owned(el) ? "%v" : "%v0");

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
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%items, i32 %%i\n  %%v0 = load %s, ptr %%vp\n", mty, mty);
    sb_printf(b, "  %%has = call i1 @_%s_contains(ptr %%a, %s %%v0)\n  br i1 %%has, label %%rem, label %%add\nrem:\n", m, mty);
    sb_printf(b, "  call void @_%s_discard(ptr %%a, %s %%v0)\n  br label %%next\nadd:\n", m, mty);
    if (type_is_owned(el)) {
        sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", mty, el->mangled, mty);
    }
    sb_printf(b, "  call void @_%s_add(ptr %%a, %s %s)\n  br label %%next\nnext:\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n", m, mty, type_is_owned(el) ? "%v" : "%v0");

    sb_printf(b, "define internal void @_%s_intersection_update(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%tmp = call ptr @_%s_intersection(ptr %%a, ptr %%b)\n", m);
    sb_printf(b, "  call void @_%s_clear(ptr %%a)\n  call void @_%s_update(ptr %%a, ptr %%tmp)\n", m, m);
    if (type_is_owned(el)) {
        sb_printf(b, "  call void @_drop_%s(ptr %%tmp)\n", m);
    } else {
        sb_printf(b, "  call void @_%s_clear(ptr %%tmp)\n", m);
    }
    sb_append(b, "  ret void\n}\n\n");

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
    if (type_is_owned(kt)) { emit_drop_for_type(kt); emit_clone_for_type(kt); }
    if (type_is_owned(vt)) { emit_drop_for_type(vt); emit_clone_for_type(vt); }
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
    sb_printf(b, "  %%vpu = getelementptr %s, ptr %%valsu, i32 %%idx\n", vmty);
    if (type_is_owned(vt)) {
        sb_printf(b, "  %%oldu = load %s, ptr %%vpu\n  call void @_drop_%s(%s %%oldu)\n", vmty, vt->mangled, vmty);
    }
    if (type_is_owned(kt)) {
        sb_printf(b, "  call void @_drop_%s(%s %%key)\n", kt->mangled, kmty);
    }
    sb_printf(b, "  store %s %%val, ptr %%vpu\n  ret void\nins:\n", vmty);
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
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%idx\n  %%v = load %s, ptr %%vp\n", vmty, vmty);
    if (type_is_owned(vt)) {
        sb_printf(b, "  %%vc = call %s @_clone_%s(%s %%v)\n  ret %s %%vc\nmiss:\n  %%dc = call %s @_clone_%s(%s %%def)\n  ret %s %%dc\n}\n\n",
                  vmty, vt->mangled, vmty, vmty, vmty, vt->mangled, vmty, vmty);
    } else {
        sb_printf(b, "  ret %s %%v\nmiss:\n  ret %s %%def\n}\n\n", vmty, vmty);
    }

    sb_printf(b, "define internal %s @_%s_getitem(ptr %%d, %s %%key) alwaysinline {\nentry:\n", vmty, m, kmty);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr null)\n  %%ok = icmp sge i32 %%idx, 0\n  br i1 %%ok, label %%hit, label %%miss\nhit:\n", m, kmty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%idx\n  %%v = load %s, ptr %%vp\n  ret %s %%v\nmiss:\n", vmty, vmty, vmty);
    sb_printf(b, "  call void @stricta_set_exc(i32 6, ptr null, ptr null)\n  call void @stricta_unhandled_exc()\n  unreachable\n}\n\n");

    sb_printf(b, "define internal %s @_%s_setdefault(ptr %%d, %s %%key, %s %%def) alwaysinline {\nentry:\n", vmty, m, kmty, vmty);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr null)\n  %%ok = icmp sge i32 %%idx, 0\n  br i1 %%ok, label %%hit, label %%ins\nhit:\n", m, kmty);
    if (type_is_owned(kt)) sb_printf(b, "  call void @_drop_%s(%s %%key)\n", kt->mangled, kmty);
    if (type_is_owned(vt)) sb_printf(b, "  call void @_drop_%s(%s %%def)\n", vt->mangled, vmty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%idx\n  %%v = load %s, ptr %%vp\n", vmty, vmty);
    if (type_is_owned(vt)) {
        sb_printf(b, "  %%vc = call %s @_clone_%s(%s %%v)\n  ret %s %%vc\n", vmty, vt->mangled, vmty, vmty);
    } else {
        sb_printf(b, "  ret %s %%v\n", vmty);
    }
    sb_printf(b, "ins:\n  call void @_%s_set(ptr %%d, %s %%key, %s %%def)\n", m, kmty, vmty);
    sb_printf(b, "  %%idx2 = call i32 @_%s_find(ptr %%d, %s %%key, ptr null)\n", m, kmty);
    sb_printf(b, "  %%p1b = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%valsb = load ptr, ptr %%p1b\n", sty);
    sb_printf(b, "  %%vpb = getelementptr %s, ptr %%valsb, i32 %%idx2\n  %%vb = load %s, ptr %%vpb\n", vmty, vmty);
    if (type_is_owned(vt)) {
        sb_printf(b, "  %%vbc = call %s @_clone_%s(%s %%vb)\n  ret %s %%vbc\n}\n\n", vmty, vt->mangled, vmty, vmty);
    } else {
        sb_printf(b, "  ret %s %%vb\n}\n\n", vmty);
    }

    sb_printf(b, "define internal i1 @_%s_contains(ptr %%d, %s %%key) alwaysinline {\nentry:\n", m, kmty);
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr null)\n  %%r = icmp sge i32 %%idx, 0\n  ret i1 %%r\n}\n\n", m, kmty);

    /* pop_raw, pop, del, popitem */
    sb_printf(b, "define internal %s @_%s_pop_raw(ptr %%d, %s %%key, i1 %%drop_key) {\nentry:\n", vmty, m, kmty);
    sb_append(b, "  %slot_p = alloca i32\n");
    sb_printf(b, "  %%idx = call i32 @_%s_find(ptr %%d, %s %%key, ptr %%slot_p)\n  %%miss = icmp slt i32 %%idx, 0\n  br i1 %%miss, label %%err, label %%rem\nerr:\n", m, kmty);
    sb_append(b, "  call void @stricta_set_exc(i32 6, ptr null, ptr null)\n  call void @stricta_unhandled_exc()\n  unreachable\nrem:\n");
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%idx\n  %%retv = load %s, ptr %%vp\n", vmty, vmty);
    if (type_is_owned(kt)) {
        sb_append(b, "  br i1 %drop_key, label %dk, label %after_dk\ndk:\n");
        sb_printf(b, "  %%dkp0 = getelementptr %s, ptr %%keys, i32 %%idx\n  %%dkv0 = load %s, ptr %%dkp0\n  call void @_drop_%s(%s %%dkv0)\n  br label %%after_dk\nafter_dk:\n",
                  kmty, kmty, kt->mangled, kmty);
    }
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

    sb_printf(b, "define internal %s @_%s_pop(ptr %%d, %s %%key) {\nentry:\n  %%v = call %s @_%s_pop_raw(ptr %%d, %s %%key, i1 true)\n  ret %s %%v\n}\n\n",
              vmty, m, kmty, vmty, m, kmty, vmty);

    sb_printf(b, "define internal void @_%s_del(ptr %%d, %s %%key) {\nentry:\n", m, kmty);
    sb_printf(b, "  %%v = call %s @_%s_pop(ptr %%d, %s %%key)\n", vmty, m, kmty);
    if (type_is_owned(vt)) {
        sb_printf(b, "  call void @_drop_%s(%s %%v)\n", vt->mangled, vmty);
    }
    sb_append(b, "  ret void\n}\n\n");

    /* popitem */
    Type* pair[2] = {kt, vt};
    Type* pty = make_tuple_type(pair, 2);
    const char* psty = llvm_reg_ty(pty);
    sb_printf(b, "define internal %s @_%s_popitem(ptr %%d) {\nentry:\n", psty, m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n  %%last = sub i32 %%len, 1\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%last\n  %%k = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%v = call %s @_%s_pop_raw(ptr %%d, %s %%k, i1 false)\n", vmty, m, kmty);
    sb_printf(b, "  %%t1 = insertvalue %s zeroinitializer, %s %%k, 0\n  %%t2 = insertvalue %s %%t1, %s %%v, 1\n  ret %s %%t2\n}\n\n",
              psty, kmty, psty, vmty, psty);

    /* clear, copy, update, merge, keys, values, items, eq */
    sb_printf(b, "define internal void @_%s_clear(ptr %%d) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n", sty);
    if (type_is_owned(kt) || type_is_owned(vt)) {
        sb_printf(b, "  %%len = load i32, ptr %%p3\n  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n  br label %%dloop\n", sty, sty);
        sb_append(b, "dloop:\n  %di = phi i32 [ 0, %entry ], [ %din, %dbody ]\n  %dc = icmp slt i32 %di, %len\n  br i1 %dc, label %dbody, label %ddone\ndbody:\n");
        if (type_is_owned(kt)) {
            sb_printf(b, "  %%dkp = getelementptr %s, ptr %%keys, i32 %%di\n  %%dk = load %s, ptr %%dkp\n  call void @_drop_%s(%s %%dk)\n",
                      kmty, kmty, kt->mangled, kmty);
        }
        if (type_is_owned(vt)) {
            sb_printf(b, "  %%dvp = getelementptr %s, ptr %%vals, i32 %%di\n  %%dv = load %s, ptr %%dvp\n  call void @_drop_%s(%s %%dv)\n",
                      vmty, vmty, vt->mangled, vmty);
        }
        sb_append(b, "  %din = add i32 %di, 1\n  br label %dloop\nddone:\n");
    }
    sb_append(b, "  store i32 0, ptr %p3\n");
    sb_printf(b, "  %%p6 = getelementptr %s, ptr %%d, i32 0, i32 6\n  store i32 0, ptr %%p6\n", sty);
    sb_printf(b, "  %%p5 = getelementptr %s, ptr %%d, i32 0, i32 5\n  %%hc = load i32, ptr %%p5\n", sty);
    sb_printf(b, "  %%p2 = getelementptr %s, ptr %%d, i32 0, i32 2\n  %%buckets = load ptr, ptr %%p2\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %");
    sb_append(b, (type_is_owned(kt) || type_is_owned(vt)) ? "ddone" : "entry");
    sb_append(b, " ], [ %in, %body ]\n  %c = icmp slt i32 %i, %hc\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_append(b, "  %bp = getelementptr i32, ptr %buckets, i32 %i\n  store i32 -1, ptr %bp\n  %in = add i32 %i, 1\n  br label %loop\ndone:\n  ret void\n}\n\n");

    sb_printf(b, "define internal ptr @_%s_copy(ptr %%d) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  call void @_%s_update(ptr %%res, ptr %%d)\n  ret ptr %%res\n}\n\n", m, m);

    sb_printf(b, "define internal void @_%s_update(ptr %%a, ptr %%b) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%b, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%b, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%b, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n  br label %%loop\n", sty);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%i\n  %%k0 = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%i\n  %%v0 = load %s, ptr %%vp\n", vmty, vmty);
    if (type_is_owned(kt)) sb_printf(b, "  %%k = call %s @_clone_%s(%s %%k0)\n", kmty, kt->mangled, kmty);
    if (type_is_owned(vt)) sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", vmty, vt->mangled, vmty);
    sb_printf(b, "  call void @_%s_set(ptr %%a, %s %s, %s %s)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret void\n}\n\n",
              m, kmty, type_is_owned(kt) ? "%k" : "%k0", vmty, type_is_owned(vt) ? "%v" : "%v0");

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
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%i\n  %%k0 = load %s, ptr %%kp\n", kmty, kmty);
    if (type_is_owned(kt)) sb_printf(b, "  %%k = call %s @_clone_%s(%s %%k0)\n", kmty, kt->mangled, kmty);
    sb_printf(b, "  call void @_%s_append(ptr %%res, %s %s)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n",
              lk->mangled, kmty, type_is_owned(kt) ? "%k" : "%k0");

    sb_printf(b, "define internal ptr @_%s_values(ptr %%d) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  br label %%loop\n", lv->mangled);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%i\n  %%v0 = load %s, ptr %%vp\n", vmty, vmty);
    if (type_is_owned(vt)) sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", vmty, vt->mangled, vmty);
    sb_printf(b, "  call void @_%s_append(ptr %%res, %s %s)\n  %%in = add i32 %%i, 1\n  br label %%loop\ndone:\n  ret ptr %%res\n}\n\n",
              lv->mangled, vmty, type_is_owned(vt) ? "%v" : "%v0");

    sb_printf(b, "define internal ptr @_%s_items(ptr %%d) {\nentry:\n", m);
    sb_printf(b, "  %%p3 = getelementptr %s, ptr %%d, i32 0, i32 3\n  %%len = load i32, ptr %%p3\n", sty);
    sb_printf(b, "  %%p0 = getelementptr %s, ptr %%d, i32 0, i32 0\n  %%keys = load ptr, ptr %%p0\n", sty);
    sb_printf(b, "  %%p1 = getelementptr %s, ptr %%d, i32 0, i32 1\n  %%vals = load ptr, ptr %%p1\n", sty);
    sb_printf(b, "  %%res = call ptr @_%s_new(i32 %%len)\n  br label %%loop\n", li->mangled);
    sb_append(b, "loop:\n  %i = phi i32 [ 0, %entry ], [ %in, %body ]\n  %c = icmp slt i32 %i, %len\n  br i1 %c, label %body, label %done\nbody:\n");
    sb_printf(b, "  %%kp = getelementptr %s, ptr %%keys, i32 %%i\n  %%k0 = load %s, ptr %%kp\n", kmty, kmty);
    sb_printf(b, "  %%vp = getelementptr %s, ptr %%vals, i32 %%i\n  %%v0 = load %s, ptr %%vp\n", vmty, vmty);
    if (type_is_owned(kt)) sb_printf(b, "  %%k = call %s @_clone_%s(%s %%k0)\n", kmty, kt->mangled, kmty);
    if (type_is_owned(vt)) sb_printf(b, "  %%v = call %s @_clone_%s(%s %%v0)\n", vmty, vt->mangled, vmty);
    sb_printf(b, "  %%t1 = insertvalue %s zeroinitializer, %s %s, 0\n  %%t2 = insertvalue %s %%t1, %s %s, 1\n",
              psty, kmty, type_is_owned(kt) ? "%k" : "%k0", psty, vmty, type_is_owned(vt) ? "%v" : "%v0");
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

static VarSym* cg_find_var_sym(const char* name) {
    if (g_cg.cur_fn) {
        bool is_glob = false;
        for (int i = 0; i < g_cg.cur_fn->global_decl_count; i++) {
            if (strcmp(g_cg.cur_fn->global_decls[i], name) == 0) { is_glob = true; break; }
        }
        if (!is_glob) {
            VarSym* lv = find_local_in_fn(g_cg.cur_fn, name);
            if (lv) return lv;
        }
    }
    return find_global_in_mod(g_cg.cur_mod, name);
}

static const char* cg_expr(Expr* e, Type* hint);
static void cg_stmts(Stmt** stmts, int count);

static const char* cg_eval_recv(Expr* obj_e) {
    const char* v = cg_expr(obj_e, NULL);
    Type* t = obj_e->ty;
    while (type_is_ptr(t)) {
        Type* next = t->elem;
        const char* mv = cg_reg();
        cg_emit("%s = load %s, ptr %s", mv, llvm_mem_ty(next), v);
        v = cg_mem_to_reg(mv, next);
        t = next;
    }
    return v;
}

static const char* cg_lvalue_ptr(Expr* e);

static const char* cg_deref_to_class_ptr(Expr* obj_e, ClassInfo** out_cls) {
    Type* t = obj_e->ty;
    if (!type_is_ptr(t)) {
        if (out_cls) *out_cls = t->cls;
        if (t->cls->is_value_type) return cg_lvalue_ptr(obj_e);
        return cg_expr(obj_e, NULL);
    }
    const char* p = cg_expr(obj_e, NULL);
    while (type_is_ptr(t->elem)) {
        const char* np = cg_reg();
        cg_emit("%s = load ptr, ptr %s", np, p);
        p = np;
        t = t->elem;
    }
    ClassInfo* cls = t->elem->cls;
    if (out_cls) *out_cls = cls;
    if (cls->is_value_type) return p;
    const char* np = cg_reg();
    cg_emit("%s = load ptr, ptr %s", np, p);
    return np;
}

static const char* cg_lvalue_ptr(Expr* e) {
    if (!e) return "null";
    if (e->kind == EXPR_VAR) {
        if (strcmp(e->var.name, "self") == 0 || (strcmp(e->var.name, "cls") == 0 && g_cg.cur_fn && g_cg.cur_fn->is_classmethod)) {
            VarSym* sv = find_local_in_fn(g_cg.cur_fn, g_cg.cur_fn->param_names[0]);
            return cg_ensure_local_alloca(sv->llvm_name, sv->type);
        }
        Type* dummy = NULL;
        return cg_var_ptr(e->var.name, &dummy);
    }
    if (e->kind == EXPR_ATTR) {
        const char* an = e->attr.attr;
        if (e->attr.obj->ty->kind == TY_MODULE) {
            VarSym* gv = find_global_in_mod(e->attr.obj->ty->mod, an);
            return gv ? gv->llvm_name : "null";
        }
        ClassInfo* cls = NULL;
        const char* obj_p = cg_deref_to_class_ptr(e->attr.obj, &cls);
        FieldInfo* fi = find_field_in_class(cls, an);
        if (fi) {
            const char* fp = cg_reg();
            cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, cls->llvm_name, obj_p, fi->slot_index);
            return fp;
        }
        FieldInfo* ca = find_class_attr(cls, an);
        if (ca) {
            ClassInfo* owner = cls;
            for (int m = 0; m < cls->mro_count; m++) {
                for (int k = 0; k < cls->mro[m]->class_attr_count; k++) {
                    if (strcmp(cls->mro[m]->class_attrs[k].name, an) == 0) { owner = cls->mro[m]; goto found_ca_lv; }
                }
            }
            found_ca_lv:;
            return arena_sprintf("@_cattr_%s%s__%s", owner->mod->prefix, owner->name, an);
        }
        return "null";
    }
    if (e->kind == EXPR_INDEX) {
        const char* ov = cg_eval_recv(e->index.obj);
        Type* ot = type_deref_all(e->index.obj->ty);
        if (ot->kind == TY_LIST) {
            const char* iv = cg_coerce(cg_expr(e->index.index, g_ty_i64), e->index.index->ty, g_ty_i64);
            const char* idx_ok = iv;
            if (!e->is_inbounds) {
                const char* lp = cg_reg(); const char* len32 = cg_reg(); const char* len64 = cg_reg();
                cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 0", lp, ot->mangled, ov);
                cg_emit("%s = load i32, ptr %s", len32, lp);
                cg_emit("%s = sext i32 %s to i64", len64, len32);
                if (!e->index.index->is_nonneg) {
                    const char* is_neg = cg_reg(); const char* adj = cg_reg(); const char* sel = cg_reg();
                    cg_emit("%s = icmp slt i64 %s, 0", is_neg, iv);
                    cg_emit("%s = add i64 %s, %s", adj, iv, len64);
                    cg_emit("%s = select i1 %s, i64 %s, i64 %s", sel, is_neg, adj, iv);
                    idx_ok = sel;
                }
                const char* ok = cg_reg();
                const char* l_inb = cg_lbl("lv_inb"); const char* l_oob = cg_lbl("lv_oob");
                cg_emit("%s = icmp ult i64 %s, %s", ok, idx_ok, len64);
                cg_cond_br(ok, l_inb, l_oob);
                cg_label(l_oob);
                cg_emit("call void @stricta_bounds_fail(i64 %s, i64 %s, ptr null, i32 0)", iv, len64);
                cg_emit("unreachable");
                cg_label(l_inb);
            }
            const char* ip = cg_reg(); const char* arr = cg_reg(); const char* slot = cg_reg();
            cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 2", ip, ot->mangled, ov);
            cg_emit("%s = load ptr, ptr %s", arr, ip);
            cg_emit("%s = getelementptr %s, ptr %s, i64 %s", slot, llvm_mem_ty(ot->elem), arr, idx_ok);
            return slot;
        }
        return "null";
    }
    if (e->kind == EXPR_CALL && e->is_ptr_take) {
        int lvl = (e->call.arg_count == 2) ? (int)e->call.args[0]->int_lit.ival : 1;
        Expr* parg = e->call.args[(e->call.arg_count == 2) ? 1 : 0];
        const char* p = cg_expr(parg, NULL);
        for (int k = 1; k < lvl; k++) {
            const char* np = cg_reg();
            cg_emit("%s = load ptr, ptr %s", np, p);
            p = np;
        }
        return p;
    }
    return "null";
}

static void cg_reg_temp(const char* reg, Type* ty) {
    if (!reg || !ty || !type_is_owned(ty) || g_cg.temp_count >= 128) return;
    emit_drop_for_type(ty);
    g_cg.temps[g_cg.temp_count].reg = reg;
    g_cg.temps[g_cg.temp_count].ty = ty;
    g_cg.temps[g_cg.temp_count].active = true;
    g_cg.temp_count++;
}

static void cg_consume_temp(const char* reg) {
    if (!reg) return;
    for (int i = g_cg.temp_count - 1; i >= 0; i--) {
        if (g_cg.temps[i].active && strcmp(g_cg.temps[i].reg, reg) == 0) {
            g_cg.temps[i].active = false;
            return;
        }
    }
}

static void cg_consume_expr(Expr* e, const char* reg) {
    cg_consume_temp(reg);
    if (e && e->kind == EXPR_VAR && e->is_move && g_cg.cur_fn) {
        VarSym* lv = find_local_in_fn(g_cg.cur_fn, e->var.name);
        if (lv && lv->drop_flag_llvm && !g_cg.block_terminated) {
            cg_emit("store i1 false, ptr %s", lv->drop_flag_llvm);
        }
    }
}

static bool cg_has_active_temps(int mark) {
    for (int i = g_cg.temp_count - 1; i >= mark; i--) {
        if (g_cg.temps[i].active) return true;
    }
    return false;
}

static void cg_flush_temps_preserve(int mark) {
    for (int i = g_cg.temp_count - 1; i >= mark; i--) {
        if (g_cg.temps[i].active && !g_cg.block_terminated) {
            Type* ty = g_cg.temps[i].ty;
            emit_drop_for_type(ty);
            cg_emit("call void @_drop_%s(%s %s)", ty->mangled, llvm_mem_ty(ty), cg_reg_to_mem(g_cg.temps[i].reg, ty));
        }
    }
}

static void cg_flush_temps(int mark) {
    for (int i = g_cg.temp_count - 1; i >= mark; i--) {
        if (g_cg.temps[i].active && !g_cg.block_terminated) {
            Type* ty = g_cg.temps[i].ty;
            emit_drop_for_type(ty);
            cg_emit("call void @_drop_%s(%s %s)", ty->mangled, llvm_mem_ty(ty), cg_reg_to_mem(g_cg.temps[i].reg, ty));
            g_cg.temps[i].active = false;
        }
    }
    if (g_cg.temp_count > mark) g_cg.temp_count = mark;
}

static void cg_drop_var_if_alive(VarSym* lv) {
    if (!lv || !type_is_owned(lv->type) || !lv->drop_flag_llvm || g_cg.block_terminated) return;
    emit_drop_for_type(lv->type);
    const char* fl = cg_reg();
    cg_emit("%s = load i1, ptr %s", fl, lv->drop_flag_llvm);
    const char* l_do = cg_lbl("dv_do");
    const char* l_ok = cg_lbl("dv_ok");
    cg_cond_br(fl, l_do, l_ok);
    cg_label(l_do);
    const char* vp = (lv->llvm_name[0] == '@') ? lv->llvm_name : cg_ensure_local_alloca(lv->llvm_name, lv->type);
    const char* mv = cg_reg();
    cg_emit("%s = load %s, ptr %s", mv, llvm_mem_ty(lv->type), vp);
    if (lv->is_stack_owned) {
        cg_emit("call void @_drop_fields_%s(ptr %s)", lv->type->mangled, mv);
    } else {
        cg_emit("call void @_drop_%s(%s %s)", lv->type->mangled, llvm_mem_ty(lv->type), mv);
    }
    cg_emit("store i1 false, ptr %s", lv->drop_flag_llvm);
    cg_br(l_ok);
    cg_label(l_ok);
}

static void cg_drop_all_fn_locals(FuncInfo* fn) {
    if (!fn || g_cg.block_terminated) return;
    for (int i = 0; i < fn->local_count; i++) {
        cg_drop_var_if_alive(&fn->locals[i]);
    }
}

static bool expr_produces_fresh_temp(Expr* e) {
    if (!e || !e->ty || !type_is_owned(e->ty)) return false;
    switch (e->kind) {
        case EXPR_INT:
            return e->ty->kind == TY_BIGINT;
        case EXPR_BYTES: case EXPR_FSTRING: case EXPR_LIST: case EXPR_SET: case EXPR_DICT:
        case EXPR_LISTCOMP: case EXPR_SETCOMP: case EXPR_DICTCOMP: case EXPR_GENEXPR:
        case EXPR_TUPLE: case EXPR_SLICE: case EXPR_TERNARY:
        case EXPR_AWAIT:
        case EXPR_BINOP: case EXPR_UNOP:
            return true;
        case EXPR_INDEX:
            return e->index.obj->ty && e->index.obj->ty->kind == TY_STR;
        case EXPR_ATTR:
            if (e->attr.obj->ty && e->attr.obj->ty->kind == TY_CLASS) {
                FuncInfo* prop = find_method_in_class(e->attr.obj->ty->cls, e->attr.attr);
                return prop && prop->is_property;
            }
            return false;
        case EXPR_CALL:
            if (e->is_ptr_take) return false;
            if (e->call.callee->kind == EXPR_VAR && strcmp(e->call.callee->var.name, "super") == 0) return false;
            return true;
        default:
            return false;
    }
}

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
    if (type_is_ptr(t)) {
        const char* iv = cg_reg();
        const char* r = cg_reg();
        cg_emit("%s = ptrtoint ptr %s to i64", iv, val);
        cg_emit("%s = call ptr @stricta_hex(i64 %s)", r, iv);
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
        cg_emit("call void @stricta_str_free(ptr %s)", a1);
        cg_emit("store ptr %s, ptr %s", a2, acc_p);
        cg_br(l_item);
        cg_label(l_item);
        const char* ep = cg_reg(); const char* evm = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, mty, arr, iv);
        cg_emit("%s = load %s, ptr %s", evm, mty, ep);
        const char* ev = cg_mem_to_reg(evm, t->elem);
        const char* es = NULL;
        if (t->elem->kind == TY_STR) {
            const char* q = cg_strictastr_lit("'", 1); const char* q1 = cg_reg(); const char* q2 = cg_reg();
            cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q1, q, ev);
            cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q2, q1, q);
            cg_emit("call void @stricta_str_free(ptr %s)", q1);
            es = q2;
        } else {
            es = cg_to_str(ev, t->elem);
        }
        const char* a3 = cg_reg(); const char* a4 = cg_reg(); const char* in = cg_reg();
        cg_emit("%s = load ptr, ptr %s", a3, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", a4, a3, es);
        cg_emit("call void @stricta_str_free(ptr %s)", a3);
        cg_emit("call void @stricta_str_free(ptr %s)", es);
        cg_emit("store ptr %s, ptr %s", a4, acc_p);
        cg_emit("%s = add i32 %s, 1", in, iv);
        cg_emit("store i32 %s, ptr %s", in, i_p);
        cg_br(l_loop);
        cg_label(l_done);
        const char* af = cg_reg(); const char* res = cg_reg();
        cg_emit("%s = load ptr, ptr %s", af, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", res, af, cg_strictastr_lit("]", 1));
        cg_emit("call void @stricta_str_free(ptr %s)", af);
        return res;
    }
    if (t->kind == TY_TUPLE) {
        if (t->tuple_count == 0) return cg_strictastr_lit("()", 2);
        const char* acc = cg_strictastr_lit("(", 1);
        for (int i = 0; i < t->tuple_count; i++) {
            if (i > 0) {
                const char* n = cg_reg();
                cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, cg_strictastr_lit(", ", 2));
                cg_emit("call void @stricta_str_free(ptr %s)", acc);
                acc = n;
            }
            const char* fm = cg_reg();
            cg_emit("%s = extractvalue %s %s, %d", fm, llvm_reg_ty(t), val, i);
            const char* fv = cg_mem_to_reg(fm, t->tuple_elems[i]);
            const char* fs = NULL;
            if (t->tuple_elems[i]->kind == TY_STR) {
                const char* q = cg_strictastr_lit("'", 1); const char* q1 = cg_reg(); const char* q2 = cg_reg();
                cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q1, q, fv);
                cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", q2, q1, q);
                cg_emit("call void @stricta_str_free(ptr %s)", q1);
                fs = q2;
            } else {
                fs = cg_to_str(fv, t->tuple_elems[i]);
            }
            const char* n = cg_reg();
            cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, fs);
            cg_emit("call void @stricta_str_free(ptr %s)", acc);
            cg_emit("call void @stricta_str_free(ptr %s)", fs);
            acc = n;
        }
        const char* res = cg_reg();
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", res, acc, cg_strictastr_lit(t->tuple_count == 1 ? ",)" : ")", t->tuple_count == 1 ? 2 : 1));
        cg_emit("call void @stricta_str_free(ptr %s)", acc);
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
        cg_emit("call void @stricta_str_free(ptr %s)", a1);
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
        cg_emit("call void @stricta_str_free(ptr %s)", a3);
        if (t->elem->kind != TY_STR) cg_emit("call void @stricta_str_free(ptr %s)", evs);
        cg_emit("store ptr %s, ptr %s", a4, acc_p);
        cg_emit("%s = add i32 %s, 1", in, iv);
        cg_emit("store i32 %s, ptr %s", in, i_p);
        cg_br(l_loop);
        cg_label(l_done);
        const char* af = cg_reg(); const char* ns = cg_reg();
        cg_emit("%s = load ptr, ptr %s", af, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", ns, af, cg_strictastr_lit("}", 1));
        cg_emit("call void @stricta_str_free(ptr %s)", af);
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
        cg_emit("call void @stricta_str_free(ptr %s)", a1);
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
            cg_emit("call void @stricta_str_free(ptr %s)", q1);
            ks = q2;
        }
        const char* p1 = cg_reg(); const char* p2 = cg_reg(); const char* a3 = cg_reg(); const char* a4 = cg_reg();
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", p1, ks, cg_strictastr_lit(": ", 2));
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", p2, p1, vs);
        cg_emit("call void @stricta_str_free(ptr %s)", p1);
        cg_emit("call void @stricta_str_free(ptr %s)", ks);
        if (t->val->kind != TY_STR) cg_emit("call void @stricta_str_free(ptr %s)", vs);
        cg_emit("%s = load ptr, ptr %s", a3, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", a4, a3, p2);
        cg_emit("call void @stricta_str_free(ptr %s)", a3);
        cg_emit("call void @stricta_str_free(ptr %s)", p2);
        cg_emit("store ptr %s, ptr %s", a4, acc_p);
        const char* in = cg_reg();
        cg_emit("%s = add i32 %s, 1", in, iv);
        cg_emit("store i32 %s, ptr %s", in, i_p);
        cg_br(l_loop);
        cg_label(l_done);
        const char* af = cg_reg(); const char* res = cg_reg();
        cg_emit("%s = load ptr, ptr %s", af, acc_p);
        cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", res, af, cg_strictastr_lit("}", 1));
        cg_emit("call void @stricta_str_free(ptr %s)", af);
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
        cg_emit("call void @stricta_str_free(ptr %s)", s);
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
        if ((rt->kind == TY_NONE_LIT && lt->kind != TY_OPTIONAL && lt->kind != TY_NONE_LIT && !type_is_heap_ref(lt)) ||
            (lt->kind == TY_NONE_LIT && rt->kind != TY_OPTIONAL && rt->kind != TY_NONE_LIT && !type_is_heap_ref(rt))) {
            return neg ? "true" : "false";
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
        if ((strcmp(op, "==") == 0 || strcmp(op, "!=") == 0) &&
            (rt->kind == TY_NONE_LIT || lt->kind == TY_NONE_LIT)) {
            return cg_binop_values(is_ne ? "is not" : "is", lv, lt, rv, rt, g_ty_bool);
        }
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
        if (lt->kind == TY_BYTES && rt->kind == TY_BYTES) {
            const char* eq32 = cg_reg(); const char* r = cg_reg();
            cg_emit("%s = call i32 @stricta_bytes_eq(ptr %s, ptr %s)", eq32, lv, rv);
            cg_emit("%s = icmp %s i32 %s, 0", r, is_ne ? "eq" : "ne", eq32);
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
        if (lt->kind == TY_OPTIONAL && type_eq(lt, rt) && (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0)) {
            emit_hash_eq_for_type(lt);
            const char* eq = cg_reg();
            cg_emit("%s = call i1 @_eq_%s(%s %s, %s %s)", eq, lt->mangled, llvm_reg_ty(lt), cg_reg_to_mem(lv, lt), llvm_reg_ty(lt), cg_reg_to_mem(rv, lt));
            if (!is_ne) return eq;
            const char* r = cg_reg(); cg_emit("%s = xor i1 %s, true", r, eq); return r;
        }
        if (type_is_numeric(lt) || lt->kind == TY_BOOL) {
            Type* ct = numeric_common_type(lt, rt);
            const char* lc = cg_coerce(lv, lt, ct);
            const char* rc = cg_coerce(rv, rt, ct);
            const char* r = cg_reg();
            if (type_is_float(ct)) {
                const char* fp = strcmp(op, "==") == 0 ? "oeq" : strcmp(op, "!=") == 0 ? "une" :
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
    if (lt->kind == TY_ADDR && type_is_int(rt) && (strcmp(op, "+") == 0 || strcmp(op, "-") == 0)) {
        const char* off = cg_coerce(rv, rt, g_ty_i64);
        if (strcmp(op, "-") == 0) {
            const char* neg = cg_reg();
            cg_emit("%s = sub i64 0, %s", neg, off);
            off = neg;
        }
        const char* r = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i64 %s", r, llvm_mem_ty(lt->elem), lv, off);
        return r;
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

static const char* cg_construct_class_at(ClassInfo* cls, Expr** args, int argc, const char* prealloc_obj) {
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
                const char* raw_av = cg_expr(args[i], cls->fields[i].type);
                cg_consume_expr(args[i], raw_av);
                const char* av = cg_coerce(raw_av, args[i]->ty, cls->fields[i].type);
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
        cg_emit("call void @stricta_set_exc(i32 5, ptr null, ptr %s)", cg_cstr_lit("TypeError: cannot instantiate abstract class", 44));
        cg_raise_branch();
        return "null";
    }
    const char* obj = prealloc_obj;
    bool has_owned_fld = false;
    for (int f = 0; f < cls->field_count; f++) {
        if (type_is_owned(cls->fields[f].type)) { has_owned_fld = true; break; }
    }
    if (!obj) {
        const char* sz = cg_reg();
        obj = cg_reg();
        cg_emit("%s = ptrtoint ptr getelementptr (%s, ptr null, i32 1) to i64", sz, cls->llvm_name);
        if ((find_method_in_class(cls, "__init__") || cls->is_dataclass) && !has_owned_fld) {
            cg_emit("%s = call ptr @malloc(i64 %s)", obj, sz);
        } else {
            cg_emit("%s = call ptr @calloc(i64 1, i64 %s)", obj, sz);
        }
        cg_reg_temp(obj, cls->type);
    } else if ((!find_method_in_class(cls, "__init__") && !cls->is_dataclass) || has_owned_fld) {
        cg_emit("store %s zeroinitializer, ptr %s", cls->llvm_name, obj);
    }
    if (cls->needs_vtable) {
        const char* vp = cg_reg();
        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", vp, cls->llvm_name, obj);
        cg_emit("store ptr @_vtable_%s%s, ptr %s", cls->mod->prefix, cls->name, vp);
    }
    for (int f = 0; f < cls->field_count; f++) {
        if (cls->fields[f].default_expr) {
            const char* raw_dv = cg_expr(cls->fields[f].default_expr, cls->fields[f].type);
            cg_consume_expr(cls->fields[f].default_expr, raw_dv);
            const char* dv = cg_coerce(raw_dv, cls->fields[f].default_expr->ty, cls->fields[f].type);
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
            const char* raw_av = cg_expr(ae, pt);
            if (init_fn->param_modes && init_fn->param_modes[i] == PM_MOVE) cg_consume_expr(ae, raw_av);
            const char* av = cg_coerce(raw_av, ae->ty, pt);
            sb_printf(&call_args, ", %s %s", llvm_reg_ty(pt), av);
        }
        cg_emit_call_check(init_fn, init_fn->llvm_name, call_args.data);
        free(call_args.data);
    } else if (cls->is_dataclass) {
        for (int i = 0; i < argc && i < cls->field_count; i++) {
            const char* raw_av = cg_expr(args[i], cls->fields[i].type);
            cg_consume_expr(args[i], raw_av);
            const char* av = cg_coerce(raw_av, args[i]->ty, cls->fields[i].type);
            const char* fp = cg_reg();
            cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, cls->llvm_name, obj, cls->fields[i].slot_index);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(cls->fields[i].type), cg_reg_to_mem(av, cls->fields[i].type), fp);
        }
    }
    if (!prealloc_obj) cg_consume_temp(obj);
    return obj;
}

static const char* cg_construct_class(ClassInfo* cls, Expr** args, int argc) {
    return cg_construct_class_at(cls, args, argc, NULL);
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
        g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){after_lbl, l_step, NULL, NULL, g_cg.temp_count};
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
        VarSym* kvs = cg_find_var_sym(targets[0]);
        VarSym* vvs = cg_find_var_sym(targets[1]);
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
        if (kvs && kvs->drop_flag_llvm) { cg_drop_var_if_alive(kvs); cg_emit("store i1 false, ptr %s", kvs->drop_flag_llvm); }
        if (vvs && vvs->drop_flag_llvm) { cg_drop_var_if_alive(vvs); cg_emit("store i1 false, ptr %s", vvs->drop_flag_llvm); }
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(kvt), km, kvp);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vvt), vm, vvp);
        g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){after_lbl, l_step, NULL, NULL, g_cg.temp_count};
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
        VarSym* vvs = cg_find_var_sym(targets[0]);
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
        if (vvs && vvs->drop_flag_llvm) { cg_drop_var_if_alive(vvs); cg_emit("store i1 false, ptr %s", vvs->drop_flag_llvm); }
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vvt), vm, vvp);
        g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){after_lbl, l_step, NULL, NULL, g_cg.temp_count};
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
        VarSym* vs = cg_find_var_sym(targets[0]);
        if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(cg_coerce(elem_v, el_ty, vt), vt), vp);
        if (vs && vs->drop_flag_llvm) {
            cg_emit("store i1 %s, ptr %s", (it_ty->kind == TY_STR) ? "true" : "false", vs->drop_flag_llvm);
        }
    } else {
        for (int k = 0; k < target_count; k++) {
            Type* vt = NULL;
            const char* vp = cg_var_ptr(targets[k], &vt);
            VarSym* vs = cg_find_var_sym(targets[k]);
            if (vs && vs->drop_flag_llvm) { cg_drop_var_if_alive(vs); cg_emit("store i1 false, ptr %s", vs->drop_flag_llvm); }
            const char* fm = cg_reg();
            cg_emit("%s = extractvalue %s %s, %d", fm, llvm_reg_ty(el_ty), elem_v, k);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), fm, vp);
        }
    }
    g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){after_lbl, l_step, NULL, NULL, g_cg.temp_count};
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
    int outer_m = g_cg.temp_count;
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
            int fm = g_cg.temp_count;
            const char* fv = cg_truthy(cg_expr(cg->ifs[f], g_ty_bool), cg->ifs[f]->ty);
            cg_flush_temps(fm);
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
        cg_flush_temps(outer_m);
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
            int fm = g_cg.temp_count;
            const char* fv = cg_truthy(cg_expr(cg->ifs[f], g_ty_bool), cg->ifs[f]->ty);
            cg_flush_temps(fm);
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
        cg_flush_temps(outer_m);
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
    int bm = g_cg.temp_count;
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
        VarSym* vs = cg_find_var_sym(cg->targets[0]);
        if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(ev, el_ty), vp);
        if (vs && vs->drop_flag_llvm) {
            cg_emit("store i1 %s, ptr %s", (it_ty->kind == TY_STR) ? "true" : "false", vs->drop_flag_llvm);
        }
    } else {
        for (int k = 0; k < cg->target_count; k++) {
            Type* vt = NULL; const char* vp = cg_var_ptr(cg->targets[k], &vt);
            VarSym* vs = cg_find_var_sym(cg->targets[k]);
            if (vs && vs->drop_flag_llvm) { cg_drop_var_if_alive(vs); cg_emit("store i1 false, ptr %s", vs->drop_flag_llvm); }
            const char* fm = cg_reg();
            cg_emit("%s = extractvalue %s %s, %d", fm, llvm_reg_ty(el_ty), ev, k);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), fm, vp);
        }
    }
    for (int f = 0; f < cg->if_count; f++) {
        int fm = g_cg.temp_count;
        const char* fv = cg_truthy(cg_expr(cg->ifs[f], g_ty_bool), cg->ifs[f]->ty);
        cg_flush_temps(fm);
        const char* l_ok = cg_lbl("cg_ok");
        cg_cond_br(fv, l_ok, l_step);
        cg_label(l_ok);
    }
    cg_comp_loop_emit(gens, gidx + 1, gcount, emit_leaf, ctx);
    cg_br(l_step);
    cg_label(l_step);
    cg_flush_temps(bm);
    if (it_ty->kind == TY_STR && cg->target_count == 1) {
        VarSym* vs = cg_find_var_sym(cg->targets[0]);
        if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
    }
    const char* iv2 = cg_reg(); const char* in = cg_reg();
    cg_emit("%s = load i32, ptr %s", iv2, i_p);
    cg_emit("%s = add i32 %s, 1", in, iv2);
    cg_emit("store i32 %s, ptr %s", in, i_p);
    cg_br(l_cond);
    cg_label(l_after);
    cg_flush_temps(outer_m);
}

typedef struct { Expr* comp; const char* container; Type* cty; const char* exit_lbl; bool is_all; int exit_tmark; } CompLeafCtx;

static void comp_leaf_list(void* vctx) {
    CompLeafCtx* c = (CompLeafCtx*)vctx;
    int lm = g_cg.temp_count;
    Type* el = c->cty->elem;
    const char* raw = cg_expr(c->comp->comp.elt, el);
    cg_consume_expr(c->comp->comp.elt, raw);
    const char* ev = cg_coerce(raw, c->comp->comp.elt->ty, el);
    cg_emit("call void @_%s_append(ptr %s, %s %s)", c->cty->mangled, c->container, llvm_mem_ty(el), cg_reg_to_mem(ev, el));
    cg_flush_temps(lm);
}

static void comp_leaf_set(void* vctx) {
    CompLeafCtx* c = (CompLeafCtx*)vctx;
    int lm = g_cg.temp_count;
    Type* el = c->cty->elem;
    const char* raw = cg_expr(c->comp->comp.elt, el);
    cg_consume_expr(c->comp->comp.elt, raw);
    const char* ev = cg_coerce(raw, c->comp->comp.elt->ty, el);
    cg_emit("call void @_%s_add(ptr %s, %s %s)", c->cty->mangled, c->container, llvm_mem_ty(el), cg_reg_to_mem(ev, el));
    cg_flush_temps(lm);
}

static void comp_leaf_dict(void* vctx) {
    CompLeafCtx* c = (CompLeafCtx*)vctx;
    int lm = g_cg.temp_count;
    Type* kt = c->cty->key; Type* vt = c->cty->val;
    const char* raw_k = cg_expr(c->comp->comp.elt, kt);
    cg_consume_expr(c->comp->comp.elt, raw_k);
    const char* raw_v = cg_expr(c->comp->comp.val, vt);
    cg_consume_expr(c->comp->comp.val, raw_v);
    const char* kv = cg_coerce(raw_k, c->comp->comp.elt->ty, kt);
    const char* vv = cg_coerce(raw_v, c->comp->comp.val->ty, vt);
    cg_emit("call void @_%s_set(ptr %s, %s %s, %s %s)", c->cty->mangled, c->container,
            llvm_mem_ty(kt), cg_reg_to_mem(kv, kt), llvm_mem_ty(vt), cg_reg_to_mem(vv, vt));
    cg_flush_temps(lm);
}

static void comp_leaf_sum(void* vctx) {
    CompLeafCtx* c = (CompLeafCtx*)vctx;
    int lm = g_cg.temp_count;
    Type* el = c->cty;
    const char* ev = cg_coerce(cg_expr(c->comp->comp.elt, el), c->comp->comp.elt->ty, el);
    const char* cur = cg_reg(); const char* sum = cg_reg();
    cg_emit("%s = load %s, ptr %s", cur, llvm_reg_ty(el), c->container);
    if (type_is_float(el)) cg_emit("%s = fadd %s %s, %s", sum, llvm_reg_ty(el), cur, ev);
    else cg_emit("%s = add %s %s, %s", sum, llvm_reg_ty(el), cur, ev);
    cg_emit("store %s %s, ptr %s", llvm_reg_ty(el), sum, c->container);
    cg_flush_temps(lm);
}

static void comp_leaf_any_all(void* vctx) {
    CompLeafCtx* c = (CompLeafCtx*)vctx;
    int lm = g_cg.temp_count;
    const char* b = cg_truthy(cg_expr(c->comp->comp.elt, NULL), c->comp->comp.elt->ty);
    cg_flush_temps(lm);
    const char* l_hit = cg_lbl("aa_h"); const char* l_cont = cg_lbl("aa_c");
    if (c->is_all) cg_cond_br(b, l_cont, l_hit); else cg_cond_br(b, l_hit, l_cont);
    cg_label(l_hit);
    cg_flush_temps_preserve(c->exit_tmark);
    cg_emit("store i1 %s, ptr %s", c->is_all ? "false" : "true", c->container);
    cg_br(c->exit_lbl);
    cg_label(l_cont);
}

static const char* cg_method_call(Expr* call, Expr* obj_e, const char* mname, Type* obj_ty) {
    int argc = call->call.arg_count;
    Expr** args = call->call.args;

    obj_ty = type_deref_all(obj_ty);

    if (obj_ty->kind == TY_STR) {
        const char* self_v = cg_eval_recv(obj_e);
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
                        cg_emit("call void @stricta_str_free(ptr %s)", acc);
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
                    bool free_part = false;
                    if (aidx >= 0 && aidx < argc) {
                        if (colon && strchr(colon + 1, 'f')) {
                            const char* dot = strchr(colon + 1, '.');
                            int prec = dot ? atoi(dot + 1) : 6;
                            const char* fv = cg_coerce(arg_strs[aidx], args[aidx]->ty, g_ty_f64);
                            part = cg_reg();
                            cg_emit("%s = call ptr @stricta_float_fmt(double %s, i32 %d)", part, fv, prec);
                            free_part = true;
                        } else {
                            part = cg_to_str(arg_strs[aidx], args[aidx]->ty);
                            free_part = (args[aidx]->ty->kind != TY_STR);
                        }
                    } else {
                        part = cg_strictastr_lit("", 0);
                    }
                    const char* n = cg_reg();
                    cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, part);
                    cg_emit("call void @stricta_str_free(ptr %s)", acc);
                    if (free_part) cg_emit("call void @stricta_str_free(ptr %s)", part);
                    acc = n;
                } else {
                    sb_append_n(&lit, &fmt[p], 1);
                    p++;
                }
            }
            if (lit.len > 0) {
                const char* n = cg_reg();
                cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, cg_strictastr_lit(lit.data, lit.len));
                cg_emit("call void @stricta_str_free(ptr %s)", acc);
                acc = n;
            }
            free(lit.data);
            return acc;
        }
        if (strcmp(mname, "copy") == 0) {
            const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_str_clone(ptr %s)", r, self_v); return r;
        }
        return cg_strictastr_lit("", 0);
    }

    if (obj_ty->kind == TY_BYTES || obj_ty->kind == TY_BIGINT || obj_ty->kind == TY_TUPLE) {
        const char* ov = cg_eval_recv(obj_e);
        if (strcmp(mname, "copy") == 0) {
            if (!type_is_owned(obj_ty)) return ov;
            emit_clone_for_type(obj_ty);
            const char* r = cg_reg();
            cg_emit("%s = call %s @_clone_%s(%s %s)", r, llvm_reg_ty(obj_ty), obj_ty->mangled, llvm_reg_ty(obj_ty), ov);
            return r;
        }
        return ov;
    }

    if (obj_ty->kind == TY_FILE) {
        const char* fv = cg_eval_recv(obj_e);
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
        const char* lv = cg_eval_recv(obj_e);
        const char* m = obj_ty->mangled;
        Type* el = obj_ty->elem;
        const char* mty = llvm_mem_ty(el);
        if (strcmp(mname, "append") == 0) {
            const char* raw_av = cg_expr(args[0], el);
            cg_consume_expr(args[0], raw_av);
            const char* av = cg_reg_to_mem(cg_coerce(raw_av, args[0]->ty, el), el);
            cg_emit("call void @_%s_append(ptr %s, %s %s)", m, lv, mty, av); return "0";
        }
        if (strcmp(mname, "extend") == 0) {
            const char* ov = cg_expr(args[0], obj_ty);
            cg_emit("call void @_%s_extend(ptr %s, ptr %s)", m, lv, ov); return "0";
        }
        if (strcmp(mname, "insert") == 0) {
            const char* iv = cg_coerce(cg_expr(args[0], g_ty_i64), args[0]->ty, g_ty_i64);
            const char* raw_vv = cg_expr(args[1], el);
            cg_consume_expr(args[1], raw_vv);
            const char* vv = cg_reg_to_mem(cg_coerce(raw_vv, args[1]->ty, el), el);
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
            cg_emit("call void @_%s_clear(ptr %s)", m, lv); return "0";
        }
        if (strcmp(mname, "copy") == 0) {
            const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_copy(ptr %s)", r, m, lv); return r;
        }
    }

    if (obj_ty->kind == TY_SET) {
        const char* sv = cg_eval_recv(obj_e);
        const char* m = obj_ty->mangled;
        Type* el = obj_ty->elem;
        const char* mty = llvm_mem_ty(el);
        if (strcmp(mname, "add") == 0) {
            const char* raw_av = cg_expr(args[0], el);
            cg_consume_expr(args[0], raw_av);
            const char* av = cg_reg_to_mem(cg_coerce(raw_av, args[0]->ty, el), el);
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
        const char* dv = cg_eval_recv(obj_e);
        const char* m = obj_ty->mangled;
        Type* kt = obj_ty->key; Type* vt = obj_ty->val;
        const char* kmty = llvm_mem_ty(kt); const char* vmty = llvm_mem_ty(vt);
        if (strcmp(mname, "keys") == 0 || strcmp(mname, "values") == 0 || strcmp(mname, "items") == 0 || strcmp(mname, "copy") == 0) {
            const char* r = cg_reg(); cg_emit("%s = call ptr @_%s_%s(ptr %s)", r, m, mname, dv); return r;
        }
        if (strcmp(mname, "get") == 0 || strcmp(mname, "setdefault") == 0) {
            const char* raw_kv = cg_expr(args[0], kt);
            const char* raw_vv = cg_expr(args[1], vt);
            if (strcmp(mname, "setdefault") == 0) {
                cg_consume_expr(args[0], raw_kv);
                cg_consume_expr(args[1], raw_vv);
            }
            const char* kv = cg_reg_to_mem(cg_coerce(raw_kv, args[0]->ty, kt), kt);
            const char* vv = cg_reg_to_mem(cg_coerce(raw_vv, args[1]->ty, vt), vt);
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
                const char* raw_av = cg_expr(args[i], pt);
                if (def_m->param_modes && def_m->param_modes[i + 1] == PM_MOVE) cg_consume_expr(args[i], raw_av);
                const char* av = cg_coerce(raw_av, args[i]->ty, pt);
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

            const char* rty = llvm_fn_ret_ty(def_m);
            if (alt_cnt == 0) {
                const char* r = cg_emit_call_check(def_m, def_m->llvm_name, ca.data);
                free(ca.data);
                return r;
            }

            const char* vpp = cg_reg(); const char* vt = cg_reg(); const char* tid = cg_reg();
            cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", vpp, self_cls->llvm_name, self_reg);
            cg_emit("%s = load ptr, ptr %s", vt, vpp);
            cg_emit("%s = load i32, ptr %s", tid, vt);
            const char* l_def = cg_lbl("sup_def");
            const char* l_end = cg_lbl("sup_end");
            bool has_ret_slot = (def_m->can_raise || def_m->ret_type->kind != TY_VOID);
            const char* res_p = has_ret_slot ? cg_reg() : NULL;
            if (res_p) cg_entry("%s = alloca %s", res_p, rty);

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
                if (!has_ret_slot) {
                    cg_emit("call void %s(%s)", alt_fns[k]->llvm_name, ca.data);
                } else {
                    const char* cr = cg_reg();
                    cg_emit("%s = call %s %s(%s)", cr, rty, alt_fns[k]->llvm_name, ca.data);
                    cg_emit("store %s %s, ptr %s", rty, cr, res_p);
                }
                cg_br(l_end);
            }
            cg_label(l_def);
            if (!has_ret_slot) {
                cg_emit("call void %s(%s)", def_m->llvm_name, ca.data);
            } else {
                const char* cr = cg_reg();
                cg_emit("%s = call %s %s(%s)", cr, rty, def_m->llvm_name, ca.data);
                cg_emit("store %s %s, ptr %s", rty, cr, res_p);
            }
            cg_br(l_end);
            cg_label(l_end);
            free(ca.data);
            if (!res_p) return "0";
            const char* raw = cg_reg();
            cg_emit("%s = load %s, ptr %s", raw, rty, res_p);
            if (!def_m->can_raise) return raw;
            const char* is_exc = raw;
            if (def_m->ret_type->kind != TY_VOID) {
                is_exc = cg_reg();
                cg_emit("%s = extractvalue %s %s, 0", is_exc, rty, raw);
            }
            const char* l_ok = cg_lbl("sup_ok");
            if (g_cg.cur_unwind_lbl) {
                if (cg_has_active_temps(0)) {
                    const char* l_ed = cg_lbl("sup_ed");
                    cg_cond_br(is_exc, l_ed, l_ok);
                    cg_label(l_ed);
                    cg_flush_temps_preserve(0);
                    cg_br(g_cg.cur_unwind_lbl);
                } else {
                    cg_cond_br(is_exc, g_cg.cur_unwind_lbl, l_ok);
                }
            } else {
                const char* l_unh = cg_lbl("sup_unh");
                cg_cond_br(is_exc, l_unh, l_ok);
                cg_label(l_unh);
                cg_emit("call void @stricta_unhandled_exc()");
                cg_emit("unreachable");
                g_cg.block_terminated = true;
            }
            cg_label(l_ok);
            if (def_m->ret_type->kind == TY_VOID) return "0";
            const char* val = cg_reg();
            cg_emit("%s = extractvalue %s %s, 1", val, rty, raw);
            return val;
        }

        FuncInfo* m = call->call.resolved_fn ? call->call.resolved_fn : find_method_in_class(cls, mname);
        if (!m && strcmp(mname, "copy") == 0) {
            const char* ov = cg_eval_recv(obj_e);
            if (!type_is_owned(obj_ty)) return ov;
            emit_clone_for_type(obj_ty);
            const char* r = cg_reg();
            cg_emit("%s = call %s @_clone_%s(%s %s)", r, llvm_reg_ty(obj_ty), obj_ty->mangled, llvm_reg_ty(obj_ty), ov);
            return r;
        }
        bool call_on_class_type = (obj_e->kind == EXPR_VAR && find_class_in_mod(g_cg.cur_mod, obj_e->var.name) != NULL);
        const char* self_val = call_on_class_type ? NULL : cg_eval_recv(obj_e);
        if (!call_on_class_type && !m->is_static && !m->is_classmethod && m->param_count > 0 &&
            m->param_modes && m->param_modes[0] == PM_MOVE) {
            cg_consume_expr(obj_e, self_val);
        }
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
            const char* raw_av = cg_expr(ae, pt);
            if (m->param_modes && m->param_modes[i] == PM_MOVE) cg_consume_expr(ae, raw_av);
            const char* av = cg_coerce(raw_av, ae->ty, pt);
            if (ca.len > 0) sb_append(&ca, ", ");
            sb_printf(&ca, "%s %s", llvm_reg_ty(pt), av);
        }

        const char* rty = llvm_fn_ret_ty(m);
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
                bool has_ret_slot = (m->can_raise || m->ret_type->kind != TY_VOID);
                const char* res_p = has_ret_slot ? cg_reg() : NULL;
                if (res_p) cg_entry("%s = alloca %s", res_p, rty);

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
                    if (!has_ret_slot) {
                        cg_emit("call void %s(%s)", ov_fns[k]->llvm_name, ca.data);
                    } else {
                        const char* cr = cg_reg();
                        cg_emit("%s = call %s %s(%s)", cr, rty, ov_fns[k]->llvm_name, ca.data);
                        cg_emit("store %s %s, ptr %s", rty, cr, res_p);
                    }
                    cg_br(l_end);
                }
                cg_label(l_def);
                if (!has_ret_slot) {
                    cg_emit("call void %s(%s)", m->llvm_name, ca.data);
                } else {
                    const char* cr = cg_reg();
                    cg_emit("%s = call %s %s(%s)", cr, rty, m->llvm_name, ca.data);
                    cg_emit("store %s %s, ptr %s", rty, cr, res_p);
                }
                cg_br(l_end);
                cg_label(l_end);
                free(ca.data);
                if (!res_p) return "0";
                const char* raw = cg_reg();
                cg_emit("%s = load %s, ptr %s", raw, rty, res_p);
                if (!m->can_raise) return raw;
                const char* is_exc = raw;
                if (m->ret_type->kind != TY_VOID) {
                    is_exc = cg_reg();
                    cg_emit("%s = extractvalue %s %s, 0", is_exc, rty, raw);
                }
                const char* l_ok = cg_lbl("v_ok");
                if (g_cg.cur_unwind_lbl) {
                    if (cg_has_active_temps(0)) {
                        const char* l_ed = cg_lbl("v_ed");
                        cg_cond_br(is_exc, l_ed, l_ok);
                        cg_label(l_ed);
                        cg_flush_temps_preserve(0);
                        cg_br(g_cg.cur_unwind_lbl);
                    } else {
                        cg_cond_br(is_exc, g_cg.cur_unwind_lbl, l_ok);
                    }
                } else {
                    const char* l_unh = cg_lbl("v_unh");
                    cg_cond_br(is_exc, l_unh, l_ok);
                    cg_label(l_unh);
                    cg_emit("call void @stricta_unhandled_exc()");
                    cg_emit("unreachable");
                    g_cg.block_terminated = true;
                }
                cg_label(l_ok);
                if (m->ret_type->kind == TY_VOID) return "0";
                const char* val = cg_reg();
                cg_emit("%s = extractvalue %s %s, 1", val, rty, raw);
                return val;
            }
        }
        const char* r = cg_emit_call_check(m, m->llvm_name, ca.data);
        free(ca.data); return r;
    }

    if (obj_ty->kind == TY_MODULE) {
        ModuleInfo* mmod = obj_ty->mod;
        FuncInfo* mf = call->call.resolved_fn ? call->call.resolved_fn : find_func_in_mod(mmod, mname);
        if (mf) {
            StrBuf ca; sb_init(&ca);
            for (int i = 0; i < mf->param_count; i++) {
                Expr* ae = (i < argc) ? args[i] : mf->param_defaults[i];
                Type* pt = mf->param_types[i];
                const char* raw_av = cg_expr(ae, pt);
                if (mf->param_modes && mf->param_modes[i] == PM_MOVE) cg_consume_expr(ae, raw_av);
                const char* av = cg_coerce(raw_av, ae->ty, pt);
                if (i > 0) sb_append(&ca, ", ");
                sb_printf(&ca, "%s %s", llvm_reg_ty(pt), av);
            }
            const char* r = cg_emit_call_check(mf, mf->llvm_name, ca.data);
            free(ca.data); return r;
        }
        ClassInfo* mci = find_class_in_mod(mmod, mname);
        if (mci) return cg_construct_class(mci, args, argc);
    }

    return "0";
}

static const char* cg_expr_inner(Expr* e, Type* hint) {
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
                bool free_ps = false;
                if (!fp->is_expr) {
                    ps = cg_strictastr_lit(fp->text, strlen(fp->text));
                } else {
                    const char* ev = cg_expr(fp->expr, NULL);
                    if (fp->fmt_spec && strchr(fp->fmt_spec, 'f')) {
                        const char* dot = strchr(fp->fmt_spec, '.');
                        int prec = dot ? atoi(dot + 1) : 6;
                        ps = cg_reg();
                        cg_emit("%s = call ptr @stricta_float_fmt(double %s, i32 %d)", ps, cg_coerce(ev, fp->expr->ty, g_ty_f64), prec);
                        free_ps = true;
                    } else {
                        ps = cg_to_str(ev, fp->expr->ty);
                        free_ps = (fp->expr->ty->kind != TY_STR);
                    }
                }
                const char* n = cg_reg();
                cg_emit("%s = call ptr @stricta_str_concat(ptr %s, ptr %s)", n, acc, ps);
                cg_emit("call void @stricta_str_free(ptr %s)", acc);
                if (free_ps) cg_emit("call void @stricta_str_free(ptr %s)", ps);
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
                const char* rv = cg_mem_to_reg(mv, vt);
                if (vt->kind == TY_OPTIONAL && ty->kind != TY_OPTIONAL && !type_is_heap_ref(vt->elem)) {
                    const char* unwrapped = cg_reg();
                    cg_emit("%s = extractvalue %s %s, 1", unwrapped, llvm_reg_ty(vt), rv);
                    return cg_mem_to_reg(unwrapped, vt->elem);
                }
                return rv;
            }
            FuncInfo* fi = find_func_in_mod(g_cg.cur_mod, n);
            if (fi) return fi->llvm_name;
            return "null";
        }
        case EXPR_WALRUS: {
            const char* raw_v = cg_expr(e->walrus.val, ty);
            cg_consume_expr(e->walrus.val, raw_v);
            Type* vt = NULL;
            const char* vp = cg_var_ptr(e->walrus.name, &vt);
            VarSym* vs = cg_find_var_sym(e->walrus.name);
            if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
            const char* cv = cg_coerce(raw_v, e->walrus.val->ty, vt);
            cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(cv, vt), vp);
            if (vs && vs->drop_flag_llvm) cg_emit("store i1 true, ptr %s", vs->drop_flag_llvm);
            return cv;
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
        case EXPR_AWAIT: {
            const char* tv = cg_expr(e->await_e.expr, NULL);
            cg_consume_expr(e->await_e.expr, tv);
            Type* inner = e->ty;
            const char* bits = cg_reg();
            cg_emit("%s = call i64 @stricta_task_await(ptr %s)", bits, tv);
            const char* tid = cg_reg();
            cg_emit("%s = call i32 @stricta_exc_type_id()", tid);
            const char* has_e = cg_reg();
            cg_emit("%s = icmp ne i32 %s, 0", has_e, tid);
            const char* l_exc = cg_lbl("await_exc");
            const char* l_ok = cg_lbl("await_ok");
            cg_cond_br(has_e, l_exc, l_ok);
            cg_label(l_exc);
            cg_raise_branch();
            cg_label(l_ok);
            if (!inner || inner->kind == TY_VOID) return "0";
            if (inner->kind == TY_BOOL) {
                const char* r = cg_reg();
                cg_emit("%s = trunc i64 %s to i1", r, bits);
                return r;
            }
            if (type_is_int(inner)) {
                if (inner->kind == TY_I64 || inner->kind == TY_U64) return bits;
                const char* r = cg_reg();
                cg_emit("%s = trunc i64 %s to %s", r, bits, llvm_reg_ty(inner));
                return r;
            }
            if (inner->kind == TY_F64) {
                const char* r = cg_reg();
                cg_emit("%s = bitcast i64 %s to double", r, bits);
                return r;
            }
            if (inner->kind == TY_F32) {
                const char* d = cg_reg(); const char* r = cg_reg();
                cg_emit("%s = bitcast i64 %s to double", d, bits);
                cg_emit("%s = fptrunc double %s to float", r, d);
                return r;
            }
            if (inner->kind == TY_TUPLE || (inner->kind == TY_OPTIONAL && !type_is_heap_ref(inner->elem)) ||
                (inner->kind == TY_CLASS && inner->cls->is_value_type)) {
                const char* box = cg_reg(); const char* r = cg_reg();
                cg_emit("%s = inttoptr i64 %s to ptr", box, bits);
                cg_emit("%s = load %s, ptr %s", r, llvm_mem_ty(inner), box);
                cg_emit("call void @stricta_free(ptr %s)", box);
                return r;
            }
            const char* r = cg_reg();
            cg_emit("%s = inttoptr i64 %s to ptr", r, bits);
            return r;
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
                if (type_is_owned(ty)) {
                    emit_clone_for_type(ty);
                    const char* svc = cg_reg();
                    cg_emit("%s = call %s @_clone_%s(%s %s)", svc, llvm_mem_ty(ty), ty->mangled, llvm_mem_ty(ty), cg_reg_to_mem(sv, ty));
                    sv = cg_mem_to_reg(svc, ty);
                }
                cg_br(l_end);

                cg_label(l_rhs);
                int rm = g_cg.temp_count;
                const char* rv = cg_expr(e->binop.right, ty);
                const char* rb = (ty->kind == TY_BOOL) ? cg_truthy(rv, e->binop.right->ty) : cg_coerce(rv, e->binop.right->ty, ty);
                if (type_is_owned(ty)) {
                    emit_clone_for_type(ty);
                    const char* rbc = cg_reg();
                    cg_emit("%s = call %s @_clone_%s(%s %s)", rbc, llvm_mem_ty(ty), ty->mangled, llvm_mem_ty(ty), cg_reg_to_mem(rb, ty));
                    rb = cg_mem_to_reg(rbc, ty);
                }
                cg_flush_temps(rm);
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
            int tm = g_cg.temp_count;
            const char* raw_t = cg_expr(e->ternary.then_e, ty);
            if (type_is_owned(ty)) {
                if (e->ternary.then_e->is_move || expr_produces_fresh_temp(e->ternary.then_e)) {
                    cg_consume_expr(e->ternary.then_e, raw_t);
                } else {
                    emit_clone_for_type(ty);
                    const char* tc = cg_reg();
                    cg_emit("%s = call %s @_clone_%s(%s %s)", tc, llvm_mem_ty(ty), ty->mangled, llvm_mem_ty(ty),
                            cg_reg_to_mem(cg_coerce(raw_t, e->ternary.then_e->ty, ty), ty));
                    raw_t = cg_mem_to_reg(tc, ty);
                }
            }
            const char* tv = cg_coerce(raw_t, e->ternary.then_e->ty, ty);
            cg_flush_temps(tm);
            const char* l_te = cg_lbl("tern_te"); cg_br(l_te); cg_label(l_te); cg_br(l_end);
            cg_label(l_f);
            int fm = g_cg.temp_count;
            const char* raw_f = cg_expr(e->ternary.else_e, ty);
            if (type_is_owned(ty)) {
                if (e->ternary.else_e->is_move || expr_produces_fresh_temp(e->ternary.else_e)) {
                    cg_consume_expr(e->ternary.else_e, raw_f);
                } else {
                    emit_clone_for_type(ty);
                    const char* fc = cg_reg();
                    cg_emit("%s = call %s @_clone_%s(%s %s)", fc, llvm_mem_ty(ty), ty->mangled, llvm_mem_ty(ty),
                            cg_reg_to_mem(cg_coerce(raw_f, e->ternary.else_e->ty, ty), ty));
                    raw_f = cg_mem_to_reg(fc, ty);
                }
            }
            const char* fv = cg_coerce(raw_f, e->ternary.else_e->ty, ty);
            cg_flush_temps(fm);
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
                const char* raw_iv = cg_expr(e->seq.items[i], ty->elem);
                cg_consume_expr(e->seq.items[i], raw_iv);
                const char* iv = cg_coerce(raw_iv, e->seq.items[i]->ty, ty->elem);
                cg_emit("call void @_%s_append(ptr %s, %s %s)", ty->mangled, l, llvm_mem_ty(ty->elem), cg_reg_to_mem(iv, ty->elem));
            }
            return l;
        }
        case EXPR_TUPLE: {
            const char* t = "zeroinitializer";
            for (int i = 0; i < e->seq.count; i++) {
                const char* raw_iv = cg_expr(e->seq.items[i], ty->tuple_elems[i]);
                cg_consume_expr(e->seq.items[i], raw_iv);
                const char* iv = cg_coerce(raw_iv, e->seq.items[i]->ty, ty->tuple_elems[i]);
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
                const char* raw_iv = cg_expr(e->seq.items[i], ty->elem);
                cg_consume_expr(e->seq.items[i], raw_iv);
                const char* iv = cg_coerce(raw_iv, e->seq.items[i]->ty, ty->elem);
                cg_emit("call void @_%s_add(ptr %s, %s %s)", ty->mangled, s, llvm_mem_ty(ty->elem), cg_reg_to_mem(iv, ty->elem));
            }
            return s;
        }
        case EXPR_DICT: {
            const char* d = cg_reg();
            int cap = e->dict.count > 8 ? e->dict.count : 8;
            cg_emit("%s = call ptr @_%s_new(i32 %d)", d, ty->mangled, cap);
            for (int i = 0; i < e->dict.count; i++) {
                const char* raw_kv = cg_expr(e->dict.keys[i], ty->key);
                cg_consume_expr(e->dict.keys[i], raw_kv);
                const char* raw_vv = cg_expr(e->dict.vals[i], ty->val);
                cg_consume_expr(e->dict.vals[i], raw_vv);
                const char* kv = cg_coerce(raw_kv, e->dict.keys[i]->ty, ty->key);
                const char* vv = cg_coerce(raw_vv, e->dict.vals[i]->ty, ty->val);
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
            CompLeafCtx ctx = {e, l, ty, NULL, false, 0};
            cg_comp_loop_emit(e->comp.gens, 0, e->comp.gen_count, comp_leaf_list, &ctx);
            return l;
        }
        case EXPR_SETCOMP: {
            const char* s = cg_reg();
            cg_emit("%s = call ptr @_%s_new(i32 16)", s, ty->mangled);
            CompLeafCtx ctx = {e, s, ty, NULL, false, 0};
            cg_comp_loop_emit(e->comp.gens, 0, e->comp.gen_count, comp_leaf_set, &ctx);
            return s;
        }
        case EXPR_DICTCOMP: {
            const char* d = cg_reg();
            cg_emit("%s = call ptr @_%s_new(i32 16)", d, ty->mangled);
            CompLeafCtx ctx = {e, d, ty, NULL, false, 0};
            cg_comp_loop_emit(e->comp.gens, 0, e->comp.gen_count, comp_leaf_dict, &ctx);
            return d;
        }
        case EXPR_INDEX: {
            Type* ptr_ot = e->index.obj->ty;
            while (ptr_ot && (ptr_ot->kind == TY_REF || ptr_ot->kind == TY_MUT_REF)) ptr_ot = ptr_ot->elem;
            if (ptr_ot && ptr_ot->kind == TY_ADDR) {
                const char* pv = cg_expr(e->index.obj, NULL);
                Type* rt = e->index.obj->ty;
                while (rt && (rt->kind == TY_REF || rt->kind == TY_MUT_REF)) {
                    const char* np = cg_reg();
                    cg_emit("%s = load ptr, ptr %s", np, pv);
                    pv = np;
                    rt = rt->elem;
                }
                const char* iv = cg_coerce(cg_expr(e->index.index, g_ty_i64), e->index.index->ty, g_ty_i64);
                const char* slot = cg_reg(); const char* mv = cg_reg();
                cg_emit("%s = getelementptr %s, ptr %s, i64 %s", slot, llvm_mem_ty(ptr_ot->elem), pv, iv);
                cg_emit("%s = load %s, ptr %s", mv, llvm_mem_ty(ptr_ot->elem), slot);
                return cg_mem_to_reg(mv, ptr_ot->elem);
            }
            const char* ov = cg_eval_recv(e->index.obj);
            Type* ot = type_deref_all(e->index.obj->ty);
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
                if (e->is_inbounds) {
                    const char* ip = cg_reg(); const char* arr = cg_reg(); const char* slot = cg_reg();
                    cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 2", ip, ot->mangled, ov);
                    cg_emit("%s = load ptr, ptr %s", arr, ip);
                    cg_emit("%s = getelementptr %s, ptr %s, i64 %s", slot, llvm_mem_ty(ot->elem), arr, i64);
                    cg_emit("%s = load %s, ptr %s", mv, llvm_mem_ty(ot->elem), slot);
                } else {
                    cg_emit("%s = call %s @_%s_%s(ptr %s, i64 %s)", mv, llvm_mem_ty(ot->elem), ot->mangled,
                            e->index.index->is_nonneg ? "get_nn" : "get", ov, i64);
                }
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
                const char* i64 = cg_coerce(iv, e->index.index->ty, g_ty_i64);
                const char* lp = cg_reg(); const char* len32 = cg_reg(); const char* len64 = cg_reg();
                const char* is_neg = cg_reg(); const char* adj = cg_reg(); const char* idx_ok = cg_reg();
                const char* ok = cg_reg();
                cg_emit("%s = getelementptr %%StrictaBytes, ptr %s, i32 0, i32 0", lp, ov);
                cg_emit("%s = load i32, ptr %s", len32, lp);
                cg_emit("%s = sext i32 %s to i64", len64, len32);
                cg_emit("%s = icmp slt i64 %s, 0", is_neg, i64);
                cg_emit("%s = add i64 %s, %s", adj, i64, len64);
                cg_emit("%s = select i1 %s, i64 %s, i64 %s", idx_ok, is_neg, adj, i64);
                cg_emit("%s = icmp ult i64 %s, %s", ok, idx_ok, len64);
                const char* l_inb = cg_lbl("b_inb"); const char* l_oob = cg_lbl("b_oob");
                cg_cond_br(ok, l_inb, l_oob);
                cg_label(l_oob);
                cg_emit("call void @stricta_bounds_fail(i64 %s, i64 %s, ptr null, i32 0)", i64, len64);
                cg_emit("unreachable");
                cg_label(l_inb);
                const char* dp = cg_reg(); const char* data = cg_reg(); const char* bp = cg_reg();
                const char* b = cg_reg(); const char* r = cg_reg();
                cg_emit("%s = getelementptr %%StrictaBytes, ptr %s, i32 0, i32 2", dp, ov);
                cg_emit("%s = load ptr, ptr %s", data, dp);
                cg_emit("%s = getelementptr i8, ptr %s, i64 %s", bp, data, idx_ok);
                cg_emit("%s = load i8, ptr %s", b, bp);
                cg_emit("%s = zext i8 %s to i64", r, b);
                return r;
            }
            return "0";
        }
        case EXPR_SLICE: {
            const char* ov = cg_eval_recv(e->slice.obj);
            Type* ot = type_deref_all(e->slice.obj->ty);
            if (!e->slice.step) {
                const char* st = e->slice.start ? cg_coerce(cg_expr(e->slice.start, g_ty_i64), e->slice.start->ty, g_ty_i64) : "0";
                const char* sp = e->slice.stop ? cg_coerce(cg_expr(e->slice.stop, g_ty_i64), e->slice.stop->ty, g_ty_i64) : "9223372036854775807";
                const char* r = cg_reg();
                if (ot->kind == TY_STR) cg_emit("%s = call ptr @stricta_str_slice(ptr %s, i64 %s, i64 %s)", r, ov, st, sp);
                else if (ot->kind == TY_BYTES) cg_emit("%s = call ptr @stricta_bytes_slice(ptr %s, i64 %s, i64 %s)", r, ov, st, sp);
                else cg_emit("%s = call ptr @_%s_slice(ptr %s, i64 %s, i64 %s)", r, ot->mangled, ov, st, sp);
                return r;
            }
            const char* st = e->slice.start ? cg_coerce(cg_expr(e->slice.start, g_ty_i64), e->slice.start->ty, g_ty_i64) : "0";
            const char* sp = e->slice.stop ? cg_coerce(cg_expr(e->slice.stop, g_ty_i64), e->slice.stop->ty, g_ty_i64) : "0";
            const char* stp = cg_coerce(cg_expr(e->slice.step, g_ty_i64), e->slice.step->ty, g_ty_i64);
            int hs = e->slice.start ? 1 : 0;
            int he = e->slice.stop ? 1 : 0;
            if (!(e->slice.step->kind == EXPR_INT && e->slice.step->int_lit.ival != 0)) {
                const char* is_z = cg_reg();
                cg_emit("%s = icmp eq i64 %s, 0", is_z, stp);
                const char* lz = cg_lbl("slice_zero");
                const char* lok = cg_lbl("slice_ok");
                cg_cond_br(is_z, lz, lok);
                cg_label(lz);
                cg_emit("call void @stricta_set_exc(i32 1, ptr null, ptr %s)", cg_cstr_lit("ValueError: slice step cannot be zero", 37));
                cg_raise_branch();
                cg_label(lok);
            }
            const char* r = cg_reg();
            if (ot->kind == TY_STR) cg_emit("%s = call ptr @stricta_str_slice_step(ptr %s, i64 %s, i32 %d, i64 %s, i32 %d, i64 %s)", r, ov, st, hs, sp, he, stp);
            else if (ot->kind == TY_BYTES) cg_emit("%s = call ptr @stricta_bytes_slice_step(ptr %s, i64 %s, i32 %d, i64 %s, i32 %d, i64 %s)", r, ov, st, hs, sp, he, stp);
            else cg_emit("%s = call ptr @_%s_slice_step(ptr %s, i64 %s, i32 %d, i64 %s, i32 %d, i64 %s)", r, ot->mangled, ov, st, hs, sp, he, stp);
            return r;
        }
        case EXPR_ATTR: {
            if (e->is_ptr_take) {
                return cg_lvalue_ptr(e->attr.obj);
            }
            const char* an = e->attr.attr;
            if (e->attr.obj->ty->kind == TY_MODULE) {
                ModuleInfo* mm = e->attr.obj->ty->mod;
                VarSym* gv = find_global_in_mod(mm, an);
                if (gv) {
                    const char* mv = cg_reg();
                    cg_emit("%s = load %s, ptr %s", mv, llvm_mem_ty(gv->type), gv->llvm_name);
                    return cg_mem_to_reg(mv, gv->type);
                }
            }
            Type* raw_ot = e->attr.obj->ty;
            Type* ot = type_deref_all(raw_ot);
            if ((ot->kind == TY_STR || ot->kind == TY_BYTES) && strcmp(an, "bin") == 0) {
                const char* ov = cg_eval_recv(e->attr.obj);
                const char* sty = (ot->kind == TY_BYTES) ? "%StrictaBytes" : "%StrictaStr";
                const char* cp = cg_reg(); const char* r = cg_reg();
                cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", cp, sty, ov);
                cg_emit("%s = load ptr, ptr %s", r, cp);
                return r;
            }
            if (ot->kind == TY_CLASS) {
                ClassInfo* cls = ot->cls;
                FieldInfo* fi = find_field_in_class(cls, an);
                if (fi) {
                    if (!type_is_ptr(raw_ot) && cls->is_value_type) {
                        const char* ov = cg_expr(e->attr.obj, NULL);
                        const char* mv = cg_reg();
                        cg_emit("%s = extractvalue %s %s, %d", mv, cls->llvm_name, ov, fi->slot_index);
                        return cg_mem_to_reg(mv, fi->type);
                    }
                    const char* obj_p = cg_deref_to_class_ptr(e->attr.obj, NULL);
                    const char* fp = cg_reg(); const char* mv = cg_reg();
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, cls->llvm_name, obj_p, fi->slot_index);
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
                    const char* ov = cg_eval_recv(e->attr.obj);
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
                    const char* av = cg_eval_recv(args[0]);
                    Type* at = type_deref_all(args[0]->ty);
                    if (at->kind == TY_TUPLE) return arena_sprintf("%d", at->tuple_count);
                    const char* lp = cg_reg(); const char* l32 = cg_reg(); const char* r = cg_reg();
                    const char* sty = (at->kind == TY_STR) ? "%StrictaStr" : (at->kind == TY_BYTES) ? "%StrictaBytes" : arena_sprintf("%%%s", at->mangled);
                    int fld = (at->kind == TY_SET) ? 2 : (at->kind == TY_DICT) ? 3 : 0;
                    cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", lp, sty, av, fld);
                    cg_emit("%s = load i32, ptr %s", l32, lp);
                    cg_emit("%s = sext i32 %s to i64", r, l32);
                    return r;
                }
                if (e->is_ptr_take) {
                    const char* lp = cg_lvalue_ptr(e);
                    const char* mv = cg_reg();
                    cg_emit("%s = load %s, ptr %s", mv, llvm_mem_ty(ty), lp);
                    return cg_mem_to_reg(mv, ty);
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
                    const char* av = cg_expr(args[0], NULL);
                    if (args[0]->ty->kind == TY_STR) {
                        const char* r = cg_reg(); cg_emit("%s = call ptr @stricta_str_clone(ptr %s)", r, av); return r;
                    }
                    return cg_to_str(av, args[0]->ty);
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
                if (strcmp(cname, "unbin") == 0) {
                    const char* pv = cg_expr(args[0], NULL);
                    const char* r = cg_reg();
                    cg_emit("%s = call ptr @stricta_str_cstr(ptr %s)", r, pv);
                    return r;
                }
                if (strcmp(cname, "sys_argv") == 0) {
                    const char* r = cg_reg();
                    cg_emit("%s = call ptr @stricta_sys_get_argv()", r);
                    return r;
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
                        CompLeafCtx ctx = {args[0], acc_p, ty, NULL, false, 0};
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
                    if (args[0]->kind == EXPR_GENEXPR || args[0]->kind == EXPR_LISTCOMP) {
                        const char* res_p = cg_reg();
                        cg_entry("%s = alloca i1", res_p);
                        cg_emit("store i1 %s, ptr %s", is_all ? "true" : "false", res_p);
                        const char* l_exit = cg_lbl("aa_exit");
                        CompLeafCtx ctx = {args[0], res_p, g_ty_bool, l_exit, is_all, g_cg.temp_count};
                        cg_comp_loop_emit(args[0]->comp.gens, 0, args[0]->comp.gen_count, comp_leaf_any_all, &ctx);
                        cg_br(l_exit);
                        cg_label(l_exit);
                        const char* res = cg_reg();
                        cg_emit("%s = load i1, ptr %s", res, res_p);
                        return res;
                    }
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
                            const char* ep = cg_reg(); const char* ev0 = cg_reg();
                            cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, llvm_mem_ty(at->elem), arr, iv);
                            cg_emit("%s = load %s, ptr %s", ev0, llvm_mem_ty(at->elem), ep);
                            const char* ev = ev0;
                            if (type_is_owned(ty->elem)) {
                                emit_clone_for_type(ty->elem);
                                ev = cg_reg();
                                cg_emit("%s = call %s @_clone_%s(%s %s)", ev, llvm_mem_ty(ty->elem), ty->elem->mangled, llvm_mem_ty(ty->elem), ev0);
                            }
                            cg_emit("call void @_%s_append(ptr %s, %s %s)", ty->mangled, res, llvm_mem_ty(ty->elem), ev);
                            const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, iv); cg_emit("store i32 %s, ptr %s", in, i_p);
                            cg_br(ll); cg_label(ld);
                        }
                    }
                    if (strcmp(cname, "sorted") == 0 && argc == 2) {
                        const char* kf = cg_expr(args[1], NULL);
                        Type* kty = args[1]->ty->func_ret;
                        const char* sty = arena_sprintf("%%%s", ty->mangled);
                        const char* emty = llvm_mem_ty(ty->elem);
                        const char* kmty = llvm_mem_ty(kty);
                        const char* lp = cg_reg(); const char* len = cg_reg();
                        const char* ip = cg_reg(); const char* arr = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", lp, sty, res);
                        cg_emit("%s = load i32, ptr %s", len, lp);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", ip, sty, res);
                        cg_emit("%s = load ptr, ptr %s", arr, ip);
                        const char* len64 = cg_reg(); const char* ksz = cg_reg(); const char* karr = cg_reg();
                        cg_emit("%s = sext i32 %s to i64", len64, len);
                        cg_emit("%s = mul i64 %s, ptrtoint (ptr getelementptr (%s, ptr null, i32 1) to i64)", ksz, len64, kmty);
                        cg_emit("%s = call ptr @malloc(i64 %s)", karr, ksz);
                        const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p); cg_emit("store i32 0, ptr %s", i_p);
                        const char* lk1 = cg_lbl("sk_l"); const char* lb1 = cg_lbl("sk_b"); const char* ld1 = cg_lbl("sk_d");
                        cg_br(lk1); cg_label(lk1);
                        const char* iv = cg_reg(); const char* c1 = cg_reg();
                        cg_emit("%s = load i32, ptr %s", iv, i_p); cg_emit("%s = icmp slt i32 %s, %s", c1, iv, len);
                        cg_cond_br(c1, lb1, ld1); cg_label(lb1);
                        const char* ep = cg_reg(); const char* ev = cg_reg(); const char* kv = cg_reg(); const char* kp = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, emty, arr, iv);
                        cg_emit("%s = load %s, ptr %s", ev, emty, ep);
                        cg_emit("%s = call %s %s(%s %s)", kv, llvm_reg_ty(kty), kf, llvm_reg_ty(ty->elem), cg_mem_to_reg(ev, ty->elem));
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", kp, kmty, karr, iv);
                        cg_emit("store %s %s, ptr %s", kmty, cg_reg_to_mem(kv, kty), kp);
                        const char* in1 = cg_reg(); cg_emit("%s = add i32 %s, 1", in1, iv); cg_emit("store i32 %s, ptr %s", in1, i_p);
                        cg_br(lk1); cg_label(ld1);

                        /* Insertion sort on arr + karr by karr */
                        const char* j_p = cg_reg(); const char* key_p = cg_reg(); const char* val_p = cg_reg();
                        cg_entry("%s = alloca i32", j_p); cg_entry("%s = alloca %s", key_p, kmty); cg_entry("%s = alloca %s", val_p, emty);
                        cg_emit("store i32 1, ptr %s", i_p);
                        const char* lo = cg_lbl("so_l"); const char* lob = cg_lbl("so_b"); const char* lod = cg_lbl("so_d");
                        const char* li = cg_lbl("si_l"); const char* lic = cg_lbl("si_c"); const char* lib = cg_lbl("si_b"); const char* lid = cg_lbl("si_d");
                        cg_br(lo); cg_label(lo);
                        const char* io = cg_reg(); const char* co = cg_reg();
                        cg_emit("%s = load i32, ptr %s", io, i_p); cg_emit("%s = icmp slt i32 %s, %s", co, io, len);
                        cg_cond_br(co, lob, lod); cg_label(lob);
                        const char* skp = cg_reg(); const char* skv = cg_reg(); const char* svp = cg_reg(); const char* svv = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", skp, kmty, karr, io);
                        cg_emit("%s = load %s, ptr %s", skv, kmty, skp); cg_emit("store %s %s, ptr %s", kmty, skv, key_p);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", svp, emty, arr, io);
                        cg_emit("%s = load %s, ptr %s", svv, emty, svp); cg_emit("store %s %s, ptr %s", emty, svv, val_p);
                        const char* j0 = cg_reg(); cg_emit("%s = sub i32 %s, 1", j0, io); cg_emit("store i32 %s, ptr %s", j0, j_p);
                        cg_br(li); cg_label(li);
                        const char* jv = cg_reg(); const char* jge0 = cg_reg();
                        cg_emit("%s = load i32, ptr %s", jv, j_p); cg_emit("%s = icmp sge i32 %s, 0", jge0, jv);
                        cg_cond_br(jge0, lic, lid); cg_label(lic);
                        const char* kjp = cg_reg(); const char* kjv = cg_reg(); const char* kcur = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", kjp, kmty, karr, jv);
                        cg_emit("%s = load %s, ptr %s", kjv, kmty, kjp);
                        cg_emit("%s = load %s, ptr %s", kcur, kmty, key_p);
                        const char* gt = cg_binop_values(">", cg_mem_to_reg(kjv, kty), kty, cg_mem_to_reg(kcur, kty), kty, g_ty_bool);
                        cg_cond_br(gt, lib, lid); cg_label(lib);
                        const char* jp1 = cg_reg();
                        const char* vjp = cg_reg(); const char* vjv = cg_reg();
                        const char* kdp = cg_reg(); const char* vdp = cg_reg();
                        cg_emit("%s = add i32 %s, 1", jp1, jv);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", vjp, emty, arr, jv);
                        cg_emit("%s = load %s, ptr %s", vjv, emty, vjp);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", kdp, kmty, karr, jp1);
                        cg_emit("store %s %s, ptr %s", kmty, kjv, kdp);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", vdp, emty, arr, jp1);
                        cg_emit("store %s %s, ptr %s", emty, vjv, vdp);
                        const char* jn = cg_reg(); cg_emit("%s = sub i32 %s, 1", jn, jv); cg_emit("store i32 %s, ptr %s", jn, j_p);
                        cg_br(li); cg_label(lid);
                        const char* jf = cg_reg(); const char* jfp1 = cg_reg();
                        const char* fk = cg_reg(); const char* fv = cg_reg();
                        const char* fkp = cg_reg(); const char* fvp = cg_reg();
                        cg_emit("%s = load i32, ptr %s", jf, j_p); cg_emit("%s = add i32 %s, 1", jfp1, jf);
                        cg_emit("%s = load %s, ptr %s", fk, kmty, key_p); cg_emit("%s = load %s, ptr %s", fv, emty, val_p);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", fkp, kmty, karr, jfp1);
                        cg_emit("store %s %s, ptr %s", kmty, fk, fkp);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", fvp, emty, arr, jfp1);
                        cg_emit("store %s %s, ptr %s", emty, fv, fvp);
                        const char* ion = cg_reg(); cg_emit("%s = add i32 %s, 1", ion, io); cg_emit("store i32 %s, ptr %s", ion, i_p);
                        cg_br(lo); cg_label(lod);
                        cg_emit("call void @free(ptr %s)", karr);
                        return res;
                    }
                    cg_emit("call void @_%s_%s(ptr %s)", ty->mangled, strcmp(cname, "sorted") == 0 ? "sort" : "reverse", res);
                    return res;
                }
                if (strcmp(cname, "map") == 0 || strcmp(cname, "filter") == 0) {
                    bool is_map = (strcmp(cname, "map") == 0);
                    const char* fv = cg_expr(args[0], NULL);
                    const char* iv = cg_expr(args[1], NULL);
                    Type* it = args[1]->ty;
                    Type* el = it->elem;
                    const char* res = cg_reg();
                    cg_emit("%s = call ptr @_%s_new(i32 8)", res, ty->mangled);
                    if (it->kind == TY_LIST || it->kind == TY_SET) {
                        const char* sty = arena_sprintf("%%%s", it->mangled);
                        int lfld = (it->kind == TY_SET) ? 2 : 0;
                        int afld = (it->kind == TY_SET) ? 0 : 2;
                        const char* lp = cg_reg(); const char* len = cg_reg();
                        const char* ip = cg_reg(); const char* arr = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", lp, sty, iv, lfld);
                        cg_emit("%s = load i32, ptr %s", len, lp);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", ip, sty, iv, afld);
                        cg_emit("%s = load ptr, ptr %s", arr, ip);
                        const char* i_p = cg_reg(); cg_entry("%s = alloca i32", i_p); cg_emit("store i32 0, ptr %s", i_p);
                        const char* ll = cg_lbl("mf_l"); const char* lb = cg_lbl("mf_b"); const char* ld = cg_lbl("mf_d");
                        cg_br(ll); cg_label(ll);
                        const char* idx = cg_reg(); const char* c = cg_reg();
                        cg_emit("%s = load i32, ptr %s", idx, i_p); cg_emit("%s = icmp slt i32 %s, %s", c, idx, len);
                        cg_cond_br(c, lb, ld); cg_label(lb);
                        const char* ep = cg_reg(); const char* evm = llvm_mem_ty(el); const char* ev = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, evm, arr, idx);
                        cg_emit("%s = load %s, ptr %s", ev, evm, ep);
                        const char* evr = cg_mem_to_reg(ev, el);
                        Type* fr = args[0]->ty->func_ret;
                        const char* cv = cg_reg();
                        cg_emit("%s = call %s %s(%s %s)", cv, llvm_reg_ty(fr), fv, llvm_reg_ty(el), evr);
                        if (is_map) {
                            const char* mv = cg_coerce(cv, fr, ty->elem);
                            cg_emit("call void @_%s_append(ptr %s, %s %s)", ty->mangled, res, llvm_mem_ty(ty->elem), cg_reg_to_mem(mv, ty->elem));
                        } else {
                            const char* keep = cg_truthy(cv, fr);
                            const char* lk = cg_lbl("fl_k"); const char* ln = cg_lbl("fl_n");
                            cg_cond_br(keep, lk, ln); cg_label(lk);
                            const char* ev_add = ev;
                            if (type_is_owned(el)) {
                                emit_clone_for_type(el);
                                ev_add = cg_reg();
                                cg_emit("%s = call %s @_clone_%s(%s %s)", ev_add, evm, el->mangled, evm, ev);
                            }
                            cg_emit("call void @_%s_append(ptr %s, %s %s)", ty->mangled, res, evm, ev_add);
                            cg_br(ln); cg_label(ln);
                        }
                        const char* in = cg_reg(); cg_emit("%s = add i32 %s, 1", in, idx); cg_emit("store i32 %s, ptr %s", in, i_p);
                        cg_br(ll); cg_label(ld);
                    }
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
                        const char* ep = cg_reg(); const char* ev0 = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, llvm_mem_ty(ty->elem), arr, iv);
                        cg_emit("%s = load %s, ptr %s", ev0, llvm_mem_ty(ty->elem), ep);
                        const char* ev = ev0;
                        if (type_is_owned(ty->elem)) {
                            emit_clone_for_type(ty->elem);
                            ev = cg_reg();
                            cg_emit("%s = call %s @_clone_%s(%s %s)", ev, llvm_mem_ty(ty->elem), ty->elem->mangled, llvm_mem_ty(ty->elem), ev0);
                        }
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
                        const char* ep = cg_reg(); const char* ev0 = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 %s", ep, llvm_mem_ty(at->elem), arr, iv);
                        cg_emit("%s = load %s, ptr %s", ev0, llvm_mem_ty(at->elem), ep);
                        const char* ev = ev0;
                        if (type_is_owned(ty->elem)) {
                            emit_clone_for_type(ty->elem);
                            ev = cg_reg();
                            cg_emit("%s = call %s @_clone_%s(%s %s)", ev, llvm_mem_ty(ty->elem), ty->elem->mangled, llvm_mem_ty(ty->elem), ev0);
                        }
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
                            const char* fv0 = cg_reg();
                            cg_emit("%s = extractvalue %s %s, %d", fv0, llvm_reg_ty(at), av, k);
                            const char* fv = fv0;
                            if (type_is_owned(ty->elem)) {
                                emit_clone_for_type(ty->elem);
                                fv = cg_reg();
                                cg_emit("%s = call %s @_clone_%s(%s %s)", fv, llvm_mem_ty(ty->elem), ty->elem->mangled, llvm_mem_ty(ty->elem), fv0);
                            }
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
                    const char* defv = llvm_zero(lt->elem);
                    if (argc == 2) {
                        const char* raw_def = cg_expr(args[1], lt->elem);
                        if (type_is_owned(lt->elem)) {
                            if (args[1]->is_move || expr_produces_fresh_temp(args[1])) {
                                cg_consume_expr(args[1], raw_def);
                            } else {
                                emit_clone_for_type(lt->elem);
                                const char* dc = cg_reg();
                                cg_emit("%s = call %s @_clone_%s(%s %s)", dc, llvm_mem_ty(lt->elem), lt->elem->mangled,
                                        llvm_mem_ty(lt->elem), cg_reg_to_mem(cg_coerce(raw_def, args[1]->ty, lt->elem), lt->elem));
                                raw_def = cg_mem_to_reg(dc, lt->elem);
                            }
                        }
                        defv = cg_coerce(raw_def, args[1]->ty, lt->elem);
                    }
                    const char* lee = cg_lbl("nx_ee");
                    if (argc < 2) {
                        cg_emit("call void @stricta_set_exc(i32 7, ptr null, ptr %s)", cg_cstr_lit("StopIteration", 13));
                        cg_raise_branch();
                    } else {
                        cg_br(lee);
                    }
                    cg_label(lee); cg_br(ld);
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
                FuncInfo* target_fn = e->call.resolved_fn;
                if (!target_fn && g_cg.cur_fn) {
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
                        const char* raw_av = cg_expr(ae, pt);
                        if (target_fn->param_modes && target_fn->param_modes[i] == PM_MOVE) cg_consume_expr(ae, raw_av);
                        const char* av = cg_coerce(raw_av, ae->ty, pt);
                        if (arg_pos > 0 || i > 0) sb_append(&ca, ", ");
                        sb_printf(&ca, "%s %s", llvm_reg_ty(pt), av);
                    }
                    const char* call_sym = target_fn->llvm_name;
                    if (!target_fn->is_extern && target_fn->ast && target_fn->ast->def_s.deco_count > 0 && !target_fn->parent_fn) {
                        const char* fp = cg_reg();
                        cg_emit("%s = load ptr, ptr @_decfn_%s%s", fp, target_fn->mod->prefix, target_fn->name);
                        call_sym = fp;
                    }
                    const char* r = cg_emit_call_check(target_fn, call_sym, ca.data);
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

static const char* cg_expr(Expr* e, Type* hint) {
    const char* r = cg_expr_inner(e, hint);
    if (e && e->ty && type_is_owned(e->ty) && expr_produces_fresh_temp(e)) {
        cg_reg_temp(r, e->ty);
    }
    return r;
}

static bool stmts_escape_var_d(Stmt** stmts, int count, const char* vname, int depth);

static bool expr_escapes_var_d(Expr* e, const char* vname, bool allowed_pos, int depth) {
    if (!e) return false;
    switch (e->kind) {
        case EXPR_VAR:
            if (strcmp(e->var.name, vname) == 0 && !allowed_pos) return true;
            return false;
        case EXPR_INDEX:
            return expr_escapes_var_d(e->index.obj, vname, true, depth) || expr_escapes_var_d(e->index.index, vname, false, depth);
        case EXPR_CALL:
            if (e->call.callee->kind == EXPR_ATTR && e->call.callee->attr.obj->kind == EXPR_VAR &&
                strcmp(e->call.callee->attr.obj->var.name, vname) == 0) {
                const char* mn = e->call.callee->attr.attr;
                Expr* obj_e = e->call.callee->attr.obj;
                if (strcmp(mn, "append") == 0 || strcmp(mn, "clear") == 0 || strcmp(mn, "pop") == 0) {
                    for (int i = 0; i < e->call.arg_count; i++) {
                        if (expr_escapes_var_d(e->call.args[i], vname, false, depth)) return true;
                    }
                    return false;
                }
                if (depth < 3 && obj_e->ty && obj_e->ty->kind == TY_CLASS && !obj_e->ty->cls->has_subclasses) {
                    FuncInfo* m = find_method_in_class(obj_e->ty->cls, mn);
                    if (m && (m->is_async || (m->param_modes && m->param_modes[0] == PM_MOVE))) return true;
                    if (m && m->ast && m->param_count > 0 &&
                        !stmts_escape_var_d(m->ast->def_s.body, m->ast->def_s.body_count, m->param_names[0], depth + 1)) {
                        for (int i = 0; i < e->call.arg_count; i++) {
                            if (expr_escapes_var_d(e->call.args[i], vname, false, depth)) return true;
                        }
                        return false;
                    }
                }
            }
            if (e->call.callee->kind == EXPR_VAR && strcmp(e->call.callee->var.name, "len") == 0 && e->call.arg_count == 1) {
                return expr_escapes_var_d(e->call.args[0], vname, true, depth);
            }
            if (expr_escapes_var_d(e->call.callee, vname, false, depth)) return true;
            for (int i = 0; i < e->call.arg_count; i++) {
                if (expr_escapes_var_d(e->call.args[i], vname, false, depth)) return true;
            }
            return false;
        case EXPR_BINOP:
            return expr_escapes_var_d(e->binop.left, vname, false, depth) || expr_escapes_var_d(e->binop.right, vname, false, depth);
        case EXPR_UNOP:
            return expr_escapes_var_d(e->unop.operand, vname, false, depth);
        case EXPR_AWAIT:
            return expr_escapes_var_d(e->await_e.expr, vname, true, depth);
        case EXPR_TERNARY:
            return expr_escapes_var_d(e->ternary.cond, vname, false, depth) ||
                   expr_escapes_var_d(e->ternary.then_e, vname, false, depth) ||
                   expr_escapes_var_d(e->ternary.else_e, vname, false, depth);
        case EXPR_LIST: case EXPR_TUPLE: case EXPR_SET:
            for (int i = 0; i < e->seq.count; i++) if (expr_escapes_var_d(e->seq.items[i], vname, false, depth)) return true;
            return false;
        case EXPR_DICT:
            for (int i = 0; i < e->dict.count; i++) {
                if (expr_escapes_var_d(e->dict.keys[i], vname, false, depth) ||
                    expr_escapes_var_d(e->dict.vals[i], vname, false, depth)) return true;
            }
            return false;
        case EXPR_SLICE:
            return expr_escapes_var_d(e->slice.obj, vname, true, depth) ||
                   expr_escapes_var_d(e->slice.start, vname, false, depth) ||
                   expr_escapes_var_d(e->slice.stop, vname, false, depth) ||
                   expr_escapes_var_d(e->slice.step, vname, false, depth);
        case EXPR_LAMBDA: {
            for (int i = 0; i < e->lambda.param_count; i++) {
                if (strcmp(e->lambda.params[i], vname) == 0) return false;
            }
            return expr_escapes_var_d(e->lambda.body, vname, false, depth);
        }
        case EXPR_LISTCOMP: case EXPR_SETCOMP: case EXPR_DICTCOMP: case EXPR_GENEXPR:
            if (expr_escapes_var_d(e->comp.elt, vname, false, depth) ||
                expr_escapes_var_d(e->comp.val, vname, false, depth)) return true;
            for (int i = 0; i < e->comp.gen_count; i++) {
                if (expr_escapes_var_d(e->comp.gens[i].iter, vname, true, depth)) return true;
                for (int f = 0; f < e->comp.gens[i].if_count; f++) {
                    if (expr_escapes_var_d(e->comp.gens[i].ifs[f], vname, false, depth)) return true;
                }
            }
            return false;
        case EXPR_ATTR:
            if (e->is_ptr_take || expr_is_ptr_attr(e->attr.attr)) {
                return expr_escapes_var_d(e->attr.obj, vname, false, depth);
            }
            return expr_escapes_var_d(e->attr.obj, vname, true, depth);
        case EXPR_INT: case EXPR_FLOAT: case EXPR_BOOL: case EXPR_STR: case EXPR_NONE: case EXPR_BYTES:
            return false;
        case EXPR_FSTRING:
            for (int i = 0; i < e->fstr.count; i++) {
                if (e->fstr.parts[i].is_expr && expr_escapes_var_d(e->fstr.parts[i].expr, vname, false, depth)) return true;
            }
            return false;
        default:
            return true;
    }
}

static bool stmts_escape_var_d(Stmt** stmts, int count, const char* vname, int depth) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        switch (s->kind) {
            case STMT_RETURN:
                if (expr_escapes_var_d(s->ret.expr, vname, false, depth)) return true;
                break;
            case STMT_YIELD: case STMT_YIELD_FROM: case STMT_EXPR:
                if (expr_escapes_var_d(s->expr_s.expr, vname, false, depth)) return true;
                break;
            case STMT_ASSIGN:
                if (s->assign.target->kind == EXPR_VAR && strcmp(s->assign.target->var.name, vname) == 0) {
                    if (s->assign.value && ((s->assign.value->kind == EXPR_LIST && s->assign.value->seq.count <= 64) ||
                        s->assign.value->kind == EXPR_CALL)) {
                        for (int k = 0; s->assign.value->kind == EXPR_LIST && k < s->assign.value->seq.count; k++) {
                            if (expr_escapes_var_d(s->assign.value->seq.items[k], vname, false, depth)) return true;
                        }
                        if (s->assign.value->kind == EXPR_CALL) {
                            for (int k = 0; k < s->assign.value->call.arg_count; k++) {
                                if (expr_escapes_var_d(s->assign.value->call.args[k], vname, false, depth)) return true;
                            }
                        }
                        break;
                    }
                    return true;
                }
                if (((s->assign.target->kind == EXPR_INDEX || s->assign.target->kind == EXPR_ATTR) &&
                     expr_escapes_var_d(s->assign.target, vname, true, depth)) ||
                    expr_escapes_var_d(s->assign.value, vname, false, depth)) return true;
                break;
            case STMT_AUG_ASSIGN:
                if (expr_escapes_var_d(s->aug_assign.target, vname, false, depth) ||
                    expr_escapes_var_d(s->aug_assign.value, vname, false, depth)) return true;
                break;
            case STMT_IF:
                for (int b = 0; b < s->if_s.branch_count; b++) {
                    if (expr_escapes_var_d(s->if_s.conds[b], vname, false, depth)) return true;
                    if (stmts_escape_var_d(s->if_s.bodies[b], s->if_s.body_counts[b], vname, depth)) return true;
                }
                if (stmts_escape_var_d(s->if_s.else_body, s->if_s.else_count, vname, depth)) return true;
                break;
            case STMT_WHILE:
                if (expr_escapes_var_d(s->while_s.cond, vname, false, depth)) return true;
                if (stmts_escape_var_d(s->while_s.body, s->while_s.body_count, vname, depth)) return true;
                if (stmts_escape_var_d(s->while_s.else_body, s->while_s.else_count, vname, depth)) return true;
                break;
            case STMT_FOR:
                if (expr_escapes_var_d(s->for_s.iter, vname, true, depth)) return true;
                if (stmts_escape_var_d(s->for_s.body, s->for_s.body_count, vname, depth)) return true;
                if (stmts_escape_var_d(s->for_s.else_body, s->for_s.else_count, vname, depth)) return true;
                break;
            case STMT_ASSERT:
                if (expr_escapes_var_d(s->assert_s.cond, vname, false, depth) ||
                    expr_escapes_var_d(s->assert_s.msg, vname, false, depth)) return true;
                break;
            case STMT_DEF:
                return true;
            default:
                break;
        }
    }
    return false;
}

static bool stmts_escape_var(Stmt** stmts, int count, const char* vname) {
    return stmts_escape_var_d(stmts, count, vname, 0);
}

static int stmts_count_var_assigns(Stmt** stmts, int count, const char* vname) {
    int c = 0;
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        if (s->kind == STMT_ASSIGN && s->assign.target->kind == EXPR_VAR &&
            strcmp(s->assign.target->var.name, vname) == 0) c++;
        else if (s->kind == STMT_IF) {
            for (int b = 0; b < s->if_s.branch_count; b++) c += stmts_count_var_assigns(s->if_s.bodies[b], s->if_s.body_counts[b], vname);
            c += stmts_count_var_assigns(s->if_s.else_body, s->if_s.else_count, vname);
        } else if (s->kind == STMT_WHILE) {
            c += stmts_count_var_assigns(s->while_s.body, s->while_s.body_count, vname);
            c += stmts_count_var_assigns(s->while_s.else_body, s->while_s.else_count, vname);
        } else if (s->kind == STMT_FOR) {
            c += stmts_count_var_assigns(s->for_s.body, s->for_s.body_count, vname);
            c += stmts_count_var_assigns(s->for_s.else_body, s->for_s.else_count, vname);
        }
    }
    return c;
}

static void cg_stmts(Stmt** stmts, int count) {
    for (int i = 0; i < count; i++) {
        if (g_cg.block_terminated) break;
        Stmt* s = stmts[i];
        int stm_mark = g_cg.temp_count;
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
                    cg_flush_temps_preserve(lc->body_temp_mark);
                    if (lc->broke_alloca) cg_emit("store i1 true, ptr %s", lc->broke_alloca);
                    cg_br(lc->break_lbl);
                }
                break;
            case STMT_CONTINUE:
                if (g_cg.loop_depth > 0) {
                    LoopCtx* lc = &g_cg.loop_stack[g_cg.loop_depth - 1];
                    cg_flush_temps_preserve(lc->body_temp_mark);
                    cg_br(lc->cont_lbl);
                }
                break;
            case STMT_RETURN: {
                Type* rt = g_cg.cur_fn->ret_type;
                const char* rv = NULL;
                if (rt->kind != TY_VOID) {
                    const char* raw_rv = s->ret.expr ? cg_expr(s->ret.expr, rt) : llvm_zero(rt);
                    if (s->ret.expr) cg_consume_expr(s->ret.expr, raw_rv);
                    rv = s->ret.expr ? cg_coerce(raw_rv, s->ret.expr->ty, rt) : raw_rv;
                }
                cg_flush_temps_preserve(0);
                cg_drop_all_fn_locals(g_cg.cur_fn);
                if (g_cg.cur_fn->can_raise) {
                    if (rt->kind == TY_VOID) {
                        cg_emit("ret i1 false");
                    } else {
                        const char* rty = llvm_fn_ret_ty(g_cg.cur_fn);
                        const char* pkt = cg_reg();
                        cg_emit("%s = insertvalue %s { i1 false, %s undef }, %s %s, 1",
                                pkt, rty, llvm_reg_ty(rt), llvm_reg_ty(rt), rv);
                        cg_emit("ret %s %s", rty, pkt);
                    }
                } else {
                    if (rt->kind == TY_VOID) cg_emit("ret void");
                    else cg_emit("ret %s %s", llvm_reg_ty(rt), rv);
                }
                g_cg.block_terminated = true;
                break;
            }
            case STMT_YIELD: {
                Type* el = g_cg.cur_fn->ret_type->elem;
                const char* raw_yv = cg_expr(s->expr_s.expr, el);
                cg_consume_expr(s->expr_s.expr, raw_yv);
                const char* yv = cg_coerce(raw_yv, s->expr_s.expr->ty, el);
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
                    VarSym* vs = cg_find_var_sym(tgt->var.name);
                    if (g_cg.cur_fn && g_cg.cur_fn->ast && s->assign.value->kind == EXPR_LIST &&
                        s->assign.value->seq.count <= 64 && vt->kind == TY_LIST &&
                        !stmts_escape_var(g_cg.cur_fn->ast->def_s.body, g_cg.cur_fn->ast->def_s.body_count, tgt->var.name) &&
                        stmts_count_var_assigns(g_cg.cur_fn->ast->def_s.body, g_cg.cur_fn->ast->def_s.body_count, tgt->var.name) == 1) {
                        const char* sty = arena_sprintf("%%%s", vt->mangled);
                        const char* mty = llvm_mem_ty(vt->elem);
                        const char* sl = cg_reg(); const char* sb = cg_reg();
                        cg_entry("%s = alloca %s", sl, sty);
                        cg_entry("%s = alloca [64 x %s]", sb, mty);
                        int n = s->assign.value->seq.count;
                        for (int k = 0; k < n; k++) {
                            const char* raw_iv = cg_expr(s->assign.value->seq.items[k], vt->elem);
                            cg_consume_expr(s->assign.value->seq.items[k], raw_iv);
                            const char* iv = cg_coerce(raw_iv, s->assign.value->seq.items[k]->ty, vt->elem);
                            const char* sp = cg_reg();
                            cg_emit("%s = getelementptr [64 x %s], ptr %s, i32 0, i32 %d", sp, mty, sb, k);
                            cg_emit("store %s %s, ptr %s", mty, cg_reg_to_mem(iv, vt->elem), sp);
                        }
                        const char* p0 = cg_reg(); const char* p1 = cg_reg(); const char* p2 = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 0", p0, sty, sl);
                        cg_emit("store i32 %d, ptr %s", n, p0);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 1", p1, sty, sl);
                        cg_emit("store i32 -64, ptr %s", p1);
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 2", p2, sty, sl);
                        cg_emit("store ptr %s, ptr %s", sb, p2);
                        cg_emit("store ptr %s, ptr %s", sl, vp);
                        if (vs) {
                            vs->is_stack_owned = true;
                            if (vs->drop_flag_llvm) cg_emit("store i1 true, ptr %s", vs->drop_flag_llvm);
                        }
                        break;
                    }
                    if (g_cg.cur_fn && g_cg.cur_fn->ast &&
                        (s->assign.value->kind == EXPR_LISTCOMP || s->assign.value->kind == EXPR_GENEXPR) &&
                        vt->kind == TY_LIST &&
                        !stmts_escape_var(g_cg.cur_fn->ast->def_s.body, g_cg.cur_fn->ast->def_s.body_count, tgt->var.name) &&
                        stmts_count_var_assigns(g_cg.cur_fn->ast->def_s.body, g_cg.cur_fn->ast->def_s.body_count, tgt->var.name) == 1) {
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
                        CompLeafCtx ctx = {s->assign.value, sl, vt, NULL, false, 0};
                        cg_comp_loop_emit(s->assign.value->comp.gens, 0, s->assign.value->comp.gen_count, comp_leaf_list, &ctx);
                        cg_emit("store ptr %s, ptr %s", sl, vp);
                        if (vs) {
                            vs->is_stack_owned = true;
                            if (vs->drop_flag_llvm) cg_emit("store i1 true, ptr %s", vs->drop_flag_llvm);
                        }
                        break;
                    }
                    if (g_cg.cur_fn && g_cg.cur_fn->ast && s->assign.value->kind == EXPR_CALL &&
                        s->assign.value->call.callee->kind == EXPR_VAR && vt->kind == TY_CLASS &&
                        !vt->cls->is_value_type && !vt->cls->has_unimpl_abstract &&
                        find_class_in_mod(g_cg.cur_mod, s->assign.value->call.callee->var.name) == vt->cls &&
                        !stmts_escape_var(g_cg.cur_fn->ast->def_s.body, g_cg.cur_fn->ast->def_s.body_count, tgt->var.name) &&
                        stmts_count_var_assigns(g_cg.cur_fn->ast->def_s.body, g_cg.cur_fn->ast->def_s.body_count, tgt->var.name) == 1) {
                        FuncInfo* init_fn = find_method_in_class(vt->cls, "__init__");
                        bool init_safe = (!init_fn || (init_fn->ast && init_fn->param_count > 0 &&
                                           !stmts_escape_var_d(init_fn->ast->def_s.body, init_fn->ast->def_s.body_count, init_fn->param_names[0], 1)));
                        if (init_safe) {
                            const char* stk_obj = cg_reg();
                            cg_entry("%s = alloca %s", stk_obj, vt->cls->llvm_name);
                            const char* obj_v = cg_construct_class_at(vt->cls, s->assign.value->call.args, s->assign.value->call.arg_count, stk_obj);
                            cg_emit("store ptr %s, ptr %s", obj_v, vp);
                            if (vs) {
                                vs->is_stack_owned = true;
                                if (vs->drop_flag_llvm) cg_emit("store i1 true, ptr %s", vs->drop_flag_llvm);
                            }
                            break;
                        }
                    }
                    const char* raw_v = cg_expr(s->assign.value, vt);
                    bool is_borrow = g_cg.cur_fn && expr_is_borrow_source(s->assign.value) && type_is_owned(vt);
                    if (!is_borrow) cg_consume_expr(s->assign.value, raw_v);
                    if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
                    const char* v = cg_coerce(raw_v, s->assign.value->ty, vt);
                    cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(v, vt), vp);
                    if (vs && vs->drop_flag_llvm) {
                        cg_emit("store i1 %s, ptr %s", is_borrow ? "false" : "true", vs->drop_flag_llvm);
                    }
                } else if (tgt->kind == EXPR_CALL && tgt->is_ptr_take) {
                    const char* dst_p = cg_lvalue_ptr(tgt);
                    Type* tt = tgt->ty;
                    const char* raw_v = cg_expr(s->assign.value, tt);
                    cg_consume_expr(s->assign.value, raw_v);
                    const char* v = cg_coerce(raw_v, s->assign.value->ty, tt);
                    if (type_is_owned(tt)) {
                        emit_drop_for_type(tt);
                        const char* old_v = cg_reg();
                        cg_emit("%s = load %s, ptr %s", old_v, llvm_mem_ty(tt), dst_p);
                        cg_emit("call void @_drop_%s(%s %s)", tt->mangled, llvm_mem_ty(tt), old_v);
                    }
                    cg_emit("store %s %s, ptr %s", llvm_mem_ty(tt), cg_reg_to_mem(v, tt), dst_p);
                } else if (tgt->kind == EXPR_ATTR) {
                    Type* ot = type_deref_all(tgt->attr.obj->ty);
                    FieldInfo* fi = find_field_in_class(ot->cls, tgt->attr.attr);
                    if (fi) {
                        const char* obj_p = cg_deref_to_class_ptr(tgt->attr.obj, NULL);
                        const char* raw_v = cg_expr(s->assign.value, fi->type);
                        cg_consume_expr(s->assign.value, raw_v);
                        const char* v = cg_coerce(raw_v, s->assign.value->ty, fi->type);
                        const char* fp = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, ot->cls->llvm_name, obj_p, fi->slot_index);
                        if (type_is_owned(fi->type)) {
                            emit_drop_for_type(fi->type);
                            const char* old_v = cg_reg();
                            cg_emit("%s = load %s, ptr %s", old_v, llvm_mem_ty(fi->type), fp);
                            cg_emit("call void @_drop_%s(%s %s)", fi->type->mangled, llvm_mem_ty(fi->type), old_v);
                        }
                        cg_emit("store %s %s, ptr %s", llvm_mem_ty(fi->type), cg_reg_to_mem(v, fi->type), fp);
                    } else {
                        FieldInfo* ca = find_class_attr(ot->cls, tgt->attr.attr);
                        ClassInfo* owner = ot->cls;
                        for (int m = 0; m < ot->cls->mro_count; m++) {
                            for (int k = 0; k < ot->cls->mro[m]->class_attr_count; k++) {
                                if (strcmp(ot->cls->mro[m]->class_attrs[k].name, tgt->attr.attr) == 0) { owner = ot->cls->mro[m]; goto fca_s; }
                            }
                        }
                        fca_s:;
                        const char* raw_v = cg_expr(s->assign.value, ca->type);
                        cg_consume_expr(s->assign.value, raw_v);
                        const char* v = cg_coerce(raw_v, s->assign.value->ty, ca->type);
                        cg_emit("store %s %s, ptr @_cattr_%s%s__%s", llvm_mem_ty(ca->type), cg_reg_to_mem(v, ca->type),
                                owner->mod->prefix, owner->name, tgt->attr.attr);
                    }
                } else if (tgt->kind == EXPR_INDEX) {
                    Type* ptr_ot = tgt->index.obj->ty;
                    while (ptr_ot && (ptr_ot->kind == TY_REF || ptr_ot->kind == TY_MUT_REF)) ptr_ot = ptr_ot->elem;
                    if (ptr_ot && ptr_ot->kind == TY_ADDR) {
                        const char* pv = cg_expr(tgt->index.obj, NULL);
                        Type* rt = tgt->index.obj->ty;
                        while (rt && (rt->kind == TY_REF || rt->kind == TY_MUT_REF)) {
                            const char* np = cg_reg();
                            cg_emit("%s = load ptr, ptr %s", np, pv);
                            pv = np;
                            rt = rt->elem;
                        }
                        const char* iv = cg_coerce(cg_expr(tgt->index.index, g_ty_i64), tgt->index.index->ty, g_ty_i64);
                        const char* raw_vv = cg_expr(s->assign.value, ptr_ot->elem);
                        cg_consume_expr(s->assign.value, raw_vv);
                        const char* vv = cg_coerce(raw_vv, s->assign.value->ty, ptr_ot->elem);
                        const char* slot = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i64 %s", slot, llvm_mem_ty(ptr_ot->elem), pv, iv);
                        cg_emit("store %s %s, ptr %s", llvm_mem_ty(ptr_ot->elem), cg_reg_to_mem(vv, ptr_ot->elem), slot);
                        break;
                    }
                    const char* ov = cg_eval_recv(tgt->index.obj);
                    Type* ot = type_deref_all(tgt->index.obj->ty);
                    if (ot->kind == TY_LIST) {
                        const char* iv = cg_coerce(cg_expr(tgt->index.index, g_ty_i64), tgt->index.index->ty, g_ty_i64);
                        const char* raw_vv = cg_expr(s->assign.value, ot->elem);
                        cg_consume_expr(s->assign.value, raw_vv);
                        const char* vv = cg_coerce(raw_vv, s->assign.value->ty, ot->elem);
                        if (tgt->is_inbounds) {
                            const char* ip = cg_reg(); const char* arr = cg_reg(); const char* slot = cg_reg();
                            cg_emit("%s = getelementptr %%%s, ptr %s, i32 0, i32 2", ip, ot->mangled, ov);
                            cg_emit("%s = load ptr, ptr %s", arr, ip);
                            cg_emit("%s = getelementptr %s, ptr %s, i64 %s", slot, llvm_mem_ty(ot->elem), arr, iv);
                            if (type_is_owned(ot->elem)) {
                                emit_drop_for_type(ot->elem);
                                const char* old_e = cg_reg();
                                cg_emit("%s = load %s, ptr %s", old_e, llvm_mem_ty(ot->elem), slot);
                                cg_emit("call void @_drop_%s(%s %s)", ot->elem->mangled, llvm_mem_ty(ot->elem), old_e);
                            }
                            cg_emit("store %s %s, ptr %s", llvm_mem_ty(ot->elem), cg_reg_to_mem(vv, ot->elem), slot);
                        } else {
                            cg_emit("call void @_%s_%s(ptr %s, i64 %s, %s %s)", ot->mangled,
                                    tgt->index.index->is_nonneg ? "set_nn" : "set", ov, iv, llvm_mem_ty(ot->elem), cg_reg_to_mem(vv, ot->elem));
                        }
                    } else if (ot->kind == TY_DICT) {
                        const char* raw_kv = cg_expr(tgt->index.index, ot->key);
                        cg_consume_expr(tgt->index.index, raw_kv);
                        const char* raw_vv = cg_expr(s->assign.value, ot->val);
                        cg_consume_expr(s->assign.value, raw_vv);
                        const char* kv = cg_coerce(raw_kv, tgt->index.index->ty, ot->key);
                        const char* vv = cg_coerce(raw_vv, s->assign.value->ty, ot->val);
                        cg_emit("call void @_%s_set(ptr %s, %s %s, %s %s)", ot->mangled, ov,
                                llvm_mem_ty(ot->key), cg_reg_to_mem(kv, ot->key),
                                llvm_mem_ty(ot->val), cg_reg_to_mem(vv, ot->val));
                    }
                }
                break;
            }
            case STMT_MULTI_ASSIGN: {
                const char* tv = cg_expr(s->multi_assign.value, NULL);
                bool owned_src = s->multi_assign.value->is_move || expr_produces_fresh_temp(s->multi_assign.value);
                if (owned_src) cg_consume_expr(s->multi_assign.value, tv);
                Type* tt = s->multi_assign.value->ty;
                for (int k = 0; k < s->multi_assign.target_count; k++) {
                    Type* vt = NULL;
                    const char* vp = cg_var_ptr(s->multi_assign.targets[k]->var.name, &vt);
                    VarSym* vs = cg_find_var_sym(s->multi_assign.targets[k]->var.name);
                    if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
                    const char* fm = cg_reg();
                    cg_emit("%s = extractvalue %s %s, %d", fm, llvm_reg_ty(tt), tv, k);
                    if (!owned_src && type_is_owned(vt) && !g_cg.cur_fn) {
                        emit_clone_for_type(vt);
                        const char* fc = cg_reg();
                        cg_emit("%s = call %s @_clone_%s(%s %s)", fc, llvm_mem_ty(vt), vt->mangled, llvm_mem_ty(vt), fm);
                        fm = fc;
                    }
                    cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), fm, vp);
                    if (vs && vs->drop_flag_llvm) {
                        bool take = owned_src || !g_cg.cur_fn;
                        cg_emit("store i1 %s, ptr %s", take ? "true" : "false", vs->drop_flag_llvm);
                    }
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
                    VarSym* vs = cg_find_var_sym(tgt->var.name);
                    const char* cur = cg_mem_to_reg(({ const char* r = cg_reg(); cg_emit("%s = load %s, ptr %s", r, llvm_mem_ty(vt), vp); r; }), vt);
                    const char* rv = cg_expr(s->aug_assign.value, vt);
                    if (vt->kind == TY_SET) {
                        const char* fnm = strcmp(bop, "|") == 0 ? "update" : strcmp(bop, "&") == 0 ? "intersection_update" :
                                          strcmp(bop, "-") == 0 ? "difference_update" : "symmetric_difference_update";
                        cg_emit("call void @_%s_%s(ptr %s, ptr %s)", vt->mangled, fnm, cur, rv);
                    } else {
                        const char* res = cg_binop_values(bop, cur, vt, rv, s->aug_assign.value->ty, vt);
                        if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
                        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(cg_coerce(res, vt, vt), vt), vp);
                        if (vs && vs->drop_flag_llvm) cg_emit("store i1 true, ptr %s", vs->drop_flag_llvm);
                    }
                } else if (tgt->kind == EXPR_CALL && tgt->is_ptr_take) {
                    const char* vp = cg_lvalue_ptr(tgt);
                    Type* vt = tgt->ty;
                    const char* cur_m = cg_reg();
                    cg_emit("%s = load %s, ptr %s", cur_m, llvm_mem_ty(vt), vp);
                    const char* cur = cg_mem_to_reg(cur_m, vt);
                    const char* rv = cg_expr(s->aug_assign.value, vt);
                    if (vt->kind == TY_SET) {
                        const char* fnm = strcmp(bop, "|") == 0 ? "update" : strcmp(bop, "&") == 0 ? "intersection_update" :
                                          strcmp(bop, "-") == 0 ? "difference_update" : "symmetric_difference_update";
                        cg_emit("call void @_%s_%s(ptr %s, ptr %s)", vt->mangled, fnm, cur, rv);
                    } else {
                        const char* res = cg_binop_values(bop, cur, vt, rv, s->aug_assign.value->ty, vt);
                        if (type_is_owned(vt)) {
                            emit_drop_for_type(vt);
                            cg_emit("call void @_drop_%s(%s %s)", vt->mangled, llvm_mem_ty(vt), cur_m);
                        }
                        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(cg_coerce(res, vt, vt), vt), vp);
                    }
                } else if (tgt->kind == EXPR_ATTR) {
                    Type* ot = type_deref_all(tgt->attr.obj->ty);
                    FieldInfo* fi = find_field_in_class(ot->cls, tgt->attr.attr);
                    if (fi) {
                        const char* obj_p = cg_deref_to_class_ptr(tgt->attr.obj, NULL);
                        const char* fp = cg_reg();
                        cg_emit("%s = getelementptr %s, ptr %s, i32 0, i32 %d", fp, ot->cls->llvm_name, obj_p, fi->slot_index);
                        const char* cur_m = cg_reg();
                        cg_emit("%s = load %s, ptr %s", cur_m, llvm_mem_ty(fi->type), fp);
                        const char* cur = cg_mem_to_reg(cur_m, fi->type);
                        const char* rv = cg_expr(s->aug_assign.value, fi->type);
                        const char* res = cg_binop_values(bop, cur, fi->type, rv, s->aug_assign.value->ty, fi->type);
                        if (type_is_owned(fi->type)) {
                            emit_drop_for_type(fi->type);
                            cg_emit("call void @_drop_%s(%s %s)", fi->type->mangled, llvm_mem_ty(fi->type), cur_m);
                        }
                        cg_emit("store %s %s, ptr %s", llvm_mem_ty(fi->type), cg_reg_to_mem(cg_coerce(res, fi->type, fi->type), fi->type), fp);
                    }
                } else if (tgt->kind == EXPR_INDEX) {
                    const char* ov = cg_eval_recv(tgt->index.obj);
                    Type* ot = type_deref_all(tgt->index.obj->ty);
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
                    int cm = g_cg.temp_count;
                    const char* cv = cg_truthy(cg_expr(s->if_s.conds[b], g_ty_bool), s->if_s.conds[b]->ty);
                    cg_flush_temps(cm);
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
                int cm = g_cg.temp_count;
                const char* cv = cg_truthy(cg_expr(s->while_s.cond, g_ty_bool), s->while_s.cond->ty);
                cg_flush_temps(cm);
                cg_cond_br(cv, l_body, l_else ? l_else : l_end);
                cg_label(l_body);
                g_cg.loop_stack[g_cg.loop_depth++] = (LoopCtx){l_end, l_cond, NULL, NULL, g_cg.temp_count};
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
                if (s->raise_s.exc) {
                    const char* raw_ev = cg_expr(s->raise_s.exc, NULL);
                    Type* et = s->raise_s.exc->ty;
                    if (et->kind == TY_CLASS) {
                        cg_consume_expr(s->raise_s.exc, raw_ev);
                        cg_emit("call void @stricta_set_exc(i32 %d, ptr %s, ptr null)", et->cls->type_id, raw_ev);
                    } else {
                        const char* es = cg_to_str(raw_ev, et);
                        const char* sp = cg_reg(); const char* cp = cg_reg();
                        cg_emit("%s = getelementptr %%StrictaStr, ptr %s, i32 0, i32 2", sp, es);
                        cg_emit("%s = load ptr, ptr %s", cp, sp);
                        cg_emit("call void @stricta_set_exc(i32 1, ptr null, ptr %s)", cp);
                        if (et->kind != TY_STR) cg_emit("call void @stricta_str_free(ptr %s)", es);
                    }
                }
                cg_raise_branch();
                break;
            }
            case STMT_TRY: {
                const char* l_catch = (s->try_s.handler_count > 0) ? cg_lbl("try_c") : NULL;
                const char* l_fin_ok = (s->try_s.finally_count > 0) ? cg_lbl("try_fok") : NULL;
                const char* l_fin_exc = (s->try_s.finally_count > 0) ? cg_lbl("try_fex") : NULL;
                const char* l_end = cg_lbl("try_end");
                const char* prev_unwind = g_cg.cur_unwind_lbl;
                const char* post_try_target = l_fin_ok ? l_fin_ok : l_end;
                const char* post_exc_unwind = l_fin_exc ? l_fin_exc : prev_unwind;

                g_cg.cur_unwind_lbl = l_catch ? l_catch : post_exc_unwind;
                cg_stmts(s->try_s.try_body, s->try_s.try_count);
                if (!g_cg.block_terminated && s->try_s.else_count > 0) {
                    g_cg.cur_unwind_lbl = post_exc_unwind;
                    cg_stmts(s->try_s.else_body, s->try_s.else_count);
                }
                cg_br(post_try_target);

                if (l_catch) {
                    cg_label(l_catch);
                    g_cg.cur_unwind_lbl = post_exc_unwind;
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
                            VarSym* vs = cg_find_var_sym(eh->as_name);
                            if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
                            const char* ev = cg_reg();
                            if (vt->kind == TY_CLASS) cg_emit("%s = call ptr @stricta_exc_obj()", ev);
                            else cg_emit("%s = call ptr @stricta_exc_msg()", ev);
                            cg_emit("store ptr %s, ptr %s", ev, vp);
                            if (vs && vs->drop_flag_llvm) cg_emit("store i1 false, ptr %s", vs->drop_flag_llvm);
                        }
                        cg_stmts(eh->body, eh->body_count);
                        if (!g_cg.block_terminated) {
                            ClassInfo* eci = eh->exc_type ? find_class_in_mod(g_cg.cur_mod, eh->exc_type) : NULL;
                            if (eci && type_is_owned(eci->type)) {
                                emit_drop_for_type(eci->type);
                                const char* eobj = cg_reg();
                                cg_emit("%s = call ptr @stricta_exc_obj()", eobj);
                                cg_emit("call void @_drop_%s(ptr %s)", eci->type->mangled, eobj);
                                cg_emit("call void @stricta_clear_exc(i32 0)");
                            } else {
                                cg_emit("call void @stricta_clear_exc(i32 1)");
                            }
                        }
                        cg_br(post_try_target);
                        cg_label(l_nxt);
                        if (!eh->exc_type) break;
                    }
                    g_cg.cur_unwind_lbl = post_exc_unwind;
                    cg_raise_branch();
                }

                if (l_fin_ok) {
                    cg_label(l_fin_ok);
                    g_cg.cur_unwind_lbl = prev_unwind;
                    cg_stmts(s->try_s.finally_body, s->try_s.finally_count);
                    cg_br(l_end);
                }
                if (l_fin_exc) {
                    cg_label(l_fin_exc);
                    g_cg.cur_unwind_lbl = prev_unwind;
                    cg_stmts(s->try_s.finally_body, s->try_s.finally_count);
                    cg_raise_branch();
                }
                cg_label(l_end);
                g_cg.cur_unwind_lbl = prev_unwind;
                break;
            }
            case STMT_WITH: {
                const char* cv = cg_expr(s->with_s.ctx, NULL);
                Type* ct = s->with_s.ctx->ty;
                const char* bound = cv;
                if (ct->kind == TY_CLASS) {
                    FuncInfo* ent = find_method_in_class(ct->cls, "__enter__");
                    StrBuf eca; sb_init(&eca);
                    sb_printf(&eca, "%s %s", llvm_reg_ty(ct), cv);
                    bound = cg_emit_call_check(ent, ent->llvm_name, eca.data);
                    free(eca.data);
                    if (s->with_s.as_name) {
                        Type* vt = NULL; const char* vp = cg_var_ptr(s->with_s.as_name, &vt);
                        VarSym* vs = cg_find_var_sym(s->with_s.as_name);
                        if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
                        cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(bound, vt), vp);
                        if (vs && vs->drop_flag_llvm) cg_emit("store i1 false, ptr %s", vs->drop_flag_llvm);
                    }
                } else if (ct->kind == TY_FILE && s->with_s.as_name) {
                    Type* vt = NULL; const char* vp = cg_var_ptr(s->with_s.as_name, &vt);
                    VarSym* vs = cg_find_var_sym(s->with_s.as_name);
                    if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
                    cg_emit("store ptr %s, ptr %s", bound, vp);
                    if (vs && vs->drop_flag_llvm) cg_emit("store i1 false, ptr %s", vs->drop_flag_llvm);
                }
                const char* l_ex = cg_lbl("w_ex");
                const char* l_end = cg_lbl("w_end");
                const char* prev_unwind = g_cg.cur_unwind_lbl;
                g_cg.cur_unwind_lbl = l_ex;
                cg_stmts(s->with_s.body, s->with_s.body_count);
                g_cg.cur_unwind_lbl = prev_unwind;
                if (!g_cg.block_terminated) {
                    if (ct->kind == TY_FILE) {
                        cg_emit("call void @stricta_file_close(ptr %s)", cv);
                    } else if (ct->kind == TY_CLASS) {
                        FuncInfo* ex = find_method_in_class(ct->cls, "__exit__");
                        if (ex) {
                            StrBuf exa; sb_init(&exa);
                            sb_printf(&exa, "%s %s, %s %s, %s %s, %s %s",
                                      llvm_reg_ty(ct), cv,
                                      llvm_reg_ty(ex->param_types[1]), llvm_zero(ex->param_types[1]),
                                      llvm_reg_ty(ex->param_types[2]), llvm_zero(ex->param_types[2]),
                                      llvm_reg_ty(ex->param_types[3]), llvm_zero(ex->param_types[3]));
                            cg_emit_call_check(ex, ex->llvm_name, exa.data);
                            free(exa.data);
                        }
                    }
                    cg_br(l_end);
                }
                cg_label(l_ex);
                g_cg.cur_unwind_lbl = prev_unwind;
                if (ct->kind == TY_FILE) {
                    cg_emit("call void @stricta_file_close(ptr %s)", cv);
                    cg_raise_branch();
                } else if (ct->kind == TY_CLASS) {
                    FuncInfo* ex = find_method_in_class(ct->cls, "__exit__");
                    if (ex) {
                        StrBuf exa; sb_init(&exa);
                        sb_printf(&exa, "%s %s, %s %s, %s %s, %s %s",
                                  llvm_reg_ty(ct), cv,
                                  llvm_reg_ty(ex->param_types[1]), llvm_zero(ex->param_types[1]),
                                  llvm_reg_ty(ex->param_types[2]), llvm_zero(ex->param_types[2]),
                                  llvm_reg_ty(ex->param_types[3]), llvm_zero(ex->param_types[3]));
                        const char* sup = cg_emit_call_check(ex, ex->llvm_name, exa.data);
                        free(exa.data);
                        const char* l_rr = cg_lbl("w_rr");
                        cg_cond_br(sup, l_end, l_rr);
                        cg_label(l_rr);
                    }
                    cg_raise_branch();
                }
                cg_label(l_end);
                g_cg.cur_unwind_lbl = prev_unwind;
                break;
            }
            case STMT_DEL: {
                Expr* dt = s->del_s.target;
                if (dt->kind == EXPR_VAR) {
                    VarSym* vs = cg_find_var_sym(dt->var.name);
                    if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
                } else if (dt->kind == EXPR_INDEX) {
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
                            VarSym* vs = cg_find_var_sym(mc->capture);
                            if (vs && vs->drop_flag_llvm) cg_drop_var_if_alive(vs);
                            const char* cv = cg_coerce(sv, st, vt);
                            cg_emit("store %s %s, ptr %s", llvm_mem_ty(vt), cg_reg_to_mem(cv, vt), vp);
                            if (vs && vs->drop_flag_llvm) cg_emit("store i1 false, ptr %s", vs->drop_flag_llvm);
                        }
                        cg_br(l_b);
                    } else {
                        int pm = g_cg.temp_count;
                        const char* pv = cg_expr(mc->pattern, st);
                        const char* eq = cg_binop_values("==", sv, st, pv, mc->pattern->ty, g_ty_bool);
                        cg_flush_temps(pm);
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
        cg_flush_temps(stm_mark);
    }
}

static void emit_async_wrappers(FuncInfo* fn);

static void emit_func_ir(FuncInfo* fn) {
    g_cg.cur_fn = fn;
    g_cg.cur_mod = fn->mod;
    g_cg.in_func = true;
    g_cg.block_terminated = false;
    g_cg.reg_id = 0;
    g_cg.fn_alloca_count = 0;
    g_cg.temp_count = 0;
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
    }
    g_cg.fn_unwind_lbl = fn->can_raise ? cg_lbl("fn_unwind") : NULL;
    g_cg.cur_unwind_lbl = g_cg.fn_unwind_lbl;
    for (int i = 0; i < fn->local_count; i++) {
        VarSym* lv = &fn->locals[i];
        cg_ensure_local_alloca(lv->llvm_name, lv->type);
        lv->is_stack_owned = false;
        if (type_is_owned(lv->type)) {
            lv->drop_flag_llvm = arena_sprintf("%s_alive", lv->llvm_name);
            cg_entry("%s = alloca i1", lv->drop_flag_llvm);
            cg_entry("store i1 false, ptr %s", lv->drop_flag_llvm);
        } else {
            lv->drop_flag_llvm = NULL;
        }
    }
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
            if (lv->drop_flag_llvm && fn->param_modes && fn->param_modes[i] == PM_MOVE) {
                cg_entry("store i1 true, ptr %s", lv->drop_flag_llvm);
            }
        }
    }

    if (fn->is_generator) {
        g_cg.yield_list_reg = "%__yield_list";
        cg_emit("%s = call ptr @_%s_new(i32 8)", g_cg.yield_list_reg, fn->ret_type->mangled);
    }

    if (fn->is_lambda) {
        Expr* lb = fn->lambda_ast->lambda.body;
        const char* raw_rv = cg_expr(lb, fn->ret_type);
        cg_consume_expr(lb, raw_rv);
        const char* rv = cg_coerce(raw_rv, lb->ty, fn->ret_type);
        cg_flush_temps_preserve(0);
        cg_drop_all_fn_locals(fn);
        if (fn->can_raise) {
            if (fn->ret_type->kind == TY_VOID) cg_emit("ret i1 false");
            else {
                const char* rty = llvm_fn_ret_ty(fn);
                const char* pkt = cg_reg();
                cg_emit("%s = insertvalue %s { i1 false, %s undef }, %s %s, 1",
                        pkt, rty, llvm_reg_ty(fn->ret_type), llvm_reg_ty(fn->ret_type), rv);
                cg_emit("ret %s %s", rty, pkt);
            }
        } else {
            if (fn->ret_type->kind == TY_VOID) cg_emit("ret void");
            else cg_emit("ret %s %s", llvm_reg_ty(fn->ret_type), rv);
        }
        g_cg.block_terminated = true;
    } else if (fn->ast) {
        cg_stmts(fn->ast->def_s.body, fn->ast->def_s.body_count);
    }

    if (!g_cg.block_terminated) {
        cg_flush_temps(0);
        cg_drop_all_fn_locals(fn);
        if (fn->can_raise) {
            if (fn->is_generator) {
                const char* rty = llvm_fn_ret_ty(fn);
                const char* pkt = cg_reg();
                cg_emit("%s = insertvalue %s { i1 false, ptr undef }, ptr %s, 1", pkt, rty, g_cg.yield_list_reg);
                cg_emit("ret %s %s", rty, pkt);
            } else if (fn->ret_type->kind == TY_VOID) {
                cg_emit("ret i1 false");
            } else {
                const char* rty = llvm_fn_ret_ty(fn);
                const char* pkt = cg_reg();
                cg_emit("%s = insertvalue %s { i1 false, %s undef }, %s %s, 1",
                        pkt, rty, llvm_reg_ty(fn->ret_type), llvm_reg_ty(fn->ret_type), llvm_zero(fn->ret_type));
                cg_emit("ret %s %s", rty, pkt);
            }
        } else {
            if (fn->is_generator) cg_emit("ret ptr %s", g_cg.yield_list_reg);
            else if (fn->ret_type->kind == TY_VOID) cg_emit("ret void");
            else cg_emit("ret %s %s", llvm_reg_ty(fn->ret_type), llvm_zero(fn->ret_type));
        }
    }

    if (fn->can_raise && g_cg.fn_unwind_lbl) {
        cg_label(g_cg.fn_unwind_lbl);
        cg_drop_all_fn_locals(fn);
        if (fn->ret_type->kind == TY_VOID) {
            cg_emit("ret i1 true");
        } else {
            cg_emit("ret %s { i1 true, %s %s }", llvm_fn_ret_ty(fn), llvm_reg_ty(fn->ret_type), llvm_zero(fn->ret_type));
        }
    }

    sb_printf(&g_cg.funcs, "define %s %s(%s) {\nentry:\n", llvm_fn_ret_ty(fn), fn->llvm_name, sig.data);
    sb_append_n(&g_cg.funcs, g_cg.cur_entry.data, g_cg.cur_entry.len);
    sb_append_n(&g_cg.funcs, g_cg.cur_body.data, g_cg.cur_body.len);
    sb_append(&g_cg.funcs, "}\n\n");
    free(sig.data);
    g_cg.fn_unwind_lbl = NULL;
    g_cg.cur_unwind_lbl = NULL;
    g_cg.in_func = false;
    g_cg.cur_fn = NULL;
    if (fn->is_async) emit_async_wrappers(fn);
}

static TypeRegistry g_async_drop_types = {NULL, 0, 0};

static bool type_needs_async_box(Type* t) {
    if (!t) return false;
    return t->kind == TY_TUPLE ||
           (t->kind == TY_OPTIONAL && !type_is_heap_ref(t->elem)) ||
           (t->kind == TY_CLASS && t->cls->is_value_type);
}

static const char* emit_async_drop_for_type(Type* rt) {
    if (!rt || rt->kind == TY_VOID || (!type_is_owned(rt) && !type_needs_async_box(rt))) return "null";
    for (int i = 0; i < g_async_drop_types.count; i++) {
        if (type_eq(g_async_drop_types.items[i], rt)) {
            return arena_sprintf("@_async_drop_%s", rt->mangled);
        }
    }
    reg_add_unique(&g_async_drop_types, rt);
    if (type_is_owned(rt)) emit_drop_for_type(rt);
    StrBuf* b = &g_cg.funcs;
    sb_printf(b, "define internal void @_async_drop_%s(i64 %%bits) {\nentry:\n", rt->mangled);
    if (type_needs_async_box(rt)) {
        sb_append(b, "  %box = inttoptr i64 %bits to ptr\n");
        if (type_is_owned(rt)) {
            sb_printf(b, "  %%v = load %s, ptr %%box\n", llvm_mem_ty(rt));
            sb_printf(b, "  call void @_drop_%s(%s %%v)\n", rt->mangled, llvm_mem_ty(rt));
        }
        sb_append(b, "  call void @stricta_free(ptr %box)\n  ret void\n}\n\n");
    } else {
        sb_append(b, "  %p = inttoptr i64 %bits to ptr\n");
        sb_printf(b, "  call void @_drop_%s(ptr %%p)\n  ret void\n}\n\n", rt->mangled);
    }
    return arena_sprintf("@_async_drop_%s", rt->mangled);
}

static void emit_async_wrappers(FuncInfo* fn) {
    const char* bare = (fn->llvm_name[0] == '@') ? fn->llvm_name + 1 : fn->llvm_name;
    const char* drop_fn = emit_async_drop_for_type(fn->ret_type);
    StrBuf* b = &g_cg.funcs;

    if (fn->param_count > 0) {
        sb_printf(b, "%%AsyncArgs_%s = type { ", bare);
        for (int i = 0; i < fn->param_count; i++) {
            if (i > 0) sb_append(b, ", ");
            sb_append(b, llvm_mem_ty(fn->param_types[i]));
        }
        sb_append(b, " }\n\n");
    }

    /* Worker entry point: ptr @_async_entry_<bare>(ptr %task) */
    sb_printf(b, "define internal ptr @_async_entry_%s(ptr %%task) {\nentry:\n", bare);
    StrBuf call_args;
    sb_init(&call_args);
    if (fn->param_count > 0) {
        sb_append(b, "  %ab = call ptr @stricta_task_args(ptr %task)\n");
        for (int i = 0; i < fn->param_count; i++) {
            Type* pt = fn->param_types[i];
            const char* rty = llvm_reg_ty(pt);
            const char* mty = llvm_mem_ty(pt);
            sb_printf(b, "  %%ap_%d = getelementptr %%AsyncArgs_%s, ptr %%ab, i32 0, i32 %d\n", i, bare, i);
            sb_printf(b, "  %%am_%d = load %s, ptr %%ap_%d\n", i, mty, i);
            if (pt->kind == TY_BOOL) {
                sb_printf(b, "  %%ar_%d = trunc i8 %%am_%d to i1\n", i, i);
                if (i > 0) sb_append(&call_args, ", ");
                sb_printf(&call_args, "i1 %%ar_%d", i);
            } else {
                if (i > 0) sb_append(&call_args, ", ");
                sb_printf(&call_args, "%s %%am_%d", rty, i);
            }
        }
        sb_append(b, "  call void @stricta_free(ptr %ab)\n");
    }
    const char* frty = llvm_fn_ret_ty(fn);
    if (strcmp(frty, "void") == 0) {
        sb_printf(b, "  call void %s(%s)\n", fn->llvm_name, call_args.data);
    } else {
        sb_printf(b, "  %%raw = call %s %s(%s)\n", frty, fn->llvm_name, call_args.data);
    }
    free(call_args.data);

    if (fn->can_raise) {
        if (fn->ret_type->kind == TY_VOID) {
            sb_append(b, "  br i1 %raw, label %l_exc, label %l_ok\n");
        } else {
            sb_append(b, "  %is_exc = extractvalue ");
            sb_printf(b, "%s %%raw, 0\n  br i1 %%is_exc, label %%l_exc, label %%l_ok\n", frty);
        }
        sb_append(b, "l_exc:\n  call void @stricta_task_capture_exc(ptr %task)\n  ret ptr null\nl_ok:\n");
        if (fn->ret_type->kind != TY_VOID) {
            sb_printf(b, "  %%rv = extractvalue %s %%raw, 1\n", frty);
        }
    } else if (fn->ret_type->kind != TY_VOID) {
        sb_append(b, "  br label %l_ok\nl_ok:\n");
    }

    const char* rv_name = fn->can_raise ? "%rv" : "%raw";
    Type* rt = fn->ret_type;
    if (!rt || rt->kind == TY_VOID) {
        sb_append(b, "  call void @stricta_task_finish(ptr %task, i64 0)\n  ret ptr null\n}\n\n");
    } else if (rt->kind == TY_BOOL) {
        sb_printf(b, "  %%bits = zext i1 %s to i64\n  call void @stricta_task_finish(ptr %%task, i64 %%bits)\n  ret ptr null\n}\n\n", rv_name);
    } else if (type_is_int(rt)) {
        if (rt->kind == TY_I64 || rt->kind == TY_U64) {
            sb_printf(b, "  call void @stricta_task_finish(ptr %%task, i64 %s)\n  ret ptr null\n}\n\n", rv_name);
        } else {
            const char* ext = type_is_unsigned_int(rt) ? "zext" : "sext";
            sb_printf(b, "  %%bits = %s %s %s to i64\n  call void @stricta_task_finish(ptr %%task, i64 %%bits)\n  ret ptr null\n}\n\n",
                      ext, llvm_reg_ty(rt), rv_name);
        }
    } else if (rt->kind == TY_F64) {
        sb_printf(b, "  %%bits = bitcast double %s to i64\n  call void @stricta_task_finish(ptr %%task, i64 %%bits)\n  ret ptr null\n}\n\n", rv_name);
    } else if (rt->kind == TY_F32) {
        sb_printf(b, "  %%d = fpext float %s to double\n  %%bits = bitcast double %%d to i64\n  call void @stricta_task_finish(ptr %%task, i64 %%bits)\n  ret ptr null\n}\n\n", rv_name);
    } else if (type_needs_async_box(rt)) {
        const char* mty = llvm_mem_ty(rt);
        sb_printf(b, "  %%sz = ptrtoint ptr getelementptr (%s, ptr null, i32 1) to i64\n", mty);
        sb_append(b, "  %box = call ptr @stricta_alloc(i64 %sz)\n");
        sb_printf(b, "  store %s %s, ptr %%box\n", mty, rv_name);
        sb_append(b, "  %bits = ptrtoint ptr %box to i64\n  call void @stricta_task_finish(ptr %task, i64 %bits)\n  ret ptr null\n}\n\n");
    } else {
        sb_printf(b, "  %%bits = ptrtoint ptr %s to i64\n  call void @stricta_task_finish(ptr %%task, i64 %%bits)\n  ret ptr null\n}\n\n", rv_name);
    }

    /* Spawn wrapper: ptr @_async_spawn_<bare>(params...) */
    StrBuf ssig; sb_init(&ssig);
    for (int i = 0; i < fn->param_count; i++) {
        if (i > 0) sb_append(&ssig, ", ");
        sb_printf(&ssig, "%s %%arg_%d", llvm_reg_ty(fn->param_types[i]), i);
    }
    sb_printf(b, "define internal ptr @_async_spawn_%s(%s) {\nentry:\n", bare, ssig.data);
    free(ssig.data);
    if (fn->param_count > 0) {
        sb_printf(b, "  %%sz = ptrtoint ptr getelementptr (%%AsyncArgs_%s, ptr null, i32 1) to i64\n", bare);
        sb_append(b, "  %ab = call ptr @stricta_alloc(i64 %sz)\n");
        for (int i = 0; i < fn->param_count; i++) {
            Type* pt = fn->param_types[i];
            sb_printf(b, "  %%sp_%d = getelementptr %%AsyncArgs_%s, ptr %%ab, i32 0, i32 %d\n", i, bare, i);
            if (pt->kind == TY_BOOL) {
                sb_printf(b, "  %%bx_%d = zext i1 %%arg_%d to i8\n", i, i);
                sb_printf(b, "  store i8 %%bx_%d, ptr %%sp_%d\n", i, i);
            } else {
                sb_printf(b, "  store %s %%arg_%d, ptr %%sp_%d\n", llvm_mem_ty(pt), i, i);
            }
        }
        sb_printf(b, "  %%t = call ptr @stricta_task_spawn(ptr @_async_entry_%s, ptr %%ab, ptr %s)\n  ret ptr %%t\n}\n\n", bare, drop_fn);
    } else {
        sb_printf(b, "  %%t = call ptr @stricta_task_spawn(ptr @_async_entry_%s, ptr null, ptr %s)\n  ret ptr %%t\n}\n\n", bare, drop_fn);
    }
}

static bool expr_can_raise(FuncInfo* cur_fn, Expr* e, bool in_catch_all) {
    if (!e) return false;
    switch (e->kind) {
        case EXPR_FSTRING:
            for (int i = 0; i < e->fstr.count; i++) {
                if (e->fstr.parts[i].is_expr && expr_can_raise(cur_fn, e->fstr.parts[i].expr, in_catch_all)) return true;
            }
            return false;
        case EXPR_BINOP:
            return expr_can_raise(cur_fn, e->binop.left, in_catch_all) ||
                   expr_can_raise(cur_fn, e->binop.right, in_catch_all);
        case EXPR_UNOP:
            return expr_can_raise(cur_fn, e->unop.operand, in_catch_all);
        case EXPR_AWAIT:
            if (!in_catch_all) return true;
            return expr_can_raise(cur_fn, e->await_e.expr, in_catch_all);
        case EXPR_TERNARY:
            return expr_can_raise(cur_fn, e->ternary.cond, in_catch_all) ||
                   expr_can_raise(cur_fn, e->ternary.then_e, in_catch_all) ||
                   expr_can_raise(cur_fn, e->ternary.else_e, in_catch_all);
        case EXPR_WALRUS:
            return expr_can_raise(cur_fn, e->walrus.val, in_catch_all);
        case EXPR_INDEX:
            return expr_can_raise(cur_fn, e->index.obj, in_catch_all) ||
                   expr_can_raise(cur_fn, e->index.index, in_catch_all);
        case EXPR_SLICE:
            if (e->slice.step && !(e->slice.step->kind == EXPR_INT && e->slice.step->int_lit.ival != 0) && !in_catch_all) return true;
            return expr_can_raise(cur_fn, e->slice.obj, in_catch_all) ||
                   expr_can_raise(cur_fn, e->slice.start, in_catch_all) ||
                   expr_can_raise(cur_fn, e->slice.stop, in_catch_all) ||
                   expr_can_raise(cur_fn, e->slice.step, in_catch_all);
        case EXPR_ATTR:
            if (e->attr.obj->ty && e->attr.obj->ty->kind == TY_CLASS) {
                FuncInfo* prop = find_method_in_class(e->attr.obj->ty->cls, e->attr.attr);
                if (prop && prop->is_property && prop->can_raise && !in_catch_all) return true;
            }
            return expr_can_raise(cur_fn, e->attr.obj, in_catch_all);
        case EXPR_LIST:
        case EXPR_TUPLE:
        case EXPR_SET:
            for (int i = 0; i < e->seq.count; i++) {
                if (expr_can_raise(cur_fn, e->seq.items[i], in_catch_all)) return true;
            }
            return false;
        case EXPR_DICT:
            for (int i = 0; i < e->dict.count; i++) {
                if (expr_can_raise(cur_fn, e->dict.keys[i], in_catch_all) ||
                    expr_can_raise(cur_fn, e->dict.vals[i], in_catch_all)) return true;
            }
            return false;
        case EXPR_LISTCOMP:
        case EXPR_SETCOMP:
        case EXPR_DICTCOMP:
        case EXPR_GENEXPR:
            if (expr_can_raise(cur_fn, e->comp.elt, in_catch_all) ||
                expr_can_raise(cur_fn, e->comp.val, in_catch_all)) return true;
            for (int i = 0; i < e->comp.gen_count; i++) {
                if (expr_can_raise(cur_fn, e->comp.gens[i].iter, in_catch_all)) return true;
                for (int f = 0; f < e->comp.gens[i].if_count; f++) {
                    if (expr_can_raise(cur_fn, e->comp.gens[i].ifs[f], in_catch_all)) return true;
                }
            }
            return false;
        case EXPR_CALL:
            for (int i = 0; i < e->call.arg_count; i++) {
                if (expr_can_raise(cur_fn, e->call.args[i], in_catch_all)) return true;
            }
            if (e->call.callee->kind == EXPR_ATTR) {
                if (expr_can_raise(cur_fn, e->call.callee->attr.obj, in_catch_all)) return true;
                Type* ot = e->call.callee->attr.obj->ty;
                if (ot && ot->kind == TY_CLASS) {
                    FuncInfo* m = e->call.resolved_fn ? e->call.resolved_fn : find_method_in_class(ot->cls, e->call.callee->attr.attr);
                    if (m && m->can_raise && !m->is_async && !in_catch_all) return true;
                } else if (ot && ot->kind == TY_MODULE) {
                    FuncInfo* mf = e->call.resolved_fn ? e->call.resolved_fn : find_func_in_mod(ot->mod, e->call.callee->attr.attr);
                    if (mf && mf->can_raise && !mf->is_async && !in_catch_all) return true;
                    ClassInfo* mci = find_class_in_mod(ot->mod, e->call.callee->attr.attr);
                    if (mci && !in_catch_all) {
                        if (mci->has_unimpl_abstract) return true;
                        FuncInfo* inf = find_method_in_class(mci, "__init__");
                        if (inf && inf->can_raise) return true;
                    }
                }
                return false;
            }
            if (e->call.callee->kind == EXPR_VAR) {
                const char* cname = e->call.callee->var.name;
                if (strcmp(cname, "next") == 0 && e->call.arg_count == 1 && !in_catch_all) return true;
                if (e->call.resolved_fn && e->call.resolved_fn->can_raise && !e->call.resolved_fn->is_async && !in_catch_all) return true;
                ClassInfo* ci = cur_fn ? find_class_in_mod(cur_fn->mod, cname) : NULL;
                if (ci && !in_catch_all) {
                    if (ci->has_unimpl_abstract) return true;
                    FuncInfo* inf = find_method_in_class(ci, "__init__");
                    if (inf && inf->can_raise) return true;
                }
                FuncInfo* tf = cur_fn ? find_func_in_mod(cur_fn->mod, cname) : NULL;
                if (tf && tf->can_raise && !tf->is_async && !in_catch_all) return true;
            }
            return false;
        default:
            return false;
    }
}

static bool stmts_can_raise(FuncInfo* cur_fn, Stmt** stmts, int count, bool in_catch_all) {
    for (int i = 0; i < count; i++) {
        Stmt* s = stmts[i];
        switch (s->kind) {
            case STMT_RAISE:
                if (!in_catch_all) return true;
                break;
            case STMT_RETURN:
                if (expr_can_raise(cur_fn, s->ret.expr, in_catch_all)) return true;
                break;
            case STMT_EXPR:
            case STMT_YIELD:
            case STMT_YIELD_FROM:
                if (expr_can_raise(cur_fn, s->expr_s.expr, in_catch_all)) return true;
                break;
            case STMT_ASSIGN:
                if (expr_can_raise(cur_fn, s->assign.target, in_catch_all) ||
                    expr_can_raise(cur_fn, s->assign.value, in_catch_all)) return true;
                break;
            case STMT_MULTI_ASSIGN:
                if (expr_can_raise(cur_fn, s->multi_assign.value, in_catch_all)) return true;
                break;
            case STMT_AUG_ASSIGN:
                if (expr_can_raise(cur_fn, s->aug_assign.target, in_catch_all) ||
                    expr_can_raise(cur_fn, s->aug_assign.value, in_catch_all)) return true;
                break;
            case STMT_IF:
                for (int b = 0; b < s->if_s.branch_count; b++) {
                    if (expr_can_raise(cur_fn, s->if_s.conds[b], in_catch_all)) return true;
                    if (stmts_can_raise(cur_fn, s->if_s.bodies[b], s->if_s.body_counts[b], in_catch_all)) return true;
                }
                if (stmts_can_raise(cur_fn, s->if_s.else_body, s->if_s.else_count, in_catch_all)) return true;
                break;
            case STMT_WHILE:
                if (expr_can_raise(cur_fn, s->while_s.cond, in_catch_all) ||
                    stmts_can_raise(cur_fn, s->while_s.body, s->while_s.body_count, in_catch_all) ||
                    stmts_can_raise(cur_fn, s->while_s.else_body, s->while_s.else_count, in_catch_all)) return true;
                break;
            case STMT_FOR:
                if (expr_can_raise(cur_fn, s->for_s.iter, in_catch_all) ||
                    stmts_can_raise(cur_fn, s->for_s.body, s->for_s.body_count, in_catch_all) ||
                    stmts_can_raise(cur_fn, s->for_s.else_body, s->for_s.else_count, in_catch_all)) return true;
                break;
            case STMT_TRY: {
                bool has_bare = false;
                for (int h = 0; h < s->try_s.handler_count; h++) {
                    if (!s->try_s.handlers[h].exc_type) { has_bare = true; break; }
                }
                if (stmts_can_raise(cur_fn, s->try_s.try_body, s->try_s.try_count, in_catch_all || has_bare)) return true;
                for (int h = 0; h < s->try_s.handler_count; h++) {
                    if (stmts_can_raise(cur_fn, s->try_s.handlers[h].body, s->try_s.handlers[h].body_count, in_catch_all)) return true;
                }
                if (stmts_can_raise(cur_fn, s->try_s.else_body, s->try_s.else_count, in_catch_all)) return true;
                if (stmts_can_raise(cur_fn, s->try_s.finally_body, s->try_s.finally_count, in_catch_all)) return true;
                break;
            }
            case STMT_WITH:
                if (expr_can_raise(cur_fn, s->with_s.ctx, in_catch_all) ||
                    stmts_can_raise(cur_fn, s->with_s.body, s->with_s.body_count, in_catch_all)) return true;
                break;
            case STMT_ASSERT:
                if (expr_can_raise(cur_fn, s->assert_s.cond, in_catch_all) ||
                    expr_can_raise(cur_fn, s->assert_s.msg, in_catch_all)) return true;
                break;
            case STMT_DEL:
                if (expr_can_raise(cur_fn, s->del_s.target, in_catch_all)) return true;
                break;
            case STMT_MATCH:
                if (expr_can_raise(cur_fn, s->match_s.subject, in_catch_all)) return true;
                for (int c = 0; c < s->match_s.case_count; c++) {
                    if (expr_can_raise(cur_fn, s->match_s.cases[c].pattern, in_catch_all) ||
                        stmts_can_raise(cur_fn, s->match_s.cases[c].body, s->match_s.cases[c].body_count, in_catch_all)) return true;
                }
                break;
            default:
                break;
        }
    }
    return false;
}

static void analyze_can_raise(void) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < g_all_func_count; i++) {
            FuncInfo* fn = g_all_funcs[i];
            if (fn->is_generic || fn->is_extern || fn->can_raise) continue;
            bool cr = false;
            if (fn->is_lambda && fn->lambda_ast) {
                cr = expr_can_raise(fn, fn->lambda_ast->lambda.body, false);
            } else if (fn->ast) {
                cr = stmts_can_raise(fn, fn->ast->def_s.body, fn->ast->def_s.body_count, false);
            }
            if (cr) { fn->can_raise = true; changed = true; }
        }
        /* Propagate can_raise across vtable families so overrides share exact LLVM return ABI */
        for (int c = 0; c < g_all_class_count; c++) {
            ClassInfo* cls = g_all_classes[c];
            for (int v = 0; v < cls->vtable_count; v++) {
                FuncInfo* vm = cls->vtable[v].fn;
                if (!vm) continue;
                for (int m = 0; m < cls->mro_count; m++) {
                    FuncInfo* am = find_method_in_class_exact(cls->mro[m], cls->vtable[v].name);
                    if (am && am->can_raise != vm->can_raise) {
                        am->can_raise = true;
                        vm->can_raise = true;
                        changed = true;
                    }
                }
            }
        }
        for (int m = 0; m < g_module_count; m++) {
            for (int f = 0; f < g_modules[m]->func_count; f++) {
                FuncInfo* fn = g_modules[m]->funcs[f];
                if (fn->orig_fn && fn->orig_fn->can_raise && !fn->can_raise) {
                    fn->can_raise = true;
                    changed = true;
                }
            }
        }
    }
}

static void emit_rt_cstr(StrBuf* b, const char* sym, const char* s) {
    size_t slen = strlen(s);
    sb_printf(b, "%s = private unnamed_addr constant [%zu x i8] c\"", sym, slen + 1);
    for (size_t i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == '\\') sb_append(b, "\\5C");
        else if (c == '"') sb_append(b, "\\22");
        else if (c < 32 || c >= 127) sb_printf(b, "\\%02X", (unsigned)c);
        else sb_printf(b, "%c", (char)c);
    }
    sb_append(b, "\\00\"\n");
}

static void emit_llvm_rt_core(StrBuf* b) {
    sb_append(b, "%StrictaStr = type { i32, i32, ptr, i32 }\n");
    sb_append(b, "%StrictaBytes = type { i32, i32, ptr }\n");
    sb_append(b, "%StrictaPtrList = type { i32, i32, ptr }\n");
    sb_append(b, "%StrictaBigInt = type { i32, i32, ptr }\n");
    sb_append(b, "%StrictaFile = type { ptr }\n");
    sb_append(b, "%StrictaTask = type { i64, i32, i32, i64, ptr, ptr, i32, ptr, ptr }\n\n");

    sb_append(b, "@stricta_sys_argv = internal global ptr null\n");
    sb_append(b, "@_exc_type_id = internal thread_local global i32 0\n");
    sb_append(b, "@_exc_obj = internal thread_local global ptr null\n");
    sb_append(b, "@_exc_msg = internal thread_local global ptr null\n\n");

    emit_rt_cstr(b, "@.rt_oom", "MemoryError: out of memory\n");
    emit_rt_cstr(b, "@.rt_empty", "");
    emit_rt_cstr(b, "@.rt_true", "True");
    emit_rt_cstr(b, "@.rt_false", "False");
    emit_rt_cstr(b, "@.rt_zero", "0");
    emit_rt_cstr(b, "@.rt_one", "1");
    emit_rt_cstr(b, "@.rt_neg_one", "-1");
    emit_rt_cstr(b, "@.rt_unk", "<unknown>");
    emit_rt_cstr(b, "@.rt_str_ctx", "<str>");
    emit_rt_cstr(b, "@.rt_unh_msg", "Unhandled exception: %s\n");
    emit_rt_cstr(b, "@.rt_unh_id", "Unhandled exception (type_id=%d)\n");
    emit_rt_cstr(b, "@.rt_assert_msg", "AssertionError (%s:%d): %s\n");
    emit_rt_cstr(b, "@.rt_assert_nomsg", "AssertionError (%s:%d)\n");
    emit_rt_cstr(b, "@.rt_bounds", "IndexError (%s:%d): index %lld out of range for length %lld\n");
    emit_rt_cstr(b, "@.rt_err_step", "ValueError: slice step cannot be zero");
    emit_rt_cstr(b, "@.rt_err_sep", "ValueError: empty separator");
    emit_rt_cstr(b, "@.rt_err_sub", "ValueError: substring not found");
    emit_rt_cstr(b, "@.rt_err_zdiv", "ZeroDivisionError: integer division by zero");
    emit_rt_cstr(b, "@.rt_err_open", "FileNotFoundError: cannot open file");
    emit_rt_cstr(b, "@.rt_err_task", "RuntimeError: failed to spawn async task thread (%d)\n");
    emit_rt_cstr(b, "@.rt_mode_r", "r");
    emit_rt_cstr(b, "@.rt_fmt_lld", "%lld");
    emit_rt_cstr(b, "@.rt_fmt_llu", "%llu");
    emit_rt_cstr(b, "@.rt_fmt_g", "%.15g");
    emit_rt_cstr(b, "@.rt_fmt_ffmt", "%.*f");
    emit_rt_cstr(b, "@.rt_fmt_s", "%s");
    emit_rt_cstr(b, "@.rt_fmt_sn", "%.*s");
    emit_rt_cstr(b, "@.rt_fmt_hex", "0x%llx");
    emit_rt_cstr(b, "@.rt_fmt_nhex", "-0x%llx");
    emit_rt_cstr(b, "@.rt_fmt_oct", "0o%llo");
    emit_rt_cstr(b, "@.rt_fmt_noct", "-0o%llo");
    sb_append(b, "\n");

    /* Libc / POSIX external declarations */
    sb_append(b, "declare noalias ptr @malloc(i64)\n");
    sb_append(b, "declare noalias ptr @calloc(i64, i64)\n");
    sb_append(b, "declare ptr @realloc(ptr, i64)\n");
    sb_append(b, "declare void @free(ptr)\n");
    sb_append(b, "declare i32 @memcmp(ptr, ptr, i64)\n");
    sb_append(b, "declare i64 @strlen(ptr)\n");
    sb_append(b, "declare ptr @strstr(ptr, ptr)\n");
    sb_append(b, "declare ptr @strchr(ptr, i32)\n");
    sb_append(b, "declare i64 @strtoll(ptr, ptr, i32)\n");
    sb_append(b, "declare double @strtod(ptr, ptr)\n");
    sb_append(b, "declare i32 @printf(ptr, ...)\n");
    sb_append(b, "declare i32 @dprintf(i32, ptr, ...)\n");
    sb_append(b, "declare i32 @snprintf(ptr, i64, ptr, ...)\n");
    sb_append(b, "declare i32 @putchar(i32)\n");
    sb_append(b, "declare i32 @getchar()\n");
    sb_append(b, "declare i32 @fflush(ptr)\n");
    sb_append(b, "declare ptr @fopen(ptr, ptr)\n");
    sb_append(b, "declare i32 @fseek(ptr, i64, i32)\n");
    sb_append(b, "declare i64 @ftell(ptr)\n");
    sb_append(b, "declare i64 @fread(ptr, i64, i64, ptr)\n");
    sb_append(b, "declare i64 @fwrite(ptr, i64, i64, ptr)\n");
    sb_append(b, "declare i32 @fclose(ptr)\n");
    sb_append(b, "declare void @exit(i32)\n");
    sb_append(b, "declare double @pow(double, double)\n");
    sb_append(b, "declare i32 @pthread_create(ptr, ptr, ptr, ptr)\n");
    sb_append(b, "declare i32 @pthread_join(i64, ptr)\n");
    sb_append(b, "declare i32 @llvm.smax.i32(i32, i32)\n");
    sb_append(b, "declare i32 @llvm.smin.i32(i32, i32)\n");
    sb_append(b, "declare i64 @llvm.smax.i64(i64, i64)\n");
    sb_append(b, "declare i64 @llvm.smin.i64(i64, i64)\n");
    sb_append(b, "declare double @llvm.floor.f64(double)\n");
    sb_append(b, "declare float @llvm.floor.f32(float)\n");
    sb_append(b, "declare double @llvm.round.f64(double)\n");
    sb_append(b, "declare double @llvm.fabs.f64(double)\n");
    sb_append(b, "declare float @llvm.fabs.f32(float)\n");
    sb_append(b, "declare i64 @llvm.abs.i64(i64, i1)\n");
    sb_append(b, "declare i32 @llvm.abs.i32(i32, i1)\n");
    sb_append(b, "declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)\n");
    sb_append(b, "declare void @llvm.memmove.p0.p0.i64(ptr, ptr, i64, i1)\n\n");

    /* Memory helpers */
    sb_append(b,
        "define internal ptr @stricta_alloc(i64 %sz) {\nentry:\n"
        "  %nz = icmp eq i64 %sz, 0\n"
        "  %rsz = select i1 %nz, i64 1, i64 %sz\n"
        "  %p = call ptr @malloc(i64 %rsz)\n"
        "  %ok = icmp ne ptr %p, null\n"
        "  br i1 %ok, label %done, label %oom\noom:\n"
        "  %e = call i32 (i32, ptr, ...) @dprintf(i32 2, ptr @.rt_oom)\n"
        "  call void @exit(i32 1)\n  unreachable\ndone:\n  ret ptr %p\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_calloc(i64 %cnt, i64 %sz) {\nentry:\n"
        "  %c0 = icmp eq i64 %cnt, 0\n  %rc = select i1 %c0, i64 1, i64 %cnt\n"
        "  %s0 = icmp eq i64 %sz, 0\n  %rs = select i1 %s0, i64 1, i64 %sz\n"
        "  %p = call ptr @calloc(i64 %rc, i64 %rs)\n"
        "  %ok = icmp ne ptr %p, null\n  br i1 %ok, label %done, label %oom\noom:\n"
        "  %e = call i32 (i32, ptr, ...) @dprintf(i32 2, ptr @.rt_oom)\n"
        "  call void @exit(i32 1)\n  unreachable\ndone:\n  ret ptr %p\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_realloc(ptr %ptr, i64 %sz) {\nentry:\n"
        "  %s0 = icmp eq i64 %sz, 0\n  %rs = select i1 %s0, i64 1, i64 %sz\n"
        "  %p = call ptr @realloc(ptr %ptr, i64 %rs)\n"
        "  %ok = icmp ne ptr %p, null\n  br i1 %ok, label %done, label %oom\noom:\n"
        "  %e = call i32 (i32, ptr, ...) @dprintf(i32 2, ptr @.rt_oom)\n"
        "  call void @exit(i32 1)\n  unreachable\ndone:\n  ret ptr %p\n}\n\n");
    sb_append(b,
        "define internal void @stricta_free(ptr %ptr) alwaysinline {\nentry:\n"
        "  call void @free(ptr %ptr)\n  ret void\n}\n\n");

    /* CLI args */
    sb_append(b,
        "define internal void @stricta_init_args(i32 %argc, ptr %argv) {\nentry:\n"
        "  %lsz = ptrtoint ptr getelementptr (%StrictaPtrList, ptr null, i32 1) to i64\n"
        "  %lst = call ptr @stricta_alloc(i64 %lsz)\n"
        "  %p0 = getelementptr %StrictaPtrList, ptr %lst, i32 0, i32 0\n"
        "  store i32 %argc, ptr %p0\n"
        "  %c4 = icmp sgt i32 %argc, 4\n"
        "  %cap = select i1 %c4, i32 %argc, i32 4\n"
        "  %p1 = getelementptr %StrictaPtrList, ptr %lst, i32 0, i32 1\n"
        "  store i32 %cap, ptr %p1\n"
        "  %cap64 = sext i32 %cap to i64\n"
        "  %asz = mul i64 %cap64, 8\n"
        "  %items = call ptr @stricta_alloc(i64 %asz)\n"
        "  %p2 = getelementptr %StrictaPtrList, ptr %lst, i32 0, i32 2\n"
        "  store ptr %items, ptr %p2\n"
        "  %ip = alloca i32\n  store i32 0, ptr %ip\n  br label %loop\nloop:\n"
        "  %i = load i32, ptr %ip\n  %cond = icmp slt i32 %i, %argc\n"
        "  br i1 %cond, label %body, label %done\nbody:\n"
        "  %ap = getelementptr ptr, ptr %argv, i32 %i\n"
        "  %arg = load ptr, ptr %ap\n"
        "  %s = call ptr @stricta_str_cstr(ptr %arg)\n"
        "  %dp = getelementptr ptr, ptr %items, i32 %i\n"
        "  store ptr %s, ptr %dp\n"
        "  %ni = add i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %loop\ndone:\n"
        "  store ptr %lst, ptr @stricta_sys_argv\n  ret void\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_sys_get_argv() {\nentry:\n"
        "  %sav = load ptr, ptr @stricta_sys_argv\n"
        "  %has = icmp ne ptr %sav, null\n"
        "  br i1 %has, label %gn, label %zn\nzn:\n  br label %bn\ngn:\n"
        "  %lp0 = getelementptr %StrictaPtrList, ptr %sav, i32 0, i32 0\n"
        "  %n0 = load i32, ptr %lp0\n  br label %bn\nbn:\n"
        "  %n = phi i32 [ 0, %zn ], [ %n0, %gn ]\n"
        "  %lsz = ptrtoint ptr getelementptr (%StrictaPtrList, ptr null, i32 1) to i64\n"
        "  %res = call ptr @stricta_alloc(i64 %lsz)\n"
        "  %p0 = getelementptr %StrictaPtrList, ptr %res, i32 0, i32 0\n  store i32 %n, ptr %p0\n"
        "  %c4 = icmp sgt i32 %n, 4\n  %cap = select i1 %c4, i32 %n, i32 4\n"
        "  %p1 = getelementptr %StrictaPtrList, ptr %res, i32 0, i32 1\n  store i32 %cap, ptr %p1\n"
        "  %cap64 = sext i32 %cap to i64\n  %asz = mul i64 %cap64, 8\n"
        "  %items = call ptr @stricta_alloc(i64 %asz)\n"
        "  %p2 = getelementptr %StrictaPtrList, ptr %res, i32 0, i32 2\n  store ptr %items, ptr %p2\n"
        "  %has2 = icmp ne ptr %sav, null\n  br i1 %has2, label %initl, label %done\ninitl:\n"
        "  %sp2 = getelementptr %StrictaPtrList, ptr %sav, i32 0, i32 2\n"
        "  %sitems = load ptr, ptr %sp2\n"
        "  %ip = alloca i32\n  store i32 0, ptr %ip\n  br label %loop\nloop:\n"
        "  %i = load i32, ptr %ip\n  %cond = icmp slt i32 %i, %n\n"
        "  br i1 %cond, label %body, label %done\nbody:\n"
        "  %sp = getelementptr ptr, ptr %sitems, i32 %i\n  %sv = load ptr, ptr %sp\n"
        "  %cl = call ptr @stricta_str_clone(ptr %sv)\n"
        "  %dp = getelementptr ptr, ptr %items, i32 %i\n  store ptr %cl, ptr %dp\n"
        "  %ni = add i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %loop\ndone:\n"
        "  ret ptr %res\n}\n\n");

    /* Exceptions & traps */
    sb_append(b,
        "define internal void @stricta_set_exc(i32 %tid, ptr %obj, ptr %msg) {\nentry:\n"
        "  %old = load ptr, ptr @_exc_msg\n  %has = icmp ne ptr %old, null\n"
        "  br i1 %has, label %freemsg, label %set\nfreemsg:\n"
        "  call void @stricta_str_free(ptr %old)\n  store ptr null, ptr @_exc_msg\n  br label %set\nset:\n"
        "  store i32 %tid, ptr @_exc_type_id\n  store ptr %obj, ptr @_exc_obj\n"
        "  %hasm = icmp ne ptr %msg, null\n  %cstr = select i1 %hasm, ptr %msg, ptr @.rt_empty\n"
        "  %ns = call ptr @stricta_str_cstr(ptr %cstr)\n"
        "  store ptr %ns, ptr @_exc_msg\n  ret void\n}\n\n");
    sb_append(b,
        "define internal void @stricta_clear_exc(i32 %free_obj) {\nentry:\n"
        "  %old = load ptr, ptr @_exc_msg\n  %has = icmp ne ptr %old, null\n"
        "  br i1 %has, label %freemsg, label %chkobj\nfreemsg:\n"
        "  call void @stricta_str_free(ptr %old)\n  store ptr null, ptr @_exc_msg\n  br label %chkobj\nchkobj:\n"
        "  %fo = icmp ne i32 %free_obj, 0\n  %obj = load ptr, ptr @_exc_obj\n"
        "  %ho = icmp ne ptr %obj, null\n  %do_free = and i1 %fo, %ho\n"
        "  br i1 %do_free, label %freeobj, label %done\nfreeobj:\n"
        "  call void @free(ptr %obj)\n  br label %done\ndone:\n"
        "  store ptr null, ptr @_exc_obj\n  store i32 0, ptr @_exc_type_id\n  ret void\n}\n\n");
    sb_append(b,
        "define internal void @stricta_unhandled_exc() {\nentry:\n"
        "  %m = load ptr, ptr @_exc_msg\n  %hm = icmp ne ptr %m, null\n"
        "  br i1 %hm, label %chklen, label %noid\nchklen:\n"
        "  %lp = getelementptr %StrictaStr, ptr %m, i32 0, i32 0\n"
        "  %l = load i32, ptr %lp\n  %gt0 = icmp sgt i32 %l, 0\n"
        "  br i1 %gt0, label %pmsg, label %noid\npmsg:\n"
        "  %cp = getelementptr %StrictaStr, ptr %m, i32 0, i32 2\n"
        "  %cs = load ptr, ptr %cp\n"
        "  %r1 = call i32 (i32, ptr, ...) @dprintf(i32 2, ptr @.rt_unh_msg, ptr %cs)\n"
        "  br label %die\nnoid:\n"
        "  %tid = load i32, ptr @_exc_type_id\n"
        "  %r2 = call i32 (i32, ptr, ...) @dprintf(i32 2, ptr @.rt_unh_id, i32 %tid)\n"
        "  br label %die\ndie:\n  call void @exit(i32 1)\n  unreachable\n}\n\n");
    sb_append(b,
        "define internal i32 @stricta_exc_type_id() {\nentry:\n  %v = load i32, ptr @_exc_type_id\n  ret i32 %v\n}\n\n"
        "define internal ptr @stricta_exc_obj() {\nentry:\n  %v = load ptr, ptr @_exc_obj\n  ret ptr %v\n}\n\n"
        "define internal ptr @stricta_exc_msg() {\nentry:\n"
        "  %v = load ptr, ptr @_exc_msg\n  %ok = icmp ne ptr %v, null\n"
        "  br i1 %ok, label %retm, label %empt\nretm:\n  ret ptr %v\nempt:\n"
        "  %e = call ptr @stricta_str_cstr(ptr @.rt_empty)\n  ret ptr %e\n}\n\n");
    sb_append(b,
        "define internal void @stricta_assert_fail(ptr %msg, ptr %file, i32 %line) {\nentry:\n"
        "  %hf = icmp ne ptr %file, null\n  %rf = select i1 %hf, ptr %file, ptr @.rt_unk\n"
        "  %hm = icmp ne ptr %msg, null\n  br i1 %hm, label %chk0, label %nomsg\nchk0:\n"
        "  %c0 = load i8, ptr %msg\n  %ne0 = icmp ne i8 %c0, 0\n"
        "  br i1 %ne0, label %withmsg, label %nomsg\nwithmsg:\n"
        "  %r1 = call i32 (i32, ptr, ...) @dprintf(i32 2, ptr @.rt_assert_msg, ptr %rf, i32 %line, ptr %msg)\n"
        "  br label %die\nnomsg:\n"
        "  %r2 = call i32 (i32, ptr, ...) @dprintf(i32 2, ptr @.rt_assert_nomsg, ptr %rf, i32 %line)\n"
        "  br label %die\ndie:\n  call void @exit(i32 1)\n  unreachable\n}\n\n");
    sb_append(b,
        "define internal void @stricta_bounds_fail(i64 %idx, i64 %len, ptr %file, i32 %line) {\nentry:\n"
        "  %hf = icmp ne ptr %file, null\n  %rf = select i1 %hf, ptr %file, ptr @.rt_unk\n"
        "  %r = call i32 (i32, ptr, ...) @dprintf(i32 2, ptr @.rt_bounds, ptr %rf, i32 %line, i64 %idx, i64 %len)\n"
        "  call void @exit(i32 1)\n  unreachable\n}\n\n");

    /* Print & Input */
    sb_append(b,
        "define internal void @stricta_print_i64(i64 %v) {\nentry:\n"
        "  %r = call i32 (ptr, ...) @printf(ptr @.rt_fmt_lld, i64 %v)\n  ret void\n}\n\n"
        "define internal void @stricta_print_u64(i64 %v) {\nentry:\n"
        "  %r = call i32 (ptr, ...) @printf(ptr @.rt_fmt_llu, i64 %v)\n  ret void\n}\n\n"
        "define internal void @stricta_print_bool(i32 %v) {\nentry:\n"
        "  %b = icmp ne i32 %v, 0\n  %s = select i1 %b, ptr @.rt_true, ptr @.rt_false\n"
        "  %r = call i32 (ptr, ...) @printf(ptr @.rt_fmt_s, ptr %s)\n  ret void\n}\n\n"
        "define internal void @stricta_print_str(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %chk, label %done\nchk:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %gt0 = icmp sgt i32 %len, 0\n  br i1 %gt0, label %pr, label %done\npr:\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %cs = load ptr, ptr %cp\n"
        "  %r = call i32 (ptr, ...) @printf(ptr @.rt_fmt_sn, i32 %len, ptr %cs)\n  br label %done\ndone:\n  ret void\n}\n\n"
        "define internal void @stricta_print_cstr(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %pr, label %done\npr:\n"
        "  %r = call i32 (ptr, ...) @printf(ptr @.rt_fmt_s, ptr %s)\n  br label %done\ndone:\n  ret void\n}\n\n"
        "define internal void @stricta_print_space() {\nentry:\n  %r = call i32 @putchar(i32 32)\n  ret void\n}\n\n"
        "define internal void @stricta_print_newline() {\nentry:\n  %r = call i32 @putchar(i32 10)\n  ret void\n}\n\n");
    sb_append(b,
        "define internal void @stricta_print_f64(double %v) {\nentry:\n"
        "  %buf = alloca [64 x i8]\n"
        "  %n = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 64, ptr @.rt_fmt_g, double %v)\n"
        "  %p1 = call ptr @strchr(ptr %buf, i32 46)\n  %b1 = icmp ne ptr %p1, null\n  br i1 %b1, label %out, label %c2\nc2:\n"
        "  %p2 = call ptr @strchr(ptr %buf, i32 101)\n  %b2 = icmp ne ptr %p2, null\n  br i1 %b2, label %out, label %c3\nc3:\n"
        "  %p3 = call ptr @strchr(ptr %buf, i32 69)\n  %b3 = icmp ne ptr %p3, null\n  br i1 %b3, label %out, label %c4\nc4:\n"
        "  %p4 = call ptr @strchr(ptr %buf, i32 110)\n  %b4 = icmp ne ptr %p4, null\n  br i1 %b4, label %out, label %c5\nc5:\n"
        "  %p5 = call ptr @strchr(ptr %buf, i32 105)\n  %b5 = icmp ne ptr %p5, null\n  br i1 %b5, label %out, label %app\napp:\n"
        "  %n2 = add i32 %n, 2\n  %fit = icmp slt i32 %n2, 64\n  br i1 %fit, label %doapp, label %out\ndoapp:\n"
        "  %g0 = getelementptr i8, ptr %buf, i32 %n\n  store i8 46, ptr %g0\n"
        "  %n1 = add i32 %n, 1\n  %g1 = getelementptr i8, ptr %buf, i32 %n1\n  store i8 48, ptr %g1\n"
        "  %g2 = getelementptr i8, ptr %buf, i32 %n2\n  store i8 0, ptr %g2\n  br label %out\nout:\n"
        "  %r = call i32 (ptr, ...) @printf(ptr @.rt_fmt_s, ptr %buf)\n  ret void\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_input(ptr %prompt) {\nentry:\n"
        "  %hp = icmp ne ptr %prompt, null\n  br i1 %hp, label %chk, label %read\nchk:\n"
        "  %plp = getelementptr %StrictaStr, ptr %prompt, i32 0, i32 0\n  %pl = load i32, ptr %plp\n"
        "  %pg = icmp sgt i32 %pl, 0\n  br i1 %pg, label %pr, label %read\npr:\n"
        "  %pcp = getelementptr %StrictaStr, ptr %prompt, i32 0, i32 2\n  %pcs = load ptr, ptr %pcp\n"
        "  %r1 = call i32 (ptr, ...) @printf(ptr @.rt_fmt_sn, i32 %pl, ptr %pcs)\n"
        "  %r2 = call i32 @fflush(ptr null)\n  br label %read\nread:\n"
        "  %cap_p = alloca i64\n  %len_p = alloca i64\n  %buf_p = alloca ptr\n"
        "  store i64 256, ptr %cap_p\n  store i64 0, ptr %len_p\n"
        "  %ib = call ptr @stricta_alloc(i64 256)\n  store ptr %ib, ptr %buf_p\n  br label %loop\nloop:\n"
        "  %ch = call i32 @getchar()\n"
        "  %eof = icmp eq i32 %ch, -1\n  %nl = icmp eq i32 %ch, 10\n  %stop = or i1 %eof, %nl\n"
        "  br i1 %stop, label %done, label %push\npush:\n"
        "  %len = load i64, ptr %len_p\n  %cap = load i64, ptr %cap_p\n"
        "  %need = add i64 %len, 2\n  %grow = icmp ugt i64 %need, %cap\n"
        "  br i1 %grow, label %dogrow, label %storec\ndogrow:\n"
        "  %ncap = mul i64 %cap, 2\n  %ob = load ptr, ptr %buf_p\n"
        "  %nb = call ptr @stricta_realloc(ptr %ob, i64 %ncap)\n"
        "  store ptr %nb, ptr %buf_p\n  store i64 %ncap, ptr %cap_p\n  br label %storec\nstorec:\n"
        "  %cb = load ptr, ptr %buf_p\n  %gp = getelementptr i8, ptr %cb, i64 %len\n"
        "  %c8 = trunc i32 %ch to i8\n  store i8 %c8, ptr %gp\n"
        "  %nlen = add i64 %len, 1\n  store i64 %nlen, ptr %len_p\n  br label %loop\ndone:\n"
        "  %flen = load i64, ptr %len_p\n  %fb = load ptr, ptr %buf_p\n"
        "  %gt0 = icmp sgt i64 %flen, 0\n  br i1 %gt0, label %chkcr, label %mk\nchkcr:\n"
        "  %m1 = sub i64 %flen, 1\n  %lp = getelementptr i8, ptr %fb, i64 %m1\n"
        "  %lc = load i8, ptr %lp\n  %iscr = icmp eq i8 %lc, 13\n"
        "  br i1 %iscr, label %stripcr, label %mk\nstripcr:\n"
        "  store i64 %m1, ptr %len_p\n  br label %mk\nmk:\n"
        "  %fl2 = load i64, ptr %len_p\n  %fl32 = trunc i64 %fl2 to i32\n"
        "  %res = call ptr @stricta_str_new(ptr %fb, i32 %fl32)\n"
        "  call void @free(ptr %fb)\n  ret ptr %res\n}\n\n");
}

static void emit_llvm_rt_str(StrBuf* b) {
    sb_append(b,
        "define internal ptr @stricta_str_new(ptr %s, i32 %len) {\nentry:\n"
        "  %ssz = ptrtoint ptr getelementptr (%StrictaStr, ptr null, i32 1) to i64\n"
        "  %st = call ptr @stricta_alloc(i64 %ssz)\n"
        "  %p0 = getelementptr %StrictaStr, ptr %st, i32 0, i32 0\n  store i32 %len, ptr %p0\n"
        "  %p1 = getelementptr %StrictaStr, ptr %st, i32 0, i32 1\n  store i32 0, ptr %p1\n"
        "  %p3 = getelementptr %StrictaStr, ptr %st, i32 0, i32 3\n  store i32 0, ptr %p3\n"
        "  %l64 = sext i32 %len to i64\n  %asize = add i64 %l64, 1\n"
        "  %ch = call ptr @stricta_alloc(i64 %asize)\n"
        "  %p2 = getelementptr %StrictaStr, ptr %st, i32 0, i32 2\n  store ptr %ch, ptr %p2\n"
        "  %gt0 = icmp sgt i32 %len, 0\n  %ns = icmp ne ptr %s, null\n  %do = and i1 %gt0, %ns\n"
        "  br i1 %do, label %cp, label %nul\ncp:\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %ch, ptr %s, i64 %l64, i1 false)\n  br label %nul\nnul:\n"
        "  %end = getelementptr i8, ptr %ch, i64 %l64\n  store i8 0, ptr %end\n  ret ptr %st\n}\n\n");
    sb_append(b,
        "define internal void @stricta_str_free(ptr %s) alwaysinline {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %chk, label %done\nchk:\n"
        "  %sp = getelementptr %StrictaStr, ptr %s, i32 0, i32 3\n  %stat = load i32, ptr %sp\n"
        "  %dyn = icmp eq i32 %stat, 0\n  br i1 %dyn, label %dofree, label %done\ndofree:\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  call void @free(ptr %ch)\n  call void @free(ptr %s)\n  br label %done\ndone:\n  ret void\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_clone(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %chk, label %ret_same\nchk:\n"
        "  %sp = getelementptr %StrictaStr, ptr %s, i32 0, i32 3\n  %stat = load i32, ptr %sp\n"
        "  %dyn = icmp eq i32 %stat, 0\n  br i1 %dyn, label %doclone, label %ret_same\nret_same:\n"
        "  ret ptr %s\ndoclone:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %r = call ptr @stricta_str_new(ptr %ch, i32 %len)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_cstr(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %nonnull, label %empt\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nnonnull:\n"
        "  %l64 = call i64 @strlen(ptr %s)\n  %l32 = trunc i64 %l64 to i32\n"
        "  %r = call ptr @stricta_str_new(ptr %s, i32 %l32)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_concat(ptr %a, ptr %b) {\nentry:\n"
        "  %ha = icmp ne ptr %a, null\n  br i1 %ha, label %ga, label %za\nza:\n  br label %ba\nga:\n"
        "  %lap = getelementptr %StrictaStr, ptr %a, i32 0, i32 0\n  %lav = load i32, ptr %lap\n  br label %ba\nba:\n"
        "  %la = phi i32 [ 0, %za ], [ %lav, %ga ]\n"
        "  %hb = icmp ne ptr %b, null\n  br i1 %hb, label %gb, label %zb\nzb:\n  br label %bb\ngb:\n"
        "  %lbp = getelementptr %StrictaStr, ptr %b, i32 0, i32 0\n  %lbv = load i32, ptr %lbp\n  br label %bb\nbb:\n"
        "  %lb = phi i32 [ 0, %zb ], [ %lbv, %gb ]\n"
        "  %tot = add i32 %la, %lb\n  %ssz = ptrtoint ptr getelementptr (%StrictaStr, ptr null, i32 1) to i64\n"
        "  %st = call ptr @stricta_alloc(i64 %ssz)\n"
        "  %p0 = getelementptr %StrictaStr, ptr %st, i32 0, i32 0\n  store i32 %tot, ptr %p0\n"
        "  %p1 = getelementptr %StrictaStr, ptr %st, i32 0, i32 1\n  store i32 0, ptr %p1\n"
        "  %p3 = getelementptr %StrictaStr, ptr %st, i32 0, i32 3\n  store i32 0, ptr %p3\n"
        "  %tot64 = sext i32 %tot to i64\n  %asz = add i64 %tot64, 1\n"
        "  %ch = call ptr @stricta_alloc(i64 %asz)\n"
        "  %p2 = getelementptr %StrictaStr, ptr %st, i32 0, i32 2\n  store ptr %ch, ptr %p2\n"
        "  %la64 = sext i32 %la to i64\n  %ca = icmp sgt i32 %la, 0\n  br i1 %ca, label %cpa, label %chkb\ncpa:\n"
        "  %acp = getelementptr %StrictaStr, ptr %a, i32 0, i32 2\n  %ach = load ptr, ptr %acp\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %ch, ptr %ach, i64 %la64, i1 false)\n  br label %chkb\nchkb:\n"
        "  %cb = icmp sgt i32 %lb, 0\n  br i1 %cb, label %cpb, label %nul\ncpb:\n"
        "  %bcp = getelementptr %StrictaStr, ptr %b, i32 0, i32 2\n  %bch = load ptr, ptr %bcp\n"
        "  %dstb = getelementptr i8, ptr %ch, i64 %la64\n  %lb64 = sext i32 %lb to i64\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %dstb, ptr %bch, i64 %lb64, i1 false)\n  br label %nul\nnul:\n"
        "  %end = getelementptr i8, ptr %ch, i64 %tot64\n  store i8 0, ptr %end\n  ret ptr %st\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_repeat(ptr %s, i64 %cnt) {\nentry:\n"
        "  %hs = icmp ne ptr %s, null\n  %cpos = icmp sgt i64 %cnt, 0\n  %ok1 = and i1 %hs, %cpos\n"
        "  br i1 %ok1, label %chklen, label %empt\nchklen:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %lpos = icmp sgt i32 %len, 0\n  br i1 %lpos, label %mk, label %empt\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nmk:\n"
        "  %len64 = sext i32 %len to i64\n  %tot = mul i64 %len64, %cnt\n"
        "  %ssz = ptrtoint ptr getelementptr (%StrictaStr, ptr null, i32 1) to i64\n"
        "  %st = call ptr @stricta_alloc(i64 %ssz)\n"
        "  %tot32 = trunc i64 %tot to i32\n"
        "  %p0 = getelementptr %StrictaStr, ptr %st, i32 0, i32 0\n  store i32 %tot32, ptr %p0\n"
        "  %p1 = getelementptr %StrictaStr, ptr %st, i32 0, i32 1\n  store i32 0, ptr %p1\n"
        "  %p3 = getelementptr %StrictaStr, ptr %st, i32 0, i32 3\n  store i32 0, ptr %p3\n"
        "  %asz = add i64 %tot, 1\n  %ch = call ptr @stricta_alloc(i64 %asz)\n"
        "  %p2 = getelementptr %StrictaStr, ptr %st, i32 0, i32 2\n  store ptr %ch, ptr %p2\n"
        "  %scp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %sch = load ptr, ptr %scp\n"
        "  %ip = alloca i64\n  store i64 0, ptr %ip\n  br label %loop\nloop:\n"
        "  %i = load i64, ptr %ip\n  %cond = icmp slt i64 %i, %cnt\n"
        "  br i1 %cond, label %body, label %done\nbody:\n"
        "  %off = mul i64 %i, %len64\n  %dp = getelementptr i8, ptr %ch, i64 %off\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %dp, ptr %sch, i64 %len64, i1 false)\n"
        "  %ni = add i64 %i, 1\n  store i64 %ni, ptr %ip\n  br label %loop\ndone:\n"
        "  %end = getelementptr i8, ptr %ch, i64 %tot\n  store i8 0, ptr %end\n  ret ptr %st\n}\n\n");
    sb_append(b,
        "define internal i64 @stricta_slice_adjust(i64 %len, i64 %start, i32 %has_start, i64 %stop, i32 %has_stop, i64 %step, ptr %out_start) {\nentry:\n"
        "  %z = icmp eq i64 %step, 0\n  br i1 %z, label %bad, label %st0\nbad:\n"
        "  call void @stricta_set_exc(i32 1, ptr null, ptr @.rt_err_step)\n"
        "  store i64 0, ptr %out_start\n  ret i64 -1\nst0:\n"
        "  %sneg = icmp slt i64 %step, 0\n  %hs = icmp ne i32 %has_start, 0\n"
        "  br i1 %hs, label %adj_st, label %def_st\ndef_st:\n"
        "  %lm1 = sub i64 %len, 1\n  %ds = select i1 %sneg, i64 %lm1, i64 0\n  br label %st_ok\nadj_st:\n"
        "  %sn = icmp slt i64 %start, 0\n  br i1 %sn, label %st_lt0, label %st_ge0\nst_lt0:\n"
        "  %sa = add i64 %start, %len\n  %sa0 = icmp slt i64 %sa, 0\n"
        "  %cl1 = select i1 %sneg, i64 -1, i64 0\n"
        "  %r1 = select i1 %sa0, i64 %cl1, i64 %sa\n  br label %st_ok\nst_ge0:\n"
        "  %sge = icmp sge i64 %start, %len\n"
        "  %lm1b = sub i64 %len, 1\n  %cl2 = select i1 %sneg, i64 %lm1b, i64 %len\n"
        "  %r2 = select i1 %sge, i64 %cl2, i64 %start\n  br label %st_ok\nst_ok:\n"
        "  %fs = phi i64 [ %ds, %def_st ], [ %r1, %st_lt0 ], [ %r2, %st_ge0 ]\n"
        "  %hsp = icmp ne i32 %has_stop, 0\n  br i1 %hsp, label %adj_sp, label %def_sp\ndef_sp:\n"
        "  %dsp = select i1 %sneg, i64 -1, i64 %len\n  br label %sp_ok\nadj_sp:\n"
        "  %spn = icmp slt i64 %stop, 0\n  br i1 %spn, label %sp_lt0, label %sp_ge0\nsp_lt0:\n"
        "  %spa = add i64 %stop, %len\n  %spa0 = icmp slt i64 %spa, 0\n"
        "  %spcl1 = select i1 %sneg, i64 -1, i64 0\n"
        "  %spr1 = select i1 %spa0, i64 %spcl1, i64 %spa\n  br label %sp_ok\nsp_ge0:\n"
        "  %spge = icmp sge i64 %stop, %len\n"
        "  %lm1c = sub i64 %len, 1\n  %spcl2 = select i1 %sneg, i64 %lm1c, i64 %len\n"
        "  %spr2 = select i1 %spge, i64 %spcl2, i64 %stop\n  br label %sp_ok\nsp_ok:\n"
        "  %fsp = phi i64 [ %dsp, %def_sp ], [ %spr1, %sp_lt0 ], [ %spr2, %sp_ge0 ]\n"
        "  store i64 %fs, ptr %out_start\n"
        "  %spos = icmp sgt i64 %step, 0\n  br i1 %spos, label %pos, label %neg\npos:\n"
        "  %le = icmp sle i64 %fsp, %fs\n  br i1 %le, label %ret0, label %pcalc\npcalc:\n"
        "  %span1 = sub i64 %fsp, %fs\n  %nm1 = add i64 %span1, %step\n  %nm2 = sub i64 %nm1, 1\n"
        "  %q1 = udiv i64 %nm2, %step\n  ret i64 %q1\nneg:\n"
        "  %le2 = icmp sle i64 %fs, %fsp\n  br i1 %le2, label %ret0, label %ncalc\nncalc:\n"
        "  %span2 = sub i64 %fs, %fsp\n  %sp1 = add i64 %step, 1\n  %nsp1 = sub i64 0, %sp1\n"
        "  %ustep = add i64 %nsp1, 1\n  %nn1 = add i64 %span2, %ustep\n  %nn2 = sub i64 %nn1, 1\n"
        "  %q2 = udiv i64 %nn2, %ustep\n  ret i64 %q2\nret0:\n  ret i64 0\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_slice(ptr %s, i64 %start, i64 %end) {\nentry:\n"
        "  %hs = icmp ne ptr %s, null\n  br i1 %hs, label %work, label %empt\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nwork:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %l32 = load i32, ptr %lp\n"
        "  %len = sext i32 %l32 to i64\n"
        "  %sn = icmp slt i64 %start, 0\n  %s1 = add i64 %start, %len\n  %s2 = select i1 %sn, i64 %s1, i64 %start\n"
        "  %sl0 = icmp slt i64 %s2, 0\n  %fs = select i1 %sl0, i64 0, i64 %s2\n"
        "  %en = icmp slt i64 %end, 0\n  %e1 = add i64 %end, %len\n  %e2 = select i1 %en, i64 %e1, i64 %end\n"
        "  %egl = icmp sgt i64 %e2, %len\n  %fe = select i1 %egl, i64 %len, i64 %e2\n"
        "  %bad = icmp sge i64 %fs, %fe\n  br i1 %bad, label %empt, label %mk\nmk:\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %src = getelementptr i8, ptr %ch, i64 %fs\n  %dl = sub i64 %fe, %fs\n  %dl32 = trunc i64 %dl to i32\n"
        "  %r = call ptr @stricta_str_new(ptr %src, i32 %dl32)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_slice_step(ptr %s, i64 %start, i32 %hs, i64 %stop, i32 %hsp, i64 %step) {\nentry:\n"
        "  %cur_p = alloca i64\n  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %work, label %nulls\nnulls:\n"
        "  %d0 = call i64 @stricta_slice_adjust(i64 0, i64 %start, i32 %hs, i64 %stop, i32 %hsp, i64 %step, ptr %cur_p)\n"
        "  %e0 = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e0\nwork:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %l32 = load i32, ptr %lp\n"
        "  %len = sext i32 %l32 to i64\n"
        "  %cnt = call i64 @stricta_slice_adjust(i64 %len, i64 %start, i32 %hs, i64 %stop, i32 %hsp, i64 %step, ptr %cur_p)\n"
        "  %le0 = icmp sle i64 %cnt, 0\n  br i1 %le0, label %empt, label %chk1\nempt:\n"
        "  %e1 = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e1\nchk1:\n"
        "  %cur0 = load i64, ptr %cur_p\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %is1 = icmp eq i64 %step, 1\n  br i1 %is1, label %fast1, label %slow\nfast1:\n"
        "  %src = getelementptr i8, ptr %ch, i64 %cur0\n  %c32 = trunc i64 %cnt to i32\n"
        "  %r1 = call ptr @stricta_str_new(ptr %src, i32 %c32)\n  ret ptr %r1\nslow:\n"
        "  %ssz = ptrtoint ptr getelementptr (%StrictaStr, ptr null, i32 1) to i64\n"
        "  %st = call ptr @stricta_alloc(i64 %ssz)\n  %cnt32 = trunc i64 %cnt to i32\n"
        "  %p0 = getelementptr %StrictaStr, ptr %st, i32 0, i32 0\n  store i32 %cnt32, ptr %p0\n"
        "  %p1 = getelementptr %StrictaStr, ptr %st, i32 0, i32 1\n  store i32 0, ptr %p1\n"
        "  %p3 = getelementptr %StrictaStr, ptr %st, i32 0, i32 3\n  store i32 0, ptr %p3\n"
        "  %asz = add i64 %cnt, 1\n  %och = call ptr @stricta_alloc(i64 %asz)\n"
        "  %p2 = getelementptr %StrictaStr, ptr %st, i32 0, i32 2\n  store ptr %och, ptr %p2\n"
        "  %ip = alloca i64\n  store i64 0, ptr %ip\n  br label %loop\nloop:\n"
        "  %i = load i64, ptr %ip\n  %cond = icmp slt i64 %i, %cnt\n  br i1 %cond, label %body, label %done\nbody:\n"
        "  %cur = load i64, ptr %cur_p\n  %sp = getelementptr i8, ptr %ch, i64 %cur\n  %bval = load i8, ptr %sp\n"
        "  %dp = getelementptr i8, ptr %och, i64 %i\n  store i8 %bval, ptr %dp\n"
        "  %ncur = add i64 %cur, %step\n  store i64 %ncur, ptr %cur_p\n"
        "  %ni = add i64 %i, 1\n  store i64 %ni, ptr %ip\n  br label %loop\ndone:\n"
        "  %end = getelementptr i8, ptr %och, i64 %cnt\n  store i8 0, ptr %end\n  ret ptr %st\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_char_at(ptr %s, i64 %idx) {\nentry:\n"
        "  %hs = icmp ne ptr %s, null\n  br i1 %hs, label %chk, label %fail0\nfail0:\n"
        "  call void @stricta_bounds_fail(i64 %idx, i64 0, ptr @.rt_str_ctx, i32 0)\n  unreachable\nchk:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %l32 = load i32, ptr %lp\n"
        "  %z = icmp eq i32 %l32, 0\n  br i1 %z, label %fail0, label %adj\nadj:\n"
        "  %len = sext i32 %l32 to i64\n  %neg = icmp slt i64 %idx, 0\n"
        "  %ai = add i64 %idx, %len\n  %fi = select i1 %neg, i64 %ai, i64 %idx\n"
        "  %b1 = icmp slt i64 %fi, 0\n  %b2 = icmp sge i64 %fi, %len\n  %bad = or i1 %b1, %b2\n"
        "  br i1 %bad, label %failn, label %ok\nfailn:\n"
        "  call void @stricta_bounds_fail(i64 %fi, i64 %len, ptr @.rt_str_ctx, i32 0)\n  unreachable\nok:\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %sp = getelementptr i8, ptr %ch, i64 %fi\n"
        "  %r = call ptr @stricta_str_new(ptr %sp, i32 1)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal i32 @stricta_str_eq(ptr %a, ptr %b) {\nentry:\n"
        "  %same = icmp eq ptr %a, %b\n  br i1 %same, label %ret1, label %chkn\nchkn:\n"
        "  %na = icmp eq ptr %a, null\n  %nb = icmp eq ptr %b, null\n  %any = or i1 %na, %nb\n"
        "  br i1 %any, label %ret0, label %chkl\nchkl:\n"
        "  %lap = getelementptr %StrictaStr, ptr %a, i32 0, i32 0\n  %la = load i32, ptr %lap\n"
        "  %lbp = getelementptr %StrictaStr, ptr %b, i32 0, i32 0\n  %lb = load i32, ptr %lbp\n"
        "  %leq = icmp eq i32 %la, %lb\n  br i1 %leq, label %chkh, label %ret0\nchkh:\n"
        "  %hap = getelementptr %StrictaStr, ptr %a, i32 0, i32 1\n  %ha = load i32, ptr %hap\n"
        "  %hbp = getelementptr %StrictaStr, ptr %b, i32 0, i32 1\n  %hb = load i32, ptr %hbp\n"
        "  %ha0 = icmp ne i32 %ha, 0\n  %hb0 = icmp ne i32 %hb, 0\n  %hneq = icmp ne i32 %ha, %hb\n"
        "  %hbad1 = and i1 %ha0, %hb0\n  %hbad = and i1 %hbad1, %hneq\n"
        "  br i1 %hbad, label %ret0, label %docmp\ndocmp:\n"
        "  %acp = getelementptr %StrictaStr, ptr %a, i32 0, i32 2\n  %ac = load ptr, ptr %acp\n"
        "  %bcp = getelementptr %StrictaStr, ptr %b, i32 0, i32 2\n  %bc = load ptr, ptr %bcp\n"
        "  %l64 = sext i32 %la to i64\n  %mc = call i32 @memcmp(ptr %ac, ptr %bc, i64 %l64)\n"
        "  %eq = icmp eq i32 %mc, 0\n  %r = zext i1 %eq to i32\n  ret i32 %r\nret1:\n  ret i32 1\nret0:\n  ret i32 0\n}\n\n");
    sb_append(b,
        "define internal i32 @stricta_str_cmp(ptr %a, ptr %b) {\nentry:\n"
        "  %same = icmp eq ptr %a, %b\n  br i1 %same, label %ret0, label %c1\nc1:\n"
        "  %na = icmp eq ptr %a, null\n  br i1 %na, label %retm1, label %c2\nc2:\n"
        "  %nb = icmp eq ptr %b, null\n  br i1 %nb, label %ret1, label %work\nwork:\n"
        "  %lap = getelementptr %StrictaStr, ptr %a, i32 0, i32 0\n  %la = load i32, ptr %lap\n"
        "  %lbp = getelementptr %StrictaStr, ptr %b, i32 0, i32 0\n  %lb = load i32, ptr %lbp\n"
        "  %minl = call i32 @llvm.smin.i32(i32 %la, i32 %lb)\n"
        "  %min64 = sext i32 %minl to i64\n"
        "  %acp = getelementptr %StrictaStr, ptr %a, i32 0, i32 2\n  %ac = load ptr, ptr %acp\n"
        "  %bcp = getelementptr %StrictaStr, ptr %b, i32 0, i32 2\n  %bc = load ptr, ptr %bcp\n"
        "  %mc = call i32 @memcmp(ptr %ac, ptr %bc, i64 %min64)\n"
        "  %nz = icmp ne i32 %mc, 0\n  br i1 %nz, label %retmc, label %cmplen\nretmc:\n  ret i32 %mc\ncmplen:\n"
        "  %lt = icmp slt i32 %la, %lb\n  br i1 %lt, label %retm1, label %gtchk\ngtchk:\n"
        "  %gt = icmp sgt i32 %la, %lb\n  br i1 %gt, label %ret1, label %ret0\nretm1:\n  ret i32 -1\nret1:\n  ret i32 1\nret0:\n  ret i32 0\n}\n\n");
    sb_append(b,
        "define internal i64 @stricta_str_hash(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %chk, label %ret0\nret0:\n  ret i64 0\nchk:\n"
        "  %hp = getelementptr %StrictaStr, ptr %s, i32 0, i32 1\n  %h0 = load i32, ptr %hp\n"
        "  %hz = icmp eq i32 %h0, 0\n  br i1 %hz, label %calc, label %fin\ncalc:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %ip = alloca i32\n  %hv = alloca i32\n"
        "  store i32 0, ptr %ip\n  store i32 -2128831035, ptr %hv\n  br label %loop\nloop:\n"
        "  %i = load i32, ptr %ip\n  %cond = icmp slt i32 %i, %len\n  br i1 %cond, label %body, label %lout\nbody:\n"
        "  %gp = getelementptr i8, ptr %ch, i32 %i\n  %b8 = load i8, ptr %gp\n  %b32 = zext i8 %b8 to i32\n"
        "  %hc = load i32, ptr %hv\n  %hx = xor i32 %hc, %b32\n  %hm = mul i32 %hx, 16777619\n"
        "  store i32 %hm, ptr %hv\n  %ni = add i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %loop\nlout:\n"
        "  %hf = load i32, ptr %hv\n  %is0 = icmp eq i32 %hf, 0\n  %h1 = select i1 %is0, i32 1, i32 %hf\n"
        "  %sp = getelementptr %StrictaStr, ptr %s, i32 0, i32 3\n  %stat = load i32, ptr %sp\n"
        "  %dyn = icmp eq i32 %stat, 0\n  br i1 %dyn, label %saveh, label %fin\nsaveh:\n"
        "  store i32 %h1, ptr %hp\n  br label %fin\nfin:\n"
        "  %fh = phi i32 [ %h0, %chk ], [ %h1, %lout ], [ %h1, %saveh ]\n"
        "  %x0 = zext i32 %fh to i64\n"
        "  %x1 = lshr i64 %x0, 16\n  %x2 = xor i64 %x0, %x1\n  %x3 = mul i64 %x2, 73244475\n"
        "  %x4 = lshr i64 %x3, 16\n  %x5 = xor i64 %x3, %x4\n  ret i64 %x5\n}\n\n");
    sb_append(b,
        "define internal i32 @stricta_str_contains(ptr %sub, ptr %s) {\nentry:\n"
        "  %ns = icmp eq ptr %sub, null\n  %nt = icmp eq ptr %s, null\n  %bad = or i1 %ns, %nt\n"
        "  br i1 %bad, label %ret0, label %chkl\nchkl:\n"
        "  %lp1 = getelementptr %StrictaStr, ptr %sub, i32 0, i32 0\n  %l1 = load i32, ptr %lp1\n"
        "  %z1 = icmp eq i32 %l1, 0\n  br i1 %z1, label %ret1, label %chkl2\nchkl2:\n"
        "  %lp2 = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %l2 = load i32, ptr %lp2\n"
        "  %gt = icmp sgt i32 %l1, %l2\n  br i1 %gt, label %ret0, label %find\nfind:\n"
        "  %cp1 = getelementptr %StrictaStr, ptr %sub, i32 0, i32 2\n  %c1 = load ptr, ptr %cp1\n"
        "  %cp2 = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %c2 = load ptr, ptr %cp2\n"
        "  %hit = call ptr @strstr(ptr %c2, ptr %c1)\n"
        "  %ok = icmp ne ptr %hit, null\n  %r = zext i1 %ok to i32\n  ret i32 %r\nret1:\n  ret i32 1\nret0:\n  ret i32 0\n}\n\n");
    sb_append(b,
        "define internal void @_rt_split_push(ptr %lst, ptr %item) {\nentry:\n"
        "  %lp = getelementptr %StrictaPtrList, ptr %lst, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %cp = getelementptr %StrictaPtrList, ptr %lst, i32 0, i32 1\n  %cap = load i32, ptr %cp\n"
        "  %ip = getelementptr %StrictaPtrList, ptr %lst, i32 0, i32 2\n"
        "  %full = icmp sge i32 %len, %cap\n  br i1 %full, label %grow, label %put\ngrow:\n"
        "  %ncap = mul i32 %cap, 2\n  store i32 %ncap, ptr %cp\n"
        "  %arr = load ptr, ptr %ip\n  %ncap64 = sext i32 %ncap to i64\n  %nsz = mul i64 %ncap64, 8\n"
        "  %narr = call ptr @stricta_realloc(ptr %arr, i64 %nsz)\n  store ptr %narr, ptr %ip\n  br label %put\nput:\n"
        "  %farr = load ptr, ptr %ip\n  %dp = getelementptr ptr, ptr %farr, i32 %len\n  store ptr %item, ptr %dp\n"
        "  %nlen = add i32 %len, 1\n  store i32 %nlen, ptr %lp\n  ret void\n}\n\n");
    sb_append(b,
        "define internal i1 @_rt_is_space(i8 %c) alwaysinline {\nentry:\n"
        "  %s1 = icmp eq i8 %c, 32\n  %ge9 = icmp uge i8 %c, 9\n  %le13 = icmp ule i8 %c, 13\n"
        "  %s2 = and i1 %ge9, %le13\n  %r = or i1 %s1, %s2\n  ret i1 %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_split(ptr %s, ptr %sep) {\nentry:\n"
        "  %lsz = ptrtoint ptr getelementptr (%StrictaPtrList, ptr null, i32 1) to i64\n"
        "  %lst = call ptr @stricta_alloc(i64 %lsz)\n"
        "  %p0 = getelementptr %StrictaPtrList, ptr %lst, i32 0, i32 0\n  store i32 0, ptr %p0\n"
        "  %p1 = getelementptr %StrictaPtrList, ptr %lst, i32 0, i32 1\n  store i32 8, ptr %p1\n"
        "  %items = call ptr @stricta_alloc(i64 64)\n"
        "  %p2 = getelementptr %StrictaPtrList, ptr %lst, i32 0, i32 2\n  store ptr %items, ptr %p2\n"
        "  %hs = icmp ne ptr %s, null\n  br i1 %hs, label %chks, label %retl\nchks:\n"
        "  %slp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %slen = load i32, ptr %slp\n"
        "  %scp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %sch = load ptr, ptr %scp\n"
        "  %hsep = icmp ne ptr %sep, null\n  br i1 %hsep, label %wsep, label %wspace\nwspace:\n"
        "  %ip = alloca i32\n  store i32 0, ptr %ip\n  br label %wsl1\nwsl1:\n"
        "  %i0 = load i32, ptr %ip\n  %c0 = icmp slt i32 %i0, %slen\n  br i1 %c0, label %wsc1, label %retl\nwsc1:\n"
        "  %gp0 = getelementptr i8, ptr %sch, i32 %i0\n  %ch0 = load i8, ptr %gp0\n"
        "  %sp0 = call i1 @_rt_is_space(i8 %ch0)\n  br i1 %sp0, label %wsinc1, label %wsstart\nwsinc1:\n"
        "  %ni0 = add i32 %i0, 1\n  store i32 %ni0, ptr %ip\n  br label %wsl1\nwsstart:\n"
        "  %st0 = load i32, ptr %ip\n  br label %wsl2\nwsl2:\n"
        "  %i1 = load i32, ptr %ip\n  %c1 = icmp slt i32 %i1, %slen\n  br i1 %c1, label %wsc2, label %wspush\nwsc2:\n"
        "  %gp1 = getelementptr i8, ptr %sch, i32 %i1\n  %ch1 = load i8, ptr %gp1\n"
        "  %sp1 = call i1 @_rt_is_space(i8 %ch1)\n  br i1 %sp1, label %wspush, label %wsinc2\nwsinc2:\n"
        "  %ni1 = add i32 %i1, 1\n  store i32 %ni1, ptr %ip\n  br label %wsl2\nwspush:\n"
        "  %end0 = load i32, ptr %ip\n  %tlen = sub i32 %end0, %st0\n"
        "  %tsrc = getelementptr i8, ptr %sch, i32 %st0\n"
        "  %tok = call ptr @stricta_str_new(ptr %tsrc, i32 %tlen)\n"
        "  call void @_rt_split_push(ptr %lst, ptr %tok)\n  br label %wsl1\nwsep:\n"
        "  %seplp = getelementptr %StrictaStr, ptr %sep, i32 0, i32 0\n  %seplen = load i32, ptr %seplp\n"
        "  %sep0 = icmp eq i32 %seplen, 0\n  br i1 %sep0, label %badsep, label %seploop0\nbadsep:\n"
        "  call void @stricta_set_exc(i32 2, ptr null, ptr @.rt_err_sep)\n"
        "  call void @stricta_unhandled_exc()\n  unreachable\nseploop0:\n"
        "  %sepcp = getelementptr %StrictaStr, ptr %sep, i32 0, i32 2\n  %sepch = load ptr, ptr %sepcp\n"
        "  %posp = alloca i32\n  store i32 0, ptr %posp\n  br label %seploop\nseploop:\n"
        "  %pos = load i32, ptr %posp\n  %cond = icmp sle i32 %pos, %slen\n  br i1 %cond, label %sepbody, label %retl\nsepbody:\n"
        "  %hay = getelementptr i8, ptr %sch, i32 %pos\n"
        "  %hit = call ptr @strstr(ptr %hay, ptr %sepch)\n"
        "  %nf = icmp eq ptr %hit, null\n  br i1 %nf, label %septail, label %sephit\nseptail:\n"
        "  %rlen = sub i32 %slen, %pos\n"
        "  %rtok = call ptr @stricta_str_new(ptr %hay, i32 %rlen)\n"
        "  call void @_rt_split_push(ptr %lst, ptr %rtok)\n  br label %retl\nsephit:\n"
        "  %hit_i = ptrtoint ptr %hit to i64\n  %sch_i = ptrtoint ptr %sch to i64\n"
        "  %diff = sub i64 %hit_i, %sch_i\n  %end = trunc i64 %diff to i32\n"
        "  %plen = sub i32 %end, %pos\n"
        "  %ptok = call ptr @stricta_str_new(ptr %hay, i32 %plen)\n"
        "  call void @_rt_split_push(ptr %lst, ptr %ptok)\n"
        "  %npos = add i32 %end, %seplen\n  store i32 %npos, ptr %posp\n  br label %seploop\nretl:\n  ret ptr %lst\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_join(ptr %sep, ptr %list) {\nentry:\n"
        "  %hl = icmp ne ptr %list, null\n  br i1 %hl, label %chkl, label %empt\nchkl:\n"
        "  %lp = getelementptr %StrictaPtrList, ptr %list, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %z = icmp eq i32 %len, 0\n  br i1 %z, label %empt, label %work\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nwork:\n"
        "  %hsep = icmp ne ptr %sep, null\n  br i1 %hsep, label %gsep, label %zsep\nzsep:\n  br label %bsep\ngsep:\n"
        "  %slp = getelementptr %StrictaStr, ptr %sep, i32 0, i32 0\n  %slv = load i32, ptr %slp\n  %scp = getelementptr %StrictaStr, ptr %sep, i32 0, i32 2\n  %scv = load ptr, ptr %scp\n  br label %bsep\nbsep:\n"
        "  %seplen = phi i32 [ 0, %zsep ], [ %slv, %gsep ]\n"
        "  %sepch = phi ptr [ null, %zsep ], [ %scv, %gsep ]\n"
        "  %seplen64 = sext i32 %seplen to i64\n"
        "  %ip0 = getelementptr %StrictaPtrList, ptr %list, i32 0, i32 2\n  %items = load ptr, ptr %ip0\n"
        "  %totp = alloca i64\n  %ip = alloca i32\n  store i64 0, ptr %totp\n  store i32 0, ptr %ip\n  br label %p1\np1:\n"
        "  %i = load i32, ptr %ip\n  %c1 = icmp slt i32 %i, %len\n  br i1 %c1, label %p1b, label %p1d\np1b:\n"
        "  %ep = getelementptr ptr, ptr %items, i32 %i\n  %it = load ptr, ptr %ep\n"
        "  %hit = icmp ne ptr %it, null\n  br i1 %hit, label %p1add, label %p1sep\np1add:\n"
        "  %ilp = getelementptr %StrictaStr, ptr %it, i32 0, i32 0\n  %il = load i32, ptr %ilp\n"
        "  %il64 = sext i32 %il to i64\n  %t0 = load i64, ptr %totp\n  %t1 = add i64 %t0, %il64\n  store i64 %t1, ptr %totp\n  br label %p1sep\np1sep:\n"
        "  %gt0 = icmp sgt i32 %i, 0\n  br i1 %gt0, label %p1as, label %p1n\np1as:\n"
        "  %t2 = load i64, ptr %totp\n  %t3 = add i64 %t2, %seplen64\n  store i64 %t3, ptr %totp\n  br label %p1n\np1n:\n"
        "  %ni = add i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %p1\np1d:\n"
        "  %tot = load i64, ptr %totp\n"
        "  %ssz = ptrtoint ptr getelementptr (%StrictaStr, ptr null, i32 1) to i64\n"
        "  %out = call ptr @stricta_alloc(i64 %ssz)\n  %tot32 = trunc i64 %tot to i32\n"
        "  %o0 = getelementptr %StrictaStr, ptr %out, i32 0, i32 0\n  store i32 %tot32, ptr %o0\n"
        "  %o1 = getelementptr %StrictaStr, ptr %out, i32 0, i32 1\n  store i32 0, ptr %o1\n"
        "  %o3 = getelementptr %StrictaStr, ptr %out, i32 0, i32 3\n  store i32 0, ptr %o3\n"
        "  %asz = add i64 %tot, 1\n  %och = call ptr @stricta_alloc(i64 %asz)\n"
        "  %o2 = getelementptr %StrictaStr, ptr %out, i32 0, i32 2\n  store ptr %och, ptr %o2\n"
        "  %offp = alloca i64\n  store i64 0, ptr %offp\n  store i32 0, ptr %ip\n  br label %p2\np2:\n"
        "  %j = load i32, ptr %ip\n  %c2 = icmp slt i32 %j, %len\n  br i1 %c2, label %p2b, label %p2d\np2b:\n"
        "  %jgt = icmp sgt i32 %j, 0\n  %sg = icmp sgt i32 %seplen, 0\n  %csep = and i1 %jgt, %sg\n"
        "  br i1 %csep, label %cpsep, label %cpitem\ncpsep:\n"
        "  %off0 = load i64, ptr %offp\n  %dp0 = getelementptr i8, ptr %och, i64 %off0\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %dp0, ptr %sepch, i64 %seplen64, i1 false)\n"
        "  %off1 = add i64 %off0, %seplen64\n  store i64 %off1, ptr %offp\n  br label %cpitem\ncpitem:\n"
        "  %ep2 = getelementptr ptr, ptr %items, i32 %j\n  %it2 = load ptr, ptr %ep2\n"
        "  %hit2 = icmp ne ptr %it2, null\n  br i1 %hit2, label %chkilen, label %p2n\nchkilen:\n"
        "  %ilp2 = getelementptr %StrictaStr, ptr %it2, i32 0, i32 0\n  %il2 = load i32, ptr %ilp2\n"
        "  %ig0 = icmp sgt i32 %il2, 0\n  br i1 %ig0, label %doitem, label %p2n\ndoitem:\n"
        "  %icp2 = getelementptr %StrictaStr, ptr %it2, i32 0, i32 2\n  %ich2 = load ptr, ptr %icp2\n"
        "  %il2_64 = sext i32 %il2 to i64\n  %off2 = load i64, ptr %offp\n"
        "  %dp1 = getelementptr i8, ptr %och, i64 %off2\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %dp1, ptr %ich2, i64 %il2_64, i1 false)\n"
        "  %off3 = add i64 %off2, %il2_64\n  store i64 %off3, ptr %offp\n  br label %p2n\np2n:\n"
        "  %nj = add i32 %j, 1\n  store i32 %nj, ptr %ip\n  br label %p2\np2d:\n"
        "  %fend = load i64, ptr %offp\n  %endp = getelementptr i8, ptr %och, i64 %fend\n"
        "  store i8 0, ptr %endp\n  ret ptr %out\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_upper(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %work, label %empt\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nwork:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %out = call ptr @stricta_str_new(ptr %ch, i32 %len)\n"
        "  %ocp = getelementptr %StrictaStr, ptr %out, i32 0, i32 2\n  %och = load ptr, ptr %ocp\n"
        "  %ip = alloca i32\n  store i32 0, ptr %ip\n  br label %loop\nloop:\n"
        "  %i = load i32, ptr %ip\n  %cond = icmp slt i32 %i, %len\n  br i1 %cond, label %body, label %done\nbody:\n"
        "  %gp = getelementptr i8, ptr %och, i32 %i\n  %c = load i8, ptr %gp\n"
        "  %ge = icmp uge i8 %c, 97\n  %le = icmp ule i8 %c, 122\n  %islo = and i1 %ge, %le\n"
        "  %up = sub i8 %c, 32\n  %nc = select i1 %islo, i8 %up, i8 %c\n"
        "  store i8 %nc, ptr %gp\n  %ni = add i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %loop\ndone:\n  ret ptr %out\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_lower(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %work, label %empt\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nwork:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %out = call ptr @stricta_str_new(ptr %ch, i32 %len)\n"
        "  %ocp = getelementptr %StrictaStr, ptr %out, i32 0, i32 2\n  %och = load ptr, ptr %ocp\n"
        "  %ip = alloca i32\n  store i32 0, ptr %ip\n  br label %loop\nloop:\n"
        "  %i = load i32, ptr %ip\n  %cond = icmp slt i32 %i, %len\n  br i1 %cond, label %body, label %done\nbody:\n"
        "  %gp = getelementptr i8, ptr %och, i32 %i\n  %c = load i8, ptr %gp\n"
        "  %ge = icmp uge i8 %c, 65\n  %le = icmp ule i8 %c, 90\n  %isup = and i1 %ge, %le\n"
        "  %lo = add i8 %c, 32\n  %nc = select i1 %isup, i8 %lo, i8 %c\n"
        "  store i8 %nc, ptr %gp\n  %ni = add i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %loop\ndone:\n  ret ptr %out\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_replace(ptr %s, ptr %olds, ptr %news) {\nentry:\n"
        "  %hs = icmp ne ptr %s, null\n  %ho = icmp ne ptr %olds, null\n  %hn = icmp ne ptr %news, null\n"
        "  %a1 = and i1 %hs, %ho\n  %a2 = and i1 %a1, %hn\n  br i1 %a2, label %chkol, label %fallback\nchkol:\n"
        "  %olp = getelementptr %StrictaStr, ptr %olds, i32 0, i32 0\n  %olen = load i32, ptr %olp\n"
        "  %og0 = icmp sgt i32 %olen, 0\n  br i1 %og0, label %count0, label %fallback\nfallback:\n"
        "  br i1 %hs, label %cl, label %empt\ncl:\n"
        "  %rc = call ptr @stricta_str_clone(ptr %s)\n  ret ptr %rc\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\ncount0:\n"
        "  %scp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %sch = load ptr, ptr %scp\n"
        "  %ocp = getelementptr %StrictaStr, ptr %olds, i32 0, i32 2\n  %och = load ptr, ptr %ocp\n"
        "  %olen64 = sext i32 %olen to i64\n"
        "  %cntp = alloca i64\n  %pp = alloca ptr\n"
        "  store i64 0, ptr %cntp\n  store ptr %sch, ptr %pp\n  br label %cloop\ncloop:\n"
        "  %cp0 = load ptr, ptr %pp\n  %hit0 = call ptr @strstr(ptr %cp0, ptr %och)\n"
        "  %fnd0 = icmp ne ptr %hit0, null\n  br i1 %fnd0, label %chit, label %cdone\nchit:\n"
        "  %c0 = load i64, ptr %cntp\n  %c1 = add i64 %c0, 1\n  store i64 %c1, ptr %cntp\n"
        "  %np0 = getelementptr i8, ptr %hit0, i64 %olen64\n  store ptr %np0, ptr %pp\n  br label %cloop\ncdone:\n"
        "  %cnt = load i64, ptr %cntp\n  %zcnt = icmp eq i64 %cnt, 0\n  br i1 %zcnt, label %cl, label %alloc\nalloc:\n"
        "  %slp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %slen = load i32, ptr %slp\n"
        "  %nlp = getelementptr %StrictaStr, ptr %news, i32 0, i32 0\n  %nlen = load i32, ptr %nlp\n"
        "  %ncp = getelementptr %StrictaStr, ptr %news, i32 0, i32 2\n  %nch = load ptr, ptr %ncp\n"
        "  %slen64 = sext i32 %slen to i64\n  %nlen64 = sext i32 %nlen to i64\n"
        "  %diff = sub i64 %nlen64, %olen64\n  %delta = mul i64 %cnt, %diff\n"
        "  %tot = add i64 %slen64, %delta\n"
        "  %ssz = ptrtoint ptr getelementptr (%StrictaStr, ptr null, i32 1) to i64\n"
        "  %out = call ptr @stricta_alloc(i64 %ssz)\n  %tot32 = trunc i64 %tot to i32\n"
        "  %o0 = getelementptr %StrictaStr, ptr %out, i32 0, i32 0\n  store i32 %tot32, ptr %o0\n"
        "  %o1 = getelementptr %StrictaStr, ptr %out, i32 0, i32 1\n  store i32 0, ptr %o1\n"
        "  %o3 = getelementptr %StrictaStr, ptr %out, i32 0, i32 3\n  store i32 0, ptr %o3\n"
        "  %asz = add i64 %tot, 1\n  %dch = call ptr @stricta_alloc(i64 %asz)\n"
        "  %o2 = getelementptr %StrictaStr, ptr %out, i32 0, i32 2\n  store ptr %dch, ptr %o2\n"
        "  %dstp = alloca ptr\n  store ptr %dch, ptr %dstp\n  store ptr %sch, ptr %pp\n  br label %rloop\nrloop:\n"
        "  %sp1 = load ptr, ptr %pp\n  %hit1 = call ptr @strstr(ptr %sp1, ptr %och)\n"
        "  %fnd1 = icmp ne ptr %hit1, null\n  br i1 %fnd1, label %rhit, label %rtail\nrhit:\n"
        "  %hi = ptrtoint ptr %hit1 to i64\n  %si = ptrtoint ptr %sp1 to i64\n"
        "  %chunk = sub i64 %hi, %si\n  %dp0 = load ptr, ptr %dstp\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %dp0, ptr %sp1, i64 %chunk, i1 false)\n"
        "  %dp1 = getelementptr i8, ptr %dp0, i64 %chunk\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %dp1, ptr %nch, i64 %nlen64, i1 false)\n"
        "  %dp2 = getelementptr i8, ptr %dp1, i64 %nlen64\n  store ptr %dp2, ptr %dstp\n"
        "  %np1 = getelementptr i8, ptr %hit1, i64 %olen64\n  store ptr %np1, ptr %pp\n  br label %rloop\nrtail:\n"
        "  %sp2 = load ptr, ptr %pp\n  %rem = call i64 @strlen(ptr %sp2)\n  %dp3 = load ptr, ptr %dstp\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %dp3, ptr %sp2, i64 %rem, i1 false)\n"
        "  %dp4 = getelementptr i8, ptr %dp3, i64 %rem\n  store i8 0, ptr %dp4\n  ret ptr %out\n}\n\n");
    sb_append(b,
        "define internal i64 @stricta_str_find(ptr %s, ptr %sub) {\nentry:\n"
        "  %hs = icmp ne ptr %s, null\n  %hb = icmp ne ptr %sub, null\n  %ok = and i1 %hs, %hb\n"
        "  br i1 %ok, label %chkl, label %retm1\nchkl:\n"
        "  %lp = getelementptr %StrictaStr, ptr %sub, i32 0, i32 0\n  %sl = load i32, ptr %lp\n"
        "  %z = icmp eq i32 %sl, 0\n  br i1 %z, label %ret0, label %find\nfind:\n"
        "  %scp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %sch = load ptr, ptr %scp\n"
        "  %bcp = getelementptr %StrictaStr, ptr %sub, i32 0, i32 2\n  %bch = load ptr, ptr %bcp\n"
        "  %p = call ptr @strstr(ptr %sch, ptr %bch)\n"
        "  %fnd = icmp ne ptr %p, null\n  br i1 %fnd, label %hit, label %retm1\nhit:\n"
        "  %pi = ptrtoint ptr %p to i64\n  %si = ptrtoint ptr %sch to i64\n"
        "  %d = sub i64 %pi, %si\n  ret i64 %d\nret0:\n  ret i64 0\nretm1:\n  ret i64 -1\n}\n\n");
    sb_append(b,
        "define internal i64 @stricta_str_index(ptr %s, ptr %sub) {\nentry:\n"
        "  %idx = call i64 @stricta_str_find(ptr %s, ptr %sub)\n"
        "  %bad = icmp slt i64 %idx, 0\n  br i1 %bad, label %fail, label %ok\nfail:\n"
        "  call void @stricta_set_exc(i32 2, ptr null, ptr @.rt_err_sub)\n"
        "  call void @stricta_unhandled_exc()\n  unreachable\nok:\n  ret i64 %idx\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_strip(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %work, label %empt\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nwork:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %l_p = alloca i32\n  %r_p = alloca i32\n"
        "  store i32 0, ptr %l_p\n  store i32 %len, ptr %r_p\n  br label %lloop\nlloop:\n"
        "  %l = load i32, ptr %l_p\n  %r = load i32, ptr %r_p\n  %c1 = icmp slt i32 %l, %r\n"
        "  br i1 %c1, label %lc, label %rloop\nlc:\n"
        "  %gp1 = getelementptr i8, ptr %ch, i32 %l\n  %ch1 = load i8, ptr %gp1\n"
        "  %sp1 = call i1 @_rt_is_space(i8 %ch1)\n  br i1 %sp1, label %linc, label %rloop\nlinc:\n"
        "  %nl = add i32 %l, 1\n  store i32 %nl, ptr %l_p\n  br label %lloop\nrloop:\n"
        "  %l2 = load i32, ptr %l_p\n  %r2 = load i32, ptr %r_p\n  %c2 = icmp sgt i32 %r2, %l2\n"
        "  br i1 %c2, label %rc, label %done\nrc:\n"
        "  %rm1 = sub i32 %r2, 1\n  %gp2 = getelementptr i8, ptr %ch, i32 %rm1\n  %ch2 = load i8, ptr %gp2\n"
        "  %sp2 = call i1 @_rt_is_space(i8 %ch2)\n  br i1 %sp2, label %rdec, label %done\nrdec:\n"
        "  store i32 %rm1, ptr %r_p\n  br label %rloop\ndone:\n"
        "  %lf = load i32, ptr %l_p\n  %rf = load i32, ptr %r_p\n  %nlen = sub i32 %rf, %lf\n"
        "  %src = getelementptr i8, ptr %ch, i32 %lf\n"
        "  %res = call ptr @stricta_str_new(ptr %src, i32 %nlen)\n  ret ptr %res\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_lstrip(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %work, label %empt\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nwork:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %l_p = alloca i32\n  store i32 0, ptr %l_p\n  br label %loop\nloop:\n"
        "  %l = load i32, ptr %l_p\n  %c1 = icmp slt i32 %l, %len\n  br i1 %c1, label %chk, label %done\nchk:\n"
        "  %gp = getelementptr i8, ptr %ch, i32 %l\n  %c = load i8, ptr %gp\n"
        "  %sp = call i1 @_rt_is_space(i8 %c)\n  br i1 %sp, label %inc, label %done\ninc:\n"
        "  %nl = add i32 %l, 1\n  store i32 %nl, ptr %l_p\n  br label %loop\ndone:\n"
        "  %lf = load i32, ptr %l_p\n  %nlen = sub i32 %len, %lf\n  %src = getelementptr i8, ptr %ch, i32 %lf\n"
        "  %res = call ptr @stricta_str_new(ptr %src, i32 %nlen)\n  ret ptr %res\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_str_rstrip(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %work, label %empt\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nwork:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %r_p = alloca i32\n  store i32 %len, ptr %r_p\n  br label %loop\nloop:\n"
        "  %r = load i32, ptr %r_p\n  %c1 = icmp sgt i32 %r, 0\n  br i1 %c1, label %chk, label %done\nchk:\n"
        "  %rm1 = sub i32 %r, 1\n  %gp = getelementptr i8, ptr %ch, i32 %rm1\n  %c = load i8, ptr %gp\n"
        "  %sp = call i1 @_rt_is_space(i8 %c)\n  br i1 %sp, label %dec, label %done\ndec:\n"
        "  store i32 %rm1, ptr %r_p\n  br label %loop\ndone:\n"
        "  %rf = load i32, ptr %r_p\n"
        "  %res = call ptr @stricta_str_new(ptr %ch, i32 %rf)\n  ret ptr %res\n}\n\n");
    sb_append(b,
        "define internal i32 @stricta_str_startswith(ptr %s, ptr %pref) {\nentry:\n"
        "  %hs = icmp ne ptr %s, null\n  %hp = icmp ne ptr %pref, null\n  %ok = and i1 %hs, %hp\n"
        "  br i1 %ok, label %chkl, label %ret0\nchkl:\n"
        "  %slp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %slen = load i32, ptr %slp\n"
        "  %plp = getelementptr %StrictaStr, ptr %pref, i32 0, i32 0\n  %plen = load i32, ptr %plp\n"
        "  %gt = icmp sgt i32 %plen, %slen\n  br i1 %gt, label %ret0, label %cmp\ncmp:\n"
        "  %scp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %sch = load ptr, ptr %scp\n"
        "  %pcp = getelementptr %StrictaStr, ptr %pref, i32 0, i32 2\n  %pch = load ptr, ptr %pcp\n"
        "  %p64 = sext i32 %plen to i64\n  %mc = call i32 @memcmp(ptr %sch, ptr %pch, i64 %p64)\n"
        "  %eq = icmp eq i32 %mc, 0\n  %r = zext i1 %eq to i32\n  ret i32 %r\nret0:\n  ret i32 0\n}\n\n");
    sb_append(b,
        "define internal i32 @stricta_str_endswith(ptr %s, ptr %suf) {\nentry:\n"
        "  %hs = icmp ne ptr %s, null\n  %hp = icmp ne ptr %suf, null\n  %ok = and i1 %hs, %hp\n"
        "  br i1 %ok, label %chkl, label %ret0\nchkl:\n"
        "  %slp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %slen = load i32, ptr %slp\n"
        "  %ulp = getelementptr %StrictaStr, ptr %suf, i32 0, i32 0\n  %ulen = load i32, ptr %ulp\n"
        "  %gt = icmp sgt i32 %ulen, %slen\n  br i1 %gt, label %ret0, label %cmp\ncmp:\n"
        "  %scp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %sch = load ptr, ptr %scp\n"
        "  %ucp = getelementptr %StrictaStr, ptr %suf, i32 0, i32 2\n  %uch = load ptr, ptr %ucp\n"
        "  %off = sub i32 %slen, %ulen\n  %off64 = sext i32 %off to i64\n"
        "  %sp = getelementptr i8, ptr %sch, i64 %off64\n  %u64 = sext i32 %ulen to i64\n"
        "  %mc = call i32 @memcmp(ptr %sp, ptr %uch, i64 %u64)\n"
        "  %eq = icmp eq i32 %mc, 0\n  %r = zext i1 %eq to i32\n  ret i32 %r\nret0:\n  ret i32 0\n}\n\n");
    sb_append(b,
        "define internal i64 @stricta_str_count(ptr %s, ptr %sub) {\nentry:\n"
        "  %hs = icmp ne ptr %s, null\n  %hb = icmp ne ptr %sub, null\n  %ok = and i1 %hs, %hb\n"
        "  br i1 %ok, label %chkl, label %ret0\nret0:\n  ret i64 0\nchkl:\n"
        "  %slp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %slen = load i32, ptr %slp\n"
        "  %ulp = getelementptr %StrictaStr, ptr %sub, i32 0, i32 0\n  %ulen = load i32, ptr %ulp\n"
        "  %z = icmp eq i32 %ulen, 0\n  br i1 %z, label %ret_empty, label %work\nret_empty:\n"
        "  %s64 = sext i32 %slen to i64\n  %sp1 = add i64 %s64, 1\n  ret i64 %sp1\nwork:\n"
        "  %scp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %sch = load ptr, ptr %scp\n"
        "  %ucp = getelementptr %StrictaStr, ptr %sub, i32 0, i32 2\n  %uch = load ptr, ptr %ucp\n"
        "  %u64 = sext i32 %ulen to i64\n  %cntp = alloca i64\n  %pp = alloca ptr\n"
        "  store i64 0, ptr %cntp\n  store ptr %sch, ptr %pp\n  br label %loop\nloop:\n"
        "  %p = load ptr, ptr %pp\n  %hit = call ptr @strstr(ptr %p, ptr %uch)\n"
        "  %fnd = icmp ne ptr %hit, null\n  br i1 %fnd, label %inc, label %done\ninc:\n"
        "  %c = load i64, ptr %cntp\n  %nc = add i64 %c, 1\n  store i64 %nc, ptr %cntp\n"
        "  %np = getelementptr i8, ptr %hit, i64 %u64\n  store ptr %np, ptr %pp\n  br label %loop\ndone:\n"
        "  %res = load i64, ptr %cntp\n  ret i64 %res\n}\n\n");

    /* Conversions */
    sb_append(b,
        "define internal ptr @stricta_int_to_str(i64 %v) {\nentry:\n"
        "  %buf = alloca [32 x i8]\n"
        "  %n = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 32, ptr @.rt_fmt_lld, i64 %v)\n"
        "  %r = call ptr @stricta_str_new(ptr %buf, i32 %n)\n  ret ptr %r\n}\n\n"
        "define internal ptr @stricta_uint_to_str(i64 %v) {\nentry:\n"
        "  %buf = alloca [32 x i8]\n"
        "  %n = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 32, ptr @.rt_fmt_llu, i64 %v)\n"
        "  %r = call ptr @stricta_str_new(ptr %buf, i32 %n)\n  ret ptr %r\n}\n\n"
        "define internal ptr @stricta_bool_to_str(i32 %v) {\nentry:\n"
        "  %b = icmp ne i32 %v, 0\n  %s = select i1 %b, ptr @.rt_true, ptr @.rt_false\n"
        "  %r = call ptr @stricta_str_cstr(ptr %s)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_float_to_str(double %v) {\nentry:\n"
        "  %buf = alloca [64 x i8]\n  %np = alloca i32\n"
        "  %n = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 64, ptr @.rt_fmt_g, double %v)\n"
        "  store i32 %n, ptr %np\n"
        "  %p1 = call ptr @strchr(ptr %buf, i32 46)\n  %b1 = icmp ne ptr %p1, null\n  br i1 %b1, label %out, label %c2\nc2:\n"
        "  %p2 = call ptr @strchr(ptr %buf, i32 101)\n  %b2 = icmp ne ptr %p2, null\n  br i1 %b2, label %out, label %c3\nc3:\n"
        "  %p3 = call ptr @strchr(ptr %buf, i32 69)\n  %b3 = icmp ne ptr %p3, null\n  br i1 %b3, label %out, label %c4\nc4:\n"
        "  %p4 = call ptr @strchr(ptr %buf, i32 110)\n  %b4 = icmp ne ptr %p4, null\n  br i1 %b4, label %out, label %c5\nc5:\n"
        "  %p5 = call ptr @strchr(ptr %buf, i32 105)\n  %b5 = icmp ne ptr %p5, null\n  br i1 %b5, label %out, label %app\napp:\n"
        "  %n2 = add i32 %n, 2\n  %fit = icmp slt i32 %n2, 64\n  br i1 %fit, label %doapp, label %out\ndoapp:\n"
        "  %g0 = getelementptr i8, ptr %buf, i32 %n\n  store i8 46, ptr %g0\n"
        "  %n1 = add i32 %n, 1\n  %g1 = getelementptr i8, ptr %buf, i32 %n1\n  store i8 48, ptr %g1\n"
        "  %g2 = getelementptr i8, ptr %buf, i32 %n2\n  store i8 0, ptr %g2\n"
        "  store i32 %n2, ptr %np\n  br label %out\nout:\n"
        "  %fn = load i32, ptr %np\n  %r = call ptr @stricta_str_new(ptr %buf, i32 %fn)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_float_fmt(double %v, i32 %prec) {\nentry:\n"
        "  %buf = alloca [128 x i8]\n"
        "  %lt0 = icmp slt i32 %prec, 0\n  %p1 = select i1 %lt0, i32 6, i32 %prec\n"
        "  %gt50 = icmp sgt i32 %p1, 50\n  %p2 = select i1 %gt50, i32 50, i32 %p1\n"
        "  %n = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 128, ptr @.rt_fmt_ffmt, i32 %p2, double %v)\n"
        "  %r = call ptr @stricta_str_new(ptr %buf, i32 %n)\n  ret ptr %r\n}\n\n"
        "define internal i64 @stricta_str_to_int(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %work, label %ret0\nret0:\n  ret i64 0\nwork:\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %r = call i64 @strtoll(ptr %ch, ptr null, i32 10)\n  ret i64 %r\n}\n\n"
        "define internal double @stricta_str_to_float(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %work, label %ret0\nret0:\n  ret double 0.0\nwork:\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %r = call double @strtod(ptr %ch, ptr null)\n  ret double %r\n}\n\n"
        "define internal i64 @stricta_ord(ptr %s) {\nentry:\n"
        "  %ok = icmp ne ptr %s, null\n  br i1 %ok, label %chk, label %ret0\nchk:\n"
        "  %lp = getelementptr %StrictaStr, ptr %s, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %z = icmp eq i32 %len, 0\n  br i1 %z, label %ret0, label %get\nget:\n"
        "  %cp = getelementptr %StrictaStr, ptr %s, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %b = load i8, ptr %ch\n  %r = zext i8 %b to i64\n  ret i64 %r\nret0:\n  ret i64 0\n}\n\n"
        "define internal ptr @stricta_chr(i64 %code) {\nentry:\n"
        "  %buf = alloca [2 x i8]\n  %c = trunc i64 %code to i8\n"
        "  store i8 %c, ptr %buf\n  %g1 = getelementptr i8, ptr %buf, i32 1\n  store i8 0, ptr %g1\n"
        "  %r = call ptr @stricta_str_new(ptr %buf, i32 1)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_hex(i64 %v) {\nentry:\n"
        "  %buf = alloca [32 x i8]\n  %neg = icmp slt i64 %v, 0\n  br i1 %neg, label %isneg, label %ispos\nisneg:\n"
        "  %nv = sub i64 0, %v\n"
        "  %n1 = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 32, ptr @.rt_fmt_nhex, i64 %nv)\n"
        "  %r1 = call ptr @stricta_str_new(ptr %buf, i32 %n1)\n  ret ptr %r1\nispos:\n"
        "  %n2 = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 32, ptr @.rt_fmt_hex, i64 %v)\n"
        "  %r2 = call ptr @stricta_str_new(ptr %buf, i32 %n2)\n  ret ptr %r2\n}\n\n"
        "define internal ptr @stricta_oct(i64 %v) {\nentry:\n"
        "  %buf = alloca [32 x i8]\n  %neg = icmp slt i64 %v, 0\n  br i1 %neg, label %isneg, label %ispos\nisneg:\n"
        "  %nv = sub i64 0, %v\n"
        "  %n1 = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 32, ptr @.rt_fmt_noct, i64 %nv)\n"
        "  %r1 = call ptr @stricta_str_new(ptr %buf, i32 %n1)\n  ret ptr %r1\nispos:\n"
        "  %n2 = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 32, ptr @.rt_fmt_oct, i64 %v)\n"
        "  %r2 = call ptr @stricta_str_new(ptr %buf, i32 %n2)\n  ret ptr %r2\n}\n\n"
        "define internal ptr @stricta_bin(i64 %v) {\nentry:\n"
        "  %buf = alloca [80 x i8]\n  %posp = alloca i32\n  store i32 79, ptr %posp\n"
        "  %g79 = getelementptr i8, ptr %buf, i32 79\n  store i8 0, ptr %g79\n"
        "  %neg = icmp slt i64 %v, 0\n  %nv = sub i64 0, %v\n  %uv0 = select i1 %neg, i64 %nv, i64 %v\n"
        "  %uvp = alloca i64\n  store i64 %uv0, ptr %uvp\n"
        "  %isz = icmp eq i64 %uv0, 0\n  br i1 %isz, label %put0, label %loop\nput0:\n"
        "  store i32 78, ptr %posp\n  %g78 = getelementptr i8, ptr %buf, i32 78\n  store i8 48, ptr %g78\n  br label %pref\nloop:\n"
        "  %uv = load i64, ptr %uvp\n  %gt0 = icmp ugt i64 %uv, 0\n  br i1 %gt0, label %body, label %pref\nbody:\n"
        "  %bit = and i64 %uv, 1\n  %dch = add i64 %bit, 48\n  %d8 = trunc i64 %dch to i8\n"
        "  %p0 = load i32, ptr %posp\n  %p1 = sub i32 %p0, 1\n  store i32 %p1, ptr %posp\n"
        "  %gp = getelementptr i8, ptr %buf, i32 %p1\n  store i8 %d8, ptr %gp\n"
        "  %nuv = lshr i64 %uv, 1\n  store i64 %nuv, ptr %uvp\n  br label %loop\npref:\n"
        "  %p2 = load i32, ptr %posp\n  %p3 = sub i32 %p2, 1\n  %g_b = getelementptr i8, ptr %buf, i32 %p3\n  store i8 98, ptr %g_b\n"
        "  %p4 = sub i32 %p3, 1\n  %g_0 = getelementptr i8, ptr %buf, i32 %p4\n  store i8 48, ptr %g_0\n"
        "  store i32 %p4, ptr %posp\n"
        "  br i1 %neg, label %putm, label %fin\nputm:\n"
        "  %p5 = sub i32 %p4, 1\n  %g_m = getelementptr i8, ptr %buf, i32 %p5\n  store i8 45, ptr %g_m\n"
        "  store i32 %p5, ptr %posp\n  br label %fin\nfin:\n"
        "  %fp = load i32, ptr %posp\n  %sp = getelementptr i8, ptr %buf, i32 %fp\n"
        "  %r = call ptr @stricta_str_cstr(ptr %sp)\n  ret ptr %r\n}\n\n"
        "define internal i64 @stricta_pow_i64(i64 %base0, i64 %exp0) {\nentry:\n"
        "  %neg = icmp slt i64 %exp0, 0\n  br i1 %neg, label %ret0, label %init\nret0:\n  ret i64 0\ninit:\n"
        "  %res_p = alloca i64\n  %b_p = alloca i64\n  %e_p = alloca i64\n"
        "  store i64 1, ptr %res_p\n  store i64 %base0, ptr %b_p\n  store i64 %exp0, ptr %e_p\n  br label %loop\nloop:\n"
        "  %e = load i64, ptr %e_p\n  %gt0 = icmp sgt i64 %e, 0\n  br i1 %gt0, label %body, label %done\nbody:\n"
        "  %odd = and i64 %e, 1\n  %is1 = icmp ne i64 %odd, 0\n  br i1 %is1, label %mulr, label %sh\nmulr:\n"
        "  %r0 = load i64, ptr %res_p\n  %b0 = load i64, ptr %b_p\n  %nr = mul i64 %r0, %b0\n  store i64 %nr, ptr %res_p\n  br label %sh\nsh:\n"
        "  %ne = ashr i64 %e, 1\n  store i64 %ne, ptr %e_p\n  %gtn = icmp sgt i64 %ne, 0\n  br i1 %gtn, label %sqb, label %loop\nsqb:\n"
        "  %b1 = load i64, ptr %b_p\n  %nb = mul i64 %b1, %b1\n  store i64 %nb, ptr %b_p\n  br label %loop\ndone:\n"
        "  %fr = load i64, ptr %res_p\n  ret i64 %fr\n}\n\n");
}

static void emit_llvm_rt_bytes_file_task(StrBuf* b) {
    /* Bytes */
    sb_append(b,
        "define internal ptr @stricta_bytes_new(ptr %data, i32 %len) {\nentry:\n"
        "  %bsz = ptrtoint ptr getelementptr (%StrictaBytes, ptr null, i32 1) to i64\n"
        "  %st = call ptr @stricta_alloc(i64 %bsz)\n"
        "  %p0 = getelementptr %StrictaBytes, ptr %st, i32 0, i32 0\n  store i32 %len, ptr %p0\n"
        "  %p1 = getelementptr %StrictaBytes, ptr %st, i32 0, i32 1\n  store i32 0, ptr %p1\n"
        "  %l64 = sext i32 %len to i64\n  %asz = add i64 %l64, 1\n"
        "  %ch = call ptr @stricta_alloc(i64 %asz)\n"
        "  %p2 = getelementptr %StrictaBytes, ptr %st, i32 0, i32 2\n  store ptr %ch, ptr %p2\n"
        "  %gt0 = icmp sgt i32 %len, 0\n  %nd = icmp ne ptr %data, null\n  %do = and i1 %gt0, %nd\n"
        "  br i1 %do, label %cp, label %nul\ncp:\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %ch, ptr %data, i64 %l64, i1 false)\n  br label %nul\nnul:\n"
        "  %end = getelementptr i8, ptr %ch, i64 %l64\n  store i8 0, ptr %end\n  ret ptr %st\n}\n\n"
        "define internal void @stricta_bytes_free(ptr %b) alwaysinline {\nentry:\n"
        "  %ok = icmp ne ptr %b, null\n  br i1 %ok, label %chk, label %done\nchk:\n"
        "  %sp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 1\n  %stat = load i32, ptr %sp\n"
        "  %dyn = icmp eq i32 %stat, 0\n  br i1 %dyn, label %dofree, label %done\ndofree:\n"
        "  %dp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 2\n  %da = load ptr, ptr %dp\n"
        "  call void @free(ptr %da)\n  call void @free(ptr %b)\n  br label %done\ndone:\n  ret void\n}\n\n"
        "define internal ptr @stricta_bytes_clone(ptr %b) {\nentry:\n"
        "  %ok = icmp ne ptr %b, null\n  br i1 %ok, label %chk, label %ret_same\nchk:\n"
        "  %sp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 1\n  %stat = load i32, ptr %sp\n"
        "  %dyn = icmp eq i32 %stat, 0\n  br i1 %dyn, label %doclone, label %ret_same\nret_same:\n"
        "  ret ptr %b\ndoclone:\n"
        "  %lp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %dp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 2\n  %da = load ptr, ptr %dp\n"
        "  %r = call ptr @stricta_bytes_new(ptr %da, i32 %len)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_bytes_slice(ptr %b, i64 %start, i64 %end) {\nentry:\n"
        "  %hb = icmp ne ptr %b, null\n  br i1 %hb, label %work, label %empt\nempt:\n"
        "  %e = call ptr @stricta_bytes_new(ptr null, i32 0)\n  ret ptr %e\nwork:\n"
        "  %lp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 0\n  %l32 = load i32, ptr %lp\n"
        "  %len = sext i32 %l32 to i64\n"
        "  %sn = icmp slt i64 %start, 0\n  %s1 = add i64 %start, %len\n  %s2 = select i1 %sn, i64 %s1, i64 %start\n"
        "  %sl0 = icmp slt i64 %s2, 0\n  %fs = select i1 %sl0, i64 0, i64 %s2\n"
        "  %en = icmp slt i64 %end, 0\n  %e1 = add i64 %end, %len\n  %e2 = select i1 %en, i64 %e1, i64 %end\n"
        "  %egl = icmp sgt i64 %e2, %len\n  %fe = select i1 %egl, i64 %len, i64 %e2\n"
        "  %bad = icmp sge i64 %fs, %fe\n  br i1 %bad, label %empt, label %mk\nmk:\n"
        "  %dp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 2\n  %da = load ptr, ptr %dp\n"
        "  %src = getelementptr i8, ptr %da, i64 %fs\n  %dl = sub i64 %fe, %fs\n  %dl32 = trunc i64 %dl to i32\n"
        "  %r = call ptr @stricta_bytes_new(ptr %src, i32 %dl32)\n  ret ptr %r\n}\n\n"
        "define internal ptr @stricta_bytes_slice_step(ptr %b, i64 %start, i32 %hs, i64 %stop, i32 %hsp, i64 %step) {\nentry:\n"
        "  %cur_p = alloca i64\n  %ok = icmp ne ptr %b, null\n  br i1 %ok, label %work, label %nullb\nnullb:\n"
        "  %d0 = call i64 @stricta_slice_adjust(i64 0, i64 %start, i32 %hs, i64 %stop, i32 %hsp, i64 %step, ptr %cur_p)\n"
        "  %e0 = call ptr @stricta_bytes_new(ptr null, i32 0)\n  ret ptr %e0\nwork:\n"
        "  %lp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 0\n  %l32 = load i32, ptr %lp\n"
        "  %len = sext i32 %l32 to i64\n"
        "  %cnt = call i64 @stricta_slice_adjust(i64 %len, i64 %start, i32 %hs, i64 %stop, i32 %hsp, i64 %step, ptr %cur_p)\n"
        "  %le0 = icmp sle i64 %cnt, 0\n  br i1 %le0, label %empt, label %chk1\nempt:\n"
        "  %e1 = call ptr @stricta_bytes_new(ptr null, i32 0)\n  ret ptr %e1\nchk1:\n"
        "  %cur0 = load i64, ptr %cur_p\n"
        "  %dp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 2\n  %da = load ptr, ptr %dp\n"
        "  %is1 = icmp eq i64 %step, 1\n  br i1 %is1, label %fast1, label %slow\nfast1:\n"
        "  %src = getelementptr i8, ptr %da, i64 %cur0\n  %c32 = trunc i64 %cnt to i32\n"
        "  %r1 = call ptr @stricta_bytes_new(ptr %src, i32 %c32)\n  ret ptr %r1\nslow:\n"
        "  %bsz = ptrtoint ptr getelementptr (%StrictaBytes, ptr null, i32 1) to i64\n"
        "  %out = call ptr @stricta_alloc(i64 %bsz)\n  %cnt32 = trunc i64 %cnt to i32\n"
        "  %p0 = getelementptr %StrictaBytes, ptr %out, i32 0, i32 0\n  store i32 %cnt32, ptr %p0\n"
        "  %p1 = getelementptr %StrictaBytes, ptr %out, i32 0, i32 1\n  store i32 0, ptr %p1\n"
        "  %asz = add i64 %cnt, 1\n  %och = call ptr @stricta_alloc(i64 %asz)\n"
        "  %p2 = getelementptr %StrictaBytes, ptr %out, i32 0, i32 2\n  store ptr %och, ptr %p2\n"
        "  %ip = alloca i64\n  store i64 0, ptr %ip\n  br label %loop\nloop:\n"
        "  %i = load i64, ptr %ip\n  %cond = icmp slt i64 %i, %cnt\n  br i1 %cond, label %body, label %done\nbody:\n"
        "  %cur = load i64, ptr %cur_p\n  %sp = getelementptr i8, ptr %da, i64 %cur\n  %bval = load i8, ptr %sp\n"
        "  %dst = getelementptr i8, ptr %och, i64 %i\n  store i8 %bval, ptr %dst\n"
        "  %ncur = add i64 %cur, %step\n  store i64 %ncur, ptr %cur_p\n"
        "  %ni = add i64 %i, 1\n  store i64 %ni, ptr %ip\n  br label %loop\ndone:\n"
        "  %end = getelementptr i8, ptr %och, i64 %cnt\n  store i8 0, ptr %end\n  ret ptr %out\n}\n\n"
        "define internal i32 @stricta_bytes_eq(ptr %a, ptr %b) {\nentry:\n"
        "  %same = icmp eq ptr %a, %b\n  br i1 %same, label %ret1, label %chkn\nchkn:\n"
        "  %na = icmp eq ptr %a, null\n  %nb = icmp eq ptr %b, null\n  %any = or i1 %na, %nb\n"
        "  br i1 %any, label %ret0, label %chkl\nchkl:\n"
        "  %lap = getelementptr %StrictaBytes, ptr %a, i32 0, i32 0\n  %la = load i32, ptr %lap\n"
        "  %lbp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 0\n  %lb = load i32, ptr %lbp\n"
        "  %leq = icmp eq i32 %la, %lb\n  br i1 %leq, label %docmp, label %ret0\ndocmp:\n"
        "  %adp = getelementptr %StrictaBytes, ptr %a, i32 0, i32 2\n  %ad = load ptr, ptr %adp\n"
        "  %bdp = getelementptr %StrictaBytes, ptr %b, i32 0, i32 2\n  %bd = load ptr, ptr %bdp\n"
        "  %l64 = sext i32 %la to i64\n  %mc = call i32 @memcmp(ptr %ad, ptr %bd, i64 %l64)\n"
        "  %eq = icmp eq i32 %mc, 0\n  %r = zext i1 %eq to i32\n  ret i32 %r\nret1:\n  ret i32 1\nret0:\n  ret i32 0\n}\n\n");

    /* File I/O */
    sb_append(b,
        "define internal ptr @stricta_open(ptr %path, ptr %mode) {\nentry:\n"
        "  %hm = icmp ne ptr %mode, null\n  br i1 %hm, label %chkm, label %defm\nchkm:\n"
        "  %mlp = getelementptr %StrictaStr, ptr %mode, i32 0, i32 0\n  %ml = load i32, ptr %mlp\n"
        "  %mg = icmp sgt i32 %ml, 0\n  br i1 %mg, label %getm, label %defm\ngetm:\n"
        "  %mcp = getelementptr %StrictaStr, ptr %mode, i32 0, i32 2\n  %mch = load ptr, ptr %mcp\n  br label %gotm\ndefm:\n"
        "  br label %gotm\ngotm:\n"
        "  %m = phi ptr [ %mch, %getm ], [ @.rt_mode_r, %defm ]\n"
        "  %hp = icmp ne ptr %path, null\n  br i1 %hp, label %getp, label %defp\ngetp:\n"
        "  %pcp = getelementptr %StrictaStr, ptr %path, i32 0, i32 2\n  %pch = load ptr, ptr %pcp\n  br label %gotp\ndefp:\n"
        "  br label %gotp\ngotp:\n"
        "  %p = phi ptr [ %pch, %getp ], [ @.rt_empty, %defp ]\n"
        "  %h = call ptr @fopen(ptr %p, ptr %m)\n  %ok = icmp ne ptr %h, null\n"
        "  br i1 %ok, label %mk, label %fail\nfail:\n"
        "  call void @stricta_set_exc(i32 4, ptr null, ptr @.rt_err_open)\n"
        "  call void @stricta_unhandled_exc()\n  unreachable\nmk:\n"
        "  %fsz = ptrtoint ptr getelementptr (%StrictaFile, ptr null, i32 1) to i64\n"
        "  %f = call ptr @stricta_alloc(i64 %fsz)\n  store ptr %h, ptr %f\n  ret ptr %f\n}\n\n"
        "define internal ptr @stricta_file_read(ptr %f) {\nentry:\n"
        "  %hf = icmp ne ptr %f, null\n  br i1 %hf, label %chkh, label %empt\nchkh:\n"
        "  %h = load ptr, ptr %f\n  %hh = icmp ne ptr %h, null\n  br i1 %hh, label %work, label %empt\nempt:\n"
        "  %e = call ptr @stricta_str_new(ptr @.rt_empty, i32 0)\n  ret ptr %e\nwork:\n"
        "  %r1 = call i32 @fseek(ptr %h, i64 0, i32 2)\n"
        "  %sz = call i64 @ftell(ptr %h)\n"
        "  %r2 = call i32 @fseek(ptr %h, i64 0, i32 0)\n"
        "  %le0 = icmp sle i64 %sz, 0\n  br i1 %le0, label %empt, label %rd\nrd:\n"
        "  %asz = add i64 %sz, 1\n  %buf = call ptr @stricta_alloc(i64 %asz)\n"
        "  %n = call i64 @fread(ptr %buf, i64 1, i64 %sz, ptr %h)\n"
        "  %end = getelementptr i8, ptr %buf, i64 %n\n  store i8 0, ptr %end\n"
        "  %n32 = trunc i64 %n to i32\n"
        "  %s = call ptr @stricta_str_new(ptr %buf, i32 %n32)\n"
        "  call void @free(ptr %buf)\n  ret ptr %s\n}\n\n"
        "define internal void @stricta_file_write(ptr %f, ptr %text) {\nentry:\n"
        "  %hf = icmp ne ptr %f, null\n  %ht = icmp ne ptr %text, null\n  %ok = and i1 %hf, %ht\n"
        "  br i1 %ok, label %chkh, label %done\nchkh:\n"
        "  %h = load ptr, ptr %f\n  %hh = icmp ne ptr %h, null\n  br i1 %hh, label %chkl, label %done\nchkl:\n"
        "  %lp = getelementptr %StrictaStr, ptr %text, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %gt0 = icmp sgt i32 %len, 0\n  br i1 %gt0, label %wr, label %done\nwr:\n"
        "  %cp = getelementptr %StrictaStr, ptr %text, i32 0, i32 2\n  %ch = load ptr, ptr %cp\n"
        "  %l64 = sext i32 %len to i64\n"
        "  %n = call i64 @fwrite(ptr %ch, i64 1, i64 %l64, ptr %h)\n  br label %done\ndone:\n  ret void\n}\n\n"
        "define internal void @stricta_file_close(ptr %f) {\nentry:\n"
        "  %hf = icmp ne ptr %f, null\n  br i1 %hf, label %chkh, label %done\nchkh:\n"
        "  %h = load ptr, ptr %f\n  %hh = icmp ne ptr %h, null\n  br i1 %hh, label %cl, label %done\ncl:\n"
        "  %r = call i32 @fclose(ptr %h)\n  store ptr null, ptr %f\n  br label %done\ndone:\n  ret void\n}\n\n"
        "define internal void @stricta_file_free(ptr %f) {\nentry:\n"
        "  %hf = icmp ne ptr %f, null\n  br i1 %hf, label %work, label %done\nwork:\n"
        "  call void @stricta_file_close(ptr %f)\n  call void @free(ptr %f)\n  br label %done\ndone:\n  ret void\n}\n\n");

    /* Async Tasks */
    sb_append(b,
        "define internal ptr @stricta_task_spawn(ptr %entry_fn, ptr %arg_block, ptr %drop_res) {\nentry:\n"
        "  %tsz = ptrtoint ptr getelementptr (%StrictaTask, ptr null, i32 1) to i64\n"
        "  %t = call ptr @stricta_calloc(i64 1, i64 %tsz)\n"
        "  %p4 = getelementptr %StrictaTask, ptr %t, i32 0, i32 4\n  store ptr %arg_block, ptr %p4\n"
        "  %p5 = getelementptr %StrictaTask, ptr %t, i32 0, i32 5\n  store ptr %drop_res, ptr %p5\n"
        "  %p0 = getelementptr %StrictaTask, ptr %t, i32 0, i32 0\n"
        "  %rc = call i32 @pthread_create(ptr %p0, ptr null, ptr %entry_fn, ptr %t)\n"
        "  %ok = icmp eq i32 %rc, 0\n  br i1 %ok, label %done, label %fail\nfail:\n"
        "  %r = call i32 (i32, ptr, ...) @dprintf(i32 2, ptr @.rt_err_task, i32 %rc)\n"
        "  call void @exit(i32 1)\n  unreachable\ndone:\n  ret ptr %t\n}\n\n"
        "define internal ptr @stricta_task_args(ptr %t) {\nentry:\n"
        "  %ok = icmp ne ptr %t, null\n  br i1 %ok, label %get, label %ret0\nget:\n"
        "  %p4 = getelementptr %StrictaTask, ptr %t, i32 0, i32 4\n  %ab = load ptr, ptr %p4\n  ret ptr %ab\nret0:\n  ret ptr null\n}\n\n"
        "define internal void @stricta_task_finish(ptr %t, i64 %bits) {\nentry:\n"
        "  %ok = icmp ne ptr %t, null\n  br i1 %ok, label %set, label %done\nset:\n"
        "  %p3 = getelementptr %StrictaTask, ptr %t, i32 0, i32 3\n  store i64 %bits, ptr %p3\n"
        "  %p2 = getelementptr %StrictaTask, ptr %t, i32 0, i32 2\n  store i32 1, ptr %p2\n  br label %done\ndone:\n  ret void\n}\n\n"
        "define internal void @stricta_task_capture_exc(ptr %t) {\nentry:\n"
        "  %ok = icmp ne ptr %t, null\n  br i1 %ok, label %cap, label %done\ncap:\n"
        "  %tid = load i32, ptr @_exc_type_id\n  %obj = load ptr, ptr @_exc_obj\n  %msg = load ptr, ptr @_exc_msg\n"
        "  %p6 = getelementptr %StrictaTask, ptr %t, i32 0, i32 6\n  store i32 %tid, ptr %p6\n"
        "  %p7 = getelementptr %StrictaTask, ptr %t, i32 0, i32 7\n  store ptr %obj, ptr %p7\n"
        "  %p8 = getelementptr %StrictaTask, ptr %t, i32 0, i32 8\n  store ptr %msg, ptr %p8\n"
        "  store i32 0, ptr @_exc_type_id\n  store ptr null, ptr @_exc_obj\n  store ptr null, ptr @_exc_msg\n  br label %done\ndone:\n  ret void\n}\n\n");
    sb_append(b,
        "define internal i64 @stricta_task_await(ptr %t) {\nentry:\n"
        "  %ok = icmp ne ptr %t, null\n  br i1 %ok, label %chkj, label %ret0\nret0:\n  ret i64 0\nchkj:\n"
        "  %p1 = getelementptr %StrictaTask, ptr %t, i32 0, i32 1\n  %j = load i32, ptr %p1\n"
        "  %nj = icmp eq i32 %j, 0\n  br i1 %nj, label %dojoin, label %chkexc\ndojoin:\n"
        "  %p0 = getelementptr %StrictaTask, ptr %t, i32 0, i32 0\n  %th = load i64, ptr %p0\n"
        "  %jr = call i32 @pthread_join(i64 %th, ptr null)\n  store i32 1, ptr %p1\n  br label %chkexc\nchkexc:\n"
        "  %p6 = getelementptr %StrictaTask, ptr %t, i32 0, i32 6\n  %tid = load i32, ptr %p6\n"
        "  %he = icmp ne i32 %tid, 0\n  br i1 %he, label %rethrow, label %okret\nrethrow:\n"
        "  call void @stricta_clear_exc(i32 0)\n"
        "  %p7 = getelementptr %StrictaTask, ptr %t, i32 0, i32 7\n  %obj = load ptr, ptr %p7\n"
        "  %p8 = getelementptr %StrictaTask, ptr %t, i32 0, i32 8\n  %msg = load ptr, ptr %p8\n"
        "  store i32 %tid, ptr @_exc_type_id\n  store ptr %obj, ptr @_exc_obj\n  store ptr %msg, ptr @_exc_msg\n"
        "  call void @free(ptr %t)\n  ret i64 0\nokret:\n"
        "  %p3 = getelementptr %StrictaTask, ptr %t, i32 0, i32 3\n  %bits = load i64, ptr %p3\n"
        "  call void @free(ptr %t)\n  ret i64 %bits\n}\n\n"
        "define internal void @stricta_task_free(ptr %t) {\nentry:\n"
        "  %ok = icmp ne ptr %t, null\n  br i1 %ok, label %chkj, label %done\nchkj:\n"
        "  %p1 = getelementptr %StrictaTask, ptr %t, i32 0, i32 1\n  %j = load i32, ptr %p1\n"
        "  %nj = icmp eq i32 %j, 0\n  br i1 %nj, label %dojoin, label %chkexc\ndojoin:\n"
        "  %p0 = getelementptr %StrictaTask, ptr %t, i32 0, i32 0\n  %th = load i64, ptr %p0\n"
        "  %jr = call i32 @pthread_join(i64 %th, ptr null)\n  store i32 1, ptr %p1\n  br label %chkexc\nchkexc:\n"
        "  %p6 = getelementptr %StrictaTask, ptr %t, i32 0, i32 6\n  %tid = load i32, ptr %p6\n"
        "  %he = icmp ne i32 %tid, 0\n  br i1 %he, label %dropexc, label %chkres\ndropexc:\n"
        "  %p8 = getelementptr %StrictaTask, ptr %t, i32 0, i32 8\n  %msg = load ptr, ptr %p8\n"
        "  %hm = icmp ne ptr %msg, null\n  br i1 %hm, label %fmsg, label %fobj\nfmsg:\n"
        "  call void @stricta_str_free(ptr %msg)\n  br label %fobj\nfobj:\n"
        "  %p7 = getelementptr %StrictaTask, ptr %t, i32 0, i32 7\n  %obj = load ptr, ptr %p7\n"
        "  %ho = icmp ne ptr %obj, null\n  br i1 %ho, label %dfobj, label %freet\ndfobj:\n"
        "  call void @free(ptr %obj)\n  br label %freet\nchkres:\n"
        "  %p2 = getelementptr %StrictaTask, ptr %t, i32 0, i32 2\n  %hr = load i32, ptr %p2\n"
        "  %p5 = getelementptr %StrictaTask, ptr %t, i32 0, i32 5\n  %df = load ptr, ptr %p5\n"
        "  %b1 = icmp ne i32 %hr, 0\n  %b2 = icmp ne ptr %df, null\n  %do_drop = and i1 %b1, %b2\n"
        "  br i1 %do_drop, label %dropres, label %freet\ndropres:\n"
        "  %p3 = getelementptr %StrictaTask, ptr %t, i32 0, i32 3\n  %bits = load i64, ptr %p3\n"
        "  call void %df(i64 %bits)\n  br label %freet\nfreet:\n"
        "  call void @free(ptr %t)\n  br label %done\ndone:\n  ret void\n}\n\n");
}

static void emit_llvm_rt_bigint(StrBuf* b) {
    sb_append(b,
        "define internal void @_bi_strip(ptr %d, ptr %lenp) {\nentry:\n"
        "  %l = load i32, ptr %lenp\n  %lm1 = sub i32 %l, 1\n  %ip = alloca i32\n  store i32 0, ptr %ip\n  br label %loop\nloop:\n"
        "  %i = load i32, ptr %ip\n  %c1 = icmp slt i32 %i, %lm1\n  br i1 %c1, label %chk, label %done\nchk:\n"
        "  %gp = getelementptr i8, ptr %d, i32 %i\n  %ch = load i8, ptr %gp\n  %z = icmp eq i8 %ch, 48\n"
        "  br i1 %z, label %inc, label %done\ninc:\n"
        "  %ni = add i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %loop\ndone:\n"
        "  %fi = load i32, ptr %ip\n  %gt0 = icmp sgt i32 %fi, 0\n  br i1 %gt0, label %mv, label %retb\nmv:\n"
        "  %fi64 = sext i32 %fi to i64\n  %src = getelementptr i8, ptr %d, i64 %fi64\n"
        "  %nl = sub i32 %l, %fi\n  %n1 = add i32 %nl, 1\n  %n1_64 = sext i32 %n1 to i64\n"
        "  call void @llvm.memmove.p0.p0.i64(ptr %d, ptr %src, i64 %n1_64, i1 false)\n"
        "  store i32 %nl, ptr %lenp\n  br label %retb\nretb:\n  ret void\n}\n\n");
    sb_append(b,
        "define internal i32 @_bi_abs_cmp(ptr %a, i32 %al, ptr %b, i32 %bl) {\nentry:\n"
        "  %neq = icmp ne i32 %al, %bl\n  br i1 %neq, label %by_len, label %by_mem\nby_len:\n"
        "  %lt = icmp slt i32 %al, %bl\n  %r = select i1 %lt, i32 -1, i32 1\n  ret i32 %r\nby_mem:\n"
        "  %al64 = sext i32 %al to i64\n  %mc = call i32 @memcmp(ptr %a, ptr %b, i64 %al64)\n  ret i32 %mc\n}\n\n");
    sb_append(b,
        "define internal ptr @_bi_add_abs(ptr %a, i32 %al, ptr %b, i32 %bl, ptr %olp) {\nentry:\n"
        "  %mx = call i32 @llvm.smax.i32(i32 %al, i32 %bl)\n  %m1 = add i32 %mx, 2\n  %m64 = sext i32 %m1 to i64\n"
        "  %rev = call ptr @stricta_alloc(i64 %m64)\n"
        "  %ip = alloca i32\n  %jp = alloca i32\n  %cp = alloca i32\n  %kp = alloca i32\n"
        "  %am1 = sub i32 %al, 1\n  %bm1 = sub i32 %bl, 1\n"
        "  store i32 %am1, ptr %ip\n  store i32 %bm1, ptr %jp\n  store i32 0, ptr %cp\n  store i32 0, ptr %kp\n  br label %loop\nloop:\n"
        "  %i = load i32, ptr %ip\n  %j = load i32, ptr %jp\n  %c = load i32, ptr %cp\n"
        "  %ci = icmp sge i32 %i, 0\n  %cj = icmp sge i32 %j, 0\n  %cc = icmp ne i32 %c, 0\n"
        "  %a1 = or i1 %ci, %cj\n  %go = or i1 %a1, %cc\n  br i1 %go, label %body, label %revl\nbody:\n"
        "  %sp = alloca i32\n  store i32 %c, ptr %sp\n  br i1 %ci, label %adda, label %chkb\nadda:\n"
        "  %ap = getelementptr i8, ptr %a, i32 %i\n  %ac = load i8, ptr %ap\n  %a32 = zext i8 %ac to i32\n"
        "  %ad = sub i32 %a32, 48\n  %s0 = load i32, ptr %sp\n  %s1 = add i32 %s0, %ad\n  store i32 %s1, ptr %sp\n"
        "  %ni = sub i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %chkb\nchkb:\n"
        "  br i1 %cj, label %addb, label %putd\naddb:\n"
        "  %bp = getelementptr i8, ptr %b, i32 %j\n  %bc = load i8, ptr %bp\n  %b32 = zext i8 %bc to i32\n"
        "  %bd = sub i32 %b32, 48\n  %s2 = load i32, ptr %sp\n  %s3 = add i32 %s2, %bd\n  store i32 %s3, ptr %sp\n"
        "  %nj = sub i32 %j, 1\n  store i32 %nj, ptr %jp\n  br label %putd\nputd:\n"
        "  %s = load i32, ptr %sp\n  %rem = srem i32 %s, 10\n  %nc = sdiv i32 %s, 10\n  store i32 %nc, ptr %cp\n"
        "  %d32 = add i32 %rem, 48\n  %d8 = trunc i32 %d32 to i8\n"
        "  %k = load i32, ptr %kp\n  %rp = getelementptr i8, ptr %rev, i32 %k\n  store i8 %d8, ptr %rp\n"
        "  %nk = add i32 %k, 1\n  store i32 %nk, ptr %kp\n  br label %loop\nrevl:\n"
        "  %fk = load i32, ptr %kp\n  %fk1 = add i32 %fk, 1\n  %fk64 = sext i32 %fk1 to i64\n"
        "  %res = call ptr @stricta_alloc(i64 %fk64)\n  %pp = alloca i32\n  store i32 0, ptr %pp\n  br label %rloop\nrloop:\n"
        "  %p = load i32, ptr %pp\n  %c2 = icmp slt i32 %p, %fk\n  br i1 %c2, label %rbody, label %rdone\nrbody:\n"
        "  %si = sub i32 %fk, 1\n  %si2 = sub i32 %si, %p\n"
        "  %sp0 = getelementptr i8, ptr %rev, i32 %si2\n  %ch = load i8, ptr %sp0\n"
        "  %dp0 = getelementptr i8, ptr %res, i32 %p\n  store i8 %ch, ptr %dp0\n"
        "  %np = add i32 %p, 1\n  store i32 %np, ptr %pp\n  br label %rloop\nrdone:\n"
        "  %end = getelementptr i8, ptr %res, i32 %fk\n  store i8 0, ptr %end\n"
        "  call void @free(ptr %rev)\n  store i32 %fk, ptr %olp\n  ret ptr %res\n}\n\n");
    sb_append(b,
        "define internal ptr @_bi_sub_abs(ptr %a, i32 %al, ptr %b, i32 %bl, ptr %olp) {\nentry:\n"
        "  %a1 = add i32 %al, 1\n  %a64 = sext i32 %a1 to i64\n"
        "  %rev = call ptr @stricta_alloc(i64 %a64)\n"
        "  %ip = alloca i32\n  %jp = alloca i32\n  %brp = alloca i32\n  %kp = alloca i32\n"
        "  %am1 = sub i32 %al, 1\n  %bm1 = sub i32 %bl, 1\n"
        "  store i32 %am1, ptr %ip\n  store i32 %bm1, ptr %jp\n  store i32 0, ptr %brp\n  store i32 0, ptr %kp\n  br label %loop\nloop:\n"
        "  %i = load i32, ptr %ip\n  %j = load i32, ptr %jp\n"
        "  %ci = icmp sge i32 %i, 0\n  %cj = icmp sge i32 %j, 0\n  %go = or i1 %ci, %cj\n"
        "  br i1 %go, label %body, label %trim\nbody:\n"
        "  %dp = alloca i32\n  %br0 = load i32, ptr %brp\n  store i32 %br0, ptr %dp\n"
        "  br i1 %ci, label %adda, label %chkb\nadda:\n"
        "  %ap = getelementptr i8, ptr %a, i32 %i\n  %ac = load i8, ptr %ap\n  %a32 = zext i8 %ac to i32\n"
        "  %ad = sub i32 %a32, 48\n  %d0 = load i32, ptr %dp\n  %d1 = add i32 %d0, %ad\n  store i32 %d1, ptr %dp\n"
        "  %ni = sub i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %chkb\nchkb:\n"
        "  br i1 %cj, label %subb, label %putd\nsubb:\n"
        "  %bp = getelementptr i8, ptr %b, i32 %j\n  %bc = load i8, ptr %bp\n  %b32 = zext i8 %bc to i32\n"
        "  %bd = sub i32 %b32, 48\n  %d2 = load i32, ptr %dp\n  %d3 = sub i32 %d2, %bd\n  store i32 %d3, ptr %dp\n"
        "  %nj = sub i32 %j, 1\n  store i32 %nj, ptr %jp\n  br label %putd\nputd:\n"
        "  %d4 = load i32, ptr %dp\n  %lt0 = icmp slt i32 %d4, 0\n"
        "  %d5 = add i32 %d4, 10\n  %fd = select i1 %lt0, i32 %d5, i32 %d4\n"
        "  %nbr = select i1 %lt0, i32 -1, i32 0\n  store i32 %nbr, ptr %brp\n"
        "  %c32 = add i32 %fd, 48\n  %c8 = trunc i32 %c32 to i8\n"
        "  %k = load i32, ptr %kp\n  %rp = getelementptr i8, ptr %rev, i32 %k\n  store i8 %c8, ptr %rp\n"
        "  %nk = add i32 %k, 1\n  store i32 %nk, ptr %kp\n  br label %loop\ntrim:\n"
        "  %tk = load i32, ptr %kp\n  %gt1 = icmp sgt i32 %tk, 1\n  br i1 %gt1, label %tchk, label %revl\ntchk:\n"
        "  %tm1 = sub i32 %tk, 1\n  %tp = getelementptr i8, ptr %rev, i32 %tm1\n  %tc = load i8, ptr %tp\n"
        "  %isz = icmp eq i8 %tc, 48\n  br i1 %isz, label %tdec, label %revl\ntdec:\n"
        "  store i32 %tm1, ptr %kp\n  br label %trim\nrevl:\n"
        "  %fk = load i32, ptr %kp\n  %fk1 = add i32 %fk, 1\n  %fk64 = sext i32 %fk1 to i64\n"
        "  %res = call ptr @stricta_alloc(i64 %fk64)\n  %pp = alloca i32\n  store i32 0, ptr %pp\n  br label %rloop\nrloop:\n"
        "  %p = load i32, ptr %pp\n  %c2 = icmp slt i32 %p, %fk\n  br i1 %c2, label %rbody, label %rdone\nrbody:\n"
        "  %si = sub i32 %fk, 1\n  %si2 = sub i32 %si, %p\n"
        "  %sp0 = getelementptr i8, ptr %rev, i32 %si2\n  %ch = load i8, ptr %sp0\n"
        "  %dp0 = getelementptr i8, ptr %res, i32 %p\n  store i8 %ch, ptr %dp0\n"
        "  %np = add i32 %p, 1\n  store i32 %np, ptr %pp\n  br label %rloop\nrdone:\n"
        "  %end = getelementptr i8, ptr %res, i32 %fk\n  store i8 0, ptr %end\n"
        "  call void @free(ptr %rev)\n  store i32 %fk, ptr %olp\n  ret ptr %res\n}\n\n");
    sb_append(b,
        "define internal ptr @_bi_make(ptr %digits, i32 %len0, i32 %sign0) {\nentry:\n"
        "  %lp = alloca i32\n  store i32 %len0, ptr %lp\n"
        "  call void @_bi_strip(ptr %digits, ptr %lp)\n"
        "  %len = load i32, ptr %lp\n"
        "  %l1 = icmp eq i32 %len, 1\n  %c0 = load i8, ptr %digits\n  %z0 = icmp eq i8 %c0, 48\n"
        "  %isz = and i1 %l1, %z0\n  %sign = select i1 %isz, i32 1, i32 %sign0\n"
        "  %bsz = ptrtoint ptr getelementptr (%StrictaBigInt, ptr null, i32 1) to i64\n"
        "  %bi = call ptr @stricta_alloc(i64 %bsz)\n"
        "  %p0 = getelementptr %StrictaBigInt, ptr %bi, i32 0, i32 0\n  store i32 %len, ptr %p0\n"
        "  %p1 = getelementptr %StrictaBigInt, ptr %bi, i32 0, i32 1\n  store i32 %sign, ptr %p1\n"
        "  %p2 = getelementptr %StrictaBigInt, ptr %bi, i32 0, i32 2\n  store ptr %digits, ptr %p2\n"
        "  ret ptr %bi\n}\n\n"
        "define internal ptr @stricta_bigint_from_str(ptr %s0) {\nentry:\n"
        "  %hs = icmp ne ptr %s0, null\n  br i1 %hs, label %chkc, label %mkz\nchkc:\n"
        "  %c0 = load i8, ptr %s0\n  %nz = icmp ne i8 %c0, 0\n  br i1 %nz, label %parse, label %mkz\nmkz:\n"
        "  %zd = call ptr @stricta_alloc(i64 2)\n  store i8 48, ptr %zd\n"
        "  %z1 = getelementptr i8, ptr %zd, i32 1\n  store i8 0, ptr %z1\n"
        "  %rz = call ptr @_bi_make(ptr %zd, i32 1, i32 1)\n  ret ptr %rz\nparse:\n"
        "  %ism = icmp eq i8 %c0, 45\n  br i1 %ism, label %mneg, label %chkp\nmneg:\n"
        "  %s1 = getelementptr i8, ptr %s0, i32 1\n  br label %copy\nchkp:\n"
        "  %isp = icmp eq i8 %c0, 43\n  %s2 = getelementptr i8, ptr %s0, i32 1\n"
        "  %s3 = select i1 %isp, ptr %s2, ptr %s0\n  br label %copy\ncopy:\n"
        "  %s = phi ptr [ %s1, %mneg ], [ %s3, %chkp ]\n"
        "  %sign = phi i32 [ -1, %mneg ], [ 1, %chkp ]\n"
        "  %l64 = call i64 @strlen(ptr %s)\n  %l32 = trunc i64 %l64 to i32\n"
        "  %asz = add i64 %l64, 1\n  %d = call ptr @stricta_alloc(i64 %asz)\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %s, i64 %asz, i1 false)\n"
        "  %r = call ptr @_bi_make(ptr %d, i32 %l32, i32 %sign)\n  ret ptr %r\n}\n\n"
        "define internal ptr @stricta_bigint_from_i64(i64 %v) {\nentry:\n"
        "  %buf = alloca [32 x i8]\n"
        "  %n = call i32 (ptr, i64, ptr, ...) @snprintf(ptr %buf, i64 32, ptr @.rt_fmt_lld, i64 %v)\n"
        "  %r = call ptr @stricta_bigint_from_str(ptr %buf)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_bigint_add(ptr %a, ptr %b) {\nentry:\n"
        "  %olp = alloca i32\n"
        "  %asp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 1\n  %as = load i32, ptr %asp\n"
        "  %bsp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 1\n  %bs = load i32, ptr %bsp\n"
        "  %alp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 0\n  %al = load i32, ptr %alp\n"
        "  %blp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 0\n  %bl = load i32, ptr %blp\n"
        "  %adp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 2\n  %ad = load ptr, ptr %adp\n"
        "  %bdp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 2\n  %bd = load ptr, ptr %bdp\n"
        "  %eqs = icmp eq i32 %as, %bs\n  br i1 %eqs, label %same, label %diff\nsame:\n"
        "  %d1 = call ptr @_bi_add_abs(ptr %ad, i32 %al, ptr %bd, i32 %bl, ptr %olp)\n"
        "  %l1 = load i32, ptr %olp\n  %r1 = call ptr @_bi_make(ptr %d1, i32 %l1, i32 %as)\n  ret ptr %r1\ndiff:\n"
        "  %c = call i32 @_bi_abs_cmp(ptr %ad, i32 %al, ptr %bd, i32 %bl)\n"
        "  %c0 = icmp eq i32 %c, 0\n  br i1 %c0, label %ret0, label %chkgt\nret0:\n"
        "  %r0 = call ptr @stricta_bigint_from_str(ptr @.rt_zero)\n  ret ptr %r0\nchkgt:\n"
        "  %cgt = icmp sgt i32 %c, 0\n  br i1 %cgt, label %agtb, label %blta\nagtb:\n"
        "  %d2 = call ptr @_bi_sub_abs(ptr %ad, i32 %al, ptr %bd, i32 %bl, ptr %olp)\n"
        "  %l2 = load i32, ptr %olp\n  %r2 = call ptr @_bi_make(ptr %d2, i32 %l2, i32 %as)\n  ret ptr %r2\nblta:\n"
        "  %d3 = call ptr @_bi_sub_abs(ptr %bd, i32 %bl, ptr %ad, i32 %al, ptr %olp)\n"
        "  %l3 = load i32, ptr %olp\n  %r3 = call ptr @_bi_make(ptr %d3, i32 %l3, i32 %bs)\n  ret ptr %r3\n}\n\n"
        "define internal ptr @stricta_bigint_sub(ptr %a, ptr %b) {\nentry:\n"
        "  %nb = alloca %StrictaBigInt\n"
        "  %bv = load %StrictaBigInt, ptr %b\n  store %StrictaBigInt %bv, ptr %nb\n"
        "  %bsp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 1\n  %bs = load i32, ptr %bsp\n"
        "  %nbs = sub i32 0, %bs\n"
        "  %nbsp = getelementptr %StrictaBigInt, ptr %nb, i32 0, i32 1\n  store i32 %nbs, ptr %nbsp\n"
        "  %r = call ptr @stricta_bigint_add(ptr %a, ptr %nb)\n  ret ptr %r\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_bigint_mul(ptr %a, ptr %b) {\nentry:\n"
        "  %alp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 0\n  %al = load i32, ptr %alp\n"
        "  %blp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 0\n  %bl = load i32, ptr %blp\n"
        "  %asp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 1\n  %as = load i32, ptr %asp\n"
        "  %bsp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 1\n  %bs = load i32, ptr %bsp\n"
        "  %adp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 2\n  %ad = load ptr, ptr %adp\n"
        "  %bdp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 2\n  %bd = load ptr, ptr %bdp\n"
        "  %rl0 = add i32 %al, %bl\n  %rl4 = add i32 %rl0, 4\n  %rl4_64 = sext i32 %rl4 to i64\n"
        "  %tmp = call ptr @stricta_calloc(i64 %rl4_64, i64 4)\n"
        "  %ip = alloca i32\n  %jp = alloca i32\n"
        "  %am1 = sub i32 %al, 1\n  store i32 %am1, ptr %ip\n  br label %l1\nl1:\n"
        "  %i = load i32, ptr %ip\n  %ci = icmp sge i32 %i, 0\n  br i1 %ci, label %l2init, label %carryinit\nl2init:\n"
        "  %bm1 = sub i32 %bl, 1\n  store i32 %bm1, ptr %jp\n  %ap = getelementptr i8, ptr %ad, i32 %i\n"
        "  %ac = load i8, ptr %ap\n  %a32 = zext i8 %ac to i32\n  %av = sub i32 %a32, 48\n"
        "  %ia = sub i32 %am1, %i\n  br label %l2\nl2:\n"
        "  %j = load i32, ptr %jp\n  %cj = icmp sge i32 %j, 0\n  br i1 %cj, label %l2b, label %l1next\nl2b:\n"
        "  %bp = getelementptr i8, ptr %bd, i32 %j\n  %bc = load i8, ptr %bp\n  %b32 = zext i8 %bc to i32\n"
        "  %bv = sub i32 %b32, 48\n  %prod = mul i32 %av, %bv\n"
        "  %jb = sub i32 %bm1, %j\n  %idx = add i32 %ia, %jb\n"
        "  %tp = getelementptr i32, ptr %tmp, i32 %idx\n  %tv = load i32, ptr %tp\n"
        "  %ntv = add i32 %tv, %prod\n  store i32 %ntv, ptr %tp\n"
        "  %nj = sub i32 %j, 1\n  store i32 %nj, ptr %jp\n  br label %l2\nl1next:\n"
        "  %ni = sub i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %l1\ncarryinit:\n"
        "  %cp = alloca i32\n  %rlp = alloca i32\n"
        "  store i32 0, ptr %cp\n  store i32 %rl0, ptr %rlp\n  store i32 0, ptr %ip\n  br label %cloop\ncloop:\n"
        "  %k = load i32, ptr %ip\n  %ck = icmp slt i32 %k, %rl0\n  br i1 %ck, label %cbody, label %cdone\ncbody:\n"
        "  %tp2 = getelementptr i32, ptr %tmp, i32 %k\n  %tv2 = load i32, ptr %tp2\n"
        "  %c0 = load i32, ptr %cp\n  %t = add i32 %tv2, %c0\n"
        "  %mod = srem i32 %t, 10\n  %nc = sdiv i32 %t, 10\n"
        "  store i32 %mod, ptr %tp2\n  store i32 %nc, ptr %cp\n"
        "  %nk = add i32 %k, 1\n  store i32 %nk, ptr %ip\n  br label %cloop\ncdone:\n"
        "  %c1 = load i32, ptr %cp\n  %cgt0 = icmp sgt i32 %c1, 0\n  br i1 %cgt0, label %cpush, label %trim\ncpush:\n"
        "  %crl = load i32, ptr %rlp\n  %tp3 = getelementptr i32, ptr %tmp, i32 %crl\n"
        "  %cm = srem i32 %c1, 10\n  %cd = sdiv i32 %c1, 10\n  store i32 %cm, ptr %tp3\n"
        "  %nrl = add i32 %crl, 1\n  store i32 %nrl, ptr %rlp\n  store i32 %cd, ptr %cp\n  br label %cdone\ntrim:\n"
        "  %trl = load i32, ptr %rlp\n  %gt1 = icmp sgt i32 %trl, 1\n  br i1 %gt1, label %tchk, label %revinit\ntchk:\n"
        "  %tm1 = sub i32 %trl, 1\n  %tp4 = getelementptr i32, ptr %tmp, i32 %tm1\n  %tv4 = load i32, ptr %tp4\n"
        "  %isz = icmp eq i32 %tv4, 0\n  br i1 %isz, label %tdec, label %revinit\ntdec:\n"
        "  store i32 %tm1, ptr %rlp\n  br label %trim\nrevinit:\n"
        "  %frl = load i32, ptr %rlp\n  %frl1 = add i32 %frl, 1\n  %frl64 = sext i32 %frl1 to i64\n"
        "  %res = call ptr @stricta_alloc(i64 %frl64)\n  store i32 0, ptr %ip\n  br label %rloop\nrloop:\n"
        "  %p = load i32, ptr %ip\n  %cond = icmp slt i32 %p, %frl\n  br i1 %cond, label %rbody, label %rdone\nrbody:\n"
        "  %si = sub i32 %frl, 1\n  %si2 = sub i32 %si, %p\n"
        "  %tp5 = getelementptr i32, ptr %tmp, i32 %si2\n  %tv5 = load i32, ptr %tp5\n"
        "  %d32 = add i32 %tv5, 48\n  %d8 = trunc i32 %d32 to i8\n"
        "  %rp5 = getelementptr i8, ptr %res, i32 %p\n  store i8 %d8, ptr %rp5\n"
        "  %np = add i32 %p, 1\n  store i32 %np, ptr %ip\n  br label %rloop\nrdone:\n"
        "  %end = getelementptr i8, ptr %res, i32 %frl\n  store i8 0, ptr %end\n"
        "  call void @free(ptr %tmp)\n"
        "  %rsign = mul i32 %as, %bs\n"
        "  %bi = call ptr @_bi_make(ptr %res, i32 %frl, i32 %rsign)\n  ret ptr %bi\n}\n\n");
    sb_append(b,
        "define internal ptr @stricta_bigint_div(ptr %a, ptr %b) {\nentry:\n"
        "  %alp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 0\n  %al = load i32, ptr %alp\n"
        "  %blp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 0\n  %bl = load i32, ptr %blp\n"
        "  %asp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 1\n  %as = load i32, ptr %asp\n"
        "  %bsp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 1\n  %bs = load i32, ptr %bsp\n"
        "  %adp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 2\n  %ad = load ptr, ptr %adp\n"
        "  %bdp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 2\n  %bd = load ptr, ptr %bdp\n"
        "  %bl1 = icmp eq i32 %bl, 1\n  %bc0 = load i8, ptr %bd\n  %bz0 = icmp eq i8 %bc0, 48\n"
        "  %bzero = and i1 %bl1, %bz0\n  br i1 %bzero, label %zdiv, label %chkfast\nzdiv:\n"
        "  call void @stricta_set_exc(i32 3, ptr null, ptr @.rt_err_zdiv)\n"
        "  call void @stricta_unhandled_exc()\n  unreachable\nchkfast:\n"
        "  %ac0 = call i32 @_bi_abs_cmp(ptr %ad, i32 %al, ptr %bd, i32 %bl)\n"
        "  %lt = icmp slt i32 %ac0, 0\n  br i1 %lt, label %fast, label %work\nfast:\n"
        "  %eqs = icmp eq i32 %as, %bs\n  %al1 = icmp eq i32 %al, 1\n  %ach0 = load i8, ptr %ad\n  %az0 = icmp eq i8 %ach0, 48\n"
        "  %ais0 = and i1 %al1, %az0\n  %retz = or i1 %eqs, %ais0\n"
        "  br i1 %retz, label %ret_0, label %ret_m1\nret_0:\n"
        "  %r0 = call ptr @stricta_bigint_from_str(ptr @.rt_zero)\n  ret ptr %r0\nret_m1:\n"
        "  %rm1 = call ptr @stricta_bigint_from_str(ptr @.rt_neg_one)\n  ret ptr %rm1\nwork:\n"
        "  %al1p = add i32 %al, 1\n  %al1_64 = sext i32 %al1p to i64\n"
        "  %res = call ptr @stricta_calloc(i64 %al1_64, i64 1)\n"
        "  %al2 = add i32 %al, 2\n  %ccap0 = sext i32 %al2 to i64\n"
        "  %ccap_p = alloca i64\n  %cur_p = alloca ptr\n  %clp = alloca i32\n  %rip = alloca i32\n  %ip = alloca i32\n  %olp = alloca i32\n"
        "  store i64 %ccap0, ptr %ccap_p\n  %cur0 = call ptr @stricta_alloc(i64 %ccap0)\n"
        "  store ptr %cur0, ptr %cur_p\n  store i32 0, ptr %clp\n  store i32 0, ptr %rip\n  store i32 0, ptr %ip\n  br label %dloop\ndloop:\n"
        "  %i = load i32, ptr %ip\n  %cond = icmp slt i32 %i, %al\n  br i1 %cond, label %dbody, label %dfin\ndbody:\n"
        "  %cl = load i32, ptr %clp\n  %cl2 = add i32 %cl, 2\n  %cl2_64 = sext i32 %cl2 to i64\n"
        "  %ccap = load i64, ptr %ccap_p\n  %needg = icmp ugt i64 %cl2_64, %ccap\n"
        "  br i1 %needg, label %grow1, label %append\ngrow1:\n"
        "  %cl16 = add i32 %cl, 16\n  %ncap1 = sext i32 %cl16 to i64\n"
        "  %oc1 = load ptr, ptr %cur_p\n  %ncur1 = call ptr @stricta_realloc(ptr %oc1, i64 %ncap1)\n"
        "  store ptr %ncur1, ptr %cur_p\n  store i64 %ncap1, ptr %ccap_p\n  br label %append\nappend:\n"
        "  %cur1 = load ptr, ptr %cur_p\n  %ap = getelementptr i8, ptr %ad, i32 %i\n  %ch = load i8, ptr %ap\n"
        "  %cp1 = getelementptr i8, ptr %cur1, i32 %cl\n  store i8 %ch, ptr %cp1\n"
        "  %cln = add i32 %cl, 1\n  %cp2 = getelementptr i8, ptr %cur1, i32 %cln\n  store i8 0, ptr %cp2\n"
        "  store i32 %cln, ptr %clp\n  call void @_bi_strip(ptr %cur1, ptr %clp)\n"
        "  %cl_s = load i32, ptr %clp\n"
        "  %cmp0 = call i32 @_bi_abs_cmp(ptr %cur1, i32 %cl_s, ptr %bd, i32 %bl)\n"
        "  %islt = icmp slt i32 %cmp0, 0\n  br i1 %islt, label %pushz, label %qinit\npushz:\n"
        "  %ri0 = load i32, ptr %rip\n  %rgt = icmp sgt i32 %ri0, 0\n  br i1 %rgt, label %pz2, label %dnext\npz2:\n"
        "  %rpz = getelementptr i8, ptr %res, i32 %ri0\n  store i8 48, ptr %rpz\n"
        "  %nri0 = add i32 %ri0, 1\n  store i32 %nri0, ptr %rip\n  br label %dnext\nqinit:\n"
        "  %qp = alloca i32\n  %cmpp = alloca i32\n  store i32 0, ptr %qp\n  store i32 %cmp0, ptr %cmpp\n  br label %qloop\nqloop:\n"
        "  %cm = load i32, ptr %cmpp\n  %ge0 = icmp sge i32 %cm, 0\n  br i1 %ge0, label %qbody, label %qdone\nqbody:\n"
        "  %cur2 = load ptr, ptr %cur_p\n  %cl2b = load i32, ptr %clp\n"
        "  %nc = call ptr @_bi_sub_abs(ptr %cur2, i32 %cl2b, ptr %bd, i32 %bl, ptr %olp)\n"
        "  %nl = load i32, ptr %olp\n  store i32 %nl, ptr %clp\n"
        "  %nl2 = add i32 %nl, 2\n  %nl2_64 = sext i32 %nl2 to i64\n"
        "  %ccap2 = load i64, ptr %ccap_p\n  %ng2 = icmp ugt i64 %nl2_64, %ccap2\n"
        "  br i1 %ng2, label %grow2, label %copync\ngrow2:\n"
        "  %nl16 = add i32 %nl, 16\n  %ncap2 = sext i32 %nl16 to i64\n"
        "  %ncur2 = call ptr @stricta_realloc(ptr %cur2, i64 %ncap2)\n"
        "  store ptr %ncur2, ptr %cur_p\n  store i64 %ncap2, ptr %ccap_p\n  br label %copync\ncopync:\n"
        "  %cur3 = load ptr, ptr %cur_p\n  %nl1 = add i32 %nl, 1\n  %nl1_64 = sext i32 %nl1 to i64\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %cur3, ptr %nc, i64 %nl1_64, i1 false)\n"
        "  call void @free(ptr %nc)\n"
        "  %qv = load i32, ptr %qp\n  %nqv = add i32 %qv, 1\n  store i32 %nqv, ptr %qp\n"
        "  %ncmp = call i32 @_bi_abs_cmp(ptr %cur3, i32 %nl, ptr %bd, i32 %bl)\n"
        "  store i32 %ncmp, ptr %cmpp\n  br label %qloop\nqdone:\n"
        "  %fq = load i32, ptr %qp\n  %qd = add i32 %fq, 48\n  %q8 = trunc i32 %qd to i8\n"
        "  %ri1 = load i32, ptr %rip\n  %rpq = getelementptr i8, ptr %res, i32 %ri1\n  store i8 %q8, ptr %rpq\n"
        "  %nri1 = add i32 %ri1, 1\n  store i32 %nri1, ptr %rip\n  br label %dnext\ndnext:\n"
        "  %ni = add i32 %i, 1\n  store i32 %ni, ptr %ip\n  br label %dloop\ndfin:\n"
        "  %fcl = load i32, ptr %clp\n  %fcur = load ptr, ptr %cur_p\n"
        "  %cl1 = icmp eq i32 %fcl, 1\n  %fc0 = load i8, ptr %fcur\n  %fz0 = icmp eq i8 %fc0, 48\n"
        "  %remz = and i1 %cl1, %fz0\n  %hasrem = xor i1 %remz, true\n"
        "  call void @free(ptr %fcur)\n"
        "  %fri = load i32, ptr %rip\n  %riz = icmp eq i32 %fri, 0\n  br i1 %riz, label %setz, label %mkq\nsetz:\n"
        "  store i8 48, ptr %res\n  store i32 1, ptr %rip\n  br label %mkq\nmkq:\n"
        "  %fri2 = load i32, ptr %rip\n  %rpe = getelementptr i8, ptr %res, i32 %fri2\n  store i8 0, ptr %rpe\n"
        "  %qsign = mul i32 %as, %bs\n"
        "  %q = call ptr @_bi_make(ptr %res, i32 %fri2, i32 %qsign)\n"
        "  %neqs = icmp ne i32 %as, %bs\n  %adj = and i1 %neqs, %hasrem\n"
        "  br i1 %adj, label %doadj, label %retq\ndoadj:\n"
        "  %one = call ptr @stricta_bigint_from_str(ptr @.rt_one)\n"
        "  %qadj = call ptr @stricta_bigint_sub(ptr %q, ptr %one)\n"
        "  call void @stricta_bigint_free(ptr %q)\n  call void @stricta_bigint_free(ptr %one)\n"
        "  ret ptr %qadj\nretq:\n  ret ptr %q\n}\n\n");
    sb_append(b,
        "define internal void @stricta_bigint_free(ptr %a) {\nentry:\n"
        "  %ok = icmp ne ptr %a, null\n  br i1 %ok, label %dofree, label %done\ndofree:\n"
        "  %dp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 2\n  %d = load ptr, ptr %dp\n"
        "  call void @free(ptr %d)\n  call void @free(ptr %a)\n  br label %done\ndone:\n  ret void\n}\n\n"
        "define internal ptr @stricta_bigint_clone(ptr %a) {\nentry:\n"
        "  %ok = icmp ne ptr %a, null\n  br i1 %ok, label %work, label %retn\nretn:\n  ret ptr null\nwork:\n"
        "  %lp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %sp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 1\n  %sign = load i32, ptr %sp\n"
        "  %dp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 2\n  %d0 = load ptr, ptr %dp\n"
        "  %l1 = add i32 %len, 1\n  %l1_64 = sext i32 %l1 to i64\n"
        "  %d = call ptr @stricta_alloc(i64 %l1_64)\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %d0, i64 %l1_64, i1 false)\n"
        "  %r = call ptr @_bi_make(ptr %d, i32 %len, i32 %sign)\n  ret ptr %r\n}\n\n"
        "define internal ptr @stricta_bigint_mod(ptr %a, ptr %b) {\nentry:\n"
        "  %q = call ptr @stricta_bigint_div(ptr %a, ptr %b)\n"
        "  %p = call ptr @stricta_bigint_mul(ptr %q, ptr %b)\n"
        "  %r = call ptr @stricta_bigint_sub(ptr %a, ptr %p)\n"
        "  call void @stricta_bigint_free(ptr %q)\n  call void @stricta_bigint_free(ptr %p)\n"
        "  ret ptr %r\n}\n\n"
        "define internal ptr @stricta_bigint_pow(ptr %a, ptr %b) {\nentry:\n"
        "  %bsp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 1\n  %bs = load i32, ptr %bsp\n"
        "  %neg = icmp slt i32 %bs, 0\n  br i1 %neg, label %ret0, label %init\nret0:\n"
        "  %r0 = call ptr @stricta_bigint_from_str(ptr @.rt_zero)\n  ret ptr %r0\ninit:\n"
        "  %bdp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 2\n  %bd = load ptr, ptr %bdp\n"
        "  %e0 = call i64 @strtoll(ptr %bd, ptr null, i32 10)\n"
        "  %ep = alloca i64\n  %rp = alloca ptr\n  %bp = alloca ptr\n"
        "  store i64 %e0, ptr %ep\n"
        "  %one = call ptr @stricta_bigint_from_str(ptr @.rt_one)\n  store ptr %one, ptr %rp\n"
        "  %base0 = call ptr @stricta_bigint_clone(ptr %a)\n  store ptr %base0, ptr %bp\n"
        "  br label %loop\nloop:\n"
        "  %e = load i64, ptr %ep\n  %gt0 = icmp sgt i64 %e, 0\n  br i1 %gt0, label %body, label %done\nbody:\n"
        "  %odd = and i64 %e, 1\n  %is1 = icmp ne i64 %odd, 0\n  br i1 %is1, label %mulr, label %sh\nmulr:\n"
        "  %r1 = load ptr, ptr %rp\n  %b1 = load ptr, ptr %bp\n"
        "  %nr = call ptr @stricta_bigint_mul(ptr %r1, ptr %b1)\n"
        "  call void @stricta_bigint_free(ptr %r1)\n  store ptr %nr, ptr %rp\n  br label %sh\nsh:\n"
        "  %ne = ashr i64 %e, 1\n  store i64 %ne, ptr %ep\n"
        "  %gtn = icmp sgt i64 %ne, 0\n  br i1 %gtn, label %sqb, label %loop\nsqb:\n"
        "  %b2 = load ptr, ptr %bp\n  %nb = call ptr @stricta_bigint_mul(ptr %b2, ptr %b2)\n"
        "  call void @stricta_bigint_free(ptr %b2)\n  store ptr %nb, ptr %bp\n  br label %loop\ndone:\n"
        "  %fb = load ptr, ptr %bp\n  call void @stricta_bigint_free(ptr %fb)\n"
        "  %fr = load ptr, ptr %rp\n  ret ptr %fr\n}\n\n"
        "define internal ptr @stricta_bigint_neg(ptr %a) {\nentry:\n"
        "  %lp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %sp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 1\n  %sign = load i32, ptr %sp\n"
        "  %dp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 2\n  %d0 = load ptr, ptr %dp\n"
        "  %l1 = add i32 %len, 1\n  %l1_64 = sext i32 %l1 to i64\n"
        "  %d = call ptr @stricta_alloc(i64 %l1_64)\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %d0, i64 %l1_64, i1 false)\n"
        "  %ns = sub i32 0, %sign\n"
        "  %r = call ptr @_bi_make(ptr %d, i32 %len, i32 %ns)\n  ret ptr %r\n}\n\n"
        "define internal ptr @stricta_bigint_abs(ptr %a) {\nentry:\n"
        "  %lp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %dp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 2\n  %d0 = load ptr, ptr %dp\n"
        "  %l1 = add i32 %len, 1\n  %l1_64 = sext i32 %l1 to i64\n"
        "  %d = call ptr @stricta_alloc(i64 %l1_64)\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %d, ptr %d0, i64 %l1_64, i1 false)\n"
        "  %r = call ptr @_bi_make(ptr %d, i32 %len, i32 1)\n  ret ptr %r\n}\n\n"
        "define internal i32 @stricta_bigint_cmp(ptr %a, ptr %b) {\nentry:\n"
        "  %asp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 1\n  %as = load i32, ptr %asp\n"
        "  %bsp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 1\n  %bs = load i32, ptr %bsp\n"
        "  %neq = icmp ne i32 %as, %bs\n  br i1 %neq, label %bysign, label %byabs\nbysign:\n"
        "  %lt = icmp slt i32 %as, %bs\n  %r1 = select i1 %lt, i32 -1, i32 1\n  ret i32 %r1\nbyabs:\n"
        "  %alp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 0\n  %al = load i32, ptr %alp\n"
        "  %blp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 0\n  %bl = load i32, ptr %blp\n"
        "  %adp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 2\n  %ad = load ptr, ptr %adp\n"
        "  %bdp = getelementptr %StrictaBigInt, ptr %b, i32 0, i32 2\n  %bd = load ptr, ptr %bdp\n"
        "  %c = call i32 @_bi_abs_cmp(ptr %ad, i32 %al, ptr %bd, i32 %bl)\n"
        "  %pos = icmp sgt i32 %as, 0\n  %nc = sub i32 0, %c\n  %r2 = select i1 %pos, i32 %c, i32 %nc\n  ret i32 %r2\n}\n\n"
        "define internal ptr @stricta_bigint_to_str(ptr %a) {\nentry:\n"
        "  %lp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 0\n  %len = load i32, ptr %lp\n"
        "  %sp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 1\n  %sign = load i32, ptr %sp\n"
        "  %dp = getelementptr %StrictaBigInt, ptr %a, i32 0, i32 2\n  %d = load ptr, ptr %dp\n"
        "  %neg = icmp slt i32 %sign, 0\n  %l1 = icmp eq i32 %len, 1\n  %c0 = load i8, ptr %d\n"
        "  %z0 = icmp eq i8 %c0, 48\n  %isz = and i1 %l1, %z0\n  %n0 = xor i1 %isz, true\n"
        "  %isneg = and i1 %neg, %n0\n  br i1 %isneg, label %wneg, label %wpos\nwneg:\n"
        "  %len2 = add i32 %len, 2\n  %l2_64 = sext i32 %len2 to i64\n"
        "  %buf = call ptr @stricta_alloc(i64 %l2_64)\n  store i8 45, ptr %buf\n"
        "  %dst = getelementptr i8, ptr %buf, i32 1\n"
        "  %len1 = add i32 %len, 1\n  %l1_64 = sext i32 %len1 to i64\n"
        "  call void @llvm.memcpy.p0.p0.i64(ptr %dst, ptr %d, i64 %l1_64, i1 false)\n"
        "  %r1 = call ptr @stricta_str_new(ptr %buf, i32 %len1)\n"
        "  call void @free(ptr %buf)\n  ret ptr %r1\nwpos:\n"
        "  %r2 = call ptr @stricta_str_new(ptr %d, i32 %len)\n  ret ptr %r2\n}\n\n");
}

static void emit_program_llvm(ModuleInfo* main_mod, StrBuf* out) {
    cg_init();
    analyze_can_raise();

    /* Header: self-contained LLVM IR runtime & libc declarations (no external runtime.c needed) */
    sb_append(&g_cg.hdr, "; Stricta Static LLVM IR Compiler Output\n\n");
    emit_llvm_rt_core(&g_cg.hdr);
    emit_llvm_rt_str(&g_cg.hdr);
    emit_llvm_rt_bytes_file_task(&g_cg.hdr);
    emit_llvm_rt_bigint(&g_cg.hdr);

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
            if (type_is_owned(gv->type)) {
                gv->drop_flag_llvm = arena_sprintf("@_alive_%s%s", mod->prefix, gv->name);
                sb_printf(&globs, "%s = global i1 false\n", gv->drop_flag_llvm);
            }
        }
        for (int f = 0; f < mod->func_count; f++) {
            FuncInfo* fn = mod->funcs[f];
            if (fn->mod == mod && !fn->is_generic && !fn->is_extern && fn->ast && fn->ast->def_s.deco_count > 0) {
                sb_printf(&globs, "@_decfn_%s%s = global ptr %s\n", mod->prefix, fn->name, fn->llvm_name);
            }
        }
    }
    const char* ext_syms[256];
    int ext_cnt = 0;
    static const char* predeclared_syms[] = {
        "@malloc", "@calloc", "@realloc", "@free", "@memcmp", "@strlen",
        "@strstr", "@strchr", "@strtoll", "@strtod", "@printf", "@dprintf",
        "@snprintf", "@putchar", "@getchar", "@fflush", "@fopen", "@fseek",
        "@ftell", "@fread", "@fwrite", "@fclose", "@exit", "@pow",
        "@pthread_create", "@pthread_join", "@stricta_alloc", "@stricta_free",
        NULL
    };
    for (int k = 0; predeclared_syms[k]; k++) ext_syms[ext_cnt++] = predeclared_syms[k];
    for (int i = 0; i < g_all_func_count; i++) {
        FuncInfo* fn = g_all_funcs[i];
        if (!fn->is_extern) continue;
        bool seen = false;
        for (int k = 0; k < ext_cnt; k++) {
            if (strcmp(ext_syms[k], fn->llvm_name) == 0) { seen = true; break; }
        }
        if (seen) continue;
        if (ext_cnt < 256) ext_syms[ext_cnt++] = fn->llvm_name;
        sb_printf(&globs, "declare %s %s(", llvm_reg_ty(fn->ret_type), fn->llvm_name);
        for (int p = 0; p < fn->param_count; p++) {
            if (p > 0) sb_append(&globs, ", ");
            sb_append(&globs, llvm_reg_ty(fn->param_types[p]));
        }
        sb_append(&globs, ")\n");
    }
    sb_append(&globs, "\n");

    /* Emit all user functions & methods first (note: some statements inside functions might register mono types via dict.items() etc.) */
    for (int i = 0; i < g_all_func_count; i++) {
        if (g_all_funcs[i]->is_generic || g_all_funcs[i]->is_extern) continue;
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
        g_cg.temp_count = 0;
        g_cg.cur_entry.len = 0; g_cg.cur_entry.data[0] = '\0';
        g_cg.cur_body.len = 0; g_cg.cur_body.data[0] = '\0';
        VarSym* dn = find_global_in_mod(mod, "__name__");
        if (dn) {
            const char* sv = cg_strictastr_lit(mod->name, strlen(mod->name));
            cg_emit("store ptr %s, ptr %s", sv, dn->llvm_name);
            if (dn->drop_flag_llvm) cg_emit("store i1 true, ptr %s", dn->drop_flag_llvm);
        }
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
    g_cg.temp_count = 0;
    g_cg.cur_entry.len = 0; g_cg.cur_entry.data[0] = '\0';
    g_cg.cur_body.len = 0; g_cg.cur_body.data[0] = '\0';
    VarSym* main_dn = find_global_in_mod(main_mod, "__name__");
    if (main_dn) {
        const char* sv = cg_strictastr_lit(main_mod->name, strlen(main_mod->name));
        cg_emit("store ptr %s, ptr %s", sv, main_dn->llvm_name);
        if (main_dn->drop_flag_llvm) cg_emit("store i1 true, ptr %s", main_dn->drop_flag_llvm);
    }
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
        if (fn->mod == main_mod && !fn->is_extern && fn->ast && fn->ast->def_s.deco_count > 0) {
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
    for (int m = 0; m < g_module_count; m++) {
        ModuleInfo* mod = g_modules[m];
        for (int g = 0; g < mod->global_count; g++) {
            cg_drop_var_if_alive(&mod->globals[g]);
        }
    }
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
        int prev_oc = g_mono_opts.count;
        int prev_tc = g_mono_tuples.count;
        int prev_lc = g_mono_lists.count;
        int prev_sc = g_mono_sets.count;
        int prev_dc = g_mono_dicts.count;
        while (di < g_mono_dicts.count) emit_mono_dict(g_mono_dicts.items[di++]);
        while (si < g_mono_sets.count) emit_mono_set(g_mono_sets.items[si++]);
        while (li < g_mono_lists.count) emit_mono_list(g_mono_lists.items[li++]);
        for (int k = prev_oc; k < g_mono_opts.count; k++) {
            Type* t = g_mono_opts.items[k];
            sb_printf(&g_cg.hdr, "%%%s = type { i8, %s }\n", t->mangled, llvm_mem_ty(t->elem));
        }
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
        for (int k = prev_sc; k < g_mono_sets.count; k++) {
            Type* t = g_mono_sets.items[k];
            sb_printf(&g_cg.hdr, "%%%s = type { ptr, ptr, i32, i32, i32 }\n", t->mangled);
        }
        for (int k = prev_dc; k < g_mono_dicts.count; k++) {
            Type* t = g_mono_dicts.items[k];
            sb_printf(&g_cg.hdr, "%%%s = type { ptr, ptr, ptr, i32, i32, i32, i32 }\n", t->mangled);
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

int main(int argc, char** argv) {
    {
        char* ecopy = strdup(argv[0]);
        g_exe_dir = arena_strdup(dirname(ecopy));
        free(ecopy);
    }
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <input.str> [-o <output>] [-I <dir>] [-M <[scope:]alias=dir>]\n", argv[0]);
        return 1;
    }
    const char* input_file = NULL;
    const char* output_file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-I") == 0 && i + 1 < argc) {
            if (g_import_dir_count < 64) g_import_dirs[g_import_dir_count++] = argv[++i];
        } else if (strncmp(argv[i], "-I", 2) == 0 && argv[i][2] != '\0') {
            if (g_import_dir_count < 64) g_import_dirs[g_import_dir_count++] = argv[i] + 2;
        } else if (strcmp(argv[i], "-M") == 0 && i + 1 < argc) {
            add_mod_map(argv[++i]);
        } else if (strncmp(argv[i], "-M", 2) == 0 && argv[i][2] != '\0') {
            add_mod_map(argv[i] + 2);
        } else if (!input_file) {
            input_file = argv[i];
        }
    }
    if (!input_file) {
        fprintf(stderr, "Usage: %s <input.str> [-o <output>] [-I <dir>] [-M <[scope:]alias=dir>]\n", argv[0]);
        return 1;
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
    check_program_ownership();

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

    char cmd[4096];
    snprintf(cmd, sizeof(cmd), "clang -O2 -Wno-override-module '%s' -lm -lpthread -o '%s'", tmp_ll, output_file);
    int rc = system(cmd);
    remove(tmp_ll);
    return rc == 0 ? 0 : 1;
}


