add_executable(vtsh main.c)
target_link_libraries(vtsh PRIVATE libvtsh)

add_executable(proc-fork proc-fork.c)
target_link_libraries(proc-fork PRIVATE libvtsh)

add_executable(shell shell.c)
target_link_libraries(shell PRIVATE libvtsh)

add_executable(cpu-sort cpu_sort.c)
target_link_libraries(cpu-sort PRIVATE libvtsh)

add_executable(ema-replace-int ema_replace_int.c)
target_link_libraries(ema-replace-int PRIVATE libvtsh)

