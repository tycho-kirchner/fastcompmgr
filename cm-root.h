#pragma once

#include <stdbool.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xrender.h>

extern Window root;
extern Picture root_picture;
extern Picture root_buffer;
extern int root_width;
extern int root_height;
extern const char *root_background_props[];
extern Atom atom_rootpmap_id;
extern Atom atom_xsetroot_id;


bool root_init();
Picture root_create_tile();
