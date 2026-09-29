/*
 * Decrypt encrypted Neo Geo MAME sets to plain .zip for neo_zip_flash.
 *
 *   make decrypt-zip
 *   ./tools/decrypt_neogeo_zip <game.zip> [-o out.zip]
 *   ./tools/decrypt_neogeo_zip --list
 *
 * Needs tools/data/cmc42.xor + cmc50.xor (repo). Peak RAM ≈ 2×C + P + V.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <strings.h>
#include <ftw.h>

#include "SDL.h"
#include "roms.h"
#include "mame_layer.h"
#include "miniz.h"

void kof98_decrypt_68k(GAME_ROMS *r);
void kof99_decrypt_68k(GAME_ROMS *r);
void garou_decrypt_68k(GAME_ROMS *r);
void mslug3_decrypt_68k(GAME_ROMS *r);
void kof2000_decrypt_68k(GAME_ROMS *r);
void kof2002_decrypt_68k(GAME_ROMS *r);
void matrim_decrypt_68k(GAME_ROMS *r);
void samsho5_decrypt_68k(GAME_ROMS *r);
void samsh5sp_decrypt_68k(GAME_ROMS *r);
void mslug5_decrypt_68k(GAME_ROMS *r);
void svc_px_decrypt(GAME_ROMS *r);
void kof2003_decrypt_68k(GAME_ROMS *r);
void neo_pcm2_swap(GAME_ROMS *r, int value);
void neo_pcm2_snk_1999(GAME_ROMS *r, int value);
void neogeo_cmc50_m1_decrypt(GAME_ROMS *r);
void kof99_neogeo_gfx_decrypt(GAME_ROMS *r, int extra_xor);
void kof2000_neogeo_gfx_decrypt(GAME_ROMS *r, int extra_xor);

void gn_init_pbar(const char *a, int b)
{
	(void)b;
	if (a && a[0])
		printf("  %s\n", a);
}
void gn_update_pbar(int a) { (void)a; }
void gn_terminate_pbar(void) {}

/* ---- XOR tables ---------------------------------------------------------- */

static uint8_t xor_scratch[0xB00];

void *res_load_data(char *name)
{
	FILE *f = NULL;
	void *p;
	const char *path = NULL;
	const char *cands42[] = {
		"tools/data/cmc42.xor", "./tools/data/cmc42.xor", NULL
	};
	const char *cands50[] = {
		"tools/data/cmc50.xor", "./tools/data/cmc50.xor", NULL
	};
	const char **cands;
	int i;

	if (!name)
		return NULL;
	if (strcmp(name, "rom/cmc42.xor") == 0)
		cands = cands42;
	else if (strcmp(name, "rom/cmc50.xor") == 0)
		cands = cands50;
	else {
		fprintf(stderr, "res_load_data: unknown %s\n", name);
		return NULL;
	}
	for (i = 0; cands[i]; i++) {
		f = fopen(cands[i], "rb");
		if (f) {
			path = cands[i];
			break;
		}
	}
	if (!f) {
		fprintf(stderr, "missing %s (run from repo root)\n", name);
		return NULL;
	}
	if (fread(xor_scratch, 1, 0xB00, f) != 0xB00) {
		fclose(f);
		fprintf(stderr, "%s short read\n", path);
		return NULL;
	}
	fclose(f);
	p = malloc(0xB00);
	if (!p)
		return NULL;
	memcpy(p, xor_scratch, 0xB00);
	printf("  loaded %s\n", path);
	return p;
}

/* ---- source zip/dir ------------------------------------------------------ */

typedef struct {
	mz_zip_archive zip;
	int is_zip;
	char dir[512];
} src_t;

typedef struct {
	char name[256];
	size_t size;
	mz_uint zip_idx;
} member_t;

static int path_is_dir(const char *path)
{
	struct stat st;
	return (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
}

static int src_open(src_t *s, const char *path)
{
	memset(s, 0, sizeof(*s));
	if (path_is_dir(path)) {
		snprintf(s->dir, sizeof(s->dir), "%s", path);
		s->is_zip = 0;
		return 1;
	}
	memset(&s->zip, 0, sizeof(s->zip));
	if (!mz_zip_reader_init_file(&s->zip, path, 0)) {
		fprintf(stderr, "open %s: %s\n", path,
			mz_zip_get_error_string(mz_zip_get_last_error(&s->zip)));
		return 0;
	}
	s->is_zip = 1;
	return 1;
}

static void src_close(src_t *s)
{
	if (s->is_zip)
		mz_zip_reader_end(&s->zip);
}

static const char *basename_of(const char *path)
{
	const char *b = strrchr(path, '/');
	return b ? b + 1 : path;
}

static int zip_find_basename(mz_zip_archive *z, const char *base, mz_uint *out_idx)
{
	mz_uint n = mz_zip_reader_get_num_files(z);
	mz_uint i;
	for (i = 0; i < n; i++) {
		char name[256];
		if (mz_zip_reader_is_file_a_directory(z, i))
			continue;
		mz_zip_reader_get_filename(z, i, name, sizeof(name));
		if (strcasecmp(basename_of(name), base) == 0) {
			*out_idx = i;
			return 1;
		}
	}
	return 0;
}

static int src_has(src_t *s, const char *base)
{
	if (s->is_zip) {
		mz_uint idx;
		return zip_find_basename(&s->zip, base, &idx);
	}
	{
		char path[768];
		struct stat st;
		snprintf(path, sizeof(path), "%s/%s", s->dir, base);
		return (stat(path, &st) == 0);
	}
}

static uint8_t *load_named(src_t *s, const char *basename, size_t expect)
{
	if (s->is_zip) {
		mz_uint idx;
		size_t got = 0;
		void *p;
		if (!zip_find_basename(&s->zip, basename, &idx)) {
			fprintf(stderr, "zip missing %s\n", basename);
			return NULL;
		}
		p = mz_zip_reader_extract_to_heap(&s->zip, idx, &got, 0);
		if (!p || (expect && got != expect)) {
			fprintf(stderr, "%s: got %zu want %zu\n", basename, got, expect);
			free(p);
			return NULL;
		}
		return (uint8_t *)p;
	}
	{
		char path[768];
		FILE *f;
		uint8_t *buf;
		size_t n;
		snprintf(path, sizeof(path), "%s/%s", s->dir, basename);
		f = fopen(path, "rb");
		if (!f) {
			fprintf(stderr, "open %s: %s\n", path, strerror(errno));
			return NULL;
		}
		if (!expect) {
			fseek(f, 0, SEEK_END);
			expect = (size_t)ftell(f);
			fseek(f, 0, SEEK_SET);
		}
		buf = malloc(expect);
		if (!buf) {
			fclose(f);
			return NULL;
		}
		n = fread(buf, 1, expect, f);
		fclose(f);
		if (n != expect) {
			fprintf(stderr, "%s: got %zu want %zu\n", basename, n, expect);
			free(buf);
			return NULL;
		}
		return buf;
	}
}

/* Classify Neo Geo ROM member: p/c/v/m/s, or 0 if unknown. */
static char member_kind(const char *b)
{
	const char *dot = strrchr(b, '.');
	const char *ext;
	if (strcasestr(b, "neo-sma"))
		return 'p';
	if (!dot || !dot[1])
		return 0;
	ext = dot + 1;
	if (ext[0] == 's' && ext[1] == 'p' && isdigit((unsigned char)ext[2]))
		return 'p'; /* .sp2 */
	if (ext[0] == 'p' && (ext[1] == '\0' || isdigit((unsigned char)ext[1])))
		return 'p';
	if (ext[0] == 'c' && isdigit((unsigned char)ext[1]))
		return 'c';
	if (ext[0] == 'v' && isdigit((unsigned char)ext[1]))
		return 'v';
	if (ext[0] == 'm' && isdigit((unsigned char)ext[1]))
		return 'm';
	if (ext[0] == 's' && isdigit((unsigned char)ext[1]))
		return 's';
	return 0;
}

/* List members; optional filter: 'p','c','v','m','s', or 0 = all. */
static int list_members(src_t *s, member_t *out, int max, char kind)
{
	int n = 0;
	if (!s->is_zip)
		return 0;
	{
		mz_uint i, total = mz_zip_reader_get_num_files(&s->zip);
		for (i = 0; i < total && n < max; i++) {
			char name[256];
			const char *b;
			char k;
			if (mz_zip_reader_is_file_a_directory(&s->zip, i))
				continue;
			mz_zip_archive_file_stat st;
			mz_zip_reader_get_filename(&s->zip, i, name, sizeof(name));
			b = basename_of(name);
			k = member_kind(b);
			if (!k || (kind && k != kind))
				continue;
			if (!mz_zip_reader_file_stat(&s->zip, i, &st))
				continue;
			snprintf(out[n].name, sizeof(out[n].name), "%s", b);
			out[n].size = (size_t)st.m_uncomp_size;
			out[n].zip_idx = i;
			n++;
		}
	}
	return n;
}

static int member_num_suffix(const char *name)
{
	/* …c1 / …p2 / …v3 / …sp2 → trailing digit before end or after last letter */
	const char *dot = strrchr(name, '.');
	const char *p = dot ? dot + 1 : name;
	int v = 0;
	while (*p && !isdigit((unsigned char)*p))
		p++;
	while (*p && isdigit((unsigned char)*p))
		v = v * 10 + (*p++ - '0');
	return v;
}

static int cmp_member_num(const void *a, const void *b)
{
	const member_t *ma = a, *mb = b;
	int na = member_num_suffix(ma->name);
	int nb = member_num_suffix(mb->name);
	if (na != nb)
		return na - nb;
	return strcasecmp(ma->name, mb->name);
}

/* ---- P load helpers ------------------------------------------------------ */

static void bytes_place(uint8_t *dst, size_t dst_sz, size_t off,
			const uint8_t *src, size_t n)
{
	if (off + n > dst_sz) {
		fprintf(stderr, "P overflow off=%zx n=%zx sz=%zx\n", off, n, dst_sz);
		abort();
	}
	memcpy(dst + off, src, n);
}

static uint8_t *load_p_concat(src_t *s, size_t p_size,
			      const char **names, const size_t *offs,
			      const size_t *fsizes, int nfiles)
{
	uint8_t *out = calloc(1, p_size);
	int i;
	if (!out)
		return NULL;
	for (i = 0; i < nfiles; i++) {
		uint8_t *buf = load_named(s, names[i], fsizes[i]);
		if (!buf) {
			free(out);
			return NULL;
		}
		bytes_place(out, p_size, offs[i], buf, fsizes[i]);
		free(buf);
		printf("  P + %s @ %07zx (%zu)\n", names[i], offs[i], fsizes[i]);
	}
	return out;
}

/* First half of file → 0x100000, second → 0x000000 (ganryu/bangbead/sengoku3). */
static uint8_t *load_p_swaphalf(src_t *s, const char *name, size_t fsz)
{
	uint8_t *buf, *out;
	size_t half = fsz / 2;
	buf = load_named(s, name, fsz);
	if (!buf)
		return NULL;
	out = calloc(1, fsz);
	if (!out) {
		free(buf);
		return NULL;
	}
	memcpy(out + 0x100000, buf, half);
	memcpy(out, buf + half, half);
	free(buf);
	printf("  P swaphalf %s (%zu)\n", name, fsz);
	return out;
}

/* LOAD32_WORD interleave (no host byteswap — GnGeo Musashi LE dumps). */
static uint8_t *load_p_interleave32(src_t *s, const char *a, const char *b,
				    size_t half)
{
	uint8_t *p1, *p2, *out;
	size_t i;
	p1 = load_named(s, a, half);
	p2 = load_named(s, b, half);
	if (!p1 || !p2) {
		free(p1);
		free(p2);
		return NULL;
	}
	out = malloc(half * 2);
	if (!out) {
		free(p1);
		free(p2);
		return NULL;
	}
	for (i = 0; i < half; i += 2) {
		out[2 * i + 0] = p1[i];
		out[2 * i + 1] = p1[i + 1];
		out[2 * i + 2] = p2[i];
		out[2 * i + 3] = p2[i + 1];
	}
	free(p1);
	free(p2);
	printf("  P interleave32 %s+%s → %zu\n", a, b, half * 2);
	return out;
}

static uint8_t *load_sma(src_t *s, size_t *out_sz)
{
	member_t ms[8];
	int n = list_members(s, ms, 8, 'p');
	int i;
	for (i = 0; i < n; i++) {
		if (strcasestr(ms[i].name, "neo-sma")) {
			*out_sz = ms[i].size;
			return load_named(s, ms[i].name, ms[i].size);
		}
	}
	/* Exact names used by some sets */
	{
		static const char *alts[] = {
			"neo-sma", "ka.neo-sma", "kf.neo-sma", "green.neo-sma", NULL
		};
		for (i = 0; alts[i]; i++) {
			if (src_has(s, alts[i])) {
				*out_sz = 0x40000;
				return load_named(s, alts[i], 0x40000);
			}
		}
	}
	fprintf(stderr, "SMA blob (neo-sma) not found\n");
	return NULL;
}

/* ---- C / V / M1 ---------------------------------------------------------- */

static uint8_t *load_c_roms(src_t *s, int npairs, size_t plane_sz, size_t *out_sz)
{
	member_t cs[16];
	int n, pair;
	uint8_t *out;
	size_t tiles = (size_t)npairs * plane_sz * 2;

	n = list_members(s, cs, 16, 'c');
	qsort(cs, (size_t)n, sizeof(cs[0]), cmp_member_num);
	if (n < npairs * 2) {
		fprintf(stderr, "need %d C ROMs, found %d\n", npairs * 2, n);
		return NULL;
	}
	out = calloc(1, tiles);
	if (!out)
		return NULL;
	*out_sz = tiles;
	for (pair = 0; pair < npairs; pair++) {
		uint8_t *a = load_named(s, cs[pair * 2].name, plane_sz);
		uint8_t *b = load_named(s, cs[pair * 2 + 1].name, plane_sz);
		size_t i;
		uint32_t base = (uint32_t)pair * (uint32_t)(plane_sz * 2);
		if (!a || !b) {
			free(a);
			free(b);
			free(out);
			return NULL;
		}
		for (i = 0; i < plane_sz; i++) {
			out[base + i * 2] = a[i];
			out[base + i * 2 + 1] = b[i];
		}
		free(a);
		free(b);
		printf("  C pair %d %s+%s @ %07x\n", pair,
		       cs[pair * 2].name, cs[pair * 2 + 1].name, (unsigned)base);
	}
	return out;
}

static int split_c_planes(const uint8_t *tiles, size_t tiles_sz,
			  int npairs, size_t plane_sz, uint8_t **planes)
{
	int pair;
	for (pair = 0; pair < npairs; pair++) {
		uint8_t *a = malloc(plane_sz);
		uint8_t *b = malloc(plane_sz);
		size_t i;
		uint32_t base = (uint32_t)pair * (uint32_t)(plane_sz * 2);
		if (!a || !b || base + plane_sz * 2 > tiles_sz) {
			free(a);
			free(b);
			return 0;
		}
		for (i = 0; i < plane_sz; i++) {
			a[i] = tiles[base + i * 2];
			b[i] = tiles[base + i * 2 + 1];
		}
		planes[pair * 2] = a;
		planes[pair * 2 + 1] = b;
	}
	return 1;
}

static uint8_t *load_v_roms(src_t *s, size_t *out_sz)
{
	member_t vs[16];
	int n = list_members(s, vs, 16, 'v');
	int i;
	size_t total = 0;
	uint8_t *out, *p;
	qsort(vs, (size_t)n, sizeof(vs[0]), cmp_member_num);
	if (n <= 0) {
		fprintf(stderr, "no V ROMs\n");
		return NULL;
	}
	for (i = 0; i < n; i++)
		total += vs[i].size;
	out = malloc(total);
	if (!out)
		return NULL;
	p = out;
	for (i = 0; i < n; i++) {
		uint8_t *buf = load_named(s, vs[i].name, vs[i].size);
		if (!buf) {
			free(out);
			return NULL;
		}
		memcpy(p, buf, vs[i].size);
		p += vs[i].size;
		free(buf);
		printf("  V + %s (%zu)\n", vs[i].name, vs[i].size);
	}
	*out_sz = total;
	return out;
}

static uint8_t *load_m1(src_t *s, size_t *out_sz)
{
	member_t ms[8];
	int n = list_members(s, ms, 8, 'm');
	int i;
	for (i = 0; i < n; i++) {
		if (strcasestr(ms[i].name, ".m1") || strcasestr(ms[i].name, "m1")) {
			*out_sz = ms[i].size;
			return load_named(s, ms[i].name, ms[i].size);
		}
	}
	fprintf(stderr, "M1 not found\n");
	return NULL;
}

static uint8_t *load_s1(src_t *s, size_t *out_sz)
{
	member_t ms[8];
	int n = list_members(s, ms, 8, 's');
	int i;
	for (i = 0; i < n; i++) {
		if (strcasestr(ms[i].name, ".s1") ||
		    (ms[i].name[0] && member_num_suffix(ms[i].name) == 1 &&
		     strchr(ms[i].name, 's'))) {
			/* Prefer clear .s1 */
			if (strcasestr(ms[i].name, ".s1") || strcasestr(ms[i].name, "-s1")) {
				*out_sz = ms[i].size;
				return load_named(s, ms[i].name, ms[i].size);
			}
		}
	}
	for (i = 0; i < n; i++) {
		if (strcasestr(ms[i].name, "s1")) {
			*out_sz = ms[i].size;
			return load_named(s, ms[i].name, ms[i].size);
		}
	}
	return NULL;
}

/* ---- game table ---------------------------------------------------------- */

enum p_style {
	P_CONCAT = 0,
	P_SWAPHALF,
	P_INTER32,
	P_SMA_P12,   /* sma@0xc0000 + p1@0x100000 + p2@0x500000 */
	P_SMA_EP4,   /* sma@0xc0000 + ep1..4 @ 0x100000 step 0x200000 */
	P_KOF2003,   /* inter32 8M + p3 @ 0x800000 */
};

typedef struct game_def {
	const char *name;
	const char *detect;     /* unique member basename */
	enum p_style pstyle;
	int c_pairs;            /* number of C odd/even pairs */
	size_t c_plane;         /* size of each Cx file */
	int s_from_c;           /* 1 = allocate empty S, decrypt fills it */
	size_t s_size;          /* FIX layer size (0x20000 or 0x80000) */
	int need_cmc50_m1;
	void (*decrypt)(GAME_ROMS *r);
	const char *note;
} game_def_t;

static void dec_kof98(GAME_ROMS *r) { kof98_decrypt_68k(r); }
static void dec_kof99(GAME_ROMS *r)
{
	kof99_decrypt_68k(r);
	kof99_neogeo_gfx_decrypt(r, 0x00);
}
static void dec_garou(GAME_ROMS *r)
{
	garou_decrypt_68k(r);
	kof99_neogeo_gfx_decrypt(r, 0x06);
}
static void dec_mslug3(GAME_ROMS *r)
{
	mslug3_decrypt_68k(r);
	kof99_neogeo_gfx_decrypt(r, 0xad);
}
static void dec_kof2000(GAME_ROMS *r)
{
	kof2000_decrypt_68k(r);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x00);
}
static void dec_kof2001(GAME_ROMS *r)
{
	kof2000_neogeo_gfx_decrypt(r, 0x1e);
	neogeo_cmc50_m1_decrypt(r);
}
static void dec_mslug4(GAME_ROMS *r)
{
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x31);
	neo_pcm2_snk_1999(r, 8);
}
static void dec_rotd(GAME_ROMS *r)
{
	neo_pcm2_snk_1999(r, 16);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x3f);
}
static void dec_kof2002(GAME_ROMS *r)
{
	kof2002_decrypt_68k(r);
	neo_pcm2_swap(r, 0);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0xec);
}
static void dec_matrim(GAME_ROMS *r)
{
	matrim_decrypt_68k(r);
	neo_pcm2_swap(r, 1);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x6a);
}
static void dec_pnyaa(GAME_ROMS *r)
{
	neo_pcm2_snk_1999(r, 4);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x2e);
}
static void dec_ganryu(GAME_ROMS *r) { kof99_neogeo_gfx_decrypt(r, 0x07); }
static void dec_s1945p(GAME_ROMS *r) { kof99_neogeo_gfx_decrypt(r, 0x05); }
static void dec_preisle2(GAME_ROMS *r) { kof99_neogeo_gfx_decrypt(r, 0x9f); }
static void dec_bangbead(GAME_ROMS *r) { kof99_neogeo_gfx_decrypt(r, 0xf8); }
static void dec_nitd(GAME_ROMS *r) { kof99_neogeo_gfx_decrypt(r, 0xff); }
static void dec_zupapa(GAME_ROMS *r) { kof99_neogeo_gfx_decrypt(r, 0xbd); }
static void dec_sengoku3(GAME_ROMS *r) { kof99_neogeo_gfx_decrypt(r, 0xfe); }
static void dec_mslug5(GAME_ROMS *r)
{
	mslug5_decrypt_68k(r);
	neo_pcm2_swap(r, 2);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x19);
}
static void dec_svc(GAME_ROMS *r)
{
	svc_px_decrypt(r);
	neo_pcm2_swap(r, 3);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x57);
}
static void dec_kof2003(GAME_ROMS *r)
{
	kof2003_decrypt_68k(r);
	neo_pcm2_swap(r, 5);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x9d);
}
static void dec_samsho5(GAME_ROMS *r)
{
	samsho5_decrypt_68k(r);
	neo_pcm2_swap(r, 4);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x0f);
}
static void dec_samsh5sp(GAME_ROMS *r)
{
	samsh5sp_decrypt_68k(r);
	neo_pcm2_swap(r, 6);
	neogeo_cmc50_m1_decrypt(r);
	kof2000_neogeo_gfx_decrypt(r, 0x0d);
}

/* s_size: match MAME hash/neogeo.xml "fixed" (0x20000 or 0x80000).
 * Banking (type 1/2) only applies when size > 0x20000 — see video.c. */
static const game_def_t games[] = {
	{ "kof98",    "242-p1.p1",       P_CONCAT,    4, 0x800000, 0, 0x20000, 0, dec_kof98,    "P only" },
	{ "kof99",    "251-p1.p1",       P_SMA_P12,   4, 0x800000, 1, 0x20000, 0, dec_kof99,    "SMA+CMC42" },
	{ "garou",    "253-ep1.p1",      P_SMA_EP4,   4, 0x800000, 1, 0x80000, 0, dec_garou,    "SMA+CMC42" },
	{ "mslug3",   "256-pg1.p1",      P_SMA_P12,   4, 0x800000, 1, 0x80000, 0, dec_mslug3,   "SMA+CMC42" },
	{ "kof2000",  "257-p1.p1",       P_SMA_P12,   4, 0x800000, 1, 0x80000, 1, dec_kof2000,  "SMA+CMC50" },
	{ "kof2001",  "262-p1-08-e0.p1", P_CONCAT,    4, 0x800000, 1, 0x20000, 1, dec_kof2001,  "CMC50" },
	{ "kof2002",  "265-p1.p1",       P_CONCAT,    4, 0x800000, 1, 0x20000, 1, dec_kof2002,  "CMC50+PCM2" },
	{ "mslug4",   "263-p1.p1",       P_CONCAT,    3, 0x800000, 1, 0x80000, 1, dec_mslug4,   "CMC50+PCM2" },
	{ "rotd",     "264-p1.p1",       P_CONCAT,    4, 0x800000, 1, 0x20000, 1, dec_rotd,     "CMC50+PCM2" },
	{ "pnyaa",    "pn202.p1",        P_CONCAT,    1, 0x800000, 1, 0x20000, 1, dec_pnyaa,    "CMC50+PCM2" },
	{ "ganryu",   "252-p1.p1",       P_SWAPHALF,  1, 0x800000, 1, 0x20000, 0, dec_ganryu,   "CMC42" },
	{ "s1945p",   "254-p1.p1",       P_CONCAT,    4, 0x800000, 1, 0x20000, 0, dec_s1945p,   "CMC42" },
	{ "preisle2", "255-p1.p1",       P_CONCAT,    3, 0x800000, 1, 0x20000, 0, dec_preisle2, "CMC42" },
	{ "bangbead", "259-p1.p1",       P_SWAPHALF,  1, 0x800000, 1, 0x20000, 0, dec_bangbead, "CMC42" },
	{ "nitd",     "260-p1.p1",       P_CONCAT,    1, 0x800000, 1, 0x20000, 0, dec_nitd,     "CMC42" },
	{ "zupapa",   "070-p1.p1",       P_CONCAT,    1, 0x800000, 1, 0x20000, 0, dec_zupapa,   "CMC42" },
	{ "sengoku3", "261-ph1.p1",      P_SWAPHALF,  2, 0x800000, 1, 0x20000, 0, dec_sengoku3, "CMC42" },
	/* MAME fixed=0x20000 + bank_type 1, but banking is size-gated off. */
	{ "mslug5",   "268-p1cr.p1",     P_INTER32,   4, 0x800000, 1, 0x20000, 1, dec_mslug5,   "PVC+CMC50+PCM2" },
	{ "svc",      "269-p1.p1",       P_INTER32,   4, 0x800000, 1, 0x80000, 1, dec_svc,      "PVC+CMC50+PCM2" },
	{ "kof2003",  "271-p1c.p1",      P_KOF2003,   4, 0x800000, 1, 0x80000, 1, dec_kof2003,  "PVC+CMC50+PCM2" },
	{ "samsho5",  "270-p1.p1",       P_CONCAT,    4, 0x800000, 1, 0x20000, 1, dec_samsho5,  "CMC50+PCM2" },
	{ "samsh5sp", "272-p1.p1",       P_CONCAT,    4, 0x800000, 1, 0x20000, 1, dec_samsh5sp, "CMC50+PCM2" },
	{ NULL, NULL, 0, 0, 0, 0, 0, 0, NULL, NULL }
};

static const game_def_t *find_game(src_t *s, const char *hint)
{
	const game_def_t *g;
	if (hint && hint[0]) {
		for (g = games; g->name; g++) {
			if (strcmp(g->name, hint) == 0)
				return g;
		}
	}
	for (g = games; g->name; g++) {
		if (src_has(s, g->detect))
			return g;
	}
	return NULL;
}

static uint8_t *load_p_for_game(src_t *s, const game_def_t *g, size_t *out_sz)
{
	switch (g->pstyle) {
	case P_SWAPHALF: {
		member_t ps[8];
		int n = list_members(s, ps, 8, 'p');
		int i;
		qsort(ps, (size_t)n, sizeof(ps[0]), cmp_member_num);
		for (i = 0; i < n; i++) {
			if (strcasestr(ps[i].name, "neo-sma"))
				continue;
			*out_sz = ps[i].size;
			return load_p_swaphalf(s, ps[i].name, ps[i].size);
		}
		return NULL;
	}
	case P_INTER32: {
		member_t ps[8];
		int n = list_members(s, ps, 8, 'p');
		int i, j = 0;
		member_t sel[2];
		qsort(ps, (size_t)n, sizeof(ps[0]), cmp_member_num);
		for (i = 0; i < n && j < 2; i++) {
			if (strcasestr(ps[i].name, "neo-sma"))
				continue;
			/* skip p3 for kof2003 path */
			if (member_num_suffix(ps[i].name) >= 3 &&
			    !strcasestr(ps[i].name, "p1") &&
			    !strcasestr(ps[i].name, "p2"))
				continue;
			if (strcasestr(ps[i].name, "p1") || strcasestr(ps[i].name, "p2") ||
			    strcasestr(ps[i].name, ".p1") || strcasestr(ps[i].name, ".p2"))
				sel[j++] = ps[i];
		}
		if (j < 2) {
			/* fallback: first two non-sma p* */
			j = 0;
			for (i = 0; i < n && j < 2; i++) {
				if (!strcasestr(ps[i].name, "neo-sma") &&
				    member_num_suffix(ps[i].name) <= 2)
					sel[j++] = ps[i];
			}
		}
		if (j < 2) {
			fprintf(stderr, "need 2 P ROMs for interleave\n");
			return NULL;
		}
		*out_sz = sel[0].size * 2;
		return load_p_interleave32(s, sel[0].name, sel[1].name, sel[0].size);
	}
	case P_KOF2003: {
		uint8_t *base, *p3, *out;
		base = load_p_interleave32(s, "271-p1c.p1", "271-p2c.p2", 0x400000);
		if (!base)
			return NULL;
		p3 = load_named(s, "271-p3c.p3", 0x100000);
		if (!p3) {
			free(base);
			return NULL;
		}
		out = calloc(1, 0x900000);
		if (!out) {
			free(base);
			free(p3);
			return NULL;
		}
		memcpy(out, base, 0x800000);
		memcpy(out + 0x800000, p3, 0x100000);
		free(base);
		free(p3);
		*out_sz = 0x900000;
		return out;
	}
	case P_SMA_P12: {
		uint8_t *sma, *out;
		size_t sma_sz = 0;
		member_t ps[8];
		int n = list_members(s, ps, 8, 'p');
		int i;
		member_t p1 = {{0}}, p2 = {{0}};
		int got1 = 0, got2 = 0;
		sma = load_sma(s, &sma_sz);
		if (!sma)
			return NULL;
		qsort(ps, (size_t)n, sizeof(ps[0]), cmp_member_num);
		for (i = 0; i < n; i++) {
			if (strcasestr(ps[i].name, "neo-sma"))
				continue;
			if (!got1 && (strcasestr(ps[i].name, "p1") ||
				      strcasestr(ps[i].name, "pg1"))) {
				p1 = ps[i];
				got1 = 1;
			} else if (!got2 && (strcasestr(ps[i].name, "p2") ||
					    strcasestr(ps[i].name, "pg2") ||
					    strcasestr(ps[i].name, "sp2"))) {
				p2 = ps[i];
				got2 = 1;
			}
		}
		if (!got1 || !got2) {
			fprintf(stderr, "SMA set needs p1+p2\n");
			free(sma);
			return NULL;
		}
		out = calloc(1, 0x900000);
		if (!out) {
			free(sma);
			return NULL;
		}
		{
			uint8_t *b1 = load_named(s, p1.name, p1.size);
			uint8_t *b2 = load_named(s, p2.name, p2.size);
			if (!b1 || !b2) {
				free(b1);
				free(b2);
				free(sma);
				free(out);
				return NULL;
			}
			bytes_place(out, 0x900000, 0x0c0000, sma, sma_sz);
			bytes_place(out, 0x900000, 0x100000, b1, p1.size);
			bytes_place(out, 0x900000, 0x500000, b2, p2.size);
			free(b1);
			free(b2);
			free(sma);
			printf("  P SMA + %s + %s\n", p1.name, p2.name);
		}
		*out_sz = 0x900000;
		return out;
	}
	case P_SMA_EP4: {
		uint8_t *sma, *out;
		size_t sma_sz = 0;
		static const char *eps[] = {
			"253-ep1.p1", "253-ep2.p2", "253-ep3.p3", "253-ep4.p4"
		};
		int i;
		sma = load_sma(s, &sma_sz);
		if (!sma)
			return NULL;
		out = calloc(1, 0x900000);
		if (!out) {
			free(sma);
			return NULL;
		}
		bytes_place(out, 0x900000, 0x0c0000, sma, sma_sz);
		free(sma);
		for (i = 0; i < 4; i++) {
			uint8_t *b = load_named(s, eps[i], 0x200000);
			if (!b) {
				free(out);
				return NULL;
			}
			bytes_place(out, 0x900000, 0x100000 + (size_t)i * 0x200000, b, 0x200000);
			free(b);
		}
		*out_sz = 0x900000;
		printf("  P SMA + ep1-4\n");
		return out;
	}
	case P_CONCAT:
	default: {
		member_t ps[8];
		int n = list_members(s, ps, 8, 'p');
		int i, j = 0;
		member_t sel[4];
		size_t total = 0, off = 0;
		uint8_t *out;
		qsort(ps, (size_t)n, sizeof(ps[0]), cmp_member_num);
		for (i = 0; i < n && j < 4; i++) {
			if (strcasestr(ps[i].name, "neo-sma"))
				continue;
			sel[j++] = ps[i];
			total += ps[i].size;
		}
		if (j == 0) {
			fprintf(stderr, "no P ROMs\n");
			return NULL;
		}
		out = calloc(1, total);
		if (!out)
			return NULL;
		for (i = 0; i < j; i++) {
			uint8_t *b = load_named(s, sel[i].name, sel[i].size);
			if (!b) {
				free(out);
				return NULL;
			}
			memcpy(out + off, b, sel[i].size);
			printf("  P + %s @ %07zx\n", sel[i].name, off);
			off += sel[i].size;
			free(b);
		}
		*out_sz = total;
		return out;
	}
	}
}

/* ---- zip out ------------------------------------------------------------- */

static int zip_write_member(const char *dir, const char *name,
			    const void *data, size_t sz)
{
	char path[768];
	FILE *f;
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	f = fopen(path, "wb");
	if (!f) {
		fprintf(stderr, "write %s: %s\n", path, strerror(errno));
		return 0;
	}
	if (fwrite(data, 1, sz, f) != sz) {
		fclose(f);
		return 0;
	}
	fclose(f);
	printf("  + %s (%zu)\n", name, sz);
	return 1;
}

static int rm_path(const char *fpath, const struct stat *sb, int typeflag,
		   struct FTW *ftwbuf)
{
	(void)sb;
	(void)typeflag;
	(void)ftwbuf;
	return remove(fpath);
}

static int pack_zip(const char *tmpdir, const char *outpath)
{
	char cmd[2048];
	char out_abs[1024];
	if (outpath[0] == '/') {
		snprintf(out_abs, sizeof(out_abs), "%s", outpath);
	} else {
		char cwd[512];
		if (!getcwd(cwd, sizeof(cwd)))
			return 0;
		snprintf(out_abs, sizeof(out_abs), "%s/%s", cwd, outpath);
	}
	snprintf(cmd, sizeof(cmd),
		 "cd '%s' && zip -0 -q -j '%s' *", tmpdir, out_abs);
	if (system(cmd) != 0) {
		fprintf(stderr, "zip failed (is `zip` installed?)\n");
		return 0;
	}
	return 1;
}

static void path_stem(const char *path, char *out, size_t out_sz)
{
	const char *base = basename_of(path);
	const char *dot = strrchr(base, '.');
	size_t n = dot ? (size_t)(dot - base) : strlen(base);
	if (n >= out_sz)
		n = out_sz - 1;
	memcpy(out, base, n);
	out[n] = 0;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s <game.zip|rom-dir> [-o out.zip] [--game name]\n"
		"       %s --list\n"
		"Decrypt encrypted Neo Geo MAME sets to plain zips for "
		"neo_zip_flash.\n",
		argv0, argv0);
}

static void list_games(void)
{
	const game_def_t *g;
	printf("Supported sets:\n");
	for (g = games; g->name; g++)
		printf("  %-10s  detect=%-20s  %s\n", g->name, g->detect, g->note);
}

int main(int argc, char **argv)
{
	const char *inpath = NULL;
	const char *outpath = NULL;
	const char *force_game = NULL;
	char stem[64], outbuf[512], namebuf[64];
	src_t src;
	GAME_ROMS r;
	const game_def_t *g;
	uint8_t *planes[8];
	size_t p_sz = 0, c_sz = 0, v_sz = 0, m_sz = 0, s_sz = 0;
	int i, np;

	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
			outpath = argv[++i];
		else if (strcmp(argv[i], "--game") == 0 && i + 1 < argc)
			force_game = argv[++i];
		else if (strcmp(argv[i], "--list") == 0) {
			list_games();
			return 0;
		} else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(argv[0]);
			return 0;
		} else if (!inpath)
			inpath = argv[i];
		else {
			usage(argv[0]);
			return 1;
		}
	}
	if (!inpath) {
		usage(argv[0]);
		return 1;
	}

	path_stem(inpath, stem, sizeof(stem));
	if (!outpath) {
		snprintf(outbuf, sizeof(outbuf), "%s_dec.zip", stem);
		outpath = outbuf;
	}

	if (!src_open(&src, inpath))
		return 1;
	g = find_game(&src, force_game ? force_game : stem);
	if (!g) {
		fprintf(stderr, "unrecognized encrypted set (try --list / --game)\n");
		src_close(&src);
		return 1;
	}
	printf("game: %s (%s)\n", g->name, g->note);

	memset(&r, 0, sizeof(r));
	r.info.name = (char *)g->name;

	printf("loading P…\n");
	r.cpu_m68k.p = load_p_for_game(&src, g, &p_sz);
	r.cpu_m68k.size = (Uint32)p_sz;
	if (!r.cpu_m68k.p) {
		src_close(&src);
		return 1;
	}

	printf("loading C…\n");
	r.tiles.p = load_c_roms(&src, g->c_pairs, g->c_plane, &c_sz);
	r.tiles.size = (Uint32)c_sz;
	if (!r.tiles.p) {
		src_close(&src);
		return 1;
	}

	if (g->s_from_c) {
		size_t ssz = g->s_size ? g->s_size : 0x20000;
		r.game_sfix.p = calloc(1, ssz);
		r.game_sfix.size = (Uint32)ssz;
		if (!r.game_sfix.p) {
			src_close(&src);
			return 1;
		}
		printf("  S from C (%zu KiB)\n", ssz / 1024);
	} else {
		r.game_sfix.p = load_s1(&src, &s_sz);
		r.game_sfix.size = (Uint32)s_sz;
		if (!r.game_sfix.p) {
			fprintf(stderr, "S1 required for %s\n", g->name);
			src_close(&src);
			return 1;
		}
	}

	printf("loading V…\n");
	r.adpcma.p = load_v_roms(&src, &v_sz);
	r.adpcma.size = (Uint32)v_sz;
	if (!r.adpcma.p) {
		src_close(&src);
		return 1;
	}

	printf("loading M1…\n");
	{
		uint8_t *m1 = load_m1(&src, &m_sz);
		if (!m1) {
			src_close(&src);
			return 1;
		}
		if (g->need_cmc50_m1) {
			/* cmc50_m1_decrypt always reads 0x80000 from audiocrypt.
			 * MAME mirrors short dumps (e.g. kof2000 256 KiB) with
			 * ROM_RELOAD — zero-padding decrypts to garbage banks. */
			size_t off = 0;
			size_t chunk = m_sz;
			r.cpu_z80c.p = calloc(1, 0x80000);
			if (!r.cpu_z80c.p) {
				free(m1);
				src_close(&src);
				return 1;
			}
			if (chunk > 0x80000)
				chunk = 0x80000;
			while (off < 0x80000 && chunk) {
				size_t n = chunk;
				if (n > 0x80000 - off)
					n = 0x80000 - off;
				memcpy(r.cpu_z80c.p + off, m1, n);
				off += n;
			}
			if (chunk && chunk < 0x80000)
				printf("  M1 mirrored %zu → 512 KiB\n", chunk);
			r.cpu_z80c.size = 0x80000;
			free(m1);
			r.cpu_z80.p = calloc(1, 0x90000);
			r.cpu_z80.size = 0x90000;
			if (!r.cpu_z80.p) {
				src_close(&src);
				return 1;
			}
		} else {
			r.cpu_z80.p = m1;
			r.cpu_z80.size = (Uint32)m_sz;
			r.cpu_z80c.p = NULL;
			r.cpu_z80c.size = 0;
		}
	}

	src_close(&src);

	printf("decrypting %s…\n", g->name);
	g->decrypt(&r);
	{
		uint8_t *p = r.cpu_m68k.p;
		uint16_t w0 = (uint16_t)(p[0] | (p[1] << 8));
		uint16_t w1 = (uint16_t)(p[2] | (p[3] << 8));
		uint16_t w2 = (uint16_t)(p[4] | (p[5] << 8));
		uint16_t w3 = (uint16_t)(p[6] | (p[7] << 8));
		printf("  P vectors SP=%08x PC=%08x\n",
		       ((uint32_t)w0 << 16) | w1, ((uint32_t)w2 << 16) | w3);
	}

	np = g->c_pairs;
	printf("splitting C (%d pairs)…\n", np);
	memset(planes, 0, sizeof(planes));
	if (!split_c_planes(r.tiles.p, c_sz, np, g->c_plane, planes)) {
		fprintf(stderr, "OOM splitting C\n");
		return 1;
	}

	printf("writing members…\n");
	{
		char tmpl[] = "/tmp/neo_dec_XXXXXX";
		char *tmpdir = mkdtemp(tmpl);
		size_t v_chunk, v_off;
		int vi;
		if (!tmpdir) {
			perror("mkdtemp");
			return 1;
		}
		snprintf(namebuf, sizeof(namebuf), "%s-p1.p1", g->name);
		if (!zip_write_member(tmpdir, namebuf, r.cpu_m68k.p, r.cpu_m68k.size))
			goto fail_tmp;
		snprintf(namebuf, sizeof(namebuf), "%s-m1.m1", g->name);
		if (g->need_cmc50_m1) {
			/* After cmc50_m1_decrypt, audiocrypt holds plain decrypted 512 KiB.
			 * Write that — not the 0x90000 audiocpu banking image. neo_zip's
			 * Z80 banking indexes from ROM+0 (FBNeo-style); the 0x90000 layout
			 * shifts every bank ≥64 KiB and yields looping/garbage ADPCM. */
			if (!zip_write_member(tmpdir, namebuf, r.cpu_z80c.p, 0x80000))
				goto fail_tmp;
		} else {
			if (!zip_write_member(tmpdir, namebuf, r.cpu_z80.p, r.cpu_z80.size))
				goto fail_tmp;
		}
		snprintf(namebuf, sizeof(namebuf), "%s-s1.s1", g->name);
		if (!zip_write_member(tmpdir, namebuf, r.game_sfix.p, r.game_sfix.size))
			goto fail_tmp;

		/* Split V into ≤8 MiB chunks for neo_zip_flash comfort */
		v_chunk = 0x800000;
		v_off = 0;
		vi = 1;
		while (v_off < v_sz) {
			size_t n = v_sz - v_off;
			if (n > v_chunk)
				n = v_chunk;
			snprintf(namebuf, sizeof(namebuf), "%s-v%d.v%d", g->name, vi, vi);
			if (!zip_write_member(tmpdir, namebuf, r.adpcma.p + v_off, n))
				goto fail_tmp;
			v_off += n;
			vi++;
		}
		for (i = 0; i < np * 2; i++) {
			snprintf(namebuf, sizeof(namebuf), "%s-c%d.c%d", g->name, i + 1, i + 1);
			if (!zip_write_member(tmpdir, namebuf, planes[i], g->c_plane))
				goto fail_tmp;
		}
		printf("packing %s…\n", outpath);
		if (!pack_zip(tmpdir, outpath))
			goto fail_tmp;
		nftw(tmpdir, rm_path, 8, FTW_DEPTH | FTW_PHYS);
		printf("wrote %s\n", outpath);
		return 0;
fail_tmp:
		nftw(tmpdir, rm_path, 8, FTW_DEPTH | FTW_PHYS);
		return 1;
	}
}
