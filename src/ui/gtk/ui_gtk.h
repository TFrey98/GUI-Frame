#ifndef WORKBENCH_UI_GTK_H
#define WORKBENCH_UI_GTK_H

/* ui_gtk.c implements the platform_ui_* contract declared in
 * ../ui_platform.h - this header exists only for the conventional
 * "every .c file has a matching .h" symmetry every other module in
 * this codebase follows. Nothing outside gtk/ ever includes it:
 * workbench.c only ever sees the platform through ui_platform.h,
 * never GTK-aware. */
#include "ui/ui_platform.h"

#endif /* WORKBENCH_UI_GTK_H */
