#pragma once

unsigned long resolve_kallsyms_lookup_name(void);
unsigned long (*get_kallsyms_lookup_name_fn(void))(const char *name);