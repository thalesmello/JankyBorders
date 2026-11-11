#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <stdbool.h>
#include <stdint.h>

struct border;
struct table;

void mission_control_init(void);
bool mission_control_active(void);
bool mission_control_window_event(uint32_t event, uint32_t wid);
void mission_control_request_notifications(struct table* windows);
bool mission_control_border_affected(struct border* border);
float mission_control_opacity_for_border(struct border* border);
void mission_control_settings_changed(void);
void mission_control_defer_space_update(void);
