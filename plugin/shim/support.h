#ifndef FH2_SUPPORT_H
#define FH2_SUPPORT_H
#include "extra_options.h"
void memory_copy(void *destination, const void *source, size_t size);
int memory_read(const void *address, void *output, size_t size);
int memory_read_pointer(const void *address, void **value);
int float_is_finite(float value);
int memory_is_writable(const void *address, size_t size);
#endif
