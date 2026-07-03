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

/*
 * Answer codes on М16.  The documentation (extracodes.md §5.3.156-180)
 * names them only mnemonically; the numbering below is sequential in
 * the order of first appearance.
 */
#define ARFA_OK             0
#define ARFA_BAD_NAME       1   /* неверное имя обл. */
#define ARFA_BAD_SHIFR      2   /* ошибка в шифре */
#define ARFA_NO_NAME        3   /* такого имени нет */
#define ARFA_EXISTS         4   /* область уже есть */
#define ARFA_TOO_LONG       5   /* длина обл.> разр. */
#define ARFA_BAD_KIND       6   /* ошибка вида */
#define ARFA_NO_BUDGET      7   /* нет бюджета */
#define ARFA_BUDGET_BUSY    8   /* бюджет занят */
#define ARFA_BAD_GROUP      9   /* запрещ.группа */
#define ARFA_NO_RES         10  /* нет ресурсов */
#define ARFA_NO_SPACE_VOL   11  /* нет места на томе */
#define ARFA_NO_SPACE_CAT   12  /* нет места в кат. */
#define ARFA_NO_RIGHTS      13  /* нет полномочий */
#define ARFA_BUSY           14  /* область занята */
#define ARFA_MAINTENANCE    15  /* служебные работы */
#define ARFA_IS_CATALOG     16  /* область / каталог */
#define ARFA_NO_PASSWORD    17  /* не задан пароль */
#define ARFA_LUN_BUSY       18  /* вирт.номер занят */
#define ARFA_NO_VOLUME      19  /* не установлен том */
#define ARFA_MANY_LUNS      20  /* вирт.устр-в > 12 */

extern char *arfa_dir;          /* --arfa-dir override, NULL = ~/.besm6/arfa */

int  arfa(void);                /* ЭК 063 КЛЮЧАР dispatch */
void arfa_lun_close(int lun);   /* release an область-attached LUN */
void arfa_cleanup(void);        /* drop захваты held by this process */

#endif  /* arfa_h */
