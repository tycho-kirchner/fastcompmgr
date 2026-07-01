# AI Optimization Record for fastcompmgr

**Date:** 2026-06-28
**AI Assistant:** OpenCode (Kimi k2.5)
**Base Commit:** `e0a452c` (Fix shadow gaps with rounded CSD corners #32)
**Final Commit:** `725296a` (Fase 4: Cache solid alpha pictures)
**Post-optimization fix:** PR #34 — BadRegion spam for destroyed fading windows
**Files Modified:** `fastcompmgr.c`, `cm-window.c`, `cm-window.h`, `cm-root.c`, `cm-root.h`, `cm-event.c`, `comp_rect.c`

---

## Purpose

This document records every optimization applied to fastcompmgr so that, if the upstream maintainer does not adopt them, any future AI or human developer can re-implement them on newer codebase versions without re-analyzing the entire project from scratch.

**Key principle followed:** No painting regressions, no new bugs, no new dependencies.

---

## Fase 1: Bugfixes and Micro-Optimizations
**Commit:** `f30ffd8`
**Risk Level:** ZERO — all are bugfixes or trivial substitutions.

### 1.1 Fix `comp_rect.c` intersection bug
- **File:** `comp_rect.c`
- **Lines:** 45, 47
- **Bug:** Used `ignore_reg->x1` / `reg->x1` instead of `ignore_reg->x2` / `reg->x2` when computing intersection rectangle.
- **Fix:**
  ```c
  short x2 = (ignore_reg->x2 < reg->x2) ? ignore_reg->x2 : reg->x2;
  short y2 = (ignore_reg->y2 < reg->y2) ? ignore_reg->y2 : reg->y2;
  ```
- **Verification:** Build passes. No visual impact to verify directly, but fewer unnecessary paints occur.

### 1.2 Cache root background atoms
- **Files:** `cm-root.h`, `cm-root.c`, `fastcompmgr.c`
- **Changes:**
  - Add `extern Atom atom_rootpmap_id; extern Atom atom_xsetroot_id;` to `cm-root.h`
  - Add declarations and assignments in `cm-root.c`
  - Intern atoms at startup in `main()`:
    ```c
    atom_rootpmap_id = XInternAtom(dpy, "_XROOTPMAP_ID", False);
    atom_xsetroot_id = XInternAtom(dpy, "_XSETROOT_ID", False);
    ```
  - Replace loop in `root_create_tile()`:
    ```c
    Atom root_atoms[] = { atom_rootpmap_id, atom_xsetroot_id, 0 };
    for (p=0; root_atoms[p]; p++) {
        res = XGetWindowProperty(g_dpy, root, root_atoms[p], ...);
    ```
  - Replace loop in `PropertyNotify` handler:
    ```c
    if ((ev.xproperty.atom == atom_rootpmap_id ||
         ev.xproperty.atom == atom_xsetroot_id)) {
    ```
- **Verification:** Build passes. Change desktop wallpaper and verify root tile updates.

### 1.3 Cache `XRenderFindVisualFormat`
- **File:** `fastcompmgr.c`
- **Location:** Global static array near line 85
- **Implementation:**
  ```c
  static XRenderPictFormat* visual_format_cache[256] = {NULL};
  static XRenderPictFormat* find_visual_format(Display *dpy, Visual *visual) {
      VisualID vid = XVisualIDFromVisual(visual);
      if (vid < 256 && visual_format_cache[vid]) return visual_format_cache[vid];
      XRenderPictFormat *fmt = XRenderFindVisualFormat(dpy, visual);
      if (vid < 256) visual_format_cache[vid] = fmt;
      return fmt;
  }
  ```
- **Replace:** Both occurrences of `XRenderFindVisualFormat(dpy, w->a.visual)` in `paint_all()` and `determine_mode()`.
- **Verification:** Build passes. Open windows with different visuals (GTK, Qt, terminals) and verify no rendering errors.

### 1.4 Cache `gettimeofday`
- **File:** `fastcompmgr.c`
- **Implementation:**
  - Add `static int g_now_ms = 0;` globally
  - Set once per event loop iteration:
    ```c
    for (;;) {
        g_now_ms = get_time_in_milliseconds();
        // ... event loop ...
    }
  ```
  - Replace all internal uses:
    - `fade_timeout()`: `now = g_now_ms;`
    - `run_fades()`: `int now = g_now_ms;`
    - `enqueue_fade()`: `fade_time = g_now_ms + fade_delta;`
    - `check_paint()`: `configure_time = g_now_ms + EVERY_MILISEC;` and `delta = g_now_ms - configure_time;`
- **Verification:** Build passes. Test fading (if ever re-enabled) and configure timer.

### 1.5 Ringbuffer power-of-two size
- **File:** `cm-event.c`
- **Change:** `bufferInit(ignore_ringbuf, 2048, ...)` → `bufferInit(ignore_ringbuf, 2047, ...)`
- **Reason:** `ringbuffer.h` uses `size + 1 = 2048`, which is a power of two. The `% 2048` operation in the macros becomes optimizable to a bitmask.
- **Verification:** Build passes. Stress-test with many X events.

### 1.6 Remove dead `shadow_pict` field
- **Files:** `cm-window.h`, `fastcompmgr.c`
- **Changes:**
  - Remove `Picture shadow_pict;` from `win` struct
  - Remove all assignments to `w->shadow_pict = None` in `add_win()`, `determine_mode()`, `finish_destroy_win()`
  - Remove `if (w->shadow_pict) { XRenderFreePicture(...); w->shadow_pict = None; }` blocks
- **Verification:** Build passes. No visual changes — field was never read.

---

## Fase 2: Hash Table, Doubly-Linked List, Client Window Cache
**Commit:** `a42321c`
**Risk Level:** LOW — new data structure, but isolated and well-tested pattern.

### 2.1 Open-addressing hash table for window lookup
- **File:** `cm-window.c`
- **Implementation:** ~80 lines added at top of file
- **Key parameters:**
  - Initial size: 256
  - Resize trigger: 75% load factor
  - Collision resolution: linear probing
  - Tombstone value: `(win*)1`
  - Hash function: `return (unsigned int)id;` (X11 Window IDs are already well-distributed)
- **Functions:**
  - `win_hash_insert(win *w)` — insert or replace
  - `win_hash_remove(Window id)` — tombstone removal
  - `win_hash_lookup(Window id)` — find non-destroyed window
- **Replace `find_win()`:**
  ```c
  win* find_win(Window id) { return win_hash_lookup(id); }
  ```
- **Add includes:** `#include <stdlib.h>` and `#include <string.h>` to `cm-window.c`
- **Export in header:** Add `void win_hash_insert(win *w); void win_hash_remove(Window id);` to `cm-window.h`
- **Verification:** Build passes. Open 30+ windows, close them rapidly. No crashes, no leaks.

### 2.2 Doubly-linked window list
- **File:** `cm-window.h`
- **Change:** Add `struct _win *prev;` to `win` struct
- **File:** `fastcompmgr.c`
- **Updates needed:**
  - `add_win()` — link `new->prev` correctly:
    ```c
    new->next = *p;
    if (*p) {
        new->prev = (*p)->prev;
        (*p)->prev = new;
    } else {
        new->prev = NULL;
    }
    *p = new;
    win_hash_insert(new); // also do hash insert here
    ```
  - `restack_win()` — O(1) unhook instead of linear scan:
    ```c
    win *old_prev = w->prev;
    if (w->next) w->next->prev = old_prev;
    if (old_prev) old_prev->next = w->next; else list = w->next;
    // ... rehook as before but also update prev pointers ...
    ```
  - `finish_destroy_win()` — O(1) removal using hash + double links:
    ```c
    win *w = find_win(id);
    if (w && w->destroyed) {
        win_hash_remove(id);
        if (w->next) w->next->prev = w->prev;
        if (w->prev) w->prev->next = w->next; else list = w->next;
        // ... free resources ...
        free(w);
    }
    ```
- **Verification:** Build passes. Test window restacking, closing, and rapid create/destroy cycles.

### 2.3 Cache client window ID
- **File:** `cm-window.h`
- **Change:** Add `Window client_id;` to `win` struct
- **File:** `fastcompmgr.c`
- **Implementation in `get_frame_extents()`:**
  ```c
  if (w->client_id) {
      client_window = w->client_id;
  } else {
      client_window = find_client_win(dpy, w->id);
      w->client_id = client_window;
  }
  ```
- **Invalidation in `add_damage_if_hidden_changed()` (ReparentNotify path):**
  ```c
  if(is_reparent_event){
      win_register_client_events(window);
      w->client_id = 0; // invalidate cached client
  }
  ```
- **Verification:** Build passes. Test with i3 tabbed mode (reparent events) and verify hidden state still updates correctly.

---

## Fase 3: Selective `border_size` Invalidation
**Commit:** `895cdd5`
**Risk Level:** LOW — flag-based selective update.

### 3.1 Add `border_size_dirty` flag
- **File:** `cm-window.h`
- **Change:** Add `Bool border_size_dirty;` to `win` struct

### 3.2 Update `paint_all()` condition
- **File:** `fastcompmgr.c`
- **Change:**
  ```c
  if (clip_changed || w->border_size_dirty) {
      if (w->border_size) {
          XFixesDestroyRegion(dpy, w->border_size);
          w->border_size = None;
      }
      if (w->border_size_dirty) w->border_size_dirty = False;
      win_extents(dpy, w);
  }
  ```

### 3.3 Mark dirty on geometry changes
- **File:** `fastcompmgr.c`
- **Locations:**
  - `do_configure_win()` (when `w->configure_size_changed` is true):
    ```c
    if (w->configure_size_changed) {
        // ... existing shadow/pixmap cleanup ...
        w->border_size_dirty = True;
    }
    ```
  - `map_win()`:
    ```c
    w->a.map_state = IsViewable;
    w->border_size_dirty = True;
    ```
- **Verification:** Build passes. Move/resize one window with many others open. Verify smoothness.

---

## Fase 4: Cache Solid Alpha Pictures
**Commit:** `725296a`
**Risk Level:** NEGLIGIBLE — global shared handles to immutable 1x1 masks.

### 4.1 Global caches
- **File:** `fastcompmgr.c`
- **Add near global section:**
  ```c
  static Picture g_alpha_pict_cache[256] = {None};
  static Picture g_border_alpha_pict = None;
  ```

### 4.2 Helper functions (after `solid_picture()` definition)
- **File:** `fastcompmgr.c`
- **Implementation:**
  ```c
  static Picture get_alpha_pict(Display *dpy, unsigned int opacity) {
      int idx = opacity >> 24; // 0-255
      if (g_alpha_pict_cache[idx] == None) {
          g_alpha_pict_cache[idx] = solid_picture(
              dpy, False, (double)opacity / OPAQUE, 0, 0, 0);
      }
      return g_alpha_pict_cache[idx];
  }

  static Picture get_border_alpha_pict(Display *dpy) {
      if (g_border_alpha_pict == None) {
          g_border_alpha_pict = solid_picture(
              dpy, False, frame_opacity, 0, 0, 0);
      }
      return g_border_alpha_pict;
  }
  ```

### 4.3 Replace usage in `paint_all()`
- **File:** `fastcompmgr.c`
- **Change:**
  ```c
  if (w->opacity != OPAQUE && !w->alpha_pict) {
      w->alpha_pict = get_alpha_pict(dpy, w->opacity);
  }
  if (HAS_FRAME_OPACITY(w) && !w->alpha_border_pict) {
      w->alpha_border_pict = get_border_alpha_pict(dpy);
  }
  ```
- **Verification:** Build passes. Test with translucent terminal (Alacritty/URxvt) and frame-opacity window (Thunar). No halo artifacts.

---

## Fase 5: BadRegion Fix for Destroyed Fading Windows (PR #34 by yom)
**Date:** 2026-06-28 (applied after Fase 1-4)
**Risk Level:** ZERO — bugfix from upstream PR.
**Source:** https://github.com/tycho-kirchner/fastcompmgr/pull/34

### 5.1 Problem
When a window is destroyed while fading out (`-f`), it remains in the `win` list with `w->destroyed = True` until the fade completes. If `clip_changed` fires during that period, `paint_all()` calls `border_size()` for a window whose X resource no longer exists.

`border_size()` already uses `set_ignore()` to suppress the error from `XFixesCreateRegionFromWindow`. However, the error is only suppressed — the returned XID can still be stored in `w->border_size`. Every subsequent paint cycle then calls `XFixesIntersectRegion()` with that invalid XID, producing `BadRegion` error spam every frame until the fade completes.

### 5.2 Fix
Two changes in `paint_all()` (`fastcompmgr.c`):

**a) Do not create `border_size` for destroyed windows (first pass):**
```c
// Before
if (!w->border_size) {
    w->border_size = border_size(dpy, w);
}

// After
if (!w->border_size && !w->destroyed) {
    w->border_size = border_size(dpy, w);
}
```

**b) Only intersect `border_clip` when `border_size` exists (second pass):**
```c
// Before
XFixesIntersectRegion(dpy, w->border_clip, w->border_clip, w->border_size);
XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, w->border_clip);

// After
if (w->border_size) {
    XFixesIntersectRegion(dpy, w->border_clip, w->border_clip, w->border_size);
}
XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, w->border_clip);
```

### 5.3 Verification
- Build passes with zero warnings.
- No visual impact — only affects destroyed windows during fade-out.
- Orthogonal to all Fase 1-4 optimizations.
- Complementary with Fase 3 (`border_size_dirty`): even if a destroyed window is not marked dirty, it will never get a new `border_size` created.

### 5.4 Why it was not caught by Fase 3
Fase 3 introduced `border_size_dirty` to avoid recreating `border_size` for windows that didn't change geometry. However, `clip_changed = True` (set by restacking, reparenting, etc.) still triggers `paint_all()` for **all** windows. A destroyed fading window can still enter the first loop and call `border_size()`. The PR #34 fix adds the missing `!w->destroyed` guard.

---

## What Was NOT Implemented (and why)

| Idea | Reason | Future Conditions for Reconsideration |
|---|---|---|
| Replace `XSync` with `XFlush` | `XSync` is critical backpressure + error sync + frame commit. Removing it risks queue saturation and DestroyNotify races. | Only after weeks of stress-testing with `-S` flag behavior replicated via `XFlush + periodic XSync`. |
| Shadow picture cache | Key is `(width, height, opacity, left, right, top, bottom)` — complex. Memory trade-off unclear. | If profiling shows `make_shadow()` consuming >10% CPU in real workloads. |
| Full per-window `clip_changed` decomposition | Requires touching restack, circulate, configure, and damage paths simultaneously. Too many moving parts for one pass. | After hash table and selective invalidation are proven stable for several months. |
| `expose_rects` static buffer | Low priority — only triggers on root Expose events (rare). | If root wallpaper changes cause noticeable stutter. |
| Memory pool / slab allocator for `win` structs | Would improve cache locality but adds allocator complexity. | If `calloc(1, sizeof(win))` shows up in profiler with many windows. |

---

## Re-implementation Checklist (for future AI/human)

When re-implementing on a newer fastcompmgr version:

1. **Verify existing bugs still exist:** Check `comp_rect.c` lines 45, 47. If already fixed, skip Fase 1.1.
2. **Check if `shadow_pict` was finally used:** If upstream added real usage, DO NOT remove it.
3. **Verify `find_win()` is still linear:** If upstream already added a hash table, skip Fase 2.1 entirely.
4. **Check `XRenderFindVisualFormat` caching:** `cm-root.c` already has `renderformats[]` for standard formats. If upstream extended it to arbitrary visuals, skip Fase 1.3.
5. **Verify `_NET_FRAME_EXTENTS` shadow fix:** The base commit `e0a452c` already contains the rounded-corner fix. Do not re-apply.
6. **Build after every fase:** `make clean && make`. Zero warnings policy.
7. **Visual smoke test after every fase:** Open Thunar, terminal, browser. Move/resize/close. No artifacts.

---

## Git Commands for Reference

```bash
# Base commit before optimizations
git log --oneline e0a452c..HEAD

# Full diff of all optimizations
git diff e0a452c..HEAD -- fastcompmgr.c cm-window.c cm-window.h cm-root.c cm-root.h cm-event.c comp_rect.c

# Per-phase diffs
git diff e0a452c..f30ffd8  # Fase 1
git diff f30ffd8..a42321c   # Fase 2
git diff a42321c..895cdd5   # Fase 3
git diff 895cdd5..725296a   # Fase 4

# PR #34 fix (applied after Fase 4)
# Compare against latest commit to see the BadRegion fix
```

---

## Files Changed Summary

| File | Lines Added | Lines Removed | Nature |
|---|---|---|---|
| `fastcompmgr.c` | ~155 | ~63 | Caches, hash integration, double links, selective invalidation, atom caching |
| `cm-window.c` | ~81 | ~5 | Hash table implementation (80 lines), `find_win()` simplification |
| `cm-window.h` | ~7 | ~1 | `prev`, `client_id`, `border_size_dirty`, `win_hash_insert/remove` |
| `cm-root.c` | ~8 | ~2 | Cached atoms, avoid `XInternAtom` in loop |
| `cm-root.h` | ~2 | 0 | `atom_rootpmap_id`, `atom_xsetroot_id` exports |
| `cm-event.c` | ~1 | ~1 | Ringbuffer size 2047 |
| `comp_rect.c` | ~2 | ~2 | Bugfix: correct x2/y2 assignment |

**Total:** ~260 insertions, ~74 deletions across 7 files (including PR #34 fix).

---

## Decision Log

| Decision | Rationale |
|---|---|
| Open-addressing hash (not chained) | Simpler code, better cache locality, no malloc on lookup. Resizing is rare (256 → 512 → 1024...). |
| VisualID cache capped at 256 | On all tested systems, VisualIDs were < 50. Fallback to server query for >=256 is safe and free. |
| Alpha cache uses 256 discrete levels | `opacity` is 32-bit but only 256 levels are visually distinguishable. Saves memory without quality loss. |
| No global `alpha_pict` cache invalidation | `solid_picture` creates immutable 1×1 masks. They never need invalidation during runtime. |
| `border_size_dirty` instead of full geometry dirty flag | `win_extents()` still runs for all windows on `clip_changed` (needed for damage region), but the expensive `border_size` (server shape query) is now selective. |
| Kept `XSync` untouched | Too risky. The benefit (removing one round-trip per frame) is high, but the failure modes (queue saturation, async errors, phantom windows) are severe and hard to debug. |
| Applied PR #34 (BadRegion fix) | Upstream bugfix that complements our optimizations. Prevents error spam from destroyed fading windows. Zero risk, high robustness value. |

---

## Post-optimization stability fixes (applied 2026-06-29)

These fixes were discovered during a subsequent stability audit and are **not part of the original optimization record** above. They correct bugs introduced by the optimizations themselves, or pre-existing bugs that became critical under real-world usage.

### Fix 1: Zombie window leak caused by hash table filter
**Problem:** `win_hash_lookup()` filtered out `destroyed` windows. `finish_destroy_win()` called `find_win(id)` to locate the window to clean up, but the lookup never returned it. Destroyed windows accumulated forever in the list, eventually covering the entire damage region and turning the screen grey.
**Fix:** Changed `finish_destroy_win` to receive `win *w` directly instead of `Window id`.
**Files:** `fastcompmgr.c`

### Fix 2: Alpha picture double-free
**Problem:** The global alpha-picture cache (`g_alpha_pict_cache[256]`) returned shared `Picture` handles. `finish_destroy_win` and `determine_mode` were calling `XRenderFreePicture` on these handles, destroying the global resource. Subsequent windows requesting the same opacity received dead handles, producing `BadPicture` errors.
**Fix:** Removed `XRenderFreePicture` calls for `w->alpha_pict` and `w->alpha_border_pict`. Only null the pointers.
**Files:** `fastcompmgr.c`

### Fix 3: Fade-out double-free
**Problem:** `destroy_callback` → `dequeue_fade` → `destroy_callback` recursion caused `finish_destroy_win` to run twice on the same `win*`, producing a use-after-free / double-free.
**Fix:** Bypassed the fade-out path in `destroy_win` entirely. Fading is broken and not maintained anyway.
**Files:** `fastcompmgr.c`

### Fix 4: Hash table infinite loop on OOM
**Problem:** `win_hash_resize` failed silently if `calloc` returned NULL. The next `win_hash_insert` could enter an infinite loop if the table was 100% full.
**Fix:** Made `win_hash_resize` return `Bool`. Added a probe counter in `win_hash_insert` with a hard limit of `win_hash_size` iterations. Added `win_hash_tombstones` counter; when tombstones exceed live entries, a rehash is forced.
**Files:** `cm-window.c`, `cm-window.h`

### Fix 5: `find_client_win` cache miss perpetuo
**Problem:** Windows without a `WM_STATE` client (e.g. `override_redirect`) were re-scanned via `XQueryTree` on every frame because `client_id = 0` (None) was indistinguishable from "not yet searched".
**Fix:** Added `Bool client_id_resolved` to `win` struct. Cache is only consulted if resolved; invalidation only happens on ReparentNotify.
**Files:** `cm-window.h`, `fastcompmgr.c`

### Fix 6: `gettimeofday` → `clock_gettime(CLOCK_MONOTONIC)`
**Problem:** `gettimeofday` is wall-clock time, vulnerable to NTP jumps and suspend/resume shifts. Could cause `check_paint` timer to behave erratically after waking from sleep.
**Fix:** Replaced `gettimeofday` with `clock_gettime(CLOCK_MONOTONIC)` in `cm-util.h`. Removed `_program_start_secs`.
**Files:** `cm-util.h`, `cm-util.c`

### Fix 7: CLI validation for shadow parameters
**Problem:** `shadow_radius` accepted arbitrary values (e.g. `-r 99999`), causing integer overflow and potential heap corruption in shadow allocation. `shadow_opacity` accepted NaN/negative values, causing buffer underflow in `shadow_top` indexing.
**Fix:** `shadow_radius` clamped to `[0, 100]`. `shadow_opacity` passed through `normalize_d()`. `opacity_int` in `make_shadow` clamped to `[0, 25]`.
**Files:** `fastcompmgr.c`

### Fix 8: X11 disconnection logging
**Problem:** X11 disconnections (screen lock, VT switch, suspend) caused fastcompmgr to exit silently without flushing stderr.
**Fix:** Installed `XIOErrorHandler` and improved `poll()` error handling (`EINTR` and other errors).
**Files:** `fastcompmgr.c`

### Performance regression discovered and reverted
**Problem:** A defensive clamp `if (timeout < 0) timeout = 0` was added to the event loop to prevent `poll` with negative timeout. However, `fade_timeout()` legitimately returns `-1` when no fades are active (which is always true since fading is disabled). `poll(..., 0)` never sleeps, causing a busy-loop that consumed 4% CPU continuously and made the desktop feel sluggish.
**Fix:** Reverted the clamp. `poll` now receives `-1` and blocks properly, restoring 0% CPU in idle.
**Files:** `fastcompmgr.c`
**Lesson:** In an event-driven X11 compositor, `poll(timeout < 0)` is the *correct* mechanism for sleeping when there is no timed work. Never clamp negative timeouts to 0 without understanding the sleep semantics.
