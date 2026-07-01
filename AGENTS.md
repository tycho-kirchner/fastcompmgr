# fastcompmgr

## Build
- Simple Makefile project. **No autotools** despite `.gitignore` remnants.
- `make` builds the binary; `make install` copies to `PREFIX` (default `/usr/local`).
- `make clean` removes `*.o` and the binary.
- Default `CFLAGS` include `-O2 -flto -pipe -Wall -fno-plt`. Expect link-time optimization.

## Dependencies
Install dev packages for: `libx11`, `libxcomposite`, `libxfixes`, `libxdamage`, `libxrender`, plus `pkg-config` and `make`.

## Architecture
- `fastcompmgr.c` — `main()`, argument parsing, X11 extension init, and the **poll-based event loop**. All compositing logic lives here.
- `cm-root.c/h` — root window state (`root_picture`, `root_buffer`, background tile).
- `cm-global.c/h` — X atoms and global `Display*` / screen index.
- `cm-window.c/h` — window list, `win` struct, window-type and hidden-state enums, event registration. **Also contains an open-addressing hash table** that makes `find_win()` O(1).
- `cm-event.c/h` — event-ignore tracking (`set_ignore` / `should_ignore` / `discard_ignore`).
- `cm-util.c/h` — `likely`/`unlikely` builtins, `READ_ONCE`/`WRITE_ONCE` macros, millisecond time helper.
- `comp_rect.c/h` — occlusion check (`rect_paint_needed`) to skip painting occluded windows.
- `ringbuffer.h` — third-party ring buffer macros (MIT, Philip Thrasher).

### AI optimizations already applied
This codebase contains **AI-applied performance optimizations** not present in upstream. See `AI_OPTIMIZATIONS.md` for the full record. Key changes already in place:
- Window lookup is an open-addressing hash table (not linear scan).
- Window list is doubly-linked for O(1) restack and removal.
- `XRenderFindVisualFormat` is cached by VisualID.
- Solid alpha pictures are cached at 256 discrete levels.
- Root background atoms are cached at startup.
- `border_size_dirty` flag avoids expensive server shape queries on every paint.
- Time is read once per event-loop iteration using `clock_gettime(CLOCK_MONOTONIC)` (replaced `gettimeofday`).

### Stability fixes applied (not in upstream)
- **Zombie window leak fixed**: `find_win` filtering `!destroyed` prevented `finish_destroy_win` from ever cleaning up destroyed windows. Fixed by passing the `win*` pointer directly.
- **Alpha picture double-free fixed**: The global alpha-picture cache (Fase 4) was being freed per-window in `finish_destroy_win` and `determine_mode`. Fixed by only nulling the pointer, never freeing shared cache entries.
- **Fade-out double-free eliminated**: `destroy_callback` -> `dequeue_fade` -> `destroy_callback` recursion caused `finish_destroy_win` to run twice on the same `win*`. The fade-out path in `destroy_win` is now bypassed entirely (fading is broken anyway).
- **Hash table OOM resilience**: `win_hash_resize` no longer fails silently. `win_hash_insert` guards against infinite loops with a probe limit if the table ever fills completely.
- **Hash table tombstone rehash**: `win_hash_remove` leaves tombstones; `win_hash_insert` forces a rehash when tombstones exceed live entries, preventing O(n) lookup degradation over long sessions.
- **X11 disconnection robustness**: `poll()` handles `EINTR` and errors gracefully. An `XIOErrorHandler` is installed so X11 disconnections (screen lock, VT switch, suspend) are logged clearly instead of vanishing silently.
- **`find_client_win` cache miss fixed**: Added `client_id_resolved` flag to `win` struct so windows without a client window (e.g. `override_redirect`) are not re-scanned via `XQueryTree` on every frame.
- **CLI input validation**: `shadow_radius` is clamped to `[0, 100]`. `shadow_opacity` uses `normalize_d()`. `opacity_int` in `make_shadow` is clamped to `[0, 25]` to prevent buffer underflow on NaN/negative inputs.
- **Monotonic clock**: `gettimeofday` (wall-clock, vulnerable to NTP jumps) replaced by `clock_gettime(CLOCK_MONOTONIC)` for all internal timers (`check_paint`, configure debounce).

## Key conventions
- Code uses GCC builtins: `__builtin_expect`, `typeof` (GNU C).
- Macros `likely`/`unlikely` and `READ_ONCE`/`WRITE_ONCE` are used for hot-path optimization.
- `WINTYPE_UNKNOWN`, `SHADOW_UNKNOWN`, and `HIDDEN_UNKNOWN` must remain the first enum value because `add_win` optimizes initialization by zeroing the struct.
- `CAN_DO_USABLE` is hardcoded to `0`; all `usable` / `damage_bounds` code is dead.

## Known constraints
- **Fading is broken** and not maintained; do not try to fix it unless explicitly asked.
- **No test suite, CI, or linting setup exists.** Verify changes with `make clean && make`.
- No pre-commit hooks or automated checks.

## How to run
Typical invocation:
```bash
fastcompmgr -o 0.4 -r 12 -c -C
```
Use `-S` for synchronous X11 mode when debugging.

## Known issues resolved in this branch
- **Zombie/double-add race condition (SIGSEGV):** The anti-duplicate guard in `add_win()` used `find_win()`, which filters out `destroyed` entries. When a window was `destroyed` but not yet freed, `add_win()` could create a second `win*` for the same Window ID, causing `paint_all()` to traverse freed memory. Fixed by adding `find_win_any_state()` which skips the `destroyed` filter, and using it exclusively for the anti-duplicate check in `add_win()`.
- **Restack `prev` pointer corruption causing grey regions:** `restack_win()` set `w->prev = NULL` when inserting at the end of the list (new_above not found). The `finish_destroy_win()` then set `list = NULL` if `w->prev == NULL`, wiping the entire window list and causing `paint_all()` to paint nothing → grey screen. Fixed by tracking `new_pred` during the rehook loop and setting `w->prev = new_pred` when inserting at the end.
