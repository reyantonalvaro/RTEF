# Análisis de Funcionamiento: OS_Timer

**Módulo:** `os/src/OS_Timer.c` · `os/inc/OS_Timer.h` · `os/src/OS_Timer_Private.h`  
**Configuración:** `os/inc/OS_Config.h`  
**Dependencias:** `OS_Event`, `OS_Hsm`, `OS_Watchdog`, `OS_Port`, `OS_Error`

---

## 1. Resumen de Funcionamiento

OS_Timer implementa temporizadores software sobre una **timing wheel con contadores de ronda** (round-counter hashed timing wheel). La arquitectura se compone de:

| Componente | Descripción |
|---|---|
| **Pool[WHEEL_SIZE]** | Array estático de `TimerBlock`. Sin `malloc`. |
| **Free list** | Lista simplemente enlazada (`NextFree`) para asignar/liberar bloques O(1). |
| **Wheel[WHEEL_SIZE]** | Array de cabezas de listas doblemente enlazadas — los "slots" de la rueda. |
| **Per-HSM list** | Lista doblemente enlazada (`NextHsm`/`PrevHsm`) por cada HSM, anclada en `OS_Hsm.TimerHead`. |
| **Round counter** | Entero que cuenta rotaciones completas de la rueda antes de disparar. |
| **Generation** | Contador de generación por slot para detectar handles obsoletos. |

### Flujo de vida de un timer

1. **Creación** (`OS_TimerCreate`): se toma un bloque de la free list, se calcula `Expiry = TickCounter + periodTicks`, se calcula el Round, se inserta en el slot de la wheel correspondiente y en la lista per-HSM.
2. **Tick** (`OS_SysTick`): cada tick se inspecciona un solo slot (`TickCounter & MASK`). Si `Round > 0`, se decrementa. Si `Round == 0`, el timer expiró: se encola la señal y se reschedula (periódico) o se libera (one-shot).
3. **Borrado manual** (`OS_TimerDelete`): se valida el handle vía Generation, se desenlaza de wheel y per-HSM, se devuelve a la free list.
4. **Auto-borrado** (`OS_TimerDeleteByState`): al transicionar de estado, la HSM borra automáticamente todos los timers creados por el estado que sale.

---

## 2. ¿Es la Forma Más Eficiente?

### 2.1 Lo que está bien hecho

**Timing wheel con Round counter — diseño excelente para este contexto.**

La timing wheel es la estructura clásica óptima para temporizadores software en sistemas embebidos. Frente a las alternativas:

| Estructura | Insert | Delete | Tick (peor caso) | RAM | Adecuada aquí? |
|---|:---:|:---:|:---:|:---:|:---:|
| **Lista ordenada** | O(n) | O(1) | O(1) | Baja | ❌ Insert lento |
| **Min-heap** | O(log n) | O(log n) | O(log n) | Baja | ❌ Innecesario |
| **Timing wheel simple** | O(1) | O(1) | O(1) amort. | Media | ⚠️ Limitado a un rango |
| **Timing wheel + Round** ✅ | O(1) | O(1) | O(1) amort. | Media | ✅ Óptima |
| **Hierarchical wheel** | O(1) | O(1) | O(1) amort. | Alta | ❌ Sobreingeniería para 16 timers |

Con solo 16 timers máximos simultáneos (`WHEEL_BITS=4`), una rueda jerárquica o un min-heap serían sobreingeniería. La timing wheel + Round es la solución óptima: **O(1) en todo excepto `OS_TimerDeleteByState`** (que es O(m) acotado por `OS_TIMER_MAX_PER_HSM = 4`, así que efectivamente O(1)).

**Pool estática sin malloc — correcto para embebido.**

Sin fragmentación de heap, determinista, 100% predecible en uso de RAM.

**Secciones críticas mínimas.**

`OS_SysTick` solo encola eventos bajo sección crítica; nunca despacha. El dispatch ocurre fuera de ISR en `OS_EventDispatch()`, reduciendo la latencia de interrupción.

**Free list O(1).**

El patrón LIFO con `FreeHead` + `NextFree` es la forma más eficiente de reciclar bloques de un pool estático.

**Round counter — solución elegante para periodos largos.**

La fórmula `Round = (Expiry - TickCounter - 1) >> WHEEL_BITS` permite periodos de hasta 2³²−1 ticks (~49.7 días a 1 ms/tick) sin aritmética signada y sin overflow.

**Reschedule sin drift.**

Los timers periódicos usan `Expiry += Period` (no `TickCounter + Period`), lo cual elimina drift acumulado. Esto es correcto y frecuentemente se hace mal en otros RTOS.

### 2.2 Margen de mejora en eficiencia

**a) Pool y Wheel tienen el mismo tamaño — innecesario.**

`Pool[OS_TIMER_WHEEL_SIZE]` tiene 16 bloques y `Wheel[OS_TIMER_WHEEL_SIZE]` tiene 16 slots. Esto significa que el tamaño de la wheel es igual al de la pool, lo que es atípico. Normalmente `WHEEL_SIZE >= POOL_SIZE` para reducir colisiones por slot, pero tener `WHEEL_SIZE == POOL_SIZE` no aporta beneficio extra: si la wheel tiene 16 slots y el pool tiene 16 timers, la probabilidad de colisión por slot es la misma que si tuvieras menos slots.

Sin embargo, desacoplarlos tampoco ahorra casi nada (solo el array `Wheel[]` de `16 × sizeof(OS_I16) = 32 bytes`). **En un sistema de 16 timers esto es irrelevante.** Decisión razonable.

**b) `TimerBlock` tiene 13 campos — 44+ bytes por timer.**

Cada `TimerBlock` contiene 4 punteros de lista (NextFree, NextHsm, PrevHsm, NextWheel, PrevWheel) más datos. Un timer muerto en la free list solo necesita `NextFree`, pero los otros campos siguen ocupando RAM. Esto es inevitable con un pool estático y está bien.

**c) Duplicate-signal check es O(m) en la lista per-HSM.**

Cuando se crea un timer, se recorre la lista per-HSM completa para verificar que no haya señal duplicada. Con `OS_TIMER_MAX_PER_HSM = 4`, esto es como máximo 4 iteraciones — trivial. Si el límite per-HSM fuera mucho mayor, podría considerarse un bitmap de señales activas, pero para 4 no tiene sentido.

### 2.3 Veredicto de eficiencia

**Sí, es la forma más eficiente para este tamaño de pool y contexto embebido.** No hay estructura de datos con mejor rendimiento asintótico que resulte práctica para 16 timers en un microcontrolador. La implementación no tiene operaciones innecesarias ni desperdicio de memoria significativo.

---

## 3. Errores Encontrados

### 3.1 🔴 Sección crítica en `OS_SysTick` tiene duración no acotada por diseño

**Ubicación:** `OS_Timer.c:385-428`

El comentario dice "O(1) — one slot per tick", pero el `while (cur >= 0)` recorre **todos los timers en un slot**. Con `WHEEL_SIZE = POOL_SIZE = 16`, en el peor caso los 16 timers caen en el mismo slot (si todos tienen el mismo `Expiry & MASK`). Dentro de cada iteración se llama a `OS_InsertEventFromIsr` (que hace Q_ASSERT + writes), `WheelRemove` + `WheelInsert` (periódico), o `TimerFree` (one-shot). Todo esto bajo `Port_CriticalEnter()`.

**Impacto:** Con 16 timers en un slot, la sección crítica es 16× más larga que el caso típico. En un MCU a 48 MHz esto podría ser ~10-20 µs (aceptable), pero con WHEEL_SIZE grande (ej: 256) podría ser problemático.

**Matiz:** Dado que `WHEEL_SIZE = 16` y el `OS_TIMER_MAX_PER_HSM = 4`, el peor caso real es que 4 HSMs tengan 4 timers cada uno colisionando en el mismo slot — 16 timers. Esto es acotado por configuración, así que **es O(1) con constante acotada por el pool size**. No es un bug, pero la documentación "O(1) — one slot per tick" es optimista; sería más preciso decir "O(k) donde k ≤ WHEEL_SIZE".

**Severidad:** Baja. Es un problema de documentación más que de lógica.

### 3.2 🟡 `OS_TimerDeleteByState` recorre timers de TODOS los estados del HSM

**Ubicación:** `OS_Timer.c:354-376`

```c
cur = hook->TimerHead;
while (cur >= 0) {
    next = Pool[(OS_U16)cur].NextHsm;
    if (Pool[(OS_U16)cur].OwnerState == state) {
        TimerFree((OS_U16)cur);
    }
    cur = next;
}
```

El bucle recorre **toda la lista per-HSM**, no solo los timers del estado que sale. Si el HSM tiene 4 timers pero solo 1 pertenece al estado actual, se visitan los 4. Con `MAX_PER_HSM = 4` esto es trivial, pero conceptualmente es subóptimo.

**Alternativa:** Una lista per-estado (en lugar de per-HSM) permitiría borrar solo los timers del estado sin recorrer los de otros estados. Pero esto requeriría un puntero extra en `TimerBlock` y otro `TimerHead` por estado. Con 4 timers máximos, **no vale la pena**.

**Severidad:** Ninguna. Es correcto y eficiente para el tamaño actual.

### 3.3 🟡 `TimerFree` limpia 12 campos — excesivo para seguridad en release

**Ubicación:** `OS_Timer.c:179-211`

Después de liberar un timer, `TimerFree` escribe ceros en todos los campos: `Active`, `Hook`, `OwnerState`, `Expiry`, `Period`, `Round`, `Signal`, `NextHsm`, `PrevHsm`, `NextWheel`, `PrevWheel`, más el relinkado de `NextFree`. Esto son ~12 escrituras a memoria.

La motivación es "prevent stale-data access", lo cual es defensivo y está bien. Pero si `Active = false` ya se verifica antes de acceder a cualquier campo (y el `Generation` previene uso de handles stale), la limpieza de todos los campos es redundante.

**Impacto:** En `OS_SysTick` (ISR), cuando un one-shot expira, `TimerFree` ejecuta ~12 stores adicionales innecesarios bajo sección crítica. Con 1 timer esto son ~100-200 ns en un Cortex-M4, trivial.

**Severidad:** Cosmética. La seguridad defensiva es preferible en embebido.

### 3.4 🟢 `TickCounter` overflow a 0 es técnicamente correcto pero merece comentario

**Ubicación:** `OS_Timer.c:387`

`TickCounter` es `OS_U32`. Después de ~49.7 días a 1 ms/tick, hace wraparound a 0. La aritmética unsigned en la fórmula del Round (`Expiry - TickCounter - 1`) sigue funcionando correctamente por las propiedades del complemento a dos de unsigned. Sin embargo, **no hay un comentario que explique explícitamente que el wraparound es seguro**. El archivo documenta la fórmula del Round pero no menciona el escenario de wrap de `TickCounter`.

**Severidad:** Ninguna (el código es correcto). Solo una sugerencia de documentación.

### 3.5 🟢 No hay error real de lógica

Tras revisar exhaustivamente:
- La fórmula del Round es correcta para todo el rango `[1, 2³²-1]`.
- La Generation salta el 0 en wraparound (correcto).
- Las secciones críticas cubren todas las estructuras compartidas.
- `OS_GetTickCount` usa sección crítica para plataformas de 8/16 bits (correcto).
- Los periodic timers usan `Expiry += Period` (sin drift).
- `OS_TimerDelete` maneja handles stale sin assert (correcto).
- `OS_TimerDeleteByState` guarda `next` antes de `TimerFree` (correcto — no se invalida el iterador).

**No se encontraron errores de lógica o de concurrencia.**

---

## 4. Sugerencias

### 4.1 📋 Desacoplar `POOL_SIZE` de `WHEEL_SIZE`

**Motivación:** Actualmente `Pool[OS_TIMER_WHEEL_SIZE]` ata el número máximo de timers al número de slots de la wheel. Si se quisiera 32 timers pero solo 16 slots (para ahorrar RAM en `Wheel[]`), no se puede.

**Propuesta:** Añadir un `OS_TIMER_POOL_SIZE` separado en `OS_Config.h`:

```c
#define OS_TIMER_POOL_SIZE   16U   /* Máximo timers simultáneos */
#define OS_TIMER_WHEEL_BITS  4U    /* 16 slots de wheel */
```

Donde `POOL_SIZE` puede ser ≥ o ≤ `WHEEL_SIZE` (más timers que slots → más colisiones por slot, pero funciona).

**Impacto:** Bajo. Solo cambia el tamaño de `Pool[]` y los `_Static_assert`. Todas las demás operaciones siguen igual.

**Prioridad:** Baja. Con 16 timers esto no importa. Solo si el sistema crece.

### 4.2 📋 Añadir `OS_TimerIsActive(handle)` a la API pública

**Motivación:** Actualmente no hay forma de saber si un timer sigue activo sin intentar borrarlo. Un usuario podría querer verificar si un timer sigue pendiente sin cancelarlo.

**Propuesta:**

```c
bool OS_TimerIsActive(OS_TimerHandle handle)
{
    if (handle.Index >= OS_TIMER_WHEEL_SIZE) return false;
    Port_CriticalEnter();
    bool active = Pool[handle.Index].Active
               && (Pool[handle.Index].Generation == handle.Generation);
    Port_CriticalExit();
    return active;
}
```

**Impacto:** Mínimo. O(1), no cambia nada existente.

**Prioridad:** Media. Es una funcionalidad útil que falta.

### 4.3 📋 Considerar `OS_TimerRestart(handle, newPeriod)` para evitar delete+create

**Motivación:** Si un estado quiere reiniciar un timer (ej: resetear un timeout al recibir un evento), actualmente necesita:

```c
OS_TimerDelete(myHandle);
myHandle = OS_TimerCreate(SIG_TIMEOUT, 500, false);
```

Esto consume 2 operaciones (free + alloc), cambia la generación, y tiene el overhead de relinkado en 3 listas.

**Propuesta:** Un `OS_TimerRestart` que reuse el slot:

```c
bool OS_TimerRestart(OS_TimerHandle handle, OS_U32 newPeriod);
```

Internamente: `WheelRemove` → actualizar `Expiry`/`Period`/`Round` → `WheelInsert`. Sin tocar la free list ni la lista per-HSM.

**Impacto:** ~50% menos operaciones que delete+create. El handle no cambia.

**Prioridad:** Media-alta. Es un patrón de uso muy común en HSMs.

### 4.4 📋 Documentar explícitamente que el wraparound de `TickCounter` es seguro

**Motivación:** Como se menciona en §3.4, la aritmética unsigned hace que el Round se calcule correctamente incluso cuando `TickCounter` hace wrap. Pero un desarrollador nuevo que lea el código podría preocuparse.

**Propuesta:** Añadir un comentario en `WheelInsert`:

```c
/*
 * NOTE: TickCounter wraparound is safe.  Since both Expiry and
 * TickCounter are unsigned 32-bit, the subtraction (Expiry - TickCounter)
 * produces the correct distance modulo 2^32 regardless of wrap.
 */
```

**Prioridad:** Baja. Solo documentación.

### 4.5 📋 Considerar reducir la limpieza en `TimerFree` bajo ISR

**Motivación:** Como se menciona en §3.3, `TimerFree` escribe 12 campos a cero. Cuando se llama desde `OS_SysTick` (ISR), esto alarga la sección crítica innecesariamente.

**Propuesta:** Reducir la limpieza al mínimo necesario:

```c
static void TimerFree(OS_U16 idx)
{
    WheelRemove(idx);
    /* Unlink from per-HSM list */
    ...
    Pool[idx].Hook->TimerCount--;
    Pool[idx].Active   = false;
    Pool[idx].NextFree = FreeHead;
    FreeHead           = (OS_I16)idx;
}
```

Los demás campos se sobreescriben en `OS_TimerCreate` al reasignar el slot.

**Prioridad:** Baja. La limpieza defensiva tiene valor en debug. Podría condicionarse con `#ifndef NDEBUG`.

### 4.6 📋 Añadir `OS_TimerGetRemaining(handle)` para debugging/observabilidad

**Motivación:** No hay forma de saber cuántos ticks faltan para que un timer expire. Útil para debug, logging, o para que un estado decida si reiniciar un timer o dejarlo correr.

**Propuesta:**

```c
OS_U32 OS_TimerGetRemaining(OS_TimerHandle handle);
```

Calcula `(Round * WHEEL_SIZE) + distancia al slot`. La precisión depende de la resolución de la wheel, no es exacta al tick, pero es suficiente para diagnóstico.

**Prioridad:** Baja. Solo para observabilidad.

---

## 5. Resumen

| Categoría | Hallazgos |
|---|---|
| **¿Es eficiente?** | ✅ Sí. Timing wheel + Round es óptima para 16 timers. O(1) en todas las operaciones del hot path. Sin malloc, sin drift, sin overflow. |
| **Errores de lógica** | ✅ Ninguno encontrado. La aritmética, la concurrencia y el manejo de handles son correctos. |
| **Errores de documentación** | ⚠️ El O(1) en `OS_SysTick` es técnicamente O(k) con k ≤ WHEEL_SIZE. Falta comentario sobre wraparound de TickCounter. |
| **Sugerencias de mejora** | 📋 `OS_TimerRestart` (prioridad media-alta), `OS_TimerIsActive` (media), desacoplar pool/wheel (baja), reducir limpieza en TimerFree (baja), documentar wraparound (baja). |

**Conclusión:** El módulo está bien diseñado y bien implementado. La estructura de datos elegida es la correcta para el contexto. No hay errores funcionales. Las sugerencias son mejoras incrementales, no correcciones.
