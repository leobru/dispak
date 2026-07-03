/*
 * Display a Dispak input queue buffer.
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
#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "defs.h"
#include "encoding.h"
#include "gost10859.h"
#include "iobuf.h"
#include "arfa.h"

static const char *opname_short_bemsh[64] = {
	"зп",   "зпм",  "рег",  "счм",  "сл",   "вч",   "вчоб", "вчаб",
	"сч",   "и",    "нтж",  "слц",  "знак", "или",  "дел",  "умн",
	"сбр",  "рзб",  "чед",  "нед",  "слп",  "вчп",  "сд",   "рж",
	"счрж", "счмр", "э32",  "увв",  "слпа", "вчпа", "сда",  "ржа",
	"уи",   "уим",  "счи",  "счим", "уии",  "сли",  "э46",  "э47",
	"э50",  "э51",  "э52",  "э53",  "э54",  "э55",  "э56",  "э57",
	"э60",  "э61",  "э62",  "э63",  "э64",  "э65",  "э66",  "э67",
	"э70",  "э71",  "э72",  "э73",  "э74",  "э75",  "э76",  "э77",
};

static const char *opname_long_bemsh[16] = {
	"э20", "э21", "мода", "мод", "уиа",  "слиа", "по",  "пе",
	"пб",  "пв",  "выпр", "стоп", "пио", "пино", "э36", "цикл",
};

static void
usage(FILE *out)
{
	fprintf(out, "Usage: ibview [--latin] [--path DIR] <buffer-number>\n");
	fprintf(out, "       ibview --help\n");
}

static int
parse_buffer_number(const char *arg, unsigned *bufno)
{
	char *end;
	unsigned long val;

	errno = 0;
	val = strtoul(arg, &end, 8);
	if (errno || *arg == '\0' || *end != '\0' || val == 0 || val >= 0200)
		return -1;
	*bufno = (unsigned) val;
	return 0;
}

static void
default_queue_dir(char *path)
{
	const char *home = getenv("HOME");

	if (!home)
		home = "/tmp";
	snprintf(path, MAXPATHLEN, "%s/.besm6/input_queue", home);
}

static uint64_t
word_value(word_t w)
{
	return (uint64_t) w.w_b[0] << 40 |
	       (uint64_t) w.w_b[1] << 32 |
	       (uint64_t) w.w_b[2] << 24 |
	       (uint64_t) w.w_b[3] << 16 |
	       (uint64_t) w.w_b[4] << 8 |
	       (uint64_t) w.w_b[5];
}

static unsigned
left_half(word_t w)
{
	return (unsigned) w.w_b[0] << 16 |
	       (unsigned) w.w_b[1] << 8 |
	       (unsigned) w.w_b[2];
}

static unsigned
right_half(word_t w)
{
	return (unsigned) w.w_b[3] << 16 |
	       (unsigned) w.w_b[4] << 8 |
	       (unsigned) w.w_b[5];
}

static void
sprint_command(char *str, unsigned cmd)
{
	int reg, opcode, addr;

	reg = (cmd >> 20) & 017;
	if (cmd & 02000000) {
		opcode = (cmd >> 12) & 0370;
		addr = cmd & 077777;
	} else {
		opcode = (cmd >> 12) & 077;
		addr = cmd & 07777;
		if (cmd & 01000000)
			addr |= 070000;
	}
	if (opcode & 0200)
		strcpy(str, opname_long_bemsh[(opcode >> 3) & 017]);
	else
		strcpy(str, opname_short_bemsh[opcode]);
	str += strlen(str);
	if (addr) {
		if (addr >= 077700)
			sprintf(str, " -%o", (addr ^ 077777) + 1);
		else
			sprintf(str, " %o", addr);
		str += strlen(str);
	}
	if (reg) {
		if (!addr)
			*str++ = ' ';
		sprintf(str, "(%o)", reg);
	}
}

static void
print_insn_fields(unsigned cmd)
{
	if (cmd & 02000000)
		printf("%02o %02o %05o", (cmd >> 20) & 017,
		    (cmd >> 15) & 037, cmd & 077777);
	else
		printf("%02o %03o %04o", (cmd >> 20) & 017,
		    (cmd >> 12) & 0177, cmd & 07777);
}

static void
print_spaces(const char *str, int width)
{
	for (; *str; ++str)
		if (!(*str & 0x80) || (*str & 0xc0) == 0xc0)
			--width;
	while (width-- > 0)
		putchar(' ');
}

static void
print_gost_bytes(const unsigned char *bytes, int n)
{
	int i;

	for (i = 0; i < n; ++i)
		gost_putc(bytes[i], stdout);
}

static int
is_text_candidate(const unsigned char *bytes, int n)
{
	int i, text = 0;

	for (i = 0; i < n; ++i) {
		unsigned char c = bytes[i];

		if (c == 0)
			continue;
		if (!gost_to_unicode(c))
			return 0;
		if (c != GOST_SPACE)
			text = 1;
	}
	return text;
}

static void
print_text_candidate(word_t w)
{
	if (!is_text_candidate(w.w_b, BPW))
		return;
	printf("  text \"");
	print_gost_bytes(w.w_b, BPW);
	printf("\"");
}

static void
print_user(alureg_t user)
{
	int i;

	for (i = 5; i >= 0; --i)
		putchar('0' + ((user.l >> (4 * i)) & 017));
	putchar(' ');
	for (i = 5; i >= 0; --i)
		putchar('0' + ((user.r >> (4 * i)) & 017));
}

static void
print_passport(const struct passport *psp)
{
	unsigned i;
	int meters = -1;

	if (psp->lprlim <= 0200000 && (0200000 - psp->lprlim) % 236 == 0)
		meters = (0200000 - psp->lprlim) / 236;

	printf("Passport\n");
	printf("  user:      ");
	print_user(psp->user);
	printf("\n");
	printf("  entry:     %05o\n", psp->entry);
	printf("  intercept: %05o\n", psp->intercept);
	printf("  tele:      %u\n", psp->tele);
	printf("  spec:      %u\n", psp->spec);
	printf("  phys:      %o\n", psp->phys);
	if (meters >= 0)
		printf("  lprlim:    %05o (%d meter%s)\n", psp->lprlim,
		    meters, meters == 1 ? "" : "s");
	else
		printf("  lprlim:    %05o\n", psp->lprlim);
	printf("  arr_end:   %u\n", psp->arr_end);
	printf("  volumes:   %u\n", psp->nvol);
	for (i = 0; i < psp->nvol && i < MAXVOL; ++i) {
		const char *mode = (psp->vol[i].wr & VOL_READ_WRITE) ?
		    "write" : "read";

		if (psp->vol[i].volno >= ARFA_ID_BASE)
			printf("    %2u: lun %02o  arfa-id %u  mode %s  offset %o\n",
			    i, psp->vol[i].u, psp->vol[i].volno, mode,
			    psp->vol[i].offset);
		else if (psp->vol[i].wr == VOL_CHUNK)
			printf("    %2u: lun %02o  chunk %u  offset %o\n",
			    i, psp->vol[i].u, psp->vol[i].volno,
			    psp->vol[i].offset);
		else
			printf("    %2u: lun %02o  volume %u  mode %s  offset %o\n",
			    i, psp->vol[i].u, psp->vol[i].volno, mode,
			    psp->vol[i].offset);
	}
}

static int
read_word_at(FILE *f, long pos, struct ibword *ibw)
{
	if (fseek(f, pos, SEEK_SET) < 0)
		return -1;
	if (fread(ibw, sizeof(*ibw), 1, f) != 1)
		return -1;
	return 0;
}

static void
print_code_word(unsigned addr, word_t w)
{
	char left[40], right[40];
	unsigned l = left_half(w);
	unsigned r = right_half(w);

	sprint_command(left, l);
	sprint_command(right, r);
	printf("  %05o  CODE  ", addr);
	print_insn_fields(l);
	printf("  %s", left);
	print_spaces(left, 16);
	printf(" | ");
	print_insn_fields(r);
	printf("  %s\n", right);
}

static void
print_data_word(unsigned addr, word_t w)
{
	uint64_t val = word_value(w);

	printf("  %05o  DATA  %04llo %04llo %04llo %04llo",
	    addr,
	    (unsigned long long) ((val >> 36) & 07777),
	    (unsigned long long) ((val >> 24) & 07777),
	    (unsigned long long) ((val >> 12) & 07777),
	    (unsigned long long) (val & 07777));
	print_text_candidate(w);
	printf("\n");
}

static int
print_startup(FILE *f, long start, long end)
{
	long pos;
	unsigned addr = 0;

	printf("\nStartup Image\n");
	if (start == end) {
		printf("  (empty)\n");
		return 0;
	}
	for (pos = start; pos < end; pos += (long) sizeof(struct ibword)) {
		struct ibword ibw;

		if (read_word_at(f, pos, &ibw) < 0) {
			perror("read");
			return -1;
		}
		switch (ibw.tag) {
		case W_IADDR:
			addr = ibw.w.w_b[4] << 8 | ibw.w.w_b[5];
			printf("  @ %05o\n", addr);
			break;
		case W_CODE:
			print_code_word(addr++, ibw.w);
			break;
		case W_DATA:
			print_data_word(addr++, ibw.w);
			break;
		default:
			printf("  ? tag %u at file offset %ld\n", ibw.tag, pos);
			break;
		}
	}
	return 0;
}

static void
print_card_text(unsigned char card[120])
{
	int end = 120;

	while (end > 0 && (card[end - 1] == GOST_SPACE || card[end - 1] == GOST_0))
		--end;
	printf("\"");
	print_gost_bytes(card, end);
	printf("\"");
}

static int
print_a3(FILE *f, long start, long end)
{
	long pos = start;
	unsigned addr = 0;
	unsigned cardno = 0;

	printf("\nA3 Input Array\n");
	if (start == end) {
		printf("  (empty)\n");
		return 0;
	}
	while (pos < end) {
		struct ibword ibw[24];
		unsigned char card[120];
		int i, j;

		if (read_word_at(f, pos, &ibw[0]) < 0) {
			perror("read");
			return -1;
		}
		if (ibw[0].tag == W_IADDR) {
			addr = ibw[0].w.w_b[4] << 8 | ibw[0].w.w_b[5];
			printf("  @ %05o\n", addr);
			pos += sizeof(struct ibword);
			if (pos == end)
				break;
		}
		if (end - pos < (long) (24 * sizeof(struct ibword))) {
			fprintf(stderr, "ibview: incomplete A3 card at file offset %ld\n",
			    pos);
			return -1;
		}
		for (i = 0; i < 24; ++i) {
			if (read_word_at(f, pos + i * (long) sizeof(struct ibword),
			    &ibw[i]) < 0) {
				perror("read");
				return -1;
			}
			if (ibw[i].tag != W_DATA) {
				fprintf(stderr,
				    "ibview: expected A3 data at file offset %ld, got tag %u\n",
				    pos + i * (long) sizeof(struct ibword), ibw[i].tag);
				return -1;
			}
			for (j = 0; j < 5; ++j)
				card[i * 5 + j] = ibw[i].w.w_b[j + 1] & 0177;
		}
		for (i = 0; i < 24 && word_value(ibw[i].w) == 1; ++i)
			;
		if (i == 24) {
			printf("  card %-4u addr %05o  (A3 terminator)\n",
			    ++cardno, addr);
			pos += 24 * sizeof(struct ibword);
			continue;
		}
		printf("  card %-4u addr %05o  ", ++cardno, addr);
		print_card_text(card);
		printf("\n");
		addr += 24;
		pos += 24 * sizeof(struct ibword);
	}
	return 0;
}

static int
validate_file(long size, const struct passport *psp)
{
	if (size < (long) sizeof(*psp)) {
		fprintf(stderr, "ibview: truncated passport\n");
		return -1;
	}
	if ((size - (long) sizeof(*psp)) % (long) sizeof(struct ibword)) {
		fprintf(stderr, "ibview: input records are not aligned\n");
		return -1;
	}
	if (psp->arr_end < sizeof(*psp) || psp->arr_end > (uint) size ||
	    (psp->arr_end - sizeof(*psp)) % sizeof(struct ibword)) {
		fprintf(stderr, "ibview: invalid arr_end %u for file size %ld\n",
		    psp->arr_end, size);
		return -1;
	}
	if (psp->nvol > MAXVOL) {
		fprintf(stderr, "ibview: invalid volume count %u\n", psp->nvol);
		return -1;
	}
	return 0;
}

int
main(int argc, char **argv)
{
	static struct option longopts[] = {
		{ "help",  0, 0, 'h' },
		{ "latin", 0, 0, 'l' },
		{ "path",  1, 0, 'p' },
		{ 0,       0, 0, 0 },
	};
	const char *queue_dir = NULL;
	char default_dir[MAXPATHLEN], path[MAXPATHLEN];
	unsigned bufno;
	struct passport psp;
	struct stat st;
	FILE *f;
	int opt, status = 1;

	while ((opt = getopt_long(argc, argv, "hlp:", longopts, NULL)) != -1) {
		switch (opt) {
		case 'h':
			usage(stdout);
			return 0;
		case 'l':
			gost_latin = 1;
			break;
		case 'p':
			queue_dir = optarg;
			break;
		default:
			usage(stderr);
			return 1;
		}
	}
	if (argc - optind != 1 || parse_buffer_number(argv[optind], &bufno) < 0) {
		usage(stderr);
		return 1;
	}
	if (!queue_dir) {
		default_queue_dir(default_dir);
		queue_dir = default_dir;
	}
	if (snprintf(path, sizeof(path), "%s/%03o", queue_dir, bufno) >=
	    (int) sizeof(path)) {
		fprintf(stderr, "ibview: path is too long\n");
		return 1;
	}

	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	if (fstat(fileno(f), &st) < 0) {
		perror(path);
		goto out;
	}
	if (fread(&psp, sizeof(psp), 1, f) != 1) {
		perror(path);
		goto out;
	}
	if (validate_file(st.st_size, &psp) < 0)
		goto out;

	printf("%s\n", path);
	print_passport(&psp);
	if (print_startup(f, sizeof(psp), psp.arr_end) < 0)
		goto out;
	if (print_a3(f, psp.arr_end, st.st_size) < 0)
		goto out;

	status = 0;
out:
	fclose(f);
	return status;
}
