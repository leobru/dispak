/*
 * Master/subordinate task (ГЗ/ПЗ) interaction between dispak processes.
 *
 * Each dispak process running with subtask support attaches to a shared
 * memory registry of tasks and occupies one slot; the slot index + 1 is
 * the task's "program channel number".  A task formed with э50 7701 is
 * spawned as a child dispak process given the input queue buffer number.
 * The ГЗ/ПЗ extracodes operate on the slots: a stopped (parked) task
 * publishes its full state in its slot, so the master manipulates it
 * directly; a running task is reached through per-slot inboxes and a
 * SIGUSR1 doorbell.
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
#ifndef tasks_h
#define tasks_h

#include "defs.h"

#define TASK_MAXCHAN    16      /* program channels 1..TASK_MAXCHAN */

/* slot states */
#define TS_FREE         0
#define TS_RUN          1
#define TS_STOP_REQ     2       /* the master asked the task to stop */
#define TS_STOPPED      3       /* parked; slot contents authoritative */
#define TS_END_REQ      4       /* the master asked the task to end */

/* stop causes (причина авоста) */
#define TC_NONE         0
#define TC_BY_MASTER    1       /* остановлена главной */
#define TC_APPEAL       2       /* обратилась к главной */

/* event scale bits (N р. шкалы событий -> 1 << (N-1)) */
#define EVENT_ALARM         (1 << 0)    /* будильник, 1 р. */
#define EVENT_PZ_APPEARED   (1 << 10)   /* появилась/отключилась ПЗ, 11 р. */
#define EVENT_RESOURCE      (1 << 11)   /* доверение ресурса, 12 р. */
#define EVENT_ACCIDENT      (1 << 16)   /* авария, 17 р. */
#define EVENT_INTERCEPT     (1 << 18)   /* перехват экстракода, 19 р. */

/* marks an inbox word as holding a pending value */
#define TASK_IN_VALID   0x80000000

typedef struct {
	volatile int    pid;            /* 0 = free slot */
	volatile uint   shifr_l, shifr_r; /* task code (шифр), 24+24 bits */
	volatile int    state;          /* TS_* */
	volatile int    cause;          /* TC_*, valid while stopped */
	volatile int    master;         /* program channel of the master or 0 */
	volatile int    inpause;        /* the task is sleeping in э50 7700/э53 17 */
	volatile int    cancel_pause;   /* э53 47: cancel pause/alarm */

	/* Event apparatus, mirrored by the owner on every instruction;
	 * authoritative while the task is stopped. */
	volatile uint   events;
	volatile uint   emask;
	volatile uint   ehandler;
	volatile int    eenab;

	/* Inboxes for modifications while the owner is running. */
	volatile uint   events_in;      /* event bits to raise */
	volatile uint   events_clr_in;  /* event bits to clear */
	volatile uint   emask_in;       /* new event mask | TASK_IN_VALID */
	volatile uint   ehandler_in;    /* new decoder address | TASK_IN_VALID */
	volatile int    eenab_in;       /* 0/1, or -1 = no change pending */

	/* CPU context, valid while stopped. */
	ushort  reg[NREGS];
	ushort  pc;
	uchar   right;
	uint    acc_l, acc_r, accex_l, accex_r;

	/* Dataset table for LUN exchange (э53 37), valid while stopped. */
	struct {
		ushort  diskno, offset, mode;
	} disks[NDISKS];

	/* User memory image, valid while stopped. */
	word_t  core[CORESZ];
	uchar   convol[CORESZ];
} task_slot_t;

typedef struct {
	uint            magic;
	volatile int    creator;        /* pid of the registry creator */
	task_slot_t     slot[TASK_MAXCHAN];
} task_reg_t;

#define TASK_REG_MAGIC  0x2b3c4d5e

extern task_reg_t  *task_reg;   /* NULL when subtask support is off */
extern int         task_chan;   /* own program channel, 1-based */
extern char        *task_argv0; /* for spawning subtask processes */

#define task_self()     (&task_reg->slot[task_chan - 1])
#define task_channo(t)  ((int)((t) - task_reg->slot) + 1)

int  task_init(int create);     /* attach/create registry, take a slot */
void task_cleanup(void);        /* release the slot, notify relatives */
int  task_poll(void);           /* run loop hook; E_TERM = asked to end */
int  task_park(int cause);      /* stop self; E_TERM = asked to end */
void task_spawn(int bufno);     /* start a dispak process on an input buffer */
task_slot_t *task_find_pz(void);/* resolve "шифр или № канала ПЗ" on acc */
task_slot_t *task_by_shifr(uint l, uint r);
int  task_stop_pz(task_slot_t *t);      /* 0 when the task has parked */
void task_wake(task_slot_t *t);
void task_kick(task_slot_t *t);         /* ring the doorbell */
void task_raise(task_slot_t *t, uint ev);
void task_clear(task_slot_t *t, uint ev);

#endif  /* tasks_h */
