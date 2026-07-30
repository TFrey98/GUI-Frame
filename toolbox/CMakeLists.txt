cmake_minimum_required(VERSION 3.16)
project(toolbox C)

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

add_library(toolbox_core STATIC
    src/app/app.c
    src/core/workspace.c
    src/core/tab.c
    src/core/terminal_session.c
    src/ui/workbench.c
    src/db/database.c
    src/tools/tool_registry.c
    src/tools/toolkit_index.c
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
    src/platform/linux/ui_gtk.c
    src/platform/linux/terminal_vte.c
)

target_include_directories(toolbox_core PUBLIC
    src
    src/app
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

target_compile_options(toolbox_core PUBLIC
    ${GTK_CFLAGS_OTHER}
    ${VTE_CFLAGS_OTHER}
    -Wall
    -Wextra
)

target_link_libraries(toolbox_core PUBLIC
    ${GTK_LIBRARIES}
    ${VTE_LIBRARIES}
    ${SQLITE3_LIBRARIES}
    ${OPENSSL_LIBRARIES}
    Threads::Threads
)

add_executable(toolbox src/main.c)
target_link_libraries(toolbox PRIVATE toolbox_core)

add_custom_command(TARGET toolbox POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:toolbox>/toolkit"
    COMMENT "Ensuring toolkit/ exists next to the toolbox executable"
)

enable_testing()
add_subdirectory(tests)
