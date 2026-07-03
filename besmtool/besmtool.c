/*
 * Create, modify, display BESM-6 disk images.
 * Usage:
 *	besmtool list [<disk-number>]
 *	besmtool erase <disk-number> [<options>...]
 *	besmtool zero <disk-number> [<options>...]
 *	besmtool dump <disk-number> [<options>...] [--to-file=<filename>]
 *	besmtool write <disk-number> [<options>...]
 *
 * Options:
 * 	--start=<zone>
 *	--last=<zone>
 *	--length=<nzones>
 *
 * Write options:
 *	--from-file=<filename>
 *	--from-disk=<disknum>
 * 	--from-start=<zone>
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
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <getopt.h>
#include <sys/param.h>
#include "config.h"
#include "besmtool.h"
#include "disk.h"
#include "encoding.h"
#include "gost10859.h"

enum {
	OPT_START,
	OPT_LAST,
	OPT_LENGTH,
	OPT_FROM_FILE,
	OPT_FROM_DISK,
	OPT_FROM_DIR,
	OPT_FROM_START,
	OPT_TO_FILE,
	OPT_ENCODING,
	OPT_ARFA_DIR,
	OPT_FROM_ARFA,
};

/* Table of options. */
static struct option longopts[] = {
	/* option	     has arg		integer code */
	{ "help",		0,	0,	'h'		},
	{ "version",		0,	0,	'V'		},
	{ "start",		1,	0,	OPT_START	},
	{ "last",		1,	0,	OPT_LAST	},
	{ "length",		1,	0,	OPT_LENGTH	},
	{ "from-file",		1,	0,	OPT_FROM_FILE	},
	{ "from-disk",		1,	0,	OPT_FROM_DISK	},
	{ "from-dir",		1,	0,	OPT_FROM_DIR	},
	{ "from-start",		1,	0,	OPT_FROM_START	},
	{ "to-file",		1,	0,	OPT_TO_FILE	},
	{ "encoding",		1,	0,	OPT_ENCODING	},
	{ "arfa-dir",		1,	0,	OPT_ARFA_DIR	},
	{ "from-arfa",		1,	0,	OPT_FROM_ARFA	},
	{ 0,			0,	0,	0		},
};

/*
 * АРФА archive regions (области) are individual Unix files under an archive
 * root.  A volume argument that is not a number is taken as an область name
 * (region names cannot start with a digit) and opened as such a file instead
 * of a numbered disk image.  Unlike numbered volumes, области are addressed
 * physically (no ZONE_OFFSET) - disk_open_path handles that.
 */
char *besm_arfa_dir;		/* --arfa-dir, NULL = ~/.besm6/arfa */
char *besm_arfa_region;		/* primary volume, if "arfa:<name>" */
char *besm_from_arfa;		/* --from-arfa=<name> source region */

static void
utf8_encode (char **pp, unsigned short u)
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

/*
 * Build the Unix path of an область from its (UTF-8) name, exactly as
 * dispak's fs_path does: fold the name to GOST, then render it back through
 * gost_to_unicode, with the dot (GOST_DOT) separating каталог levels into
 * directories.
 */
static void
arfa_path (char *dst, const char *name)
{
	unsigned char *s = (unsigned char *) name;
	char *p;

	if (besm_arfa_dir)
		strcpy (dst, besm_arfa_dir);
	else {
		disk_local_path (dst);
		strcat (dst, "/arfa");
	}
	p = dst + strlen (dst);
	*p++ = '/';
	while (*s) {
		unsigned char g = utf8_to_gost (&s);
		if (g == GOST_DOT)
			*p++ = '/';
		else
			utf8_encode (&p, gost_to_unicode (g));
	}
	*p = 0;
}

/*
 * Open the primary volume: an АРФА region if "arfa:<name>" was given,
 * otherwise a numbered disk image.
 */
void *
open_disk (unsigned diskno, unsigned mode)
{
	char path [MAXPATHLEN];
	void *d;

	if (! besm_arfa_region)
		return disk_open (diskno, mode);
	arfa_path (path, besm_arfa_region);
	/* Never fabricate a region file: that is mkarfa's job (it also
	 * maintains the catalog).  Open existing files only. */
	if (mode == DISK_CREATE)
		mode = DISK_READ_WRITE;
	d = disk_open_path (path, mode);
	if (! d)
		fprintf (stderr, "Region '%s': cannot open %s\n",
			besm_arfa_region, path);
	return d;
}

/* Open a copy source: an область if --from-arfa was given, else a disk. */
void *
open_from_disk (unsigned diskno, unsigned mode)
{
	char path [MAXPATHLEN];
	void *d;

	if (! besm_from_arfa)
		return disk_open (diskno, mode);
	arfa_path (path, besm_from_arfa);
	d = disk_open_path (path, mode);
	if (! d)
		fprintf (stderr, "Region '%s': cannot open %s\n",
			besm_from_arfa, path);
	return d;
}

/* Length of an область in zones, or -1. */
static int
arfa_region_zones (const char *name)
{
	char path [MAXPATHLEN];
	void *d;
	int n;

	arfa_path (path, name);
	d = disk_open_path (path, DISK_READ_ONLY);
	if (! d)
		return -1;
	n = disk_size (d);
	disk_close (d);
	return n;
}

void
usage ()
{
	fprintf (stderr, "besmtool version %s\n", PACKAGE_VERSION);
	fprintf (stderr, "Handle BESM-6 disk images.\n");
	fprintf (stderr, "\n");

	fprintf (stderr, "Usage:\n");
	fprintf (stderr, "\tbesmtool list [<volume>]\n");
	fprintf (stderr, "\tbesmtool pass [<volume>]\n");
	fprintf (stderr, "\tbesmtool search <volume> <pattern>\n");
	fprintf (stderr, "\tbesmtool erase <volume> [<options>...]\n");
	fprintf (stderr, "\tbesmtool zero <volume> [<options>...]\n");
	fprintf (stderr, "\tbesmtool view <volume> [<options>...] [--encoding=g,k,t,i]\n");
	fprintf (stderr, "\tbesmtool dump <volume> [<options>...] [--to-file=<filename>]\n");
	fprintf (stderr, "\tbesmtool write <volume> [<options>...]\n");

	fprintf (stderr, "A <volume> is a disk number, or an АРФА region name\n");
	fprintf (stderr, "(anything not starting with a digit).\n");

	fprintf (stderr, "Options:\n");
	fprintf (stderr, "\t--start=<zone>\n");
	fprintf (stderr, "\t--last=<zone>\n");
	fprintf (stderr, "\t--length=<nzones>\n");
	fprintf (stderr, "\t--arfa-dir=<dir> (АРФА archive root, default ~/.besm6/arfa)\n");

	fprintf (stderr, "View options:\n");
	fprintf (stderr, "\t--encoding=g,k,t,i (default g,k)\n");
	fprintf (stderr, "\t\tg - GOST-10859 encoding\n");
	fprintf (stderr, "\t\tk - KOI-7 encoding\n");
	fprintf (stderr, "\t\tt - 'Text' encoding of Dubna monitoring system\n");
	fprintf (stderr, "\t\ti - encoding of IPMCE autocode by Chaikovsky\n");

	fprintf (stderr, "Write options:\n");
	fprintf (stderr, "\t--from-file=<filename>\n");
	fprintf (stderr, "\t--from-disk=<disknum>\n");
	fprintf (stderr, "\t--from-arfa=<region> (copy from an АРФА region)\n");
	fprintf (stderr, "\t--from-dir=<dirname>\n");
	fprintf (stderr, "\t--from-start=<zone>\n");
	exit (-1);
}

int
main (int argc, char **argv)
{
	unsigned length = 0, from_diskno = 0, from_start = 0;
	int start = 0, last = -1;
	char *from_file = 0, *to_file = 0, *from_dir = 0, *view_encoding = "g,k";
	unsigned diskno;
	int c;

	for (;;) {
		c = getopt_long (argc, argv, "hV", longopts, 0);
		if (c < 0)
			break;
		switch (c) {
		case 'h':
			usage ();
			break;
		case 'V':
			printf ("Version: %s\n", PACKAGE_VERSION);
			return 0;
		case OPT_START:
			start = strtol (optarg, 0, 0);
			break;
		case OPT_LAST:
			last = strtol (optarg, 0, 0);
			break;
		case OPT_LENGTH:
			length = strtol (optarg, 0, 0);
			break;
		case OPT_FROM_FILE:
			from_file = optarg;
			break;
		case OPT_FROM_DISK:
			from_diskno = strtol (optarg, 0, 0);
			break;
		case OPT_FROM_DIR:
			from_dir = optarg;
			break;
		case OPT_FROM_START:
			from_start = strtol (optarg, 0, 0);
			break;
		case OPT_TO_FILE:
			to_file = optarg;
			break;
		case OPT_ENCODING:
			view_encoding = optarg;
			break;
		case OPT_ARFA_DIR:
			besm_arfa_dir = optarg;
			break;
		case OPT_FROM_ARFA:
			besm_from_arfa = optarg;
			break;
		}
	}
	argc -= optind;
	argv += optind;
	/*
	 * A non-numeric volume argument is an АРФА region name
	 * (region names cannot start with a digit).
	 */
	if (argc >= 2 && argv[1][0] && ! isdigit ((unsigned char) argv[1][0]))
		besm_arfa_region = argv[1];
	if (last >= start && ! length)
		length = 1 + last - start;
	/* For a region, default the extent to its whole length. */
	if (besm_arfa_region && ! length && last < 0) {
		int n = arfa_region_zones (besm_arfa_region);
		if (n > 0)
			length = n;
	}
	switch (argc) {
	case 1:
		if (strcmp ("list", argv[0]) == 0) {
			list_all_disks ();
			return 0;
		}
		if (strcmp ("pass", argv[0]) == 0) {
			passports (2053, start);
			return 0;
		}
		break;
	case 2:
		diskno = strtol (argv[1], 0, 0);

		if (strcmp ("list", argv[0]) == 0) {
			list_disk (diskno);
			return 0;
		}
		if (strcmp ("pass", argv[0]) == 0) {
			passports (diskno, start);
			return 0;
		}
		if (strcmp ("erase", argv[0]) == 0) {
			erase_disk (diskno, start, length, 1);
			return 0;
		}
		if (strcmp ("zero", argv[0]) == 0) {
			erase_disk (diskno, start, length, 0);
			return 0;
		}
		if (strcmp ("view", argv[0]) == 0) {
			view_disk (diskno, start, length, view_encoding);
			return 0;
		}
		if (strcmp ("dump", argv[0]) == 0) {
			if (to_file)
				disk_to_file (diskno, start, length, to_file);
			else
				dump_disk (diskno, start, length);
			return 0;
		}
		if (strcmp ("check", argv[0]) == 0) {
			check_disk (diskno, start, length);
			return 0;
		}
		if (strcmp ("write", argv[0]) == 0) {
			if (from_file)
				file_to_disk (diskno, start, length,
					from_file, from_start);
			else if (from_dir)
				dir_to_disk (diskno, from_dir);
			else
				disk_to_disk (diskno, start, length,
					from_diskno, from_start);
			return 0;
		}
		break;
	case 3:
		diskno = strtol (argv[1], 0, 0);

		if (strcmp ("search", argv[0]) == 0) {
			search_disk (diskno, (unsigned char*) argv[2], start, length);
			return 0;
		}
		break;
	}
	usage ();
	return 0;
}

unsigned long long userid() {
	return 0x419912345678LL;
}
