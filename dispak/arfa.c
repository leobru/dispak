/*
 * АРФА archive system emulation: ЭК 063, СМ = "КЛЮЧАР".
 * See arfa.h for an overview of the storage model.
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
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include "defs.h"
#include "disk.h"
#include "gost10859.h"
#include "encoding.h"
#include "arfa.h"
#include "arfaname.h"

extern int gost_latin;          /* dispak -l: render ambiguous names in Latin */

#define ARFA_MAGIC      0x41524641      /* "ARFA" */
#define ARFA_MAXREC     128
#define ARFA_MAXACL     6
#define ARFA_MAXZONES   01000           /* макс. длина области, зон */
#define ARFA_MAXLUNS    12
#define ARFA_MAXHOLD    16              /* own захваты/заказы tracked */

/* авост numbers (indexes into errtxt.c) */
#define A_NUM_ALIEN     16              /* ЧИС.В ЧУЖ.ЛИСТЕ */
#define A_UNORDERED     33              /* ОБРАЩ.К НЕЗАК.МЛ */
#define A_BAD_INFO      34              /* ОШ.В ИНФ.СЛ.ЭКСТ */
#define A_BAD_LUN       47              /* ЗАПРЕЩ.НАПР.В ЭК */
#define A_BAD_PASSWORD  49              /* НЕВЕРНЫЙ ПАРОЛЬ */

typedef struct {
	uchar   used;
	uchar   is_catalog;
	uchar   group;                  /* 2 bits */
	uchar   kind;                   /* 3 bits */
	uchar   individual;
	uchar   pad;
	ushort  acl_rights;             /* 2 bits each: 0 - все прочие,
					 * 1..6 - the acl[] entries */
	uint    id;                     /* 34-bit identifier */
	uint    owner;                  /* шифр хозяина, 24-bit BCD */
	uint    rpass, wpass;           /* 12 bits, 0 = нет пароля */
	uint    len;                    /* зон; 0 у области-каталога */
	uint    acl[ARFA_MAXACL];       /* шифры допущенных лиц */
	int     excl_pid;               /* монопольный захват */
	int     shared_cnt;             /* совместные захваты */
	int     orders;                 /* активные заказы */
	uchar   name[ARFA_PATHLEN];     /* canonical GOST path */
} arfa_rec_t;

typedef struct {
	uint            magic;
	uint            idseq;
	arfa_rec_t      rec[ARFA_MAXREC];
} arfa_cat_t;

extern uchar    getbyte(ptr *bp);       /* extra.c */

static arfa_cat_t       cat;
static int              cat_fd = -1;
static char             arfa_root[MAXPATHLEN];
char                    *arfa_dir;      /* --arfa-dir override */

/* session state */
static uint     eff_user;               /* 27b: шифр вместо своего, 0 = нет */
static uint     admin_budget;           /* 16b: подтвержденный бюджет */
static uint     lun_id[NDISKS];         /* область id attached to a LUN */
static uint     conf_r[ARFA_MAXHOLD], conf_w[ARFA_MAXHOLD];
static uint     my_excl[ARFA_MAXHOLD];
static uint     my_shared[ARFA_MAXHOLD];

static uint
user_id(uint id)
{
	return (id << 17) | 0x0ee;
}

static uint
internal_id(uint id)
{
	id &= ~0x10000;
	if ((id & 07777) == 0x0ee)
		return id >> 17;
	return id;
}

int
arfa_id_is_user(uint id)
{
	id &= ~0x10000;
	return (id & 07777) == 0x0ee;
}

uint
arfa_lun_id(int lun)
{
	return lun_id[lun] ? user_id(lun_id[lun]) : 0;
}

const char *
arfa_msg(uint n)
{
	static const char *msg[] = {
		"ВЫПОЛНЕНО",
		"ТАКОГО ИМЕНИ НЕТ",
		"НЕВЕРНОЕ ИМЯ ОБЛ.",
		"ТОМ УЖЕ В АРХИВЕ",
		"НЕТ ВИРТ.УСТР-ВА",
		"ОШИБКА В ШИФРЕ",
		"ОБЛАСТЬ УЖЕ ЕСТЬ",
		"НЕТ БЮДЖЕТА",
		"ОБЛАСТЬ ЗАНЯТА",
		"НЕТ МЕСТА В КАТ.",
		"НЕ УСТАНОВЛЕН ТОМ",
		"ВИРТ.УСТР-В > 12",
		"ОШИБКА В ДАТЕ",
		"ДЛИНА ОБЛ.> РАЗР.",
		"ОШ.В НОМЕРЕ ТОМА",
		"БЮДЖЕТ ЗАНЯТ",
		"НАРУШЕНА ИЕРАРХИЯ",
		"НЕТ РЕСУРСОВ",
		"НЕТ МЕСТА НА ТОМЕ",
		"НЕТ ПОЛНОМОЧИЙ",
		"ОБЛАСТЬ / КАТАЛОГ",
		"ЗАПРЕЩ.ГРУППА",
		"ИДЕТ ВТАЛКИВАНИЕ",
		"ОШИБКА ВИДА",
		"БЮДЖЕТ АРХИВА",
		"ПОДТВЕРДИ ПАРОЛЬ",
		"ВИРТ.НОМЕР ЗАНЯТ",
		"ЛИСТ В ОБМЕНЕ",
	};

	if (n >= sizeof(msg) / sizeof(msg[0]) || !msg[n])
		return NULL;
	return msg[n];
}

/*
 * The catalog index file, shared by all dispak processes:
 * every operation is a read-modify-write under an exclusive lock.
 */
static int
cat_lock(void)
{
	char    path[MAXPATHLEN];
	struct flock fl;
	int     n;

	if (!arfa_root[0]) {
		if (arfa_dir)
			strcpy(arfa_root, arfa_dir);
		else {
			disk_local_path(arfa_root);
			strcat(arfa_root, "/arfa");
		}
		mkdir(arfa_root, 0755);
	}
	sprintf(path, "%s/.catalog", arfa_root);
	cat_fd = open(path, O_RDWR | O_CREAT, 0644);
	if (cat_fd < 0) {
		perror(path);
		return -1;
	}
	memset(&fl, 0, sizeof(fl));
	fl.l_type = F_WRLCK;
	fl.l_whence = SEEK_SET;
	while (fcntl(cat_fd, F_SETLKW, &fl) < 0 && errno == EINTR)
		continue;
	n = read(cat_fd, &cat, sizeof(cat));
	if (n < (int) sizeof(cat) || cat.magic != ARFA_MAGIC) {
		memset(&cat, 0, sizeof(cat));
		cat.magic = ARFA_MAGIC;
		cat.idseq = ARFA_ID_BASE;
	}
	if (cat.idseq < ARFA_ID_BASE)
		cat.idseq = ARFA_ID_BASE;
	return 0;
}

static void
cat_unlock(int dirty)
{
	if (cat_fd < 0)
		return;
	if (dirty) {
		lseek(cat_fd, 0, SEEK_SET);
		if (write(cat_fd, &cat, sizeof(cat)) != sizeof(cat))
			perror("arfa catalog");
	}
	close(cat_fd);          /* releases the lock */
	cat_fd = -1;
}

static uint
me(void)
{
	return eff_user ? eff_user : user.l;
}

/* шифры may contain "точки" (0o16 nibbles) matching any digit */
static int
shifr_match(uint a, uint b)
{
	int     i;

	for (i = 0; i < 24; i += 4) {
		uint da = (a >> i) & 017, db = (b >> i) & 017;
		if (da != db && da != 016 && db != 016)
			return 0;
	}
	return 1;
}

/*
 * GOST name <-> file system path is handled by the shared resolver in
 * arfaname.c (arfa_resolve / arfa_render_name), which maps each каталог level
 * to a directory and renders homoglyphs per component (см. arfaname.h).
 */

static arfa_rec_t *
find_by_name(const uchar *gname)
{
	int     i;

	for (i = 0; i < ARFA_MAXREC; ++i)
		if (cat.rec[i].used &&
		    !memcmp(cat.rec[i].name, gname, ARFA_PATHLEN))
			return &cat.rec[i];
	return NULL;
}

static arfa_rec_t *
find_by_name_owner(const uchar *gname, uint owner)
{
	int     i;

	for (i = 0; i < ARFA_MAXREC; ++i)
		if (cat.rec[i].used &&
		    !memcmp(cat.rec[i].name, gname, ARFA_PATHLEN) &&
		    shifr_match(owner, cat.rec[i].owner))
			return &cat.rec[i];
	return NULL;
}

static arfa_rec_t *
find_by_id(uint id)
{
	int     i;

	if (!id)
		return NULL;
	for (i = 0; i < ARFA_MAXREC; ++i)
		if (cat.rec[i].used && cat.rec[i].id == id)
			return &cat.rec[i];
	return NULL;
}

/*
 * Parse an область name per §5.2 into the canonical form: GOST chars,
 * simple names separated by GOST_DOT, terminated by 0377.
 * way = 1..7: "по-старому", n pairs of words (6 + 3 chars, owner шифр
 * in the low half of the second word of the first pair);
 * way = 010: "по-новому" in УПП; way = 014: "по-новому" in МС-80.
 * Returns an М16 answer code.
 */
static int
parse_name(ushort addr, int way, uchar *gname, uint *owner)
{
	int     n = 0, i, j;
	uchar   c;

	*owner = 0;
	memset(gname, 0377, ARFA_PATHLEN);
	if (way >= 1 && way <= 7) {
		alureg_t w1, w2;
		for (i = 0; i < way; ++i) {
			int     last = n;
			uchar   simple[9];
			addr = ADDR(addr + 1);
			LOAD(w1, addr);
			addr = ADDR(addr + 1);
			LOAD(w2, addr);
			simple[0] = w1.l >> 16;
			simple[1] = w1.l >> 8;
			simple[2] = w1.l;
			simple[3] = w1.r >> 16;
			simple[4] = w1.r >> 8;
			simple[5] = w1.r;
			simple[6] = w2.l >> 16;
			simple[7] = w2.l >> 8;
			simple[8] = w2.l;
			if (i == 0)
				*owner = w2.r & 0xffffff;
			else if (w2.r & 0xffffff)
				return ARFA_BAD_NAME;
			if (i)
				gname[n++] = GOST_DOT;
			for (j = 0; j < 9; ++j) {
				c = simple[j];
				if (c == GOST_SPACE || c == 0)
					break;
				gname[n++] = c;
			}
			if (n == last + (i ? 1 : 0))
				return ARFA_BAD_NAME;
		}
	} else if (way == 010 || way == 014) {
		ptr     bp;
		bp.p_w = ADDR(addr + 1);
		bp.p_b = 0;
		for (;;) {
			if (!bp.p_w || n >= ARFA_PATHLEN - 1)
				return ARFA_BAD_NAME;
			c = getbyte(&bp);
			if (way == 010) {
				if (c == 0143)
					continue;
				if (c == GOST_SEMICOLON || c == 0377)
					break;
			} else {
				if (c == 0)
					continue;
				if (c == ';' || c == 012)
					break;
				c = unicode_to_gost(koi7_to_unicode[c & 0177]);
			}
			gname[n++] = c;
		}
		if (!n)
			return ARFA_BAD_NAME;
		/* Optional owner шифр prefix: digits ended by a dot. */
		for (i = 0; i < n && gname[i] <= GOST_9; ++i)
			continue;
		if (i > 0 && i < n && gname[i] == GOST_DOT) {
			for (j = 0; j < i; ++j)
				*owner = *owner << 4 | gname[j];
			n -= i + 1;
			memmove(gname, gname + i + 1, n);
			memset(gname + n, 0377, ARFA_PATHLEN - n);
			if (!n)
				return ARFA_BAD_NAME;
		}
	} else
		return ARFA_BAD_NAME;
	gname[n < ARFA_PATHLEN ? n : ARFA_PATHLEN - 1] = 0377;
	return ARFA_OK;
}

/*
 * Resolve "область, заданная логическим номером, идентификатором или
 * именем" - the common preamble of most КЛЮЧАР subfunctions.
 * On success *out points into cat (which must be locked).
 * Returns 0, an М16 answer code, or the negated авост number.
 */
static int
resolve(alureg_t is, ushort isaddr, arfa_rec_t **out, uint *owner)
{
	int     lun = (is.l >> 12) & 077;
	int     way = is.r & 017;
	uchar   gname[ARFA_PATHLEN];
	uint    ow = 0;
	arfa_rec_t *r;

	*out = NULL;
	if (lun) {
		if (lun < 030 || lun >= 070)
			return -A_BAD_LUN;
		r = find_by_id(lun_id[lun]);
		if (!r)
			return -A_UNORDERED;
	} else if (way == 0) {
		alureg_t w;
		LOAD(w, ADDR(isaddr + 1));
		r = find_by_id(internal_id((w.l & 01777) << 24 | w.r));
		if (!r)
			return ARFA_NO_NAME;
	} else {
		int rc = parse_name(isaddr, way, gname, &ow);
		if (rc)
			return rc;
		r = find_by_name(gname);
		if (!r)
			return ARFA_NO_NAME;
		if (ow && !shifr_match(ow, r->owner))
			return ARFA_BAD_SHIFR;
	}
	if (owner)
		*owner = ow;
	*out = r;
	return 0;
}

static int
in_list(uint *list, uint id)
{
	int     i;

	for (i = 0; i < ARFA_MAXHOLD; ++i)
		if (list[i] == id)
			return 1;
	return 0;
}

static void
list_add(uint *list, uint id)
{
	int     i;

	if (in_list(list, id))
		return;
	for (i = 0; i < ARFA_MAXHOLD; ++i)
		if (!list[i]) {
			list[i] = id;
			return;
		}
}

static void
list_del(uint *list, uint id)
{
	int     i;

	for (i = 0; i < ARFA_MAXHOLD; ++i)
		if (list[i] == id)
			list[i] = 0;
}

/*
 * Access rights of the current task: 0 - none, 1 - read, 2/3 - read/write.
 */
static int
rights_of(arfa_rec_t *r)
{
	uint    who = me();
	int     i;

	if (shifr_match(r->owner, who))
		return 3;
	for (i = 0; i < ARFA_MAXACL; ++i)
		if (r->acl[i] && shifr_match(r->acl[i], who))
			return (r->acl_rights >> (2 * (i + 1))) & 3;
	if (shifr_match(r->owner, 0x999999))
		return (r->acl_rights & 3) | 1;
	return r->acl_rights & 3;
}

int
arfa_lookup_id(const uchar *gname, uint user, uint owner, uint *id)
{
	arfa_rec_t *r;
	uint owners[4];
	int i, nowners;
	int rc = ARFA_OK;

	if (cat_lock() < 0)
		return ARFA_NO_VOLUME;
	if (owner) {
		owners[0] = owner;
		nowners = 1;
	} else {
		owners[0] = user;
		owners[1] = (user & 0xffff00) | 016 << 4 | 016;
		owners[2] = (user & 0xff0000) | 0x0099;
		owners[3] = 0x999999;
		nowners = 4;
	}
	r = NULL;
	for (i = 0; i < nowners && !r; ++i)
		r = find_by_name_owner(gname, owners[i]);
	if (!r)
		rc = ARFA_NO_NAME;
	else if (r->is_catalog)
		rc = ARFA_IS_CATALOG;
	else
		*id = user_id(r->id);
	cat_unlock(0);
	return rc;
}

static int
arfa_attach_region_lun(int lun, arfa_rec_t *r, int write, ushort offset)
{
	char path[ARFA_PATH_MAX];
	void *h;

	if (r->is_catalog)
		return ARFA_IS_CATALOG;
	if (rights_of(r) < (write ? 2 : 1))
		return ARFA_NO_RIGHTS;
	if (!arfa_resolve(arfa_root, r->name, 0, gost_latin, path))
		return ARFA_NO_VOLUME;
	h = disk_open_path(path, write ? DISK_READ_WRITE : DISK_READ_ONLY);
	if (!h)
		return ARFA_NO_VOLUME;
	disks[lun].diskh = h;
	disks[lun].diskno = 0;
	disks[lun].offset = offset;
	disks[lun].mode = write ? DISK_READ_WRITE : DISK_READ_ONLY;
	lun_id[lun] = r->id;
	++r->orders;
	return ARFA_OK;
}

static int
arfa_order_region(arfa_rec_t *r, int ro, int *lunp)
{
	int i, lun = 0, inuse = 0;

	if (r->is_catalog)
		return ARFA_IS_CATALOG;
	if (rights_of(r) < (ro ? 1 : 2))
		return ARFA_NO_RIGHTS;
	/* РМР 48-17 рр.: шкала номеров, 48 р. - номер 30b */
	for (i = 0; i < 040; ++i) {
		int u = 030 + i;
		int bit = i < 24 ? (accex.l >> (23 - i)) & 1
				 : (accex.r >> (23 - (i - 24))) & 1;
		if (disks[u].diskno || disks[u].diskh || lun_id[u])
			++inuse;
		else if (bit && !lun)
			lun = u;
	}
	if (!lun)
		return ARFA_LUN_BUSY;
	if (inuse >= ARFA_MAXLUNS)
		return ARFA_MANY_LUNS;
	*lunp = lun;
	return arfa_attach_region_lun(lun, r, !ro, 0);
}

int
arfa_attach_lun(int lun, uint id, int write, ushort offset)
{
	arfa_rec_t *r;
	int rc = ARFA_OK;

	if (cat_lock() < 0)
		return ARFA_NO_VOLUME;
	r = find_by_id(internal_id(id));
	if (!r)
		rc = ARFA_NO_NAME;
	else
		rc = arfa_attach_region_lun(lun, r, write, offset);
	cat_unlock(rc == ARFA_OK);
	return rc;
}

/* Has this область subregions? */
static int
has_children(arfa_rec_t *r)
{
	int     i, len;

	for (len = 0; r->name[len] != 0377; ++len)
		continue;
	for (i = 0; i < ARFA_MAXREC; ++i) {
		arfa_rec_t *s = &cat.rec[i];
		if (s->used && s != r && !memcmp(s->name, r->name, len) &&
		    s->name[len] == GOST_DOT)
			return 1;
	}
	return 0;
}

static void
store_word(ushort addr, uint l, uint r)
{
	alureg_t t;

	t.l = l & 0xffffff;
	t.r = r & 0xffffff;
	STORE(t, addr);
}

/*
 * Fill two words of a catalog listing: the last simple name of the
 * область "по-старому" (6 + 3 chars) with the owner шифр.
 */
static void
name_2words(arfa_rec_t *r, ushort addr)
{
	const uchar *last = r->name;
	uchar   c[9];
	int     i, j;

	for (i = 0; r->name[i] != 0377; ++i)
		if (r->name[i] == GOST_DOT)
			last = r->name + i + 1;
	memset(c, GOST_SPACE, sizeof(c));
	for (j = 0; j < 9 && last[j] != 0377 && last[j] != GOST_DOT; ++j)
		c[j] = last[j];
	store_word(addr, c[0] << 16 | c[1] << 8 | c[2],
		c[3] << 16 | c[4] << 8 | c[5]);
	store_word(ADDR(addr + 1), c[6] << 16 | c[7] << 8 | c[8], r->owner);
}

/*
 * Release stale захваты of dead processes.
 */
static void
heal(arfa_rec_t *r)
{
	if (r->excl_pid && kill(r->excl_pid, 0) < 0 && errno == ESRCH) {
		r->excl_pid = 0;
	}
}

void
arfa_lun_close(int lun)
{
	arfa_rec_t *r;

	if (!lun_id[lun])
		return;
	if (disks[lun].diskh) {
		disk_close(disks[lun].diskh);
		disks[lun].diskh = 0;
	}
	disks[lun].offset = 0;
	if (cat_lock() == 0) {
		r = find_by_id(lun_id[lun]);
		if (r && r->orders > 0)
			--r->orders;
		cat_unlock(r != NULL);
	}
	lun_id[lun] = 0;
}

void
arfa_cleanup(void)
{
	int     i, dirty = 0;

	for (i = 030; i < 070; ++i)
		if (lun_id[i])
			arfa_lun_close(i);
	if (cat_lock() < 0)
		return;
	for (i = 0; i < ARFA_MAXREC; ++i) {
		arfa_rec_t *r = &cat.rec[i];
		if (!r->used)
			continue;
		if (r->excl_pid == (int) getpid()) {
			r->excl_pid = 0;
			dirty = 1;
		}
		if (in_list(my_shared, r->id) && r->shared_cnt > 0) {
			--r->shared_cnt;
			dirty = 1;
		}
	}
	memset(my_shared, 0, sizeof(my_shared));
	memset(my_excl, 0, sizeof(my_excl));
	cat_unlock(dirty);
}

/*
 * ЭК 063, СМ = "КЛЮЧАР": the ИС word is at М16, extra parameters in РМР
 * (accex).  The answer code is returned on М16; 0 means success.
 */
int
arfa(void)
{
	ushort  isaddr = reg[016];
	alureg_t is;
	arfa_rec_t *r;
	int     code, rc;
	uint    ow;

	LOAD(is, isaddr);
	reg[016] = 0;
	code = is.l >> 18;

	switch (code) {
	case 001: {             /* создание области */
		uchar   gname[ARFA_PATHLEN];
		char    path[ARFA_PATH_MAX];
		uint    group = (accex.r >> 20) & 3;
		uint    kind = (accex.r >> 17) & 7;
		uint    indiv = (accex.r >> 16) & 1;
		uint    len = accex.r & 0xffff;
		int     i;

		rc = parse_name(isaddr, is.r & 017, gname, &ow);
		if (rc) {
			reg[016] = rc;
			return E_SUCCESS;
		}
		if (!ow)
			ow = me();
		if (len > ARFA_MAXZONES) {
			reg[016] = ARFA_TOO_LONG;
			return E_SUCCESS;
		}
		if (cat_lock() < 0)
			return E_UNIMP;
		if (find_by_name(gname)) {
			cat_unlock(0);
			reg[016] = ARFA_EXISTS;
			return E_SUCCESS;
		}
		/* A composite name requires an own область-каталог. */
		for (i = ARFA_PATHLEN - 1; i > 0; --i)
			if (gname[i] == GOST_DOT)
				break;
		if (i > 0) {
			uchar   parent[ARFA_PATHLEN];
			memset(parent, 0377, sizeof(parent));
			memcpy(parent, gname, i);
			r = find_by_name(parent);
			if (!r || !r->is_catalog) {
				cat_unlock(0);
				reg[016] = ARFA_NO_NAME;
				return E_SUCCESS;
			}
			if (!shifr_match(r->owner, me())) {
				cat_unlock(0);
				reg[016] = ARFA_NO_RIGHTS;
				return E_SUCCESS;
			}
		}
		for (r = NULL, i = 0; i < ARFA_MAXREC; ++i)
			if (!cat.rec[i].used) {
				r = &cat.rec[i];
				break;
			}
		if (!r) {
			cat_unlock(0);
			reg[016] = ARFA_NO_SPACE_CAT;
			return E_SUCCESS;
		}
		if (!arfa_resolve(arfa_root, gname, 1, gost_latin, path)) {
			cat_unlock(0);
			reg[016] = ARFA_NO_NAME;
			return E_SUCCESS;
		}
		if (len == 0) {
			if (mkdir(path, 0755) < 0 && errno != EEXIST) {
				cat_unlock(0);
				reg[016] = ARFA_NO_SPACE_VOL;
				return E_SUCCESS;
			}
		} else {
			char    zero[6144];
			void    *h = disk_open_path(path, DISK_CREATE);
			uint    z;
			if (!h) {
				cat_unlock(0);
				reg[016] = ARFA_NO_SPACE_VOL;
				return E_SUCCESS;
			}
			memset(zero, 0, sizeof(zero));
			for (z = 0; z < len; ++z)
				if (disk_writei(h, z, zero, NULL, NULL,
				    DISK_MODE_QUIET) != DISK_IO_OK) {
					disk_close(h);
					unlink(path);
					cat_unlock(0);
					reg[016] = ARFA_NO_SPACE_VOL;
					return E_SUCCESS;
				}
			disk_close(h);
		}
		memset(r, 0, sizeof(*r));
		r->used = 1;
		r->is_catalog = len == 0;
		r->group = group;
		r->kind = kind;
		r->individual = indiv;
		r->id = cat.idseq++;
		r->owner = ow;
		r->len = len;
		memcpy(r->name, gname, ARFA_PATHLEN);
		cat_unlock(1);
		return E_SUCCESS;
	}
	case 002:               /* уничтожение области */
	case 025: {
		char    path[ARFA_PATH_MAX];
		uint    pass = is.l & 07777;

		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		if (!shifr_match(r->owner, me())) {
			cat_unlock(0);
			reg[016] = ARFA_NO_RIGHTS;
			return E_SUCCESS;
		}
		if (r->wpass && !in_list(conf_w, r->id)) {
			if (!pass) {
				cat_unlock(0);
				reg[016] = ARFA_NO_PASSWORD;
				return E_SUCCESS;
			}
			if (pass != r->wpass) {
				cat_unlock(0);
				return A_BAD_PASSWORD;
			}
		}
		heal(r);
		/* Заказы (orders) do not block: unlinking a file that other
		 * processes hold open is safe on Unix. */
		if (r->excl_pid || r->shared_cnt) {
			cat_unlock(0);
			reg[016] = ARFA_BUSY;
			return E_SUCCESS;
		}
		if (r->is_catalog && has_children(r)) {
			cat_unlock(0);
			reg[016] = ARFA_IS_CATALOG;
			return E_SUCCESS;
		}
		if (arfa_resolve(arfa_root, r->name, 0, gost_latin, path)) {
			if (r->is_catalog)
				rmdir(path);
			else
				unlink(path);
		}
		list_del(conf_r, r->id);
		list_del(conf_w, r->id);
		r->used = 0;
		cat_unlock(1);
		return E_SUCCESS;
	}
	case 003:               /* изменение пароля на доступ */
	case 026: {
		uint    old = is.l & 07777;             /* 36-25 рр. */
		uint    new = (is.r >> 12) & 07777;     /* 24-13 рр. */
		int     rd = (is.r >> 11) & 1;

		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		if (!shifr_match(r->owner, me())) {
			cat_unlock(0);
			reg[016] = ARFA_NO_RIGHTS;
			return E_SUCCESS;
		}
		if (r->is_catalog) {
			cat_unlock(0);
			reg[016] = ARFA_IS_CATALOG;
			return E_SUCCESS;
		}
		if ((rd ? r->rpass : r->wpass) != 0) {
			if (!old) {
				cat_unlock(0);
				reg[016] = ARFA_NO_PASSWORD;
				return E_SUCCESS;
			}
			if (old != (rd ? r->rpass : r->wpass)) {
				cat_unlock(0);
				return A_BAD_PASSWORD;
			}
		}
		if (rd)
			r->rpass = new;
		else
			r->wpass = new;
		cat_unlock(1);
		return E_SUCCESS;
	}
	case 004: {             /* задание прав доступа */
		uint    who = accex.l & 0xffffff;
		uint    what = accex.r & 3;
		uint    pass = is.l & 07777;
		int     i, slot;

		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		if (!shifr_match(r->owner, me())) {
			cat_unlock(0);
			reg[016] = ARFA_NO_RIGHTS;
			return E_SUCCESS;
		}
		if (r->is_catalog) {
			cat_unlock(0);
			reg[016] = ARFA_IS_CATALOG;
			return E_SUCCESS;
		}
		if (r->wpass && !in_list(conf_w, r->id)) {
			if (!pass) {
				cat_unlock(0);
				reg[016] = ARFA_NO_PASSWORD;
				return E_SUCCESS;
			}
			if (pass != r->wpass) {
				cat_unlock(0);
				return A_BAD_PASSWORD;
			}
		}
		if (!who) {
			/* права "всех прочих" */
			r->acl_rights = (r->acl_rights & ~3) | what;
			cat_unlock(1);
			return E_SUCCESS;
		}
		slot = -1;
		for (i = 0; i < ARFA_MAXACL; ++i) {
			if (r->acl[i] == who) {
				slot = i;
				break;
			}
			if (slot < 0 && !r->acl[i])
				slot = i;
		}
		if (slot < 0) {
			cat_unlock(0);
			reg[016] = ARFA_NO_SPACE_CAT;
			return E_SUCCESS;
		}
		if (what) {
			r->acl[slot] = who;
			r->acl_rights = (r->acl_rights & ~(3 << (2*(slot+1))))
				| what << (2*(slot+1));
		} else if (r->acl[slot] == who) {
			r->acl[slot] = 0;
			r->acl_rights &= ~(3 << (2*(slot+1)));
		}
		cat_unlock(1);
		return E_SUCCESS;
	}
	case 005: {             /* подтверждение пароля */
		uint    pass = is.l & 07777;
		int     rd = (is.r >> 12) & 1;

		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		if (r->is_catalog) {
			cat_unlock(0);
			reg[016] = ARFA_IS_CATALOG;
			return E_SUCCESS;
		}
		if ((rd ? r->rpass : r->wpass) == 0) {
			/* нет пароля - подтверждать нечего */
			cat_unlock(0);
			return E_SUCCESS;
		}
		if (!pass) {
			cat_unlock(0);
			reg[016] = ARFA_NO_PASSWORD;
			return E_SUCCESS;
		}
		if (pass != (rd ? r->rpass : r->wpass)) {
			cat_unlock(0);
			return A_BAD_PASSWORD;
		}
		list_add(rd ? conf_r : conf_w, r->id);
		cat_unlock(0);
		return E_SUCCESS;
	}
	case 006: {             /* список допущенных лиц */
		ushort  buf = accex.r & 077777;
		uint    w;
		int     i;

		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		if (!shifr_match(r->owner, me())) {
			cat_unlock(0);
			reg[016] = ARFA_NO_RIGHTS;
			return E_SUCCESS;
		}
		if (!buf)
			return A_NUM_ALIEN;
		/* C: 16-15 рр. - все прочие, 12-11 - первый ... 2-1 - шестой */
		w = (r->acl_rights & 3) << 14;
		for (i = 0; i < ARFA_MAXACL; ++i)
			w |= ((r->acl_rights >> (2*(i+1))) & 3) << (10 - 2*i);
		store_word(buf, 0, w);
		for (i = 0; i < 3; ++i)
			store_word(ADDR(buf + 1 + i),
				r->acl[2*i], r->acl[2*i + 1]);
		cat_unlock(0);
		return E_SUCCESS;
	}
	case 007: {             /* сведения об области */
		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		acc.l = r->owner;
		acc.r = (r->rpass ? 1 << 23 : 0) | (r->wpass ? 1 << 22 : 0) |
			(r->group << 17) | (r->individual << 16) |
			(r->len & 0xffff);
		cat_unlock(0);
		return E_SUCCESS;
	}
	case 010:               /* каталог областей */
	case 011: {             /* каталог общих областей */
		int     top = (is.r >> 4) & 1;
		uint    count = (accex.l >> 12) & 07777;
		uint    first = accex.l & 07777;
		ushort  buf = accex.r & 077777;
		uchar   prefix[ARFA_PATHLEN];
		int     plen = -1, i, n = 0;

		memset(prefix, 0377, sizeof(prefix));
		if (!count || !first)
			return A_BAD_INFO;
		if (!buf)
			return A_NUM_ALIEN;
		if (cat_lock() < 0)
			return E_UNIMP;
		if (code == 010 && !top) {
			uint    dummy;
			rc = resolve(is, isaddr, &r, &dummy);
			if (rc) {
				cat_unlock(0);
				if (rc < 0)
					return -rc;
				reg[016] = rc;
				return E_SUCCESS;
			}
			if (!r->is_catalog) {
				cat_unlock(0);
				reg[016] = ARFA_IS_CATALOG;
				return E_SUCCESS;
			}
			if (!shifr_match(r->owner, me())) {
				cat_unlock(0);
				reg[016] = ARFA_NO_RIGHTS;
				return E_SUCCESS;
			}
			memcpy(prefix, r->name, ARFA_PATHLEN);
			for (plen = 0; prefix[plen] != 0377; ++plen)
				continue;
		}
		for (i = 0; i < ARFA_MAXREC && count; ++i) {
			arfa_rec_t *s = &cat.rec[i];
			int     j, sl = 0;
			if (!s->used)
				continue;
			for (j = 0; s->name[j] != 0377; ++j)
				if (s->name[j] == GOST_DOT)
					++sl;
			if (code == 011) {
				if (sl || !shifr_match(s->owner, 0x999999))
					continue;
			} else
			if (plen < 0) {
				/* области верхнего уровня данного хозяина */
				if (sl || !shifr_match(s->owner, me()))
					continue;
			} else {
				if (memcmp(s->name, prefix, plen) ||
				    s->name[plen] != GOST_DOT)
					continue;
				for (j = plen + 1; s->name[j] != 0377; ++j)
					if (s->name[j] == GOST_DOT)
						break;
				if (s->name[j] != 0377)
					continue;   /* deeper than one level */
			}
			if (++n < (int) first)
				continue;
			name_2words(s, buf);
			buf = ADDR(buf + 2);
			--count;
		}
		store_word(buf, 077777777, 077777777);
		cat_unlock(0);
		return E_SUCCESS;
	}
	case 012: {             /* дозаказ области */
		int     ro = (is.r >> 11) & 1;
		int     lun;

		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		rc = arfa_order_region(r, ro, &lun);
		if (rc) {
			cat_unlock(0);
			reg[016] = rc;
			return E_SUCCESS;
		}
		acc.l = 0;
		acc.r = lun;
		cat_unlock(1);
		return E_SUCCESS;
	}
	case 013: {             /* имя и идентификатор области */
		ushort  buf = accex.r & 077777;
		uint    id;
		int     i;

		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		id = user_id(r->id);
		acc.l = (id >> 24) & 01777;
		acc.r = id & 0xffffff;
		if (buf) {
			uchar   c[6];
			int     n = 0;
			for (i = 0; ; ++i) {
				c[n++] = r->name[i];
				if (n == 6) {
					store_word(buf,
					    c[0] << 16 | c[1] << 8 | c[2],
					    c[3] << 16 | c[4] << 8 | c[5]);
					buf = ADDR(buf + 1);
					n = 0;
				}
				if (r->name[i] == 0377)
					break;
			}
			if (n) {
				while (n < 6)
					c[n++] = 0143;
				store_word(buf,
				    c[0] << 16 | c[1] << 8 | c[2],
				    c[3] << 16 | c[4] << 8 | c[5]);
			}
		}
		cat_unlock(0);
		return E_SUCCESS;
	}
	case 014:               /* установка сдвига по области */
	case 015: {             /* запрос величины сдвига */
		int     lun;

		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		if (r->is_catalog) {
			cat_unlock(0);
			reg[016] = ARFA_IS_CATALOG;
			return E_SUCCESS;
		}
		cat_unlock(0);
		lun = (is.l >> 12) & 077;
		if (!lun) {
			int u;
			for (u = 030; u < 070; ++u)
				if (lun_id[u] == r->id) {
					lun = u;
					break;
				}
			if (!lun)
				return A_UNORDERED;
		}
		if (code == 014) {
			int neg = (is.r >> 20) & 1;     /* 21 р. */
			int val = (is.r >> 4) & 0xffff; /* 20-5 рр. */
			disks[lun].offset = neg ? -val : val;
		} else {
			short off = disks[lun].offset;
			acc.l = 0;
			acc.r = off < 0 ? 1 << 16 | -off : off;
		}
		return E_SUCCESS;
	}
	case 016:               /* подтверждение прав администратора */
		admin_budget = accex.l & 0xffffff;
		eff_user = 0;
		acc.l = user.l;
		acc.r = admin_budget;
		return E_SUCCESS;
	case 017:               /* изменение шифра и ключа администратора */
	case 020:               /* создание нового бюджета */
	case 021:               /* выделение ресурсов бюджету */
	case 022:               /* установка шкалы доступных групп */
	case 023:               /* уничтожение бюджета */
		/* Budgets are not really maintained: one implicit
		 * unlimited budget covers everything. */
		return E_SUCCESS;
	case 024: {             /* запрос сведений о бюджете */
		ushort  buf = accex.r & 077777;
		uint    shifr = accex.l & 0xffffff;

		if (!buf)
			return A_NUM_ALIEN;
		store_word(buf, shifr, 017 << 20);      /* все группы */
		store_word(ADDR(buf + 1), user.l, 0);
		store_word(ADDR(buf + 2), 0, 0xffff);   /* резидентный */
		store_word(ADDR(buf + 3), 0, 0xffff);   /* нерезидентный */
		store_word(ADDR(buf + 4), 0, 0xffff);   /* ленточный */
		store_word(ADDR(buf + 5), 077777777, 077777777);
		return E_SUCCESS;
	}
	case 027:               /* режим работы от имени подчиненного */
		if (accex.l == 0)
			/* запрос ранее установленного шифра */;
		else if (accex.l == 1)
			eff_user = 0;   /* восстановление истинного шифра */
		else
			eff_user = accex.l & 0xffffff;
		acc.l = me();
		acc.r = admin_budget;
		return E_SUCCESS;
	case 030: {             /* захват области */
		int     excl = (is.r >> 23) & 1;        /* 24 р. */
		int     wait = (is.r >> 22) & 1;        /* 23 р. */

		for (;;) {
			if (cat_lock() < 0)
				return E_UNIMP;
			rc = resolve(is, isaddr, &r, &ow);
			if (rc) {
				cat_unlock(0);
				if (rc < 0)
					return -rc;
				reg[016] = rc;
				return E_SUCCESS;
			}
			if (r->is_catalog) {
				cat_unlock(0);
				reg[016] = ARFA_IS_CATALOG;
				return E_SUCCESS;
			}
			if (!rights_of(r)) {
				cat_unlock(0);
				reg[016] = ARFA_NO_RIGHTS;
				return E_SUCCESS;
			}
			heal(r);
			if (excl ? (!r->excl_pid && !r->shared_cnt) :
				   !r->excl_pid) {
				if (excl) {
					r->excl_pid = getpid();
					list_add(my_excl, r->id);
				} else {
					++r->shared_cnt;
					list_add(my_shared, r->id);
				}
				cat_unlock(1);
				return E_SUCCESS;
			}
			cat_unlock(0);
			if (!wait) {
				reg[016] = ARFA_BUSY;
				return E_SUCCESS;
			}
			usleep(50000);
		}
	}
	case 031: {             /* освобождение области */
		if (cat_lock() < 0)
			return E_UNIMP;
		rc = resolve(is, isaddr, &r, &ow);
		if (rc) {
			cat_unlock(0);
			if (rc < 0)
				return -rc;
			reg[016] = rc;
			return E_SUCCESS;
		}
		if (r->excl_pid == (int) getpid()) {
			r->excl_pid = 0;
			list_del(my_excl, r->id);
		} else if (in_list(my_shared, r->id) && r->shared_cnt > 0) {
			--r->shared_cnt;
			list_del(my_shared, r->id);
		}
		cat_unlock(1);
		return E_SUCCESS;
	}
	case 065: {             /* reading main archive volume (undocumented) */
		void    *h;
		int     page, zone;

		h = disk_open(2248, DISK_READ_ONLY);
		if (!h) {
			fprintf(stderr, "No archive volume 2248\n");
			return E_UNIMP;
		}
		page = (is.l >> 6) & 037;
		zone = is.r & 07777;
		rc = disk_readi(h, zone, (char*) (core + page*02000),
			(char*) convol + page*02000, NULL, DISK_MODE_QUIET);
		disk_close(h);
		return rc == DISK_IO_OK ? E_SUCCESS : E_DISKERR;
	}
	default:
                fprintf(stderr, "Unimplemented КЛЮЧАР code %02o\n", code);
		return E_TERM;
	}
}
