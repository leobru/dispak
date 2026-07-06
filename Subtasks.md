# Subordinate tasks (ГЗ/ПЗ) in dispak

ОС ДИСПАК allowed a running task (главная задача, ГЗ) to form other tasks
(подчинённые задачи, ПЗ), stop and restart them, exchange memory and datasets
with them, receive events from them, and read their output streams.  A dialog
monitor system is the typical client of this apparatus: it forms a subordinate
task per user request and supervises its execution.

dispak emulates one BESM-6 task per Unix process.  The subordinate-task
apparatus is therefore implemented with *parallel dispak processes* that
cooperate through a shared-memory task registry.  The implementation lives in
`dispak/tasks.c` and `dispak/tasks.h`; the extracode handlers are in
`dispak/extra.c`.

## The `--subtasks` option

`--subtasks` enables the apparatus for a dispak invocation:

    dispak --subtasks monitor.b6

Without the flag (and outside a subtask process, see below) `task_reg == NULL`,
no registry is created, and every ПЗ extracode returns its documented denial
code ("нет ПЗ в решении", "задача не является подчинённой", etc.); э50 7701
still forms input-queue entries, but no process is launched for them.  This is
exercised by `tests/pz-basic`.

With the flag, `task_init()` (called from `main()` after the input buffer is
loaded):

1. creates the registry file `~/.besm6/task_registry.<pid>` sized to
   `task_reg_t` and mmaps it shared;
2. exports its path in the `DISPAK_TASK_REG` environment variable, which child
   dispak processes inherit;
3. occupies a free slot: slot index + 1 is the task's *program channel number*
   (программный канал), 1..057 (`TASK_MAXCHAN`).  Formed subtasks are placed
   in invented channels starting at 041 octal; these nonzero channel numbers
   are accepted wherever an `Аисп` channel operand is allowed.

A spawned subtask process finds `DISPAK_TASK_REG` set, so it *attaches* to the
inherited registry instead of creating one — only the topmost invocation needs
`--subtasks`.  The registry creator unlinks the file when it exits.

## When and how parallel dispak processes are launched

The only spawn point is extracode **э50 7701** (формирование задачи,
`exform()` in `extra.c`):

1. The requesting task supplies deck text (from memory, or loaded from a
   drum/disk).  In editing mode the deck must be preceded by a "ТКН" control
   word (`TKH000` in `iobuf.h`).  `vsinput()` parses the passport and the
   information array exactly as it does for a task file given on the command
   line, and writes an input-queue buffer `~/.besm6/input_queue/NNN`
   (NNN octal, 001..177).
2. The buffer (catalog) number NNN is returned to the caller in М16.
3. If the task registry exists, `task_spawn(NNN)` forks and execs

       dispak -x --output-raw=pzNNN.raw NNN

   with stdin redirected to `/dev/null` and stdout to `pzNNN.out`.  A numeric
   command-line argument tells dispak to run directly from the input queue,
   skipping the passport-file parsing step.

The child process loads the passport and image with `input()`, attaches to the
registry via `task_init()`, and runs the task like any other.  Its raw print
stream is dumped to `pzNNN.raw` when it terminates; `main()` deliberately calls
`task_cleanup()` *after* `pout_dump()`, so the master sees the slot freed only
once the stream file is complete, and э62 41 (чтение потока вывода) can then
serve zones from that file.

Note that the ГЗ/ПЗ relation is *not* established by spawning.  A freshly
spawned task is initially independent; it becomes subordinate when the linkage
is declared (see below).

## The task registry

`task_reg_t` (in `tasks.h`) is an array of 16 `task_slot_t` slots.  Each slot
holds:

- identification: owner `pid`, шифр (`shifr_l/shifr_r`, BCD), input-catalog
  number `catno`;
- coordination state: `state` (`TS_RUN`, `TS_STOP_REQ`, `TS_STOPPED`,
  `TS_END_REQ`), stop `cause`, `master` (program channel of the главная, or 0),
  pause/terminal/incognito flags;
- the event apparatus mirror and inboxes (below);
- the full CPU context, dataset (LUN) table, and user core image — valid while
  the task is parked.

Inter-process signalling uses three signals, installed in `task_init()`:

- `SIGUSR1` — the doorbell: "look at your slot".  Installed with `SA_RESTART`
  so it does not break terminal reads or disk I/O; `pause()`/`sigsuspend()` are
  still interrupted, which stop requests and э53 47 rely on.
- `SIGUSR2` — installed *without* `SA_RESTART`; its only purpose is to break a
  blocking terminal read (э62 102, сброс терминала с ввода).
- `SIGCHLD` — reaps dead children; if one died without releasing its slot the
  parent frees it and raises the "появилась/отключилась ПЗ" event on itself.

### Run-loop integration

At the top of the instruction loop (`run()` in `cu.c`) every dispak process
with a registry calls `task_poll()` before each instruction.  It:

- consumes the inboxes: event bits raised or cleared by other tasks
  (`events_in`, `events_clr_in`), replacement event mask/decoder
  address/enable flag (э53 34/42/43 executed by the master while the ПЗ was
  running), "Инкогнито" (э62 44), pause cancellation (э53 47), and terminal
  ownership changes (э62 46 rewires stdin/stdout to `/dev/tty` and back);
- honours `TS_STOP_REQ` by parking and `TS_END_REQ` by terminating;
- mirrors the live event apparatus (`events`, `emask`, `ehandler`, `eenab`)
  into the slot so the master can query it cheaply.

### Parking (остановленная ПЗ)

`task_park()` publishes the complete task state in the slot — registers, pc,
accumulator, the LUN table, the user core image and its convolution tags — sets
`state = TS_STOPPED` and sleeps in `sigsuspend()` until the state changes.
While a task is stopped its slot contents are authoritative: the master reads
and *modifies* them directly (э53 35 exchanges pages, э53 37 passes datasets,
э53 36 reads the event scale, э62 77 plants an авост).  On wake-up the task
reloads everything from the slot, so the master's edits take effect.

`task_stop_pz()` implements the master's side of э53 30: it flips
`TS_RUN → TS_STOP_REQ`, rings the doorbell and waits (up to 5 s) for the park
to complete.  `task_wake()` (э53 31) sets `TS_RUN` and rings the doorbell.

## Master/subordinate linkage

The relation is declared by the subordinate, or transferred by the master:

- **э53 25** — declare the task with the шифр on the accumulator as own
  главная; raises "появилась ПЗ" in it.
- **э53 46** — the same, then appeal (fall through to э53 27).
- **э53 27** — appeal to the главная: raise the event in the master and park
  with cause "обратилась к главной".
- **э53 26** — detach from the главная (notifies it).
- **э62 72** — the master passes a stopped ПЗ to a new master or releases it.
- **ГЛА passport option** — static declaration at task start (see below).

A stopped-by-master or appealing task reports авост cause 67 «ЗАДАЧА
УПРЯТАНА» (э62 54).

## Event apparatus

Every task has an event scale (шкала событий) `events`, a mask `emask`, a
decoder address `ehandler` and an enable flag `eenab` (э53 11/12/13/14/16/20/21
manage the own apparatus).  When an enabled, unmasked event is raised, the run
loop transfers control to the decoder with the scale on the accumulator.

Bits used by the subordinate-task apparatus (N р. шкалы = `1 << (N-1)`):

| bit | р. | meaning |
|-----|----|---------|
| `EVENT_ALARM` (1<<0) | 1 | будильник (э50 7700, э53 17) |
| `EVENT_PZ_APPEARED` (1<<10) | 11 | появилась/отключилась ПЗ |
| `EVENT_RESOURCE` (1<<11) | 12 | доверение ресурса (э53 35/37) |
| `EVENT_ACCIDENT` (1<<16) | 17 | авария |
| `EVENT_INTERCEPT` (1<<18) | 19 | перехват экстракода |

`task_raise()`/`task_clear()` modify another task's scale: directly in the
slot if it is stopped, through the `events_in`/`events_clr_in` inboxes plus a
doorbell if it is running.

## Extracode inventory

Own apparatus (э53): 10 time, 11 decoder address, 12 mask, 13/14
disable/enable, 15 restore state, 16 read scale, 17 wait, 20 read mask,
21 declare/clear events.

Master's view of a ПЗ (э53, "Аисп" = шифр or nonzero channel number on the
accumulator): 24 read ПЗ mask, 30 stop, 31 start, 32 declare/clear events in
ПЗ, 33 end ПЗ, 34 set ПЗ mask, 35 exchange memory pages, 36 read ПЗ scale,
37 pass a dataset, 40/42 enable/disable async processes in ПЗ, 43 set ПЗ
decoder address, 47 cancel pause/alarm in ПЗ.

Linkage (э53): 25 declare master, 26 detach, 27 appeal, 46 declare + appeal.

э50: 206b channel number by шифр, 207b channel number of own master,
7700 alarm, 7701 form task.

э62: 41 read a ПЗ output stream zone, 44 cancel output stream (Инкогнито),
46 transfer terminal(s), 54 авост cause of a ПЗ, 61 шифры of own ПЗ
(returning the MSB plus their channel scale in the low half: channel 041 is
0100000), 63 hide area, 64 event mask of ПЗ-for-ГЗ,
72 transfer ПЗ to a new master, 77 raise авост in self or a stopped ПЗ,
101 scale of stopped ПЗ, 102 reset a terminal from input.

## The ГЛА passport option

    шифр 419900 зс5
    гла 419999
    ...

`ГЛА <шифр>` (главная) names the main task in the passport of the subordinate
one.  When such a task reaches the *first instruction of its user code* (the
first instruction executed out of supervisor mode — nothing of the user
program has run yet, the published pc is the entry point):

1. the task with the given шифр is looked up in the registry and becomes the
   master of this task, as if э53 25 had been executed;
2. the task parks with cause "остановлена главной", exactly like after э53 30:
   it shows in the stopped-ПЗ scale (э62 101), reports авост cause 67 (э62 54),
   and its published state may be inspected and modified;
3. the event "появилась ПЗ" (11 р., `EVENT_PZ_APPEARED` = 1<<10) is raised in
   the master — only after the state is published, so a master reacting to the
   event immediately finds the ПЗ already stopped.

The master typically waits for the event (э53 17), prepares the ПЗ — plants
registers or core words, transfers a terminal (э62 46), sets its event mask
(э53 34) — and starts it with э53 31.

The шифр is 6 or 12 decimal digits, like the task's own шифр.  If no task with
that шифр exists in the registry, or the run has no registry at all (no
`--subtasks`), the option is ignored and the task runs normally, which keeps
formed decks testable standalone.

The parsing is in `vsinput.c` (`scan()`), the stop itself in `task_poll()`
(`tasks.c`), armed by `input()` from the passport.  End-to-end test:
`tests/pz-gla`.
