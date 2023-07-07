/*
 * compton - a compositor for X11
 *
 * based on xcompmgr - copyright (c) 2003, keith packard
 *
 * copyright (c) 2011, christopher jeffrey
 * See LICENSE for more information.
 *
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <sys/poll.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <getopt.h>

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

#define likely(x)       __builtin_expect(!!(x), 1)
#define unlikely(x)     __builtin_expect(!!(x), 0)


#if COMPOSITE_MAJOR > 0 || COMPOSITE_MINOR >= 2
#define HAS_NAME_WINDOW_PIXMAP 1
#endif

#define CAN_DO_USABLE 0

typedef enum {
  WINTYPE_UNKNOWN, // MUST ALWAYS STAY first, due to init optimization in add_win
  WINTYPE_DESKTOP,
  WINTYPE_DOCK,
  WINTYPE_TOOLBAR,
  WINTYPE_MENU,
  WINTYPE_UTILITY,
  WINTYPE_SPLASH,
  WINTYPE_DIALOG,
  WINTYPE_NORMAL,
  WINTYPE_DROPDOWN_MENU,
  WINTYPE_POPUP_MENU,
  WINTYPE_TOOLTIP,
  WINTYPE_NOTIFY,
  WINTYPE_COMBO,
  WINTYPE_DND,
  NUM_WINTYPES
} wintype;

typedef struct _ignore {
  struct _ignore *next;
  unsigned long sequence;
} ignore;

typedef struct _win {
  struct _win *next;
  Window id;
#if HAS_NAME_WINDOW_PIXMAP
  Pixmap pixmap;
#endif
  XWindowAttributes a;
#if CAN_DO_USABLE
  Bool usable; /* mapped and all damaged at one point */
  XRectangle damage_bounds; /* bounds of damage */
#endif
  int mode;
  int damaged;
  Damage damage;
  Picture picture;
  Picture alpha_pict;
  Picture alpha_border_pict;
  Picture shadow_pict;
  XserverRegion border_size;
  XserverRegion extents;
  Picture shadow;
  int shadow_dx;
  int shadow_dy;
  int shadow_width;
  int shadow_height;
  unsigned int opacity;
  wintype window_type;
  unsigned long damage_sequence; /* sequence when damage was created */
  Bool destroyed;
  unsigned int left_width;
  unsigned int right_width;
  unsigned int top_width;
  unsigned int bottom_width;

  Bool need_configure;
  XConfigureEvent queue_configure;

  /* for drawing translucent windows */
  XserverRegion border_clip;
  struct _win *prev_trans;
} win;

typedef struct _conv {
  int size;
  double *data;
} conv;

typedef struct _fade {
  struct _fade *next;
  win *w;
  double cur;
  double finish;
  double step;
  void (*callback) (Display *dpy, win *w);
  Display *dpy;
} fade;

win *list;
fade *fades;
Display *dpy;
int scr;
Window root;
Picture root_picture;
Picture root_buffer;
Picture black_picture;
Picture root_tile;
XserverRegion all_damage;
Bool clip_changed;
#if HAS_NAME_WINDOW_PIXMAP
Bool has_name_pixmap;
#endif
int root_height, root_width;
ignore *ignore_head, **ignore_tail = &ignore_head;
int xfixes_event, xfixes_error;
int damage_event, damage_error;
int composite_event, composite_error;
int render_event, render_error;
Bool synchronize;
int composite_opcode;

/* find these once and be done with it */
Atom atom_opacity;
Atom atom_win_type;
Atom atom_pixmap;
Atom atom_wm_state;
Atom atom_net_frame_extents;
Atom win_type[NUM_WINTYPES];
double win_type_opacity[NUM_WINTYPES];
Bool win_type_shadow[NUM_WINTYPES];
Bool win_type_fade[NUM_WINTYPES];

#define REGISTER_PROP "_NET_WM_CM_S"

#define OPAQUE 0xffffffff

conv *gaussian_map;

#define WINDOW_SOLID 0
#define WINDOW_TRANS 1
#define WINDOW_ARGB 2

#ifndef DEBUG_REPAINT
#define DEBUG_REPAINT 0
#endif
#ifndef DEBUG_EVENTS
#define DEBUG_EVENTS 0
#endif
#define MONITOR_REPAINT 0

static void
determine_mode(Display *dpy, win *w);

static double
get_opacity_percent(Display *dpy, win *w);
static void
do_configure_win(Display *dpy, win* w);

static XserverRegion
win_extents(Display *dpy, win *w);

int shadow_radius = 12;
int shadow_offset_x = -15;
int shadow_offset_y = -15;
double shadow_opacity = .75;

double fade_in_step = 0.028;
double fade_out_step = 0.03;
int fade_delta = 10;
int fade_time = 0;
Bool fade_trans = False;

double inactive_opacity = 0;
double frame_opacity = 0;

#define INACTIVE_OPACITY \
(unsigned long)((double)inactive_opacity * OPAQUE)

#define IS_NORMAL_WIN(w) \
((w) && ((w)->window_type == WINTYPE_NORMAL \
         || (w)->window_type == WINTYPE_UTILITY))

#define HAS_FRAME_OPACITY(w) (frame_opacity && (w)->top_width)

/* For shadow precomputation */
int Gsize = -1;
unsigned char *shadow_corner = NULL;
unsigned char *shadow_top = NULL;

int
get_time_in_milliseconds() {
  struct timeval tv;

  gettimeofday(&tv, NULL);

  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

fade *
find_fade(win *w) {
  fade *f;

  for (f = fades; f; f = f->next) {
    if (f->w == w) return f;
  }

  return 0;
}

void
dequeue_fade(Display *dpy, fade *f) {
  fade **prev;

  for (prev = &fades; *prev; prev = &(*prev)->next) {
    if (*prev == f) {
      *prev = f->next;
      if (f->callback) {
        (*f->callback)(dpy, f->w);
      }
      free(f);
      break;
    }
  }
}

void
cleanup_fade(Display *dpy, win *w) {
  fade *f = find_fade (w);
  if (f) {
    dequeue_fade(dpy, f);
  }
}

void
enqueue_fade(Display *dpy, fade *f) {
  if (!fades) {
    fade_time = get_time_in_milliseconds() + fade_delta;
  }
  f->next = fades;
  fades = f;
}

static void
set_fade(Display *dpy, win *w, double start,
         double finish, double step,
         void(*callback) (Display *dpy, win *w),
         Bool exec_callback, Bool override) {
  fade *f;

  f = find_fade(w);
  if (!f) {
    f = malloc(sizeof(fade));
    f->next = 0;
    f->w = w;
    f->cur = start;
    enqueue_fade(dpy, f);
  } else if (!override) {
    return;
  } else {
    if (exec_callback && f->callback) {
      (*f->callback)(dpy, f->w);
    }
  }

  if (finish < 0) finish = 0;
  if (finish > 1) finish = 1;
  f->finish = finish;

  if (f->cur < finish) {
    f->step = step;
  } else if (f->cur > finish) {
    f->step = -step;
  }

  f->callback = callback;
  w->opacity = f->cur * OPAQUE;

#if 0
  printf("set_fade start %g step %g\n", f->cur, f->step);
#endif

  determine_mode(dpy, w);

  if (w->shadow) {
    XRenderFreePicture(dpy, w->shadow);
    w->shadow = None;

    if (w->extents != None) {
      XFixesDestroyRegion(dpy, w->extents);
    }

    /* rebuild the shadow */
    w->extents = win_extents(dpy, w);
  }

  /* fading windows need to be drawn, mark
     them as damaged.  when a window maps,
     if it tries to fade in but it already
     at the right opacity (map/unmap/map fast)
     then it will never get drawn without this
     until it repaints */
  w->damaged = 1;
}

int
fade_timeout(void) {
  int now;
  int delta;

  if (!fades) return -1;

  now = get_time_in_milliseconds();
  delta = fade_time - now;

  if (delta < 0) delta = 0;
/* printf("timeout %d\n", delta); */

  return delta;
}

void
run_fades(Display *dpy) {
  int now = get_time_in_milliseconds();
  fade *next = fades;
  int steps;
  Bool need_dequeue;

#if 0
  printf("run fades\n");
#endif

  if (fade_time - now > 0) return;
  steps = 1 + (now - fade_time) / fade_delta;

  while (next) {
    fade *f = next;
    win *w = f->w;
    next = f->next;

    f->cur += f->step * steps;
    if (f->cur >= 1) {
      f->cur = 1;
    } else if (f->cur < 0) {
      f->cur = 0;
    }

#if 0
    printf("opacity now %g\n", f->cur);
#endif

    w->opacity = f->cur * OPAQUE;
    need_dequeue = False;
    if (f->step > 0) {
      if (f->cur >= f->finish) {
        w->opacity = f->finish * OPAQUE;
        need_dequeue = True;
      }
    } else {
      if (f->cur <= f->finish) {
        w->opacity = f->finish * OPAQUE;
        need_dequeue = True;
      }
    }

    determine_mode(dpy, w);

    if (w->shadow) {
      XRenderFreePicture(dpy, w->shadow);
      w->shadow = None;

      if (w->extents != None) {
        XFixesDestroyRegion(dpy, w->extents);
      }

      /* rebuild the shadow */
      w->extents = win_extents(dpy, w);
    }

    /* Must do this last as it might
       destroy f->w in callbacks */
    if (need_dequeue) dequeue_fade(dpy, f);
  }

  fade_time = now + fade_delta;
}

static double
gaussian(double r, double x, double y) {
  return ((1 / (sqrt(2 * M_PI * r))) *
      exp((- (x * x + y * y)) / (2 * r * r)));
}


static conv *
make_gaussian_map(Display *dpy, double r) {
  conv *c;
  int size = ((int) ceil((r * 3)) + 1) & ~1;
  int center = size / 2;
  int x, y;
  double t;
  double g;

  c = malloc(sizeof(conv) + size * size * sizeof(double));
  c->size = size;
  c->data = (double *) (c + 1);
  t = 0.0;

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      g = gaussian(r, (double) (x - center), (double) (y - center));
      t += g;
      c->data[y * size + x] = g;
    }
  }

/*  printf("gaussian total %f\n", t); */

  for (y = 0; y < size; y++) {
    for (x = 0; x < size; x++) {
      c->data[y * size + x] /= t;
    }
  }

  return c;
}

/*
 * A picture will help
 *
 *      -center   0                width  width+center
 *  -center +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *        0 +-----+-------------------+-----+
 *          |     |                   |     |
 *          |     |                   |     |
 *          |     |                   |     |
 *   height +-----+-------------------+-----+
 *          |     |                   |     |
 * height+  |     |                   |     |
 *  center  +-----+-------------------+-----+
 */

static unsigned char
sum_gaussian(conv *map, double opacity,
             int x, int y, int width, int height) {
  int fx, fy;
  double *g_data;
  double *g_line = map->data;
  int g_size = map->size;
  int center = g_size / 2;
  int fx_start, fx_end;
  int fy_start, fy_end;
  double v;

  /*
   * Compute set of filter values which are "in range",
   * that's the set with:
   *    0 <= x + (fx-center) && x + (fx-center) < width &&
   *  0 <= y + (fy-center) && y + (fy-center) < height
   *
   *  0 <= x + (fx - center)    x + fx - center < width
   *  center - x <= fx    fx < width + center - x
   */

  fx_start = center - x;
  if (fx_start < 0) fx_start = 0;
  fx_end = width + center - x;
  if (fx_end > g_size) fx_end = g_size;

  fy_start = center - y;
  if (fy_start < 0) fy_start = 0;
  fy_end = height + center - y;
  if (fy_end > g_size) fy_end = g_size;

  g_line = g_line + fy_start * g_size + fx_start;

  v = 0;

  for (fy = fy_start; fy < fy_end; fy++) {
    g_data = g_line;
    g_line += g_size;

    for (fx = fx_start; fx < fx_end; fx++) {
      v += *g_data++;
    }
  }

  if (v > 1) v = 1;

  return ((unsigned char) (v * opacity * 255.0));
}

/* precompute shadow corners and sides
   to save time for large windows */
static void
presum_gaussian(conv *map) {
  int center = map->size/2;
  int opacity, x, y;

  Gsize = map->size;

  if (shadow_corner) free((void *)shadow_corner);
  if (shadow_top) free((void *)shadow_top);

  shadow_corner = (unsigned char *)(malloc((Gsize + 1) * (Gsize + 1) * 26));
  shadow_top = (unsigned char *)(malloc((Gsize + 1) * 26));

  for (x = 0; x <= Gsize; x++) {
    shadow_top[25 * (Gsize + 1) + x] =
      sum_gaussian(map, 1, x - center, center, Gsize * 2, Gsize * 2);

    for (opacity = 0; opacity < 25; opacity++) {
      shadow_top[opacity * (Gsize + 1) + x] =
        shadow_top[25 * (Gsize + 1) + x] * opacity / 25;
    }

    for (y = 0; y <= x; y++) {
      shadow_corner[25 * (Gsize + 1) * (Gsize + 1) + y * (Gsize + 1) + x]
        = sum_gaussian(map, 1, x - center, y - center, Gsize * 2, Gsize * 2);
      shadow_corner[25 * (Gsize + 1) * (Gsize + 1) + x * (Gsize + 1) + y]
        = shadow_corner[25 * (Gsize + 1) * (Gsize + 1) + y * (Gsize + 1) + x];

      for (opacity = 0; opacity < 25; opacity++) {
        shadow_corner[opacity * (Gsize + 1) * (Gsize + 1)
                      + y * (Gsize + 1) + x]
          = shadow_corner[opacity * (Gsize + 1) * (Gsize + 1)
                          + x * (Gsize + 1) + y]
          = shadow_corner[25 * (Gsize + 1) * (Gsize + 1)
                          + y * (Gsize + 1) + x] * opacity / 25;
      }
    }
  }
}

static XImage *
make_shadow(Display *dpy, double opacity,
            int width, int height) {
  XImage *ximage;
  unsigned char *data;
  int gsize = gaussian_map->size;
  int ylimit, xlimit;
  int swidth = width + gsize;
  int sheight = height + gsize;
  int center = gsize / 2;
  int x, y;
  unsigned char d;
  int x_diff;
  int opacity_int = (int)(opacity * 25);

  data = malloc(swidth * sheight * sizeof(unsigned char));
  if (!data) return 0;

  ximage = XCreateImage(
    dpy, DefaultVisual(dpy, DefaultScreen(dpy)), 8,
    ZPixmap, 0, (char *) data, swidth, sheight, 8,
    swidth * sizeof(unsigned char));

  if (!ximage) {
    free(data);
    return 0;
  }

  /*
   * Build the gaussian in sections
   */

  /*
   * center (fill the complete data array)
   */

  if (Gsize > 0) {
    d = shadow_top[opacity_int * (Gsize + 1) + Gsize];
  } else {
    d = sum_gaussian(gaussian_map,
      opacity, center, center, width, height);
  }

  memset(data, d, sheight * swidth);

  /*
   * corners
   */

  ylimit = gsize;
  if (ylimit > sheight / 2) ylimit = (sheight + 1) / 2;

  xlimit = gsize;
  if (xlimit > swidth / 2) xlimit = (swidth + 1) / 2;

  for (y = 0; y < ylimit; y++)
    for (x = 0; x < xlimit; x++) {
      if (xlimit == Gsize && ylimit == Gsize) {
        d = shadow_corner[opacity_int * (Gsize + 1) * (Gsize + 1)
                          + y * (Gsize + 1) + x];
      } else {
        d = sum_gaussian(gaussian_map,
          opacity, x - center, y - center, width, height);
      }
      data[y * swidth + x] = d;
      data[(sheight - y - 1) * swidth + x] = d;
      data[(sheight - y - 1) * swidth + (swidth - x - 1)] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }

  /*
   * top/bottom
   */

  x_diff = swidth - (gsize * 2);
  if (x_diff > 0 && ylimit > 0) {
    for (y = 0; y < ylimit; y++) {
      if (ylimit == Gsize) {
        d = shadow_top[opacity_int * (Gsize + 1) + y];
      } else {
        d = sum_gaussian(gaussian_map,
          opacity, center, y - center, width, height);
      }
      memset(&data[y * swidth + gsize], d, x_diff);
      memset(&data[(sheight - y - 1) * swidth + gsize], d, x_diff);
    }
  }

  /*
   * sides
   */

  for (x = 0; x < xlimit; x++) {
    if (xlimit == Gsize) {
      d = shadow_top[opacity_int * (Gsize + 1) + x];
    } else {
      d = sum_gaussian(gaussian_map,
        opacity, x - center, center, width, height);
    }
    for (y = gsize; y < sheight - gsize; y++) {
      data[y * swidth + x] = d;
      data[y * swidth + (swidth - x - 1)] = d;
    }
  }

  return ximage;
}

static Picture
shadow_picture(Display *dpy, double opacity, Picture alpha_pict,
               int width, int height, int *wp, int *hp) {
  XImage *shadowImage;
  Pixmap shadowPixmap;
  Picture shadow_picture;
  GC gc;

  shadowImage = make_shadow(dpy, opacity, width, height);
  if (!shadowImage) return None;

  shadowPixmap = XCreatePixmap(dpy, root,
    shadowImage->width, shadowImage->height, 8);

  if (!shadowPixmap) {
    XDestroyImage(shadowImage);
    return None;
  }

  shadow_picture = XRenderCreatePicture(dpy, shadowPixmap,
    XRenderFindStandardFormat(dpy, PictStandardA8), 0, 0);
  if (!shadow_picture) {
    XDestroyImage(shadowImage);
    XFreePixmap(dpy, shadowPixmap);
    return None;
  }

  gc = XCreateGC(dpy, shadowPixmap, 0, 0);
  if (!gc) {
    XDestroyImage(shadowImage);
    XFreePixmap(dpy, shadowPixmap);
    XRenderFreePicture(dpy, shadow_picture);
    return None;
  }

  XPutImage(
    dpy, shadowPixmap, gc, shadowImage, 0, 0, 0, 0,
    shadowImage->width, shadowImage->height);

  *wp = shadowImage->width;
  *hp = shadowImage->height;
  XFreeGC(dpy, gc);
  XDestroyImage(shadowImage);
  XFreePixmap(dpy, shadowPixmap);

  return shadow_picture;
}

Picture
solid_picture(Display *dpy, Bool argb, double a,
              double r, double g, double b) {
  Pixmap pixmap;
  Picture picture;
  XRenderPictureAttributes pa;
  XRenderColor c;

  pixmap = XCreatePixmap(dpy, root, 1, 1, argb ? 32 : 8);

  if (!pixmap) return None;

  pa.repeat = True;
  picture = XRenderCreatePicture(dpy, pixmap,
    XRenderFindStandardFormat(dpy, argb
      ? PictStandardARGB32 : PictStandardA8),
    CPRepeat,
    &pa);

  if (!picture) {
    XFreePixmap(dpy, pixmap);
    return None;
  }

  c.alpha = a * 0xffff;
  c.red = r * 0xffff;
  c.green = g * 0xffff;
  c.blue = b * 0xffff;

  XRenderFillRectangle(dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
  XFreePixmap(dpy, pixmap);

  return picture;
}

void
discard_ignore(Display *dpy, unsigned long sequence) {
  while (ignore_head) {
    if ((long) (sequence - ignore_head->sequence) > 0) {
      ignore  *next = ignore_head->next;
      free(ignore_head);
      ignore_head = next;
      if (!ignore_head) {
        ignore_tail = &ignore_head;
      }
    } else {
      break;
    }
  }
}

void
set_ignore(Display *dpy, unsigned long sequence) {
  ignore *i = malloc(sizeof(ignore));
  if (!i) return;

  i->sequence = sequence;
  i->next = 0;
  *ignore_tail = i;
  ignore_tail = &i->next;
}

int
should_ignore(Display *dpy, unsigned long sequence) {
  discard_ignore(dpy, sequence);
  return ignore_head && ignore_head->sequence == sequence;
}

static win *
find_win(Display *dpy, Window id) {
  win *w;

  for (w = list; w; w = w->next) {
    if (w->id == id && !w->destroyed)
      return w;
  }

  return 0;
}

static const char *background_props[] = {
  "_XROOTPMAP_ID",
  "_XSETROOT_ID",
  0,
};

static Picture
root_tile_f(Display *dpy) {
  Picture picture;
  Atom actual_type;
  Pixmap pixmap;
  int actual_format;
  unsigned long nitems;
  unsigned long bytes_after;
  unsigned char *prop;
  Bool fill;
  XRenderPictureAttributes pa;
  int p;
  int res;

  pixmap = None;

  for (p = 0; background_props[p]; p++) {
    prop = NULL;
    res = XGetWindowProperty(dpy, root,
          XInternAtom(dpy, background_props[p], False),
          0, 4, False, AnyPropertyType, &actual_type,
          &actual_format, &nitems, &bytes_after, &prop);
    if (likely(res == Success && prop != NULL )) {
      if(actual_type == atom_pixmap
            && actual_format == 32 && nitems == 1) {
        memcpy(&pixmap, prop, 4);
      }
      XFree(prop);
      fill = False;
      break;
    }
  }

  if (!pixmap) {
    pixmap = XCreatePixmap(dpy, root, 1, 1, DefaultDepth(dpy, scr));
    fill = True;
  }

  pa.repeat = True;
  picture = XRenderCreatePicture(
    dpy, pixmap, XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
    CPRepeat, &pa);

  if (fill) {
    XRenderColor  c;

    c.red = c.green = c.blue = 0x8080;
    c.alpha = 0xffff;
    XRenderFillRectangle(
      dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
  }

  return picture;
}

static void
paint_root(Display *dpy) {
  if (!root_tile) {
    root_tile = root_tile_f(dpy);
  }

  XRenderComposite(
    dpy, PictOpSrc, root_tile, None,
    root_buffer, 0, 0, 0, 0, 0, 0,
    root_width, root_height);
}

static XserverRegion
win_extents(Display *dpy, win *w) {
  XRectangle r;

  r.x = w->a.x;
  r.y = w->a.y;
  r.width = w->a.width + w->a.border_width * 2;
  r.height = w->a.height + w->a.border_width * 2;

  if (likely(w->window_type)
      && win_type_shadow[w->window_type]) {
    XRectangle sr;

    w->shadow_dx = shadow_offset_x;
    w->shadow_dy = shadow_offset_y;

    if (!w->shadow) {
      double opacity = shadow_opacity;

      if (w->mode != WINDOW_SOLID) {
        opacity = opacity * ((double)w->opacity) / ((double)OPAQUE);
      }

      if (HAS_FRAME_OPACITY(w)) {
        opacity = opacity * frame_opacity;
      }

      w->shadow = shadow_picture(
        dpy, opacity, w->alpha_pict,
        w->a.width + w->a.border_width * 2,
        w->a.height + w->a.border_width * 2,
        &w->shadow_width, &w->shadow_height);
    }

    sr.x = w->a.x + w->shadow_dx;
    sr.y = w->a.y + w->shadow_dy;
    sr.width = w->shadow_width;
    sr.height = w->shadow_height;

    if (sr.x < r.x) {
      r.width = (r.x + r.width) - sr.x;
      r.x = sr.x;
    }

    if (sr.y < r.y) {
      r.height = (r.y + r.height) - sr.y;
      r.y = sr.y;
    }

    if (sr.x + sr.width > r.x + r.width) {
      r.width = sr.x + sr.width - r.x;
    }

    if (sr.y + sr.height > r.y + r.height) {
      r.height = sr.y + sr.height - r.y;
    }
  }

  return XFixesCreateRegion(dpy, &r, 1);
}

static XserverRegion
border_size(Display *dpy, win *w) {
  XserverRegion border;

  /*
   * if window doesn't exist anymore,  this will generate an error
   * as well as not generate a region.  Perhaps a better XFixes
   * architecture would be to have a request that copies instead
   * of creates, that way you'd just end up with an empty region
   * instead of an invalid XID.
   */

  set_ignore(dpy, NextRequest(dpy));
  border = XFixesCreateRegionFromWindow(
    dpy, w->id, WindowRegionBounding);

  /* translate this */
  set_ignore(dpy, NextRequest(dpy));
  XFixesTranslateRegion(dpy, border,
    w->a.x + w->a.border_width,
    w->a.y + w->a.border_width);

  return border;
}

static Window
find_client_win(Display *dpy, Window win) {
  Window root, parent;
  Window *children;
  unsigned int nchildren;
  unsigned int i;
  Atom type = None;
  int format;
  unsigned long nitems, after;
  unsigned char *data = NULL;
  Window client = 0;
  int res;

  res = XGetWindowProperty(
    dpy, win, atom_wm_state, 0, 0, False,
    AnyPropertyType, &type, &format, &nitems,
    &after, &data);
  if (likely(res == Success && data != NULL )) {
      XFree(data);
      if (likely(type)) return win;
  }

  if (!XQueryTree(dpy, win, &root,
      &parent, &children, &nchildren)) {
    return 0;
  }

  for (i = 0; i < nchildren; i++) {
    client = find_client_win(dpy, children[i]);
    if (client) break;
  }

  if (children) XFree((char *)children);

  return client;
}

static void
get_frame_extents(Display *dpy, Window w,
                  unsigned int *left,
                  unsigned int *right,
                  unsigned int *top,
                  unsigned int *bottom) {
  long *extents;
  Atom type;
  int format;
  unsigned long nitems, after;
  unsigned char *data = NULL;
  int result;

  *left = 0;
  *right = 0;
  *top = 0;
  *bottom = 0;

  w = find_client_win(dpy, w);
  if (!w) return;

  result = XGetWindowProperty(
    dpy, w, atom_net_frame_extents,
    0L, 4L, False, AnyPropertyType,
    &type, &format, &nitems, &after,
    (unsigned char **)&data);

  if (result == Success) {
    if (nitems == 4 && after == 0) {
      extents = (long *)data;
      *left =
        (unsigned int)extents[0];
      *right =
        (unsigned int)extents[1];
      *top =
        (unsigned int)extents[2];
      *bottom =
        (unsigned int)extents[3];
    }
    XFree(data);
  }
}

static void
paint_all(Display *dpy, XserverRegion region) {
  win *w;
  win *t = 0;

  if (!region) {
    XRectangle r;
    r.x = 0;
    r.y = 0;
    r.width = root_width;
    r.height = root_height;
    region = XFixesCreateRegion(dpy, &r, 1);
  }

#if MONITOR_REPAINT
  root_buffer = root_picture;
#else
  if (!root_buffer) {
    Pixmap rootPixmap = XCreatePixmap(
      dpy, root, root_width, root_height,
      DefaultDepth(dpy, scr));

    root_buffer = XRenderCreatePicture(dpy, rootPixmap,
      XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
      0, 0);

    XFreePixmap(dpy, rootPixmap);
  }
#endif

  XFixesSetPictureClipRegion(dpy, root_picture, 0, 0, region);

#if MONITOR_REPAINT
  XRenderComposite(
    dpy, PictOpSrc, black_picture, None,
    root_picture, 0, 0, 0, 0, 0, 0,
    root_width, root_height);
#endif

#if DEBUG_REPAINT
  printf("paint:");
#endif

  for (w = list; w; w = w->next) {
    // Don't do this here, otherwise we get artifacts after move.
    // if (w->need_configure){
    //   do_configure_win(dpy, w);
    // }


#if CAN_DO_USABLE
    if (!w->usable) continue;
#endif

    /* never painted, ignore it */
    if (!w->damaged) continue;

    /* if invisible, ignore it */
    if (w->a.x + w->a.width < 1 || w->a.y + w->a.height < 1
        || w->a.x >= root_width || w->a.y >= root_height) {
      continue;
    }

    if (!w->picture) {
      XRenderPictureAttributes pa;
      XRenderPictFormat *format;
      Drawable draw = w->id;

#if HAS_NAME_WINDOW_PIXMAP
      if (has_name_pixmap && !w->pixmap) {
        set_ignore(dpy, NextRequest(dpy));
        w->pixmap = XCompositeNameWindowPixmap(dpy, w->id);
      }
      if (w->pixmap) draw = w->pixmap;
#endif

      format = XRenderFindVisualFormat(dpy, w->a.visual);
      pa.subwindow_mode = IncludeInferiors;
      w->picture = XRenderCreatePicture(
        dpy, draw, format, CPSubwindowMode, &pa);
    }

#if DEBUG_REPAINT
    printf(" 0x%x", w->id);
#endif

    if (clip_changed) {
      if (w->border_size) {
        set_ignore(dpy, NextRequest(dpy));
        XFixesDestroyRegion(dpy, w->border_size);
        w->border_size = None;
      }
      if (w->extents) {
        XFixesDestroyRegion(dpy, w->extents);
        w->extents = None;
      }
      if (w->border_clip) {
        XFixesDestroyRegion(dpy, w->border_clip);
        w->border_clip = None;
      }
    }

    if (!w->border_size) {
      w->border_size = border_size (dpy, w);
    }

    if (!w->extents) {
      w->extents = win_extents(dpy, w);
    }

    if (w->mode == WINDOW_SOLID && !HAS_FRAME_OPACITY(w)) {
      int x, y, wid, hei;

#if HAS_NAME_WINDOW_PIXMAP
      x = w->a.x;
      y = w->a.y;
      wid = w->a.width + w->a.border_width * 2;
      hei = w->a.height + w->a.border_width * 2;
#else
      x = w->a.x + w->a.border_width;
      y = w->a.y + w->a.border_width;
      wid = w->a.width;
      hei = w->a.height;
#endif

      XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, region);
      set_ignore(dpy, NextRequest(dpy));

      XFixesSubtractRegion(dpy, region, region, w->border_size);
      set_ignore(dpy, NextRequest(dpy));

      XRenderComposite(
        dpy, PictOpSrc, w->picture,
        None, root_buffer, 0, 0, 0, 0,
        x, y, wid, hei);
    }

    if (!w->border_clip) {
      w->border_clip = XFixesCreateRegion(dpy, 0, 0);
      XFixesCopyRegion(dpy, w->border_clip, region);
    }

    w->prev_trans = t;
    t = w;
  }

#if DEBUG_REPAINT
  printf("\n");
  fflush(stdout);
#endif

  XFixesSetPictureClipRegion(dpy,
    root_buffer, 0, 0, region);
  paint_root(dpy);

  for (w = t; w; w = w->prev_trans) {
    XFixesSetPictureClipRegion(dpy,
      root_buffer, 0, 0, w->border_clip);

    if (win_type_shadow[w->window_type]) {
      XRenderComposite(
        dpy, PictOpOver, black_picture, w->shadow,
        root_buffer, 0, 0, 0, 0,
        w->a.x + w->shadow_dx, w->a.y + w->shadow_dy,
        w->shadow_width, w->shadow_height);
    }

    if (w->opacity != OPAQUE && !w->alpha_pict) {
      w->alpha_pict = solid_picture(
        dpy, False, (double)w->opacity / OPAQUE, 0, 0, 0);
    }
    if (HAS_FRAME_OPACITY(w) && !w->alpha_border_pict) {
      w->alpha_border_pict = solid_picture(
        dpy, False, frame_opacity, 0, 0, 0);
    }

    if (w->mode != WINDOW_SOLID || HAS_FRAME_OPACITY(w)) {
      int x, y, wid, hei;

#if HAS_NAME_WINDOW_PIXMAP
      x = w->a.x;
      y = w->a.y;
      wid = w->a.width + w->a.border_width * 2;
      hei = w->a.height + w->a.border_width * 2;
#else
      x = w->a.x + w->a.border_width;
      y = w->a.y + w->a.border_width;
      wid = w->a.width;
      hei = w->a.height;
#endif

      set_ignore(dpy, NextRequest(dpy));

      if (!HAS_FRAME_OPACITY(w)) {
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_pict,
          root_buffer, 0, 0, 0, 0, x, y, wid, hei);
      } else {
        /* TODO - clean me */
        unsigned int t = w->top_width;
        unsigned int l = w->left_width;
        unsigned int b = w->bottom_width;
        unsigned int r = w->right_width;

        /* top */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_border_pict, root_buffer,
          0, 0, 0, 0, x, y, wid, t);

        /* left */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_border_pict, root_buffer,
          0, t, 0, t, x, y + t, l, hei - t);

        /* bottom */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_border_pict, root_buffer,
          l, hei - b, l, hei - b, x + l, y + hei - b, wid - l - r, b);

        /* right */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_border_pict, root_buffer,
          wid - r, t, wid - r, t, x + wid - r, y + t, r, hei - t);

        /* body */
        XRenderComposite(
          dpy, PictOpOver, w->picture, w->alpha_pict, root_buffer,
          l, t, l, t, x + l, y + t, wid - l - r, hei - t - b);
      }
    }

    XFixesDestroyRegion(dpy, w->border_clip);
    w->border_clip = None;
  }

  XFixesDestroyRegion(dpy, region);

  if (root_buffer != root_picture) {
    XFixesSetPictureClipRegion(dpy, root_buffer, 0, 0, None);
    XRenderComposite(
      dpy, PictOpSrc, root_buffer, None,
      root_picture, 0, 0, 0, 0,
      0, 0, root_width, root_height);
  }
}

static void
add_damage(Display *dpy, XserverRegion damage) {
  if (all_damage) {
    XFixesUnionRegion(dpy, all_damage, all_damage, damage);
    XFixesDestroyRegion(dpy, damage);
  } else {
    all_damage = damage;
  }
}

static void
repair_win(Display *dpy, win *w) {
  XserverRegion parts;

  if (!w->damaged) {
    parts = win_extents(dpy, w);
    set_ignore(dpy, NextRequest(dpy));
    XDamageSubtract(dpy, w->damage, None, None);
  } else {
    parts = XFixesCreateRegion(dpy, 0, 0);
    set_ignore(dpy, NextRequest(dpy));
    XDamageSubtract(dpy, w->damage, None, parts);
    XFixesTranslateRegion(dpy, parts,
      w->a.x + w->a.border_width,
      w->a.y + w->a.border_width);
  }

  add_damage(dpy, parts);
  w->damaged = 1;
}

#if 0
static const char*
wintype_name(wintype type) {
  const char *t;

  switch (type) {
    case WINTYPE_DESKTOP:
      t = "desktop";
      break;
    case WINTYPE_DOCK:
      t = "dock";
      break;
    case WINTYPE_TOOLBAR:
      t = "toolbar";
      break;
    case WINTYPE_MENU:
      t = "menu";
      break;
    case WINTYPE_UTILITY:
      t = "utility";
      break;
    case WINTYPE_SPLASH:
      t = "slash";
      break;
    case WINTYPE_DIALOG:
      t = "dialog";
      break;
    case WINTYPE_NORMAL:
      t = "normal";
      break;
    case WINTYPE_DROPDOWN_MENU:
      t = "dropdown";
      break;
    case WINTYPE_POPUP_MENU:
      t = "popup";
      break;
    case WINTYPE_TOOLTIP:
      t = "tooltip";
      break;
    case WINTYPE_NOTIFY:
      t = "notification";
      break;
    case WINTYPE_COMBO:
      t = "combo";
      break;
    case WINTYPE_DND:
      t = "dnd";
      break;
    default:
      t = "unknown";
      break;
  }

  return t;
}
#endif

static wintype
get_wintype_prop(Display * dpy, Window w) {
  Atom actual;
  int format;
  unsigned long n, left, off;
  unsigned char *data;

  off = 0;

  do {
    set_ignore(dpy, NextRequest(dpy));

    int result = XGetWindowProperty(
      dpy, w, atom_win_type, off, 1L, False, XA_ATOM,
      &actual, &format, &n, &left, &data);

    if (unlikely(result != Success)) break;

    if (likely(data != None)) {
      int i;
      for (i = 1; i < NUM_WINTYPES; ++i) {
        Atom a;
        memcpy(&a, data, sizeof(Atom));
        if (a == win_type[i]) {
          /* known type */
          XFree((void *) data);
          return i;
        }
      }
      XFree((void *) data);
    }
    ++off;
  } while (left >= 4);

  return WINTYPE_UNKNOWN;
}

static wintype
determine_wintype(Display *dpy, Window w, Window top) {
  Window root_return, parent_return;
  Window *children = NULL;
  unsigned int nchildren, i;
  wintype type;

  type = get_wintype_prop(dpy, w);
  if (type != WINTYPE_UNKNOWN) return type;

  set_ignore(dpy, NextRequest(dpy));
  if (unlikely(!XQueryTree(dpy, w, &root_return, &parent_return,
                  &children, &nchildren))) {
    goto free_out;
  }

  for (i = 0; i < nchildren; i++) {
    type = determine_wintype(dpy, children[i], top);
    if (type != WINTYPE_UNKNOWN) goto free_out;
  }

  if (w != top) {
    type = WINTYPE_UNKNOWN;
  } else {
    type = WINTYPE_NORMAL;
  }

free_out:
  if (children) XFree((void *)children);
  return type;
}

static unsigned int
get_opacity_prop(Display *dpy, win *w, unsigned int def);

static void
handle_ConfigureNotify(Display *dpy, XConfigureEvent *ce);

static void
map_win(Display *dpy, Window id,
        unsigned long sequence, Bool fade) {
  win *w = find_win(dpy, id);

  if (unlikely(!w)) return;

  w->a.map_state = IsViewable;
  w->window_type = determine_wintype(dpy, w->id, w->id);

#if 0
  printf("window 0x%x type %s\n",
    w->id, wintype_name(w->window_type));
#endif

  /* select before reading the property
     so that no property changes are lost */
  XSelectInput(dpy, id, PropertyChangeMask | FocusChangeMask);

  // this causes problems for inactive transparency
  //w->opacity = get_opacity_prop(dpy, w, OPAQUE);

  determine_mode(dpy, w);

#if CAN_DO_USABLE
  w->damage_bounds.x = w->damage_bounds.y = 0;
  w->damage_bounds.width = w->damage_bounds.height = 0;
#endif
  w->damaged = 0;

  if (fade && win_type_fade[w->window_type]) {
    set_fade(
      dpy, w, 0, get_opacity_percent(dpy, w),
      fade_in_step, 0, True, True);
  }

  /* if any configure events happened while
     the window was unmapped, then configure
     the window to its correct place */
  // if (w->need_configure) {
  //   handle_ConfigureNotify(dpy, &w->queue_configure);
  // }
}

static void
finish_unmap_win(Display *dpy, win *w) {
  w->damaged = 0;
#if CAN_DO_USABLE
  w->usable = False;
#endif

  if (w->extents != None) {
    add_damage(dpy, w->extents);  /* destroys region */
    w->extents = None;
  }

#if HAS_NAME_WINDOW_PIXMAP
  if (w->pixmap) {
    XFreePixmap(dpy, w->pixmap);
    w->pixmap = None;
  }
#endif

  if (w->picture) {
    set_ignore(dpy, NextRequest(dpy));
    XRenderFreePicture(dpy, w->picture);
    w->picture = None;
  }

  /* don't care about properties anymore */
  set_ignore(dpy, NextRequest(dpy));
  XSelectInput(dpy, w->id, 0);

  if (w->border_size) {
    set_ignore(dpy, NextRequest(dpy));
      XFixesDestroyRegion(dpy, w->border_size);
      w->border_size = None;
  }

  if (w->shadow) {
    XRenderFreePicture(dpy, w->shadow);
    w->shadow = None;
  }

  if (w->border_clip) {
    XFixesDestroyRegion(dpy, w->border_clip);
    w->border_clip = None;
  }

  clip_changed = True;
}

#if HAS_NAME_WINDOW_PIXMAP
static void
unmap_callback(Display *dpy, win *w) {
  finish_unmap_win(dpy, w);
}
#endif

static void
unmap_win(Display *dpy, Window id, Bool fade) {
  win *w = find_win(dpy, id);

  if (!w) return;

  w->a.map_state = IsUnmapped;
#if HAS_NAME_WINDOW_PIXMAP
  if (w->pixmap && fade && win_type_fade[w->window_type]) {
    set_fade(dpy, w, w->opacity * 1.0 / OPAQUE, 0.0,
             fade_out_step, unmap_callback, False, True);
  } else
#endif
    finish_unmap_win(dpy, w);
}

/* Get the opacity prop from window
   not found: default
   otherwise the value
 */
static unsigned int
get_opacity_prop(Display *dpy, win *w, unsigned int def) {
  Atom actual;
  int format;
  unsigned long n, left;

  unsigned char *data;
  int result = XGetWindowProperty(
    dpy, w->id, atom_opacity, 0L, 1L, False,
    XA_CARDINAL, &actual, &format, &n, &left, &data);

  if (result == Success && data != NULL) {
    unsigned int i;
    memcpy(&i, data, sizeof(unsigned int));
    XFree((void *)data);
    return i;
  }

  return def;
}

/*
 * Get the opacity property from the window in a percent format
 * not found: default
 * otherwise: the value
*/

static double
get_opacity_percent(Display *dpy, win *w) {
  double def = win_type_opacity[w->window_type];
  unsigned int opacity =
    get_opacity_prop(dpy, w, (unsigned int)(OPAQUE * def));

  return opacity * 1.0 / OPAQUE;
}

static void
determine_mode(Display *dpy, win *w) {
  int mode;
  XRenderPictFormat *format;

  /* if trans prop == -1 fall back on  previous tests*/

  if (w->alpha_pict) {
    XRenderFreePicture(dpy, w->alpha_pict);
    w->alpha_pict = None;
  }

  if (w->alpha_border_pict) {
    XRenderFreePicture(dpy, w->alpha_border_pict);
    w->alpha_border_pict = None;
  }

  if (w->shadow_pict) {
    XRenderFreePicture(dpy, w->shadow_pict);
    w->shadow_pict = None;
  }

  if (w->a.class == InputOnly) {
    format = 0;
  } else {
    format = XRenderFindVisualFormat(dpy, w->a.visual);
  }

  if (format && format->type == PictTypeDirect
      && format->direct.alphaMask) {
    mode = WINDOW_ARGB;
  } else if (w->opacity != OPAQUE) {
    mode = WINDOW_TRANS;
  } else {
    mode = WINDOW_SOLID;
  }

  w->mode = mode;

  if (w->extents) {
    XserverRegion damage;
    damage = XFixesCreateRegion(dpy, 0, 0);
    XFixesCopyRegion(dpy, damage, w->extents);
    add_damage(dpy, damage);
  }
}

static void
set_opacity(Display *dpy, win *w, unsigned long opacity) {
  if (fade_trans) {
    double old_opacity = (double)w->opacity / OPAQUE;
    w->opacity = opacity;
    set_fade(dpy, w, old_opacity,
      (double)w->opacity / OPAQUE,
      fade_out_step, 0, True, False);
  } else {
    w->opacity = opacity;
    determine_mode(dpy, w);
    if (w->shadow) {
      XRenderFreePicture(dpy, w->shadow);
      w->shadow = None;

      if (w->extents != None) {
        XFixesDestroyRegion(dpy, w->extents);
      }

      /* rebuild the shadow */
      w->extents = win_extents(dpy, w);
    }
  }
}

static void
add_win(Display *dpy, Window id, Window prev) {
  win *new = calloc(1, sizeof(win));
  win **p;

  if (unlikely(!new)) return;

  if (prev) {
    for (p = &list; *p; p = &(*p)->next) {
      if ((*p)->id == prev && !(*p)->destroyed)
        break;
    }
  } else {
    p = &list;
  }

  new->id = id;
  set_ignore(dpy, NextRequest(dpy));

  if (unlikely(!XGetWindowAttributes(dpy, id, &new->a))) {
    free(new);
    return;
  }

#if HAS_NAME_WINDOW_PIXMAP
  new->pixmap = None;
#endif
  new->picture = None;

  if (new->a.class == InputOnly) {
    // we used calloc, so no need to set zeroes
    // new->damage_sequence = 0;
    new->damage = None;
  } else {
    new->damage_sequence = NextRequest(dpy);
    set_ignore(dpy, NextRequest(dpy));
    new->damage = XDamageCreate(dpy, id, XDamageReportNonEmpty);
  }

  new->alpha_pict = None;
  new->alpha_border_pict = None;
  new->shadow_pict = None;
  new->border_size = None;
  new->extents = None;
  new->shadow = None;

  // we used calloc, so no need to set zeroes
  // new->shadow_dx = 0;
  // new->shadow_dy = 0;
  // new->shadow_width = 0;
  // new->shadow_height = 0;
  // new->window_type = WINTYPE_UNKNOWN;
  // new->prev_trans = 0;
  // new->left_width = 0;
  // new->right_width = 0;
  // new->top_width = 0;
  // new->bottom_width = 0;
  // new->need_configure = False;
  // new->destroyed = False;
  // new->damaged = 0;
// #if CAN_DO_USABLE
  // new->usable = False;
// #endif

  new->opacity = OPAQUE;

  new->border_clip = None;
  get_frame_extents(dpy, id,
    &new->left_width, &new->right_width,
    &new->top_width, &new->bottom_width);

  new->next = *p;
  *p = new;

  if (new->a.map_state == IsViewable) {
    new->window_type = determine_wintype(dpy, id, id);
    if (inactive_opacity && IS_NORMAL_WIN(new)) {
      new->opacity = INACTIVE_OPACITY;
    }
    map_win(dpy, id, new->damage_sequence - 1, True);
  }
}

void
restack_win(Display *dpy, win *w, Window new_above) {
  Window old_above;

  if (w->next) {
    old_above = w->next->id;
  } else {
    old_above = None;
  }

  if (old_above != new_above) {
    win **prev;

    /* unhook */
    for (prev = &list; *prev; prev = &(*prev)->next) {
      if ((*prev) == w) break;
    }

    *prev = w->next;

    /* rehook */
    for (prev = &list; *prev; prev = &(*prev)->next) {
      if ((*prev)->id == new_above && !(*prev)->destroyed)
        break;
    }

    w->next = *prev;
    *prev = w;
  }
}

static void
do_configure_win(Display *dpy, win* w){
  XserverRegion damage = None;
  XConfigureEvent* ce = &w->queue_configure;

  w->need_configure = False;

#if CAN_DO_USABLE
  if (w->usable)
#endif
  {
    damage = XFixesCreateRegion(dpy, 0, 0);
    if (w->extents != None)
      XFixesCopyRegion(dpy, damage, w->extents);
  }


  w->a.x = ce->x;
  w->a.y = ce->y;
  if (w->a.width != ce->width || w->a.height != ce->height) {

#if HAS_NAME_WINDOW_PIXMAP
    if (w->pixmap) {
      XFreePixmap(dpy, w->pixmap);
      w->pixmap = None;
      if (w->picture) {
        XRenderFreePicture(dpy, w->picture);
        w->picture = None;
      }
    }
#endif

    if (w->shadow) {
      XRenderFreePicture(dpy, w->shadow);
      w->shadow = None;
    }
  }

  w->a.width = ce->width;
  w->a.height = ce->height;
  w->a.border_width = ce->border_width;

  if (w->a.map_state != IsUnmapped && damage) {
    XserverRegion extents = win_extents(dpy, w);
    XFixesUnionRegion(dpy, damage, damage, extents);
    XFixesDestroyRegion(dpy, extents);
    add_damage(dpy, damage);
  }

  clip_changed = True;
  w->a.override_redirect = ce->override_redirect;
}

Bool g_configure_needed = False;

static void
handle_ConfigureNotify(Display *dpy, XConfigureEvent *ce) {
  win *w = find_win(dpy, ce->window);

  if (unlikely(!w)) {
    if (ce->window == root) {
      if (root_buffer) {
        XRenderFreePicture(dpy, root_buffer);
        root_buffer = None;
      }
      root_width = ce->width;
      root_height = ce->height;
    }
    return;
  }
  // save the configure event for later
  g_configure_needed = True;
  w->need_configure = True;
  w->queue_configure = *ce;

  // w->a.override_redirect = ce->override_redirect;
  restack_win(dpy, w, ce->above);
}

static void
circulate_win(Display *dpy, XCirculateEvent *ce) {
  win *w = find_win(dpy, ce->window);
  Window new_above;

  if (!w) return;

  if (ce->place == PlaceOnTop) {
    new_above = list->id;
  } else {
    new_above = None;
  }

  restack_win(dpy, w, new_above);
  clip_changed = True;
}

static void
finish_destroy_win(Display *dpy, Window id) {
  win **prev, *w;

  for (prev = &list; (w = *prev); prev = &w->next) {
    if (w->id == id && w->destroyed) {
      finish_unmap_win(dpy, w);
      *prev = w->next;

      if (w->alpha_pict) {
        XRenderFreePicture(dpy, w->alpha_pict);
        w->alpha_pict = None;
      }

      if (w->alpha_border_pict) {
        XRenderFreePicture(dpy, w->alpha_border_pict);
        w->alpha_border_pict = None;
      }

      if (w->shadow_pict) {
        XRenderFreePicture(dpy, w->shadow_pict);
        w->shadow_pict = None;
      }

      /* fix leak, from freedesktop repo */
      if (w->shadow) {
        XRenderFreePicture (dpy, w->shadow);
        w->shadow = None;
      }

      if (w->damage != None) {
        set_ignore(dpy, NextRequest(dpy));
        XDamageDestroy(dpy, w->damage);
        w->damage = None;
      }

      cleanup_fade(dpy, w);
      free(w);
      break;
    }
  }
}

#if HAS_NAME_WINDOW_PIXMAP
static void
destroy_callback(Display *dpy, win *w) {
  finish_destroy_win(dpy, w->id);
}
#endif

static void
destroy_win(Display *dpy, Window id, Bool fade) {
  win *w = find_win(dpy, id);

  if (w) w->destroyed = True;

#if HAS_NAME_WINDOW_PIXMAP
  if (w && w->pixmap && fade && win_type_fade[w->window_type]) {
    set_fade(dpy, w, w->opacity * 1.0 / OPAQUE,
      0.0, fade_out_step, destroy_callback,
      False, True);
  } else
#endif
  {
    finish_destroy_win(dpy, id);
  }
}

#if 0
static void
dump_win(win *w) {
  printf("\t%08lx: %d x %d + %d + %d(%d)\n", w->id,
    w->a.width, w->a.height,
    w->a.x, w->a.y, w->a.border_width);
}


static void
dump_wins(void) {
  win *w;

  printf("windows:\n");
  for (w = list; w; w = w->next) {
    dump_win(w);
  }
}
#endif

static void
damage_win(Display *dpy, XDamageNotifyEvent *de) {
  win *w = find_win(dpy, de->drawable);

  if (unlikely(!w)) return;

#if CAN_DO_USABLE
  if (!w->usable) {
    if (w->damage_bounds.width == 0 || w->damage_bounds.height == 0) {
      w->damage_bounds = de->area;
    } else {
      if (de->area.x < w->damage_bounds.x) {
        w->damage_bounds.width += (w->damage_bounds.x - de->area.x);
        w->damage_bounds.x = de->area.x;
      }
      if (de->area.y < w->damage_bounds.y) {
        w->damage_bounds.height += (w->damage_bounds.y - de->area.y);
        w->damage_bounds.y = de->area.y;
      }
      if (de->area.x + de->area.width
          > w->damage_bounds.x + w->damage_bounds.width) {
        w->damage_bounds.width =
          de->area.x + de->area.width - w->damage_bounds.x;
      }
      if (de->area.y + de->area.height
          > w->damage_bounds.y + w->damage_bounds.height) {
        w->damage_bounds.height =
          de->area.y + de->area.height - w->damage_bounds.y;
      }
    }

#if 0
    printf("unusable damage %d, %d: %d x %d bounds %d, %d: %d x %d\n",
      de->area.x,
      de->area.y,
      de->area.width,
      de->area.height,
      w->damage_bounds.x,
      w->damage_bounds.y,
      w->damage_bounds.width,
      w->damage_bounds.height);
#endif

    if (w->damage_bounds.x <= 0
        && w->damage_bounds.y <= 0
        && w->a.width <= w->damage_bounds.x + w->damage_bounds.width
        && w->a.height <= w->damage_bounds.y + w->damage_bounds.height) {
      clip_changed = True;
      if (win_type_fade[w->window_type]) {
        set_fade(dpy, w, 0, get_opacity_percent(dpy, w),
                 fade_in_step, 0, True, True);
      }
      w->usable = True;
    }
  }

  if (w->usable)
#endif
    repair_win(dpy, w);
}

static int
error(Display *dpy, XErrorEvent *ev) {
  int o;
  const char *name = "Unknown";

  if (should_ignore(dpy, ev->serial)) {
    return 0;
  }

  if (ev->request_code == composite_opcode
      && ev->minor_code == X_CompositeRedirectSubwindows) {
    fprintf(stderr, "Another composite manager is already running\n");
    exit(1);
  }

  o = ev->error_code - xfixes_error;
  switch (o) {
    case BadRegion:
      name = "BadRegion";
      break;
    default:
      break;
  }

  o = ev->error_code - damage_error;
  switch (o) {
    case BadDamage:
      name = "BadDamage";
      break;
    default:
      break;
  }

  o = ev->error_code - render_error;
  switch (o) {
    case BadPictFormat:
      name ="BadPictFormat";
      break;
    case BadPicture:
      name ="BadPicture";
      break;
    case BadPictOp:
      name ="BadPictOp";
      break;
    case BadGlyphSet:
      name ="BadGlyphSet";
      break;
    case BadGlyph:
      name ="BadGlyph";
      break;
    default:
      break;
  }

  fprintf(stderr, "error %d (%s) request %d minor %d serial %lu\n",
      ev->error_code, name, ev->request_code,
      ev->minor_code, ev->serial);

/*  abort();      this is just annoying to most people */
  return 0;
}

static void
expose_root(Display *dpy, Window root, XRectangle *rects, int nrects) {
  XserverRegion region = XFixesCreateRegion(dpy, rects, nrects);

  add_damage(dpy, region);
}

#if DEBUG_EVENTS
static int
ev_serial(XEvent *ev) {
  if ((ev->type & 0x7f) != KeymapNotify) {
    return ev->xany.serial;
  }
  return NextRequest(ev->xany.display);
}

static char *
ev_name(XEvent *ev) {
  static char buf[128];
  // what?? switch (ev->type & 0x7f) {
  switch (ev->type) {
    case FocusIn:
      return "FocusIn";
    case FocusOut:
      return "FocusOut";
    case CreateNotify:
      return "CreateNotify";
    case ConfigureNotify:
      return "ConfigureNotify";
    case DestroyNotify:
      return "DestroyNotify";
    case Expose:
      return "Expose";
    case MapNotify:
      return "Map";
    case UnmapNotify:
      return "Unmap";
    case ReparentNotify:
      return "Reparent";
    case CirculateNotify:
      return "Circulate";
    case PropertyNotify:
      return "PropertyNotify";
    default:
      if (ev->type == damage_event + XDamageNotify) {
        return "Damage";
      }
      sprintf(buf, "Event %d", ev->type);
      return buf;
  }
}

static Window
ev_window(XEvent *ev) {
  switch (ev->type) {
    case Expose:
      return ev->xexpose.window;
    case MapNotify:
      return ev->xmap.window;
    case UnmapNotify:
      return ev->xunmap.window;
    case ReparentNotify:
      return ev->xreparent.window;
    case CirculateNotify:
      return ev->xcirculate.window;
    default:
      if (ev->type == damage_event + XDamageNotify) {
        return ((XDamageNotifyEvent *)ev)->drawable;
      }
      return 0;
  }
}
#endif

void
usage(char *program) {
  fprintf(stderr, "%s v0.0.1\n", program);
  fprintf(stderr, "usage: %s [options]\n", program);

  fprintf(stderr, "Options\n");
  fprintf(stderr,
    "   -d display\n    "
    "Which display should be managed.\n");
  fprintf(stderr,
    "   -r radius\n    "
    "The blur radius for shadows. (default 12)\n");
  fprintf(stderr,
    "   -o opacity\n    "
    "The translucency for shadows. (default .75)\n");
  fprintf(stderr,
    "   -l left-offset\n    "
    "The left offset for shadows. (default -15)\n");
  fprintf(stderr,
   "   -t top-offset\n    "
   "The top offset for shadows. (default -15)\n");
  fprintf(stderr,
    "   -I fade-in-step\n    "
    "Opacity change between steps while fading in. (default 0.028)\n");
  fprintf(stderr,
    "   -O fade-out-step\n    "
    "Opacity change between steps while fading out. (default 0.03)\n");
  fprintf(stderr,
    "   -D fade-delta-time\n    "
    "The time between steps in a fade in milliseconds. (default 10)\n");
  fprintf(stderr,
    "   -m opacity\n    "
    "The opacity for menus. (default 1.0)\n");
  fprintf(stderr,
    "   -c\n    "
    "Enabled client-side shadows on windows.\n");
  fprintf(stderr,
    "   -C\n    "
    "Avoid drawing shadows on dock/panel windows.\n");
  fprintf(stderr,
    "   -f\n    "
    "Fade windows in/out when opening/closing.\n");
  fprintf(stderr,
    "   -F\n    "
    "Fade windows during opacity changes.\n");
  fprintf(stderr,
    "   -i opacity\n    "
    "Opacity of inactive windows. (0.1 - 1.0)\n");
  fprintf(stderr,
    "   -e opacity\n    "
    "Opacity of window titlebars and borders. (0.1 - 1.0)\n");
  fprintf(stderr,
    "   -S\n    "
    "Enable synchronous operation (for debugging).\n");

  exit(1);
}

static void
register_cm(int scr) {
  Window w;
  Atom a;
  char *buf;
  int len, s;

  if (scr < 0) return;

  w = XCreateSimpleWindow(
    dpy, RootWindow(dpy, 0),
    0, 0, 1, 1, 0, None, None);

  Xutf8SetWMProperties(
    dpy, w, "xcompmgr", "xcompmgr",
    NULL, 0, NULL, NULL, NULL);

  len = strlen(REGISTER_PROP) + 2;
  s = scr;

  while (s >= 10) {
    ++len;
    s /= 10;
  }

  buf = malloc(len);
  snprintf(buf, len, REGISTER_PROP"%d", scr);

  a = XInternAtom(dpy, buf, False);
  free(buf);

  XSetSelectionOwner(dpy, a, w, 0);
}

static void run_configures(Display *dpy){
  win *w;
  for (w = list; w; w = w->next) {
    if (w->need_configure && !w->destroyed){
      do_configure_win(dpy, w);
    }
  }
}

static void
do_paint(Display *dpy){
   paint_all(dpy, all_damage);
   XSync(dpy, False);
   all_damage = None;
   clip_changed = False;
}

static Bool configure_timer_started = False;
static int configure_time = 0;

/// When a window is moved, or resized, a lot of ConfigureNotify events
/// occur. However, painting and Xsyncing of complex windows, e.g.
/// web-browser contents, may introduce a considerable lag. Therefore, for each
/// window, we cache the "latest" configure event and paint the events after
/// some timeout. On the other hand, we want to handle other events, especially
/// damage events, as fast as possible, so we do not timeout in this case.
static void
check_paint(Display *dpy){
  if(unlikely(g_configure_needed)){
    const int EVERY_MILISEC = 2;
    if(!configure_timer_started){
      // Not strictly necessary to paint now, but until we run, the
      // configured window has already been moving/resizing for a (short)
      // while, so give early feedback to the user.
      run_configures(dpy);
      do_paint(dpy);
      configure_timer_started = True;
      configure_time = get_time_in_milliseconds() + EVERY_MILISEC;
    } else {
      int delta;
      delta = get_time_in_milliseconds() - configure_time;
      if (delta < EVERY_MILISEC){
        return;
      }
      g_configure_needed = False;
      configure_timer_started = False;
      run_configures(dpy);
      do_paint(dpy);
    }
  } else {
    if(likely(all_damage)) {
      do_paint(dpy);
    }
  }
}


int
main(int argc, char **argv) {
  XEvent ev;
  Window root_return, parent_return;
  Window *children;
  unsigned int nchildren;
  int i;
  XRenderPictureAttributes pa;
  XRectangle *expose_rects = 0;
  int size_expose = 0;
  int n_expose = 0;
  struct pollfd ufd;
  int p;
  int composite_major, composite_minor;
  char *display = 0;
  int o;
  Bool no_dock_shadow = False;

  for (i = 0; i < NUM_WINTYPES; ++i) {
    win_type_fade[i] = False;
    win_type_shadow[i] = False;
    win_type_opacity[i] = 1.0;
  }

  while ((o = getopt(argc, argv, "D:I:O:d:r:o:m:l:t:i:e:scnfFCaS")) != -1) {
    switch (o) {
      case 'd':
        display = optarg;
        break;
      case 'D':
        fade_delta = atoi(optarg);
        if (fade_delta < 1) {
          fade_delta = 10;
        }
        break;
      case 'I':
        fade_in_step = atof(optarg);
        if (fade_in_step <= 0) {
          fade_in_step = 0.01;
        }
        break;
      case 'O':
        fade_out_step = atof(optarg);
        if (fade_out_step <= 0) {
          fade_out_step = 0.01;
        }
        break;
      case 'c':
        for (i = 1; i < NUM_WINTYPES; ++i) {
          win_type_shadow[i] = True;
        }
        win_type_shadow[WINTYPE_DESKTOP] = False;
        break;
      case 'C':
        no_dock_shadow = True;
        break;
      case 'm':
        win_type_opacity[WINTYPE_DROPDOWN_MENU] = atof(optarg);
        win_type_opacity[WINTYPE_POPUP_MENU] = atof(optarg);
        break;
      case 'f':
        for (i = 1; i < NUM_WINTYPES; ++i) {
          win_type_fade[i] = True;
        }
        break;
      case 'F':
        fade_trans = True;
        break;
      case 'S':
        synchronize = True;
        break;
      case 'r':
        shadow_radius = atoi(optarg);
        break;
      case 'o':
        shadow_opacity = atof(optarg);
        break;
      case 'l':
        shadow_offset_x = atoi(optarg);
        break;
      case 't':
        shadow_offset_y = atoi(optarg);
        break;
      case 'i':
        inactive_opacity = (double)atof(optarg);
        break;
      case 'e':
        frame_opacity = (double)atof(optarg);
        break;
      case 'n':
      case 'a':
      case 's':
        fprintf(stderr, "Warning: "
          "-n, -a, and -s have been removed.\n");
        break;
      default:
        usage(argv[0]);
        break;
    }
  }

  if (no_dock_shadow) {
    win_type_shadow[WINTYPE_DOCK] = False;
  }

  dpy = XOpenDisplay(display);
  if (!dpy) {
    fprintf(stderr, "Can't open display\n");
    exit(1);
  }

  XSetErrorHandler(error);
  if (synchronize) {
    XSynchronize(dpy, 1);
  }

  scr = DefaultScreen(dpy);
  root = RootWindow(dpy, scr);

  if (!XRenderQueryExtension(dpy, &render_event, &render_error)) {
    fprintf(stderr, "No render extension\n");
    exit(1);
  }

  if (!XQueryExtension(dpy, COMPOSITE_NAME, &composite_opcode,
                       &composite_event, &composite_error)) {
    fprintf(stderr, "No composite extension\n");
    exit(1);
  }

  XCompositeQueryVersion(dpy, &composite_major, &composite_minor);

#if HAS_NAME_WINDOW_PIXMAP
  if (composite_major > 0 || composite_minor >= 2) {
    has_name_pixmap = True;
  }
#endif

  if (!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
    fprintf(stderr, "No damage extension\n");
    exit(1);
  }

  if (!XFixesQueryExtension(dpy, &xfixes_event, &xfixes_error)) {
    fprintf(stderr, "No XFixes extension\n");
    exit(1);
  }

  register_cm(scr);

  /* get atoms */
  atom_opacity = XInternAtom(dpy,
    "_NET_WM_WINDOW_OPACITY", False);
  atom_win_type = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE", False);
  atom_pixmap = XInternAtom(dpy,
    "PIXMAP", False);
  atom_wm_state = XInternAtom(dpy,
    "WM_STATE", False);
  atom_net_frame_extents = XInternAtom(dpy,
    "_NET_FRAME_EXTENTS", False),
  win_type[WINTYPE_DESKTOP] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DESKTOP", False);
  win_type[WINTYPE_DOCK] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DOCK", False);
  win_type[WINTYPE_TOOLBAR] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
  win_type[WINTYPE_MENU] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_MENU", False);
  win_type[WINTYPE_UTILITY] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_UTILITY", False);
  win_type[WINTYPE_SPLASH] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_SPLASH", False);
  win_type[WINTYPE_DIALOG] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DIALOG", False);
  win_type[WINTYPE_NORMAL] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_NORMAL", False);
  win_type[WINTYPE_DROPDOWN_MENU] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DROPDOWN_MENU", False);
  win_type[WINTYPE_POPUP_MENU] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_POPUP_MENU", False);
  win_type[WINTYPE_TOOLTIP] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_TOOLTIP", False);
  win_type[WINTYPE_NOTIFY] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_NOTIFICATION", False);
  win_type[WINTYPE_COMBO] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_COMBO", False);
  win_type[WINTYPE_DND] = XInternAtom(dpy,
    "_NET_WM_WINDOW_TYPE_DND", False);

  pa.subwindow_mode = IncludeInferiors;

  gaussian_map = make_gaussian_map(dpy, shadow_radius);
  presum_gaussian(gaussian_map);

  root_width = DisplayWidth(dpy, scr);
  root_height = DisplayHeight(dpy, scr);

  root_picture = XRenderCreatePicture(dpy, root,
    XRenderFindVisualFormat(dpy, DefaultVisual(dpy, scr)),
    CPSubwindowMode, &pa);
  black_picture = solid_picture(dpy, True, 1, 0, 0, 0);

  all_damage = None;
  clip_changed = True;
  XGrabServer(dpy);

  XCompositeRedirectSubwindows(
    dpy, root, CompositeRedirectManual);

  XSelectInput(dpy, root,
    SubstructureNotifyMask
    | ExposureMask
    | StructureNotifyMask
    | PropertyChangeMask);

  XQueryTree(dpy, root, &root_return,
    &parent_return, &children, &nchildren);

  for (i = 0; i < nchildren; i++) {
    add_win(dpy, children[i], i ? children[i-1] : None);
  }

  XFree(children);

  XUngrabServer(dpy);

  ufd.fd = ConnectionNumber(dpy);
  ufd.events = POLLIN;

  paint_all(dpy, None);

  for (;;) {
    /*    dump_wins(); */
    do {
      if (!QLength(dpy)) {
        // TODO: check and re-implement fade time logic.
        int timeout = (configure_timer_started) ? 2 : fade_timeout();
        if (unlikely(poll(&ufd, 1, timeout) == 0)) {
          check_paint(dpy);
           //   run_fades(dpy);
          break;
        }
      }

      XNextEvent(dpy, &ev);

      if (likely((ev.type & 0x7f) != KeymapNotify)) {
        discard_ignore(dpy, ev.xany.serial);
      }

#if DEBUG_EVENTS
      fprintf(stderr, "event %s serial 0x%08x window 0x%08lx\n",
          ev_name(&ev), ev_serial(&ev), ev_window(&ev));
#endif

      switch (ev.type) {
        case FocusIn: {
          if (!inactive_opacity) break;

          // stop focusing windows the cursor is over.
          // with this, windows dont focus right after being
          // deiconified, this needs to be fixed by blocking
          // the right kind of FocusOut event
          if (ev.xfocus.detail == NotifyPointer) break;

          win *fw = find_win(dpy, ev.xfocus.window);
          if (IS_NORMAL_WIN(fw)) {
            set_opacity(dpy, fw, OPAQUE);
          }
          break;
        }
        case FocusOut: {
          if (!inactive_opacity) break;

          // this fixes deiconify refocus
          // need != notifygrab here otherwise windows wont
          // lower opacity when grabbed for dragging
          if (ev.xfocus.mode != NotifyGrab
              && ev.xfocus.detail == NotifyVirtual) break;

          win *fw = find_win(dpy, ev.xfocus.window);
          if (IS_NORMAL_WIN(fw)) {
            set_opacity(dpy, fw, INACTIVE_OPACITY);
          }
          break;
        }
        case CreateNotify:
          //if (ev.xcreatewindow.override_redirect) break;
          add_win(dpy, ev.xcreatewindow.window, 0);
          break;
        case ConfigureNotify:
          handle_ConfigureNotify(dpy, &ev.xconfigure);
          break;
        case DestroyNotify:
          destroy_win(dpy, ev.xdestroywindow.window, True);
          break;
        case MapNotify:
          map_win(dpy, ev.xmap.window, ev.xmap.serial, True);
          break;
        case UnmapNotify:
          unmap_win(dpy, ev.xunmap.window, True);
          break;
        case ReparentNotify:
          if (ev.xreparent.parent == root) {
            add_win(dpy, ev.xreparent.window, 0);
          } else {
            destroy_win(dpy, ev.xreparent.window, True);
          }
          break;
        case CirculateNotify:
          circulate_win(dpy, &ev.xcirculate);
          break;
        case Expose:
          if (ev.xexpose.window == root) {
            int more = ev.xexpose.count + 1;
            if (n_expose == size_expose) {
              if (expose_rects) {
                expose_rects = realloc(expose_rects,
                  (size_expose + more) * sizeof(XRectangle));
                size_expose += more;
              } else {
                expose_rects = malloc(more * sizeof(XRectangle));
                size_expose = more;
              }
            }
            expose_rects[n_expose].x = ev.xexpose.x;
            expose_rects[n_expose].y = ev.xexpose.y;
            expose_rects[n_expose].width = ev.xexpose.width;
            expose_rects[n_expose].height = ev.xexpose.height;
            n_expose++;
            if (ev.xexpose.count == 0) {
              expose_root(dpy, root, expose_rects, n_expose);
              n_expose = 0;
            }
          }
          break;
        case PropertyNotify:
          for (p = 0; background_props[p]; p++) {
            if (ev.xproperty.atom ==
                XInternAtom(dpy, background_props[p], False)) {
              if (root_tile) {
                XClearArea(dpy, root, 0, 0, 0, 0, True);
                XRenderFreePicture(dpy, root_tile);
                root_tile = None;
                break;
              }
            }
          }
          /* check if Trans property was changed */
          if (ev.xproperty.atom == atom_opacity) {
            /* reset mode and redraw window */
            win *w = find_win(dpy, ev.xproperty.window);
            if (w) {
              double def = win_type_opacity[w->window_type];
              set_opacity(dpy, w,
                get_opacity_prop(dpy, w, (unsigned long)(OPAQUE * def)));
            }
          }
          break;
        default:
          if (likely(ev.type == damage_event + XDamageNotify)) {
            damage_win(dpy, (XDamageNotifyEvent *)&ev);
          }
          break;
      }
    } while (QLength(dpy));

    check_paint(dpy);
  }
}
