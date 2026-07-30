#ifndef TOOLBOX_TERMINAL_VTE_H
#define TOOLBOX_TERMINAL_VTE_H

#include <gtk/gtk.h>

#include "terminal/terminal.h"

/*
 * Linux/VTE-only accessor for embedding a Terminal's widget in the UI.
 * VteTerminal itself never leaves this backend - callers elsewhere only
 * ever see a plain GtkWidget.
 */
GtkWidget *terminal_get_widget(Terminal *terminal);

#endif /* TOOLBOX_TERMINAL_VTE_H */
