#ifndef PROBE_H
#define PROBE_H

int resolve_kallsyms_lookup_name(void);
unsigned long get_symbol_addr(const char *name);


#endif