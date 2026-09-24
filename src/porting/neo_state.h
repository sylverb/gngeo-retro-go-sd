#ifndef NEO_STATE_H
#define NEO_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Retro-Go path-based savestate (firmware owns /data/...-N.sav). */
int neo_save_state_path(const char *path);
int neo_load_state_path(const char *path);

#ifdef __cplusplus
}
#endif

#endif
