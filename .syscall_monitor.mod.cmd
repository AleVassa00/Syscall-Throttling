savedcmd_syscall_monitor.mod := printf '%s\n'   main.o syscall_hook.o probe.o device.o monitor.o registry.o stats.o | awk '!x[$$0]++ { print("./"$$0) }' > syscall_monitor.mod
