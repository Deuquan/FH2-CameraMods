#ifndef FH2_EXTRA_OPTIONS_H
#define FH2_EXTRA_OPTIONS_H
/* Plugin build: minimal replacement for the Extra Options host header. */
#define WIN32_LEAN_AND_MEAN
#include <stddef.h>
#include <stdint.h>
#include <windows.h>
#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))
#define PATH_CAPACITY 32768
#endif
