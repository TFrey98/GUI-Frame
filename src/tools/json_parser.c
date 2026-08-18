#include "json_parser.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

struct JsonValue {
    JsonType type;
    union {
        bool boolean;
        double number;
        char *string;
        struct {
            JsonValue **items;
            int count;
        } array;
        struct {
            char **keys;
            JsonValue **values;
            int count;
        } object;
    };
};

/* Parses in one pass over a borrowed, NUL-terminated buffer - cursor
 * only ever advances, and error latches permanently once set so every
 * caller further up the recursive-descent chain can just check it once
 * at the end instead of threading a return code through every call. */
typedef struct JsonParser {
    const char *cursor;
    bool error;
} JsonParser;

static JsonValue *parse_value(JsonParser *p);

static void skip_whitespace(JsonParser *p) {
    while (*p->cursor == ' ' || *p->cursor == '\t' || *p->cursor == '\n' || *p->cursor == '\r') {
        p->cursor++;
    }
}

static JsonValue *value_new(JsonType type) {
    JsonValue *value = calloc(1, sizeof(JsonValue));
    value->type = type;
    return value;
}

/* Encodes a single UTF-16 code unit (from a \uXXXX escape) as UTF-8.
 * Supplementary-plane characters (surrogate pairs) aren't reassembled -
 * each surrogate half is encoded as its own (invalid, but harmless)
 * 3-byte sequence rather than rejecting the whole document; nothing in
 * a tool manifest or its data rows is expected to need them. */
static void append_utf8_codepoint(char **out, size_t *len, size_t *cap, unsigned int cp) {
    char buf[4];
    int n;
    if (cp < 0x80) {
        buf[0] = (char)cp;
        n = 1;
    } else if (cp < 0x800) {
        buf[0] = (char)(0xC0 | (cp >> 6));
        buf[1] = (char)(0x80 | (cp & 0x3F));
        n = 2;
    } else {
        buf[0] = (char)(0xE0 | (cp >> 12));
        buf[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[2] = (char)(0x80 | (cp & 0x3F));
        n = 3;
    }
    if (*len + (size_t)n + 1 > *cap) {
        *cap = (*cap + (size_t)n + 1) * 2;
        *out = realloc(*out, *cap);
    }
    memcpy(*out + *len, buf, (size_t)n);
    *len += (size_t)n;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
    return -1;
}

/* Cursor must be positioned on the opening quote. Returns a malloc'd,
 * NUL-terminated string with escapes resolved, or NULL (setting
 * p->error) on an unterminated string or invalid escape. */
static char *parse_string_raw(JsonParser *p) {
    p->cursor++; /* opening quote */
    size_t cap = 32, len = 0;
    char *out = malloc(cap);
    out[0] = '\0';

    while (*p->cursor != '"') {
        if (*p->cursor == '\0') {
            p->error = true;
            free(out);
            return NULL;
        }
        if (*p->cursor == '\\') {
            p->cursor++;
            char esc = *p->cursor;
            char literal = '\0';
            switch (esc) {
                case '"': literal = '"'; break;
                case '\\': literal = '\\'; break;
                case '/': literal = '/'; break;
                case 'b': literal = '\b'; break;
                case 'f': literal = '\f'; break;
                case 'n': literal = '\n'; break;
                case 'r': literal = '\r'; break;
                case 't': literal = '\t'; break;
                case 'u': {
                    unsigned int cp = 0;
                    p->cursor++;
                    for (int i = 0; i < 4; i++) {
                        int d = hex_digit(p->cursor[i]);
                        if (d < 0) {
                            p->error = true;
                            free(out);
                            return NULL;
                        }
                        cp = (cp << 4) | (unsigned int)d;
                    }
                    p->cursor += 4; /* past all 4 hex digits, onto whatever follows the escape */
                    append_utf8_codepoint(&out, &len, &cap, cp);
                    continue;
                }
                default:
                    p->error = true;
                    free(out);
                    return NULL;
            }
            if (len + 2 > cap) {
                cap *= 2;
                out = realloc(out, cap);
            }
            out[len++] = literal;
            p->cursor++;
        } else {
            if (len + 2 > cap) {
                cap *= 2;
                out = realloc(out, cap);
            }
            out[len++] = *p->cursor;
            p->cursor++;
        }
    }
    p->cursor++; /* closing quote */
    out[len] = '\0';
    return out;
}

static JsonValue *parse_string(JsonParser *p) {
    char *s = parse_string_raw(p);
    if (p->error) {
        return NULL;
    }
    JsonValue *value = value_new(JSON_STRING);
    value->string = s;
    return value;
}

static JsonValue *parse_number(JsonParser *p) {
    const char *start = p->cursor;
    if (*p->cursor == '-') {
        p->cursor++;
    }
    if (!isdigit((unsigned char)*p->cursor)) {
        p->error = true;
        return NULL;
    }
    while (isdigit((unsigned char)*p->cursor)) {
        p->cursor++;
    }
    if (*p->cursor == '.') {
        p->cursor++;
        if (!isdigit((unsigned char)*p->cursor)) {
            p->error = true;
            return NULL;
        }
        while (isdigit((unsigned char)*p->cursor)) {
            p->cursor++;
        }
    }
    if (*p->cursor == 'e' || *p->cursor == 'E') {
        p->cursor++;
        if (*p->cursor == '+' || *p->cursor == '-') {
            p->cursor++;
        }
        if (!isdigit((unsigned char)*p->cursor)) {
            p->error = true;
            return NULL;
        }
        while (isdigit((unsigned char)*p->cursor)) {
            p->cursor++;
        }
    }
    JsonValue *value = value_new(JSON_NUMBER);
    value->number = strtod(start, NULL);
    return value;
}

static bool match_literal(JsonParser *p, const char *literal) {
    size_t len = strlen(literal);
    if (strncmp(p->cursor, literal, len) != 0) {
        return false;
    }
    p->cursor += len;
    return true;
}

static JsonValue *parse_array(JsonParser *p) {
    p->cursor++; /* '[' */
    JsonValue *value = value_new(JSON_ARRAY);
    size_t cap = 4;
    value->array.items = malloc(cap * sizeof(JsonValue *));
    value->array.count = 0;

    skip_whitespace(p);
    if (*p->cursor == ']') {
        p->cursor++;
        return value;
    }

    while (true) {
        skip_whitespace(p);
        JsonValue *item = parse_value(p);
        if (p->error) {
            json_value_free(value);
            return NULL;
        }
        if ((size_t)value->array.count >= cap) {
            cap *= 2;
            value->array.items = realloc(value->array.items, cap * sizeof(JsonValue *));
        }
        value->array.items[value->array.count++] = item;

        skip_whitespace(p);
        if (*p->cursor == ',') {
            p->cursor++;
            continue;
        }
        if (*p->cursor == ']') {
            p->cursor++;
            break;
        }
        p->error = true;
        json_value_free(value);
        return NULL;
    }
    return value;
}

static JsonValue *parse_object(JsonParser *p) {
    p->cursor++; /* '{' */
    JsonValue *value = value_new(JSON_OBJECT);
    size_t cap = 4;
    value->object.keys = malloc(cap * sizeof(char *));
    value->object.values = malloc(cap * sizeof(JsonValue *));
    value->object.count = 0;

    skip_whitespace(p);
    if (*p->cursor == '}') {
        p->cursor++;
        return value;
    }

    while (true) {
        skip_whitespace(p);
        if (*p->cursor != '"') {
            p->error = true;
            json_value_free(value);
            return NULL;
        }
        char *key = parse_string_raw(p);
        if (p->error) {
            json_value_free(value);
            return NULL;
        }

        skip_whitespace(p);
        if (*p->cursor != ':') {
            p->error = true;
            free(key);
            json_value_free(value);
            return NULL;
        }
        p->cursor++;

        skip_whitespace(p);
        JsonValue *item = parse_value(p);
        if (p->error) {
            free(key);
            json_value_free(value);
            return NULL;
        }

        if ((size_t)value->object.count >= cap) {
            cap *= 2;
            value->object.keys = realloc(value->object.keys, cap * sizeof(char *));
            value->object.values = realloc(value->object.values, cap * sizeof(JsonValue *));
        }
        value->object.keys[value->object.count] = key;
        value->object.values[value->object.count] = item;
        value->object.count++;

        skip_whitespace(p);
        if (*p->cursor == ',') {
            p->cursor++;
            continue;
        }
        if (*p->cursor == '}') {
            p->cursor++;
            break;
        }
        p->error = true;
        json_value_free(value);
        return NULL;
    }
    return value;
}

static JsonValue *parse_value(JsonParser *p) {
    skip_whitespace(p);
    switch (*p->cursor) {
        case '{': return parse_object(p);
        case '[': return parse_array(p);
        case '"': return parse_string(p);
        case 't':
            if (match_literal(p, "true")) {
                JsonValue *value = value_new(JSON_BOOL);
                value->boolean = true;
                return value;
            }
            break;
        case 'f':
            if (match_literal(p, "false")) {
                JsonValue *value = value_new(JSON_BOOL);
                value->boolean = false;
                return value;
            }
            break;
        case 'n':
            if (match_literal(p, "null")) {
                return value_new(JSON_NULL);
            }
            break;
        default:
            if (*p->cursor == '-' || isdigit((unsigned char)*p->cursor)) {
                return parse_number(p);
            }
            break;
    }
    p->error = true;
    return NULL;
}

JsonValue *json_parse(const char *text) {
    JsonParser p = {.cursor = text, .error = false};
    JsonValue *value = parse_value(&p);
    if (p.error) {
        json_value_free(value);
        return NULL;
    }
    skip_whitespace(&p);
    if (*p.cursor != '\0') {
        json_value_free(value);
        return NULL;
    }
    return value;
}

void json_value_free(JsonValue *value) {
    if (!value) {
        return;
    }
    switch (value->type) {
        case JSON_STRING:
            free(value->string);
            break;
        case JSON_ARRAY:
            for (int i = 0; i < value->array.count; i++) {
                json_value_free(value->array.items[i]);
            }
            free(value->array.items);
            break;
        case JSON_OBJECT:
            for (int i = 0; i < value->object.count; i++) {
                free(value->object.keys[i]);
                json_value_free(value->object.values[i]);
            }
            free(value->object.keys);
            free(value->object.values);
            break;
        default:
            break;
    }
    free(value);
}

JsonType json_value_type(const JsonValue *value) {
    return value->type;
}

const JsonValue *json_object_get(const JsonValue *value, const char *key) {
    if (!value || value->type != JSON_OBJECT) {
        return NULL;
    }
    for (int i = 0; i < value->object.count; i++) {
        if (strcmp(value->object.keys[i], key) == 0) {
            return value->object.values[i];
        }
    }
    return NULL;
}

int json_array_count(const JsonValue *value) {
    if (!value || value->type != JSON_ARRAY) {
        return 0;
    }
    return value->array.count;
}

const JsonValue *json_array_get(const JsonValue *value, int index) {
    if (!value || value->type != JSON_ARRAY || index < 0 || index >= value->array.count) {
        return NULL;
    }
    return value->array.items[index];
}

bool json_as_string(const JsonValue *value, const char **out) {
    if (!value || value->type != JSON_STRING) {
        return false;
    }
    *out = value->string;
    return true;
}

bool json_as_number(const JsonValue *value, double *out) {
    if (!value || value->type != JSON_NUMBER) {
        return false;
    }
    *out = value->number;
    return true;
}

bool json_as_bool(const JsonValue *value, bool *out) {
    if (!value || value->type != JSON_BOOL) {
        return false;
    }
    *out = value->boolean;
    return true;
}
