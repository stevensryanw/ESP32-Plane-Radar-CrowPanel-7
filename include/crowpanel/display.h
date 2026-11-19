#pragma once

#include <cstdint>

namespace crowpanel::display {

constexpr int kWidth = 800;
constexpr int kHeight = 480;

/** Initialize RGB panel, LVGL, GT911 touch, and PWM backlight. */
bool init();

/** Advance LVGL's clock and service rendering/input. Call frequently. */
void loop();

bool ready();
const char* lastError();

/**
 * Copy a block of RGB565 pixels (LVGL's native format) straight to the panel.
 * LVGL is not told, so use it only where no LVGL object is drawn on top.
 * `stride` is the length of a source row in pixels.
 */
void blit(int x, int y, int width, int height, const uint16_t* pixels, int stride);

/**
 * What the firmware is busy with, for the "slip" serial command's breakdown of
 * which work makes the panel's picture slip. Does nothing unless the firmware
 * was built with CROWPANEL_SERIAL_DEBUG.
 */
enum Activity : uint32_t {
  kActivityFetch = 1 << 0,        // Aircraft request, reply and parsing.
  kActivityNetworkWait = 1 << 1,  // Servicing the UI while a request waits.
  kActivityDataRedraw = 1 << 2,   // New data applied to the model and drawn.
  kActivityGlide = 1 << 3,        // Repaint between snapshots.
  kActivityLvgl = 1 << 4,         // LVGL sent pixels to the panel.
  kActivityTiles = 1 << 5,        // Radar tiles sent to the panel.
};
void beginActivity(uint32_t activity);
void endActivity(uint32_t activity);

/** Marks an activity for as long as the object lives. */
class ActivityScope {
 public:
  explicit ActivityScope(uint32_t activity) : activity_(activity) {
    beginActivity(activity_);
  }
  ~ActivityScope() { endActivity(activity_); }
  ActivityScope(const ActivityScope&) = delete;
  ActivityScope& operator=(const ActivityScope&) = delete;

 private:
  uint32_t activity_;
};

void setBrightness(uint8_t percent);
uint8_t brightness();

/** Detected GT911 address (normally 0x5D or 0x14), or zero if absent. */
uint8_t touchAddress();

}  // namespace crowpanel::display
