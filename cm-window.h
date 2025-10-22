#pragma once

#include <stdbool.h>

#include <X11/Xlib.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>

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


// Cache whether to draw a shadow or not
typedef enum {
  SHADOW_UNKNWON, // MUST ALWAYS STAY first, due to init optimization in add_win
  SHADOW_YES,
  SHADOW_NO
} shadowtype;


// _NET_WM_STATE is _NET_WM_STATE_HIDDEN (and not _FOCUSED)
typedef enum {
  HIDDEN_UNKNOWN, // MUST ALWAYS STAY first, due to init optimization in add_win
  HIDDEN_YES,
  HIDDEN_NO,
  HIDDEN_IGNORE // Don't attempt to look up client state unless there's a good reason
} hiddentype;


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
  bool userdefined_opacity; // Do not set inactive opacity, if the client requests a custom
  hiddentype hidden_type;
  wintype window_type;
  shadowtype shadow_type;
  unsigned long damage_sequence; /* sequence when damage was created */
  Bool destroyed;
  Bool paint_needed;
  unsigned int left_width;
  unsigned int right_width;
  unsigned int top_width;
  unsigned int bottom_width;

  Bool need_configure;
  bool configure_size_changed;
  XConfigureEvent queue_configure;

  /* for drawing translucent windows */
  XserverRegion border_clip;
  struct _win *prev_trans;
} win;


extern win *list;

win* find_win(Window id);
win* find_win_any_parent(Window w);

bool win_state_is_hidden(Window window);
bool win_is_client(Window window);
void win_register_client_events(Window window);
