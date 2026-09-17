/* Minimal conf backend — fixed defaults, no filesystem. */
#include <string.h>
#include <stdlib.h>
#include "conf.h"
#include "emu.h"
#include "config.h"

CONFIG conf;

static CONF_ITEM items[16];
static int nitems;

static CONF_ITEM *add_item(const char *name, CF_TYPE type)
{
    CONF_ITEM *it;
    if (nitems >= (int)(sizeof(items) / sizeof(items[0])))
        return NULL;
    it = &items[nitems++];
    memset(it, 0, sizeof(*it));
    it->name = (char *)name;
    it->type = type;
    return it;
}

void cf_create_bool_item(const char *name, const char *help, char short_opt, int def)
{
    CONF_ITEM *it = add_item(name, CFT_BOOLEAN);
    (void)help; (void)short_opt;
    if (it) {
        it->data.dt_bool.boolean = def;
        it->data.dt_bool.default_bool = def;
    }
}

void cf_create_int_item(const char *name, const char *help, const char *hlp_arg,
                        char short_opt, int def)
{
    CONF_ITEM *it = add_item(name, CFT_INT);
    (void)help; (void)hlp_arg; (void)short_opt;
    if (it) {
        it->data.dt_int.val = def;
        it->data.dt_int.default_val = def;
    }
}

void cf_create_string_item(const char *name, const char *help, const char *hlp_arg,
                           char short_opt, const char *def)
{
    CONF_ITEM *it = add_item(name, CFT_STRING);
    (void)help; (void)hlp_arg; (void)short_opt;
    if (it) {
        strncpy(it->data.dt_str.str, def ? def : "", CF_MAXSTRLEN - 1);
        it->data.dt_str.default_str = (char *)(def ? def : "");
    }
}

void cf_create_action_item(const char *name, const char *help, char short_opt,
                           int (*action)(struct CONF_ITEM *self))
{
    (void)name; (void)help; (void)short_opt; (void)action;
}

void cf_create_action_arg_item(const char *name, const char *help, const char *hlp_arg,
                               char short_opt, int (*action)(struct CONF_ITEM *self))
{
    (void)name; (void)help; (void)hlp_arg; (void)short_opt; (void)action;
}

void cf_create_array_item(const char *name, const char *help, const char *hlp_arg,
                          char short_opt, int size, int *def)
{
    (void)name; (void)help; (void)hlp_arg; (void)short_opt; (void)size; (void)def;
}

void cf_create_str_array_item(const char *name, const char *help, const char *hlp_arg,
                              char short_opt, char *def)
{
    (void)name; (void)help; (void)hlp_arg; (void)short_opt; (void)def;
}

CONF_ITEM *cf_get_item_by_name(const char *name)
{
    int i;
    for (i = 0; i < nitems; i++) {
        if (strcmp(items[i].name, name) == 0)
            return &items[i];
    }
    /* Lazy-create common keys with safe defaults */
    if (strcmp(name, "68kclock") == 0) {
        cf_create_int_item(name, "", "", 0, 0);
        return cf_get_item_by_name(name);
    }
    if (strcmp(name, "z80clock") == 0) {
        cf_create_int_item(name, "", "", 0, 0);
        return cf_get_item_by_name(name);
    }
    if (strcmp(name, "dump") == 0) {
        /* dump=true skips in-place ROM byteswap — required for flash XIP */
        cf_create_bool_item(name, "", 0, 1);
        return cf_get_item_by_name(name);
    }
    if (strcmp(name, "samplerate") == 0) {
        cf_create_int_item(name, "", "", 0, 18000);
        return cf_get_item_by_name(name);
    }
    if (strcmp(name, "autoframeskip") == 0 || strcmp(name, "bench") == 0 ||
        strcmp(name, "showfps") == 0 || strcmp(name, "sleepidle") == 0) {
        cf_create_bool_item(name, "", 0, 0);
        return cf_get_item_by_name(name);
    }
    if (strcmp(name, "sound") == 0 || strcmp(name, "screen320") == 0) {
        cf_create_bool_item(name, "", 0, 1);
        return cf_get_item_by_name(name);
    }
    if (strcmp(name, "raster") == 0 || strcmp(name, "debug") == 0 ||
        strcmp(name, "vsync") == 0 || strcmp(name, "pal") == 0 ||
        strcmp(name, "showfps") == 0 || strcmp(name, "sleepidle") == 0 ||
        strcmp(name, "940sync") == 0) {
        cf_create_bool_item(name, "", 0, 0);
        return cf_get_item_by_name(name);
    }
    if (strcmp(name, "country") == 0) {
        cf_create_string_item(name, "", "", 0, "usa");
        return cf_get_item_by_name(name);
    }
    if (strcmp(name, "system") == 0) {
        cf_create_string_item(name, "", "", 0, "unibios");
        return cf_get_item_by_name(name);
    }
    if (strcmp(name, "rompath") == 0 || strcmp(name, "libglpath") == 0 ||
        strcmp(name, "datafile") == 0 || strcmp(name, "blitter") == 0 ||
        strcmp(name, "effect") == 0 || strcmp(name, "transpack") == 0) {
        cf_create_string_item(name, "", "", 0, "");
        return cf_get_item_by_name(name);
    }
    cf_create_string_item(name, "", "", 0, "");
    return cf_get_item_by_name(name);
}

void cf_init(void)
{
    nitems = 0;
    memset(&conf, 0, sizeof(conf));
    conf.sound = 1;
    conf.sample_rate = 18000;
    /* Device/host pacing is common_emu_frame_loop — busy-wait autoframeskip hangs
     * when SDL_GetTicks is a stub (returns 0). */
    conf.autoframeskip = 0;
    conf.screen320 = 1;
    conf.system = SYS_UNIBIOS;
    conf.country = CTY_USA;
    conf.res_x = 304;
    conf.res_y = 224;
}

void cf_reset_to_default(void) {}
void cf_item_has_been_changed(CONF_ITEM *item) { (void)item; }
void cf_print_help(void) {}
void cf_init_cmd_line(void) {}
int cf_get_non_opt_index(int argc, char *argv[]) { (void)argc; (void)argv; return 1; }
char *cf_parse_cmd_line(int argc, char *argv[]) { (void)argc; (void)argv; return NULL; }
int cf_save_option(char *filename, char *optname, int flags)
{
    (void)filename; (void)optname; (void)flags; return GN_TRUE;
}
int cf_save_file(char *filename, int flags) { (void)filename; (void)flags; return GN_TRUE; }
int cf_open_file(char *filename) { (void)filename; return GN_TRUE; }

void cf_update_from_items(void)
{
    conf.sound = CF_BOOL(cf_get_item_by_name("sound"));
    conf.sample_rate = (Uint16)CF_VAL(cf_get_item_by_name("samplerate"));
    conf.raster = CF_BOOL(cf_get_item_by_name("raster"));
    conf.debug = CF_BOOL(cf_get_item_by_name("debug"));
    conf.autoframeskip = CF_BOOL(cf_get_item_by_name("autoframeskip"));
    conf.screen320 = CF_BOOL(cf_get_item_by_name("screen320"));
}
