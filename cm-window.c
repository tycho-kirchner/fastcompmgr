
#include <stdio.h>

#include <X11/Xatom.h>

#include "cm-window.h"
#include "cm-global.h"

win *list;

win* find_win(Window id) {
  win *w;
  for (w = list; w; w = w->next) {
    if (w->id == id && !w->destroyed)
      return w;
  }
  return NULL;
}
