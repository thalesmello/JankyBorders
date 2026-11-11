#include "border.h"
#include "hashtable.h"
#include "misc/extern.h"
#include "windows.h"
#include "mission_control.h"
#include <pthread.h>
#include <time.h>

extern struct settings g_settings;
static bool border_has_visible_content(struct border*, struct settings*);

struct settings* border_get_settings(struct border* border) {
  assert(pthread_main_np() != 0);
  return border->setting_override.enabled
         ? &border->setting_override
         : &g_settings;
}

static void border_destroy_window(struct border* border) {
  if (border->context) CGContextRelease(border->context);
  if (border->wid) SLSReleaseWindow(border->cid, border->wid);
  border->wid = 0;
  border->context = NULL;
}

static bool border_check_too_small(struct border* border, CGRect window_frame) {
  CGRect smallest_rect = CGRectInset(window_frame, 1.0, 1.0);
  if (smallest_rect.size.width < 2.f * border->inner_radius
      || smallest_rect.size.height < 2.f * border->inner_radius) {
    return true;
  }
  return false;
}

static bool border_calculate_bounds(struct border* border, CGRect* frame, struct settings* settings) {
  CGRect window_frame;
  if (border->is_proxy) window_frame = border->target_bounds;
  else SLSGetWindowBounds(border->cid, border->target_wid, &window_frame);

  border->target_bounds = window_frame;
  border->too_small = border_check_too_small(border, window_frame);
  if (border->too_small) {
    border_hide(border);
    return false;
  }

  float border_offset = - settings->border_width - BORDER_PADDING;
  *frame = CGRectInset(window_frame, border_offset, border_offset);

  border->origin = frame->origin;
  frame->origin = CGPointZero;


  window_frame.origin = (CGPoint){ -border_offset, -border_offset };
  border->drawing_bounds = window_frame;

  return true;
}

static void border_draw(struct border* border, CGRect frame, struct settings* settings) {
  CGContextSaveGState(border->context);
  border->needs_redraw = false;
  struct color_style color_style = border->focused
                                   ? settings->active_window
                                   : settings->inactive_window;

  CGGradientRef gradient = NULL;
  CGPoint gradient_dir[2];
  if (color_style.stype == COLOR_STYLE_SOLID
     || color_style.stype == COLOR_STYLE_GLOW) {
    bool glow = color_style.stype == COLOR_STYLE_GLOW;
    drawing_set_stroke_and_fill(border->context, color_style.color, glow);
  } else if (color_style.stype == COLOR_STYLE_GRADIENT) {
    CGAffineTransform trans = CGAffineTransformMakeScale(frame.size.width,
                                                         frame.size.height);
    gradient = drawing_create_gradient(&color_style.gradient,
                                       trans,
                                       gradient_dir          );
  }

  CGContextSetLineWidth(border->context, settings->border_width);
  CGContextClearRect(border->context, frame);

  CGRect path_rect = border->drawing_bounds;
  CGMutablePathRef inner_clip_path = CGPathCreateMutable();
  if (settings->border_style == BORDER_STYLE_SQUARE
      && settings->border_order == BORDER_ORDER_ABOVE
      && settings->border_width >= BORDER_TSMW) {
    // Inset the frame to overlap the rounding of macOS windows to create a
    // truly square border
    path_rect = CGRectInset(border->drawing_bounds,
                            BORDER_TSMN,
                            BORDER_TSMN            );

    CGPathAddRect(inner_clip_path, NULL, path_rect);
  } else {
    CGPathAddRoundedRect(inner_clip_path,
                         NULL,
                         CGRectInset(path_rect, 1.0, 1.0),
                         border->inner_radius,
                         border->inner_radius             );
  }
  drawing_clip_between_rect_and_path(border->context, frame, inner_clip_path);

  if (settings->border_style == BORDER_STYLE_SQUARE) {
    if (color_style.stype == COLOR_STYLE_SOLID
       || color_style.stype == COLOR_STYLE_GLOW) {
      drawing_draw_square_with_inset(border->context,
                                     path_rect,
                                     -settings->border_width / 2.f);
    }
    else if (color_style.stype == COLOR_STYLE_GRADIENT) {
      drawing_draw_square_gradient_with_inset(border->context,
                                              gradient,
                                              gradient_dir,
                                              path_rect,
                                              -settings->border_width / 2.f);
    }
  } else {
    float corner_radius = settings->border_style == BORDER_STYLE_ROUND_UNIFORM ? 9.0 : border->radius;

    if (settings->border_style == BORDER_STYLE_ROUND_UNIFORM) {
      drawing_draw_rounded_rect_with_inset(border->context,
                                           path_rect,
                                           corner_radius,
                                           true            );
    }

    if (color_style.stype == COLOR_STYLE_SOLID
       || color_style.stype == COLOR_STYLE_GLOW) {
      drawing_draw_rounded_rect_with_inset(border->context,
                                           path_rect,
                                           corner_radius,
                                           false           );
    } else if (color_style.stype == COLOR_STYLE_GRADIENT) {
      drawing_draw_rounded_gradient_with_inset(border->context,
                                               gradient,
                                               gradient_dir,
                                               path_rect,
                                               corner_radius  );
    }
  }
  CGGradientRelease(gradient);

  if (settings->show_background && settings->border_order != 1) {
    CGContextRestoreGState(border->context);
    CGContextSaveGState(border->context);
    color_style = settings->background;
    if (color_style.stype == COLOR_STYLE_SOLID
       || color_style.stype == COLOR_STYLE_GLOW) {
      drawing_draw_filled_path(border->context,
                               inner_clip_path,
                               color_style.color);
    }
  }
  CFRelease(inner_clip_path);
  CGContextFlush(border->context);
  CGContextRestoreGState(border->context);
  SLSFlushWindowContentRegion(border->cid, border->wid, NULL);
  SLSWindowThaw(border->cid, border->wid);
  border->content_visible = border_has_visible_content(border, settings);
}

void border_create_window(struct border* border, CGRect frame, bool unmanaged, bool hidpi) {
  pthread_mutex_lock(&border->mutex);
  int cid = border->cid;
  border->wid = window_create(cid, frame, hidpi, unmanaged);

  border->frame = frame;
  border->needs_redraw = true;
  border->context = SLWindowContextCreate(cid, border->wid, NULL);
  CGContextSetInterpolationQuality(border->context, kCGInterpolationNone);

  if (!border->sid) border->sid = window_space_id(cid, border->target_wid);
  window_send_to_space(cid, border->wid, border->sid);
  pthread_mutex_unlock(&border->mutex);
}

void border_update_internal(struct border* border, struct settings* settings) {
  if (border->external_proxy_wid) return;
  if (!border->is_proxy && mission_control_active()
      && mission_control_border_affected(border) && border->wid) {
    border->mission_control_dirty = true;
    return;
  }

  int cid = border->cid;
  CGRect frame;
  if (!border_calculate_bounds(border, &frame, settings)) return;

  uint64_t tags = window_tags(cid, border->target_wid);
  border->sticky = tags & WINDOW_TAG_STICKY;
  if (!border->sticky && !is_space_visible(cid, border->sid)) return;


  bool shown = false;
  SLSWindowIsOrderedIn(cid, border->target_wid, &shown);
  if (!shown && !border->is_proxy) {
    border_hide(border);
    return;
  } 

  int level = window_level(cid, border->target_wid);
  int sub_level = window_sub_level(cid, border->target_wid);

  if (!border->wid) {
    border_create_window(border,
                         frame,
                         border->is_proxy,
                         settings->hidpi  );
  }

  bool disabled_update = false;
  if (!CGRectEqualToRect(frame, border->frame)) {
    disabled_update = true;
    SLSDisableUpdate(cid);

    CFTypeRef frame_region;
    CGSNewRegionWithRect(&frame, &frame_region);

    SLSWindowFreezeWithOptions(border->cid, border->wid, NULL);
    SLSSetWindowShape(border->cid, border->wid, border->origin.x, border->origin.y, frame_region);
    CFRelease(frame_region);

    border->needs_redraw = true;
    border->frame = frame;
  }

  if (border->needs_redraw) border_draw(border, frame, settings);
  border->mission_control_opacity = mission_control_opacity_for_border(border);

  CFTypeRef transaction = SLSTransactionCreate(cid);
  if (!transaction) {
    if (disabled_update) SLSReenableUpdate(cid);
    return;
  }
  if (!border->is_proxy)
    SLSTransactionSetWindowAlpha(transaction, border->wid, border->mission_control_opacity);
  SLSTransactionMoveWindowWithGroup(transaction, border->wid, border->origin);

  if (!border->is_proxy) {
    CGAffineTransform transform = CGAffineTransformIdentity;
    transform.tx = -border->origin.x;
    transform.ty = -border->origin.y;
    SLSTransactionSetWindowTransform(transaction,
                                     border->wid,
                                     0,
                                     0,
                                     transform   );
  }
  SLSTransactionSetWindowLevel(transaction, border->wid, level);
  SLSTransactionSetWindowSubLevel(transaction, border->wid, sub_level);
  SLSTransactionOrderWindow(transaction,
                            border->wid,
                            settings->border_order,
                            border->target_wid      );
  SLSTransactionCommit(transaction, 0);
  CFRelease(transaction);

  uint64_t set_tags = (1ULL << 1) | (1ULL << 9);
  uint64_t clear_tags = 0;

  if (border->sticky) {
    set_tags |= WINDOW_TAG_STICKY;
    clear_tags |= (1ULL << 45);
  }

  SLSSetWindowTags(cid, border->wid, &set_tags, 0x40);
  SLSClearWindowTags(cid, border->wid, &clear_tags, 0x40);

  if (disabled_update) SLSReenableUpdate(cid);
}

static void* border_update_async_proc(void* context) {
  struct {
    struct border* border;
    struct settings settings;
  }* payload = context;

  pthread_mutex_lock(&payload->border->mutex);
  border_update_internal(payload->border, &payload->settings);
  pthread_mutex_unlock(&payload->border->mutex);
  free(payload);
  return NULL;
}

void border_init(struct border* border, int cid) {
  memset(border, 0, sizeof(struct border));
  border->mission_control_opacity = 1.f;
  pthread_mutexattr_t mattr;
  pthread_mutexattr_init(&mattr);
  pthread_mutexattr_settype(&mattr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&border->mutex, &mattr);
  animation_init(&border->animation);
  if (cid) border->cid = cid;
  else border->cid = SLSMainConnectionID();
}

struct border* border_create() {
  struct border* border = malloc(sizeof(struct border));
  int cid = 0;
  SLSNewConnection(0, &cid);
  border_init(border, cid);
  return border;
}

void border_destroy(struct border* border) {
  border_hide(border);
  dispatch_async(dispatch_get_main_queue(), ^{
    pthread_mutex_lock(&border->mutex);
    border_destroy_window(border);
    if (border->proxy) border_destroy(border->proxy);
    animation_stop(&border->animation);
    if (!border->is_proxy && border->cid != SLSMainConnectionID())
      SLSReleaseConnection(border->cid);
    pthread_mutex_unlock(&border->mutex);
    free(border);
  });
}

void border_move(struct border* border) {
  pthread_mutex_lock(&border->mutex);
  if (mission_control_active() && mission_control_border_affected(border)) {
    border->mission_control_dirty = true;
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  if (border->external_proxy_wid) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }
  pthread_mutex_unlock(&border->mutex);

  struct settings* settings = border_get_settings(border);
  dispatch_async(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0), ^{
    pthread_mutex_lock(&border->mutex);
    CGRect window_frame;
    SLSGetWindowBounds(border->cid, border->target_wid, &window_frame);
    CGPoint origin = { .x = window_frame.origin.x
                            - settings->border_width
                            - BORDER_PADDING,
                       .y = window_frame.origin.y
                            - settings->border_width
                            - BORDER_PADDING          };

    CFTypeRef transaction = SLSTransactionCreate(border->cid);
    if (transaction) {
      SLSTransactionMoveWindowWithGroup(transaction, border->wid, origin);
      SLSTransactionCommit(transaction, 0);
      CFRelease(transaction);
    }
    border->target_bounds = window_frame;
    border->origin = origin;
    pthread_mutex_unlock(&border->mutex);
  });
}

void border_update(struct border* border, bool try_async) {
  pthread_mutex_lock(&border->mutex);
  struct settings* settings = border_get_settings(border);
  border_update_internal(border, settings);
  pthread_mutex_unlock(&border->mutex);
  return;

  if (!border->wid || !try_async) {
    border_update_internal(border, settings);
    pthread_mutex_unlock(&border->mutex);
    return;
  }

  struct payload {
    struct border* border;
    struct settings settings;
  }* payload = malloc(sizeof(struct payload));

  payload->border = border;
  payload->settings = *settings;

  pthread_t thread;
  pthread_create(&thread, NULL, border_update_async_proc, payload);
  pthread_detach(thread);
  pthread_mutex_unlock(&border->mutex);
}

void border_hide(struct border* border) {
  pthread_mutex_lock(&border->mutex);
  if (border->wid) {
    CFTypeRef transaction = SLSTransactionCreate(border->cid);
    if (transaction) {
      SLSTransactionOrderWindow(transaction,
                                border->wid,
                                0,
                                border->target_wid);
      SLSTransactionCommit(transaction, 0);
      CFRelease(transaction);
    }
  }
  pthread_mutex_unlock(&border->mutex);
}

static bool border_has_visible_content(struct border* border, struct settings* settings) {
  struct color_style color = border->focused ? settings->active_window : settings->inactive_window;
  if (settings->show_background) return true;
  if (color.stype == COLOR_STYLE_GRADIENT)
    return ((color.gradient.color1 | color.gradient.color2) & 0xff000000U) != 0;
  return (color.color & 0xff000000U) != 0;
}

void border_set_mission_control_opacity(struct border* border, float opacity) {
  pthread_mutex_lock(&border->mutex);
  if (border->mission_control_opacity != opacity) {
    border->mission_control_opacity = opacity;
    // Use the rendered buffer, not pending settings: a deferred transparent
    // redraw must not leave an old opaque buffer visible during Mission Control.
    if (border->wid && (border->content_visible || border->needs_redraw)) {
#ifdef MC_TRACE
      fprintf(stderr, "mc alpha wid=%u target=%u value=%.3f focused=%d\n", border->wid, border->target_wid, opacity, border->focused);
#endif
      CFTypeRef transaction = SLSTransactionCreate(border->cid);
      if (transaction) {
        SLSTransactionSetWindowAlpha(transaction, border->wid,
                                     border->external_proxy_wid ? 0.f : opacity);
        if (border->proxy) {
          border->proxy->mission_control_opacity = opacity;
          SLSTransactionSetWindowAlpha(transaction, border->proxy->wid,
                                       border->external_proxy_wid ? opacity : 0.f);
        }
        SLSTransactionCommit(transaction, 0);
        CFRelease(transaction);
      }
    }
  }
  pthread_mutex_unlock(&border->mutex);
}

void border_unhide(struct border* border) {
  pthread_mutex_lock(&border->mutex);
  if (border->too_small
      || border->external_proxy_wid
      || (!border->sticky && !is_space_visible(border->cid, border->sid))) {
    pthread_mutex_unlock(&border->mutex);
    return;
  }

  if (border->wid) {
    struct settings* settings = border_get_settings(border);
    CFTypeRef transaction = SLSTransactionCreate(border->cid);
    if (transaction) {
      border->mission_control_opacity = mission_control_opacity_for_border(border);
      SLSTransactionSetWindowAlpha(transaction, border->wid, border->mission_control_opacity);
      SLSTransactionOrderWindow(transaction,
                                border->wid,
                                settings->border_order,
                                border->target_wid      );
      SLSTransactionCommit(transaction, 0);
      CFRelease(transaction);
    }
  }
  pthread_mutex_unlock(&border->mutex);
}
