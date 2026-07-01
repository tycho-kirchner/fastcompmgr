# Informe de Auditoría Técnica — fastcompmgr

**Fecha:** 2026-06-29  
**Auditor:** Consultor externo senior (C / X11 / sistemas gráficos)  
**Mandato:** Evaluar estabilidad, correctitud, seguridad y resiliencia. fastcompmgr debe ser el compositor más rápido, liviano, estable y resiliente posible.

---

## Índice por severidad

| # | Título | Severidad | Archivo(s) |
|---|--------|-----------|------------|
| 1 | Integer overflow en cálculo de sombra | 🔴 Crítico | `fastcompmgr.c` |
| 2 | Loop infinito en hash table si OOM | 🔴 Crítico | `cm-window.c` |
| 3 | Buffer underflow por opacidad NaN/negativa | 🔴 Crítico | `fastcompmgr.c` |
| 4 | Uninitialized read de 64 bits en `root_create_tile` | 🔴 Crítico | `cm-root.c` |
| 5 | Fuga de memoria perpetua en `expose_rects` | 🟠 Alto | `fastcompmgr.c` |
| 6 | `find_client_win` cache miss perpetuo | 🟠 Alto | `fastcompmgr.c`, `cm-window.h` |
| 7 | Falta de `XFlush()` antes de `poll()` | 🟠 Alto | `fastcompmgr.c` |
| 8 | `gettimeofday` obsoleto e inseguro | 🟠 Alto | `cm-util.h` |
| 9 | Ring buffer de ignores puede crecer sin límite | 🟡 Medio | `cm-event.c` |
| 10 | Código muerto `typedef struct _ignore` | 🟡 Medio | `fastcompmgr.c` |
| 11 | Subsistema de fading como código muerto activo | 🟡 Medio | `fastcompmgr.c` |
| 12 | Inserción O(n) en `add_win` para búsqueda de `prev` | 🟡 Medio | `fastcompmgr.c` |
| 13 | No hay manejo de `SIGPIPE` | 🟢 Bajo | `fastcompmgr.c` |
| 14 | No hay limpieza de recursos X11 globales al salir | 🟢 Bajo | `fastcompmgr.c` |
| 15 | `BadWindow` no suprimido en `find_client_win()` | 🟡 Medio | `fastcompmgr.c` |
| 16 | Corrupción de puntero `prev` en `restack_win` → pantalla gris | 🔴 Crítico | `fastcompmgr.c` |

---

## Estado de implementación

Los items marcados con ✅ han sido aplicados al código actual. Los pendientes se mantienen como deuda técnica documentada.

| # | Estado | Notas |
|---|--------|-------|
| 1 | ✅ **Implementado** | Validación CLI + `size_t` en `make_shadow` + clamp `opacity_int`. |
| 2 | ✅ **Implementado** | Resize con retorno `Bool`, probe limit, y contador de tombstones con rehash forzado. |
| 3 | ✅ **Implementado** | `shadow_opacity` usa `normalize_d()`; `opacity_int` clamp a `[0,25]` en `make_shadow`. |
| 4 | ⬜ **Pendiente** | `Pixmap` sin inicializar en `root_create_tile`. Bajo riesgo en little-endian. |
| 5 | ⬜ **Pendiente** | `expose_rects` nunca se libera. Fuga acotada, no crítica para sesiones cortas. |
| 6 | ✅ **Implementado** | Flag `client_id_resolved` añadido a `win` struct; usado en `get_frame_extents` e invalidado en ReparentNotify. |
| 6b | ✅ **Implementado** | Guarda anti-duplicado en `add_win`: `if (find_win_any_state(id)) return;` previene doble `add_win` por `CreateNotify` + `ReparentNotify`, incluso cuando la ventana anterior está marcada `destroyed` pero aún no liberada. |
| 6c | ✅ **Implementado** | `find_win_any_state()` añadido a `cm-window.c` — búsqueda en hash table sin filtrar `destroyed`, para uso exclusivo del anti-duplicado en `add_win()`. |
| 15 | ⬜ **Pendiente** | `find_client_win()` llama `XQueryTree` sin `set_ignore`. Ruido benigno en logs para ventanas efímeras. |
| 7 | ⬜ **Pendiente** | `XFlush()` antes de `poll()`. Potencial mejora de latencia, pero cambia semántica de buffering Xlib. |
| 8 | ✅ **Implementado** | `clock_gettime(CLOCK_MONOTONIC)` reemplazó `gettimeofday`; `_program_start_secs` eliminado. |
| 9 | ⬜ **Pendiente** | Cap de tamaño máximo para ring buffer de ignores. |
| 10 | ⬜ **Pendiente** | Eliminar `typedef struct _ignore`. |
| 11 | ⬜ **Pendiente** | Eliminar o `#if 0` el subsistema de fading. |
| 12 | ⬜ **Pendiente** | Inserción O(1) en `add_win` usando hash table para buscar `prev`. |
| 13 | ⬜ **Pendiente** | `signal(SIGPIPE, SIG_IGN)`. |
| 14 | ⬜ **Pendiente** | Cleanup de recursos X11 globales en `atexit`. |
| 15 | ⬜ **Pendiente** | `find_client_win()` llama `XQueryTree` sin `set_ignore`. Ruido benigno en logs para ventanas efímeras. |
| 16 | ✅ **Implementado** | `restack_win()` ahora rastrea `new_pred` durante el bucle rehook. Cuando se inserta al final (`*prev_slot == NULL`), `w->prev` se establece a `new_pred` (último nodo visitado) en vez de `NULL`. Esto evita que `finish_destroy_win` borre accidentalmente toda la lista con `list = NULL`. |

---

## 🔴 1. Integer overflow en cálculo de sombra → heap overflow potencial

**Archivos:** `fastcompmgr.c`  
**Funciones:** `make_shadow()`, `presum_gaussian()`  
**Líneas relevantes:** ~560-580 (cálculo de tamaño), ~461-498 (precomputación).

### Descripción
`width` y `height` provienen de atributos X11 (`w->a.width`, `w->a.height`, `w->a.border_width`) que son `unsigned int`, pero en `make_shadow()` se operan como `int`:

```c
int swidth = width + gsize;
int sheight = height + gsize;
...
data = malloc(swidth * sheight * sizeof(unsigned char));
```

Si `shadow_radius` es muy grande (el usuario puede pasar `-r 100000` por CLI), `gsize` explota. El producto `swidth * sheight` desborda `int` (por ejemplo, 32768 * 32768 = 1,073,741,824, que cabe en signed 32-bit... pero 65536 * 65536 = 4,294,967,296, que NO cabe). Al castear a `size_t` en `malloc`, un `int` negativo por desbordamiento se convierte en un `size_t` gigantesco, provocando fallo de `malloc` o, peor, si el producto se trunca de vuelta a un valor pequeño, se alloca un buffer diminuto y luego `memset` en `make_transparent_shadowcenter()` escribe fuera de los límites.

### Análisis técnico
- `shadow_radius` se lee de `atoi(optarg)` en `main()` sin validación de rango.
- `make_gaussian_map()` calcula `size = ((int) ceil((r * 3)) + 1) & ~1`. Con `r = 100000`, `size` ≈ 300002.
- `malloc(300004 * 300004)` = ~90 GB. El `int` se desborda antes de llegar a `malloc`.

### Pasos de implementación
1. **Validar `shadow_radius` en CLI:** Después de leerlo con `atoi`, añadir:
   ```c
   if (shadow_radius < 0 || shadow_radius > 100) {
       fprintf(stderr, "Warning: shadow radius %d out of range, using 12\n", shadow_radius);
       shadow_radius = 12;
   }
   ```
2. **Usar `size_t` en `make_shadow`:** Cambiar las variables de tamaño:
   ```c
   size_t swidth = (size_t)width + (size_t)gsize;
   size_t sheight = (size_t)height + (size_t)gsize;
   size_t alloc_size = swidth * sheight;
   if (width <= 0 || height <= 0 || swidth > INT_MAX / sheight) {
       fprintf(stderr, "fastcompmgr: shadow size overflow, skipping shadow\n");
       return 0;
   }
   data = malloc(alloc_size);
   ```
3. **Revisar `presum_gaussian`:** Los arrays `shadow_corner` y `shadow_top` también usan `Gsize + 1` como factor. Si `Gsize` es gigantesco, `malloc` fallará aquí primero. Añadir verificación de NULL tras cada `malloc`.

### Verificación
- Compilar (`make clean && make`).
- Ejecutar `./fastcompmgr -r 99999 -o 0.4 -c -C`. Debe imprimir el warning y usar radio 12, sin segfault.
- Probar `./fastcompmgr -r 50` (valor razonable) y verificar que las sombras siguen funcionando.

---

## 🔴 2. Loop infinito en hash table si `calloc` de resize falla

**Archivo:** `cm-window.c`  
**Funciones:** `win_hash_resize()`, `win_hash_insert()`  
**Líneas relevantes:** ~29-62.

### Descripción
`win_hash_resize()` falla silenciosamente si `calloc` devuelve NULL:

```c
static void win_hash_resize(void) {
  unsigned int new_size = win_hash_size ? win_hash_size * 2 : HASH_INITIAL_SIZE;
  win **new_hash = calloc(new_size, sizeof(win*));
  if (!new_hash) return;   // ← Silenciosamente falla
  ...
}
```

Si la tabla está al 75% de carga y el resize falla, `win_hash_insert` continúa:
```c
unsigned int idx = hash_window(w->id) & (win_hash_size - 1);
while (win_hash[idx] && win_hash[idx] != (win*)1) { ... }
```

Si la tabla está 100% ocupada (sin slots vacíos ni tombstones), el `while` recorre todos los slots, vuelve al origen, y nunca termina. **Loop infinito**, CPU al 100%.

### Análisis técnico
- `win_hash_size` es potencia de 2, máscara `& (size - 1)`.
- El resize se dispara cuando `win_hash_count >= win_hash_size * 3 / 4`.
- Si `calloc` falla, `win_hash_size` permanece igual. La siguiente inserción puede ser la que sature la tabla.

### Pasos de implementación
1. **Hacer `win_hash_insert` robusto ante resize fallido:** Si `win_hash_resize` falló (no hay forma de detectarlo desde fuera porque es `void`), añadir una guarda después del resize:
   ```c
   void win_hash_insert(win *w) {
     if (HASH_LOAD_FACTOR(win_hash_count + 1, win_hash_size)) {
       win_hash_resize();
     }
     if (!win_hash_size) return;  // No hay tabla (resize falló y nunca se inicializó)
     ...
   ```
2. **Mejor aún, hacer `win_hash_resize` no silencioso:** Si `calloc` falla, el programa debe manejarlo graceful. Opción mínima: `abort()` con mensaje. Opción robusta: marcar una variable global `g_hash_oom = True` y hacer que `add_win` devuelva sin insertar (la ventana no se gestionará, pero el proceso seguirá vivo).
3. **Alternativa de contingencia:** En `win_hash_insert`, si tras 2 vueltas completas a la tabla no se encuentra slot, romper el loop con `return` (perder la ventana es preferible a un DoS).

### Verificación
- Es difícil simular OOM de forma determinista. Como test de estrés: abrir 10,000 ventanas pequeñas (script de shell con `xterm` o `xclock`) y observar que `fastcompmgr` no cuelga el CPU.
- Revisar con `top` que no hay picos de CPU al 100% sostenidos.

---

## 🔴 3. Buffer underflow por opacidad NaN/negativa

**Archivo:** `fastcompmgr.c`  
**Funciones:** `presum_gaussian()`, `make_shadow()`  
**Líneas relevantes:** ~574 (opacity_int), ~476-495 (shadow_top indexing).

### Descripción
```c
int opacity_int = (int)(opacity * 25);
```
`opacity` proviene de `atof(optarg)` sin validación. Si el usuario pasa `--shadow-red nan` (indirectamente afectando el contexto de opacidad) o valores negativos, `opacity_int` puede ser negativo o un valor indeterminado.

Luego:
```c
shadow_top[opacity_int * (Gsize + 1) + x]
```
Esto es un buffer underflow/write fuera de límites si `opacity_int < 0`.

### Análisis técnico
- `shadow_top` se alloca como `(Gsize + 1) * 26` bytes. Los índices válidos son `opacity_int` de 0 a 25.
- `opacity` viene de `shadow_opacity = atof(optarg)` en `main()`. No hay clamp ni validación.

### Pasos de implementación
1. **Validar `shadow_opacity` en CLI** (junto con el radio en el punto 1):
   ```c
   case 'o':
     shadow_opacity = atof(optarg);
     if (shadow_opacity < 0.0 || shadow_opacity > 1.0) {
         fprintf(stderr, "Warning: shadow opacity %f out of range [0,1], using 0.75\n", shadow_opacity);
         shadow_opacity = 0.75;
     }
     break;
   ```
2. **Usar `normalize_d()` en `main` para colores RGB también:**
   ```c
   case 0:
     switch (longopt_idx) {
       case 0: shadow_red = normalize_d(atof(optarg)); break;
       case 1: shadow_green = normalize_d(atof(optarg)); break;
       case 2: shadow_blue = normalize_d(atof(optarg)); break;
   ```
   `normalize_d` ya existe en `cm-util.h`.
3. **Añadir assert/defensa en `make_shadow`:**
   ```c
   int opacity_int = (int)(opacity * 25);
   if (opacity_int < 0) opacity_int = 0;
   if (opacity_int > 25) opacity_int = 25;
   ```

### Verificación
- `./fastcompmgr -o -0.5` → debe mostrar warning y usar 0.75.
- `./fastcompmgr -o 1.5` → debe mostrar warning y usar 0.75 (o 1.0, según preferencia).
- `./fastcompmgr --shadow-red -1.0` → debe clamp a 0.0.

---

## 🔴 4. Uninitialized read de 64 bits en `root_create_tile`

**Archivo:** `cm-root.c`  
**Función:** `root_create_tile()`  
**Líneas relevantes:** ~116-137.

### Descripción
```c
Pixmap pixmap;   // unsigned long, 64 bits en LP64
...
memcpy(&pixmap, prop, 4);   // Solo 4 bytes escritos
```
Los 4 bytes superiores de `pixmap` (64 bits) contienen basura del stack. Si se usa `pixmap` como `Drawable` en `XGetGeometry`, puede ser un valor inválido.

### Análisis técnico
- `XGetWindowProperty` devuelve `prop` como array de bytes. La propiedad `_XROOTPMAP_ID` es un CARD32 (4 bytes).
- En arquitecturas little-endian, los 4 bytes bajos son los correctos y los altos son basura. Por casualidad suele funcionar si la basura es 0x00.
- En arquitecturas big-endian (teórico), leería el valor al revés.

### Pasos de implementación
1. **Inicializar antes del `memcpy`:**
   ```c
   Pixmap pixmap = None;
   ...
   if (actual_type == atom_pixmap && actual_format == 32 && nitems == 1) {
       unsigned int tmp;
       memcpy(&tmp, prop, 4);
       pixmap = (Pixmap)tmp;
   }
   ```
   Esto garantiza que los 4 bytes altos son cero (via `None` = 0).

### Verificación
- Compilar y ejecutar. El mensaje `"info: root background pixmap is valid/invalid"` debe seguir funcionando.
- No hay regresión visible, pero valgrind/ASAN dejaría de reportar `uninitialized-value` si se usan.

---

## 🟠 5. Fuga de memoria perpetua en `expose_rects`

**Archivo:** `fastcompmgr.c`  
**Función:** Manejador de evento `Expose` en el event loop (`main()`)  
**Líneas relevantes:** ~2797-2818.

### Descripción
```c
expose_rects = realloc(expose_rects, (size_expose + more) * sizeof(XRectangle));
```
El array `expose_rects` crece para acomodar ráfagas de eventos `Expose` sobre la ventana raíz, pero **nunca se libera**. Es una fuga acotada pero permanente.

### Pasos de implementación
1. **Liberar al final del programa:** Añadir en la sección de cleanup (o en un `atexit` handler, ver punto 14):
   ```c
   if (expose_rects) free(expose_rects);
   ```
2. **Opcional: usar buffer estático:** Si la frecuencia de root Expose es baja (sólo ocurre al redimensionar root o cambiar fondo), un buffer estático de 128 o 256 rectángulos es suficiente y elimina `realloc`:
   ```c
   static XRectangle expose_rects[256];
   static int n_expose = 0;
   ```
   Si `n_expose >= 256`, descartar o forzar flush inmediato.

### Verificación
- No hay test directo para esto. Valgrind reportaría "still reachable" en `expose_rects` al cerrar fastcompmgr.

---

## 🟠 6. `find_client_win` cache miss perpetuo para ventanas sin `WM_STATE`

**Archivo:** `fastcompmgr.c`  
**Función:** `get_frame_extents()`  
**Líneas relevantes:** ~958-1023.

### Descripción
```c
if (w->client_id) {
    client_window = w->client_id;
} else {
    client_window = find_client_win(dpy, w->id);
    w->client_id = client_window;
}
```
Si `find_client_win` devuelve `0`, `w->client_id` se setea a `0` (que es `None`). La próxima vez que se llama `get_frame_extents`, `if (w->client_id)` es falso, y se vuelve a ejecutar `find_client_win` (recursivo, con `XQueryTree`). Esto genera un round-trip X11 completo **en cada frame** para ventanas que nunca tendrán cliente.

### Análisis técnico
- `None` es `0L`. No hay forma de distinguir "aún no busqué" de "busqué y no existe".
- `get_frame_extents` se llama desde `win_paint_needed` cuando `hidden_type == HIDDEN_UNKNOWN`.

### Pasos de implementación
1. **Añadir flag a `win` struct** en `cm-window.h`:
   ```c
   Bool client_id_resolved;   // true si ya se intentó buscar el cliente
   ```
   (O reutilizar un campo existente si es posible, pero no hay candidato obvio).
2. **Modificar `get_frame_extents`:**
   ```c
   if (w->client_id_resolved) {
       client_window = w->client_id;
   } else {
       client_window = find_client_win(dpy, w->id);
       w->client_id = client_window;
       w->client_id_resolved = True;
   }
   ```
3. **Inicializar en `add_win`:** `new->client_id_resolved = False;`
4. **Invalidar en `add_damage_if_hidden_changed` (ReparentNotify):**
   ```c
   w->client_id = 0;
   w->client_id_resolved = False;
   ```

### Verificación
- Ejecutar con `xterm` (tiene `WM_STATE`, debe funcionar normal).
- Ejecutar con una ventana `override_redirect` pura (ej. `xclock -digital`, o `dmenu`). Usar `xtrace` o `strace -e connect` para verificar que no hay múltiples `XQueryTree` consecutivos.

---

## 🟠 7. Falta de `XFlush()` antes de `poll()`

**Archivo:** `fastcompmgr.c`  
**Función:** Event loop (`main()`, bucle `for (;;)`)  
**Líneas relevantes:** ~2700-2708.

### Descripción
```c
if (!QLength(dpy)) {
    int timeout = (configure_timer_started) ? 2 : fade_timeout();
    int poll_ret = poll(&ufd, 1, timeout);
    ...
}
```
`poll` espera en el fd del socket X11, pero si hay requests salientes encolados en el buffer de Xlib (por ejemplo, Damage subscriptions, XFixes operations, Composite redirects), el servidor X no puede responder hasta recibirlos. El proceso duerme innecesariamente hasta el timeout o hasta que otro evento externo llegue.

### Pasos de implementación
1. **Añadir `XFlush(dpy);` justo antes de `poll`:**
   ```c
   if (!QLength(dpy)) {
       XFlush(dpy);  // ← Añadir esta línea
       int timeout = (configure_timer_started) ? 2 : fade_timeout();
       int poll_ret = poll(&ufd, 1, timeout);
       ...
   }
   ```

### Verificación
- No hay regresión visible. La mejora es en latencia de pintado.
- Para medir: comparar el tiempo entre un `x11perf` de exposición con y sin el cambio. El cambio solo debería reducir ligeramente la latencia en escenarios de alta carga.

---

## 🟠 8. `gettimeofday` obsoleto e inseguro para medir intervalos

**Archivo:** `cm-util.h`  
**Función:** `get_time_in_milliseconds()`  
**Líneas relevantes:** ~19-24.

### Descripción
```c
static inline int get_time_in_milliseconds() {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return (tv.tv_sec-_program_start_secs) * 1000 + tv.tv_usec / 1000;
}
```
`gettimeofday()` mide tiempo de pared (wall-clock). Es sensible a saltos de reloj causados por NTP, ajustes manuales, o suspend/resume del sistema.

**Escenario de fallo:**
1. `g_now_ms = 12345`.
2. El sistema entra en suspensión.
3. Al despertar, `gettimeofday` ha avanzado 1 hora, pero el timer interno de fastcompmgr debería continuar desde donde iba.
4. `configure_time = g_now_ms + 2` se vuelve obsoleto. `check_paint()` puede pensar que pasó mucho tiempo y forzar repintados innecesarios, o, si el reloj se ajusta hacia atrás, `delta = g_now_ms - configure_time` puede ser negativo, y `check_paint` puede no pintar nunca.

### Pasos de implementación
1. **Reemplazar `gettimeofday` por `clock_gettime(CLOCK_MONOTONIC)`:**
   ```c
   #include <time.h>
   static inline int get_time_in_milliseconds() {
     struct timespec ts;
     clock_gettime(CLOCK_MONOTONIC, &ts);
     return (ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
   }
   ```
2. **Eliminar `_program_start_secs`:** Ya no es necesario contar desde el inicio del programa; se puede usar tiempo absoluto monotónico. Alternativamente, inicializar una variable global `_program_start_ms` al arrancar con `clock_gettime`.
3. **Revisar `cm-util.c`:** Eliminar `_program_start_secs` o adaptarlo.

### Verificación
- Compilar (`make clean && make`).
- Ejecutar normalmente. No debe haber cambios visibles.
- Test de estrés: suspender y reanudar el portátil. fastcompmgr debe seguir funcionando sin repintados erráticos.

---

## 🟡 9. Ring buffer de ignores puede crecer sin límite

**Archivo:** `cm-event.c`  
**Función:** `set_ignore()`  
**Líneas relevantes:** ~16-21.

### Descripción
```c
if(unlikely(isBufferFull(p_ignore_ringbuf))) {
    bufferIncrease(p_ignore_ringbuf, p_ignore_ringbuf->size*2);
}
```
`bufferIncrease` duplica el tamaño del ring buffer cada vez que se llena. Si hay una tormenta de errores X11 (driver buggy, ventanas destruyéndose masivamente), el buffer puede crecer hasta consumir toda la memoria disponible del proceso.

### Pasos de implementación
1. **Cap el tamaño máximo:**
   ```c
   #define IGNORE_RINGBUF_MAX 65536
   
   if(unlikely(isBufferFull(p_ignore_ringbuf))) {
       if (p_ignore_ringbuf->size < IGNORE_RINGBUF_MAX) {
           bufferIncrease(p_ignore_ringbuf, p_ignore_ringbuf->size*2);
       } else {
           // Forzar descarte de los más antiguos
           bufferReadSkip(p_ignore_ringbuf);
       }
   }
   ```
2. **Nota:** `bufferReadSkip` requiere que el buffer no esté vacío. Como `isBufferFull` es true, el buffer tiene al menos `size` elementos; descartar el más antiguo es seguro.

### Verificación
- Difícil de probar directamente sin un driver buggy. Se puede simular temporalmente bajando el tamaño inicial del buffer (ej. a 8) y generando muchos errores X11.

---

## 🟡 10. Código muerto `typedef struct _ignore`

**Archivo:** `fastcompmgr.c`  
**Líneas relevantes:** ~43-46.

### Descripción
```c
typedef struct _ignore {
  struct _ignore *next;
  unsigned long sequence;
} ignore;
```
Este tipo no se usa en ningún lugar. El sistema de ignore fue reemplazado por el ring buffer de `cm-event.c`. Confunde a herramientas de análisis estático y a los mantenedores.

### Pasos de implementación
1. **Eliminar las líneas 43-46.**

### Verificación
- `make clean && make` debe compilar sin errores ni warnings nuevos.

---

## 🟡 11. Subsistema de fading como código muerto activo

**Archivo:** `fastcompmgr.c`  
**Funciones:** `find_fade`, `dequeue_fade`, `enqueue_fade`, `cleanup_fade`, `set_fade`, `run_fades`, `fade_timeout`, y variables globales asociadas.

### Descripción
Aunque `AGENTS.md` dice *"Fading is broken and not maintained"*, todo el código sigue compilado y activo. Ocupa ~250 líneas. Variables globales como `fade_in_step`, `fade_out_step`, `fade_delta`, `fade_time`, `fades`, etc., consumen espacio y son tocadas por el código (aunque `run_fades` ya está comentado en el event loop).

### Pasos de implementación
**Opción A (Mínima y segura):** Envolver TODO el código de fading en `#if 0`.
- Incluir todas las funciones `find_fade` a `fade_timeout`.
- Incluir `typedef struct _fade`.
- Incluir variables globales `fade *fades;`, `fade_in_step`, etc.
- En `main()`, los flags `-D`, `-I`, `-O`, `-f`, `-F` pueden imprimir un warning:
  ```c
  case 'f':
  case 'F':
  case 'D':
  case 'I':
  case 'O':
      fprintf(stderr, "Warning: fading is disabled and not supported.\n");
      break;
  ```

**Opción B (Máxima limpieza):** Eliminar físicamente el código. Requiere más trabajo pero reduce la deuda técnica.

### Verificación
- `make clean && make`. Debe compilar.
- Ejecutar `./fastcompmgr -f`. Debe mostrar warning y funcionar normalmente sin fading.

---

## 🟡 12. Inserción O(n) en `add_win` para búsqueda de `prev`

**Archivo:** `fastcompmgr.c`  
**Función:** `add_win()`  
**Líneas relevantes:** ~1747-1754.

### Descripción
```c
for (p = &list; *p; p = &(*p)->next) {
    if ((*p)->id == prev && !(*p)->destroyed)
        break;
}
```
Para insertar una ventana nueva detrás de `prev`, se escanea linealmente toda la lista. Con N ventanas, es O(N) por creación. Aunque `find_win` es O(1) por hash table, esta búsqueda rompe la optimización.

### Pasos de implementación
1. **Usar la hash table para obtener `prev`:**
   ```c
   win *prev_win = NULL;
   if (prev) {
       prev_win = find_win(prev);   // O(1)
   }
   ```
2. **Insertar directamente:**
   ```c
   if (prev_win && !prev_win->destroyed) {
       // Insertar DESPUÉS de prev_win
       new->next = prev_win->next;
       new->prev = prev_win;
       if (prev_win->next) prev_win->next->prev = new;
       prev_win->next = new;
   } else {
       // Insertar al inicio (p = &list)
       new->next = list;
       new->prev = NULL;
       if (list) list->prev = new;
       list = new;
   }
   ```
   **Nota:** Hay que verificar la semántica exacta de `prev`. En X11 `CreateNotify`, `prev` es la ventana encima de la nueva en el stack. Si `prev == None`, la nueva ventana va al fondo. El código actual inserta `new` ANTES del nodo con `id == prev`. Hay que replicar exactamente esa semántica con punteros directos.

### Verificación
- Abrir 20 ventanas. Cerrar algunas del medio. Verificar que el orden Z de las ventanas es correcto (las que quedan detrás siguen detrás, las de delante siguen delante).

---

## 🟢 13. No hay manejo de `SIGPIPE`

**Archivo:** `fastcompmgr.c`  
**Función:** `main()`  
**Líneas relevantes:** Antes del event loop.

### Descripción
Si la conexión X11 se rompe, el socket subyacente puede cerrarse. Cualquier escritura posterior genera `SIGPIPE`, cuyo manejo por defecto termina el proceso.

### Pasos de implementación
1. **Añadir al inicio de `main()`:**
   ```c
   #include <signal.h>
   ...
   signal(SIGPIPE, SIG_IGN);
   ```
2. `signal.h` ya está incluido indirectamente en algunos sistemas, pero es mejor incluirlo explícitamente.

### Verificación
- Compilar y ejecutar. No hay test directo fácil para esto.

---

## 🟢 14. No hay limpieza de recursos X11 globales al salir

**Archivo:** `fastcompmgr.c`  
**Funciones:** `main()`, todo el ciclo de vida.

### Descripción
Al hacer `exit(0)` o `exit(1)` (por `SelectionClear`, error fatal, etc.), los recursos X11 globales nunca se liberan explícitamente:
- `all_damage`, `g_xregion_tmp` (regiones XFixes)
- `root_buffer`, `root_picture`, `root_tile` (pictures XRender)
- `black_picture`, `cshadow_picture`
- `g_alpha_pict_cache[256]` (pictures)
- `gaussian_map`, `shadow_corner`, `shadow_top` (memoria local)
- Damage handles de todas las ventanas (aunque `finish_destroy_win` libera los de ventanas destruidas correctamente, las que quedan al salir no se limpian).

En sesiones X11 de larga duración, esto puede dejar recursos huérfanos en el servidor X si fastcompmgr se reinicia frecuentemente (aunque no es el caso típico, es una mala práctica).

### Pasos de implementación
1. **Crear una función `cleanup_resources(Display *dpy)`:**
   ```c
   static void cleanup_resources(Display *dpy) {
       // Liberar recursos X11 globales
       if (all_damage) { XFixesDestroyRegion(dpy, all_damage); all_damage = None; }
       if (g_xregion_tmp) { XFixesDestroyRegion(dpy, g_xregion_tmp); g_xregion_tmp = None; }
       if (root_buffer) { XRenderFreePicture(dpy, root_buffer); root_buffer = None; }
       if (root_picture) { XRenderFreePicture(dpy, root_picture); root_picture = None; }
       if (root_tile) { XRenderFreePicture(dpy, root_tile); root_tile = None; }
       if (black_picture) { XRenderFreePicture(dpy, black_picture); black_picture = None; }
       if (cshadow_picture && cshadow_picture != black_picture) {
           XRenderFreePicture(dpy, cshadow_picture); cshadow_picture = None;
       }
       for (int i = 0; i < 256; i++) {
           if (g_alpha_pict_cache[i]) {
               XRenderFreePicture(dpy, g_alpha_pict_cache[i]);
               g_alpha_pict_cache[i] = None;
           }
       }
       if (g_border_alpha_pict) { XRenderFreePicture(dpy, g_border_alpha_pict); g_border_alpha_pict = None; }
       
       // Limpiar todas las ventanas restantes
       win *w = list;
       while (w) {
           win *next = w->next;
           // Forzar destrucción completa
           if (!w->destroyed) {
               w->destroyed = True;
               finish_destroy_win(dpy, w);
           }
           w = next;
       }
       
       // Memoria local
       free(gaussian_map);
       free(shadow_corner);
       free(shadow_top);
       free(expose_rects);
       
       XCloseDisplay(dpy);
   }
   ```
2. **Registrar con `atexit`:**
   ```c
   atexit(cleanup_resources);   // No puede pasar dpy; usar variable global g_dpy
   ```
   Nota: `atexit` no recibe argumentos. Como `g_dpy` es global, la función `cleanup_resources` puede usar `g_dpy` directamente sin parámetro.

### Verificación
- No hay verificación visual directa. Un profiler de recursos X11 (como `xrestop`) podría mostrar menos recursos huérfanos tras reiniciar fastcompmgr varias veces.

---

## 🟡 15. `BadWindow` no suprimido en `find_client_win()`

**Archivo:** `fastcompmgr.c`
**Función:** `find_client_win()`
**Líneas relevantes:** ~930-955.

### Descripción
`find_client_win()` llama `XQueryTree(dpy, win, ...)` recursivamente para localizar la ventana cliente con `WM_STATE`. Si la ventana se destruye entre el momento en que se decide buscarla y el momento de la llamada, el servidor X devuelve `error 3 (BadWindow) request 15 minor 0`.

A diferencia de `find_win_any_parent()` (que sí usa `set_ignore` antes de `XQueryTree`), `find_client_win()` no registra el serial del request en el ring buffer de ignores de `cm-event.c`. El handler `error()` lo detecta y lo imprime en stderr. Con ventanas efímeras (notificaciones, menús, diálogos), este ruido puede aparecer frecuentemente.

### Impacto
- **Funcional:** Ninguno. Es un error asíncrono benigno; el compositor sigue operando normalmente.
- **Estético:** Ruido en logs. No afecta rendimiento ni estabilidad.

### Fix propuesto (1 línea)
Añadir `set_ignore(dpy, NextRequest(dpy));` inmediatamente antes de `XQueryTree` en `find_client_win()`. Esto marca el serial en el ring buffer; si llega el error, `should_ignore` lo detecta y `error()` retorna silenciosamente.

```c
set_ignore(dpy, NextRequest(dpy));
if (!XQueryTree(dpy, win, &root, &parent, &children, &nchildren)) {
    return 0;
}
```

### Verificación
- Tras el cambio, los logs de `BadWindow` request 15 deben desaparecer o reducirse drásticamente.
- No debe haber impacto en la correctitud: si `XQueryTree` falla, `find_client_win` ya retorna `0` correctamente.

---

## Apéndice: Notas de supervisión general para el desarrollador

1. **Regla de oro:** Después de cada cambio, ejecutar `make clean && make`. No debe haber warnings nuevos.
2. **Test de humo mínimo:** Abrir Thunar, un terminal (Alacritty/URxvt), Chromium/Firefox. Mover, redimensionar, cerrar. No debe haber artefactos visuales.
3. **Test de estrés:** Abrir 30 ventanas, cerrar 15 al azar, verificar que no hay fugas de memoria (con `valgrind --leak-check=summary ./fastcompmgr ...`).
4. **Test de bloqueo:** Activar/desactivar light-locker (o el bloqueador de pantalla que use). fastcompmgr debe seguir ejecutándose al desbloquear (aunque si light-locker mata la sesión X11, el cierre es inevitable, pero ahora se loggeará claramente).
5. **No tocar el subsistema de fading** a menos que sea para eliminarlo (punto 11). Si un futuro mantenedor quiere revivir el fading, debería partir de cero, no del código roto actual.

---

*Fin del informe.*
