# OS_Timer — Arquitectura del Sistema de Temporizadores

> **Módulo:** `os/src/OS_Timer.c` · `os/inc/OS_Timer.h`  
> **Dependencias:** `OS_Config.h`, `OS_Types.h`, `OS_Event.h`, `OS_Hsm.h`, `OS_Port.h`

---

## 1. Visión General

OS_Timer es un sistema de temporizadores software basado en una **timing wheel con contador de rondas** (*round-counter hashed timing wheel*). Todas sus operaciones principales son **O(1)**.

```
┌─────────────────────────────────────────────────────────────────┐
│                       RTEF OS — Timer                           │
│                                                                 │
│   ┌──────────┐     ┌────────────────┐     ┌──────────────────┐  │
│   │ Free List│     │  Timing Wheel  │     │  Per-HSM Lists   │  │
│   │ (Pool)   │◄───►│  (Wheel[16])   │◄───►│  (TimerHead)     │  │
│   └──────────┘     └────────────────┘     └──────────────────┘  │
│         ▲                   ▲                       ▲            │
│         │                   │                       │            │
│    OS_TimerInit        OS_SysTick           OS_TimerCreate       │
│    TimerFree           (cada tick)          OS_TimerDelete       │
│                                            OS_TimerDeleteByState│
└─────────────────────────────────────────────────────────────────┘
```

**Características clave:**

| Operación              | Complejidad | Contexto           |
|------------------------|:-----------:|--------------------|
| `OS_TimerCreate`       | O(1)*       | Dispatch HSM       |
| `OS_TimerDelete`       | O(1)        | Dispatch HSM       |
| `OS_SysTick`           | O(1)        | ISR HW timer       |
| `OS_TimerDeleteByState`| O(m)†       | Transición interna |

> \* La verificación de duplicados recorre la lista per-HSM (≤ `OS_TIMER_MAX_PER_HSM`).  
> † *m* = timers del estado actual, acotado por `OS_TIMER_MAX_PER_HSM`.

---

## 2. Estructuras de Datos en Memoria

### 2.1 TimerBlock (bloque de timer)

Cada timer ocupa un slot fijo en el array `Pool[]`. Un `TimerBlock` contiene:

```
┌──────────────────────────────────────────────────────┐
│                    TimerBlock                         │
├────────────┬─────────────────────────────────────────┤
│ Expiry     │ Tick absoluto de expiración (OS_U32)    │
│ Period     │ Recarga periódica (0 = one-shot)        │
│ Round      │ Rotaciones completas antes de disparar   │
│ Signal     │ Señal a enviar al expirar               │
│ Hook       │ Puntero al HSM dueño                    │
│ OwnerState │ Estado que creó el timer                 │
│ Generation │ Contador anti-handle-stale (≥ 1)        │
│ NextFree   │ ───► Siguiente en free list             │
│ NextHsm    │ ◄──► Lista per-HSM (doble enlace)       │
│ PrevHsm    │                                         │
│ NextWheel  │ ◄──► Lista del slot de la wheel (doble) │
│ PrevWheel  │                                         │
│ Active     │ true = timer en curso                    │
└────────────┴─────────────────────────────────────────┘
```

### 2.2 Layout completo en memoria

```
Pool[] — Array estático de OS_TIMER_WHEEL_SIZE (16) bloques
═══════════════════════════════════════════════════════════

  Índice:   [0]       [1]       [2]       [3]      ...     [15]
          ┌───────┐ ┌───────┐ ┌───────┐ ┌───────┐       ┌───────┐
          │Timer  │ │Timer  │ │ FREE  │ │Timer  │  ...  │ FREE  │
          │Active │ │Active │ │       │ │Active │       │       │
          └───────┘ └───────┘ └───────┘ └───────┘       └───────┘

FreeHead ─────────────► [2] ──► [5] ──► [9] ──► ... ──► [15] ──► (-1)
                      (lista simplemente enlazada vía NextFree)


Wheel[] — Array de cabezas de slot (OS_TIMER_WHEEL_SIZE = 16)
═════════════════════════════════════════════════════════════

  Slot:   [0]    [1]    [2]    [3]    [4]   ...   [15]
         ┌────┐ ┌────┐ ┌────┐ ┌────┐ ┌────┐     ┌────┐
         │ -1 │ │  3 │ │ -1 │ │  0 │ │ -1 │ ... │  1 │
         └────┘ └────┘ └────┘ └────┘ └────┘     └────┘
                  │              │                  │
                  ▼              ▼                  ▼
               Pool[3]       Pool[0]            Pool[1]
           (lista doblemente enlazada vía NextWheel/PrevWheel)
```

### 2.3 Las tres listas intrusivas

Cada `TimerBlock` participa simultáneamente en **tres listas** según su estado:

```
                    ┌─────────────────────────────┐
                    │         TimerBlock           │
                    │                              │
      Free List ◄───┤  NextFree ──────────►        │
    (singly-linked) │                              │
                    │  NextWheel ◄────────►        │──── Timing Wheel Slot
      Wheel Slot ◄──┤  PrevWheel           (doubly)│     (un slot de Wheel[])
                    │                              │
                    │  NextHsm  ◄────────►        │──── Per-HSM List
     Per-HSM List ◄─┤  PrevHsm            (doubly)│     (desde HSM.TimerHead)
                    │                              │
                    └─────────────────────────────┘

  ► Un timer ACTIVO está en:  Wheel Slot + Per-HSM List
  ► Un timer LIBRE está en:   Free List (solo)
  ► NUNCA en ambas a la vez
```

---

## 3. Timing Wheel con Round Counter

### 3.1 Concepto

La timing wheel es un array circular de `OS_TIMER_WHEEL_SIZE` (16) slots. Cada tick, el OS inspecciona **un solo slot**. El slot se determina con:

```
slot = TickCounter & OS_TIMER_WHEEL_MASK
```

Para soportar periodos mayores que el tamaño de la wheel, cada timer lleva un **Round counter**: el número de rotaciones completas de la wheel que deben pasar antes de que el timer dispare.

```
                    Timing Wheel (WHEEL_SIZE = 16)
                    ══════════════════════════════

                              TickCounter
                                  │
                                  ▼
       ┌────┬────┬────┬────┬────┬────┬────┬────┐
       │ 0  │ 1  │ 2  │ 3  │ 4  │ 5  │ 6  │ 7  │
       ├────┼────┼────┼────┼────┼────┼────┼────┤
       │ 8  │ 9  │ 10 │ 11 │ 12 │ 13 │ 14 │ 15 │
       └────┴────┴────┴────┴────┴────┴────┴────┘

  Cada slot contiene una lista doblemente enlazada de timers.
  En cada tick, solo se inspecciona UN slot (el apuntado por TickCounter).

  Ejemplo con WHEEL_SIZE = 16:
  ┌────────────────────────────────────────────────┐
  │ Timer A: Expiry = 35, Round = 1                │
  │   → Slot = 35 & 0xF = 3                        │
  │   → Cuando TickCounter llega al slot 3:         │
  │     • Primera vez (tick 3):  Round 1→0          │
  │     • Segunda vez (tick 19): Round 0→ ¡DISPARA! │
  │       (pero realmente se inserta en tick ~20,    │
  │        se visita en tick 35)                     │
  └────────────────────────────────────────────────┘
```

### 3.2 Fórmula del Round

Al insertar un timer en la wheel:

```
delta = Expiry - TickCounter - 1
Round = delta >> OS_TIMER_WHEEL_BITS     (equivale a delta / WHEEL_SIZE)
slot  = Expiry & OS_TIMER_WHEEL_MASK
```

**Verificación con WHEEL_SIZE = 16:**

| `periodTicks` | `delta` (period) | `Round` | `slot`  | Significado                        |
|:-------------:|:-----------------:|:-------:|:-------:|------------------------------------|
| 1             | 0                 | 0       | Tick+1  | Dispara en el siguiente tick       |
| 16            | 15                | 0       | Tick+16 | Dispara al completar la rotación   |
| 17            | 16                | 1       | Tick+17 | Dispara tras 1 rotación completa   |
| 256           | 255               | 15      | Tick+256| Dispara tras 15 rotaciones         |

---

## 4. Ciclo de Vida de un Timer

### 4.1 Creación — `OS_TimerCreate()`

```
  Código del usuario (dentro de un estado HSM):
  ═════════════════════════════════════════════

     handle = OS_TimerCreate(SIG_TIMEOUT, 500, false);
                    │
                    ▼
  ┌─────────────────────────────────────────────────────────┐
  │                   OS_TimerCreate()                       │
  │                                                         │
  │  1. Verificar contexto                                  │
  │     ├── ¿Estamos en dispatch? (Q_ASSERT)                │
  │     ├── ¿HSM válido? (OS_HsmGetCurrent)                 │
  │     └── ¿periodTicks > 0? (Q_ASSERT)                    │
  │                                                         │
  │  ── Port_CriticalEnter() ──────────────────────────     │
  │                                                         │
  │  2. Verificar cuota per-HSM                             │
  │     └── TimerCount < OS_TIMER_MAX_PER_HSM (Q_ASSERT)    │
  │                                                         │
  │  3. Verificar señal duplicada                           │
  │     └── Recorrer lista per-HSM: ¿ya existe signal?      │
  │         (Q_ASSERT si duplicada)                          │
  │                                                         │
  │  4. Asignar bloque del Free List ── O(1)                │
  │     ├── idx = FreeHead                                  │
  │     └── FreeHead = Pool[idx].NextFree                   │
  │                                                         │
  │  5. Inicializar bloque                                  │
  │     ├── Generation++ (skip 0 en wraparound)             │
  │     ├── Expiry = TickCounter + periodTicks              │
  │     ├── Period = periodic ? periodTicks : 0             │
  │     ├── Signal, Hook, OwnerState, Active = true         │
  │     └── Insertar en lista per-HSM (cabeza) ── O(1)      │
  │                                                         │
  │  6. Insertar en Timing Wheel ── O(1)                    │
  │     ├── Calcular Round y Slot                           │
  │     └── WheelInsert(idx)                                │
  │                                                         │
  │  7. Construir handle = {Index, Generation}              │
  │                                                         │
  │  ── Port_CriticalExit() ───────────────────────────     │
  │                                                         │
  │  return handle                                          │
  └─────────────────────────────────────────────────────────┘
```

**Diagrama de estructuras ANTES y DESPUÉS de crear un timer:**

```
  ═══ ANTES ═══                          ═══ DESPUÉS ═══

  FreeHead ──► [2] ──► [5] ──► [9]      FreeHead ──► [5] ──► [9]
                                         (el slot [2] fue asignado)

  Wheel[slot]:  -1                       Wheel[slot]: ──► [2]
                                              Pool[2].Round = R
  HSM_A.TimerHead: ──► [7]               HSM_A.TimerHead: ──► [2] ◄──► [7]
                                              (insertado en cabeza)
```

### 4.2 Expiración — `OS_SysTick()`

El tick ISR del hardware llama a `OS_SysTick()` una vez por tick:

```
  ISR del HW Timer
       │
       ▼
  ┌──────────────────────────────────────────────────────────────┐
  │                      OS_SysTick()                            │
  │                                                              │
  │  ── Port_CriticalEnter() ────────────────────────────        │
  │                                                              │
  │  1. TickCounter++                                            │
  │                                                              │
  │  2. Calcular slot actual                                     │
  │     slot = TickCounter & WHEEL_MASK                          │
  │                                                              │
  │  3. Recorrer lista del slot:                                 │
  │     ┌──────────────────────────────────────────────┐         │
  │     │  Para cada timer en Wheel[slot]:              │         │
  │     │                                              │         │
  │     │   ¿Round > 0?                                │         │
  │     │     │                                        │         │
  │     │     ├── SÍ ──► Round-- (una rotación menos)  │         │
  │     │     │                                        │         │
  │     │     └── NO ──► ¡TIMER EXPIRADO!              │         │
  │     │                  │                            │         │
  │     │                  ├── Encolar evento:          │         │
  │     │                  │   OS_InsertEventFromIsr()  │         │
  │     │                  │                            │         │
  │     │                  ├── ¿Es periódico?           │         │
  │     │                  │    │                       │         │
  │     │                  │    ├── SÍ: Reschedule      │         │
  │     │                  │    │   Expiry += Period    │         │
  │     │                  │    │   WheelRemove + Insert│         │
  │     │                  │    │                       │         │
  │     │                  │    └── NO: TimerFree()     │         │
  │     │                  │        (devolver al pool)  │         │
  │     └──────────────────────────────────────────────┘         │
  │                                                              │
  │  4. OS_WatchdogTick()                                        │
  │                                                              │
  │  ── Port_CriticalExit() ─────────────────────────────        │
  └──────────────────────────────────────────────────────────────┘
```

**Ejemplo visual — Un timer expirando:**

```
  Tick 99:  TickCounter = 99
            slot = 99 & 0xF = 3
            Wheel[3] ──► Timer_A (Round=0, Signal=SIG_TIMEOUT)
                              │
                              ▼
                         Round == 0 → ¡EXPIRADO!
                              │
                    ┌─────────┴─────────┐
                    │                   │
               One-shot             Periódico
                    │                   │
                    ▼                   ▼
              TimerFree()         Expiry += Period
              (vuelve a           WheelRemove()
               Free List)        WheelInsert()
                                 (nuevo slot/round)
                    │                   │
                    └─────────┬─────────┘
                              │
                              ▼
                   OS_InsertEventFromIsr()
                   (señal encolada → se
                    despacha en el main loop)
```

**Ejemplo temporal completo (one-shot, 35 ticks, WHEEL_SIZE=16):**

```
  Tick  0: OS_TimerCreate(SIG, 35, false)
           Expiry = 0 + 35 = 35
           slot   = 35 & 0xF = 3
           Round  = (35-0-1) >> 4 = 34 >> 4 = 2

  Tick  3: OS_SysTick inspecciona slot 3
           Timer en slot 3 → Round = 2 → Round-- → Round = 1

  Tick 19: OS_SysTick inspecciona slot 3 de nuevo (3 + 16 = 19)
           Timer en slot 3 → Round = 1 → Round-- → Round = 0

  Tick 35: OS_SysTick inspecciona slot 3 de nuevo (19 + 16 = 35)
           Timer en slot 3 → Round = 0 → ¡EXPIRADO!
           → Encolar SIG → TimerFree()
```

### 4.3 Borrado Manual — `OS_TimerDelete()`

```
  Código del usuario:
  ═══════════════════

     bool ok = OS_TimerDelete(handle);
                    │
                    ▼
  ┌──────────────────────────────────────────────────────┐
  │                 OS_TimerDelete()                      │
  │                                                      │
  │  1. ¿handle.Index fuera de rango?                    │
  │     └── SÍ → return false (silencioso)               │
  │                                                      │
  │  ── Port_CriticalEnter() ───────────────────         │
  │                                                      │
  │  2. ¿Pool[Index].Active                              │
  │      && Generation coincide?                         │
  │     │                                                │
  │     ├── NO → return false                            │
  │     │        (handle stale: timer ya expiró          │
  │     │         o fue borrado)                         │
  │     │                                                │
  │     └── SÍ → Verificar ownership:                    │
  │              ├── Q_ASSERT: misma HSM                  │
  │              ├── Q_ASSERT: mismo OwnerState           │
  │              └── TimerFree(handle.Index)              │
  │                  return true                         │
  │                                                      │
  │  ── Port_CriticalExit() ────────────────────         │
  └──────────────────────────────────────────────────────┘
```

**Diagrama de la detección de handle stale (Generation):**

```
  Slot [2] vida del campo Generation:
  ═══════════════════════════════════

  OS_TimerInit   → Gen = 0  (slot libre, nunca usado)

  TimerCreate #1 → Gen = 1  ◄── handle_A = {Index:2, Gen:1}
  TimerFree      → (sigue Gen=1, slot vuelve a free list)

  TimerCreate #2 → Gen = 2  ◄── handle_B = {Index:2, Gen:2}

  Si el usuario intenta OS_TimerDelete(handle_A):
     Pool[2].Generation = 2  ≠  handle_A.Generation = 1
     → return false (handle stale detectado)

  ¡Protege contra uso de handles obsoletos!
```

### 4.4 Borrado Automático por Estado — `OS_TimerDeleteByState()`

Cuando un HSM transiciona de estado, el OS automáticamente borra todos los timers creados por el estado que se abandona:

```
  Transición HSM:  Estado_A  ──►  Estado_B
  ═════════════════════════════════════════

     OS_HsmTransition()
           │
           ├── Despachar Q_EXIT a Estado_A
           │
           ├── OS_TimerDeleteByState()    ◄── Borrado automático
           │        │
           │        ▼
           │   ┌───────────────────────────────────────────┐
           │   │  hook  = OS_HsmGetCurrent()                │
           │   │  state = Estado_A                          │
           │   │                                            │
           │   │  Recorrer hook->TimerHead:                 │
           │   │                                            │
           │   │    Timer_1 (OwnerState == Estado_A) → FREE │
           │   │    Timer_2 (OwnerState == Estado_B) → SKIP │
           │   │    Timer_3 (OwnerState == Estado_A) → FREE │
           │   │                                            │
           │   │  Resultado: solo timers de Estado_A        │
           │   │             son eliminados                 │
           │   └───────────────────────────────────────────┘
           │
           ├── Despachar Q_ENTRY a Estado_B
           │
           └── Despachar Q_INIT a Estado_B
               (Estado_B puede crear sus propios timers aquí)
```

---

## 5. Timer Periódico — Rescheduling

Un timer periódico se reinscribe automáticamente al expirar, sin pasar por la free list:

```
  Timer periódico con Period = 100, WHEEL_SIZE = 16
  ══════════════════════════════════════════════════

  Creación (tick 0):
     Expiry = 100
     slot   = 100 & 0xF = 4
     Round  = (100-0-1) >> 4 = 6

  Expiración (tick 100):
     → Encolar señal
     → Reschedule:
        Expiry = 100 + 100 = 200     (NO usa TickCounter → sin drift)
        WheelRemove del slot 4
        slot   = 200 & 0xF = 8
        Round  = (200-100-1) >> 4 = 6
        WheelInsert en slot 8

  Expiración (tick 200):
     → Encolar señal
     → Reschedule:
        Expiry = 200 + 100 = 300
        ...y así sucesivamente

  Timeline:
  ─────┬──────────────┬──────────────┬──────────────┬───
       0             100            200            300
       │              │              │              │
    Create         Expire+        Expire+        Expire+
                  Reschedule     Reschedule     Reschedule

  ★ Expiry += Period (NO TickCounter + Period)
    → Garantiza cero drift acumulado
```

---

## 6. Mecanismos de Protección

### 6.1 Cuota Per-HSM

Cada HSM tiene un campo `TimerCount` que se incrementa al crear y decrementa al liberar. Se verifica contra `OS_TIMER_MAX_PER_HSM` en cada creación:

```
  OS_TIMER_MAX_PER_HSM = 4

  HSM_A: TimerCount = 3  →  OS_TimerCreate() → OK (3 < 4)
  HSM_A: TimerCount = 4  →  OS_TimerCreate() → Q_ASSERT (4 ≮ 4)

  HSM_B: TimerCount = 0  →  OS_TimerCreate() → OK (0 < 4)

  ★ Ningún HSM puede acaparar toda la pool
```

### 6.2 Generation Counter (detección de handles obsoletos)

```
  Pool[idx].Generation:  0 → 1 → 2 → ... → 65534 → 65535 → 1 (skip 0)
                         ▲                                      ▲
                    nunca asignado                         wraparound
                    a un timer activo                      salta el 0

  OS_TIMER_INVALID = {0xFFFF, 0}
  ★ Generation=0 nunca coincide con un timer activo → sentinel seguro
```

### 6.3 Verificaciones en Tiempo de Compilación

```c
_Static_assert(WHEEL_SIZE es potencia de 2)
_Static_assert(WHEEL_SIZE >= 2)
_Static_assert(WHEEL_SIZE <= 32767)             // cabe en OS_I16
_Static_assert(MAX_PER_HSM <= WHEEL_SIZE)
_Static_assert(MAX_PER_HSM >= 1)
_Static_assert(MAX_PER_HSM <= 255)              // cabe en OS_U8
```

---

## 7. Flujo Completo: Del Tick a la HSM

```
  ┌──────────────┐
  │  HW Timer    │ ── cada OS_TICK_PERIOD_MS ms ──►  OS_SysTick()
  │  (ISR)       │                                       │
  └──────────────┘                                       │
                                                         ▼
                                                  TickCounter++
                                                  Inspeccionar slot
                                                  Round-- o expirar
                                                         │
                                              ┌──────────┴──────────┐
                                              │                     │
                                         Round > 0            Round == 0
                                         (esperar)           (¡expirado!)
                                                                    │
                                                                    ▼
                                                    OS_InsertEventFromIsr(signal, hsm)
                                                                    │
                                                          ┌─────────┴─────────┐
                                                          │                   │
                                                     One-shot            Periódico
                                                     TimerFree()        Reschedule
                                                                    │         │
                                                                    └────┬────┘
                                                                         │
                                                                         ▼
                                                              ┌──────────────────┐
                                                              │   Cola de Eventos │
                                                              │   (ring buffer)   │
                                                              └────────┬─────────┘
                                                                       │
                                                            OS_EventDispatch()
                                                           (en el main loop)
                                                                       │
                                                                       ▼
                                                              ┌──────────────────┐
                                                              │   OS_HsmDispatch │
                                                              │   Estado recibe  │
                                                              │   la señal       │
                                                              └──────────────────┘
```

---

## 8. Configuración (`OS_Config.h`)

| Parámetro                | Valor por defecto | Descripción                                  |
|--------------------------|:-----------------:|----------------------------------------------|
| `OS_TIMER_WHEEL_BITS`    | 4                 | Ancho de la wheel en bits                    |
| `OS_TIMER_WHEEL_SIZE`    | 16 (2⁴)           | Slots de la wheel = máx. timers simultáneos  |
| `OS_TIMER_WHEEL_MASK`    | 15 (0xF)          | Bitmask para indexado O(1)                   |
| `OS_TIMER_MAX_PER_HSM`   | 4                 | Cuota de timers por HSM                      |
| `OS_TICK_PERIOD_MS`      | 1                 | Duración de un tick en milisegundos          |

**Escalabilidad:**

```
  WHEEL_BITS = 4  →  16 timers,  16 slots  →    ~64 bytes de Wheel
  WHEEL_BITS = 5  →  32 timers,  32 slots  →   ~128 bytes de Wheel
  WHEEL_BITS = 8  → 256 timers, 256 slots  →  ~1 KB de Wheel

  Rango temporal (a 1ms/tick):
  Periodos de 1 tick a 2³² - 1 ticks ≈ 49.7 días
```

---

## 9. Resumen de la API Pública

```c
/* Inicialización (llamar una vez al arranque) */
void OS_TimerInit(void);

/* Crear timer (solo dentro de dispatch HSM) */
OS_TimerHandle OS_TimerCreate(OS_Signal signal,
                              OS_U32 periodTicks,
                              bool periodic);

/* Borrar timer manualmente (devuelve true si existía) */
bool OS_TimerDelete(OS_TimerHandle handle);

/* Tick del sistema (llamar desde ISR del HW timer) */
void OS_SysTick(void);

/* Consultar tick actual (atómico en todas las plataformas) */
OS_U32 OS_GetTickCount(void);

/* Handle inválido para comparaciones */
#define OS_TIMER_INVALID  ((OS_TimerHandle){ 0xFFFFU, 0U })
```

---

## 10. Diagrama de Estados de un TimerBlock

```
                      OS_TimerInit()
                            │
                            ▼
                    ┌───────────────┐
         ┌────────►│     LIBRE     │◄────────────────────┐
         │         │  (Free List)  │                     │
         │         └───────┬───────┘                     │
         │                 │                             │
         │        OS_TimerCreate()                       │
         │                 │                             │
         │                 ▼                             │
         │         ┌───────────────┐          OS_TimerDelete()
         │         │    ACTIVO     │          TimerFree()
         │         │  (Wheel +     │──────────────────────┘
         │         │   Per-HSM)    │               ▲
         │         └───────┬───────┘               │
         │                 │                       │
         │            OS_SysTick()            OS_TimerDeleteByState()
         │            Round == 0              (transición de estado)
         │                 │
         │        ┌────────┴────────┐
         │        │                 │
         │    One-shot          Periódico
         │        │                 │
         │        ▼                 ▼
         │   TimerFree()     Reschedule
         │        │          (sigue ACTIVO,
         └────────┘           nuevo slot/round)
```
