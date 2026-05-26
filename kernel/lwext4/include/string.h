#ifndef LWEXT4_XV6_STRING_H
#define LWEXT4_XV6_STRING_H

#include <stddef.h>

void *memset(void *dst, int c, unsigned int n);
int memcmp(const void *v1, const void *v2, unsigned int n);
void *memmove(void *dst, const void *src, unsigned int n);
void *memcpy(void *dst, const void *src, unsigned int n);
int strncmp(const char *p, const char *q, unsigned int n);
int strcmp(const char *p, const char *q);
char *strncpy(char *s, const char *t, int n);
char *strcpy(char *s, const char *t);
int strlen(const char *s);

#endif
