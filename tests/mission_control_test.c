// Deterministic tests; no Mission Control invocation or desktop mutation.
#include "../src/mission_control.c"
#include "../src/parse.h"
#include <assert.h>

struct table g_windows;
struct settings g_settings;
static bool mock_shown, mock_two_displays;
static unsigned updates, focuses, spaces;

CFArrayRef CGWindowListCreate(CGWindowListOption options, CGWindowID relative) {
  const void* wids[] = { (void*)(uintptr_t)1, (void*)(uintptr_t)2 };
  return CFArrayCreate(NULL, wids, mock_shown ? (mock_two_displays ? 2 : 1) : 0, NULL);
}

CGError SLSWindowIsOrderedIn(int cid, uint32_t wid, bool* shown) {
  *shown = mock_shown;
  return kCGErrorSuccess;
}
CGError SLSGetWindowLevel(int cid, uint32_t wid, int64_t* level) {
  *level = 17;
  return kCGErrorSuccess;
}
CGError SLSGetWindowBounds(int cid, uint32_t wid, CGRect* bounds) {
  *bounds = CGRectMake(wid == 2 ? 1000 : 0, 0, 1000, 1000);
  return kCGErrorSuccess;
}
void border_set_mission_control_opacity(struct border* border, float value) {
  border->mission_control_opacity = value;
}
void border_update(struct border* border, bool try_async) { ++updates; }
void windows_draw_borders_on_current_spaces(struct table* windows) { ++spaces; }
void windows_determine_and_focus_active_window(struct table* windows) { ++focuses; }

static void test_parsing(void) {
  struct settings settings = { .mission_control_fade = 80.f };
  char* valid[] = { "mission_control=on", "mission_control_fade=125" };
  assert(parse_settings(&settings, 2, valid) == BORDER_UPDATE_MASK_SETTING);
  assert(settings.mission_control && settings.mission_control_fade == 125.f);
  char* invalid[] = { "mission_control_fade=-1", "mission_control_fade=nan",
                       "mission_control_fade=501", "mission_control_fade=20junk" };
  assert(parse_settings(&settings, 4, invalid) == 0);
  assert(settings.mission_control_fade == 125.f);
  char* zero[] = { "mission_control_fade=0" };
  assert(parse_settings(&settings, 1, zero) == BORDER_UPDATE_MASK_SETTING);
  assert(settings.mission_control_fade == 0.f);
}

int main(void) {
  test_parsing();
  g_settings.mission_control = true;
  g_settings.mission_control_fade = 80.f;
  display_count = 2;
  displays = calloc(display_count, sizeof(*displays));
  displays[0] = (struct mc_display){CGRectMake(0, 0, 1000, 1000), true};
  displays[1] = (struct mc_display){CGRectMake(1000, 0, 1000, 1000), false};
  struct border border = { .target_bounds = CGRectMake(20, 20, 200, 100) };
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&border.mutex, &attr);
  pthread_mutexattr_destroy(&attr);
  struct bucket bucket = { .value = &border };
  struct bucket* buckets[] = { &bucket };
  g_windows = (struct table){ .capacity = 1, .count = 1, .buckets = buckets };

  // Display membership is independent of the first fade frame's opacity.
  active = true;
  opacity = 1.f;
  assert(mission_control_border_affected(&border));
  opacity = .25f;
  assert(mission_control_opacity_for_border(&border) == .25f);
  border.target_bounds.origin.x = 1020;
  assert(!mission_control_border_affected(&border));
  assert(mission_control_opacity_for_border(&border) == 1.f);
  border.target_bounds.origin.x = 20;

  // A new show must cancel the old exit even if active is already true.
  request_exit();
  assert(exit_timer);
  set_active(true);
  assert(!exit_timer && active);

  shields = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
  add_shield(1);
  add_shield(2);
  mock_shown = mock_two_displays = true;
  seed_visible_shield_displays();
  assert(displays[0].affected && displays[1].affected);
  displays[1].affected = false;
  mock_shown = mock_two_displays = false;

  // An old-session hide cannot reauthorize exit after a newer AX show.
  expose_notification(NULL, NULL, CFSTR("AXExposeShowAllWindows"), NULL);
  mission_control_window_event(EVENT_WINDOW_HIDE, 1);
  assert(active && ax_session && !exit_authorized && !exit_timer);
  expose_notification(NULL, NULL, CFSTR("AXExposeExit"), NULL);
  assert(exit_authorized && exit_timer);
  expose_notification(NULL, NULL, CFSTR("AXExposeShowAllWindows"), NULL);
  mission_control_window_event(EVENT_WINDOW_HIDE, 1);
  assert(active && !exit_authorized && !exit_timer);
  ax_session = false;

  // Absence must be confirmed, not inferred from one old shield-hide event.
  request_exit();
  exit_tick(exit_timer, NULL);
  assert(active);
  exit_tick(exit_timer, NULL);
  assert(active);
  for (int i = 0; i < 4; ++i) exit_tick(exit_timer, NULL);
  assert(!active && !exit_timer);
  stop_timer(&fade_timer);

  // Timed interpolation reaches exact endpoints and reverses from current alpha.
  fade_start = 1.f; fade_target = 0.f;
  fade_time = CFAbsoluteTimeGetCurrent() - .04;
  fade_tick(NULL, NULL);
  assert(opacity > .3f && opacity < .7f);
  float mid = opacity;
  fade_to(1.f);
  assert(fade_start == mid);
  fade_time = CFAbsoluteTimeGetCurrent() - 1.;
  fade_tick(fade_timer, NULL);
  assert(opacity == 1.f && !fade_timer);

  // Disabling must drain deferred geometry/space/focus before restoring.
  active = true;
  opacity = 0.f;
  border.mission_control_dirty = true;
  pending_space_update = true;
  g_settings.mission_control = false;
  mission_control_settings_changed();
  assert(!active && !fade_timer && opacity == 1.f);
  assert(!border.mission_control_dirty && !pending_space_update);
  assert(updates == 1 && spaces == 1 && focuses >= 1);
  assert(border.mission_control_opacity == 1.f);

  CFRelease(shields);
  free(displays);
  pthread_mutex_destroy(&border.mutex);
  puts("Mission Control state, scope, fade, parser tests passed");
  return 0;
}
