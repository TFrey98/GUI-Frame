#ifndef WORKBENCH_TERMINAL_VTE_H
#define WORKBENCH_TERMINAL_VTE_H

#include <gtk/gtk.h>

#include "terminal/terminal.h"

/*
 * Linux/VTE-only accessor for embedding a Terminal's widget in the UI.
 * VteTerminal itself never leaves this backend - callers elsewhere only
 * ever see a plain GtkWidget.
 */
GtkWidget *terminal_get_widget(Terminal *terminal);

/* Applies (dark) or clears (!dark) VTE's own foreground/background/ANSI
 * palette - GTK CSS alone can't reach into VteTerminal's cell colors, so
 * dark mode needs this separate path alongside the app-wide CSS
 * provider. !dark resets to VTE's built-in default colors. */
void terminal_apply_theme(Terminal *terminal, gboolean dark);

#endif /* WORKBENCH_TERMINAL_VTE_H */
