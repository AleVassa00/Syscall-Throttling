savedcmd_syscall_monitor.o := ld -m elf_x86_64 -z noexecstack --no-warn-rwx-segments   -r -o syscall_monitor.o @syscall_monitor.mod 
