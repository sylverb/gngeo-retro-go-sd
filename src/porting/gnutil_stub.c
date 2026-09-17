#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "gnutil.h"
#include "config.h"

char gnerror[GNERROR_SIZE];

char *get_gngeo_dir(void) { return ""; }
void chomp(char *str)
{
    size_t n;
    if (!str) return;
    n = strlen(str);
    while (n && (str[n - 1] == '\n' || str[n - 1] == '\r'))
        str[--n] = 0;
}
char *my_fgets(char *s, int size, FILE *stream) { return fgets(s, size, stream); }
char *file_basename(char *filename)
{
    char *p = strrchr(filename, '/');
    return p ? p + 1 : filename;
}
int check_dir(char *dir_name) { (void)dir_name; return GN_TRUE; }
void gn_set_error_msg(char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(gnerror, GNERROR_SIZE, fmt, ap);
    va_end(ap);
    printf("%s", gnerror);
}
void gn_strncat_dir(char *basedir, char *dir, size_t n)
{
    (void)basedir; (void)dir; (void)n;
}
void gn_init_pbar(const char *title, int max) { (void)title; (void)max; }
void gn_update_pbar(int v) { (void)v; }
void gn_terminate_pbar(void) {}
