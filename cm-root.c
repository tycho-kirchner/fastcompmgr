
#include <stdio.h>
#include <stdbool.h>
#include <string.h>

#include "cm-root.h"
#include "cm-global.h"

Window root;
Picture root_picture;
Picture root_buffer;
int root_width;
int root_height;

const char *root_background_props[] = {
  "_XROOTPMAP_ID",
  "_XSETROOT_ID",
  0
};


static inline int
_get_valid_pixmap_depth(Pixmap pxmap) {
  if (!pxmap) return 0;

  Window rroot = None;
  int rx = 0, ry = 0;
  unsigned rwid = 0, rhei = 0, rborder = 0, rdepth = 0;
  // In some window managers without managed desktops or also in some versions of
  // xfce (4.18), the found pixmap is invalid having a size of zero.
  bool is_valid =  XGetGeometry(g_dpy, pxmap, &rroot, &rx, &ry,
        &rwid, &rhei, &rborder, &rdepth) && rwid && rhei;
  if(is_valid){
    return rdepth;
  }
  return 0;
}


// XRenderFind(Standard)Format() is a roundtrip, so cache the results
static XRenderPictFormat* renderformats[ 33 ] = {NULL};

static Picture _create_background_pict(Pixmap pix, int depth)
{
  XRenderPictureAttributes pa;
  // Stay safe, and do not cache the fallback render format without further research.
  renderformats[0] = NULL;
  if (renderformats[depth] == NULL) {
    switch(depth){
      case 0:
          break;
      case 1:
          renderformats[1] = XRenderFindStandardFormat(g_dpy, PictStandardA1);
          break;
      case 8:
          renderformats[8] = XRenderFindStandardFormat(g_dpy, PictStandardA8);
          break;
      case 24:
          renderformats[24] = XRenderFindStandardFormat(g_dpy, PictStandardRGB24);
          break;
      case 32:
          renderformats[32] = XRenderFindStandardFormat(g_dpy, PictStandardARGB32);
          break;
      default: {
          fprintf(stderr, "Unhandled root background depth %d - please report!\n", depth);
          break;
      }
    }
    if (renderformats[depth] == NULL) {
      // Use renderformats[0] for all fallback-depths
      depth = 0;
      renderformats[0] = XRenderFindVisualFormat(g_dpy, DefaultVisual(g_dpy, g_screen));
    }
  }

  pa.repeat = True;
  return XRenderCreatePicture(g_dpy, pix, renderformats[depth], CPRepeat, &pa);
}

bool root_init(){
  XRenderPictureAttributes pa;
  root_width = DisplayWidth(g_dpy, g_screen);
  root_height = DisplayHeight(g_dpy, g_screen);

  pa.subwindow_mode = IncludeInferiors;
  root_picture = XRenderCreatePicture(g_dpy, root,
    XRenderFindVisualFormat(g_dpy, DefaultVisual(g_dpy, g_screen)),
    CPSubwindowMode, &pa);
  return true;
}

/// Create the root background picture. First check, if the root window already
/// has a valid corresponding pixmap. If so, do not overwrite it, such that e.g.
/// openbox's root background image is preserved. Create the picture using the
/// same depth, otherwise we're flooded with errors like
/// "error 143 (BadPicture) request 139 minor 8 serial 78698". If no valid
/// background pixmap is found, we create one ourselves using DefaultVisual()
/// and set a fixed solid background color.
Picture root_create_tile() {
  Picture picture;
  Atom actual_type;
  Pixmap pixmap;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  unsigned char *prop;
  unsigned pict_depth = 0;
  bool fill;
  int p;
  int res;
  const char* valid_pix_str;

  pixmap = None;

  for (p=0; root_background_props[p]; p++) {
    prop = NULL;
    res = XGetWindowProperty(g_dpy, root,
          XInternAtom(g_dpy, root_background_props[p], False),
          0, 4, False, AnyPropertyType, &actual_type,
          &actual_format, &nitems, &bytes_after, &prop);
    if (res != Success || prop == NULL ){
      continue;
    }
    if(actual_type == atom_pixmap
          && actual_format == 32 && nitems == 1) {
      memcpy(&pixmap, prop, 4);
    }
    XFree(prop);
    pict_depth = _get_valid_pixmap_depth(pixmap);
    if(pict_depth){
      break;
    } else {
      pixmap = None;
    }
  }

  if(pixmap == None){
    valid_pix_str = "invalid";
    pixmap = XCreatePixmap(g_dpy, root, 1, 1, DefaultDepth(g_dpy, g_screen));
    fill = true;
  } else {
    valid_pix_str = "valid";
    fill = false;
  }
  fprintf(stderr, "info: root background pixmap is %s.\n", valid_pix_str);
  picture = _create_background_pict(pixmap, pict_depth);

  if (fill) {
    XRenderColor  c;
    c.red = c.green = c.blue = 0x8080;
    c.alpha = 0xffff;
    XRenderFillRectangle(
      g_dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
  }
  return picture;
}


