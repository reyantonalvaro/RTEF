# RTEF OS — Real-Time Event Framework

A portable, event-driven operating system kernel written in C11 for
embedded and desktop targets.  Every public function executes in **O(1)**
bounded time (static memory, no `%` operator, power-of-two masks).

---

## Repository layout

```
os/
  inc/            Generic, immutable OS headers
    OS_Config.h   Compile-time constants (queue size, pool size …)
    OS_Types.h    Shared types, signals, state-handler typedef
    OS_Port.h     Hardware-abstraction interface (to be implemented per port)
    OS_Error.h    Q_ASSERT macros and fatal-error handler
    OS_Event.h    Fixed-size ring-buffer event queue
    OS_Timer.h    Software-timer pool, SysTick, watchdog
    OS_Hsm.h      Hierarchical State Machine engine
  src/            Generic, immutable OS sources
    OS_Error.c
    OS_Event.c
    OS_Timer.c
    OS_Hsm.c
port/
  linux/          Linux (POSIX) port
    OS_Port_Linux.c   Port implementation (pthread mutex + tick thread)
    main.c            Traffic-light demo application
build.ninja       Build file (clang + ninja)
```

---

## Concepts

### Signals

| Signal    | Value | Purpose                                  |
|-----------|-------|------------------------------------------|
| `Q_EMPTY` | 0     | Unused / sentinel                        |
| `Q_INIT`  | 1     | Tell a state to initialise its children  |
| `Q_ENTRY` | 2     | State-entry action                       |
| `Q_EXIT`  | 3     | State-exit action                        |
| `Q_USER`  | 4     | First user-definable signal              |

### HSM (Hierarchical State Machine)

* Allocate an `OS_Hsm` variable **statically**.  Its address is the
  unique **hook** (ID) of the machine.
* Each state is a function with signature:
  ```c
  OS_Status MyState(OS_Hsm *const me, OS_Event const *const e);
  ```
  Inside, use a `switch (e->Signal)` with cases for `Q_ENTRY`, `Q_EXIT`,
  `Q_INIT` and your user signals.  Return `OS_HANDLED` or `OS_UNHANDLED`
  (unhandled events bubble up to the parent state).
* `OS_HsmInit(me, topState)` — register and start the machine.
* `OS_HsmChildInit(me, childState)` — call from `Q_INIT` to set a child.
* `OS_HsmTransition(me, target)` — transition to a **sibling** state.

### Events

* `OS_InsertEvent(signal, hook)` — post a signal to an HSM.
  During dispatch an HSM can only post to **itself** (protection).
* `OS_EventDispatch()` — dequeue and deliver one event (call in
  the main super-loop).

### Timers

* `OS_TimerCreate(me, signal, periodMs, periodic)` — start a timer that
  posts `signal` when it expires.  Returns an `OS_TimerHandle`.
* `OS_TimerDelete(handle)` — cancel a timer (only the creating state may
  delete it).
* Timers are **automatically deleted** when their owning state exits
  (`OS_TimerDeleteByState` is called internally during transitions).
* `OS_SysTick()` — 1 ms tick handler; called from the port's ISR/thread.

### Watchdog

* `OS_WatchdogInit(timeoutMs)` — start the software watchdog.
* `OS_WatchdogFeed()` — reset the counter.
* If the counter reaches zero, `Q_ASSERT` fires and the system halts.
  The port may also drive a hardware watchdog.

### Error handling

* `Q_ASSERT(expr)` — if `expr` is false the system logs the file, line,
  and description, then halts.
* `Q_ASSERT_ID(id, expr)` — same, with a numeric error identifier.

---

## Building and running

### Prerequisites

| Tool   | Minimum | Notes              |
|--------|---------|--------------------|
| clang  | 14      | C11 support        |
| ninja  | 1.10    |                    |
| POSIX  | —       | pthread for Linux  |

### Targets

```bash
ninja              # Compile (default target)
ninja run          # Run the Linux demo
ninja clean        # Remove all build artefacts
ninja all          # Clean + Compile + Run in one step
```

### Example output

```
=== RTEF OS – Traffic Light Demo ===

[HSM] ENTRY  Operating
[HSM] ENTRY  Red    🔴
[HSM] Red -> Green
[HSM] EXIT   Red
[HSM] ENTRY  Green  🟢
[HSM] Green -> Yellow
[HSM] EXIT   Green
[HSM] ENTRY  Yellow 🟡
[HSM] Yellow -> Red
[HSM] EXIT   Yellow
[HSM] ENTRY  Red    🔴
...
=== Demo finished (3500 ms) ===
```

---

## Adding a new port

1. Create `port/<target>/OS_Port_<Target>.c` implementing every function
   declared in `os/inc/OS_Port.h`.
2. Create `port/<target>/main.c` (or your application entry point).
3. Add matching `cc_obj`, `link`, `compile_<target>`, `run_<target>`
   stanzas in `build.ninja`.

The files under `os/` must **never** be modified per-platform.

---

## Coding conventions

| Item          | Rule                                                     |
|---------------|----------------------------------------------------------|
| Naming        | `CamelCase` for types and functions, `UPPER_CASE` macros |
| Documentation | Doxygen (`/** … */`)                                     |
| Memory        | 100 % static — no `malloc`/`free`                        |
| Modulo        | Replaced by power-of-two bitmask (`& MASK`)              |
| Complexity    | Every function is O(1) (bounded by compile-time constant)|
| Standard      | C11 (`-std=c11`), MISRA-oriented                         |

---

## License

See [LICENSE](LICENSE).


## Compilación con Ninja

```bash
# Compilar
ninja

# Ejecutar
ninja run

# Limpiar artefactos de compilación
ninja clean

# Todo de una (clean + compile + run)
ninja all
```