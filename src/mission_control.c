#include "mission_control.h"
#include "border.h"
#include "events.h"
#include "windows.h"
#include "misc/extern.h"
#include <ApplicationServices/ApplicationServices.h>
#include <libproc.h>
#include <math.h>

extern struct table g_windows;
extern struct settings g_settings;

struct mc_display { CGRect bounds; bool affected; };
struct mc_owner { pid_t pid; AXObserverRef observer; };
static struct mc_owner owners[2];
static CFMutableArrayRef shields;
static struct mc_display* displays;
static uint32_t display_count;
static bool active, pending_space_update, ax_session, exit_authorized;
static float opacity = 1.f, fade_start, fade_target;
static CFAbsoluteTime fade_time;
static CFRunLoopTimerRef fade_timer, exit_timer;
static unsigned exit_ticks, exit_absent_ticks;

static void stop_timer(CFRunLoopTimerRef* timer) {
  if (!*timer) return;
  CFRunLoopTimerInvalidate(*timer);
  CFRelease(*timer);
  *timer = NULL;
}

bool mission_control_active(void) { return active; }
void mission_control_defer_space_update(void) { pending_space_update = true; }

bool mission_control_border_affected(struct border* border) {
  if (!g_settings.mission_control) return false;
  pthread_mutex_lock(&border->mutex);
  CGPoint center = CGPointMake(CGRectGetMidX(border->target_bounds),
                               CGRectGetMidY(border->target_bounds));
  bool affected = false;
  for (uint32_t i = 0; i < display_count; ++i) {
    if (displays[i].affected && CGRectContainsPoint(displays[i].bounds, center)) {
      affected = true;
      break;
    }
  }
  pthread_mutex_unlock(&border->mutex);
  return affected;
}

float mission_control_opacity_for_border(struct border* border) {
  return mission_control_border_affected(border) ? opacity : 1.f;
}

static void apply_opacity(void) {
  for (int i = 0; i < g_windows.capacity; ++i) {
    for (struct bucket* b = g_windows.buckets[i]; b; b = b->next) {
      struct border* border = b->value;
      if (border) {
        pthread_mutex_lock(&border->mutex);
        border_set_mission_control_opacity(border, mission_control_opacity_for_border(border));
        pthread_mutex_unlock(&border->mutex);
      }
    }
  }
}

static void fade_tick(CFRunLoopTimerRef timer, void* context) {
  double duration = g_settings.mission_control_fade / 1000.0;
  double t = duration > 0 ? (CFAbsoluteTimeGetCurrent() - fade_time) / duration : 1.;
  t = fmax(0., fmin(1., t));
  // Smoothstep; a reversed transition starts at the current opacity.
  double eased = t * t * (3. - 2. * t);
  opacity = fade_start + (fade_target - fade_start) * eased;
  apply_opacity();
#ifdef MC_TRACE
  fprintf(stderr, "mc fade %.3f at %.3f\n", opacity, CFAbsoluteTimeGetCurrent());
#endif
  if (t >= 1.) stop_timer(&fade_timer);
}

static void fade_to(float target) {
  stop_timer(&fade_timer);
  fade_start = opacity;
  fade_target = target;
  fade_time = CFAbsoluteTimeGetCurrent();
  if (g_settings.mission_control_fade == 0 || opacity == target) {
    opacity = target;
    apply_opacity();
    return;
  }
  fade_timer = CFRunLoopTimerCreate(NULL, fade_time, 1. / 120., 0, 0, fade_tick, NULL);
  CFRunLoopAddTimer(CFRunLoopGetMain(), fade_timer, kCFRunLoopCommonModes);
}

static void snapshot_displays(void) {
  free(displays);
  displays = NULL;
  display_count = 0;
  uint32_t count = 0;
  if (CGGetActiveDisplayList(0, NULL, &count) != kCGErrorSuccess || !count) return;
  CGDirectDisplayID* ids = calloc(count, sizeof(*ids));
  if (!ids) return;
  if (CGGetActiveDisplayList(count, ids, &display_count) != kCGErrorSuccess) {
    free(ids);
    display_count = 0;
    return;
  }
  displays = calloc(display_count, sizeof(*displays));
  if (!displays) { free(ids); display_count = 0; return; }
  CFStringRef focused = SLSCopyActiveMenuBarDisplayIdentifier(SLSMainConnectionID());
  for (uint32_t i = 0; i < display_count; ++i) {
    displays[i].bounds = CGDisplayBounds(ids[i]);
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(ids[i]);
    CFStringRef name = uuid ? CFUUIDCreateString(NULL, uuid) : NULL;
    displays[i].affected = display_count == 1 || (focused && name && CFEqual(focused, name));
    if (name) CFRelease(name);
    if (uuid) CFRelease(uuid);
  }
  if (focused) CFRelease(focused);
  free(ids);
}

static bool shield_level(uint32_t wid) {
  int64_t level = 0;
  return SLSGetWindowLevel(SLSMainConnectionID(), wid, &level) == kCGErrorSuccess
      && level >= 17 && level < 20;
}

static bool shield_shown(uint32_t wid) {
  // Ordered-in is not sufficient: WindowManager keeps invisible placeholder
  // windows ordered during/after transitions. Query visible IDs, not full info.
  CFArrayRef visible = CGWindowListCreate(kCGWindowListOptionOnScreenOnly, 0);
  bool shown = false;
  for (CFIndex i = 0; visible && i < CFArrayGetCount(visible); ++i) {
    if ((uint32_t)(uintptr_t)CFArrayGetValueAtIndex(visible, i) == wid) {
      shown = shield_level(wid);
      break;
    }
  }
  if (visible) CFRelease(visible);
  return shown;
}

static void include_shield_display(uint32_t wid) {
  CGRect bounds;
  if (SLSGetWindowBounds(SLSMainConnectionID(), wid, &bounds) != kCGErrorSuccess) return;
  for (uint32_t j = 0; j < display_count; ++j) {
    if (CGRectContainsPoint(bounds, CGPointMake(CGRectGetMidX(displays[j].bounds),
                                                CGRectGetMidY(displays[j].bounds))))
      displays[j].affected = true;
  }
}

static CFIndex shield_index(uint32_t wid);

static bool any_shield_shown(void) {
  CFArrayRef visible = CGWindowListCreate(kCGWindowListOptionOnScreenOnly, 0);
  bool shown = false;
  for (CFIndex i = 0; visible && i < CFArrayGetCount(visible); ++i) {
    uint32_t wid = (uint32_t)(uintptr_t)CFArrayGetValueAtIndex(visible, i);
    if (shield_index(wid) != kCFNotFound && shield_level(wid)) {
      shown = true;
      break;
    }
  }
  if (visible) CFRelease(visible);
  return shown;
}

static void seed_visible_shield_displays(void) {
  CFArrayRef visible = CGWindowListCreate(kCGWindowListOptionOnScreenOnly, 0);
  for (CFIndex i = 0; visible && i < CFArrayGetCount(visible); ++i) {
    uint32_t wid = (uint32_t)(uintptr_t)CFArrayGetValueAtIndex(visible, i);
    if (shield_index(wid) != kCFNotFound && shield_level(wid)) include_shield_display(wid);
  }
  if (visible) CFRelease(visible);
}

static void set_active(bool value) {
  // A new show invalidates an older exit even when we were already active.
  if (value) stop_timer(&exit_timer);
  if (active == value || (value && !g_settings.mission_control)) return;
  stop_timer(&exit_timer);
  active = value;
#ifdef MC_TRACE
  fprintf(stderr, "mc active %d at %.3f\n", value, CFAbsoluteTimeGetCurrent());
#endif
  if (value) {
    snapshot_displays();
    seed_visible_shield_displays();
    fade_to(0.f);
  } else {
    ax_session = exit_authorized = false;
    // Reconcile only deferred changes, while still invisible. Never unhide or
    // redraw every border just because Mission Control has closed.
    stop_timer(&fade_timer);
    // Preserve opacity for a quick reversal instead of jumping to zero.
    apply_opacity();
    if (pending_space_update) {
      pending_space_update = false;
      windows_draw_borders_on_current_spaces(&g_windows);
    }
    windows_determine_and_focus_active_window(&g_windows);
    for (int i = 0; i < g_windows.capacity; ++i) {
      for (struct bucket* b = g_windows.buckets[i]; b; b = b->next) {
        struct border* border = b->value;
        if (border && border->mission_control_dirty) {
          border->mission_control_dirty = false;
          border_update(border, false);
        }
      }
    }
    fade_to(1.f);
  }
}

static void exit_tick(CFRunLoopTimerRef timer, void* context) {
  if (timer != exit_timer) return;
  if (!any_shield_shown()) {
    if (++exit_absent_ticks >= 6) set_active(false);
  } else exit_absent_ticks = 0;
  if (exit_timer && ++exit_ticks >= 120) stop_timer(&exit_timer);
}

static void request_exit(void) {
  stop_timer(&exit_timer);
  exit_ticks = exit_absent_ticks = 0;
  exit_timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 1. / 120.,
                                1. / 120., 0, 0, exit_tick, NULL);
  CFRunLoopAddTimer(CFRunLoopGetMain(), exit_timer, kCFRunLoopCommonModes);
}

static void expose_notification(AXObserverRef observer, AXUIElementRef element,
                                CFStringRef notification, void* context) {
  if (!g_settings.mission_control) return;
  if (CFEqual(notification, CFSTR("AXExposeShowAllWindows"))
      || CFEqual(notification, CFSTR("AXExposeShowFrontWindows"))
      || CFEqual(notification, CFSTR("AXExposeShowDesktop"))) {
    ax_session = true;
    exit_authorized = false;
    set_active(true);
  } else if (CFEqual(notification, CFSTR("AXExposeExit")) && active) {
    // AX exit can precede the last frame; confirm absence before revealing.
    exit_authorized = true;
    request_exit();
  }
}

static void observe_owner(pid_t pid, unsigned index) {
  struct mc_owner* owner = &owners[index];
  if (owner->pid == pid) return;
  if (owner->observer) {
    CFRunLoopRemoveSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(owner->observer),
                          kCFRunLoopCommonModes);
    CFRelease(owner->observer);
    owner->observer = NULL;
  }
  owner->pid = pid;
  AXObserverRef observer = NULL;
  if (AXObserverCreate(pid, expose_notification, &observer) != kAXErrorSuccess) return;
  AXUIElementRef app = AXUIElementCreateApplication(pid);
  AXUIElementSetMessagingTimeout(app, 0.05f);
  if (index == 0) AXObserverAddNotification(observer, app, CFSTR("*"), NULL);
  else {
    AXObserverAddNotification(observer, app, CFSTR("AXExposeShowAllWindows"), NULL);
    AXObserverAddNotification(observer, app, CFSTR("AXExposeShowFrontWindows"), NULL);
    AXObserverAddNotification(observer, app, CFSTR("AXExposeShowDesktop"), NULL);
    AXObserverAddNotification(observer, app, CFSTR("AXExposeExit"), NULL);
  }
  CFRelease(app);
  owner->observer = observer;
  CFRunLoopAddSource(CFRunLoopGetMain(), AXObserverGetRunLoopSource(observer), kCFRunLoopCommonModes);
}

static CFIndex shield_index(uint32_t wid) {
  for (CFIndex i = 0; shields && i < CFArrayGetCount(shields); ++i) {
    uint32_t candidate = 0;
    CFNumberGetValue(CFArrayGetValueAtIndex(shields, i), kCFNumberSInt32Type, &candidate);
    if (candidate == wid) return i;
  }
  return kCFNotFound;
}

static void add_shield(uint32_t wid) {
  if (shield_index(wid) != kCFNotFound) return;
  CFNumberRef number = CFNumberCreate(NULL, kCFNumberSInt32Type, &wid);
  CFArrayAppendValue(shields, number);
  CFRelease(number);
}

void mission_control_request_notifications(struct table* windows) {
  size_t count = 0;
  size_t capacity = windows->count + (shields ? CFArrayGetCount(shields) : 0);
  uint32_t* ids = calloc(capacity ? capacity : 1, sizeof(*ids));
  if (!ids) return;
  for (int i = 0; i < windows->capacity; ++i) {
    for (struct bucket* b = windows->buckets[i]; b; b = b->next)
      if (b->value) ids[count++] = *(uint32_t*)b->key;
  }
  for (CFIndex i = 0; shields && i < CFArrayGetCount(shields); ++i)
    CFNumberGetValue(CFArrayGetValueAtIndex(shields, i), kCFNumberSInt32Type, &ids[count++]);
  SLSRequestNotificationsForWindows(SLSMainConnectionID(), ids, (int)count);
  free(ids);
}

bool mission_control_window_event(uint32_t event, uint32_t wid) {
  if (!shields) return false;
  CFIndex index = shield_index(wid);
  if (index == kCFNotFound && event == EVENT_WINDOW_CREATE) {
    int owner_cid = 0;
    pid_t pid = 0;
    char name[64] = {};
    SLSGetWindowOwner(SLSMainConnectionID(), wid, &owner_cid);
    SLSConnectionGetPID(owner_cid, &pid);
    proc_name(pid, name, sizeof(name));
    if (strcmp(name, "WindowManager") && strcmp(name, "Dock")) return false;
    observe_owner(pid, strcmp(name, "Dock") ? 0 : 1);
    int64_t level = 0;
    SLSGetWindowLevel(SLSMainConnectionID(), wid, &level);
    if (level < 0 || level >= 20) return true;
    add_shield(wid);
    mission_control_request_notifications(&g_windows);
    index = shield_index(wid);
  }
  if (index == kCFNotFound) return false;
  if (event == EVENT_WINDOW_CLOSE || event == EVENT_WINDOW_DESTROY) {
    CFArrayRemoveValueAtIndex(shields, index);
    mission_control_request_notifications(&g_windows);
  }
  if (event == EVENT_WINDOW_HIDE || event == EVENT_WINDOW_CLOSE || event == EVENT_WINDOW_DESTROY) {
    // In an AX-observed session, a late hide from the previous transition
    // must not authorize exit after a newer show has invalidated that exit.
    if (active && (!ax_session || exit_authorized)) request_exit();
  } else if ((event == EVENT_WINDOW_CREATE || event == EVENT_WINDOW_UNHIDE
              || event == EVENT_WINDOW_LEVEL || event == EVENT_WINDOW_REORDER)
             && shield_shown(wid)) {
    if (!active) set_active(true);
    include_shield_display(wid);
    apply_opacity();
  }
  return true;
}

void mission_control_settings_changed(void) {
  if (!g_settings.mission_control) {
    stop_timer(&fade_timer);
    stop_timer(&exit_timer);
    if (active) {
      // Keep the suppression policy in force until deferred work is drained.
      g_settings.mission_control = true;
      set_active(false);
      g_settings.mission_control = false;
      stop_timer(&fade_timer);
    }
    opacity = 1.f;
    apply_opacity();
  } else if (any_shield_shown()) set_active(true);
}

void mission_control_init(void) {
  shields = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
  // One discovery on startup, not a full window-list query per event/frame.
  CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID);
  for (CFIndex i = 0; list && i < CFArrayGetCount(list); ++i) {
    CFDictionaryRef window = CFArrayGetValueAtIndex(list, i);
    CFStringRef name = CFDictionaryGetValue(window, kCGWindowOwnerName);
    if (!name) continue;
    unsigned index;
    if (CFEqual(name, CFSTR("WindowManager"))) index = 0;
    else if (CFEqual(name, CFSTR("Dock"))) index = 1;
    else continue;
    int pid = 0, level = 0;
    uint32_t wid = 0;
    CFNumberRef p = CFDictionaryGetValue(window, kCGWindowOwnerPID);
    CFNumberRef l = CFDictionaryGetValue(window, kCGWindowLayer);
    CFNumberRef w = CFDictionaryGetValue(window, kCGWindowNumber);
    if (p) CFNumberGetValue(p, kCFNumberIntType, &pid);
    if (l) CFNumberGetValue(l, kCFNumberIntType, &level);
    if (w) CFNumberGetValue(w, kCFNumberSInt32Type, &wid);
    if (pid > 0) observe_owner(pid, index);
    if (wid && level >= 0 && level < 20) add_shield(wid);
  }
  if (list) CFRelease(list);
  mission_control_request_notifications(&g_windows);
  if (any_shield_shown()) {
    set_active(true);
    apply_opacity();
  }
}
