#pragma once

#include <stdbool.h>
#include <X11/Xlib.h>

bool event_init(void);

void set_ignore(Display *dpy, unsigned long sequence);
int should_ignore(Display *dpy, unsigned long sequence);
void discard_ignore(Display *dpy, unsigned long sequence);
