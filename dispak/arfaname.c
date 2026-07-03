/*
 * АРФА region name <-> Unix path mapping.  See arfaname.h.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 *
 * You can redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation;
 * either version 2 of the License, or (at your discretion) any later version.
 * See the accompanying file "COPYING" for more details.
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "gost10859.h"
#include "encoding.h"
#include "arfaname.h"

#define COMP_MAX	320		/* <= 96 GOST codes * 3 UTF-8 bytes + NUL */

static void
utf8_put(char **pp, unsigned short u)
{
	char *p = *pp;

	if (u < 0x80)
		*p++ = u;
	else if (u < 0x800) {
		*p++ = 0xc0 | (u >> 6);
		*p++ = 0x80 | (u & 0x3f);
	} else {
		*p++ = 0xe0 | (u >> 12);
		*p++ = 0x80 | ((u >> 6) & 0x3f);
		*p++ = 0x80 | (u & 0x3f);
	}
	*pp = p;
}

static int
is_letter(unsigned char g)
{
	return g >= GOST_A && g <= GOST_Z;	/* 0o40 .. 0o114 */
}

static int
is_homoglyph(unsigned char g)
{
	return is_letter(g) &&
		gost_to_unicode2(g, 0) != gost_to_unicode2(g, 1);
}

/*
 * Render one component [comp, comp+len) into *pp.  Ambiguous components take
 * homoglyphs from amb_latin; deterministic ones from their unique letters.
 */
static void
render_component(char **pp, const unsigned char *comp, int len, int amb_latin)
{
	int i, hasL = 0, hasC = 0, hscript;

	for (i = 0; i < len; ++i) {
		unsigned char g = comp[i];
		if (!is_letter(g) || is_homoglyph(g))
			continue;
		if (gost_to_unicode2(g, 0) < 0x400)
			hasL = 1;
		else
			hasC = 1;
	}
	if ((hasL && hasC) || (!hasL && !hasC))
		hscript = amb_latin;		/* ambiguous */
	else
		hscript = hasL ? 1 : 0;		/* deterministic */

	for (i = 0; i < len; ++i) {
		unsigned char g = comp[i];
		int lat = is_homoglyph(g) ? hscript : 0;
		utf8_put(pp, gost_to_unicode2(g, lat));
	}
}

void
arfa_render_name(char *dst, const unsigned char *gname, int amb_latin)
{
	char *p = dst;
	int i = 0;

	while (gname[i] != GOST_EOF) {
		int j = i;
		while (gname[j] != GOST_EOF && gname[j] != GOST_DOT)
			++j;
		render_component(&p, gname + i, j - i, amb_latin);
		i = j;
		if (gname[i] == GOST_DOT) {
			*p++ = '/';
			++i;
		}
	}
	*p = 0;
}

int
arfa_resolve(const char *root, const unsigned char *gname, int create,
	int amb_latin, char *path)
{
	int i = 0;

	if (strlen(root) >= ARFA_PATH_MAX)
		return 0;
	strcpy(path, root);

	while (gname[i] != GOST_EOF) {
		int j = i, is_leaf, k, nc, matched;
		char cand[2][COMP_MAX], *p;
		const char *pick[2];
		struct stat st;

		while (gname[j] != GOST_EOF && gname[j] != GOST_DOT)
			++j;
		is_leaf = (gname[j] == GOST_EOF);

		if (create && is_leaf) {
			/* Append the leaf; it need not exist yet. */
			p = cand[0];
			render_component(&p, gname + i, j - i, amb_latin);
			*p = 0;
			k = strlen(path);
			if (k + 1 + (int) strlen(cand[0]) >= ARFA_PATH_MAX)
				return 0;
			path[k] = '/';
			strcpy(path + k + 1, cand[0]);
			return 1;
		}

		/* Candidate rendering(s); dedup deterministic components. */
		p = cand[0]; render_component(&p, gname + i, j - i, amb_latin); *p = 0;
		p = cand[1]; render_component(&p, gname + i, j - i, !amb_latin); *p = 0;
		nc = 0;
		pick[nc++] = cand[0];
		if (strcmp(cand[0], cand[1]) != 0)
			pick[nc++] = cand[1];

		matched = 0;
		for (k = 0; k < nc; ++k) {
			char trial[ARFA_PATH_MAX];
			int n = strlen(path);
			if (n + 1 + (int) strlen(pick[k]) >= ARFA_PATH_MAX)
				continue;
			memcpy(trial, path, n);
			trial[n] = '/';
			strcpy(trial + n + 1, pick[k]);
			if (stat(trial, &st) == 0) {
				strcpy(path, trial);
				matched = 1;
				break;
			}
		}
		if (!matched)
			return 0;

		i = j;
		if (gname[i] == GOST_DOT)
			++i;
	}
	return 1;
}
