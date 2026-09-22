/*
 * Regression test for terminal text breaking up when the layout is resized.
 *
 * The terminals own their pty directly rather than using VTE's spawn, so
 * VTE is a display surface fed with bytes while the shell is driven
 * separately. With VTE's rewrap-on-resize left enabled, a resize made VTE
 * reflow its buffer and move the cursor underneath the shell, whose own
 * SIGWINCH redraw then landed relative to a position that no longer held.
 * Dragging the sidebar handle scattered fragments of the prompt across the
 * screen at arbitrary columns.
 *
 * The invariant asserted here: this test starts a shell and resizes, but
 * runs no command and produces no output, so everything the user can see
 * is a prompt the shell wrote at column zero. A visible line whose text
 * begins at some other column is text placed where nobody asked for it.
 *
 * Only the visible screen is examined. With rewrap disabled the scrollback
 * legitimately retains lines wider than the current window, so indented
 * text there is expected and is not what the user is looking at.
 */
#include <gtk/gtk.h>
#include <vte/vte.h>
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "test_gtk_utils.h"

#define TICK_MS 25
#define SETTLE_TICKS 20   /* let the shell start and print its first prompt */
#define TRAILING_TICKS 30 /* let the last redraw finish before reading back */
#define MAX_SWEEP 400

typedef struct TestState {
    int tick;
    gboolean pressed_enter;
    double buffer_rows_before; /* scrollback extent before any resizing */
    int sweep_index;
    int sweep[MAX_SWEEP];
    int sweep_len;
    gboolean failed;
    gboolean done;
} TestState;

static void close_all_toplevels(void) {
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next) {
        gtk_window_close(GTK_WINDOW(l->data));
    }
    g_list_free(windows);
}

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "terminal_resize_redraw_smoke: %s\n", msg);
    test->failed = TRUE;
    test->done = TRUE;
    close_all_toplevels();
}

/* A single coarse jump does not reproduce the fault - it takes a stream of
 * small steps, the way a real drag delivers them. */
static void build_sweep(TestState *test) {
    int i = 0;
    for (int pass = 0; pass < 3; pass++) {
        for (int x = 230; x <= 700 && i < MAX_SWEEP; x += 12) test->sweep[i++] = x;
        for (int x = 700; x >= 230 && i < MAX_SWEEP; x -= 12) test->sweep[i++] = x;
    }
    test->sweep_len = i;
}

static GtkWidget *find_hpaned(GtkWidget *window) {
    GPtrArray *paneds = g_ptr_array_new();
    collect_by_type(window, paneds, GTK_TYPE_PANED);
    GtkWidget *found = NULL;
    for (guint i = 0; i < paneds->len; i++) {
        GtkWidget *candidate = g_ptr_array_index(paneds, i);
        if (gtk_orientable_get_orientation(GTK_ORIENTABLE(candidate)) == GTK_ORIENTATION_HORIZONTAL) {
            found = candidate;
            break;
        }
    }
    g_ptr_array_free(paneds, TRUE);
    return found;
}

static GtkWidget *find_vte(GtkWidget *window) {
    GPtrArray *vtes = g_ptr_array_new();
    collect_by_type(window, vtes, VTE_TYPE_TERMINAL);
    GtkWidget *found = vtes->len ? g_ptr_array_index(vtes, 0) : NULL;
    g_ptr_array_free(vtes, TRUE);
    return found;
}

/* Number of visible lines that hold text starting at a column other than
 * zero, and the worst offender, for the failure message. */
static int count_indented_visible_lines(GtkWidget *vte, char *sample, size_t sample_size, int *worst_column) {
    GtkAdjustment *vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte));
    long top = (long)gtk_adjustment_get_value(vadj);
    long rows = vte_terminal_get_row_count(VTE_TERMINAL(vte));
    long cols = vte_terminal_get_column_count(VTE_TERMINAL(vte));

    int indented = 0;
    *worst_column = 0;
    sample[0] = '\0';

    /* One row at a time: asking for a range spanning rows runs blank rows
     * together, so leading spaces stop corresponding to a column. */
    for (long row = top; row < top + rows; row++) {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        char *line = vte_terminal_get_text_range(VTE_TERMINAL(vte), row, 0, row, cols - 1,
                                                 NULL, NULL, NULL);
        G_GNUC_END_IGNORE_DEPRECATIONS
        if (!line) {
            continue;
        }
        const char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\n') {
            p++;
        }
        if (*p != '\0') {
            int column = 0;
            for (const char *q = line; q < p; q++) {
                if (*q == ' ' || *q == '\t') {
                    column++;
                }
            }
            if (column > 0) {
                indented++;
                if (column > *worst_column) {
                    *worst_column = column;
                    g_strlcpy(sample, p, sample_size);
                }
            }
        }
        g_free(line);
    }
    return indented;
}

static gboolean drive(gpointer data) {
    TestState *test = data;
    test->tick++;

    GList *windows = gtk_window_list_toplevels();
    GtkWidget *window = windows && windows->data ? GTK_WIDGET(windows->data) : NULL;
    g_list_free(windows);
    if (!window || !gtk_widget_get_realized(window)) {
        return G_SOURCE_CONTINUE;
    }
    if (test->tick < SETTLE_TICKS) {
        return G_SOURCE_CONTINUE;
    }

    GtkWidget *vte = find_vte(window);
    GtkWidget *paned = find_hpaned(window);
    if (!vte || !paned) {
        fail(test, "could not find the terminal widget or the horizontal paned");
        return G_SOURCE_REMOVE;
    }

    /* Reproduce the report literally: press Enter for a second prompt
     * line, then start dragging. */
    if (!test->pressed_enter) {
        g_signal_emit_by_name(vte, "commit", "\r", 1);
        test->pressed_enter = TRUE;
        return G_SOURCE_CONTINUE;
    }
    if (test->buffer_rows_before == 0.0) {
        GtkAdjustment *vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte));
        test->buffer_rows_before = gtk_adjustment_get_upper(vadj);
        return G_SOURCE_CONTINUE;
    }

    if (test->sweep_index < test->sweep_len) {
        gtk_paned_set_position(GTK_PANED(paned), test->sweep[test->sweep_index++]);
        return G_SOURCE_CONTINUE;
    }
    if (test->sweep_index < test->sweep_len + TRAILING_TICKS) {
        test->sweep_index++;
        return G_SOURCE_CONTINUE;
    }

    /* Resizing moves no text, so it must not lengthen the buffer. Each
     * intermediate size used to be signalled to the shell separately,
     * walking it through a redraw per step whose leftovers were pushed
     * into the scrollback - seen as blank rows piling up above the
     * prompt. A couple of rows of slack keeps this off a knife edge; the
     * fault added dozens. */
    GtkAdjustment *vadj = gtk_scrollable_get_vadjustment(GTK_SCROLLABLE(vte));
    double grew_by = gtk_adjustment_get_upper(vadj) - test->buffer_rows_before;
    if (grew_by > 2.0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "the terminal buffer grew by %d rows while only being resized - blank rows are "
                 "accumulating above the prompt",
                 (int)grew_by);
        fail(test, msg);
        return G_SOURCE_REMOVE;
    }

    char sample[128];
    int worst_column = 0;
    int indented = count_indented_visible_lines(vte, sample, sizeof(sample), &worst_column);
    if (indented < 0) {
        fail(test, "could not read the terminal's visible contents back");
        return G_SOURCE_REMOVE;
    }
    if (indented > 0) {
        char msg[400];
        snprintf(msg, sizeof(msg),
                 "%d visible terminal line(s) hold text at a non-zero column after resizing "
                 "(worst starts at column %d: \"%.48s\") - the shell's redraw is landing where it "
                 "was not put",
                 indented, worst_column, sample);
        fail(test, msg);
        return G_SOURCE_REMOVE;
    }

    test->done = TRUE;
    gtk_window_close(GTK_WINDOW(window));
    return G_SOURCE_REMOVE;
}

int main(void) {
    g_log_set_always_fatal(G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_ERROR);

    TestState test = {0};
    build_sweep(&test);

    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "terminal_resize_redraw_smoke: app_create failed\n");
        return 1;
    }

    install_close_confirmation_answers();

    g_timeout_add(TICK_MS, drive, &test);
    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "terminal_resize_redraw_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "terminal_resize_redraw_smoke: test did not complete\n");
        return 1;
    }

    g_print("terminal_resize_redraw_smoke: repeated resizing leaves every visible terminal line "
            "starting at column zero\n");
    return 0;
}
