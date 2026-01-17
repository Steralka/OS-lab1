set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(vtsh_main main.c)
target_link_libraries(vtsh_main PRIVATE libvtsh)
target_include_directories(vtsh_main PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(vtsh_main PRIVATE -O0)


add_custom_command(TARGET vtsh_main POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:vtsh_main>
            ${CMAKE_CURRENT_LIST_DIR}/test/shell
)

add_executable(proc_fork proc-fork.c)
set_target_properties(proc_fork PROPERTIES OUTPUT_NAME "proc-fork")
target_include_directories(proc_fork PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(proc_fork PRIVATE -O0)

add_executable(shell_or shell.c)
set_target_properties(shell_or PROPERTIES OUTPUT_NAME "shell-or")
target_include_directories(shell_or PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(shell_or PRIVATE -O0)

add_executable(cpu_sort cpu_sort.c)
set_target_properties(cpu_sort PROPERTIES OUTPUT_NAME "cpu-sort")
target_include_directories(cpu_sort PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(cpu_sort PRIVATE -O0)

add_executable(ema_replace_int ema_replace_int.c)
set_target_properties(ema_replace_int PROPERTIES OUTPUT_NAME "ema-replace-int")
target_include_directories(ema_replace_int PRIVATE ${CMAKE_CURRENT_LIST_DIR})
target_compile_options(ema_replace_int PRIVATE -O0)
