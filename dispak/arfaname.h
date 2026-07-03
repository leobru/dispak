/*
 * Mapping between АРФА GOST-10859 region names and Unix paths, with
 * per-component (каталог level) homoglyph handling.  Shared by dispak
 * (arfa.c) and besmtool.
 *
 * A каталог level is classified by its letters:
 *   - "ambiguous"    - only homoglyphs, or both a uniquely-Cyrillic and a
 *                      uniquely-Latin letter: homoglyphs follow amb_latin;
 *   - "deterministic"- a uniquely-scripted letter fixes the script and the
 *                      homoglyphs render in it, regardless of amb_latin.
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
#ifndef arfaname_h
#define arfaname_h

#define ARFA_PATH_MAX	5120	/* size for path buffers passed to arfa_resolve */

/*
 * Render a 0377-terminated GOST name to a '/'-separated UTF-8 relative path
 * (каталог dots become '/').  Ambiguous components use amb_latin for homoglyphs.
 */
void arfa_render_name(char *dst, const unsigned char *gname, int amb_latin);

/*
 * Resolve a GOST name to a filesystem path under root.
 *  - create != 0: only the parent каталог chain must already exist on disk;
 *    the leaf is appended, its ambiguous homoglyphs rendered with amb_latin.
 *  - create == 0: every component must exist; an ambiguous component is tried
 *    both ways (homoglyphs Latin, homoglyphs Cyrillic) and whichever exists is
 *    used.  amb_latin then only sets which variant is tried first.
 * Returns 1 and fills path (>= ARFA_PATH_MAX bytes) on success, else 0.
 */
int arfa_resolve(const char *root, const unsigned char *gname, int create,
	int amb_latin, char *path);

#endif /* arfaname_h */
