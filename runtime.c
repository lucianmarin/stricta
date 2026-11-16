#include "runtime.h"
#include <ctype.h>

/* --- Memory --- */

void* stricta_alloc(size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) {
        fprintf(stderr, "MemoryError: out of memory\n");
        exit(1);
    }
    return p;
}

void* stricta_calloc(size_t count, size_t size) {
    void* p = calloc(count ? count : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "MemoryError: out of memory\n");
        exit(1);
    }
    return p;
}

void* stricta_realloc(void* ptr, size_t size) {
    void* p = realloc(ptr, size ? size : 1);
    if (!p) {
        fprintf(stderr, "MemoryError: out of memory\n");
        exit(1);
    }
    return p;
}

void stricta_free(void* ptr) {
    free(ptr);
}

/* --- CLI Args --- */

StrictaPtrList* stricta_sys_argv = NULL;

void stricta_init_args(int32_t argc, char** argv) {
    StrictaPtrList* lst = (StrictaPtrList*)stricta_alloc(sizeof(StrictaPtrList));
    lst->length = argc;
    lst->capacity = argc > 4 ? argc : 4;
    lst->items = (void**)stricta_alloc(sizeof(void*) * (size_t)lst->capacity);
    for (int32_t i = 0; i < argc; i++) {
        lst->items[i] = stricta_str_cstr(argv[i]);
    }
    stricta_sys_argv = lst;
}

/* --- Exception Handling --- */

#define STRICTA_MAX_TRY_DEPTH 128
static jmp_buf _try_stack[STRICTA_MAX_TRY_DEPTH];
static int32_t _try_depth = 0;
static int32_t _exc_type_id = 0;
static void* _exc_obj = NULL;
static StrictaStr* _exc_msg = NULL;

jmp_buf* stricta_try_push(void) {
    if (_try_depth >= STRICTA_MAX_TRY_DEPTH) {
        fprintf(stderr, "RuntimeError: maximum try nesting depth exceeded\n");
        exit(1);
    }
    return &_try_stack[_try_depth++];
}

void stricta_try_pop(void) {
    if (_try_depth > 0) _try_depth--;
}

void stricta_raise(int32_t type_id, void* exc_obj, const char* msg) {
    _exc_type_id = type_id;
    _exc_obj = exc_obj;
    _exc_msg = msg ? stricta_str_cstr(msg) : stricta_str_cstr("");
    if (_try_depth > 0) {
        longjmp(_try_stack[_try_depth - 1], 1);
    }
    if (msg && msg[0] != '\0') {
        fprintf(stderr, "Unhandled exception: %s\n", msg);
    } else {
        fprintf(stderr, "Unhandled exception (type_id=%d)\n", type_id);
    }
    exit(1);
}

void stricta_reraise(void) {
    if (_try_depth > 0) {
        longjmp(_try_stack[_try_depth - 1], 1);
    }
    if (_exc_msg && _exc_msg->length > 0) {
        fprintf(stderr, "Unhandled exception: %s\n", _exc_msg->chars);
    } else {
        fprintf(stderr, "Unhandled exception (type_id=%d)\n", _exc_type_id);
    }
    exit(1);
}

int32_t stricta_exc_type_id(void) { return _exc_type_id; }
void* stricta_exc_obj(void) { return _exc_obj; }
StrictaStr* stricta_exc_msg(void) { return _exc_msg ? _exc_msg : stricta_str_cstr(""); }

void stricta_assert_fail(const char* msg, const char* file, int32_t line) {
    if (msg && msg[0] != '\0') {
        fprintf(stderr, "AssertionError (%s:%d): %s\n", file ? file : "<unknown>", line, msg);
    } else {
        fprintf(stderr, "AssertionError (%s:%d)\n", file ? file : "<unknown>", line);
    }
    exit(1);
}

/* --- Strings --- */

static uint32_t fnv1a_32(const char* s, int32_t len) {
    uint32_t h = 2166136261u;
    for (int32_t i = 0; i < len; i++) {
        h ^= (uint8_t)s[i];
        h *= 16777619u;
    }
    return h ? h : 1u;
}

StrictaStr* stricta_str_new(const char* s, int32_t len) {
    StrictaStr* st = (StrictaStr*)stricta_alloc(sizeof(StrictaStr));
    st->length = len;
    st->hash = 0;
    st->chars = (char*)stricta_alloc((size_t)len + 1);
    if (len > 0 && s) memcpy(st->chars, s, (size_t)len);
    st->chars[len] = '\0';
    return st;
}

StrictaStr* stricta_str_cstr(const char* s) {
    if (!s) return stricta_str_new("", 0);
    return stricta_str_new(s, (int32_t)strlen(s));
}

StrictaStr* stricta_str_concat(StrictaStr* a, StrictaStr* b) {
    int32_t la = a ? a->length : 0;
    int32_t lb = b ? b->length : 0;
    StrictaStr* st = (StrictaStr*)stricta_alloc(sizeof(StrictaStr));
    st->length = la + lb;
    st->hash = 0;
    st->chars = (char*)stricta_alloc((size_t)st->length + 1);
    if (la > 0) memcpy(st->chars, a->chars, (size_t)la);
    if (lb > 0) memcpy(st->chars + la, b->chars, (size_t)lb);
    st->chars[st->length] = '\0';
    return st;
}

StrictaStr* stricta_str_repeat(StrictaStr* s, int64_t count) {
    if (!s || count <= 0 || s->length == 0) return stricta_str_new("", 0);
    int64_t total = (int64_t)s->length * count;
    StrictaStr* st = (StrictaStr*)stricta_alloc(sizeof(StrictaStr));
    st->length = (int32_t)total;
    st->hash = 0;
    st->chars = (char*)stricta_alloc((size_t)total + 1);
    for (int64_t i = 0; i < count; i++) {
        memcpy(st->chars + i * s->length, s->chars, (size_t)s->length);
    }
    st->chars[total] = '\0';
    return st;
}

StrictaStr* stricta_str_slice(StrictaStr* s, int64_t start, int64_t end) {
    if (!s) return stricta_str_new("", 0);
    int64_t len = s->length;
    if (start < 0) start += len;
    if (end < 0) end += len;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return stricta_str_new("", 0);
    return stricta_str_new(s->chars + start, (int32_t)(end - start));
}

StrictaStr* stricta_str_char_at(StrictaStr* s, int64_t idx) {
    if (!s || s->length == 0) {
        stricta_raise(1, NULL, "IndexError: string index out of range");
    }
    if (idx < 0) idx += s->length;
    if (idx < 0 || idx >= s->length) {
        stricta_raise(1, NULL, "IndexError: string index out of range");
    }
    return stricta_str_new(&s->chars[idx], 1);
}

int32_t stricta_str_eq(StrictaStr* a, StrictaStr* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->length != b->length) return 0;
    if (a->hash && b->hash && a->hash != b->hash) return 0;
    return memcmp(a->chars, b->chars, (size_t)a->length) == 0 ? 1 : 0;
}

int32_t stricta_str_cmp(StrictaStr* a, StrictaStr* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    int32_t minl = a->length < b->length ? a->length : b->length;
    int r = memcmp(a->chars, b->chars, (size_t)minl);
    if (r != 0) return r;
    if (a->length < b->length) return -1;
    if (a->length > b->length) return 1;
    return 0;
}

uint64_t stricta_str_hash(StrictaStr* s) {
    if (!s) return 0;
    if (s->hash == 0) {
        s->hash = fnv1a_32(s->chars, s->length);
    }
    uint64_t x = (uint64_t)s->hash;
    x = (x ^ (x >> 16)) * 0x45d9f3bULL;
    return x ^ (x >> 16);
}

int32_t stricta_str_contains(StrictaStr* sub, StrictaStr* s) {
    if (!sub || !s) return 0;
    if (sub->length == 0) return 1;
    if (sub->length > s->length) return 0;
    return strstr(s->chars, sub->chars) != NULL ? 1 : 0;
}

static void split_push_str(StrictaPtrList* lst, StrictaStr* item) {
    if (lst->length >= lst->capacity) {
        lst->capacity *= 2;
        lst->items = (void**)stricta_realloc(lst->items, sizeof(void*) * (size_t)lst->capacity);
    }
    lst->items[lst->length++] = item;
}

StrictaPtrList* stricta_str_split(StrictaStr* s, StrictaStr* sep) {
    StrictaPtrList* lst = (StrictaPtrList*)stricta_alloc(sizeof(StrictaPtrList));
    lst->length = 0;
    lst->capacity = 8;
    lst->items = (void**)stricta_alloc(sizeof(void*) * 8);
    if (!s) return lst;

    if (!sep) {
        int32_t i = 0;
        while (i < s->length) {
            while (i < s->length && isspace((unsigned char)s->chars[i])) i++;
            if (i >= s->length) break;
            int32_t start = i;
            while (i < s->length && !isspace((unsigned char)s->chars[i])) i++;
            split_push_str(lst, stricta_str_new(s->chars + start, i - start));
        }
        return lst;
    }

    if (sep->length == 0) {
        stricta_raise(2, NULL, "ValueError: empty separator");
    }

    int32_t pos = 0;
    while (pos <= s->length) {
        char* found = strstr(s->chars + pos, sep->chars);
        if (!found) {
            split_push_str(lst, stricta_str_new(s->chars + pos, s->length - pos));
            break;
        }
        int32_t end = (int32_t)(found - s->chars);
        split_push_str(lst, stricta_str_new(s->chars + pos, end - pos));
        pos = end + sep->length;
    }
    return lst;
}

StrictaStr* stricta_str_join(StrictaStr* sep, StrictaPtrList* list) {
    if (!list || list->length == 0) return stricta_str_new("", 0);
    int32_t seplen = sep ? sep->length : 0;
    size_t total = 0;
    for (int32_t i = 0; i < list->length; i++) {
        StrictaStr* item = (StrictaStr*)list->items[i];
        total += item ? (size_t)item->length : 0;
        if (i > 0) total += (size_t)seplen;
    }
    StrictaStr* out = (StrictaStr*)stricta_alloc(sizeof(StrictaStr));
    out->length = (int32_t)total;
    out->hash = 0;
    out->chars = (char*)stricta_alloc(total + 1);
    size_t off = 0;
    for (int32_t i = 0; i < list->length; i++) {
        if (i > 0 && seplen > 0) {
            memcpy(out->chars + off, sep->chars, (size_t)seplen);
            off += (size_t)seplen;
        }
        StrictaStr* item = (StrictaStr*)list->items[i];
        if (item && item->length > 0) {
            memcpy(out->chars + off, item->chars, (size_t)item->length);
            off += (size_t)item->length;
        }
    }
    out->chars[off] = '\0';
    return out;
}

StrictaStr* stricta_str_upper(StrictaStr* s) {
    if (!s) return stricta_str_new("", 0);
    StrictaStr* out = stricta_str_new(s->chars, s->length);
    for (int32_t i = 0; i < out->length; i++) out->chars[i] = (char)toupper((unsigned char)out->chars[i]);
    return out;
}

StrictaStr* stricta_str_lower(StrictaStr* s) {
    if (!s) return stricta_str_new("", 0);
    StrictaStr* out = stricta_str_new(s->chars, s->length);
    for (int32_t i = 0; i < out->length; i++) out->chars[i] = (char)tolower((unsigned char)out->chars[i]);
    return out;
}

StrictaStr* stricta_str_replace(StrictaStr* s, StrictaStr* old_s, StrictaStr* new_s) {
    if (!s || !old_s || !new_s || old_s->length == 0) return s ? s : stricta_str_new("", 0);
    int32_t count = 0;
    const char* p = s->chars;
    while ((p = strstr(p, old_s->chars)) != NULL) {
        count++;
        p += old_s->length;
    }
    if (count == 0) return s;
    size_t new_len = (size_t)s->length + (size_t)count * (size_t)(new_s->length - old_s->length);
    StrictaStr* out = (StrictaStr*)stricta_alloc(sizeof(StrictaStr));
    out->length = (int32_t)new_len;
    out->hash = 0;
    out->chars = (char*)stricta_alloc(new_len + 1);
    char* dst = out->chars;
    p = s->chars;
    const char* hit;
    while ((hit = strstr(p, old_s->chars)) != NULL) {
        size_t chunk = (size_t)(hit - p);
        memcpy(dst, p, chunk);
        dst += chunk;
        memcpy(dst, new_s->chars, (size_t)new_s->length);
        dst += new_s->length;
        p = hit + old_s->length;
    }
    size_t rem = strlen(p);
    memcpy(dst, p, rem);
    dst[rem] = '\0';
    return out;
}

int64_t stricta_str_find(StrictaStr* s, StrictaStr* sub) {
    if (!s || !sub) return -1;
    if (sub->length == 0) return 0;
    char* p = strstr(s->chars, sub->chars);
    return p ? (int64_t)(p - s->chars) : -1;
}

int64_t stricta_str_index(StrictaStr* s, StrictaStr* sub) {
    int64_t idx = stricta_str_find(s, sub);
    if (idx < 0) stricta_raise(2, NULL, "ValueError: substring not found");
    return idx;
}

StrictaStr* stricta_str_strip(StrictaStr* s) {
    if (!s) return stricta_str_new("", 0);
    int32_t l = 0, r = s->length;
    while (l < r && isspace((unsigned char)s->chars[l])) l++;
    while (r > l && isspace((unsigned char)s->chars[r - 1])) r--;
    return stricta_str_new(s->chars + l, r - l);
}

StrictaStr* stricta_str_lstrip(StrictaStr* s) {
    if (!s) return stricta_str_new("", 0);
    int32_t l = 0;
    while (l < s->length && isspace((unsigned char)s->chars[l])) l++;
    return stricta_str_new(s->chars + l, s->length - l);
}

StrictaStr* stricta_str_rstrip(StrictaStr* s) {
    if (!s) return stricta_str_new("", 0);
    int32_t r = s->length;
    while (r > 0 && isspace((unsigned char)s->chars[r - 1])) r--;
    return stricta_str_new(s->chars, r);
}

int32_t stricta_str_startswith(StrictaStr* s, StrictaStr* prefix) {
    if (!s || !prefix) return 0;
    if (prefix->length > s->length) return 0;
    return memcmp(s->chars, prefix->chars, (size_t)prefix->length) == 0 ? 1 : 0;
}

int32_t stricta_str_endswith(StrictaStr* s, StrictaStr* suffix) {
    if (!s || !suffix) return 0;
    if (suffix->length > s->length) return 0;
    return memcmp(s->chars + s->length - suffix->length, suffix->chars, (size_t)suffix->length) == 0 ? 1 : 0;
}

int64_t stricta_str_count(StrictaStr* s, StrictaStr* sub) {
    if (!s || !sub) return 0;
    if (sub->length == 0) return (int64_t)s->length + 1;
    int64_t count = 0;
    const char* p = s->chars;
    while ((p = strstr(p, sub->chars)) != NULL) {
        count++;
        p += sub->length;
    }
    return count;
}

/* --- Conversions & Formatting --- */

StrictaStr* stricta_int_to_str(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return stricta_str_new(buf, n);
}

StrictaStr* stricta_uint_to_str(uint64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return stricta_str_new(buf, n);
}

StrictaStr* stricta_float_to_str(double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.15g", v);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') && !strchr(buf, 'n') && !strchr(buf, 'i')) {
        if (n + 2 < (int)sizeof(buf)) {
            buf[n++] = '.';
            buf[n++] = '0';
            buf[n] = '\0';
        }
    }
    return stricta_str_new(buf, n);
}

StrictaStr* stricta_float_fmt(double v, int32_t precision) {
    char buf[128];
    if (precision < 0) precision = 6;
    if (precision > 50) precision = 50;
    int n = snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return stricta_str_new(buf, n);
}

StrictaStr* stricta_bool_to_str(int32_t v) {
    return v ? stricta_str_cstr("True") : stricta_str_cstr("False");
}

int64_t stricta_str_to_int(StrictaStr* s) {
    if (!s) return 0;
    char* end = NULL;
    long long v = strtoll(s->chars, &end, 10);
    return (int64_t)v;
}

double stricta_str_to_float(StrictaStr* s) {
    if (!s) return 0.0;
    return strtod(s->chars, NULL);
}

int64_t stricta_ord(StrictaStr* s) {
    if (!s || s->length == 0) return 0;
    return (uint8_t)s->chars[0];
}

StrictaStr* stricta_chr(int64_t code) {
    char buf[2] = { (char)code, '\0' };
    return stricta_str_new(buf, 1);
}

StrictaStr* stricta_hex(int64_t v) {
    char buf[32];
    int n = v < 0 ? snprintf(buf, sizeof(buf), "-0x%llx", (unsigned long long)(-v))
                  : snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return stricta_str_new(buf, n);
}

StrictaStr* stricta_bin(int64_t v) {
    char buf[80];
    int pos = 79;
    buf[pos] = '\0';
    uint64_t uv = v < 0 ? (uint64_t)(-v) : (uint64_t)v;
    if (uv == 0) {
        buf[--pos] = '0';
    } else {
        while (uv > 0) {
            buf[--pos] = (char)('0' + (uv & 1));
            uv >>= 1;
        }
    }
    buf[--pos] = 'b';
    buf[--pos] = '0';
    if (v < 0) buf[--pos] = '-';
    return stricta_str_cstr(buf + pos);
}

StrictaStr* stricta_oct(int64_t v) {
    char buf[32];
    int n = v < 0 ? snprintf(buf, sizeof(buf), "-0o%llo", (unsigned long long)(-v))
                  : snprintf(buf, sizeof(buf), "0o%llo", (unsigned long long)v);
    return stricta_str_new(buf, n);
}

int64_t stricta_pow_i64(int64_t base, int64_t exp) {
    if (exp < 0) return 0;
    int64_t res = 1;
    while (exp > 0) {
        if (exp & 1) res *= base;
        exp >>= 1;
        if (exp > 0) base *= base;
    }
    return res;
}

/* --- Bytes --- */

StrictaBytes* stricta_bytes_new(const uint8_t* data, int32_t len) {
    StrictaBytes* b = (StrictaBytes*)stricta_alloc(sizeof(StrictaBytes));
    b->length = len;
    b->_pad = 0;
    b->data = (uint8_t*)stricta_alloc((size_t)len + 1);
    if (len > 0 && data) memcpy(b->data, data, (size_t)len);
    b->data[len] = 0;
    return b;
}

int32_t stricta_bytes_eq(StrictaBytes* a, StrictaBytes* b) {
    if (a == b) return 1;
    if (!a || !b || a->length != b->length) return 0;
    return memcmp(a->data, b->data, (size_t)a->length) == 0 ? 1 : 0;
}

/* --- BigInt --- */

static void bi_strip(char* d, int* len) {
    int l = *len, i = 0;
    while (i < l - 1 && d[i] == '0') i++;
    if (i > 0) {
        memmove(d, d + i, (size_t)(l - i + 1));
        *len = l - i;
    }
}

static int bi_abs_cmp(const char* a, int al, const char* b, int bl) {
    if (al != bl) return al < bl ? -1 : 1;
    return memcmp(a, b, (size_t)al);
}

static char* bi_add_abs(const char* a, int al, const char* b, int bl, int* ol) {
    int m = (al > bl ? al : bl) + 1;
    char* rev = (char*)stricta_alloc((size_t)m + 1);
    int i = al - 1, j = bl - 1, c = 0, k = 0;
    while (i >= 0 || j >= 0 || c) {
        int s = c;
        if (i >= 0) s += a[i--] - '0';
        if (j >= 0) s += b[j--] - '0';
        rev[k++] = (char)((s % 10) + '0');
        c = s / 10;
    }
    char* res = (char*)stricta_alloc((size_t)k + 1);
    for (int p = 0; p < k; p++) res[p] = rev[k - 1 - p];
    res[k] = '\0';
    free(rev);
    *ol = k;
    return res;
}

static char* bi_sub_abs(const char* a, int al, const char* b, int bl, int* ol) {
    char* rev = (char*)stricta_alloc((size_t)al + 1);
    int i = al - 1, j = bl - 1, br = 0, k = 0;
    while (i >= 0 || j >= 0) {
        int d = br;
        if (i >= 0) d += a[i--] - '0';
        if (j >= 0) d -= b[j--] - '0';
        if (d < 0) { d += 10; br = -1; } else br = 0;
        rev[k++] = (char)(d + '0');
    }
    while (k > 1 && rev[k - 1] == '0') k--;
    char* res = (char*)stricta_alloc((size_t)k + 1);
    for (int p = 0; p < k; p++) res[p] = rev[k - 1 - p];
    res[k] = '\0';
    free(rev);
    *ol = k;
    return res;
}

static StrictaBigInt* bi_make(char* digits, int len, int sign) {
    bi_strip(digits, &len);
    if (len == 1 && digits[0] == '0') sign = 1;
    StrictaBigInt* b = (StrictaBigInt*)stricta_alloc(sizeof(StrictaBigInt));
    b->length = len;
    b->sign = sign;
    b->digits = digits;
    return b;
}

StrictaBigInt* stricta_bigint_from_str(const char* s) {
    if (!s || !*s) return bi_make(strdup("0"), 1, 1);
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    int len = (int)strlen(s);
    char* d = (char*)stricta_alloc((size_t)len + 1);
    memcpy(d, s, (size_t)len + 1);
    return bi_make(d, len, sign);
}

StrictaBigInt* stricta_bigint_from_i64(int64_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return stricta_bigint_from_str(buf);
}

StrictaBigInt* stricta_bigint_add(StrictaBigInt* a, StrictaBigInt* b) {
    int ol = 0;
    if (a->sign == b->sign) {
        return bi_make(bi_add_abs(a->digits, a->length, b->digits, b->length, &ol), ol, a->sign);
    }
    int c = bi_abs_cmp(a->digits, a->length, b->digits, b->length);
    if (c == 0) return stricta_bigint_from_str("0");
    if (c > 0) return bi_make(bi_sub_abs(a->digits, a->length, b->digits, b->length, &ol), ol, a->sign);
    return bi_make(bi_sub_abs(b->digits, b->length, a->digits, a->length, &ol), ol, b->sign);
}

StrictaBigInt* stricta_bigint_sub(StrictaBigInt* a, StrictaBigInt* b) {
    StrictaBigInt nb = *b;
    nb.sign = -b->sign;
    return stricta_bigint_add(a, &nb);
}

StrictaBigInt* stricta_bigint_mul(StrictaBigInt* a, StrictaBigInt* b) {
    int rl = a->length + b->length;
    int* tmp = (int*)stricta_calloc((size_t)rl + 4, sizeof(int));
    for (int i = a->length - 1; i >= 0; i--) {
        for (int j = b->length - 1; j >= 0; j--) {
            tmp[(a->length - 1 - i) + (b->length - 1 - j)] += (a->digits[i] - '0') * (b->digits[j] - '0');
        }
    }
    int carry = 0;
    for (int i = 0; i < rl; i++) {
        int t = tmp[i] + carry;
        tmp[i] = t % 10;
        carry = t / 10;
    }
    while (carry > 0) { tmp[rl++] = carry % 10; carry /= 10; }
    while (rl > 1 && tmp[rl - 1] == 0) rl--;
    char* res = (char*)stricta_alloc((size_t)rl + 1);
    for (int i = 0; i < rl; i++) res[i] = (char)(tmp[rl - 1 - i] + '0');
    res[rl] = '\0';
    free(tmp);
    return bi_make(res, rl, a->sign * b->sign);
}

StrictaBigInt* stricta_bigint_div(StrictaBigInt* a, StrictaBigInt* b) {
    if (b->length == 1 && b->digits[0] == '0') {
        stricta_raise(3, NULL, "ZeroDivisionError: integer division by zero");
    }
    if (bi_abs_cmp(a->digits, a->length, b->digits, b->length) < 0) {
        if (a->sign == b->sign || (a->length == 1 && a->digits[0] == '0')) return stricta_bigint_from_str("0");
        return stricta_bigint_from_str("-1");
    }
    char* res = (char*)stricta_calloc((size_t)a->length + 1, 1);
    int ri = 0;
    char* cur = (char*)stricta_alloc((size_t)a->length + 1);
    int cl = 0;
    for (int i = 0; i < a->length; i++) {
        cur[cl++] = a->digits[i];
        cur[cl] = '\0';
        bi_strip(cur, &cl);
        int cmp = bi_abs_cmp(cur, cl, b->digits, b->length);
        if (cmp < 0) { if (ri > 0) res[ri++] = '0'; continue; }
        int q = 0;
        while (cmp >= 0) {
            char* nc = bi_sub_abs(cur, cl, b->digits, b->length, &cl);
            free(cur); cur = nc; q++;
            cmp = bi_abs_cmp(cur, cl, b->digits, b->length);
        }
        res[ri++] = (char)(q + '0');
    }
    int has_rem = !(cl == 1 && cur[0] == '0');
    free(cur);
    if (ri == 0) res[ri++] = '0';
    res[ri] = '\0';
    StrictaBigInt* q = bi_make(res, ri, a->sign * b->sign);
    if (a->sign != b->sign && has_rem) {
        StrictaBigInt* one = stricta_bigint_from_str("1");
        StrictaBigInt* adj = stricta_bigint_sub(q, one);
        return adj;
    }
    return q;
}

StrictaBigInt* stricta_bigint_mod(StrictaBigInt* a, StrictaBigInt* b) {
    StrictaBigInt* q = stricta_bigint_div(a, b);
    StrictaBigInt* p = stricta_bigint_mul(q, b);
    return stricta_bigint_sub(a, p);
}

StrictaBigInt* stricta_bigint_pow(StrictaBigInt* a, StrictaBigInt* b) {
    if (b->sign < 0) return stricta_bigint_from_str("0");
    long long e = strtoll(b->digits, NULL, 10);
    StrictaBigInt* res = stricta_bigint_from_str("1");
    StrictaBigInt* base = a;
    while (e > 0) {
        if (e & 1) res = stricta_bigint_mul(res, base);
        e >>= 1;
        if (e > 0) base = stricta_bigint_mul(base, base);
    }
    return res;
}

StrictaBigInt* stricta_bigint_neg(StrictaBigInt* a) {
    char* d = strdup(a->digits);
    return bi_make(d, a->length, -a->sign);
}

StrictaBigInt* stricta_bigint_abs(StrictaBigInt* a) {
    char* d = strdup(a->digits);
    return bi_make(d, a->length, 1);
}

int32_t stricta_bigint_cmp(StrictaBigInt* a, StrictaBigInt* b) {
    if (a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
    int c = bi_abs_cmp(a->digits, a->length, b->digits, b->length);
    return a->sign > 0 ? c : -c;
}

StrictaStr* stricta_bigint_to_str(StrictaBigInt* a) {
    if (a->sign < 0 && !(a->length == 1 && a->digits[0] == '0')) {
        char* buf = (char*)stricta_alloc((size_t)a->length + 2);
        buf[0] = '-';
        memcpy(buf + 1, a->digits, (size_t)a->length + 1);
        StrictaStr* s = stricta_str_new(buf, a->length + 1);
        free(buf);
        return s;
    }
    return stricta_str_new(a->digits, a->length);
}

/* --- File I/O --- */

StrictaFile* stricta_open(StrictaStr* path, StrictaStr* mode) {
    const char* m = (mode && mode->length > 0) ? mode->chars : "r";
    FILE* h = fopen(path ? path->chars : "", m);
    if (!h) stricta_raise(4, NULL, "FileNotFoundError: cannot open file");
    StrictaFile* f = (StrictaFile*)stricta_alloc(sizeof(StrictaFile));
    f->handle = h;
    return f;
}

StrictaStr* stricta_file_read(StrictaFile* f) {
    if (!f || !f->handle) return stricta_str_new("", 0);
    fseek(f->handle, 0, SEEK_END);
    long sz = ftell(f->handle);
    fseek(f->handle, 0, SEEK_SET);
    if (sz <= 0) return stricta_str_new("", 0);
    char* buf = (char*)stricta_alloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f->handle);
    buf[n] = '\0';
    StrictaStr* s = stricta_str_new(buf, (int32_t)n);
    free(buf);
    return s;
}

void stricta_file_write(StrictaFile* f, StrictaStr* text) {
    if (f && f->handle && text && text->length > 0) {
        fwrite(text->chars, 1, (size_t)text->length, f->handle);
    }
}

void stricta_file_close(StrictaFile* f) {
    if (f && f->handle) {
        fclose(f->handle);
        f->handle = NULL;
    }
}

/* --- Print & Input --- */

void stricta_print_i64(int64_t v) { printf("%lld", (long long)v); }
void stricta_print_u64(uint64_t v) { printf("%llu", (unsigned long long)v); }
void stricta_print_f64(double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.15g", v);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') && !strchr(buf, 'n') && !strchr(buf, 'i')) {
        if (n + 2 < (int)sizeof(buf)) {
            buf[n++] = '.';
            buf[n++] = '0';
            buf[n] = '\0';
        }
    }
    fputs(buf, stdout);
}
void stricta_print_bool(int32_t v) { fputs(v ? "True" : "False", stdout); }
void stricta_print_str(StrictaStr* s) { if (s && s->length > 0) fwrite(s->chars, 1, (size_t)s->length, stdout); }
void stricta_print_cstr(const char* s) { if (s) fputs(s, stdout); }
void stricta_print_space(void) { putchar(' '); }
void stricta_print_newline(void) { putchar('\n'); }

StrictaStr* stricta_input(StrictaStr* prompt) {
    if (prompt && prompt->length > 0) {
        fwrite(prompt->chars, 1, (size_t)prompt->length, stdout);
        fflush(stdout);
    }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return stricta_str_new("", 0);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    return stricta_str_new(buf, (int32_t)len);
}
