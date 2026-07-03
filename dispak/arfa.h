/*
 * АРФА archive system emulation: ЭК 063, СМ = "КЛЮЧАР".
 *
 * Области (archive regions) are separate Unix files under ~/.besm6/arfa/,
 * named by their GOST names converted to UTF-8; области-каталоги are
 * directories, so a composite name is simply a nested path.  Metadata
 * with no file-system representation (owner, identifier, passwords,
 * access lists, захват holders) lives in the fcntl-locked index file
 * ~/.besm6/arfa/.catalog shared by all dispak processes.
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
#ifndef arfa_h
#define arfa_h

#include "defs.h"

#define ARFA_PATHLEN    96      /* canonical GOST path, 0377-ended */
#define ARFA_ID_BASE    10000   /* distinguishes region ids from disk numbers */

/*
 * Answer codes on М16, numbered in the order of arfamsg.txt.
 */
#define ARFA_OK             0   /* выполнено */
#define ARFA_NO_NAME        1   /* такого имени нет */
#define ARFA_BAD_NAME       2   /* неверное имя обл. */
#define ARFA_VOL_ARCHIVED   3   /* том уже в архиве */
#define ARFA_NO_VIRT_DEV    4   /* нет вирт.устр-ва */
#define ARFA_BAD_SHIFR      5   /* ошибка в шифре */
#define ARFA_EXISTS         6   /* область уже есть */
#define ARFA_NO_BUDGET      7   /* нет бюджета */
#define ARFA_BUSY           8   /* область занята */
#define ARFA_NO_SPACE_CAT   9   /* нет места в кат. */
#define ARFA_NO_VOLUME      10  /* не установлен том */
#define ARFA_MANY_LUNS      11  /* вирт.устр-в > 12 */
#define ARFA_BAD_DATE       12  /* ошибка в дате */
#define ARFA_TOO_LONG       13  /* длина обл.> разр. */
#define ARFA_BAD_VOLNO      14  /* ош.в номере тома */
#define ARFA_BUDGET_BUSY    15  /* бюджет занят */
#define ARFA_BAD_HIERARCHY  16  /* нарушена иерархия */
#define ARFA_NO_RES         17  /* нет ресурсов */
#define ARFA_NO_SPACE_VOL   18  /* нет места на томе */
#define ARFA_NO_RIGHTS      19  /* нет полномочий */
#define ARFA_IS_CATALOG     20  /* область / каталог */
#define ARFA_BAD_GROUP      21  /* запрещ.группа */
#define ARFA_MAINTENANCE    22  /* идет вталкивание */
#define ARFA_BAD_KIND       23  /* ошибка вида */
#define ARFA_ARCHIVE_BUDGET 24  /* бюджет архива */
#define ARFA_NO_PASSWORD    25  /* подтверди пароль */
#define ARFA_LUN_BUSY       26  /* вирт.номер занят */
#define ARFA_LIST_EXCHANGE  27  /* лист в обмене */
#define ARFA_MAX_MSG        27

extern char *arfa_dir;          /* --arfa-dir override, NULL = ~/.besm6/arfa */

int  arfa(void);                /* ЭК 063 КЛЮЧАР dispatch */
const char *arfa_msg(uint n);   /* КЛЮЧАР answer text by answer code */
void arfa_lun_close(int lun);   /* release an область-attached LUN */
void arfa_cleanup(void);        /* drop захваты held by this process */
int  arfa_lookup_id(const uchar *gname, uint user, uint owner, uint *id);
int  arfa_attach_lun(int lun, uint id, int write, ushort offset);

#endif  /* arfa_h */
