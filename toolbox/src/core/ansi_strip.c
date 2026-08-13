#include "ansi_strip.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Real escape sequences (SGR color codes, cursor moves, even a long
 * 256-color/truecolor SGR run) are at most a couple dozen bytes - this is
 * a generous bound on how much of a not-yet-terminated sequence is worth
 * carrying to the next feed() call. Past this, it's not a real sequence
 * (or at least not one worth blocking output on), so it gets flushed as
 * literal text instead of buffered forever. */
#define ANSI_STRIPPER_MAX_PENDING 128

struct AnsiStripper {
    unsigned char pending[ANSI_STRIPPER_MAX_PENDING];
    size_t pending_len;
};

AnsiStripper *ansi_stripper_create(void) {
    AnsiStripper *stripper = malloc(sizeof(AnsiStripper));
    stripper->pending_len = 0;
    return stripper;
}

void ansi_stripper_destroy(AnsiStripper *stripper) {
    free(stripper);
}

void ansi_stripper_reset(AnsiStripper *stripper) {
    stripper->pending_len = 0;
}

/* buf[0] is always ESC (0x1b) here. Classifies buf[0..len) as:
 *  - a complete, recognized escape sequence -> returns its length (> 0)
 *  - a sequence that needs more bytes to know -> returns 0, *incomplete = true
 *  - not (or no longer) a recognized escape -> returns 0, *incomplete = false
 *    (caller treats the lone ESC byte as literal and moves on one byte)
 */
static size_t match_escape(const unsigned char *buf, size_t len, bool *incomplete) {
    *incomplete = false;
    if (len < 2) {
        *incomplete = true;
        return 0;
    }

    if (buf[1] == '[') {
        /* CSI: ESC [ <parameter bytes 0x30-0x3F>* <intermediate bytes 0x20-0x2F>* <final byte 0x40-0x7E> */
        size_t j = 2;
        while (j < len && buf[j] >= 0x30 && buf[j] <= 0x3F) {
            j++;
        }
        while (j < len && buf[j] >= 0x20 && buf[j] <= 0x2F) {
            j++;
        }
        if (j >= len) {
            *incomplete = true;
            return 0;
        }
        if (buf[j] >= 0x40 && buf[j] <= 0x7E) {
            return j + 1;
        }
        return 0; /* not a valid final byte - not really a CSI sequence */
    }

    if (buf[1] == ']') {
        /* OSC: ESC ] ... terminated by BEL (0x07) or ESC \ (ST) */
        for (size_t j = 2; j < len; j++) {
            if (buf[j] == 0x07) {
                return j + 1;
            }
            if (buf[j] == 0x1b) {
                if (j + 1 >= len) {
                    *incomplete = true;
                    return 0;
                }
                if (buf[j + 1] == '\\') {
                    return j + 2;
                }
                return 0; /* ESC mid-OSC not followed by \ - malformed, give up on it */
            }
        }
        *incomplete = true;
        return 0;
    }

    if (buf[1] >= 0x40 && buf[1] <= 0x5f) {
        return 2; /* simple two-byte escape (ESC 7, ESC 8, ESC c, ESC M, ...) */
    }

    return 0;
}

void ansi_stripper_feed(AnsiStripper *stripper, const void *data, size_t len, unsigned char **out, size_t *out_len) {
    size_t combined_len = stripper->pending_len + len;
    unsigned char *combined = malloc(combined_len > 0 ? combined_len : 1);
    memcpy(combined, stripper->pending, stripper->pending_len);
    memcpy(combined + stripper->pending_len, data, len);
    stripper->pending_len = 0;

    unsigned char *result = malloc(combined_len > 0 ? combined_len : 1);
    size_t result_len = 0;

    size_t i = 0;
    while (i < combined_len) {
        unsigned char c = combined[i];
        if (c == 0x1b) {
            bool incomplete = false;
            size_t match_len = match_escape(combined + i, combined_len - i, &incomplete);
            if (match_len > 0) {
                i += match_len;
                continue;
            }
            if (incomplete) {
                size_t remaining = combined_len - i;
                if (remaining <= ANSI_STRIPPER_MAX_PENDING) {
                    memcpy(stripper->pending, combined + i, remaining);
                    stripper->pending_len = remaining;
                    free(combined);
                    if (result_len == 0) {
                        free(result);
                        *out = NULL;
                        *out_len = 0;
                        return;
                    }
                    *out = result;
                    *out_len = result_len;
                    return;
                }
                /* Too long to plausibly still be a real sequence - give up
                 * and fall through to emitting the ESC byte as literal. */
            }
            result[result_len++] = c;
            i++;
            continue;
        }
        result[result_len++] = c;
        i++;
    }

    free(combined);
    if (result_len == 0) {
        free(result);
        *out = NULL;
        *out_len = 0;
        return;
    }
    *out = result;
    *out_len = result_len;
}
