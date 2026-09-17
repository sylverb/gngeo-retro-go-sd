#ifndef _EFFECT_H_
#define _EFFECT_H_

#include "SDL.h"
#include "list.h"

typedef struct {
    char *name;
    char *desc;
    void (*init)(void);
    void (*update)(void);
} effect_info;

static inline Uint8 get_effect_by_name(char *name) { (void)name; return 0; }
static inline void print_effect_list(void) {}
static inline LIST *create_effect_list(void) { return NULL; }

#endif
