/* Host stand-in for the kernel's string.h (unit tests only). */
#ifndef SO_TEST_STRING_H
#define SO_TEST_STRING_H
#include_next <string.h>
size_t so_test_strlcpy(char *dest, const char *src, size_t size);
#define strlcpy so_test_strlcpy
#endif
