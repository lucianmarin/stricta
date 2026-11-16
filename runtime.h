#ifndef TURBO_RUNTIME_H
#define TURBO_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <math.h>

/* Lean Static Runtime for Turbo — Zero NaN-boxing, Zero Dynamic Dispatch */

typedef struct {
    int32_t length;
    uint32_t hash;
    char* chars;
} TurboStr;

typedef struct {
    int32_t length;
    int32_t _pad;
    uint8_t* data;
} TurboBytes;

typedef struct {
    int32_t length;
    int32_t capacity;
    void** items;
} TurboPtrList;

typedef struct {
    int32_t length;
    int32_t sign;
    char* digits;
} TurboBigInt;

typedef struct {
    FILE* handle;
} TurboFile;

/* Arena / Memory helpers */
void* turbo_alloc(size_t size);
void* turbo_calloc(size_t count, size_t size);
void* turbo_realloc(void* ptr, size_t size);
void turbo_free(void* ptr);

/* Initialization & CLI args */
extern TurboPtrList* turbo_sys_argv;
void turbo_init_args(int32_t argc, char** argv);

/* Exception handling (setjmp/longjmp) */
jmp_buf* turbo_try_push(void);
void turbo_try_pop(void);
void turbo_raise(int32_t type_id, void* exc_obj, const char* msg);
void turbo_reraise(void);
int32_t turbo_exc_type_id(void);
void* turbo_exc_obj(void);
TurboStr* turbo_exc_msg(void);
void turbo_assert_fail(const char* msg, const char* file, int32_t line);

/* Strings */
TurboStr* turbo_str_new(const char* s, int32_t len);
TurboStr* turbo_str_cstr(const char* s);
TurboStr* turbo_str_concat(TurboStr* a, TurboStr* b);
TurboStr* turbo_str_repeat(TurboStr* s, int64_t count);
TurboStr* turbo_str_slice(TurboStr* s, int64_t start, int64_t end);
TurboStr* turbo_str_char_at(TurboStr* s, int64_t idx);
int32_t turbo_str_eq(TurboStr* a, TurboStr* b);
int32_t turbo_str_cmp(TurboStr* a, TurboStr* b);
uint64_t turbo_str_hash(TurboStr* s);
int32_t turbo_str_contains(TurboStr* sub, TurboStr* s);
TurboPtrList* turbo_str_split(TurboStr* s, TurboStr* sep);
TurboStr* turbo_str_join(TurboStr* sep, TurboPtrList* list);
TurboStr* turbo_str_upper(TurboStr* s);
TurboStr* turbo_str_lower(TurboStr* s);
TurboStr* turbo_str_replace(TurboStr* s, TurboStr* old_s, TurboStr* new_s);
int64_t turbo_str_find(TurboStr* s, TurboStr* sub);
int64_t turbo_str_index(TurboStr* s, TurboStr* sub);
TurboStr* turbo_str_strip(TurboStr* s);
TurboStr* turbo_str_lstrip(TurboStr* s);
TurboStr* turbo_str_rstrip(TurboStr* s);
int32_t turbo_str_startswith(TurboStr* s, TurboStr* prefix);
int32_t turbo_str_endswith(TurboStr* s, TurboStr* suffix);
int64_t turbo_str_count(TurboStr* s, TurboStr* sub);

/* Conversions & Formatting */
TurboStr* turbo_int_to_str(int64_t v);
TurboStr* turbo_uint_to_str(uint64_t v);
TurboStr* turbo_float_to_str(double v);
TurboStr* turbo_float_fmt(double v, int32_t precision);
TurboStr* turbo_bool_to_str(int32_t v);
int64_t turbo_str_to_int(TurboStr* s);
double turbo_str_to_float(TurboStr* s);
int64_t turbo_ord(TurboStr* s);
TurboStr* turbo_chr(int64_t code);
TurboStr* turbo_hex(int64_t v);
TurboStr* turbo_bin(int64_t v);
TurboStr* turbo_oct(int64_t v);
int64_t turbo_pow_i64(int64_t base, int64_t exp);

/* Bytes */
TurboBytes* turbo_bytes_new(const uint8_t* data, int32_t len);
int32_t turbo_bytes_eq(TurboBytes* a, TurboBytes* b);

/* BigInt */
TurboBigInt* turbo_bigint_from_str(const char* s);
TurboBigInt* turbo_bigint_from_i64(int64_t v);
TurboBigInt* turbo_bigint_add(TurboBigInt* a, TurboBigInt* b);
TurboBigInt* turbo_bigint_sub(TurboBigInt* a, TurboBigInt* b);
TurboBigInt* turbo_bigint_mul(TurboBigInt* a, TurboBigInt* b);
TurboBigInt* turbo_bigint_div(TurboBigInt* a, TurboBigInt* b);
TurboBigInt* turbo_bigint_mod(TurboBigInt* a, TurboBigInt* b);
TurboBigInt* turbo_bigint_pow(TurboBigInt* a, TurboBigInt* b);
TurboBigInt* turbo_bigint_neg(TurboBigInt* a);
TurboBigInt* turbo_bigint_abs(TurboBigInt* a);
int32_t turbo_bigint_cmp(TurboBigInt* a, TurboBigInt* b);
TurboStr* turbo_bigint_to_str(TurboBigInt* a);

/* File I/O */
TurboFile* turbo_open(TurboStr* path, TurboStr* mode);
TurboStr* turbo_file_read(TurboFile* f);
void turbo_file_write(TurboFile* f, TurboStr* text);
void turbo_file_close(TurboFile* f);

/* Print & Input */
void turbo_print_i64(int64_t v);
void turbo_print_u64(uint64_t v);
void turbo_print_f64(double v);
void turbo_print_bool(int32_t v);
void turbo_print_str(TurboStr* s);
void turbo_print_cstr(const char* s);
void turbo_print_space(void);
void turbo_print_newline(void);
TurboStr* turbo_input(TurboStr* prompt);

#endif
