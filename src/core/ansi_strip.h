#ifndef WORKBENCH_ANSI_STRIP_H
#define WORKBENCH_ANSI_STRIP_H

#include <stddef.h>

/*
 * Removes ANSI/VT100 terminal escape sequences (SGR color codes like
 * "\x1b[91m", cursor movement, OSC title/hyperlink sequences, ...) from a
 * byte stream - what makes captured terminal output human-readable once
 * exported, instead of full of color-code noise. Carries a sequence
 * that's still incomplete at the end of one feed() call over to the next,
 * so a sequence split across a chunk boundary (e.g. a 4096-byte pty read)
 * still gets fully removed rather than leaking a fragment as literal
 * text. Only ever used on the copy of a chunk headed for the database -
 * the raw bytes used for on-screen display/replay are never touched,
 * since VTE needs the real escape sequences to render color/cursor
 * control correctly.
 */
typedef struct AnsiStripper AnsiStripper;

AnsiStripper *ansi_stripper_create(void);
void ansi_stripper_destroy(AnsiStripper *stripper);

/* Discards any incomplete escape sequence carried over from a previous
 * feed() call. Call this whenever the caller stops persisting this
 * stripper's output (e.g. the capture window closes), so a stale partial
 * sequence left over from an abandoned window can't bleed into a later,
 * unrelated one. */
void ansi_stripper_reset(AnsiStripper *stripper);

/* Strips data (len bytes) and writes the result into a newly malloc'd
 * buffer at *out (caller must free() it - safe to free(NULL)), with the
 * byte count in *out_len. *out_len is never greater than len; both may be
 * 0 with *out left NULL if this call's bytes ended exactly on an
 * incomplete escape sequence (the pending bytes are held internally and
 * completed by combining them with the start of the next feed() call). */
void ansi_stripper_feed(AnsiStripper *stripper, const void *data, size_t len, unsigned char **out, size_t *out_len);

#endif /* WORKBENCH_ANSI_STRIP_H */
