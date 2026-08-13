#ifndef WORKBENCH_FILE_CLASSIFY_H
#define WORKBENCH_FILE_CLASSIFY_H

#include <stdbool.h>

typedef enum FileOpenTarget {
    FILE_TARGET_EDITOR,
    FILE_TARGET_BINARY_INFO,
    FILE_TARGET_UNSUPPORTED
} FileOpenTarget;

typedef struct FileClassification {
    FileOpenTarget target;
    bool executable;
    bool likely_text;
    const char *language_name;
} FileClassification;

/*
 * Classifies absolute_path for opening - independently stat()s it
 * first (anything but a regular file is FILE_TARGET_UNSUPPORTED,
 * regardless of what the caller already believed about it), then reads
 * a small sniff window and looks for a NUL byte or a high proportion of
 * non-text control bytes - never the file's extension - to decide
 * FILE_TARGET_EDITOR vs FILE_TARGET_BINARY_INFO. executable is passed
 * straight through into the result, purely informational (an
 * executable *text* file - a script - still opens as EDITOR).
 */
FileClassification file_classify(const char *absolute_path, bool executable);

#endif /* WORKBENCH_FILE_CLASSIFY_H */
