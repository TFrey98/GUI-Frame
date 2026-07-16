add_executable(lifecycle_smoke lifecycle_smoke.c)
target_link_libraries(lifecycle_smoke PRIVATE toolbox_core)

add_executable(layout_smoke layout_smoke.c)
target_link_libraries(layout_smoke PRIVATE toolbox_core)

add_executable(tabs_smoke tabs_smoke.c)
target_link_libraries(tabs_smoke PRIVATE toolbox_core)

add_executable(terminal_smoke terminal_smoke.c)
target_link_libraries(terminal_smoke PRIVATE toolbox_core)

add_executable(multi_terminal_smoke multi_terminal_smoke.c)
target_link_libraries(multi_terminal_smoke PRIVATE toolbox_core)

add_executable(terminal_session_test terminal_session_test.c)
target_link_libraries(terminal_session_test PRIVATE toolbox_core)

add_executable(toolkit_index_test toolkit_index_test.c)
target_link_libraries(toolkit_index_test PRIVATE toolbox_core)

add_test(NAME lifecycle_smoke COMMAND lifecycle_smoke)
set_tests_properties(lifecycle_smoke PROPERTIES TIMEOUT 30)

add_test(NAME layout_smoke COMMAND layout_smoke)
set_tests_properties(layout_smoke PROPERTIES TIMEOUT 30)

add_test(NAME tabs_smoke COMMAND tabs_smoke)
set_tests_properties(tabs_smoke PROPERTIES TIMEOUT 30)

add_test(NAME terminal_smoke COMMAND terminal_smoke)
set_tests_properties(terminal_smoke PROPERTIES TIMEOUT 30)

add_test(NAME multi_terminal_smoke COMMAND multi_terminal_smoke)
set_tests_properties(multi_terminal_smoke PROPERTIES TIMEOUT 30)

add_test(NAME terminal_session_test COMMAND terminal_session_test)
set_tests_properties(terminal_session_test PROPERTIES TIMEOUT 10)

add_test(NAME toolkit_index_test COMMAND toolkit_index_test)
set_tests_properties(toolkit_index_test PROPERTIES TIMEOUT 10)
