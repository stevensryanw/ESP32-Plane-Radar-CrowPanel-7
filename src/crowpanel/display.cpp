#include "crowpanel/display.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <Wire.h>
#include <driver/periph_ctrl.h>
#include <esp32s3/rom/cache.h>
#include <esp_heap_caps.h>
#include <esp_intr_alloc.h>
#include <esp_rom_gpio.h>
#include <esp_timer.h>
#include <hal/lcd_ll.h>
#include <lgfx/v1/platforms/esp32s3/Bus_RGB.hpp>
#include <lgfx/v1/platforms/esp32s3/Panel_RGB.hpp>
#include <lvgl.h>
#include <soc/gdma_channel.h>
#include <soc/gdma_struct.h>
#include <soc/gpio_periph.h>
#include <soc/gpio_sig_map.h>
#include <soc/io_mux_reg.h>
#include <soc/lcd_cam_struct.h>
#include <soc/mcpwm_struct.h>
#include <soc/periph_defs.h>
#include <soc/soc_caps.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifndef CROWPANEL_HW_REV
#define CROWPANEL_HW_REV 3
#endif

#ifndef CROWPANEL_TOUCH_SWAP_XY
#define CROWPANEL_TOUCH_SWAP_XY 0
#endif

#ifndef CROWPANEL_TOUCH_INVERT_X
#define CROWPANEL_TOUCH_INVERT_X 0
#endif

#ifndef CROWPANEL_TOUCH_INVERT_Y
#define CROWPANEL_TOUCH_INVERT_Y 0
#endif

#ifndef CROWPANEL_SERIAL_DEBUG
#define CROWPANEL_SERIAL_DEBUG 0
#endif

namespace crowpanel::display {
namespace {

constexpr int kBacklightPin = 2;
constexpr int kBacklightChannel = 7;
constexpr int kBacklightFrequencyHz = 5000;
constexpr int kBacklightResolutionBits = 8;

constexpr int kTouchSda = 19;
constexpr int kTouchScl = 20;
constexpr uint32_t kTouchClockHz = 400000;
constexpr uint8_t kPca9557Address = 0x18;
constexpr uint8_t kPca9557Output = 0x01;
constexpr uint8_t kPca9557Config = 0x03;
constexpr uint8_t kGt911Addresses[] = {0x5D, 0x14};
constexpr uint16_t kGt911PointInfo = 0x814E;
constexpr uint16_t kGt911FirstPoint = 0x814F;

// RGB timing (Elecrow's values for this panel). Leave it alone: a slower
// pixel clock stops the picture slipping but makes the panel whine. See
// "Checking the pixel stream" in docs/CROWPANEL.md.
constexpr uint32_t kPixelClockHz = 15000000;
constexpr int kHsyncFrontPorch = 40;
constexpr int kHsyncPulseWidth = 48;
constexpr int kHsyncBackPorch = 40;
constexpr int kVsyncFrontPorch = 1;
constexpr int kVsyncPulseWidth = 31;
constexpr int kVsyncBackPorch = 13;

constexpr int kDrawBufferLines = 40;
constexpr size_t kDrawBufferPixels = kWidth * kDrawBufferLines;

class CrowPanelGfx : public lgfx::LGFX_Device {
 public:
  CrowPanelGfx() {
    {
      auto cfg = bus_.config();
      cfg.panel = &panel_;

      cfg.pin_d0 = GPIO_NUM_15;
      cfg.pin_d1 = GPIO_NUM_7;
      cfg.pin_d2 = GPIO_NUM_6;
      cfg.pin_d3 = GPIO_NUM_5;
      cfg.pin_d4 = GPIO_NUM_4;
      cfg.pin_d5 = GPIO_NUM_9;
      cfg.pin_d6 = GPIO_NUM_46;
      cfg.pin_d7 = GPIO_NUM_3;
      cfg.pin_d8 = GPIO_NUM_8;
      cfg.pin_d9 = GPIO_NUM_16;
      cfg.pin_d10 = GPIO_NUM_1;
      cfg.pin_d11 = GPIO_NUM_14;
      cfg.pin_d12 = GPIO_NUM_21;
      cfg.pin_d13 = GPIO_NUM_47;
      cfg.pin_d14 = GPIO_NUM_48;
      cfg.pin_d15 = GPIO_NUM_45;

      cfg.pin_henable = GPIO_NUM_41;
      cfg.pin_vsync = GPIO_NUM_40;
      cfg.pin_hsync = GPIO_NUM_39;
      cfg.pin_pclk = GPIO_NUM_0;
      cfg.freq_write = kPixelClockHz;
      cfg.hsync_polarity = 0;
      cfg.hsync_front_porch = kHsyncFrontPorch;
      cfg.hsync_pulse_width = kHsyncPulseWidth;
      cfg.hsync_back_porch = kHsyncBackPorch;
      cfg.vsync_polarity = 0;
      cfg.vsync_front_porch = kVsyncFrontPorch;
      cfg.vsync_pulse_width = kVsyncPulseWidth;
      cfg.vsync_back_porch = kVsyncBackPorch;
      cfg.pclk_active_neg = 1;
      cfg.de_idle_high = 0;
      cfg.pclk_idle_high = 0;
      bus_.config(cfg);
    }

    {
      auto cfg = panel_.config();
      cfg.memory_width = kWidth;
      cfg.memory_height = kHeight;
      cfg.panel_width = kWidth;
      cfg.panel_height = kHeight;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      panel_.config(cfg);
    }

    panel_.setBus(&bus_);
    setPanel(&panel_);
  }

  /** Scan-out buffer: kWidth x kHeight RGB565, high byte first, no padding. */
  const uint8_t* frameBuffer() { return bus_.getDMABuffer(0); }

 private:
  lgfx::Bus_RGB bus_;
  lgfx::Panel_RGB panel_;
};

CrowPanelGfx s_lcd;
lv_disp_draw_buf_t s_draw_buf;
lv_disp_drv_t s_display_driver;
lv_indev_drv_t s_touch_driver;
lv_color_t* s_buffer_a = nullptr;
lv_color_t* s_buffer_b = nullptr;

bool s_ready = false;
char s_error[96] = "not initialized";
uint8_t s_brightness = 75;
uint8_t s_touch_address = 0;
bool s_touch_pressed = false;
int16_t s_touch_x = 0;
int16_t s_touch_y = 0;
uint32_t s_last_touch_poll_ms = 0;
uint32_t s_last_lvgl_ms = 0;
TaskHandle_t s_ui_task = nullptr;

void setError(const char* message) {
  strncpy(s_error, message, sizeof(s_error) - 1);
  s_error[sizeof(s_error) - 1] = '\0';
  Serial.printf("CrowPanel: %s\n", s_error);
}

lv_color_t* allocateDrawBuffer() {
  const size_t bytes = kDrawBufferPixels * sizeof(lv_color_t);
  void* memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory == nullptr) {
    memory = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
  }
  return static_cast<lv_color_t*>(memory);
}

#if CROWPANEL_SERIAL_DEBUG
// Panel write recorder for the "rec" serial command, defined with the other
// serial debug aids below. Every write to the frame buffer is bracketed by
// these two calls.
enum : uint32_t { kWriteLvgl = 0, kWriteRadarTiles = 1 };
struct WriteStamp {
  int64_t started_us;
  int64_t vsync_us;
  uint32_t scan_position;
};
// The slip counter's marker, put back after anything is drawn over it.
void restoreSlipMarker(int x2, int y2);
WriteStamp beginRecordedWrite();
void endRecordedWrite(const WriteStamp& stamp, uint32_t kind, int x, int y,
                      int width, int height, const uint16_t* pixels, int stride);
#endif

void flushDisplay(lv_disp_drv_t* driver, const lv_area_t* area,
                  lv_color_t* pixels) {
  const int32_t width = area->x2 - area->x1 + 1;
  const int32_t height = area->y2 - area->y1 + 1;
#if CROWPANEL_SERIAL_DEBUG
  const WriteStamp stamp = beginRecordedWrite();
#endif
  s_lcd.startWrite();
  // With LV_COLOR_16_SWAP=0, LVGL pixels are native-order RGB565 (rgb565_t).
  // The RGB panel's frame buffer holds them byte-swapped, so name the source
  // format and let LovyanGFX convert. Passing swap565_t here, as LovyanGFX's
  // LVGL example does, is only correct when LV_COLOR_16_SWAP=1.
  s_lcd.pushImage(area->x1, area->y1, width, height,
                  reinterpret_cast<const lgfx::rgb565_t*>(pixels));
  s_lcd.endWrite();
#if CROWPANEL_SERIAL_DEBUG
  endRecordedWrite(stamp, kWriteLvgl, area->x1, area->y1, width, height,
                   reinterpret_cast<const uint16_t*>(pixels), width);
  endActivity(kActivityLvgl);
  restoreSlipMarker(area->x2, area->y2);
#endif
  lv_disp_flush_ready(driver);
}

bool i2cWriteByte(uint8_t address, uint8_t reg, uint8_t value) {
  Wire1.beginTransmission(address);
  Wire1.write(reg);
  Wire1.write(value);
  return Wire1.endTransmission() == 0;
}

bool i2cReadByte(uint8_t address, uint8_t reg, uint8_t* value) {
  Wire1.beginTransmission(address);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) {
    return false;
  }
  if (Wire1.requestFrom(address, static_cast<uint8_t>(1)) != 1) {
    return false;
  }
  *value = Wire1.read();
  return true;
}

bool pcaSetMode(uint8_t pin, bool input) {
  uint8_t config = 0;
  if (!i2cReadByte(kPca9557Address, kPca9557Config, &config)) {
    return false;
  }
  config = input ? static_cast<uint8_t>(config | (1U << pin))
                 : static_cast<uint8_t>(config & ~(1U << pin));
  return i2cWriteByte(kPca9557Address, kPca9557Config, config);
}

bool pcaSetState(uint8_t pin, bool high) {
  uint8_t output = 0;
  if (!i2cReadByte(kPca9557Address, kPca9557Output, &output)) {
    return false;
  }
  output = high ? static_cast<uint8_t>(output | (1U << pin))
                : static_cast<uint8_t>(output & ~(1U << pin));
  return i2cWriteByte(kPca9557Address, kPca9557Output, output);
}

void resetTouchController() {
#if CROWPANEL_HW_REV >= 3
  // V3 routes LCD/touch reset through PCA9557. Preserve unrelated outputs.
  if (!i2cWriteByte(kPca9557Address, kPca9557Config, 0xFF)) {
    Serial.println("CrowPanel: PCA9557 not detected; trying GT911 directly");
    return;
  }
  pcaSetMode(0, false);
  pcaSetMode(1, false);
  pcaSetState(0, false);
  pcaSetState(1, false);
  delay(20);
  pcaSetState(0, true);
  delay(100);
  pcaSetMode(1, true);
#endif
}

bool gt911ReadAt(uint8_t address, uint16_t reg, uint8_t* data, size_t size) {
  Wire1.beginTransmission(address);
  Wire1.write(static_cast<uint8_t>(reg >> 8));
  Wire1.write(static_cast<uint8_t>(reg));
  if (Wire1.endTransmission(false) != 0) {
    return false;
  }
  const size_t received = Wire1.requestFrom(address, static_cast<uint8_t>(size));
  if (received != size) {
    return false;
  }
  for (size_t i = 0; i < size; ++i) {
    data[i] = Wire1.read();
  }
  return true;
}

bool gt911WriteByte(uint16_t reg, uint8_t value) {
  if (s_touch_address == 0) {
    return false;
  }
  Wire1.beginTransmission(s_touch_address);
  Wire1.write(static_cast<uint8_t>(reg >> 8));
  Wire1.write(static_cast<uint8_t>(reg));
  Wire1.write(value);
  return Wire1.endTransmission() == 0;
}

bool detectTouchController() {
  uint8_t status = 0;
  for (uint8_t address : kGt911Addresses) {
    if (gt911ReadAt(address, kGt911PointInfo, &status, 1)) {
      s_touch_address = address;
      Serial.printf("CrowPanel: GT911 detected at 0x%02X\n", address);
      return true;
    }
  }
  s_touch_address = 0;
  return false;
}

void transformTouch(uint16_t raw_x, uint16_t raw_y, int16_t* x, int16_t* y) {
  int32_t tx = raw_x;
  int32_t ty = raw_y;
#if CROWPANEL_TOUCH_SWAP_XY
  std::swap(tx, ty);
#endif
#if CROWPANEL_TOUCH_INVERT_X
  tx = kWidth - 1 - tx;
#endif
#if CROWPANEL_TOUCH_INVERT_Y
  ty = kHeight - 1 - ty;
#endif
  *x = static_cast<int16_t>(std::max<int32_t>(0, std::min<int32_t>(kWidth - 1, tx)));
  *y = static_cast<int16_t>(std::max<int32_t>(0, std::min<int32_t>(kHeight - 1, ty)));
}

void pollTouch() {
  const uint32_t now = millis();
  if (now - s_last_touch_poll_ms < 20 || s_touch_address == 0) {
    return;
  }
  s_last_touch_poll_ms = now;

  uint8_t status = 0;
  if (!gt911ReadAt(s_touch_address, kGt911PointInfo, &status, 1)) {
    return;
  }
  if ((status & 0x80) == 0) {
    return;
  }

  const uint8_t point_count = status & 0x0F;
  s_touch_pressed = point_count > 0 && point_count <= 5;
  if (s_touch_pressed) {
    uint8_t point[7] = {};
    if (gt911ReadAt(s_touch_address, kGt911FirstPoint, point, sizeof(point))) {
      const uint16_t raw_x = point[1] | (static_cast<uint16_t>(point[2]) << 8);
      const uint16_t raw_y = point[3] | (static_cast<uint16_t>(point[4]) << 8);
      transformTouch(raw_x, raw_y, &s_touch_x, &s_touch_y);
    }
  }
  gt911WriteByte(kGt911PointInfo, 0);
}

#if CROWPANEL_SERIAL_DEBUG
// A "tap X Y" serial command is reported to LVGL as a short press.
constexpr int kDebugTapReads = 3;
volatile int s_debug_tap_reads = 0;
volatile int16_t s_debug_tap_x = 0;
volatile int16_t s_debug_tap_y = 0;
#endif

void readTouch(lv_indev_drv_t*, lv_indev_data_t* data) {
  pollTouch();
  data->state = s_touch_pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  data->point.x = s_touch_x;
  data->point.y = s_touch_y;
#if CROWPANEL_SERIAL_DEBUG
  if (s_debug_tap_reads > 0) {
    s_debug_tap_reads = s_debug_tap_reads - 1;
    data->state = LV_INDEV_STATE_PRESSED;
    data->point.x = s_debug_tap_x;
    data->point.y = s_debug_tap_y;
    // Lists pick the option under the release point, so the release that
    // follows has to be reported at the tap, not at the last real touch.
    s_touch_x = s_debug_tap_x;
    s_touch_y = s_debug_tap_y;
  }
#endif
}

#if CROWPANEL_SERIAL_DEBUG
// Scan-out health probe for the "lcd" serial command. The panel is fed by a
// DMA channel that streams the PSRAM frame buffer and is restarted on every
// vsync interrupt. If that stream runs dry, or the restart is serviced late,
// the rest of the frame is drawn in the wrong place on the glass. Neither is
// visible in a screenshot, so count both here.
constexpr uint32_t kClocksPerLine =
    kWidth + kHsyncFrontPorch + kHsyncPulseWidth + kHsyncBackPorch;
constexpr uint32_t kLinesPerFrame =
    kHeight + kVsyncFrontPorch + kVsyncPulseWidth + kVsyncBackPorch;
constexpr uint32_t framePeriodUs(uint32_t pixel_clock_hz, uint32_t clocks_per_line) {
  return static_cast<uint32_t>(static_cast<uint64_t>(clocks_per_line) *
                               kLinesPerFrame * 1000000ULL / pixel_clock_hz);
}
// The "pclk" command can change the clock and line length while running.
uint32_t s_pixel_clock_hz = kPixelClockHz;
uint32_t s_clocks_per_line = kClocksPerLine;
volatile uint32_t s_probe_frame_period_us =
    framePeriodUs(kPixelClockHz, kClocksPerLine);
constexpr int32_t kLateVsyncUs = 300;
int s_probe_dma_channel = -1;
volatile int64_t s_probe_last_us = 0;
volatile uint32_t s_probe_frames = 0;
volatile uint32_t s_probe_late_frames = 0;
volatile int32_t s_probe_worst_late_us = 0;
volatile uint32_t s_probe_underrun_frames = 0;
// Where the DMA stream stands when vsync ends. It is the same every frame
// while the stream keeps up; a frame that fell behind ends somewhere else.
uint32_t s_probe_position = 0;
uint32_t s_probe_position_run = 0;
uint32_t s_probe_synced_position = 0;
volatile uint32_t s_probe_desync_frames = 0;
volatile int32_t s_probe_worst_desync = 0;
// The stream's last byte leaves the DMA just before the last pixel is shown,
// which raises the channel's end-of-frame flag. A frame that reaches vsync
// without it missed pixels on the way, even if by less than a descriptor.
volatile uint32_t s_probe_short_frames = 0;
// When the most recent bad frames happened, for lining them up with what the
// firmware was doing.
enum : uint32_t { kEventDesync = 1, kEventShort = 2, kEventSlip = 3 };
struct ProbeEvent {
  int64_t at_us;
  uint32_t kind;
  int32_t value;
};
constexpr uint32_t kProbeEventSlots = 32;
ProbeEvent s_probe_events[kProbeEventSlots];
volatile uint32_t s_probe_event_count = 0;
// Longest single LVGL service call: how long a screen update took to reach
// the frame buffer, during which the panel shows part old and part new.
volatile uint32_t s_probe_longest_update_ms = 0;
volatile uint32_t s_probe_slow_updates = 0;
// Longest gap between LVGL service calls: how long touch and animation were
// frozen by something else on the main task (a fetch, a redraw).
volatile uint32_t s_probe_longest_stall_ms = 0;
uint32_t s_probe_last_service_ms = 0;

// Slip counter for the "slip" serial command. The other counters cannot see a
// frame whose stream fell only a few pixels behind, which is what a slip in
// normal running looks like. This one can: while it is on, the last 40 pixels
// of the bottom row are kept white, and two capture timers note when that
// white starts on a data line and when the row's data-enable ends. The time
// between the two is the same every frame unless pixels were lost on the way
// down, and it shrinks by one pixel period for each one lost.
constexpr int kSlipDataPin = 45;    // LCD data line 15.
constexpr int kSlipEnablePin = 41;  // LCD data enable.
constexpr int kSlipMarkerPixels = 40;
constexpr int kSlipLeadPixels = 8;  // Black before the marker, for a clean edge.
constexpr size_t kSlipMarkerBytes = (kSlipLeadPixels + kSlipMarkerPixels) * 2;
constexpr int kActivityKinds = 7;   // Six Activity bits, then "none of them".
volatile bool s_slip_armed = false;
uint8_t s_slip_saved[kSlipMarkerBytes];
int32_t s_slip_candidate = 0;
uint32_t s_slip_candidate_run = 0;
volatile int32_t s_slip_nominal = 0;  // Ticks from marker to line end, unslipped.
volatile uint32_t s_slip_frames = 0;
volatile uint32_t s_slip_slipped = 0;
volatile uint32_t s_slip_beyond = 0;   // Slipped by more than the marker shows.
volatile int32_t s_slip_worst = 0;
volatile uint32_t s_slip_frames_by[kActivityKinds] = {};
volatile uint32_t s_slip_slipped_by[kActivityKinds] = {};
// What the firmware has been doing during the frame being scanned.
volatile uint32_t s_activity_now = 0;
volatile uint32_t s_activity_frame = 0;

// The pixel clock as the LCD module's dividers make it, which is the nearest
// they can get to what was asked for.
uint32_t actualPixelClockHz() {
  constexpr uint64_t kSourceHz = 240000000;  // LovyanGFX selects the 240 MHz PLL.
  typeof(LCD_CAM.lcd_clock) clock;
  clock.val = LCD_CAM.lcd_clock.val;
  const uint64_t whole = clock.lcd_clkm_div_num;
  const uint64_t steps = clock.lcd_clkm_div_a;
  const uint64_t fraction = clock.lcd_clkm_div_b;
  const uint64_t second = clock.lcd_clk_equ_sysclk ? 1 : clock.lcd_clkcnt_n + 1;
  if (steps == 0) {
    return static_cast<uint32_t>(kSourceHz / whole / second);
  }
  return static_cast<uint32_t>(kSourceHz * steps / (whole * steps + fraction) / second);
}

void notePixelTiming(uint32_t clocks_per_line) {
  s_pixel_clock_hz = actualPixelClockHz();
  s_clocks_per_line = clocks_per_line;
  s_probe_frame_period_us = framePeriodUs(s_pixel_clock_hz, s_clocks_per_line);
  s_probe_last_us = 0;  // The frame under way is neither late nor early.
  Serial.printf("\n@@PCLK %u Hz, %u clocks per line, %u lines per second, "
                "%u refreshes per 100 s\n",
                static_cast<unsigned>(s_pixel_clock_hz),
                static_cast<unsigned>(s_clocks_per_line),
                static_cast<unsigned>(s_pixel_clock_hz / s_clocks_per_line),
                static_cast<unsigned>(100000000ULL / s_probe_frame_period_us));
}

void IRAM_ATTR noteProbeEvent(int64_t now, uint32_t kind, int32_t value) {
  ProbeEvent& event = s_probe_events[s_probe_event_count % kProbeEventSlots];
  event.at_us = now;
  event.kind = kind;
  event.value = value;
  s_probe_event_count = s_probe_event_count + 1;
}

void IRAM_ATTR scanoutProbeIsr(void*) {
  const int64_t now = esp_timer_get_time();
  if (s_probe_last_us != 0) {
    const int32_t late = static_cast<int32_t>(now - s_probe_last_us) -
                         static_cast<int32_t>(s_probe_frame_period_us);
    if (late > kLateVsyncUs) {
      s_probe_late_frames = s_probe_late_frames + 1;
    }
    if (late > s_probe_worst_late_us) {
      s_probe_worst_late_us = late;
    }
  }
  s_probe_last_us = now;
  s_probe_frames = s_probe_frames + 1;

  auto& out = GDMA.channel[s_probe_dma_channel].out;
  const uint32_t position = out.dscr;
  if (position == s_probe_position) {
    if (++s_probe_position_run >= 4) {
      s_probe_synced_position = position;
    }
  } else {
    s_probe_position = position;
    s_probe_position_run = 0;
  }
  if (s_probe_synced_position != 0 && position != s_probe_synced_position) {
    s_probe_desync_frames = s_probe_desync_frames + 1;
    const int32_t descriptors =
        static_cast<int32_t>(position - s_probe_synced_position) / 12;
    if (abs(descriptors) > abs(s_probe_worst_desync)) {
      s_probe_worst_desync = descriptors;
    }
    noteProbeEvent(now, kEventDesync, descriptors);
  }
  const bool reached_end = out.int_raw.out_eof;
  out.int_clr.out_eof = 1;
  if (!reached_end && s_probe_frames > 2) {  // The first frames start part-way.
    s_probe_short_frames = s_probe_short_frames + 1;
    noteProbeEvent(now, kEventShort, 0);
  }
  if (out.int_raw.outfifo_udf_l1 || out.int_raw.outfifo_udf_l3) {
    s_probe_underrun_frames = s_probe_underrun_frames + 1;
    out.int_clr.outfifo_udf_l1 = 1;
    out.int_clr.outfifo_udf_l3 = 1;
  }

  const uint32_t activity = s_activity_frame | s_activity_now;
  s_activity_frame = s_activity_now;
  if (!s_slip_armed) {
    return;
  }
  const int32_t span =
      static_cast<int32_t>(MCPWM0.cap_chn[1].val - MCPWM0.cap_chn[0].val);
  if (s_slip_nominal == 0) {
    // The unslipped value is whatever a run of frames agrees on.
    if (abs(span - s_slip_candidate) <= 1) {
      if (++s_slip_candidate_run >= 16) {
        s_slip_nominal = s_slip_candidate;
      }
    } else {
      s_slip_candidate = span;
      s_slip_candidate_run = 0;
    }
    return;
  }
  const int32_t nominal = s_slip_nominal;
  int32_t pixels = 0;
  if (span < 0 || span > nominal + nominal / kSlipMarkerPixels) {
    // The marker's edge never came out: the stream was further behind than
    // the marker is long.
    pixels = kSlipMarkerPixels;
    s_slip_beyond = s_slip_beyond + 1;
  } else {
    const int32_t scaled = (nominal - span) * kSlipMarkerPixels;
    pixels = (scaled + nominal / 2) / nominal;
  }
  s_slip_frames = s_slip_frames + 1;
  if (pixels != 0) {
    s_slip_slipped = s_slip_slipped + 1;
    if (pixels > s_slip_worst) {
      s_slip_worst = pixels;
    }
    noteProbeEvent(now, kEventSlip, pixels);
  }
  for (int kind = 0; kind < kActivityKinds; ++kind) {
    const bool applies = kind == kActivityKinds - 1 ? activity == 0
                                                    : ((activity >> kind) & 1) != 0;
    if (applies) {
      s_slip_frames_by[kind] = s_slip_frames_by[kind] + 1;
      if (pixels != 0) {
        s_slip_slipped_by[kind] = s_slip_slipped_by[kind] + 1;
      }
    }
  }
}

// Must run on the task that initialized the panel: a shared handler has to be
// attached on the same core, and attaching later puts it ahead of LovyanGFX's
// handler, which clears the interrupt status.
void startScanoutProbe() {
  for (int channel = 0; channel < SOC_GDMA_PAIRS_PER_GROUP; ++channel) {
    if (GDMA.channel[channel].out.peri_sel.sel == SOC_GDMA_TRIG_PERIPH_LCD0) {
      s_probe_dma_channel = channel;
    }
  }
  if (s_probe_dma_channel < 0) {
    Serial.println("CrowPanel: scan-out probe found no LCD DMA channel");
    return;
  }
  notePixelTiming(kClocksPerLine);
  const esp_err_t result = esp_intr_alloc_intrstatus(
      ETS_LCD_CAM_INTR_SOURCE, ESP_INTR_FLAG_SHARED,
      reinterpret_cast<uint32_t>(&LCD_CAM.lc_dma_int_st), LCD_LL_EVENT_VSYNC_END,
      scanoutProbeIsr, nullptr, nullptr);
  if (result != ESP_OK) {
    Serial.printf("CrowPanel: scan-out probe not attached (%d)\n", result);
  }
}

void sendScanoutStats() {
  // Read from another task than LVGL runs on; the figures are a snapshot.
  lv_mem_monitor_t lvgl_memory;
  lv_mem_monitor(&lvgl_memory);
  Serial.printf(
      "@@LCD frames=%u late=%u worst_late_us=%d underrun_frames=%u "
      "desync_frames=%u worst_desync_descriptors=%d short_frames=%u "
      "slow_updates=%u "
      "longest_update_ms=%u longest_stall_ms=%u heap_free=%u heap_low=%u "
      "loop_stack_free=%u lvgl_used=%u lvgl_pool=%u\n",
      static_cast<unsigned>(s_probe_frames),
      static_cast<unsigned>(s_probe_late_frames),
      static_cast<int>(s_probe_worst_late_us),
      static_cast<unsigned>(s_probe_underrun_frames),
      static_cast<unsigned>(s_probe_desync_frames),
      static_cast<int>(s_probe_worst_desync),
      static_cast<unsigned>(s_probe_short_frames),
      static_cast<unsigned>(s_probe_slow_updates),
      static_cast<unsigned>(s_probe_longest_update_ms),
      static_cast<unsigned>(s_probe_longest_stall_ms),
      static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
      static_cast<unsigned>(uxTaskGetStackHighWaterMark(s_ui_task)),
      static_cast<unsigned>(lvgl_memory.total_size - lvgl_memory.free_size),
      static_cast<unsigned>(lvgl_memory.total_size));
  s_probe_frames = 0;
  s_probe_late_frames = 0;
  s_probe_worst_late_us = 0;
  s_probe_underrun_frames = 0;
  s_probe_desync_frames = 0;
  s_probe_worst_desync = 0;
  s_probe_short_frames = 0;
  s_probe_slow_updates = 0;
  s_probe_longest_update_ms = 0;
  s_probe_longest_stall_ms = 0;
}

constexpr size_t kFrameBytes = static_cast<size_t>(kWidth) * kHeight * 2;

// Sends pixels as "@@D<base64>" lines holding run-length records of
// {count:u8, RGB565:u16 little-endian}. Each line goes out in a single
// Serial.write, which is atomic, so log lines from other tasks land between
// data lines rather than inside them.
class PixelSender {
 public:
  void add(uint16_t pixel) {
    if (run_length_ > 0 && (pixel != run_pixel_ || run_length_ == 255)) {
      endRun();
    }
    run_pixel_ = pixel;
    ++run_length_;
  }

  /** Ends the current run and line; what is added next starts a new one. */
  void finish() {
    if (run_length_ > 0) {
      endRun();
    }
    sendLine();
  }

  /** FNV-1a of every record sent so far. */
  uint32_t hash() const { return hash_; }

 private:
  static constexpr size_t kRecordsPerLine = 48;

  void endRun() {
    const uint8_t record[3] = {run_length_, static_cast<uint8_t>(run_pixel_),
                               static_cast<uint8_t>(run_pixel_ >> 8)};
    for (uint8_t value : record) {
      hash_ = (hash_ ^ value) * 16777619UL;
      records_[used_++] = value;
    }
    run_length_ = 0;
    if (used_ == sizeof(records_)) {
      sendLine();
    }
  }

  void sendLine() {
    if (used_ == 0) {
      return;
    }
    static const char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[3 + kRecordsPerLine * 4 + 1];
    size_t length = 0;
    line[length++] = '@';
    line[length++] = '@';
    line[length++] = 'D';
    for (size_t i = 0; i < used_; i += 3) {
      const uint32_t value = static_cast<uint32_t>(records_[i]) << 16 |
                             static_cast<uint32_t>(records_[i + 1]) << 8 |
                             records_[i + 2];
      line[length++] = kBase64[value >> 18];
      line[length++] = kBase64[(value >> 12) & 63];
      line[length++] = kBase64[(value >> 6) & 63];
      line[length++] = kBase64[value & 63];
    }
    line[length++] = '\n';
    Serial.write(reinterpret_cast<const uint8_t*>(line), length);
    used_ = 0;
  }

  uint32_t hash_ = 2166136261UL;
  uint8_t records_[kRecordsPerLine * 3];
  size_t used_ = 0;
  uint16_t run_pixel_ = 0;
  uint8_t run_length_ = 0;
};

// Debug aids for scripts/crowpanel_screenshot.py and crowpanel_record.py,
// driven by lines on the serial port. "tap X Y" presses the screen at that
// point. "shot" is answered with "@@SHOT <w> <h>", the frame as @@D lines
// (see PixelSender), then "@@END <FNV-1a of the records, hex>". The pixels are
// the scan-out buffer itself, so the capture is what the panel is being
// driven with.
void sendScreenshot() {
  const uint8_t* live = s_lcd.frameBuffer();
  uint8_t* frame = static_cast<uint8_t*>(
      heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (live == nullptr || frame == nullptr) {
    heap_caps_free(frame);
    Serial.println("\n@@SHOT-ERROR");
    return;
  }
  memcpy(frame, live, kFrameBytes);  // One consistent frame while LVGL keeps drawing.

  Serial.printf("\n@@SHOT %d %d\n", kWidth, kHeight);
  PixelSender sender;
  for (size_t offset = 0; offset < kFrameBytes; offset += 2) {
    sender.add(static_cast<uint16_t>(frame[offset] << 8) | frame[offset + 1]);
  }
  sender.finish();
  Serial.printf("@@END %08lx\n", static_cast<unsigned long>(sender.hash()));
  heap_caps_free(frame);
}

// Panel write recorder for the "rec SECONDS" command. While it runs, every
// write to the frame buffer is copied to PSRAM with when it happened and
// where the panel's scan was at the time, so the script can replay what the
// panel was given, frame by frame. The dump is
//   @@REC <w> <h> <writes> <microseconds> <pixel clock> <clocks per line>
//         <lines per frame> <scan position at vsync>
// then the frame at the start as @@D lines, then for each write
//   @@W <start us> <end us> <vsync us> <scan position at start> <at end>
//       <x> <y> <w> <h> <kind>
// and its pixels, "@@A <us> <kind> <value>" for each bad frame the scan-out
// probe saw meanwhile, and "@@END <hash>". A scan position is the address of
// a DMA descriptor: they are 12 bytes apart and each covers 4032 bytes of
// frame buffer.
enum class Recording : uint8_t { Idle, Armed, Running, Done };
volatile Recording s_recording = Recording::Idle;
uint8_t* s_record_arena = nullptr;  // The frame at the start, then the writes.
size_t s_record_capacity = 0;
size_t s_record_used = 0;
uint32_t s_record_writes = 0;
uint32_t s_record_duration_ms = 0;
int64_t s_record_start_us = 0;
int64_t s_record_end_us = 0;
int64_t s_record_stopped_us = 0;

struct RecordedWrite {  // Followed by width * height native RGB565 pixels.
  uint32_t start_us;    // Times count from the start of the recording.
  uint32_t end_us;
  int32_t vsync_us;     // The vsync before the write; may precede the recording.
  uint32_t scan_start;
  uint32_t scan_end;
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint16_t height;
  uint32_t kind;
};

uint32_t scanPosition() {
  return s_probe_dma_channel < 0 ? 0 : GDMA.channel[s_probe_dma_channel].out.dscr;
}

void stopRecording() {
  s_record_stopped_us = esp_timer_get_time();
  s_recording = Recording::Done;
}

WriteStamp beginRecordedWrite() {
  if (s_recording != Recording::Running) {
    return {};
  }
  int64_t vsync = s_probe_last_us;
  while (vsync != s_probe_last_us) {  // The vsync interrupt updates it.
    vsync = s_probe_last_us;
  }
  return {esp_timer_get_time(), vsync, scanPosition()};
}

void endRecordedWrite(const WriteStamp& stamp, uint32_t kind, int x, int y,
                      int width, int height, const uint16_t* pixels, int stride) {
  if (s_recording != Recording::Running || stamp.started_us == 0) {
    return;
  }
  const int64_t ended = esp_timer_get_time();
  const uint32_t scan_end = scanPosition();
  const size_t row_bytes = static_cast<size_t>(width) * sizeof(uint16_t);
  const size_t bytes = sizeof(RecordedWrite) + row_bytes * height;
  if (ended >= s_record_end_us || s_record_used + bytes > s_record_capacity) {
    stopRecording();
    return;
  }
  const RecordedWrite header = {
      static_cast<uint32_t>(stamp.started_us - s_record_start_us),
      static_cast<uint32_t>(ended - s_record_start_us),
      static_cast<int32_t>(stamp.vsync_us - s_record_start_us),
      stamp.scan_position,
      scan_end,
      static_cast<uint16_t>(x),
      static_cast<uint16_t>(y),
      static_cast<uint16_t>(width),
      static_cast<uint16_t>(height),
      kind};
  uint8_t* out = s_record_arena + s_record_used;
  memcpy(out, &header, sizeof(header));
  out += sizeof(header);
  for (int row = 0; row < height; ++row) {
    memcpy(out, pixels + row * stride, row_bytes);
    out += row_bytes;
  }
  s_record_used += bytes;
  s_record_writes = s_record_writes + 1;
}

// Runs on the task that draws, so the frame taken at the start and the
// writes that follow it are consistent.
void serviceRecording() {
  if (s_recording == Recording::Armed) {
    memcpy(s_record_arena, s_lcd.frameBuffer(), kFrameBytes);
    s_record_used = kFrameBytes;
    s_record_writes = 0;
    s_record_start_us = esp_timer_get_time();
    s_record_end_us =
        s_record_start_us + static_cast<int64_t>(s_record_duration_ms) * 1000;
    s_recording = Recording::Running;
  } else if (s_recording == Recording::Running &&
             esp_timer_get_time() >= s_record_end_us) {
    stopRecording();
  }
}

void startRecording(uint32_t seconds) {
  if (s_recording != Recording::Idle) {
    return;
  }
  // Leave PSRAM for the aircraft feed's JSON document while this runs.
  constexpr size_t kReserveBytes = 1536 * 1024;
  constexpr size_t kMostWriteBytes = 3 * 1024 * 1024;
  const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  if (largest >= kFrameBytes + kReserveBytes + 256 * 1024) {
    s_record_capacity =
        std::min(largest - kReserveBytes, kFrameBytes + kMostWriteBytes);
    s_record_arena = static_cast<uint8_t*>(
        heap_caps_malloc(s_record_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (s_record_arena == nullptr) {
    Serial.println("\n@@REC-ERROR");
    return;
  }
  s_record_duration_ms = seconds * 1000;
  s_recording = Recording::Armed;
}

void sendRecording() {
  Serial.printf(
      "\n@@REC %d %d %u %u %u %u %u %u\n", kWidth, kHeight,
      static_cast<unsigned>(s_record_writes),
      static_cast<unsigned>(s_record_stopped_us - s_record_start_us),
      static_cast<unsigned>(s_pixel_clock_hz),
      static_cast<unsigned>(s_clocks_per_line),
      static_cast<unsigned>(kLinesPerFrame),
      static_cast<unsigned>(s_probe_synced_position));
  PixelSender sender;
  const uint8_t* cursor = s_record_arena;
  for (size_t offset = 0; offset < kFrameBytes; offset += 2) {
    sender.add(static_cast<uint16_t>(cursor[offset] << 8) | cursor[offset + 1]);
  }
  sender.finish();
  cursor += kFrameBytes;
  for (uint32_t i = 0; i < s_record_writes; ++i) {
    RecordedWrite write;
    memcpy(&write, cursor, sizeof(write));
    cursor += sizeof(write);
    Serial.printf("@@W %u %u %d %u %u %u %u %u %u %u\n",
                  static_cast<unsigned>(write.start_us),
                  static_cast<unsigned>(write.end_us),
                  static_cast<int>(write.vsync_us),
                  static_cast<unsigned>(write.scan_start),
                  static_cast<unsigned>(write.scan_end),
                  static_cast<unsigned>(write.x), static_cast<unsigned>(write.y),
                  static_cast<unsigned>(write.width),
                  static_cast<unsigned>(write.height),
                  static_cast<unsigned>(write.kind));
    const size_t pixels = static_cast<size_t>(write.width) * write.height;
    for (size_t index = 0; index < pixels; ++index) {
      uint16_t pixel;
      memcpy(&pixel, cursor + index * sizeof(pixel), sizeof(pixel));
      sender.add(pixel);
    }
    sender.finish();
    cursor += pixels * sizeof(uint16_t);
  }
  const uint32_t events = s_probe_event_count;
  for (uint32_t i = events > kProbeEventSlots ? events - kProbeEventSlots : 0;
       i < events; ++i) {
    const ProbeEvent event = s_probe_events[i % kProbeEventSlots];
    if (event.at_us >= s_record_start_us && event.at_us <= s_record_stopped_us) {
      Serial.printf("@@A %u %u %d\n",
                    static_cast<unsigned>(event.at_us - s_record_start_us),
                    static_cast<unsigned>(event.kind),
                    static_cast<int>(event.value));
    }
  }
  Serial.printf("@@END %08lx\n", static_cast<unsigned long>(sender.hash()));
  s_recording = Recording::Idle;
  heap_caps_free(s_record_arena);
  s_record_arena = nullptr;
}

// Bus stress for the "stress f|p|r SECONDS" command. Flash, PSRAM and the
// panel's DMA stream share one SPI bus, and the stream cannot wait: if the bus
// is busy a few microseconds too long, the rest of that frame is drawn shifted
// sideways. 'f' reads the firmware's constant data from flash end to end, the
// heaviest flash traffic the CPU can cause; 'p' copies PSRAM to PSRAM; 'r' is
// 'f' while the whole screen is also redrawn continuously. The "lcd" counters
// show what got through.
extern "C" {
extern const uint8_t _rodata_start[];
extern const uint8_t _rodata_end[];
}
volatile uint32_t s_stress_sink = 0;
volatile bool s_stress_redraw = false;

void runBusStress(char kind, uint32_t seconds) {
  constexpr size_t kChunk = 128 * 1024;
  uint8_t* scratch = nullptr;
  if (kind == 'p') {
    scratch = static_cast<uint8_t*>(
        heap_caps_malloc(2 * kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (scratch == nullptr) {
      Serial.println("\n@@STRESS-ERROR");
      return;
    }
  }
  s_stress_redraw = kind == 'r';
  const size_t flash_bytes = static_cast<size_t>(_rodata_end - _rodata_start);
  size_t offset = 0;
  uint32_t chunks = 0;
  const int64_t end_us =
      esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000000;
  while (esp_timer_get_time() < end_us) {
    if (kind == 'p') {
      memcpy(scratch + kChunk, scratch, kChunk);
      memcpy(scratch, scratch + kChunk, kChunk);
    } else {
      uint32_t sum = 0;
      const size_t length = std::min(kChunk, flash_bytes - offset);
      for (size_t i = 0; i < length; i += 32) {  // One read per cache line.
        sum += _rodata_start[offset + i];
      }
      s_stress_sink = s_stress_sink + sum;
      offset = offset + length >= flash_bytes ? 0 : offset + length;
    }
    ++chunks;
    vTaskDelay(1);  // Lets the idle task run; the bus stays busy all but 1 ms.
  }
  s_stress_redraw = false;
  heap_caps_free(scratch);
  Serial.printf("\n@@STRESS %c %u chunks\n", kind, static_cast<unsigned>(chunks));
}

// "pclk KHZ [FRONT_PORCH]" changes the pixel clock, and optionally the idle
// clocks after each line, while running: a way to find what a panel and its
// wiring need without reflashing for each try. The nearest clock the divider
// can make is used and reported, with the line and refresh rates that result.
// A reset returns to the compiled timing.
void setPixelTiming(uint32_t khz, uint32_t front_porch) {
  // LovyanGFX runs the LCD module from the 240 MHz PLL. Keep its whole-number
  // second divider and set the first, fractional one: 240 MHz / (n + b/a).
  constexpr uint32_t kSourceHz = 240000000;
  constexpr uint32_t kFractionSteps = 63;
  const uint32_t second = LCD_CAM.lcd_clock.lcd_clkcnt_n + 1;
  const uint64_t wanted_hz = static_cast<uint64_t>(khz) * 1000 * second;
  const uint64_t scaled =
      (static_cast<uint64_t>(kSourceHz) * kFractionSteps + wanted_hz / 2) / wanted_hz;
  const uint32_t whole = std::max<uint32_t>(2, scaled / kFractionSteps);
  const uint32_t fraction = whole * kFractionSteps > scaled ? 0 : scaled % kFractionSteps;
  // Peripheral registers are written a whole word at a time.
  typeof(LCD_CAM.lcd_clock) clock;
  clock.val = LCD_CAM.lcd_clock.val;
  clock.lcd_clkm_div_num = whole;
  clock.lcd_clkm_div_a = fraction == 0 ? 0 : kFractionSteps;
  clock.lcd_clkm_div_b = fraction;
  LCD_CAM.lcd_clock.val = clock.val;
  typeof(LCD_CAM.lcd_ctrl1) line;
  line.val = LCD_CAM.lcd_ctrl1.val;
  line.lcd_ht_width = kHsyncPulseWidth + kHsyncBackPorch + kWidth + front_porch - 1;
  LCD_CAM.lcd_ctrl1.val = line.val;
  LCD_CAM.lcd_user.lcd_update = 1;
  notePixelTiming(kHsyncPulseWidth + kHsyncBackPorch + kWidth + front_porch);
}

uint8_t* slipMarkerBytes() {
  const size_t first_pixel = static_cast<size_t>(kHeight) * kWidth -
                             (kSlipLeadPixels + kSlipMarkerPixels);
  return const_cast<uint8_t*>(s_lcd.frameBuffer()) + first_pixel * 2;
}

// Nothing else draws there, but redo it often in case something does.
void drawSlipMarker() {
  uint8_t* marker = slipMarkerBytes();
  memset(marker, 0x00, kSlipLeadPixels * 2);
  memset(marker + kSlipLeadPixels * 2, 0xFF, kSlipMarkerPixels * 2);
  Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(marker), kSlipMarkerBytes);
}

void restoreSlipMarker(int x2, int y2) {
  if (s_slip_armed && y2 >= kHeight - 1 &&
      x2 >= kWidth - (kSlipLeadPixels + kSlipMarkerPixels)) {
    drawSlipMarker();
  }
}

void resetSlipCounts() {
  s_slip_frames = 0;
  s_slip_slipped = 0;
  s_slip_beyond = 0;
  s_slip_worst = 0;
  for (int kind = 0; kind < kActivityKinds; ++kind) {
    s_slip_frames_by[kind] = 0;
    s_slip_slipped_by[kind] = 0;
  }
}

void setSlipCounter(bool on) {
  if (on == s_slip_armed) {
    resetSlipCounts();
    return;
  }
  if (!on) {
    s_slip_armed = false;
    uint8_t* marker = slipMarkerBytes();
    memcpy(marker, s_slip_saved, kSlipMarkerBytes);
    Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(marker), kSlipMarkerBytes);
    return;
  }
  // The capture timers watch the pins the LCD is already driving; nothing
  // about how those pins are driven changes.
  periph_module_enable(PERIPH_PWM0_MODULE);
  MCPWM0.cap_timer_cfg.val = 1;         // Free-running capture timer.
  MCPWM0.cap_chn_cfg[0].val = 1 | 1 << 2;  // Rising edge of the data line.
  MCPWM0.cap_chn_cfg[1].val = 1 | 1 << 1;  // Falling edge of data enable.
  PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[kSlipDataPin]);
  PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[kSlipEnablePin]);
  esp_rom_gpio_connect_in_signal(kSlipDataPin, PWM0_CAP0_IN_IDX, false);
  esp_rom_gpio_connect_in_signal(kSlipEnablePin, PWM0_CAP1_IN_IDX, false);

  memcpy(s_slip_saved, slipMarkerBytes(), kSlipMarkerBytes);
  drawSlipMarker();
  s_slip_nominal = 0;
  s_slip_candidate_run = 0;
  resetSlipCounts();
  s_slip_armed = true;
}

void sendSlipCounts() {
  static const char* const kNames[kActivityKinds] = {
      "fetch", "network_wait", "data_redraw", "glide",
      "lvgl", "tiles", "none"};
  Serial.printf("\n@@SLIP frames=%u slipped=%u worst_px=%d beyond_marker=%u "
                "ticks_per_marker=%d\n",
                static_cast<unsigned>(s_slip_frames),
                static_cast<unsigned>(s_slip_slipped),
                static_cast<int>(s_slip_worst),
                static_cast<unsigned>(s_slip_beyond),
                static_cast<int>(s_slip_nominal));
  for (int kind = 0; kind < kActivityKinds; ++kind) {
    Serial.printf("@@SLIP-BY %s frames=%u slipped=%u\n", kNames[kind],
                  static_cast<unsigned>(s_slip_frames_by[kind]),
                  static_cast<unsigned>(s_slip_slipped_by[kind]));
  }
  resetSlipCounts();
}

void serialDebugTask(void*) {
  char line[24];
  size_t length = 0;
  for (;;) {
    while (Serial.available() > 0) {
      const char c = static_cast<char>(Serial.read());
      if (c != '\n' && c != '\r') {
        if (length < sizeof(line) - 1) {
          line[length++] = c;
        }
        continue;
      }
      line[length] = '\0';
      length = 0;
      int x = 0;
      int y = 0;
      char kind = 0;
      if (strcmp(line, "shot") == 0) {
        sendScreenshot();
      } else if (strcmp(line, "lcd") == 0) {
        sendScanoutStats();
      } else if (strcmp(line, "slip on") == 0) {
        setSlipCounter(true);
      } else if (strcmp(line, "slip off") == 0) {
        setSlipCounter(false);
      } else if (strcmp(line, "slip") == 0) {
        sendSlipCounts();
      } else if (sscanf(line, "rec %d", &x) == 1 && x >= 1 && x <= 60) {
        startRecording(static_cast<uint32_t>(x));
      } else if (sscanf(line, "stress %c %d", &kind, &x) == 2 &&
                 (kind == 'f' || kind == 'p' || kind == 'r') && x >= 1 && x <= 60) {
        runBusStress(kind, static_cast<uint32_t>(x));
      } else if (const int fields = sscanf(line, "pclk %d %d", &x, &y);
                 fields >= 1 && x >= 10100 && x <= 15000 &&
                 (fields == 1 || (y >= 1 && y <= 255))) {
        setPixelTiming(static_cast<uint32_t>(x),
                       fields == 2 ? static_cast<uint32_t>(y) : kHsyncFrontPorch);
      } else if (sscanf(line, "tap %d %d", &x, &y) == 2 && x >= 0 &&
                 x < kWidth && y >= 0 && y < kHeight) {
        s_debug_tap_x = static_cast<int16_t>(x);
        s_debug_tap_y = static_cast<int16_t>(y);
        s_debug_tap_reads = kDebugTapReads;
      }
    }
    if (s_recording == Recording::Done) {
      sendRecording();
    }
    if (s_slip_armed) {
      drawSlipMarker();
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}
#endif

}  // namespace

bool init() {
  s_ready = false;
  s_ui_task = xTaskGetCurrentTaskHandle();
  setBrightness(0);

  if (!psramFound()) {
    setError("PSRAM not detected (CrowPanel must use OPI PSRAM)");
    return false;
  }
  Serial.printf("CrowPanel: PSRAM %u bytes\n", static_cast<unsigned>(ESP.getPsramSize()));

  Wire1.begin(kTouchSda, kTouchScl);
  Wire1.setClock(kTouchClockHz);

  if (!s_lcd.init()) {
    setError("RGB panel initialization failed");
    return false;
  }
  s_lcd.setColorDepth(16);
  s_lcd.setSwapBytes(false);
  s_lcd.fillScreen(TFT_BLACK);
#if CROWPANEL_SERIAL_DEBUG
  startScanoutProbe();
#endif

  // Recreate the I2C bus after RGB initialization, matching Elecrow's V3 sequence.
  Wire1.end();
  delay(10);
  Wire1.begin(kTouchSda, kTouchScl);
  Wire1.setClock(kTouchClockHz);
  resetTouchController();
  if (!detectTouchController()) {
    Serial.println("CrowPanel: GT911 not detected; display will run without touch");
  }

  lv_init();
  s_buffer_a = allocateDrawBuffer();
  s_buffer_b = allocateDrawBuffer();
  if (s_buffer_a == nullptr) {
    setError("LVGL draw-buffer allocation failed");
    return false;
  }
  lv_disp_draw_buf_init(&s_draw_buf, s_buffer_a, s_buffer_b, kDrawBufferPixels);

  lv_disp_drv_init(&s_display_driver);
  s_display_driver.hor_res = kWidth;
  s_display_driver.ver_res = kHeight;
  s_display_driver.draw_buf = &s_draw_buf;
  s_display_driver.flush_cb = flushDisplay;
  lv_disp_drv_register(&s_display_driver);

  lv_indev_drv_init(&s_touch_driver);
  s_touch_driver.type = LV_INDEV_TYPE_POINTER;
  s_touch_driver.read_cb = readTouch;
  lv_indev_drv_register(&s_touch_driver);

  ledcSetup(kBacklightChannel, kBacklightFrequencyHz, kBacklightResolutionBits);
  ledcAttachPin(kBacklightPin, kBacklightChannel);
  s_last_lvgl_ms = millis();
  s_ready = true;
  strncpy(s_error, "none", sizeof(s_error));
  setBrightness(75);
  Serial.println("CrowPanel: display ready");
#if CROWPANEL_SERIAL_DEBUG
  xTaskCreate(serialDebugTask, "serial-debug", 4096, nullptr, 1, nullptr);
#endif
  return true;
}

void loop() {
  if (!s_ready) {
    return;
  }
  const uint32_t now = millis();
  const uint32_t elapsed = now - s_last_lvgl_ms;
  if (elapsed > 0) {
    lv_tick_inc(elapsed);
    s_last_lvgl_ms = now;
  }
#if CROWPANEL_SERIAL_DEBUG
  serviceRecording();
  if (s_stress_redraw) {
    lv_obj_invalidate(lv_scr_act());
  }
  const uint32_t started = millis();
  if (s_probe_last_service_ms != 0 &&
      started - s_probe_last_service_ms > s_probe_longest_stall_ms) {
    s_probe_longest_stall_ms = started - s_probe_last_service_ms;
  }
  lv_timer_handler();
  s_probe_last_service_ms = millis();
  const uint32_t took = s_probe_last_service_ms - started;
  if (took > s_probe_longest_update_ms) {
    s_probe_longest_update_ms = took;
  }
  if (took > 33) {  // Long enough to be seen as a pause.
    s_probe_slow_updates = s_probe_slow_updates + 1;
  }
#else
  lv_timer_handler();
#endif
}

bool ready() { return s_ready; }

void beginActivity(uint32_t activity) {
#if CROWPANEL_SERIAL_DEBUG
  s_activity_now = s_activity_now | activity;
#else
  (void)activity;
#endif
}

void endActivity(uint32_t activity) {
#if CROWPANEL_SERIAL_DEBUG
  // The frame under way still counts it; the vsync interrupt clears that.
  s_activity_frame = s_activity_frame | activity;
  s_activity_now = s_activity_now & ~activity;
#else
  (void)activity;
#endif
}

void blit(int x, int y, int width, int height, const uint16_t* pixels, int stride) {
  if (!s_ready || width <= 0 || height <= 0) {
    return;
  }
#if CROWPANEL_SERIAL_DEBUG
  const WriteStamp stamp = beginRecordedWrite();
#endif
  s_lcd.startWrite();
  for (int row = 0; row < height; ++row) {
    s_lcd.pushImage(x, y + row, width, 1,
                    reinterpret_cast<const lgfx::rgb565_t*>(pixels + row * stride));
  }
  s_lcd.endWrite();
#if CROWPANEL_SERIAL_DEBUG
  endRecordedWrite(stamp, kWriteRadarTiles, x, y, width, height, pixels, stride);
  endActivity(kActivityTiles);
#endif
}

const char* lastError() { return s_error; }

void setBrightness(uint8_t percent) {
  s_brightness = std::min<uint8_t>(percent, 100);
  const uint32_t duty = static_cast<uint32_t>(s_brightness) * 255U / 100U;
  if (s_ready) {
    ledcWrite(kBacklightChannel, duty);
  } else {
    pinMode(kBacklightPin, OUTPUT);
    digitalWrite(kBacklightPin, duty > 0 ? HIGH : LOW);
  }
}

uint8_t brightness() { return s_brightness; }

uint8_t touchAddress() { return s_touch_address; }

}  // namespace crowpanel::display
