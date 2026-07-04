/*
 * Shared task registry for master/subordinate task (ГЗ/ПЗ) support.
 * See tasks.h for an overview.
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
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include "defs.h"
#include "disk.h"
#include "tasks.h"

task_reg_t      *task_reg;
int             task_chan;
char            *task_argv0;
uint            task_gla_l, task_gla_r;

static char     regpath[MAXPATHLEN];
static int      is_subtask;     /* attached to an inherited registry */
static task_slot_t *park_notify; /* raise "появилась ПЗ" here only after
				  * the own state is published (ГЛА) */
static volatile sig_atomic_t    doorbell;
static volatile sig_atomic_t    childexit;

static void
usr1_handler(int sig)
{
	doorbell = 1;
}

static void
chld_handler(int sig)
{
	childexit = 1;
	doorbell = 1;
}

/*
 * SIGUSR2 is installed without SA_RESTART: its only purpose is to break
 * a blocking terminal read (э62 102, terminal transfer).
 */
static void
usr2_handler(int sig)
{
	doorbell = 1;
}

/*
 * SA_RESTART keeps the doorbell from breaking terminal reads and disk I/O;
 * pause() and the sleep functions are still interrupted, which is what
 * stop requests and э53 47 (pause cancellation) rely upon.
 */
static void
install(int sig, void (*fn)(int))
{
	struct sigaction sa;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fn;
	sa.sa_flags = sig == SIGUSR2 ? 0 : SA_RESTART;
	sigemptyset(&sa.sa_mask);
	sigaction(sig, &sa, NULL);
}

int
task_init(int create, int catno)
{
	char    *env = getenv("DISPAK_TASK_REG");
	task_slot_t *t;
	int     fd, i;

	if (env) {
		is_subtask = 1;
		strcpy(regpath, env);
		fd = open(regpath, O_RDWR);
		if (fd < 0) {
			perror(regpath);
			return -1;
		}
	} else if (create) {
		disk_local_path(regpath);
		sprintf(regpath + strlen(regpath), "/task_registry.%d",
			(int) getpid());
		fd = open(regpath, O_RDWR | O_CREAT | O_TRUNC, 0600);
		if (fd < 0 || ftruncate(fd, sizeof(task_reg_t)) < 0) {
			perror(regpath);
			if (fd >= 0)
				close(fd);
			return -1;
		}
		setenv("DISPAK_TASK_REG", regpath, 1);
	} else
		return 0;       /* subtask support not requested */

	task_reg = (task_reg_t*) mmap(NULL, sizeof(task_reg_t),
		PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (task_reg == MAP_FAILED) {
		perror("task registry mmap");
		task_reg = NULL;
		return -1;
	}
	if (!env) {
		task_reg->magic = TASK_REG_MAGIC;
		task_reg->creator = getpid();
	} else if (task_reg->magic != TASK_REG_MAGIC) {
		fprintf(stderr, "%s: bad task registry\n", regpath);
		munmap((void*) task_reg, sizeof(task_reg_t));
		task_reg = NULL;
		return -1;
	}

	/* Take a program channel. */
	for (i = 0; i < TASK_MAXCHAN; ++i)
		if (__sync_bool_compare_and_swap(&task_reg->slot[i].pid,
		    0, (int) getpid()))
			break;
	if (i == TASK_MAXCHAN) {
		fprintf(stderr, "dispak: no free program channels\n");
		munmap((void*) task_reg, sizeof(task_reg_t));
		task_reg = NULL;
		return -1;
	}
	task_chan = i + 1;
	t = task_self();
	t->shifr_l = user.l;
	t->shifr_r = user.r;
	t->catno = catno;
	t->cause = TC_NONE;
	t->master = 0;
	t->inpause = t->cancel_pause = 0;
	t->hide = 0;
	t->pz_emask = 0;
	/* The topmost TELE task starts owning the terminal;
	 * subtasks receive it with э62 46. */
	t->tty = env ? 0 : !notty;
	t->tty_read = t->tty_revoke = 0;
	t->incog_in = 0;
	t->events_in = t->events_clr_in = 0;
	t->emask_in = t->ehandler_in = 0;
	t->eenab_in = -1;
	t->events = events;
	t->emask = emask;
	t->ehandler = ehandler;
	t->eenab = eenab;
	t->state = TS_RUN;

	install(SIGUSR1, usr1_handler);
	install(SIGUSR2, usr2_handler);
	install(SIGCHLD, chld_handler);
	return 0;
}

void
task_cleanup(void)
{
	task_slot_t     *t;
	int             i;

	if (!task_reg)
		return;
	t = task_self();
	/* Ask own subordinate tasks to end. */
	for (i = 0; i < TASK_MAXCHAN; ++i) {
		task_slot_t *s = &task_reg->slot[i];
		if (s->pid && s->master == task_chan) {
			s->master = 0;
			s->state = TS_END_REQ;
			task_kick(s);
		}
	}
	/* Tell the master we are gone. */
	if (t->master) {
		task_slot_t *m = &task_reg->slot[t->master - 1];
		if (m->pid)
			task_raise(m, EVENT_PZ_APPEARED);
	}
	t->pid = 0;
	t->state = TS_FREE;
	if (task_reg->creator == getpid())
		unlink(regpath);
	munmap((void*) task_reg, sizeof(task_reg_t));
	task_reg = NULL;
}

void
task_kick(task_slot_t *t)
{
	int     pid = t->pid;

	if (pid == (int) getpid())
		doorbell = 1;
	else if (pid)
		kill(pid, SIGUSR1);
}

void
task_raise(task_slot_t *t, uint ev)
{
	ev &= 0xffffff;
	if (t->state == TS_STOPPED)
		__atomic_fetch_or(&t->events, ev, __ATOMIC_SEQ_CST);
	else
		__atomic_fetch_or(&t->events_in, ev, __ATOMIC_SEQ_CST);
	task_kick(t);
}

void
task_clear(task_slot_t *t, uint ev)
{
	ev &= 0xffffff;
	__atomic_fetch_and(&t->events, ~ev, __ATOMIC_SEQ_CST);
	__atomic_fetch_and(&t->events_in, ~ev, __ATOMIC_SEQ_CST);
	if (t->state != TS_STOPPED) {
		__atomic_fetch_or(&t->events_clr_in, ev, __ATOMIC_SEQ_CST);
		task_kick(t);
	}
}

static void
task_reap(void)
{
	int     pid, i;

	while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
		if (!task_reg)
			continue;
		for (i = 0; i < TASK_MAXCHAN; ++i) {
			task_slot_t *t = &task_reg->slot[i];
			if (t->pid == pid) {
				/* Died without releasing the slot. */
				t->pid = 0;
				t->state = TS_FREE;
			}
		}
		(void) eraise(EVENT_PZ_APPEARED);
	}
}

/*
 * A spawned subtask runs with stdin on /dev/null and stdout on a file.
 * When the master transfers the terminal to it (э62 46), rewire the
 * standard descriptors to the controlling terminal, and back again when
 * the terminal is taken away.  The topmost task keeps its own descriptors.
 */
static void
task_tty_update(task_slot_t *t)
{
	static int      owned, saved_in = -1, saved_out = -1;
	int             fd;

	if (!is_subtask || t->tty == owned)
		return;
	fflush(stdout);
	if (t->tty) {
		fd = open("/dev/tty", O_RDWR);
		if (fd < 0)
			return; /* no controlling terminal */
		if (saved_in < 0)
			saved_in = dup(0);
		if (saved_out < 0)
			saved_out = dup(1);
		dup2(fd, 0);
		dup2(fd, 1);
		close(fd);
		clearerr(stdin);
		notty = 0;
	} else {
		if (saved_in >= 0)
			dup2(saved_in, 0);
		if (saved_out >= 0)
			dup2(saved_out, 1);
		clearerr(stdin);
		notty = 1;
	}
	owned = t->tty;
}

int
task_poll(void)
{
	task_slot_t     *t = task_self();
	uint            w;
	int             e;

	if (childexit) {
		childexit = 0;
		task_reap();
	}
	if (doorbell) {
		doorbell = 0;
		task_tty_update(t);
		if (t->incog_in) {
			t->incog_in = 0;
			pout_enable = 0;
		}
		if (t->cancel_pause) {
			struct itimerval itv = {{0, 0}, {0, 0}};
			setitimer(ITIMER_REAL, &itv, NULL);
			t->cancel_pause = 0;
		}
		w = __atomic_exchange_n(&t->emask_in, 0, __ATOMIC_SEQ_CST);
		if (w & TASK_IN_VALID)
			emask = w & 0xffffff;
		w = __atomic_exchange_n(&t->ehandler_in, 0, __ATOMIC_SEQ_CST);
		if (w & TASK_IN_VALID)
			ehandler = ADDR(w);
		e = __atomic_exchange_n(&t->eenab_in, -1, __ATOMIC_SEQ_CST);
		if (e >= 0)
			eenab = e;
		w = __atomic_exchange_n(&t->events_clr_in, 0, __ATOMIC_SEQ_CST);
		events &= ~w;
		w = __atomic_exchange_n(&t->events_in, 0, __ATOMIC_SEQ_CST);
		if (w)
			(void) eraise(w);
	}
	switch (t->state) {
	case TS_STOP_REQ:
		return task_park(TC_BY_MASTER);
	case TS_END_REQ:
		return E_TERM;
	}
	if ((task_gla_l | task_gla_r) && !supmode) {
		/* ГЛА: at the first user-code instruction link to the
		 * master and park; ignored when no task with the given
		 * шифр is around. */
		task_slot_t *m = task_by_shifr(task_gla_l, task_gla_r);

		task_gla_l = task_gla_r = 0;
		if (m && m != t) {
			t->master = task_channo(m);
			park_notify = m;
			return task_park(TC_BY_MASTER);
		}
	}
	/* Mirror the event apparatus for the master to query. */
	t->events = events;
	t->emask = emask;
	t->ehandler = ehandler;
	t->eenab = eenab;
	return 0;
}

/*
 * Stop the current task: publish the full state in the own slot and wait
 * until the master starts (or ends) us.  The master is free to modify the
 * published state, so reload everything on wakeup.
 */
int
task_park(int cause)
{
	task_slot_t     *t = task_self();
	sigset_t        block, old;
	int             i;

	for (i = 0; i < NREGS; ++i)
		t->reg[i] = reg[i];
	t->pc = pc;
	t->right = right;
	t->acc_l = acc.l;
	t->acc_r = acc.r;
	t->accex_l = accex.l;
	t->accex_r = accex.r;
	for (i = 0; i < NDISKS; ++i) {
		t->disks[i].diskno = disks[i].diskno;
		t->disks[i].offset = disks[i].offset;
		t->disks[i].mode = disks[i].mode;
	}
	memcpy(t->core, core, sizeof(t->core));
	memcpy((void*) t->convol, convol, sizeof(t->convol));
	t->events = events;
	t->emask = emask;
	t->ehandler = ehandler;
	t->eenab = eenab;
	t->cause = cause;
	t->state = TS_STOPPED;
	if (park_notify) {
		task_raise(park_notify, EVENT_PZ_APPEARED);
		park_notify = NULL;
	}

	sigemptyset(&block);
	sigaddset(&block, SIGUSR1);
	sigaddset(&block, SIGCHLD);
	sigprocmask(SIG_BLOCK, &block, &old);
	while (t->state == TS_STOPPED)
		sigsuspend(&old);
	sigprocmask(SIG_SETMASK, &old, NULL);

	if (t->state == TS_END_REQ)
		return E_TERM;

	memcpy(core, t->core, sizeof(t->core));
	memcpy(convol, (void*) t->convol, sizeof(t->convol));
	for (i = 0; i < CORESZ; ++i)
		cflags[i] &= ~C_UNPACKED;
	for (i = 0; i < NREGS; ++i)
		reg[i] = t->reg[i];
	reg[0] = 0;
	pc = t->pc;
	right = t->right;
	acc.l = t->acc_l;
	acc.r = t->acc_r;
	accex.l = t->accex_l;
	accex.r = t->accex_r;
	for (i = 0; i < NDISKS; ++i) {
		if (disks[i].diskno != t->disks[i].diskno) {
			/* The LUN was given away or received. */
			if (disks[i].diskh && disks[i].diskno)
				disk_close(disks[i].diskh);
			disks[i].diskh = 0;
			disks[i].diskno = t->disks[i].diskno;
		}
		disks[i].offset = t->disks[i].offset;
		disks[i].mode = t->disks[i].mode;
	}
	events = t->events;
	emask = t->emask;
	ehandler = t->ehandler;
	eenab = t->eenab;
	/* Consume any inboxes filled while we were stopped. */
	doorbell = 1;
	return 0;
}

task_slot_t *
task_by_shifr(uint l, uint r)
{
	int     i;

	if (!task_reg)
		return NULL;
	for (i = 0; i < TASK_MAXCHAN; ++i) {
		task_slot_t *t = &task_reg->slot[i];
		if (t->pid && t->shifr_l == l && t->shifr_r == r)
			return t;
	}
	return NULL;
}

/*
 * Resolve "шифр или номер программного канала ПЗ" given on the accumulator.
 * The task must be a subordinate of the current one.
 */
task_slot_t *
task_find_pz(void)
{
	task_slot_t     *t;

	if (!task_reg)
		return NULL;
	if (acc.l == 0 && acc.r > 0 && acc.r <= TASK_MAXCHAN)
		t = &task_reg->slot[acc.r - 1];
	else
		t = task_by_shifr(acc.l, acc.r);
	if (!t || !t->pid || t->master != task_chan)
		return NULL;
	return t;
}

/*
 * Find an own subtask by its number in the input catalog.
 */
task_slot_t *
task_by_catno(int catno)
{
	int     i;

	if (!task_reg || !catno)
		return NULL;
	for (i = 0; i < TASK_MAXCHAN; ++i) {
		task_slot_t *t = &task_reg->slot[i];
		if (t->pid && t->catno == catno && t->master == task_chan)
			return t;
	}
	return NULL;
}

/*
 * Consume the "terminal reset from input" flag (э62 102) after an
 * interrupted terminal read.
 */
int
task_tty_revoked(void)
{
	task_slot_t     *t;

	if (!task_reg)
		return 0;
	t = task_self();
	if (!t->tty_revoke)
		return 0;
	t->tty_revoke = 0;
	return 1;
}

int
task_stop_pz(task_slot_t *t)
{
	int     i;

	if (t->state == TS_STOPPED)
		return 0;
	__sync_bool_compare_and_swap(&t->state, TS_RUN, TS_STOP_REQ);
	task_kick(t);
	for (i = 0; i < 5000; ++i) {
		if (t->state == TS_STOPPED)
			return 0;
		if (!t->pid)
			return -1;
		usleep(1000);
	}
	return -1;
}

void
task_wake(task_slot_t *t)
{
	t->cause = TC_NONE;
	t->state = TS_RUN;
	task_kick(t);
}

/*
 * Run a freshly formed task as a subordinate-task process:
 * dispak -x --output-raw=pzNNN.raw <input buffer number>, batch mode,
 * with the terminal output going to pzNNN.out.  The raw print stream
 * file is dumped when the subtask ends and serves э62 41 requests.
 */
void
task_spawn(int bufno)
{
	char    arg[16], outname[32], rawopt[48];
	int     pid, fd;

	if (!task_reg || !task_argv0)
		return;
	sprintf(arg, "%03o", bufno);
	sprintf(rawopt, "--output-raw=pz%03o.raw", bufno);
	fflush(stdout);
	fflush(stderr);
	pid = fork();
	if (pid < 0) {
		perror("fork");
		return;
	}
	if (pid)
		return;
	fd = open("/dev/null", O_RDONLY);
	if (fd >= 0) {
		dup2(fd, 0);
		close(fd);
	}
	sprintf(outname, "pz%03o.out", bufno);
	fd = open(outname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd >= 0) {
		dup2(fd, 1);
		close(fd);
	}
	execlp(task_argv0, task_argv0, "-x", rawopt, arg, (char*) 0);
	perror(task_argv0);
	_exit(1);
}
