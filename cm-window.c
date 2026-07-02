
#include <stdio.h>
#include <stdlib.h>

#include <X11/Xatom.h>

#include "cm-event.h"
#include "cm-root.h"
#include "cm-window.h"
#include "cm-global.h"
#include "cm-util.h"


win *list;

/* Simple open-addressing hash table: Window -> win* */
static win **win_hash = NULL;
static unsigned int win_hash_size = 0;
static unsigned int win_hash_count = 0;
static unsigned int win_hash_tombstones = 0;

#define HASH_INITIAL_SIZE 256
#define HASH_LOAD_FACTOR(num, den) ((num) >= ((den) * 3 / 4))

static unsigned int hash_window(Window id) {
  return (unsigned int)id;
}

static Bool win_hash_resize(void) {
  unsigned int new_size = win_hash_size ? win_hash_size * 2 : HASH_INITIAL_SIZE;
  win **new_hash = calloc(new_size, sizeof(win*));
  if (!new_hash) return False;
  for (unsigned int i = 0; i < win_hash_size; i++) {
    win *w = win_hash[i];
    if (w && w != (win*)1) {
      unsigned int idx = hash_window(w->id) & (new_size - 1);
      while (new_hash[idx]) {
        idx = (idx + 1) & (new_size - 1);
      }
      new_hash[idx] = w;
    }
  }
  free(win_hash);
  win_hash = new_hash;
  win_hash_size = new_size;
  win_hash_tombstones = 0;
  return True;
}

void win_hash_insert(win *w) {
  if (HASH_LOAD_FACTOR(win_hash_count + 1, win_hash_size)
      || win_hash_tombstones > win_hash_count) {
    if (!win_hash_resize()) {
      /* OOM: table may still have ~25% free slots because resize triggers at 75%.
       * Insertion will proceed, but guard against infinite loops below. */
    }
  }
  unsigned int idx = hash_window(w->id) & (win_hash_size - 1);
  unsigned int probes = 0;
  while (win_hash[idx] && win_hash[idx] != (win*)1) {
    if (win_hash[idx]->id == w->id) {
      win_hash[idx] = w; // replace
      return;
    }
    idx = (idx + 1) & (win_hash_size - 1);
    if (unlikely(++probes >= win_hash_size)) {
      /* Table is completely full of live entries. This should never happen
       * because resize triggers at 75%, but guard against pathological OOM. */
      return;
    }
  }
  win_hash[idx] = w;
  win_hash_count++;
}

void win_hash_remove(Window id) {
  if (!win_hash_size) return;
  unsigned int idx = hash_window(id) & (win_hash_size - 1);
  while (win_hash[idx]) {
    if (win_hash[idx] != (win*)1 && win_hash[idx]->id == id) {
      win_hash[idx] = (win*)1; // tombstone
      win_hash_count--;
      win_hash_tombstones++;
      return;
    }
    idx = (idx + 1) & (win_hash_size - 1);
  }
}

win* win_hash_lookup(Window id) {
  if (!win_hash_size) return NULL;
  unsigned int idx = hash_window(id) & (win_hash_size - 1);
  while (win_hash[idx]) {
    if (win_hash[idx] != (win*)1 && win_hash[idx]->id == id && !win_hash[idx]->destroyed) {
      return win_hash[idx];
    }
    idx = (idx + 1) & (win_hash_size - 1);
  }
  return NULL;
}

win* find_win_any_state(Window id) {
  if (!win_hash_size) return NULL;
  unsigned int idx = hash_window(id) & (win_hash_size - 1);
  while (win_hash[idx]) {
    if (win_hash[idx] != (win*)1 && win_hash[idx]->id == id) {
      return win_hash[idx];
    }
    idx = (idx + 1) & (win_hash_size - 1);
  }
  return NULL;
}

win* find_win(Window id) {
  return win_hash_lookup(id);
}


typedef struct _AtomArr {
  Atom *atoms;
  unsigned long n_items;
} AtomArr;


_Static_assert (sizeof(Atom) == sizeof(long),
                "_query_atom_values depends on long-sized atom. See XGetWindowProperty");

static AtomArr _query_atom_values(Window window, Atom property) {
  Atom actual_type;
  int actual_format;
  unsigned long n_items, bytes_after;
  Atom *atoms = NULL;
  AtomArr ret;

  set_ignore(g_dpy, NextRequest(g_dpy));
  int result = XGetWindowProperty(g_dpy, window, property, 0, (~0L), False,
                                  XA_ATOM, &actual_type, &actual_format,
                                  &n_items, &bytes_after, (unsigned char**)&atoms);
  if(result != Success || atoms == NULL){
    memset(&ret, 0, sizeof(AtomArr));
    return ret;
  }
  if(unlikely(actual_format != 32)){
    fprintf(stderr, "fastcompmgr error: expected actual_format==32, got %d\n",
            actual_format);
    XFree(atoms);
    memset(&ret, 0, sizeof(AtomArr));
    return ret;
  }
  ret.atoms = atoms;
  ret.n_items = n_items;
  return ret;
}


static bool win_has_atom(Window window, Atom atom){
  Atom type = None;
  int format;
  unsigned long nitems, after;
  unsigned char *data = NULL;
  int res;

  set_ignore(g_dpy, NextRequest(g_dpy));
  res = XGetWindowProperty(
    g_dpy, window, atom, 0, 0, False,
    AnyPropertyType, &type, &format, &nitems,
    &after, &data);
  if (likely(res == Success && data != NULL )) {
      XFree(data);
      if (likely(type)) return true;
  }
  return false;
}


win* find_win_any_parent(Window w) {
  Window root, parent;
  Window *children;
  win* res = NULL;
  unsigned int nchildren;
  // FIXME: find_win should rather return the window in constant time
  if((res=find_win(w)) != NULL){
      return res;
  }
  set_ignore(g_dpy, NextRequest(g_dpy));
  if (!XQueryTree(g_dpy, w, &root,
      &parent, &children, &nchildren)) {
    return NULL;
  }
  if (children) XFree((char *)children);
  if(parent == root){
      return NULL;
  }
  return find_win_any_parent(parent);
}


bool win_state_is_hidden(Window window) {
  AtomArr atom_arr;
  bool hidden;
  atom_arr = _query_atom_values(window, atom_net_wm_state);
  if(atom_arr.atoms == NULL) {
    return false;
  }
  hidden = false;
  for(unsigned long i=0; i<atom_arr.n_items; i++) {
    // After i3 restart in tabbed mode, a window may be _NET_WM_STATE_HIDDEN AND
    // _NET_WM_STATE_FOCUSED (a bug?), rendering it blank. Let's dissalow hidden
    // focused windows.
    if (atom_arr.atoms[i] == atom_net_wm_state_hidden) {
      hidden = true;
    } else if (atom_arr.atoms[i] == atom_net_wm_state_focused) {
      hidden = false;
      break;
    }
  }
  XFree(atom_arr.atoms);
  return hidden;
}


bool win_is_client(Window window){
  return win_has_atom(window, atom_wm_state);
}


void win_register_client_events(Window window)
{
  XSelectInput(g_dpy, window, PropertyChangeMask);
}
