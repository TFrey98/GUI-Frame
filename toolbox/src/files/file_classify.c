#include "file_classify.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define FILE_CLASSIFY_SNIFF_SIZE 8192

static const char *language_name_for(const char *absolute_path) {
    const char *dot = strrchr(absolute_path, '.');
    if (!dot) {
        return "Plain Text";
    }
    if (strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0) {
        return "C";
    }
    if (strcmp(dot, ".py") == 0) {
        return "Python";
    }
    if (strcmp(dot, ".sh") == 0) {
        return "Shell";
    }
    if (strcmp(dot, ".js") == 0) {
        return "JavaScript";
    }
    if (strcmp(dot, ".json") == 0) {
        return "JSON";
    }
    if (strcmp(dot, ".md") == 0) {
        return "Markdown";
    }
    return "Plain Text";
}

static bool sniff_is_text(const unsigned char *buf, size_t len) {
    if (len == 0) {
        return true; /* empty file: trivially text */
    }
    size_t suspicious = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = buf[i];
        if (c == 0) {
            return false; /* a NUL byte is a definitive binary signal */
        }
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r') {
            suspicious++;
        }
    }
    /* Small, deliberately generous threshold - real text files
     * essentially never contain any control bytes beyond tab/newline/CR,
     * so even a low bar catches genuine binary content without
     * false-positiving on odd-but-real text. */
    return suspicious * 100 < len * 2; /* < 2% suspicious bytes */
}

FileClassification file_classify(const char *absolute_path, bool executable) {
    FileClassification result = {0};
    result.executable = executable;
    result.language_name = "Plain Text";

    struct stat st;
    if (stat(absolute_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        result.target = FILE_TARGET_UNSUPPORTED;
        return result;
    }

    FILE *f = fopen(absolute_path, "rb");
    if (!f) {
        result.target = FILE_TARGET_UNSUPPORTED;
        return result;
    }

    unsigned char buf[FILE_CLASSIFY_SNIFF_SIZE];
    size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    result.likely_text = sniff_is_text(buf, n);
    if (result.likely_text) {
        result.target = FILE_TARGET_EDITOR;
        result.language_name = language_name_for(absolute_path);
    } else {
        result.target = FILE_TARGET_BINARY_INFO;
    }
    return result;
}
