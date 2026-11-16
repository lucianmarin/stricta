#include "runtime.h"
#include <ctype.h>

/* --- Memory --- */

void* turbo_alloc(size_t size) {
    void* p = malloc(size ? size : 1);
    if (!p) {
        fprintf(stderr, "MemoryError: out of memory\n");
        exit(1);
    }
    return p;
}

void* turbo_calloc(size_t count, size_t size) {
    void* p = calloc(count ? count : 1, size ? size : 1);
    if (!p) {
        fprintf(stderr, "MemoryError: out of memory\n");
        exit(1);
    }
    return p;
}

void* turbo_realloc(void* ptr, size_t size) {
    void* p = realloc(ptr, size ? size : 1);
    if (!p) {
        fprintf(stderr, "MemoryError: out of memory\n");
        exit(1);
    }
    return p;
}

void turbo_free(void* ptr) {
    free(ptr);
}

/* --- CLI Args --- */

TurboPtrList* turbo_sys_argv = NULL;

void turbo_init_args(int32_t argc, char** argv) {
    TurboPtrList* lst = (TurboPtrList*)turbo_alloc(sizeof(TurboPtrList));
    lst->length = argc;
    lst->capacity = argc > 4 ? argc : 4;
    lst->items = (void**)turbo_alloc(sizeof(void*) * (size_t)lst->capacity);
    for (int32_t i = 0; i < argc; i++) {
        lst->items[i] = turbo_str_cstr(argv[i]);
    }
    turbo_sys_argv = lst;
}

/* --- Exception Handling --- */

#define TURBO_MAX_TRY_DEPTH 128
static jmp_buf _try_stack[TURBO_MAX_TRY_DEPTH];
static int32_t _try_depth = 0;
static int32_t _exc_type_id = 0;
static void* _exc_obj = NULL;
static TurboStr* _exc_msg = NULL;

jmp_buf* turbo_try_push(void) {
    if (_try_depth >= TURBO_MAX_TRY_DEPTH) {
        fprintf(stderr, "RuntimeError: maximum try nesting depth exceeded\n");
        exit(1);
    }
    return &_try_stack[_try_depth++];
}

void turbo_try_pop(void) {
    if (_try_depth > 0) _try_depth--;
}

void turbo_raise(int32_t type_id, void* exc_obj, const char* msg) {
    _exc_type_id = type_id;
    _exc_obj = exc_obj;
    _exc_msg = msg ? turbo_str_cstr(msg) : turbo_str_cstr("");
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

void turbo_reraise(void) {
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

int32_t turbo_exc_type_id(void) { return _exc_type_id; }
void* turbo_exc_obj(void) { return _exc_obj; }
TurboStr* turbo_exc_msg(void) { return _exc_msg ? _exc_msg : turbo_str_cstr(""); }

void turbo_assert_fail(const char* msg, const char* file, int32_t line) {
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

TurboStr* turbo_str_new(const char* s, int32_t len) {
    TurboStr* st = (TurboStr*)turbo_alloc(sizeof(TurboStr));
    st->length = len;
    st->hash = 0;
    st->chars = (char*)turbo_alloc((size_t)len + 1);
    if (len > 0 && s) memcpy(st->chars, s, (size_t)len);
    st->chars[len] = '\0';
    return st;
}

TurboStr* turbo_str_cstr(const char* s) {
    if (!s) return turbo_str_new("", 0);
    return turbo_str_new(s, (int32_t)strlen(s));
}

TurboStr* turbo_str_concat(TurboStr* a, TurboStr* b) {
    int32_t la = a ? a->length : 0;
    int32_t lb = b ? b->length : 0;
    TurboStr* st = (TurboStr*)turbo_alloc(sizeof(TurboStr));
    st->length = la + lb;
    st->hash = 0;
    st->chars = (char*)turbo_alloc((size_t)st->length + 1);
    if (la > 0) memcpy(st->chars, a->chars, (size_t)la);
    if (lb > 0) memcpy(st->chars + la, b->chars, (size_t)lb);
    st->chars[st->length] = '\0';
    return st;
}

TurboStr* turbo_str_repeat(TurboStr* s, int64_t count) {
    if (!s || count <= 0 || s->length == 0) return turbo_str_new("", 0);
    int64_t total = (int64_t)s->length * count;
    TurboStr* st = (TurboStr*)turbo_alloc(sizeof(TurboStr));
    st->length = (int32_t)total;
    st->hash = 0;
    st->chars = (char*)turbo_alloc((size_t)total + 1);
    for (int64_t i = 0; i < count; i++) {
        memcpy(st->chars + i * s->length, s->chars, (size_t)s->length);
    }
    st->chars[total] = '\0';
    return st;
}

TurboStr* turbo_str_slice(TurboStr* s, int64_t start, int64_t end) {
    if (!s) return turbo_str_new("", 0);
    int64_t len = s->length;
    if (start < 0) start += len;
    if (end < 0) end += len;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return turbo_str_new("", 0);
    return turbo_str_new(s->chars + start, (int32_t)(end - start));
}

TurboStr* turbo_str_char_at(TurboStr* s, int64_t idx) {
    if (!s || s->length == 0) {
        turbo_raise(1, NULL, "IndexError: string index out of range");
    }
    if (idx < 0) idx += s->length;
    if (idx < 0 || idx >= s->length) {
        turbo_raise(1, NULL, "IndexError: string index out of range");
    }
    return turbo_str_new(&s->chars[idx], 1);
}

int32_t turbo_str_eq(TurboStr* a, TurboStr* b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->length != b->length) return 0;
    if (a->hash && b->hash && a->hash != b->hash) return 0;
    return memcmp(a->chars, b->chars, (size_t)a->length) == 0 ? 1 : 0;
}

int32_t turbo_str_cmp(TurboStr* a, TurboStr* b) {
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

uint64_t turbo_str_hash(TurboStr* s) {
    if (!s) return 0;
    if (s->hash == 0) {
        s->hash = fnv1a_32(s->chars, s->length);
    }
    uint64_t x = (uint64_t)s->hash;
    x = (x ^ (x >> 16)) * 0x45d9f3bULL;
    return x ^ (x >> 16);
}

int32_t turbo_str_contains(TurboStr* sub, TurboStr* s) {
    if (!sub || !s) return 0;
    if (sub->length == 0) return 1;
    if (sub->length > s->length) return 0;
    return strstr(s->chars, sub->chars) != NULL ? 1 : 0;
}

static void split_push_str(TurboPtrList* lst, TurboStr* item) {
    if (lst->length >= lst->capacity) {
        lst->capacity *= 2;
        lst->items = (void**)turbo_realloc(lst->items, sizeof(void*) * (size_t)lst->capacity);
    }
    lst->items[lst->length++] = item;
}

TurboPtrList* turbo_str_split(TurboStr* s, TurboStr* sep) {
    TurboPtrList* lst = (TurboPtrList*)turbo_alloc(sizeof(TurboPtrList));
    lst->length = 0;
    lst->capacity = 8;
    lst->items = (void**)turbo_alloc(sizeof(void*) * 8);
    if (!s) return lst;

    if (!sep) {
        int32_t i = 0;
        while (i < s->length) {
            while (i < s->length && isspace((unsigned char)s->chars[i])) i++;
            if (i >= s->length) break;
            int32_t start = i;
            while (i < s->length && !isspace((unsigned char)s->chars[i])) i++;
            split_push_str(lst, turbo_str_new(s->chars + start, i - start));
        }
        return lst;
    }

    if (sep->length == 0) {
        turbo_raise(2, NULL, "ValueError: empty separator");
    }

    int32_t pos = 0;
    while (pos <= s->length) {
        char* found = strstr(s->chars + pos, sep->chars);
        if (!found) {
            split_push_str(lst, turbo_str_new(s->chars + pos, s->length - pos));
            break;
        }
        int32_t end = (int32_t)(found - s->chars);
        split_push_str(lst, turbo_str_new(s->chars + pos, end - pos));
        pos = end + sep->length;
    }
    return lst;
}

TurboStr* turbo_str_join(TurboStr* sep, TurboPtrList* list) {
    if (!list || list->length == 0) return turbo_str_new("", 0);
    int32_t seplen = sep ? sep->length : 0;
    size_t total = 0;
    for (int32_t i = 0; i < list->length; i++) {
        TurboStr* item = (TurboStr*)list->items[i];
        total += item ? (size_t)item->length : 0;
        if (i > 0) total += (size_t)seplen;
    }
    TurboStr* out = (TurboStr*)turbo_alloc(sizeof(TurboStr));
    out->length = (int32_t)total;
    out->hash = 0;
    out->chars = (char*)turbo_alloc(total + 1);
    size_t off = 0;
    for (int32_t i = 0; i < list->length; i++) {
        if (i > 0 && seplen > 0) {
            memcpy(out->chars + off, sep->chars, (size_t)seplen);
            off += (size_t)seplen;
        }
        TurboStr* item = (TurboStr*)list->items[i];
        if (item && item->length > 0) {
            memcpy(out->chars + off, item->chars, (size_t)item->length);
            off += (size_t)item->length;
        }
    }
    out->chars[off] = '\0';
    return out;
}

TurboStr* turbo_str_upper(TurboStr* s) {
    if (!s) return turbo_str_new("", 0);
    TurboStr* out = turbo_str_new(s->chars, s->length);
    for (int32_t i = 0; i < out->length; i++) out->chars[i] = (char)toupper((unsigned char)out->chars[i]);
    return out;
}

TurboStr* turbo_str_lower(TurboStr* s) {
    if (!s) return turbo_str_new("", 0);
    TurboStr* out = turbo_str_new(s->chars, s->length);
    for (int32_t i = 0; i < out->length; i++) out->chars[i] = (char)tolower((unsigned char)out->chars[i]);
    return out;
}

TurboStr* turbo_str_replace(TurboStr* s, TurboStr* old_s, TurboStr* new_s) {
    if (!s || !old_s || !new_s || old_s->length == 0) return s ? s : turbo_str_new("", 0);
    int32_t count = 0;
    const char* p = s->chars;
    while ((p = strstr(p, old_s->chars)) != NULL) {
        count++;
        p += old_s->length;
    }
    if (count == 0) return s;
    size_t new_len = (size_t)s->length + (size_t)count * (size_t)(new_s->length - old_s->length);
    TurboStr* out = (TurboStr*)turbo_alloc(sizeof(TurboStr));
    out->length = (int32_t)new_len;
    out->hash = 0;
    out->chars = (char*)turbo_alloc(new_len + 1);
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

int64_t turbo_str_find(TurboStr* s, TurboStr* sub) {
    if (!s || !sub) return -1;
    if (sub->length == 0) return 0;
    char* p = strstr(s->chars, sub->chars);
    return p ? (int64_t)(p - s->chars) : -1;
}

int64_t turbo_str_index(TurboStr* s, TurboStr* sub) {
    int64_t idx = turbo_str_find(s, sub);
    if (idx < 0) turbo_raise(2, NULL, "ValueError: substring not found");
    return idx;
}

TurboStr* turbo_str_strip(TurboStr* s) {
    if (!s) return turbo_str_new("", 0);
    int32_t l = 0, r = s->length;
    while (l < r && isspace((unsigned char)s->chars[l])) l++;
    while (r > l && isspace((unsigned char)s->chars[r - 1])) r--;
    return turbo_str_new(s->chars + l, r - l);
}

TurboStr* turbo_str_lstrip(TurboStr* s) {
    if (!s) return turbo_str_new("", 0);
    int32_t l = 0;
    while (l < s->length && isspace((unsigned char)s->chars[l])) l++;
    return turbo_str_new(s->chars + l, s->length - l);
}

TurboStr* turbo_str_rstrip(TurboStr* s) {
    if (!s) return turbo_str_new("", 0);
    int32_t r = s->length;
    while (r > 0 && isspace((unsigned char)s->chars[r - 1])) r--;
    return turbo_str_new(s->chars, r);
}

int32_t turbo_str_startswith(TurboStr* s, TurboStr* prefix) {
    if (!s || !prefix) return 0;
    if (prefix->length > s->length) return 0;
    return memcmp(s->chars, prefix->chars, (size_t)prefix->length) == 0 ? 1 : 0;
}

int32_t turbo_str_endswith(TurboStr* s, TurboStr* suffix) {
    if (!s || !suffix) return 0;
    if (suffix->length > s->length) return 0;
    return memcmp(s->chars + s->length - suffix->length, suffix->chars, (size_t)suffix->length) == 0 ? 1 : 0;
}

int64_t turbo_str_count(TurboStr* s, TurboStr* sub) {
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

TurboStr* turbo_int_to_str(int64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return turbo_str_new(buf, n);
}

TurboStr* turbo_uint_to_str(uint64_t v) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    return turbo_str_new(buf, n);
}

TurboStr* turbo_float_to_str(double v) {
    char buf[64];
    int n = snprintf(buf, sizeof(buf), "%.15g", v);
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'E') && !strchr(buf, 'n') && !strchr(buf, 'i')) {
        if (n + 2 < (int)sizeof(buf)) {
            buf[n++] = '.';
            buf[n++] = '0';
            buf[n] = '\0';
        }
    }
    return turbo_str_new(buf, n);
}

TurboStr* turbo_float_fmt(double v, int32_t precision) {
    char buf[128];
    if (precision < 0) precision = 6;
    if (precision > 50) precision = 50;
    int n = snprintf(buf, sizeof(buf), "%.*f", precision, v);
    return turbo_str_new(buf, n);
}

TurboStr* turbo_bool_to_str(int32_t v) {
    return v ? turbo_str_cstr("True") : turbo_str_cstr("False");
}

int64_t turbo_str_to_int(TurboStr* s) {
    if (!s) return 0;
    char* end = NULL;
    long long v = strtoll(s->chars, &end, 10);
    return (int64_t)v;
}

double turbo_str_to_float(TurboStr* s) {
    if (!s) return 0.0;
    return strtod(s->chars, NULL);
}

int64_t turbo_ord(TurboStr* s) {
    if (!s || s->length == 0) return 0;
    return (uint8_t)s->chars[0];
}

TurboStr* turbo_chr(int64_t code) {
    char buf[2] = { (char)code, '\0' };
    return turbo_str_new(buf, 1);
}

TurboStr* turbo_hex(int64_t v) {
    char buf[32];
    int n = v < 0 ? snprintf(buf, sizeof(buf), "-0x%llx", (unsigned long long)(-v))
                  : snprintf(buf, sizeof(buf), "0x%llx", (unsigned long long)v);
    return turbo_str_new(buf, n);
}

TurboStr* turbo_bin(int64_t v) {
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
    return turbo_str_cstr(buf + pos);
}

TurboStr* turbo_oct(int64_t v) {
    char buf[32];
    int n = v < 0 ? snprintf(buf, sizeof(buf), "-0o%llo", (unsigned long long)(-v))
                  : snprintf(buf, sizeof(buf), "0o%llo", (unsigned long long)v);
    return turbo_str_new(buf, n);
}

int64_t turbo_pow_i64(int64_t base, int64_t exp) {
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

TurboBytes* turbo_bytes_new(const uint8_t* data, int32_t len) {
    TurboBytes* b = (TurboBytes*)turbo_alloc(sizeof(TurboBytes));
    b->length = len;
    b->_pad = 0;
    b->data = (uint8_t*)turbo_alloc((size_t)len + 1);
    if (len > 0 && data) memcpy(b->data, data, (size_t)len);
    b->data[len] = 0;
    return b;
}

int32_t turbo_bytes_eq(TurboBytes* a, TurboBytes* b) {
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
    char* rev = (char*)turbo_alloc((size_t)m + 1);
    int i = al - 1, j = bl - 1, c = 0, k = 0;
    while (i >= 0 || j >= 0 || c) {
        int s = c;
        if (i >= 0) s += a[i--] - '0';
        if (j >= 0) s += b[j--] - '0';
        rev[k++] = (char)((s % 10) + '0');
        c = s / 10;
    }
    char* res = (char*)turbo_alloc((size_t)k + 1);
    for (int p = 0; p < k; p++) res[p] = rev[k - 1 - p];
    res[k] = '\0';
    free(rev);
    *ol = k;
    return res;
}

static char* bi_sub_abs(const char* a, int al, const char* b, int bl, int* ol) {
    char* rev = (char*)turbo_alloc((size_t)al + 1);
    int i = al - 1, j = bl - 1, br = 0, k = 0;
    while (i >= 0 || j >= 0) {
        int d = br;
        if (i >= 0) d += a[i--] - '0';
        if (j >= 0) d -= b[j--] - '0';
        if (d < 0) { d += 10; br = -1; } else br = 0;
        rev[k++] = (char)(d + '0');
    }
    while (k > 1 && rev[k - 1] == '0') k--;
    char* res = (char*)turbo_alloc((size_t)k + 1);
    for (int p = 0; p < k; p++) res[p] = rev[k - 1 - p];
    res[k] = '\0';
    free(rev);
    *ol = k;
    return res;
}

static TurboBigInt* bi_make(char* digits, int len, int sign) {
    bi_strip(digits, &len);
    if (len == 1 && digits[0] == '0') sign = 1;
    TurboBigInt* b = (TurboBigInt*)turbo_alloc(sizeof(TurboBigInt));
    b->length = len;
    b->sign = sign;
    b->digits = digits;
    return b;
}

TurboBigInt* turbo_bigint_from_str(const char* s) {
    if (!s || !*s) return bi_make(strdup("0"), 1, 1);
    int sign = 1;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    int len = (int)strlen(s);
    char* d = (char*)turbo_alloc((size_t)len + 1);
    memcpy(d, s, (size_t)len + 1);
    return bi_make(d, len, sign);
}

TurboBigInt* turbo_bigint_from_i64(int64_t v) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)v);
    return turbo_bigint_from_str(buf);
}

TurboBigInt* turbo_bigint_add(TurboBigInt* a, TurboBigInt* b) {
    int ol = 0;
    if (a->sign == b->sign) {
        return bi_make(bi_add_abs(a->digits, a->length, b->digits, b->length, &ol), ol, a->sign);
    }
    int c = bi_abs_cmp(a->digits, a->length, b->digits, b->length);
    if (c == 0) return turbo_bigint_from_str("0");
    if (c > 0) return bi_make(bi_sub_abs(a->digits, a->length, b->digits, b->length, &ol), ol, a->sign);
    return bi_make(bi_sub_abs(b->digits, b->length, a->digits, a->length, &ol), ol, b->sign);
}

TurboBigInt* turbo_bigint_sub(TurboBigInt* a, TurboBigInt* b) {
    TurboBigInt nb = *b;
    nb.sign = -b->sign;
    return turbo_bigint_add(a, &nb);
}

TurboBigInt* turbo_bigint_mul(TurboBigInt* a, TurboBigInt* b) {
    int rl = a->length + b->length;
    int* tmp = (int*)turbo_calloc((size_t)rl + 4, sizeof(int));
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
    char* res = (char*)turbo_alloc((size_t)rl + 1);
    for (int i = 0; i < rl; i++) res[i] = (char)(tmp[rl - 1 - i] + '0');
    res[rl] = '\0';
    free(tmp);
    return bi_make(res, rl, a->sign * b->sign);
}

TurboBigInt* turbo_bigint_div(TurboBigInt* a, TurboBigInt* b) {
    if (b->length == 1 && b->digits[0] == '0') {
        turbo_raise(3, NULL, "ZeroDivisionError: integer division by zero");
    }
    if (bi_abs_cmp(a->digits, a->length, b->digits, b->length) < 0) {
        if (a->sign == b->sign || (a->length == 1 && a->digits[0] == '0')) return turbo_bigint_from_str("0");
        return turbo_bigint_from_str("-1");
    }
    char* res = (char*)turbo_calloc((size_t)a->length + 1, 1);
    int ri = 0;
    char* cur = (char*)turbo_alloc((size_t)a->length + 1);
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
    TurboBigInt* q = bi_make(res, ri, a->sign * b->sign);
    if (a->sign != b->sign && has_rem) {
        TurboBigInt* one = turbo_bigint_from_str("1");
        TurboBigInt* adj = turbo_bigint_sub(q, one);
        return adj;
    }
    return q;
}

TurboBigInt* turbo_bigint_mod(TurboBigInt* a, TurboBigInt* b) {
    TurboBigInt* q = turbo_bigint_div(a, b);
    TurboBigInt* p = turbo_bigint_mul(q, b);
    return turbo_bigint_sub(a, p);
}

TurboBigInt* turbo_bigint_pow(TurboBigInt* a, TurboBigInt* b) {
    if (b->sign < 0) return turbo_bigint_from_str("0");
    long long e = strtoll(b->digits, NULL, 10);
    TurboBigInt* res = turbo_bigint_from_str("1");
    TurboBigInt* base = a;
    while (e > 0) {
        if (e & 1) res = turbo_bigint_mul(res, base);
        e >>= 1;
        if (e > 0) base = turbo_bigint_mul(base, base);
    }
    return res;
}

TurboBigInt* turbo_bigint_neg(TurboBigInt* a) {
    char* d = strdup(a->digits);
    return bi_make(d, a->length, -a->sign);
}

TurboBigInt* turbo_bigint_abs(TurboBigInt* a) {
    char* d = strdup(a->digits);
    return bi_make(d, a->length, 1);
}

int32_t turbo_bigint_cmp(TurboBigInt* a, TurboBigInt* b) {
    if (a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
    int c = bi_abs_cmp(a->digits, a->length, b->digits, b->length);
    return a->sign > 0 ? c : -c;
}

TurboStr* turbo_bigint_to_str(TurboBigInt* a) {
    if (a->sign < 0 && !(a->length == 1 && a->digits[0] == '0')) {
        char* buf = (char*)turbo_alloc((size_t)a->length + 2);
        buf[0] = '-';
        memcpy(buf + 1, a->digits, (size_t)a->length + 1);
        TurboStr* s = turbo_str_new(buf, a->length + 1);
        free(buf);
        return s;
    }
    return turbo_str_new(a->digits, a->length);
}

/* --- File I/O --- */

TurboFile* turbo_open(TurboStr* path, TurboStr* mode) {
    const char* m = (mode && mode->length > 0) ? mode->chars : "r";
    FILE* h = fopen(path ? path->chars : "", m);
    if (!h) turbo_raise(4, NULL, "FileNotFoundError: cannot open file");
    TurboFile* f = (TurboFile*)turbo_alloc(sizeof(TurboFile));
    f->handle = h;
    return f;
}

TurboStr* turbo_file_read(TurboFile* f) {
    if (!f || !f->handle) return turbo_str_new("", 0);
    fseek(f->handle, 0, SEEK_END);
    long sz = ftell(f->handle);
    fseek(f->handle, 0, SEEK_SET);
    if (sz <= 0) return turbo_str_new("", 0);
    char* buf = (char*)turbo_alloc((size_t)sz + 1);
    size_t n = fread(buf, 1, (size_t)sz, f->handle);
    buf[n] = '\0';
    TurboStr* s = turbo_str_new(buf, (int32_t)n);
    free(buf);
    return s;
}

void turbo_file_write(TurboFile* f, TurboStr* text) {
    if (f && f->handle && text && text->length > 0) {
        fwrite(text->chars, 1, (size_t)text->length, f->handle);
    }
}

void turbo_file_close(TurboFile* f) {
    if (f && f->handle) {
        fclose(f->handle);
        f->handle = NULL;
    }
}

/* --- Print & Input --- */

void turbo_print_i64(int64_t v) { printf("%lld", (long long)v); }
void turbo_print_u64(uint64_t v) { printf("%llu", (unsigned long long)v); }
void turbo_print_f64(double v) {
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
void turbo_print_bool(int32_t v) { fputs(v ? "True" : "False", stdout); }
void turbo_print_str(TurboStr* s) { if (s && s->length > 0) fwrite(s->chars, 1, (size_t)s->length, stdout); }
void turbo_print_cstr(const char* s) { if (s) fputs(s, stdout); }
void turbo_print_space(void) { putchar(' '); }
void turbo_print_newline(void) { putchar('\n'); }

TurboStr* turbo_input(TurboStr* prompt) {
    if (prompt && prompt->length > 0) {
        fwrite(prompt->chars, 1, (size_t)prompt->length, stdout);
        fflush(stdout);
    }
    char buf[4096];
    if (!fgets(buf, sizeof(buf), stdin)) return turbo_str_new("", 0);
    size_t len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) len--;
    return turbo_str_new(buf, (int32_t)len);
}
