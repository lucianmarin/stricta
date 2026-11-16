#include "runtime.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <regex.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

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

/* --- Exception Handling (Compiler-Lowered Propagation) --- */

static _Thread_local int32_t _exc_type_id = 0;
static _Thread_local void* _exc_obj = NULL;
static _Thread_local StrictaStr* _exc_msg = NULL;

void stricta_set_exc(int32_t type_id, void* exc_obj, const char* msg) {
    if (_exc_msg) { stricta_str_free(_exc_msg); _exc_msg = NULL; }
    _exc_type_id = type_id;
    _exc_obj = exc_obj;
    _exc_msg = msg ? stricta_str_cstr(msg) : stricta_str_cstr("");
}

void stricta_clear_exc(int32_t free_obj) {
    if (_exc_msg) { stricta_str_free(_exc_msg); _exc_msg = NULL; }
    if (free_obj && _exc_obj) { free(_exc_obj); }
    _exc_obj = NULL;
    _exc_type_id = 0;
}

void stricta_unhandled_exc(void) {
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

void stricta_bounds_fail(int64_t idx, int64_t len, const char* file, int32_t line) {
    fprintf(stderr, "IndexError (%s:%d): index %lld out of range for length %lld\n",
            file ? file : "<unknown>", line, (long long)idx, (long long)len);
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
    st->is_static = 0;
    st->chars = (char*)stricta_alloc((size_t)len + 1);
    if (len > 0 && s) memcpy(st->chars, s, (size_t)len);
    st->chars[len] = '\0';
    return st;
}

void stricta_str_free(StrictaStr* s) {
    if (s && !s->is_static) {
        free(s->chars);
        free(s);
    }
}

StrictaStr* stricta_str_clone(StrictaStr* s) {
    if (!s || s->is_static) return s;
    return stricta_str_new(s->chars, s->length);
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
    st->is_static = 0;
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
    st->is_static = 0;
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

int64_t stricta_slice_adjust(int64_t len, int64_t start, int32_t has_start,
                             int64_t stop, int32_t has_stop, int64_t step, int64_t* out_start) {
    if (step == 0) {
        stricta_set_exc(1, NULL, "ValueError: slice step cannot be zero");
        *out_start = 0;
        return -1;
    }
    if (!has_start) {
        start = (step < 0) ? len - 1 : 0;
    } else {
        if (start < 0) {
            start += len;
            if (start < 0) start = (step < 0) ? -1 : 0;
        } else if (start >= len) {
            start = (step < 0) ? len - 1 : len;
        }
    }
    if (!has_stop) {
        stop = (step < 0) ? -1 : len;
    } else {
        if (stop < 0) {
            stop += len;
            if (stop < 0) stop = (step < 0) ? -1 : 0;
        } else if (stop >= len) {
            stop = (step < 0) ? len - 1 : len;
        }
    }
    *out_start = start;
    if (step > 0) {
        if (stop <= start) return 0;
        uint64_t span = (uint64_t)stop - (uint64_t)start;
        uint64_t ustep = (uint64_t)step;
        return (int64_t)((span + ustep - 1) / ustep);
    }
    if (start <= stop) return 0;
    uint64_t span = (uint64_t)start - (uint64_t)stop;
    uint64_t ustep = (uint64_t)(-(step + 1)) + 1ULL;
    return (int64_t)((span + ustep - 1) / ustep);
}

StrictaStr* stricta_str_slice_step(StrictaStr* s, int64_t start, int32_t has_start,
                                   int64_t stop, int32_t has_stop, int64_t step) {
    if (!s) {
        int64_t st0 = 0;
        stricta_slice_adjust(0, start, has_start, stop, has_stop, step, &st0);
        return stricta_str_new("", 0);
    }
    int64_t cur = 0;
    int64_t cnt = stricta_slice_adjust(s->length, start, has_start, stop, has_stop, step, &cur);
    if (cnt <= 0) return stricta_str_new("", 0);
    if (step == 1) return stricta_str_new(s->chars + cur, (int32_t)cnt);
    StrictaStr* st = (StrictaStr*)stricta_alloc(sizeof(StrictaStr));
    st->length = (int32_t)cnt;
    st->is_static = 0;
    st->hash = 0;
    st->chars = (char*)stricta_alloc((size_t)cnt + 1);
    for (int64_t i = 0; i < cnt; i++) {
        st->chars[i] = s->chars[cur];
        cur += step;
    }
    st->chars[cnt] = '\0';
    return st;
}

StrictaStr* stricta_str_char_at(StrictaStr* s, int64_t idx) {
    if (!s || s->length == 0) {
        stricta_bounds_fail(idx, 0, "<str>", 0);
    }
    if (idx < 0) idx += s->length;
    if (idx < 0 || idx >= s->length) {
        stricta_bounds_fail(idx, s->length, "<str>", 0);
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
    uint32_t h = s->hash;
    if (h == 0) {
        h = fnv1a_32(s->chars, s->length);
        if (!s->is_static) s->hash = h;
    }
    uint64_t x = (uint64_t)h;
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
        stricta_set_exc(2, NULL, "ValueError: empty separator");
        stricta_unhandled_exc();
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
    out->is_static = 0;
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
    if (!s || !old_s || !new_s || old_s->length == 0) return s ? stricta_str_clone(s) : stricta_str_new("", 0);
    int32_t count = 0;
    const char* p = s->chars;
    while ((p = strstr(p, old_s->chars)) != NULL) {
        count++;
        p += old_s->length;
    }
    if (count == 0) return stricta_str_clone(s);
    size_t new_len = (size_t)s->length + (size_t)count * (size_t)(new_s->length - old_s->length);
    StrictaStr* out = (StrictaStr*)stricta_alloc(sizeof(StrictaStr));
    out->length = (int32_t)new_len;
    out->hash = 0;
    out->is_static = 0;
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
    if (idx < 0) {
        stricta_set_exc(2, NULL, "ValueError: substring not found");
        stricta_unhandled_exc();
    }
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
    b->is_static = 0;
    b->data = (uint8_t*)stricta_alloc((size_t)len + 1);
    if (len > 0 && data) memcpy(b->data, data, (size_t)len);
    b->data[len] = 0;
    return b;
}

void stricta_bytes_free(StrictaBytes* b) {
    if (b && !b->is_static) {
        free(b->data);
        free(b);
    }
}

StrictaBytes* stricta_bytes_clone(StrictaBytes* b) {
    if (!b || b->is_static) return b;
    return stricta_bytes_new(b->data, b->length);
}

StrictaBytes* stricta_bytes_slice(StrictaBytes* b, int64_t start, int64_t end) {
    if (!b) return stricta_bytes_new(NULL, 0);
    int64_t len = b->length;
    if (start < 0) start += len;
    if (end < 0) end += len;
    if (start < 0) start = 0;
    if (end > len) end = len;
    if (start >= end) return stricta_bytes_new(NULL, 0);
    return stricta_bytes_new(b->data + start, (int32_t)(end - start));
}

StrictaBytes* stricta_bytes_slice_step(StrictaBytes* b, int64_t start, int32_t has_start,
                                       int64_t stop, int32_t has_stop, int64_t step) {
    if (!b) {
        int64_t st0 = 0;
        stricta_slice_adjust(0, start, has_start, stop, has_stop, step, &st0);
        return stricta_bytes_new(NULL, 0);
    }
    int64_t cur = 0;
    int64_t cnt = stricta_slice_adjust(b->length, start, has_start, stop, has_stop, step, &cur);
    if (cnt <= 0) return stricta_bytes_new(NULL, 0);
    if (step == 1) return stricta_bytes_new(b->data + cur, (int32_t)cnt);
    StrictaBytes* out = (StrictaBytes*)stricta_alloc(sizeof(StrictaBytes));
    out->length = (int32_t)cnt;
    out->is_static = 0;
    out->data = (uint8_t*)stricta_alloc((size_t)cnt + 1);
    for (int64_t i = 0; i < cnt; i++) {
        out->data[i] = b->data[cur];
        cur += step;
    }
    out->data[cnt] = 0;
    return out;
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
        stricta_set_exc(3, NULL, "ZeroDivisionError: integer division by zero");
        stricta_unhandled_exc();
    }
    if (bi_abs_cmp(a->digits, a->length, b->digits, b->length) < 0) {
        if (a->sign == b->sign || (a->length == 1 && a->digits[0] == '0')) return stricta_bigint_from_str("0");
        return stricta_bigint_from_str("-1");
    }
    char* res = (char*)stricta_calloc((size_t)a->length + 1, 1);
    int ri = 0;
    size_t cur_cap = (size_t)a->length + 2;
    char* cur = (char*)stricta_alloc(cur_cap);
    int cl = 0;
    for (int i = 0; i < a->length; i++) {
        if ((size_t)cl + 2 > cur_cap) {
            cur_cap = (size_t)cl + 16;
            cur = (char*)stricta_realloc(cur, cur_cap);
        }
        cur[cl++] = a->digits[i];
        cur[cl] = '\0';
        bi_strip(cur, &cl);
        int cmp = bi_abs_cmp(cur, cl, b->digits, b->length);
        if (cmp < 0) { if (ri > 0) res[ri++] = '0'; continue; }
        int q = 0;
        while (cmp >= 0) {
            char* nc = bi_sub_abs(cur, cl, b->digits, b->length, &cl);
            if ((size_t)cl + 2 > cur_cap) {
                cur_cap = (size_t)cl + 16;
                cur = (char*)stricta_realloc(cur, cur_cap);
            }
            memcpy(cur, nc, (size_t)cl + 1);
            free(nc);
            q++;
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
        stricta_bigint_free(q);
        stricta_bigint_free(one);
        return adj;
    }
    return q;
}

void stricta_bigint_free(StrictaBigInt* a) {
    if (a) {
        free(a->digits);
        free(a);
    }
}

StrictaBigInt* stricta_bigint_clone(StrictaBigInt* a) {
    if (!a) return NULL;
    char* d = (char*)stricta_alloc((size_t)a->length + 1);
    memcpy(d, a->digits, (size_t)a->length + 1);
    return bi_make(d, a->length, a->sign);
}

StrictaBigInt* stricta_bigint_mod(StrictaBigInt* a, StrictaBigInt* b) {
    StrictaBigInt* q = stricta_bigint_div(a, b);
    StrictaBigInt* p = stricta_bigint_mul(q, b);
    StrictaBigInt* r = stricta_bigint_sub(a, p);
    stricta_bigint_free(q);
    stricta_bigint_free(p);
    return r;
}

StrictaBigInt* stricta_bigint_pow(StrictaBigInt* a, StrictaBigInt* b) {
    if (b->sign < 0) return stricta_bigint_from_str("0");
    long long e = strtoll(b->digits, NULL, 10);
    StrictaBigInt* res = stricta_bigint_from_str("1");
    StrictaBigInt* base = stricta_bigint_clone(a);
    while (e > 0) {
        if (e & 1) {
            StrictaBigInt* next_res = stricta_bigint_mul(res, base);
            stricta_bigint_free(res);
            res = next_res;
        }
        e >>= 1;
        if (e > 0) {
            StrictaBigInt* next_base = stricta_bigint_mul(base, base);
            stricta_bigint_free(base);
            base = next_base;
        }
    }
    stricta_bigint_free(base);
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
    if (!h) {
        stricta_set_exc(4, NULL, "FileNotFoundError: cannot open file");
        stricta_unhandled_exc();
    }
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

void stricta_file_free(StrictaFile* f) {
    if (f) {
        if (f->handle) {
            fclose(f->handle);
            f->handle = NULL;
        }
        free(f);
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

/* --- OS & Process Primitives --- */

StrictaPtrList* stricta_os_exec_list(StrictaStr* cmd) {
    const char* c = (cmd && cmd->chars) ? cmd->chars : "";
    FILE* fp = popen(c, "r");
    StrictaPtrList* res = (StrictaPtrList*)stricta_alloc(sizeof(StrictaPtrList));
    res->length = 2;
    res->capacity = 4;
    res->items = (void**)stricta_alloc(sizeof(void*) * 4);
    if (!fp) {
        res->items[0] = stricta_str_cstr("-1");
        res->items[1] = stricta_str_new("", 0);
        return res;
    }
    size_t cap = 4096, len = 0;
    char* buf = (char*)stricta_alloc(cap);
    size_t n;
    while ((n = fread(buf + len, 1, cap - len - 1, fp)) > 0) {
        len += n;
        if (cap - len - 1 < 1024) {
            cap *= 2;
            buf = (char*)stricta_realloc(buf, cap);
        }
    }
    buf[len] = '\0';
    int st = pclose(fp);
    int64_t rc = WIFEXITED(st) ? (int64_t)WEXITSTATUS(st) : -1;
    res->items[0] = stricta_int_to_str(rc);
    res->items[1] = stricta_str_new(buf, (int32_t)len);
    free(buf);
    return res;
}

int64_t stricta_os_system(StrictaStr* cmd) {
    const char* c = (cmd && cmd->chars) ? cmd->chars : "";
    int st = system(c);
    if (st < 0) return -1;
    return WIFEXITED(st) ? (int64_t)WEXITSTATUS(st) : -1;
}

int64_t stricta_os_exists(StrictaStr* path) {
    if (!path || !path->chars) return 0;
    return access(path->chars, F_OK) == 0 ? 1 : 0;
}

int64_t stricta_os_isdir(StrictaStr* path) {
    if (!path || !path->chars) return 0;
    struct stat st;
    if (stat(path->chars, &st) != 0) return 0;
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

int64_t stricta_os_mkdir(StrictaStr* path) {
    if (!path || !path->chars || path->length == 0) return 0;
    if (mkdir(path->chars, 0755) == 0 || errno == EEXIST) return 1;
    return 0;
}

StrictaPtrList* stricta_os_listdir(StrictaStr* path) {
    StrictaPtrList* lst = (StrictaPtrList*)stricta_alloc(sizeof(StrictaPtrList));
    lst->length = 0;
    lst->capacity = 8;
    lst->items = (void**)stricta_alloc(sizeof(void*) * (size_t)lst->capacity);
    if (!path || !path->chars) return lst;
    DIR* d = opendir(path->chars);
    if (!d) return lst;
    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        if (lst->length >= lst->capacity) {
            lst->capacity *= 2;
            lst->items = (void**)stricta_realloc(lst->items, sizeof(void*) * (size_t)lst->capacity);
        }
        lst->items[lst->length++] = stricta_str_cstr(ent->d_name);
    }
    closedir(d);
    return lst;
}

StrictaStr* stricta_os_getenv(StrictaStr* name) {
    if (!name || !name->chars) return stricta_str_new("", 0);
    const char* v = getenv(name->chars);
    return v ? stricta_str_cstr(v) : stricta_str_new("", 0);
}

StrictaStr* stricta_os_getcwd(void) {
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return stricta_str_new("", 0);
    return stricta_str_cstr(buf);
}

int64_t stricta_os_remove(StrictaStr* path) {
    if (!path || !path->chars) return 0;
    return unlink(path->chars) == 0 ? 1 : 0;
}

int64_t stricta_os_symlink(StrictaStr* target, StrictaStr* linkpath) {
    if (!target || !target->chars || !linkpath || !linkpath->chars) return 0;
    return symlink(target->chars, linkpath->chars) == 0 ? 1 : 0;
}

/* --- Time & Date Primitives --- */

double stricta_time_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

double stricta_time_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void stricta_time_sleep(double secs) {
    if (secs <= 0.0) return;
    struct timespec req;
    req.tv_sec = (time_t)secs;
    req.tv_nsec = (long)((secs - (double)req.tv_sec) * 1e9);
    nanosleep(&req, NULL);
}

static StrictaPtrList* tm_to_list(const struct tm* t) {
    StrictaPtrList* lst = (StrictaPtrList*)stricta_alloc(sizeof(StrictaPtrList));
    lst->length = 8;
    lst->capacity = 8;
    lst->items = (void**)stricta_alloc(sizeof(void*) * 8);
    lst->items[0] = stricta_int_to_str(t->tm_year + 1900);
    lst->items[1] = stricta_int_to_str(t->tm_mon + 1);
    lst->items[2] = stricta_int_to_str(t->tm_mday);
    lst->items[3] = stricta_int_to_str(t->tm_hour);
    lst->items[4] = stricta_int_to_str(t->tm_min);
    lst->items[5] = stricta_int_to_str(t->tm_sec);
    lst->items[6] = stricta_int_to_str((t->tm_wday + 6) % 7); /* Monday=0 .. Sunday=6 */
    lst->items[7] = stricta_int_to_str(t->tm_yday + 1);
    return lst;
}

StrictaPtrList* stricta_time_localtime(int64_t epoch_sec) {
    time_t t = (time_t)epoch_sec;
    struct tm tm_res;
    localtime_r(&t, &tm_res);
    return tm_to_list(&tm_res);
}

StrictaPtrList* stricta_time_gmtime(int64_t epoch_sec) {
    time_t t = (time_t)epoch_sec;
    struct tm tm_res;
    gmtime_r(&t, &tm_res);
    return tm_to_list(&tm_res);
}

int64_t stricta_time_mktime(int64_t y, int64_t m, int64_t d, int64_t H, int64_t M, int64_t S) {
    struct tm tm_in;
    memset(&tm_in, 0, sizeof(tm_in));
    tm_in.tm_year = (int)(y - 1900);
    tm_in.tm_mon = (int)(m - 1);
    tm_in.tm_mday = (int)d;
    tm_in.tm_hour = (int)H;
    tm_in.tm_min = (int)M;
    tm_in.tm_sec = (int)S;
    tm_in.tm_isdst = -1;
    return (int64_t)mktime(&tm_in);
}

/* --- Sys Primitives --- */

StrictaPtrList* stricta_sys_get_argv(void) {
    StrictaPtrList* res = (StrictaPtrList*)stricta_alloc(sizeof(StrictaPtrList));
    int32_t n = stricta_sys_argv ? stricta_sys_argv->length : 0;
    res->length = n;
    res->capacity = n > 4 ? n : 4;
    res->items = (void**)stricta_alloc(sizeof(void*) * (size_t)res->capacity);
    for (int32_t i = 0; i < n; i++) {
        res->items[i] = stricta_str_clone((StrictaStr*)stricta_sys_argv->items[i]);
    }
    return res;
}

void stricta_sys_exit(int64_t code) {
    exit((int)code);
}

StrictaStr* stricta_sys_platform(void) {
#if defined(__APPLE__)
    return stricta_str_cstr("darwin");
#elif defined(__linux__)
    return stricta_str_cstr("linux");
#elif defined(_WIN32)
    return stricta_str_cstr("win32");
#else
    return stricta_str_cstr("unknown");
#endif
}

/* --- Async Tasks --- */

struct StrictaTask {
    pthread_t thread;
    int32_t joined;
    int32_t has_result;
    uint64_t result_bits;
    void* arg_block;
    void (*drop_result)(uint64_t);
    int32_t exc_type_id;
    void* exc_obj;
    StrictaStr* exc_msg;
};

StrictaTask* stricta_task_spawn(void* (*entry)(void*), void* arg_block, void (*drop_result)(uint64_t)) {
    StrictaTask* t = (StrictaTask*)stricta_calloc(1, sizeof(StrictaTask));
    t->arg_block = arg_block;
    t->drop_result = drop_result;
    int rc = pthread_create(&t->thread, NULL, entry, t);
    if (rc != 0) {
        fprintf(stderr, "RuntimeError: failed to spawn async task thread (%d)\n", rc);
        exit(1);
    }
    return t;
}

void* stricta_task_args(StrictaTask* t) {
    return t ? t->arg_block : NULL;
}

void stricta_task_finish(StrictaTask* t, uint64_t bits) {
    if (!t) return;
    t->result_bits = bits;
    t->has_result = 1;
}

void stricta_task_capture_exc(StrictaTask* t) {
    if (!t) return;
    t->exc_type_id = _exc_type_id;
    t->exc_obj = _exc_obj;
    t->exc_msg = _exc_msg;
    _exc_type_id = 0;
    _exc_obj = NULL;
    _exc_msg = NULL;
}

uint64_t stricta_task_await(StrictaTask* t) {
    if (!t) return 0;
    if (!t->joined) {
        pthread_join(t->thread, NULL);
        t->joined = 1;
    }
    if (t->exc_type_id != 0) {
        stricta_clear_exc(0);
        _exc_type_id = t->exc_type_id;
        _exc_obj = t->exc_obj;
        _exc_msg = t->exc_msg;
        free(t);
        return 0;
    }
    uint64_t r = t->result_bits;
    free(t);
    return r;
}

void stricta_task_free(StrictaTask* t) {
    if (!t) return;
    if (!t->joined) {
        pthread_join(t->thread, NULL);
        t->joined = 1;
    }
    if (t->exc_type_id != 0) {
        if (t->exc_msg) stricta_str_free(t->exc_msg);
        if (t->exc_obj) free(t->exc_obj);
    } else if (t->has_result && t->drop_result) {
        t->drop_result(t->result_bits);
    }
    free(t);
}

/* --- Regex & Concurrency Helpers --- */

StrictaPtrList* stricta_re_find(StrictaStr* pattern, StrictaStr* text, int64_t start_pos, int64_t flags) {
    StrictaPtrList* res = (StrictaPtrList*)stricta_alloc(sizeof(StrictaPtrList));
    res->length = 0;
    res->capacity = 4;
    res->items = (void**)stricta_alloc(sizeof(void*) * (size_t)res->capacity);
    if (!pattern || !text || start_pos < 0 || start_pos > text->length) return res;

    regex_t re;
    int cflags = REG_EXTENDED;
    if (flags & 2) cflags |= REG_ICASE;
    if (flags & 4) cflags |= REG_NEWLINE;
    int rc = regcomp(&re, pattern->chars, cflags);
    if (rc != 0) {
        char ebuf[256];
        regerror(rc, &re, ebuf, sizeof(ebuf));
        char msg[300];
        snprintf(msg, sizeof(msg), "ERR:%s", ebuf);
        res->items[0] = stricta_str_cstr(msg);
        res->length = 1;
        return res;
    }

    size_t nmatch = re.re_nsub + 1;
    regmatch_t* pm = (regmatch_t*)stricta_calloc(nmatch, sizeof(regmatch_t));
    int eflags = (start_pos > 0) ? REG_NOTBOL : 0;
    if (regexec(&re, text->chars + start_pos, nmatch, pm, eflags) == 0) {
        int32_t need = (int32_t)(2 + nmatch);
        if (need > res->capacity) {
            res->capacity = need;
            res->items = (void**)stricta_realloc(res->items, sizeof(void*) * (size_t)res->capacity);
        }
        int64_t mstart = start_pos + (int64_t)pm[0].rm_so;
        int64_t mend = start_pos + (int64_t)pm[0].rm_eo;
        res->items[res->length++] = stricta_int_to_str(mstart);
        res->items[res->length++] = stricta_int_to_str(mend);
        for (size_t i = 0; i < nmatch; i++) {
            if (pm[i].rm_so < 0 || pm[i].rm_eo < pm[i].rm_so) {
                res->items[res->length++] = stricta_str_new("", 0);
            } else {
                res->items[res->length++] = stricta_str_new(
                    text->chars + start_pos + pm[i].rm_so,
                    (int32_t)(pm[i].rm_eo - pm[i].rm_so));
            }
        }
    }
    free(pm);
    regfree(&re);
    return res;
}

static uint64_t _stricta_uid_seq = 0;
int64_t stricta_unique_id(void) {
    return (int64_t)__atomic_add_fetch(&_stricta_uid_seq, 1, __ATOMIC_RELAXED);
}


