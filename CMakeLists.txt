cmake_minimum_required(VERSION 3.16)
# Both are cache variables so package.sh can override them with -D without
# editing this file. The suffix is appended to the package version as
# "0.1.0~beta", which sorts *before* the final "0.1.0" under dpkg's version
# ordering - so a tester's beta is correctly upgraded by the real release.
set(WORKBENCH_VERSION "0.1.0" CACHE STRING "Release version, MAJOR.MINOR.PATCH")
set(WORKBENCH_VERSION_SUFFIX "beta" CACHE STRING "Pre-release suffix (e.g. beta, rc1); empty for a final release")

project(workbench VERSION ${WORKBENCH_VERSION} LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK REQUIRED gtk+-3.0)
pkg_check_modules(VTE REQUIRED vte-2.91)
pkg_check_modules(SQLITE3 REQUIRED sqlite3)
pkg_check_modules(OPENSSL REQUIRED openssl)

set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

add_library(workbench_core STATIC
    src/app/app.c
    src/files/workspace_root.c
    src/files/file_tree.c
    src/files/file_operations.c
    src/files/file_classify.c
    src/files/editor_document.c
    src/files/file_watch_queue.c
    src/files/file_watcher.c
    src/files/file_search.c
    src/core/workspace.c
    src/core/app_paths.c
    src/core/tab.c
    src/core/terminal_session.c
    src/core/line_accumulator.c
    src/core/ansi_strip.c
    src/ui/workbench.c
    src/db/database.c
    src/tools/tool_registry.c
    src/tools/tools_index.c
    src/tools/json_parser.c
    src/tools/tool_panel_manifest.c
    src/listeners/event_queue.c
    src/listeners/byte_buffer.c
    src/listeners/managed_object.c
    src/listeners/object_predicates.c
    src/listeners/object_registry.c
    src/listeners/listener_manager.c
    src/listeners/tcp_worker.c
    src/listeners/http_worker.c
    src/listeners/https_worker.c
    src/listeners/terminal_history.c
    src/listeners/connection_worker.c
    src/listeners/connection_manager.c
    src/listeners/listener_system.c
    src/ui/gtk/ui_gtk.c
    src/ui/gtk/ui_gtk_window.c
    src/ui/gtk/ui_gtk_tabs.c
    src/ui/gtk/ui_gtk_tab_labels.c
    src/ui/gtk/ui_gtk_tab_close.c
    src/ui/gtk/ui_gtk_listener_page.c
    src/ui/gtk/ui_gtk_terminal.c
    src/ui/gtk/ui_gtk_connection_terminal.c
    src/ui/gtk/ui_gtk_file_tree.c
    src/ui/gtk/ui_gtk_explorer_sidebar.c
    src/ui/gtk/ui_gtk_file_watch.c
    src/ui/gtk/ui_gtk_explorer_transfer.c
    src/ui/gtk/ui_gtk_explorer_navigation.c
    src/ui/gtk/ui_gtk_object_list.c
    src/ui/gtk/ui_gtk_dialogs.c
    src/ui/gtk/ui_gtk_explorer_dialogs.c
    src/ui/gtk/ui_gtk_listener_dialog.c
    src/ui/gtk/ui_gtk_run_dialog.c
    src/ui/gtk/ui_gtk_object_menu.c
    src/ui/gtk/ui_gtk_explorer_menu.c
    src/ui/gtk/ui_gtk_editor.c
    src/ui/gtk/ui_gtk_editor_tabs.c
    src/ui/gtk/ui_gtk_editor_conflicts.c
    src/ui/gtk/ui_gtk_theme.c
    src/ui/gtk/ui_gtk_search.c
    src/ui/gtk/ui_gtk_database_dialogs.c
    src/ui/gtk/ui_gtk_tool_panel.c
    src/ui/gtk/terminal_vte.c
    src/terminal/pty_worker.c
)

target_include_directories(workbench_core PUBLIC
    src
    src/app
    src/files
    src/core
    src/ui
    src/terminal
    src/db
    src/tools
    src/listeners
    ${GTK_INCLUDE_DIRS}
    ${VTE_INCLUDE_DIRS}
    ${SQLITE3_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIRS}
)

target_compile_options(workbench_core PUBLIC
    ${GTK_CFLAGS_OTHER}
    ${VTE_CFLAGS_OTHER}
    -Wall
    -Wextra
)

target_link_libraries(workbench_core PUBLIC
    ${GTK_LIBRARIES}
    ${VTE_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    Threads::Threads
)

add_executable(workbench src/main.c)
target_link_libraries(workbench PRIVATE workbench_core)

add_custom_command(TARGET workbench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:workbench>/tools"
    COMMENT "Ensuring tools/ exists next to the workbench executable"
)

add_custom_command(TARGET workbench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:workbench>/files"
    COMMENT "Ensuring files/ (the file workspace root) exists next to the workbench executable"
)

option(WORKBENCH_BUILD_TESTS "Build the unit/smoke test suite" ON)

if(WORKBENCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Installation
#
# An installed binary lives in a read-only /usr/bin, so it does NOT get the
# files/ and tools/ directories the POST_BUILD steps above create next to a
# development build. app_paths.c detects that and puts both (plus workbench.db)
# under ~/.local/share/workbench instead, created on first launch.
# ---------------------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS workbench RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES packaging/dev.workbench.app.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)

install(FILES packaging/icons/dev.workbench.app.svg
        DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps)

# PNGs alongside the SVG so the icon still shows up on a desktop whose
# gdk-pixbuf has no SVG loader installed.
foreach(icon_size 16 24 32 48 64 128 256)
    install(FILES packaging/icons/dev.workbench.app-${icon_size}.png
            DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/${icon_size}x${icon_size}/apps
            RENAME dev.workbench.app.png)
endforeach()

# ---------------------------------------------------------------------------
# Packaging (CPack -> .deb)
# ---------------------------------------------------------------------------

set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME "workbench")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
if(WORKBENCH_VERSION_SUFFIX)
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}~${WORKBENCH_VERSION_SUFFIX}")
endif()
set(CPACK_PACKAGE_VENDOR "TFrey98")
set(CPACK_PACKAGE_CONTACT "TFrey98 <taylor_frey@icloud.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "GTK workbench for network listeners and local terminals")
set(CPACK_PACKAGE_DESCRIPTION
    "A GTK3 desktop workbench for running and observing network listeners and\n\
local terminals side by side: a tabbed workspace with embedded shell\n\
terminals, reverse-TCP/HTTP/HTTPS listeners with live per-connection\n\
terminal views, a sandboxed file explorer and text editor, and a local\n\
SQLite database that captures terminal activity for later export.")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/TFrey98/workbench")
set(CPACK_STRIP_FILES ON)

set(CPACK_DEBIAN_PACKAGE_SECTION "utils")
set(CPACK_DEBIAN_PACKAGE_PRIORITY "optional")
set(CPACK_DEBIAN_FILE_NAME "DEB-DEFAULT")

# dpkg-shlibdeps reads the linked binary and writes the exact runtime
# packages (libgtk-3-0, libvte-2.91-0, libsqlite3-0, libssl3, ...) with
# correct minimum versions into Depends, so apt resolves them on install.
# Hand-maintaining that list would drift every time a dependency is added.
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# Refresh the icon cache and desktop database after install/removal, so the
# launcher entry appears without the tester logging out.
set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
    "${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postinst;${CMAKE_CURRENT_SOURCE_DIR}/packaging/debian/postrm")

include(CPack)
