/* Precomputed YM2610 tl_tab / sin_tab (see ym2610_tables.c). */
#ifndef YM2610_TABLES_H
#define YM2610_TABLES_H

#define YM2610_TL_TAB_LEN  (6656)
#define YM2610_SIN_LEN     (1024)

extern const signed int ym2610_tl_tab[YM2610_TL_TAB_LEN];
extern const unsigned int ym2610_sin_tab[YM2610_SIN_LEN];

#endif
