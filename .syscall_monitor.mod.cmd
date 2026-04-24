savedcmd_syscall_monitor.mod := printf '%s\n'   main.o registry.o device.o monitor.o syscall_hook.o | awk '!x[$$0]++ { print("./"$$0) }' > syscall_monitor.mod
