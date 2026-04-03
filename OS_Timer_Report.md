# Reporte de Análisis Exhaustivo: OS_Timer.c / OS_Timer.h

**Módulo analizado:** Gestión de temporizadores software y watchdog del RTEF OS  
**Archivos:** `os/src/OS_Timer.c`, `os/inc/OS_Timer.h`  
**Dependencias revisadas:** `OS_Types.h`, `OS_Config.h`, `OS_Error.h`, `OS_Event.h`, `OS_Hsm.h`, `OS_Hsm.c`, `OS_Port.h`  
**Fecha:** 2026-04-03

---

## 1. Fallos a Nivel Lógico y de Funcionamiento

### 1.1 — Temporizadores con periodo largo disparan inmediatamente (Bug crítico)

**Ubicación:** `OS_Timer.c:209`, `OS_Timer.c:63-66`, `OS_Timer.c:303`

`OS_TimerCreate` calcula `Expiry = TickCounter + periodMs`. `OS_SysTick` usa `TickLeq()` para determinar si un timer ha expirado:

```c
static bool TickLeq(OS_U32 a, OS_U32 b)
{
    return (OS_I32)(a - b) <= 0;
}
```

Esta comparación con signo colapsa cuando `Expiry - TickCounter > 2^31` (~24.85 días a 1 ms/tick). El cast a `OS_I32` convierte el resultado en negativo, y `TickLeq` retorna `true`, haciendo que el timer **expire inmediatamente** en lugar de esperar.

**Ejemplo:** Si `periodMs = 3_000_000_000` (~34.7 días), `(OS_I32)(Expiry - TickCounter)` es negativo → el timer dispara en el siguiente tick.

**Impacto:** Cualquier timer con periodo > ~24.85 días se dispara instantáneamente sin ningún aviso. No existe validación del rango.

---

### 1.2 — Pérdida permanente de ticks por secciones críticas largas

**Ubicación:** `OS_Timer.c:286-324`

`OS_SysTick` se llama desde la ISR del HW timer. Cada invocación hace `TickCounter++` y revisa **un solo slot** de la rueda. Si el sistema está en sección crítica por más de 1 ms, la ISR no se ejecuta y esos ticks se pierden **permanentemente**.

- `TickCounter` se atrasa respecto al tiempo real.
- Timers que debían expirar durante la sección crítica podrían no revisarse porque su slot ya pasó.
- El watchdog software también pierde ticks, extendiendo artificialmente su timeout.

**Impacto:** Cualquier sección crítica > 1 ms desincroniza todo el sistema de tiempo.

---

### 1.3 — El nombre `periodMs` es engañoso — usa ticks, no milisegundos

**Ubicación:** `OS_Timer.c:209`, `OS_Timer.h:46-47`

```c
Pool[idx].Expiry = TickCounter + periodMs;
```

El parámetro se llama `periodMs` (sugiriendo milisegundos), pero se suma directamente al contador de ticks sin conversión alguna. Funciona **solo** si 1 tick = 1 ms. Si el tick rate cambia (por ejemplo a 10 ms o 100 µs), todos los timers se descalibran silenciosamente.

**Impacto:** Error de portabilidad latente y confusión para el usuario de la API.

---

### 1.4 — Watchdog permite timeout = 0 (assert inmediato en siguiente tick)

**Ubicación:** `OS_Timer.c:327-336`

`OS_WatchdogInit(0)` establece `WdgCounter = 0` y `WdgEnabled = true`. En el siguiente `OS_SysTick`, `WatchdogTick()` ejecuta:

```c
if (WdgCounter > 0U) { WdgCounter--; }   // No decrementa (ya es 0)
if (WdgCounter == 0U) { Q_ASSERT_ID(99U, false); }  // Muere aquí
```

No hay validación de que `timeoutMs > 0`. El sistema se detiene en el siguiente tick sin explicación clara.

---

### 1.5 — Timer periódico no recupera ticks perdidos por latencia de ISR

**Ubicación:** `OS_Timer.c:307-312`

Cuando un timer periódico expira:

```c
Pool[cur].Expiry = Pool[cur].Expiry + Pool[cur].Period;
```

Si el SysTick tenía latencia y `TickCounter` avanzó varios periodos por delante del `Expiry`, el timer solo avanza un periodo por tick-check. Necesita múltiples ciclos de SysTick para "alcanzar" al tiempo actual, disparando ráfagas de eventos consecutivos uno por tick.

**Impacto:** Ráfagas de eventos que pueden saturar la cola de eventos o causar comportamiento inesperado en las HSM.

---

### 1.6 — OS_GetTickCount no es atómico en plataformas de 8/16 bits

**Ubicación:** `OS_Timer.c:349-352`

```c
OS_U32 OS_GetTickCount(void)
{
    return TickCounter;
}
```

`TickCounter` es `volatile OS_U32`. En plataformas de 32 bits, la lectura es atómica. En plataformas de 8 o 16 bits, leer 32 bits requiere múltiples instrucciones. Si la ISR modifica `TickCounter` entre instrucciones, el valor retornado es **corrupto** (torn read).

No se usa `Port_CriticalEnter/Exit` para proteger la lectura.

---

## 2. Fallos de Seguridad de la API (Cómo el Usuario Puede Romper el Sistema)

### 2.1 — Pool exhaustion = muerte del sistema

**Ubicación:** `OS_Timer.c:204`

```c
Q_ASSERT(FreeHead >= 0);
```

Si la pool está llena, el sistema **muere** (Q_ASSERT → OS_ErrorHandler → halt). No hay forma de que el usuario:
- Consulte cuántos timers quedan disponibles.
- Reciba un código de error para manejar la situación.
- Establezca prioridades entre HSMs para asignación de timers.

Cualquier HSM puede agotar toda la pool y bloquear las demás.

---

### 2.2 — Señal duplicada = muerte del sistema

**Ubicación:** `OS_Timer.c:195-201`

```c
while (cur >= 0) {
    Q_ASSERT(Pool[(OS_U16)cur].Signal != signal);
    cur = Pool[(OS_U16)cur].NextHsm;
}
```

Si un usuario intenta crear un timer con la misma señal que otro ya activo en el mismo HSM, el sistema **muere**. La detección de duplicados es destructiva en lugar de informativa.

---

### 2.3 — OS_TimerDeleteByState es pública y peligrosa

**Ubicación:** `OS_Timer.h:65`

Documentada como "OS-internal" pero declarada en el header público. Cualquier código de usuario puede llamarla. Si se llama fuera del contexto correcto (fuera de dispatch), `OS_HsmGetCurrent()` retorna `NULL` y `hook->State[...]` causa un crash por null pointer dereference.

---

### 2.4 — OS_TimerDelete falla silenciosamente

**Ubicación:** `OS_Timer.c:247-255`

```c
if (t->Active && (t->Generation == handle.Generation)) {
    /* ... delete ... */
}
// Si no coincide: no hace nada, no retorna error
```

Si el handle es stale (generación no coincide) o el timer ya expiró, la función no hace nada y no lo notifica. El usuario no puede distinguir un delete exitoso de uno que no hizo nada.

---

### 2.5 — No hay forma de cancelar un timer y saber si la señal ya fue encolada

Cuando un one-shot timer expira, `OS_SysTick` encola la señal y libera el bloque. Si el usuario llama `OS_TimerDelete` después, el handle es stale y el delete es silencioso, pero la señal ya está en la cola. No hay mecanismo para:
- Saber si la señal fue ya encolada.
- Cancelar una señal que ya está en la cola.

**Impacto:** El HSM recibe señales de timers que el usuario "canceló", causando comportamiento fantasma.

---

### 2.6 — OS_TimerCreate solo funciona durante dispatch

**Ubicación:** `OS_Timer.c:186`

```c
Q_ASSERT(OS_HsmInDispatch());
```

No se pueden crear timers desde `main()`, desde threads auxiliares, ni desde ningún contexto que no sea un handler de estado HSM. Esto limita severamente la usabilidad y no está suficientemente documentado (solo un assert, no una nota prominente en la API).

---

### 2.7 — Handle Index=0 con Generation=0 colisiona con timer real

**Ubicación:** `OS_Timer.h:21`

```c
#define OS_TIMER_INVALID  ((OS_TimerHandle){ 0xFFFFU, 0U })
```

Esto tiene Index=0xFFFF (fuera de rango para OS_MAX_TIMERS=16, así que es seguro). **Pero** el Generation=0 del sentinel coincide con Generation=0 de un timer recién creado si nunca fue reutilizado... excepto que Generation se incrementa antes de asignar. El slot 0 empieza con Generation=0, y en su primer uso pasa a Generation=1. Correcto.

**Sin embargo**, después de 65535 usos del mismo slot, Generation vuelve a 0 (wrap). En ese punto, un handle `{Index, 0}` coincide tanto con OS_TIMER_INVALID (si el usuario lo compara solo por Generation) como con un timer real.

---

## 3. Fallos a Nivel Filosófico

### 3.1 — Q_ASSERT como mecanismo de protección operacional es un error de diseño

Todo el módulo usa `Q_ASSERT` (halt fatal) para condiciones que son **operacionales**, no de programación:
- Pool llena → halt
- Timer duplicado → halt  
- Handle inválido → halt (si no es stale)

En un sistema embebido de producción, la pool llena es una condición de runtime manejable. Matar el sistema por falta de un timer es desproporcionado. Debería haber códigos de error retornables.

---

### 3.2 — Mezcla de responsabilidades: timer + watchdog en el mismo módulo

El watchdog software (`WdgTimeout`, `WdgCounter`, `WdgEnabled`, `WatchdogTick`, `OS_WatchdogInit`, `OS_WatchdogFeed`) vive dentro del módulo de timers. Son conceptos diferentes:
- **Timers**: planificación de eventos futuros para HSMs.
- **Watchdog**: supervisión de salud del sistema.

Mezclarlos viola el Principio de Responsabilidad Única. Si se quiere cambiar la estrategia del watchdog (por ejemplo, watchdog por HSM), hay que modificar el módulo de timers.

---

### 3.3 — Acoplamiento implícito con el contexto global de HSM

`OS_TimerCreate` no recibe el HSM como parámetro — lo infiere de `OS_HsmGetCurrent()`. Esto:
- Hace la API imposible de usar fuera de dispatch.
- Crea un acoplamiento invisible con el módulo HSM.
- Imposibilita crear timers para un HSM desde otro HSM o desde código que no es un estado.
- Hace imposible testear la API de timers de forma aislada.

---

### 3.4 — OwnerState como criterio de auto-borrado es frágil

Los timers se asocian al estado que los creó (`OwnerState`). Al transicionar, `OS_TimerDeleteByState` elimina todos los timers de ese estado. Esto significa que:
- Un timer que conceptualmente pertenece al HSM entero (ej: un heartbeat) debe recrearse en cada estado.
- Si un estado padre necesita un timer creado por un hijo, se pierde al transicionar entre hijos.
- No hay forma de crear un timer que sobreviva transiciones.

El modelo es demasiado rígido y fuerza patrones de código innecesariamente complicados.

---

### 3.5 — Timing wheel sobredimensionada

**Ubicación:** `OS_Config.h:18-24`

La rueda tiene 256 slots (`OS_TIMER_WHEEL_SIZE`) pero solo 16 timers máximos (`OS_MAX_TIMERS`). Es decir, **240 slots están siempre vacíos**. Esto desperdicia 240 × `sizeof(OS_I16)` = 480 bytes de RAM estática en un sistema embebido, lo cual es significativo en microcontroladores pequeños.

El tamaño de la rueda debería ser proporcional al número máximo de timers, no arbitrariamente 256.

---

### 3.6 — No hay concepto de prioridad ni fairness entre HSMs

Todos los HSMs comparten la misma pool de timers sin cuotas ni límites por HSM. Un HSM mal programado puede consumir los 16 timers disponibles, dejando al resto sin capacidad de crear timers. No existe:
- Cuota máxima por HSM.
- Prioridad en asignación.
- Mecanismo de preemption (liberar un timer de baja prioridad).

---

### 3.7 — La API no sigue el principio de mínima sorpresa

- `periodMs` no son milisegundos (son ticks).
- `OS_TimerDelete` no siempre borra (falla silenciosamente).
- `OS_TimerDeleteByState` es pública pero documentada como interna.
- Crear un timer puede matar el sistema (assert en pool llena).
- No hay forma de saber si un handle es válido sin intentar borrar.

---

## 4. Fallos de Código

### 4.1 — Overflow de Generation a 0 es peligroso

**Ubicación:** `OS_Timer.c:214`

```c
Pool[idx].Generation = Pool[idx].Generation + 1U;
```

`Generation` es `OS_U16`. Después de 65535 creaciones en el mismo slot, hace wrap a 0. Esto significa:
- Un handle antiguo `{idx, G}` podría coincidir con una asignación futura del mismo slot con el mismo `G`.
- `Generation == 0` coincide con el valor que tiene un slot nunca usado (`OS_TimerInit` lo pone a 0).

No hay protección contra el wrap. Debería saltar Generation=0 (usar `Generation = (Gen + 1U) | 1U` u otra estrategia).

---

### 4.2 — Casts constantes entre OS_I16 y OS_U16

El código está plagado de casts como `(OS_U16)Wheel[slot]`, `(OS_I16)idx`, `(OS_U16)Pool[idx].PrevWheel`, etc. Cada uno es un punto potencial de error. El uso de `-1` como sentinel en campos OS_I16 y luego indexar con OS_U16 es correcto pero frágil.

**Ejemplo peligroso:** Si `OS_MAX_TIMERS` excede 32767, los casts de `OS_I16` a índice cortan el valor. No hay static_assert que lo verifique.

---

### 4.3 — TimerFree no limpia campos sensibles

**Ubicación:** `OS_Timer.c:108-130`

Después de `TimerFree`, los campos `Hook`, `Signal`, `Period`, `Expiry`, `OwnerState` conservan sus valores anteriores. Un acceso accidental (ej: vía handle stale que coincide en generación) opera sobre datos fantasma del timer anterior.

---

### 4.4 — WatchdogTick tiene patrón de doble-check innecesariamente frágil

**Ubicación:** `OS_Timer.c:135-148`

```c
if (WdgCounter > 0U) {
    WdgCounter--;
}
if (WdgCounter == 0U) {
    Q_ASSERT_ID(99U, false);
}
```

Los dos `if` son independientes. Cuando `WdgCounter == 1`, se decrementa a 0 y **en la misma invocación** el segundo `if` dispara. Esto es correcto lógicamente, pero la estructura sugiere que deberían ser mutuamente exclusivos. Sería más claro y robusto:

```c
if (WdgCounter > 1U) {
    WdgCounter--;
} else {
    Q_ASSERT_ID(99U, false);
}
```

---

### 4.5 — No hay static_assert para verificar invariantes de configuración

Las siguientes propiedades son asumidas pero nunca verificadas en tiempo de compilación:
- `OS_TIMER_WHEEL_SIZE` es potencia de 2.
- `OS_MAX_TIMERS` cabe en `OS_I16` (≤ 32767).
- `OS_MAX_TIMERS` < 0xFFFF (para que `OS_TIMER_INVALID.Index` sea inválido).
- `OS_TIMER_WHEEL_MASK == OS_TIMER_WHEEL_SIZE - 1`.

Un error de configuración compilaría sin warnings y causaría corrupción de memoria en runtime.

---

### 4.6 — volatile solo en TickCounter; Pool y Wheel modificados desde ISR sin volatile

**Ubicación:** `OS_Timer.c:44-46`, `OS_Timer.c:53`

```c
static TimerBlock Pool[OS_MAX_TIMERS];      // NO volatile
static OS_I16     Wheel[OS_TIMER_WHEEL_SIZE]; // NO volatile
static volatile OS_U32 TickCounter;          // SÍ volatile
```

`OS_SysTick` (ISR) modifica `Pool`, `Wheel` y `FreeHead`. Estos no son `volatile`. Las secciones críticas evitan acceso concurrente, pero el compilador no lo sabe — podría optimizar lecturas/escrituras de `Pool` y `Wheel` de formas inesperadas (reordenar, cachear en registros, eliminar escrituras "redundantes").

En la práctica esto funciona con la mayoría de compiladores por la barrera implícita de llamar a funciones externas (`Port_CriticalEnter`), pero formalmente no es correcto según el modelo de memoria de C11.

---

### 4.7 — Ausencia de `_Static_assert` para OS_MAX_TIMERS < 0xFFFF

Si alguien configura `OS_MAX_TIMERS` a un valor ≥ 65535, el bucle de inicialización:

```c
for (i = 0U; i < (OS_U16)OS_MAX_TIMERS; i++)
```

nunca termina si `OS_MAX_TIMERS == 65536` (porque `(OS_U16)65536 == 0`). Resultado: loop infinito en la inicialización.

---

## 5. Fallos de Seguridad

### 5.1 — Buffer overflow si Q_ASSERT se compila fuera

**Ubicación:** `OS_Timer.c:241`

```c
Q_ASSERT(handle.Index < (OS_U16)OS_MAX_TIMERS);
```

Si `Q_ASSERT` se define como `((void)0)` (común en builds de release), un `handle.Index` de 1000 accede a `Pool[1000]`, que está **fuera del array**. Esto es un **buffer overflow** que puede:
- Leer/escribir memoria arbitraria.
- Corromper el stack o variables adyacentes.
- Ser explotable para ejecución de código arbitrario.

**Severidad:** Crítica. Las validaciones de seguridad NO deben depender de asserts que pueden ser deshabilitados.

---

### 5.2 — Null pointer dereference si asserts están deshabilitados

**Ubicación:** `OS_Timer.c:186-188`

```c
Q_ASSERT(OS_HsmInDispatch());
me = OS_HsmGetCurrent();
Q_ASSERT(me != (OS_Hsm *)0);
```

Sin asserts, si se llama fuera de dispatch: `OS_HsmGetCurrent()` retorna NULL → `me->TimerHead` causa segfault.

**Lo mismo en `OS_TimerDeleteByState`** (línea 268): `hook = OS_HsmGetCurrent()` sin verificar, seguido de `hook->State[...]`.

---

### 5.3 — Denegación de servicio (DoS) por agotamiento de pool

Cualquier HSM puede crear 16 timers (toda la pool), impidiendo que el resto del sistema funcione. No hay:
- Límite por HSM.
- Mecanismo de recuperación.
- Logging antes del halt.

Un HSM con bug puede matar el sistema entero.

---

### 5.4 — Stale handle reuse después de Generation wraparound

**Ubicación:** `OS_Timer.c:214`, `OS_Timer.c:247`

Después de 65536 ciclos de alloc/free en un mismo slot, `Generation` vuelve al mismo valor. Un handle guardado de hace 65536 ciclos ahora **pasa la verificación**:

```c
if (t->Active && (t->Generation == handle.Generation))
```

Esto permite que un HSM/estado que ya no debería tener acceso al timer pueda borrarlo. En un sistema de larga duración (meses/años), esto es alcanzable.

---

### 5.5 — Prioridad invertida: sección crítica larga en OS_SysTick

**Ubicación:** `OS_Timer.c:286-324`

`OS_SysTick` mantiene la sección crítica durante toda su ejecución:
- Iteración sobre todos los timers en un slot de la rueda.
- Posting de eventos (OS_InsertEventFromIsr).
- Re-inserción en la rueda para periódicos.
- WatchdogTick.

En el peor caso (todos los timers en el mismo slot), esto es O(n) con n = OS_MAX_TIMERS. Durante este tiempo, todas las interrupciones están deshabilitadas (o el lock está tomado), causando **latencia de interrupciones no acotada**.

---

### 5.6 — Software watchdog depende de la misma ISR que monitorea

`WatchdogTick()` se llama desde `OS_SysTick`. Si la ISR del SysTick deja de funcionar (ISR deshabilitada, bug en timer HW, prioridad de interrupción incorrecta), el watchdog software **nunca decrementa** y nunca detecta el problema.

El watchdog debería ser independiente del sistema que monitorea. Depender del mismo tick que vigila es un anti-patrón de seguridad.

---

### 5.7 — Port_WatchdogInit se llama fuera de sección crítica

**Ubicación:** `OS_Timer.c:335`

```c
Port_CriticalExit();              // línea 333 implícita
Port_WatchdogInit(timeoutMs);     // línea 335: fuera de sección crítica
```

Hay una ventana entre `Port_CriticalExit()` y `Port_WatchdogInit()` donde el software watchdog está habilitado (`WdgEnabled = true`) pero el hardware watchdog no está inicializado. Si un SysTick ocurre en esa ventana, el software watchdog empieza a contar sin que el HW watchdog esté listo.

**Lo mismo con `OS_WatchdogFeed`** (línea 345): el feed del HW watchdog ocurre fuera de la sección crítica.

---

### 5.8 — No hay mecanismo de auditoría o logging

No existe:
- Conteo de timers creados/destruidos.
- Logging de expiración de timers.
- Detección de condiciones anómalas (pool casi llena, timer expirado con mucha latencia).

En un sistema seguro, la ausencia de observabilidad dificulta la detección de ataques, bugs, o degradación del servicio.

---

## Resumen Ejecutivo

| Categoría | Críticos | Moderados | Menores |
|---|---|---|---|
| 1. Lógica y Funcionamiento | 2 | 3 | 1 |
| 2. Seguridad de la API | 3 | 3 | 1 |
| 3. Filosóficos | 2 | 4 | 1 |
| 4. Código | 3 | 3 | 1 |
| 5. Seguridad | 4 | 3 | 1 |
| **Total** | **14** | **16** | **5** |

Los fallos más graves son:
1. **Buffer overflow sin asserts** (§5.1) — vulnerabilidad de corrupción de memoria.
2. **Timers largos disparan inmediatamente** (§1.1) — bug silencioso de lógica.
3. **Toda protección depende de Q_ASSERT** (§5.1, §5.2) — en release, el sistema queda completamente desprotegido.
4. **Pool exhaustion = halt** (§2.1) — sin recuperación posible.
5. **El watchdog depende de lo que monitorea** (§5.6) — anti-patrón de seguridad fundamental.
