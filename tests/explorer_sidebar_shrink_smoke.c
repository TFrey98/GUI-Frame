/*
 * Regression test for the explorer sidebar imposing a content-driven
 * minimum width.
 *
 * A long filename used to widen the panel permanently: the tree view's
 * minimum width request tracked its widest row, the GTK_POLICY_NEVER
 * scrolled window republished that as its own minimum, and GtkPaned's
 * shrink=FALSE meant the handle could not be dragged back through it. The
 * panel grew, the window's minimum grew with it, and the user could not
 * undo either.
 *
 * What should hold instead: the panel's *natural* width may follow its
 * contents (so it still auto-fits), but its *minimum* must not - that
 * minimum is the toolbar row, a constant, and long names simply clip.
 *
 * The panel still keeps that minimum as a hard floor. Letting the handle
 * past it (GtkPaned shrink=TRUE) does not clip the panel: GTK allocates
 * it its minimum anyway, so it overlaps and draws over the terminal.
 * Hiding the panel entirely is the Sidebar button's job.
 */
#include <dirent.h>
#include <gtk/gtk.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app/app.h"
#include "files/workspace_root.h"
#include "test_gtk_utils.h"
#include "tools/tools_index.h"

#define STEP_INTERVAL_MS 100
#define STEP_TIMEOUT_MS 5000
/* Long enough for the tree view to lay out its rows and for GTK's size
 * request cache to reflect them. */
#define SETTLE_MS 600

/* Long enough that its rendered width dwarfs any sane panel width, so a
 * content-driven minimum is unmistakable rather than marginal. */
#define LONG_NAME \
    "a_really_very_extremely_long_file_name_that_should_never_be_allowed_to" \
    "_set_the_minimum_width_of_the_explorer_panel_0123456789.txt"

typedef struct TestState {
    int elapsed_ms;
    gboolean failed;
    gboolean done;
} TestState;

/* app_run() drives a GtkApplication via g_application_run(), not
 * gtk_main(), so the loop ends when the last toplevel closes - the same
 * way every other GTK smoke test here shuts down. */
static void close_all_toplevels(void) {
    GList *windows = gtk_window_list_toplevels();
    for (GList *l = windows; l; l = l->next) {
        gtk_window_close(GTK_WINDOW(l->data));
    }
    g_list_free(windows);
}

static void fail(TestState *test, const char *msg) {
    fprintf(stderr, "explorer_sidebar_shrink_smoke: %s\n", msg);
    test->failed = TRUE;
    test->done = TRUE;
    close_all_toplevels();
}

static void write_fixture_file(const char *path) {
    FILE *f = fopen(path, "w");
    if (f) {
        fclose(f);
    }
}

/* Same rationale as the other GTK smoke tests: every test binary shares
 * one physical files/ directory, so leftovers from a prior test would
 * otherwise perturb the widths measured here. */
static void clear_directory_absolute(const char *absolute_path) {
    DIR *dir = opendir(absolute_path);
    if (!dir) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[4400];
        snprintf(child, sizeof(child), "%s/%s", absolute_path, entry->d_name);
        struct stat st;
        if (lstat(child, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            clear_directory_absolute(child);
            rmdir(child);
        } else {
            unlink(child);
        }
    }
    closedir(dir);
}

/* The horizontal GtkPaned is the one holding the sidebar as child1; the
 * vertical one splits the workspace from the bottom panel. */
static GtkWidget *find_horizontal_paned(GtkWidget *window) {
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

static gboolean drive(gpointer data) {
    TestState *test = data;
    test->elapsed_ms += STEP_INTERVAL_MS;
    if (test->elapsed_ms > STEP_TIMEOUT_MS) {
        fail(test, "timed out before the window was realized");
        return G_SOURCE_REMOVE;
    }

    GList *windows = gtk_window_list_toplevels();
    GtkWidget *window = windows && windows->data ? GTK_WIDGET(windows->data) : NULL;
    g_list_free(windows);
    if (!window || !gtk_widget_get_realized(window)) {
        return G_SOURCE_CONTINUE;
    }

    /* GTK caches size requests, and the first cache fill happens before
     * the tree view has laid its rows out - measuring at the first
     * realized tick reads values that do not yet reflect the long
     * filename, which silently weakens every assertion below. Let the
     * layout settle, then force a fresh request pass. */
    if (test->elapsed_ms < SETTLE_MS) {
        return G_SOURCE_CONTINUE;
    }
    gtk_widget_queue_resize(window);
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }

    GtkWidget *paned = find_horizontal_paned(window);
    if (!paned) {
        fail(test, "could not find the horizontal GtkPaned holding the sidebar");
        return G_SOURCE_REMOVE;
    }

    GtkWidget *sidebar = gtk_paned_get_child1(GTK_PANED(paned));
    if (!sidebar) {
        fail(test, "the horizontal paned has no child1 (the explorer sidebar)");
        return G_SOURCE_REMOVE;
    }

    /* 1. The long filename must not have become a floor.
     *
     *    The bar is deliberately "well below", not merely "below": when
     *    the scrolled window republishes the tree view's content width as
     *    its own minimum, minimum and natural land within a few pixels of
     *    each other (both ~1000px for this name) and a bare
     *    minimum < natural would still pass. The name planted here renders
     *    several times wider than the panel's real floor - the toolbar
     *    row - so requiring a wide margin distinguishes the two cases
     *    without hard-coding either width. */
    gint minimum = 0, natural = 0;
    gtk_widget_get_preferred_width(sidebar, &minimum, &natural);
    if (minimum * 2 >= natural) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "sidebar minimum width (%d) is not well below its natural width (%d) - the panel's "
                 "minimum is still tracking the filename on screen",
                 minimum, natural);
        fail(test, msg);
        return G_SOURCE_REMOVE;
    }

    /* 2. The panel must keep a hard floor. shrink=TRUE does not clip a
     *    child that is dragged past its minimum - GTK still allocates it
     *    the minimum, so it overlaps its neighbour and draws over the
     *    terminal. The explorer's toolbar and tree have to stay inside
     *    the panel; hiding it altogether is the Sidebar button's job. */
    gboolean shrink = TRUE;
    gtk_container_child_get(GTK_CONTAINER(paned), sidebar, "shrink", &shrink, NULL);
    if (shrink) {
        fail(test, "the sidebar's GtkPaned shrink child property is TRUE - the handle can be dragged "
                   "past the panel's minimum, which overlaps the panel onto the terminal instead of "
                   "clipping it");
        return G_SOURCE_REMOVE;
    }

    /* 3. The observable half of that floor: dragging the handle to
     *    nothing must come to rest at the panel's minimum, not at zero.
     *
     *    GtkPaned only clamps the position during size-allocate, so
     *    reading it straight back after set_position() returns the raw
     *    request. gtk_container_check_resize() forces that allocation
     *    synchronously; waiting for one to happen on its own is what made
     *    an earlier version of this test intermittent. */
    gtk_paned_set_position(GTK_PANED(paned), 0);
    gtk_container_check_resize(GTK_CONTAINER(paned));
    while (gtk_events_pending()) {
        gtk_main_iteration();
    }
    gint position = gtk_paned_get_position(GTK_PANED(paned));
    if (position < minimum) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "paned position settled at %d, below the panel's %dpx minimum - the explorer's "
                 "contents are being pushed outside the panel",
                 position, minimum);
        fail(test, msg);
        return G_SOURCE_REMOVE;
    }

    /* 4. And the window itself must not have inherited a wide floor. With
     *    a long name on screen the window's natural width grows to
     *    accommodate it while its minimum stays put, so the two must
     *    differ. (With only short names present they are legitimately
     *    equal, which is why this test always plants a long one.) */
    gint window_minimum = 0, window_natural = 0;
    gtk_widget_get_preferred_width(window, &window_minimum, &window_natural);
    if (window_minimum >= window_natural) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "window minimum width (%d) is not below its natural width (%d) - the filename is "
                 "feeding the window's minimum size, not just its preferred size",
                 window_minimum, window_natural);
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
    App *app = app_create(0, NULL);
    if (!app) {
        fprintf(stderr, "explorer_sidebar_shrink_smoke: app_create failed\n");
        return 1;
    }

    const WorkspaceRoot *ws = app_get_file_workspace_root(app);
    clear_directory_absolute(ws->canonical_path);
    clear_directory_absolute(tools_index_dir());

    char long_file[4400];
    snprintf(long_file, sizeof(long_file), "%s/%s", ws->canonical_path, LONG_NAME);
    write_fixture_file(long_file);

    g_timeout_add(STEP_INTERVAL_MS, drive, &test);

    int status = app_run(app);
    app_destroy(app);

    if (test.failed) {
        return 1;
    }
    if (status != 0) {
        fprintf(stderr, "explorer_sidebar_shrink_smoke: app exited with status %d\n", status);
        return 1;
    }
    if (!test.done) {
        fprintf(stderr, "explorer_sidebar_shrink_smoke: test did not complete\n");
        return 1;
    }

    g_print("explorer_sidebar_shrink_smoke: a long filename drives the sidebar's natural width but "
            "not its minimum, and the panel keeps a hard floor at that minimum\n");
    return 0;
}
