#ifndef TOOLBOX_TEST_GTK_UTILS_H
#define TOOLBOX_TEST_GTK_UTILS_H

#include <gtk/gtk.h>

/* Recursively collects every descendant of `widget` (inclusive) whose type
 * matches `type` into `out`. Lets smoke tests locate widgets built by the
 * platform backend without needing test-only accessors on it. */
static inline void collect_by_type(GtkWidget *widget, GPtrArray *out, GType type) {
    if (G_TYPE_CHECK_INSTANCE_TYPE(widget, type)) {
        g_ptr_array_add(out, widget);
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *l = children; l; l = l->next) {
            collect_by_type(GTK_WIDGET(l->data), out, type);
        }
        g_list_free(children);
    }
}

#endif /* TOOLBOX_TEST_GTK_UTILS_H */
