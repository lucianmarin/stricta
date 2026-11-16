#ifndef STRICTA_RUNTIME_H
#define STRICTA_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include <math.h>

/* Lean Static Runtime for Stricta — Zero NaN-boxing, Zero Dynamic Dispatch */

typedef struct {
    int32_t length;
    uint32_t hash;
    char* chars;
} StrictaStr;

typedef struct {
    int32_t length;
    int32_t _pad;
    uint8_t* data;
} StrictaBytes;

typedef struct {
    int32_t length;
    int32_t capacity;
    void** items;
} StrictaPtrList;

typedef struct {
    int32_t length;
    int32_t sign;
    char* digits;
} StrictaBigInt;

typedef struct {
    FILE* handle;
} StrictaFile;

/* Arena / Memory helpers */
void* stricta_alloc(size_t size);
void* stricta_calloc(size_t count, size_t size);
void* stricta_realloc(void* ptr, size_t size);
void stricta_free(void* ptr);

/* Initialization & CLI args */
extern StrictaPtrList* stricta_sys_argv;
void stricta_init_args(int32_t argc, char** argv);

/* Exception handling (setjmp/longjmp) */
jmp_buf* stricta_try_push(void);
void stricta_try_pop(void);
void stricta_raise(int32_t type_id, void* exc_obj, const char* msg);
void stricta_reraise(void);
int32_t stricta_exc_type_id(void);
void* stricta_exc_obj(void);
StrictaStr* stricta_exc_msg(void);
void stricta_assert_fail(const char* msg, const char* file, int32_t line);

/* Strings */
StrictaStr* stricta_str_new(const char* s, int32_t len);
StrictaStr* stricta_str_cstr(const char* s);
StrictaStr* stricta_str_concat(StrictaStr* a, StrictaStr* b);
StrictaStr* stricta_str_repeat(StrictaStr* s, int64_t count);
StrictaStr* stricta_str_slice(StrictaStr* s, int64_t start, int64_t end);
StrictaStr* stricta_str_char_at(StrictaStr* s, int64_t idx);
int32_t stricta_str_eq(StrictaStr* a, StrictaStr* b);
int32_t stricta_str_cmp(StrictaStr* a, StrictaStr* b);
uint64_t stricta_str_hash(StrictaStr* s);
int32_t stricta_str_contains(StrictaStr* sub, StrictaStr* s);
StrictaPtrList* stricta_str_split(StrictaStr* s, StrictaStr* sep);
StrictaStr* stricta_str_join(StrictaStr* sep, StrictaPtrList* list);
StrictaStr* stricta_str_upper(StrictaStr* s);
StrictaStr* stricta_str_lower(StrictaStr* s);
StrictaStr* stricta_str_replace(StrictaStr* s, StrictaStr* old_s, StrictaStr* new_s);
int64_t stricta_str_find(StrictaStr* s, StrictaStr* sub);
int64_t stricta_str_index(StrictaStr* s, StrictaStr* sub);
StrictaStr* stricta_str_strip(StrictaStr* s);
StrictaStr* stricta_str_lstrip(StrictaStr* s);
StrictaStr* stricta_str_rstrip(StrictaStr* s);
int32_t stricta_str_startswith(StrictaStr* s, StrictaStr* prefix);
int32_t stricta_str_endswith(StrictaStr* s, StrictaStr* suffix);
int64_t stricta_str_count(StrictaStr* s, StrictaStr* sub);

/* Conversions & Formatting */
StrictaStr* stricta_int_to_str(int64_t v);
StrictaStr* stricta_uint_to_str(uint64_t v);
StrictaStr* stricta_float_to_str(double v);
StrictaStr* stricta_float_fmt(double v, int32_t precision);
StrictaStr* stricta_bool_to_str(int32_t v);
int64_t stricta_str_to_int(StrictaStr* s);
double stricta_str_to_float(StrictaStr* s);
int64_t stricta_ord(StrictaStr* s);
StrictaStr* stricta_chr(int64_t code);
StrictaStr* stricta_hex(int64_t v);
StrictaStr* stricta_bin(int64_t v);
StrictaStr* stricta_oct(int64_t v);
int64_t stricta_pow_i64(int64_t base, int64_t exp);

/* Bytes */
StrictaBytes* stricta_bytes_new(const uint8_t* data, int32_t len);
int32_t stricta_bytes_eq(StrictaBytes* a, StrictaBytes* b);

/* BigInt */
StrictaBigInt* stricta_bigint_from_str(const char* s);
StrictaBigInt* stricta_bigint_from_i64(int64_t v);
StrictaBigInt* stricta_bigint_add(StrictaBigInt* a, StrictaBigInt* b);
StrictaBigInt* stricta_bigint_sub(StrictaBigInt* a, StrictaBigInt* b);
StrictaBigInt* stricta_bigint_mul(StrictaBigInt* a, StrictaBigInt* b);
StrictaBigInt* stricta_bigint_div(StrictaBigInt* a, StrictaBigInt* b);
StrictaBigInt* stricta_bigint_mod(StrictaBigInt* a, StrictaBigInt* b);
StrictaBigInt* stricta_bigint_pow(StrictaBigInt* a, StrictaBigInt* b);
StrictaBigInt* stricta_bigint_neg(StrictaBigInt* a);
StrictaBigInt* stricta_bigint_abs(StrictaBigInt* a);
int32_t stricta_bigint_cmp(StrictaBigInt* a, StrictaBigInt* b);
StrictaStr* stricta_bigint_to_str(StrictaBigInt* a);

/* File I/O */
StrictaFile* stricta_open(StrictaStr* path, StrictaStr* mode);
StrictaStr* stricta_file_read(StrictaFile* f);
void stricta_file_write(StrictaFile* f, StrictaStr* text);
void stricta_file_close(StrictaFile* f);

/* Print & Input */
void stricta_print_i64(int64_t v);
void stricta_print_u64(uint64_t v);
void stricta_print_f64(double v);
void stricta_print_bool(int32_t v);
void stricta_print_str(StrictaStr* s);
void stricta_print_cstr(const char* s);
void stricta_print_space(void);
void stricta_print_newline(void);
StrictaStr* stricta_input(StrictaStr* prompt);

#endif
