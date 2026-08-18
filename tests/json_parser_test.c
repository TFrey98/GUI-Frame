/*
 * Exercises json_parse() - object/array/string/number/bool/null values,
 * nesting, escapes (including \uXXXX), and rejection of malformed input
 * (trailing garbage, unterminated strings, bad literals) - the exact
 * subset of JSON tool manifests and their data rows rely on.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "tools/json_parser.h"

static int status = 0;

static void fail(const char *message) {
    fprintf(stderr, "json_parser_test: %s\n", message);
    status = 1;
}

int main(void) {
    /* Scalars at the top level. */
    JsonValue *v = json_parse("null");
    if (!v || json_value_type(v) != JSON_NULL) {
        fail("null should parse as JSON_NULL");
    }
    json_value_free(v);

    v = json_parse("true");
    bool b = false;
    if (!v || !json_as_bool(v, &b) || !b) {
        fail("true should parse as JSON_BOOL(true)");
    }
    json_value_free(v);

    v = json_parse(" -12.5e2 ");
    double n = 0;
    if (!v || !json_as_number(v, &n) || fabs(n - (-1250.0)) > 0.0001) {
        fail("-12.5e2 should parse to -1250.0");
    }
    json_value_free(v);

    v = json_parse("\"hello\\nworld \\u00e9\"");
    const char *s = NULL;
    if (!v || !json_as_string(v, &s) || strcmp(s, "hello\nworld \xc3\xa9") != 0) {
        fail("string escapes (\\n and \\u00e9) did not decode correctly");
    }
    json_value_free(v);

    /* Object + array + nesting, matching the manifest shape. */
    const char *manifest = "{ \"panel\": { \"title\": \"Nmap Results\", \"data_file\": \"out.jsonl\", "
                            "\"columns\": [ {\"key\": \"host\", \"label\": \"Host\"}, "
                            "{\"key\": \"port\", \"label\": \"Port\"} ] } }";
    v = json_parse(manifest);
    if (!v || json_value_type(v) != JSON_OBJECT) {
        fail("manifest text should parse as a top-level object");
    } else {
        const JsonValue *panel = json_object_get(v, "panel");
        const JsonValue *title = panel ? json_object_get(panel, "title") : NULL;
        if (!title || !json_as_string(title, &s) || strcmp(s, "Nmap Results") != 0) {
            fail("panel.title should be \"Nmap Results\"");
        }
        const JsonValue *columns = panel ? json_object_get(panel, "columns") : NULL;
        if (!columns || json_value_type(columns) != JSON_ARRAY || json_array_count(columns) != 2) {
            fail("panel.columns should be a 2-element array");
        } else {
            const JsonValue *col0 = json_array_get(columns, 0);
            const JsonValue *key0 = col0 ? json_object_get(col0, "key") : NULL;
            if (!key0 || !json_as_string(key0, &s) || strcmp(s, "host") != 0) {
                fail("columns[0].key should be \"host\"");
            }
        }
        if (json_object_get(v, "does_not_exist") != NULL) {
            fail("json_object_get on a missing key should return NULL");
        }
    }
    json_value_free(v);

    /* Empty object/array. */
    v = json_parse("{}");
    if (!v || json_value_type(v) != JSON_OBJECT) {
        fail("{} should parse as an empty JSON_OBJECT");
    }
    json_value_free(v);

    v = json_parse("[]");
    if (!v || json_value_type(v) != JSON_ARRAY || json_array_count(v) != 0) {
        fail("[] should parse as an empty JSON_ARRAY");
    }
    json_value_free(v);

    /* Malformed input must fail cleanly (no crash, NULL return). */
    static const char *const bad_inputs[] = {
        "",
        "{",
        "[1, 2,]",
        "{\"a\": }",
        "\"unterminated",
        "tru",
        "123abc",
        "{\"a\": 1} trailing garbage",
    };
    for (size_t i = 0; i < sizeof(bad_inputs) / sizeof(bad_inputs[0]); i++) {
        JsonValue *bad = json_parse(bad_inputs[i]);
        if (bad != NULL) {
            fprintf(stderr, "json_parser_test: malformed input \"%s\" should have failed to parse\n",
                    bad_inputs[i]);
            status = 1;
            json_value_free(bad);
        }
    }

    if (status == 0) {
        printf("json_parser_test: scalars, nesting, escapes, and malformed-input rejection all verified\n");
    }
    return status;
}
