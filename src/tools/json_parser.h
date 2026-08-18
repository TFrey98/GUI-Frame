#ifndef WORKBENCH_JSON_PARSER_H
#define WORKBENCH_JSON_PARSER_H

#include <stdbool.h>

typedef enum JsonType {
    JSON_NULL,
    JSON_BOOL,
    JSON_NUMBER,
    JSON_STRING,
    JSON_ARRAY,
    JSON_OBJECT
} JsonType;

typedef struct JsonValue JsonValue;

/*
 * Minimal recursive-descent JSON parser - covers exactly what tool
 * manifests and their JSON-Lines data files need (objects, arrays,
 * strings, numbers, booleans, null), not the full grammar's edge cases
 * (no \uXXXX surrogate pairs, numbers parsed via strtod rather than a
 * spec-exact grammar). text must be NUL-terminated. Returns NULL on any
 * malformed input, including trailing non-whitespace garbage after the
 * top-level value - never partially succeeds. Caller owns the result.
 */
JsonValue *json_parse(const char *text);
void json_value_free(JsonValue *value);

JsonType json_value_type(const JsonValue *value);

/* NULL if value isn't a JSON_OBJECT or key isn't present. Returned
 * pointer is borrowed, valid until value is freed. */
const JsonValue *json_object_get(const JsonValue *value, const char *key);

/* 0/NULL if value isn't a JSON_ARRAY. */
int json_array_count(const JsonValue *value);
const JsonValue *json_array_get(const JsonValue *value, int index);

/* Scalar accessors - each returns false and leaves *out untouched if
 * value's type doesn't match. The string form's *out is borrowed,
 * valid until value is freed. */
bool json_as_string(const JsonValue *value, const char **out);
bool json_as_number(const JsonValue *value, double *out);
bool json_as_bool(const JsonValue *value, bool *out);

#endif /* WORKBENCH_JSON_PARSER_H */
