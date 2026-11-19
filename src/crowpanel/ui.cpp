#include "crowpanel/ui.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <initializer_list>
#include <iterator>

#include "crowpanel/display.h"
#include "crowpanel/geo.h"
#include "crowpanel/logger.h"
#include "crowpanel/model.h"
#include "crowpanel/phone_location.h"
#include "crowpanel/settings.h"
#include "data/airline_names.h"
#include "crowpanel/airport_catalog.h"
#include "crowpanel/airports.h"
#include "crowpanel/adsb_client.h"
#include "services/radar_location.h"
#include "services/route_lookup.h"
#include "services/wifi_setup.h"
#include "ui/radar_range.h"

namespace crowpanel::ui {
namespace {

constexpr int kRadarSize = 480;
constexpr int kRadarCenter = 240;
constexpr int kRadarRadius = 228;
constexpr int kRimRadius = 235;  // Where traffic beyond the outer ring is marked.
constexpr int kRangeRings = 5;
constexpr int kSidebarX = 480;
constexpr int kSidebarWidth = 320;
constexpr size_t kTrafficRows = 5;
constexpr size_t kAirportPickerRows = 5;
// Dense metro areas can contain hundreds of small and private fields inside a
// 50 km view. Keep every rendered marker touch-selectable in typical U.S. use.
constexpr size_t kMaxAirportHits = 384;
constexpr size_t kMaxRadarLabelBoxes = 96;

constexpr uint32_t kColorScreen = 0x050B18;
constexpr uint32_t kColorPanel = 0x0B172A;
constexpr uint32_t kColorPanelAlt = 0x101F35;
constexpr uint32_t kColorBorder = 0x213A55;
constexpr uint32_t kColorText = 0xF1F6FA;
// Brighter than it looks necessary: the panel is a TN type and the dark end of
// the range washes out off-axis.
constexpr uint32_t kColorMuted = 0xA9C0D0;
constexpr uint32_t kColorAccent = 0x29D3C2;
constexpr uint32_t kColorWarning = 0xF0A52B;
constexpr uint32_t kColorDanger = 0xEF4756;

struct ThemeColors {
  lv_color_t background;
  lv_color_t grid;
  lv_color_t aircraft;
  lv_color_t vector;
  lv_color_t trail;
  lv_color_t runway;
  lv_color_t airport;
  lv_color_t label;
  lv_color_t selected;
};

struct AircraftHit {
  int16_t x;
  int16_t y;
  char hex[7];
};

struct AirportHit {
  int16_t x;
  int16_t y;
  uint16_t airport_index;
};

struct AirportRender {
  AirportHit hit;
  float distance_km;
};

struct LabelBox {
  int16_t left;
  int16_t top;
  int16_t right;
  int16_t bottom;
};

enum class AircraftSymbolKind {
  FixedWing,
  Helicopter,
  Glider,
  Balloon,
  Drone,
  Ground,
};

lv_obj_t* s_main_screen = nullptr;
lv_obj_t* s_status_screen = nullptr;
lv_obj_t* s_status_title = nullptr;
lv_obj_t* s_status_detail = nullptr;
lv_obj_t* s_canvas = nullptr;
lv_color_t* s_canvas_buffer = nullptr;
// The radar is painted in two layers so that only what moves is repainted.
// s_backdrop holds the grid, airports and runways and is rebuilt only when the
// view changes. s_traffic paints aircraft straight into the visible canvas's
// pixels. Both are hidden canvases: drawing on s_canvas itself would mark the
// whole radar for redraw on every primitive.
lv_obj_t* s_backdrop = nullptr;
lv_color_t* s_backdrop_buffer = nullptr;
lv_obj_t* s_traffic = nullptr;
lv_obj_t* s_target = nullptr;  // Canvas the draw helpers paint on.
// Traffic pixels are tracked in tiles, one bit each, so a frame only puts the
// backdrop back, and only sends to the panel, where aircraft were or are.
constexpr int kTileSize = 60;
constexpr int kTilesPerSide = kRadarSize / kTileSize;
static_assert(kTilesPerSide * kTileSize == kRadarSize && kTilesPerSide == 8,
              "tile rows are handled as one byte of a 64-bit mask");
static_assert(sizeof(lv_color_t) == sizeof(uint16_t),
              "canvas pixels are handed to the panel as RGB565");
uint64_t s_traffic_tiles = 0;
// The aircraft on the card and since when, to decide when its route is worth
// a request.
char s_card_hex[7] = {};
uint32_t s_card_since_ms = 0;
constexpr uint32_t kRouteDwellMs = 3000;
// Data older than this turns the status strip into a warning.
constexpr unsigned long kStaleDataSeconds = 15;
constexpr int kWeakSignalDbm = -78;
int s_status_rssi = 0;  // Signal strength when data last arrived.
uint32_t s_traffic_signature = 0;
uint32_t s_traffic_cost_ms = 0;  // How long the last traffic repaint took.
// Status strip: how much is being tracked, or what is wrong.
lv_obj_t* s_status_strip = nullptr;
lv_obj_t* s_status_left = nullptr;
lv_obj_t* s_status_right = nullptr;
// The card: one aircraft (or airport) in type that reads from across a room.
// It describes the pinned aircraft, or failing that the nearest one flying.
lv_obj_t* s_card = nullptr;
lv_obj_t* s_card_caption = nullptr;
lv_obj_t* s_card_badge = nullptr;
lv_obj_t* s_card_title = nullptr;
lv_obj_t* s_card_type = nullptr;
lv_obj_t* s_card_subtitle = nullptr;
lv_obj_t* s_card_route = nullptr;
lv_obj_t* s_card_hint = nullptr;
lv_obj_t* s_card_altitude = nullptr;
lv_obj_t* s_card_speed = nullptr;
lv_obj_t* s_card_position = nullptr;
lv_obj_t* s_card_approach = nullptr;
lv_obj_t* s_range_label = nullptr;
lv_obj_t* s_traffic_buttons[kTrafficRows] = {};
lv_obj_t* s_traffic_bars[kTrafficRows] = {};
lv_obj_t* s_traffic_names[kTrafficRows] = {};
lv_obj_t* s_traffic_types[kTrafficRows] = {};
lv_obj_t* s_traffic_altitudes[kTrafficRows] = {};
lv_obj_t* s_traffic_distances[kTrafficRows] = {};
lv_obj_t* s_traffic_more = nullptr;
char s_traffic_hex[kTrafficRows][7] = {};

lv_obj_t* s_settings_overlay = nullptr;
// An opaque sheet behind whichever overlay is open. The overlays are smaller
// than the screen, and without it the radar's edges show round them.
lv_obj_t* s_overlay_scrim = nullptr;
lv_obj_t* s_brightness_slider = nullptr;
lv_obj_t* s_brightness_value = nullptr;
lv_obj_t* s_proximity_slider = nullptr;
lv_obj_t* s_proximity_value = nullptr;
lv_obj_t* s_labels_switch = nullptr;
lv_obj_t* s_trails_switch = nullptr;
lv_obj_t* s_runways_switch = nullptr;
lv_obj_t* s_ground_switch = nullptr;
lv_obj_t* s_logging_switch = nullptr;
lv_obj_t* s_alerts_switch = nullptr;
lv_obj_t* s_units_switch = nullptr;
lv_obj_t* s_altitude_dropdown = nullptr;
lv_obj_t* s_refresh_dropdown = nullptr;
lv_obj_t* s_theme_dropdown = nullptr;
lv_obj_t* s_sd_status = nullptr;
lv_obj_t* s_home_airport_value = nullptr;

lv_obj_t* s_airport_overlay = nullptr;
lv_obj_t* s_airport_picker_title = nullptr;
lv_obj_t* s_airport_picker_heading = nullptr;
lv_obj_t* s_airport_search = nullptr;
lv_obj_t* s_airport_keyboard = nullptr;
lv_obj_t* s_airport_result_buttons[kAirportPickerRows] = {};
lv_obj_t* s_airport_result_labels[kAirportPickerRows] = {};
data::airport_catalog::Match s_airport_picker_matches[kAirportPickerRows] = {};
size_t s_airport_picker_match_count = 0;

AircraftHit s_aircraft_hits[services::adsb::kMaxAircraft] = {};
size_t s_aircraft_hit_count = 0;
AirportHit s_airport_hits[kMaxAirportHits] = {};
AirportRender s_airport_render[kMaxAirportHits] = {};
size_t s_airport_hit_count = 0;
LabelBox s_airport_label_boxes[kMaxRadarLabelBoxes] = {};
size_t s_airport_label_count = 0;
LabelBox s_aircraft_label_boxes[kMaxRadarLabelBoxes] = {};
size_t s_aircraft_label_count = 0;
int s_selected_airport = -1;
// "Find me": a QR code for a phone to scan, shown over the picker's results
// while the radar waits for the phone's position.
lv_obj_t* s_locate_panel = nullptr;
lv_obj_t* s_locate_qr = nullptr;
lv_obj_t* s_locate_status = nullptr;
bool s_initialized = false;
RadarChangedFn s_radar_changed_fn = nullptr;

// The aircraft the card describes: none while it is showing an airport.
const model::AircraftView* cardAircraft() {
  return s_selected_airport >= 0 ? nullptr : model::focusAircraft();
}

ThemeColors themeColors() {
  switch (settings::get().theme) {
    case settings::Theme::Green:
      return {lv_color_hex(0x020D0A), lv_color_hex(0x176B45),
              lv_color_hex(0xFF5E62), lv_color_hex(0xF3B4FF),
              lv_color_hex(0x2C8B68), lv_color_hex(0x36B9B0),
              lv_color_hex(0x77D9CE), lv_color_hex(0xE8FFF5),
              lv_color_hex(0xFFF36B)};
    case settings::Theme::HighContrast:
      return {lv_color_black(), lv_color_hex(0x52606D), lv_color_hex(0xFF3B30),
              lv_color_hex(0xFF2DFF), lv_color_hex(0x00D1FF),
              lv_color_hex(0x00E5FF), lv_color_hex(0xFFFFFF),
              lv_color_white(), lv_color_hex(0xFFF200)};
    case settings::Theme::Night:
    default:
      // Runways and airports are a neutral blue-grey: saturated colour on
      // the radar is reserved for aircraft, where it means altitude.
      return {lv_color_hex(0x040A1C), lv_color_hex(0x2E5A4A),
              lv_color_hex(0xFF3B45), lv_color_hex(0xF13DFF),
              lv_color_hex(0x2F789B), lv_color_hex(0x56778C),
              lv_color_hex(0x93B3C7), lv_color_hex(0xEDF6FF),
              lv_color_hex(0xFFE25B)};
  }
}

void formatDistance(float km, char* output, size_t length) {
  if (::ui::radar::useMiles()) {
    snprintf(output, length, "%.1f mi", km / 1.609344f);
  } else {
    snprintf(output, length, "%.1f km", km);
  }
}

const char* airportShortIdent(const char* ident) {
  if (ident != nullptr && ident[0] == 'K' && strlen(ident) == 4) {
    return ident + 1;
  }
  return ident;
}

bool reserveLabelBox(const LabelBox& candidate, LabelBox* boxes, size_t* count,
                     bool force = false) {
  if (!force) {
    for (size_t i = 0; i < *count; ++i) {
      const LabelBox& other = boxes[i];
      if (candidate.left < other.right && candidate.right > other.left &&
          candidate.top < other.bottom && candidate.bottom > other.top) {
        return false;
      }
    }
  }
  if (*count < kMaxRadarLabelBoxes) {
    boxes[(*count)++] = candidate;
  }
  return true;
}

bool isHelicopterType(const char* type) {
  constexpr const char* kTypes[] = {
      "R22", "R44", "R66", "B06", "B47", "EC20", "EC30", "EC35",
      "EC45", "AS50", "AS55", "S76", "H60", "UH1", "A109", "A139",
      "A169", "A189", "BK17"};
  for (const char* helicopter : kTypes) {
    if (strcmp(type, helicopter) == 0) {
      return true;
    }
  }
  return false;
}

AircraftSymbolKind aircraftSymbolKind(const services::adsb::Aircraft& aircraft) {
  if (aircraft.on_ground || strcmp(aircraft.category, "C1") == 0 ||
      strcmp(aircraft.category, "C2") == 0) {
    return AircraftSymbolKind::Ground;
  }
  if (strcmp(aircraft.category, "A7") == 0 ||
      isHelicopterType(aircraft.type) ||
      strstr(aircraft.description, "helicopter") != nullptr ||
      strstr(aircraft.description, "Helicopter") != nullptr) {
    return AircraftSymbolKind::Helicopter;
  }
  if (strcmp(aircraft.category, "B1") == 0) {
    return AircraftSymbolKind::Glider;
  }
  if (strcmp(aircraft.category, "B2") == 0) {
    return AircraftSymbolKind::Balloon;
  }
  if (strcmp(aircraft.category, "B6") == 0) {
    return AircraftSymbolKind::Drone;
  }
  return AircraftSymbolKind::FixedWing;
}

void setPanelStyle(lv_obj_t* object, uint32_t color = kColorPanel) {
  lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(object, lv_color_hex(kColorBorder), 0);
  lv_obj_set_style_border_width(object, 1, 0);
  lv_obj_set_style_radius(object, 8, 0);
  lv_obj_set_style_pad_all(object, 8, 0);
  lv_obj_set_style_text_color(object, lv_color_hex(kColorText), 0);
}

lv_obj_t* addLabel(lv_obj_t* parent, const char* text, const lv_font_t* font,
                   uint32_t color = kColorText) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  return label;
}

// Notes which tiles a traffic primitive covers. Backdrop drawing is not
// tracked; the backdrop is copied in whole when it is rebuilt.
void markTraffic(int left, int top, int right, int bottom) {
  if (s_target != s_traffic) {
    return;
  }
  left = std::max(left, 0);
  top = std::max(top, 0);
  right = std::min(right, kRadarSize - 1);
  bottom = std::min(bottom, kRadarSize - 1);
  for (int row = top / kTileSize; row <= bottom / kTileSize; ++row) {
    for (int column = left / kTileSize; column <= right / kTileSize; ++column) {
      s_traffic_tiles |= 1ULL << (row * kTilesPerSide + column);
    }
  }
}

void markTraffic(const lv_point_t* points, uint32_t count, int reach) {
  if (count == 0) {
    return;
  }
  int left = points[0].x;
  int right = points[0].x;
  int top = points[0].y;
  int bottom = points[0].y;
  for (uint32_t i = 1; i < count; ++i) {
    left = std::min<int>(left, points[i].x);
    right = std::max<int>(right, points[i].x);
    top = std::min<int>(top, points[i].y);
    bottom = std::max<int>(bottom, points[i].y);
  }
  markTraffic(left - reach, top - reach, right + reach, bottom + reach);
}

void drawLine(const lv_point_t* points, uint32_t count, lv_color_t color,
              uint16_t width = 1, lv_opa_t opacity = LV_OPA_COVER) {
  lv_draw_line_dsc_t descriptor;
  lv_draw_line_dsc_init(&descriptor);
  descriptor.color = color;
  descriptor.width = width;
  descriptor.opa = opacity;
  descriptor.round_start = true;
  descriptor.round_end = true;
  // Walk each segment in half-tile steps, so a long diagonal marks the tiles
  // it crosses rather than its whole bounding box.
  for (uint32_t i = 0; i + 1 < count; ++i) {
    const int dx = points[i + 1].x - points[i].x;
    const int dy = points[i + 1].y - points[i].y;
    const int steps = std::max(std::abs(dx), std::abs(dy)) / (kTileSize / 2) + 1;
    for (int step = 0; step < steps; ++step) {
      const lv_point_t piece[] = {
          {static_cast<lv_coord_t>(points[i].x + dx * step / steps),
           static_cast<lv_coord_t>(points[i].y + dy * step / steps)},
          {static_cast<lv_coord_t>(points[i].x + dx * (step + 1) / steps),
           static_cast<lv_coord_t>(points[i].y + dy * (step + 1) / steps)}};
      markTraffic(piece, 2, width + 2);
    }
  }
  lv_canvas_draw_line(s_target, points, count, &descriptor);
}

void drawCircle(int x, int y, int radius, lv_color_t color, uint16_t width = 1,
                lv_opa_t opacity = LV_OPA_COVER) {
  lv_draw_arc_dsc_t descriptor;
  lv_draw_arc_dsc_init(&descriptor);
  descriptor.color = color;
  descriptor.width = width;
  descriptor.opa = opacity;
  const int reach = radius + width + 1;
  markTraffic(x - reach, y - reach, x + reach, y + reach);
  lv_canvas_draw_arc(s_target, x, y, radius, 0, 360, &descriptor);
}

void drawDot(int x, int y, int radius, lv_color_t color) {
  lv_draw_rect_dsc_t descriptor;
  lv_draw_rect_dsc_init(&descriptor);
  descriptor.bg_color = color;
  descriptor.bg_opa = LV_OPA_COVER;
  descriptor.radius = LV_RADIUS_CIRCLE;
  markTraffic(x - radius - 1, y - radius - 1, x + radius + 1, y + radius + 1);
  lv_canvas_draw_rect(s_target, x - radius, y - radius, radius * 2 + 1,
                      radius * 2 + 1, &descriptor);
}

void drawBox(int x, int y, int width, int height, lv_color_t color,
             lv_opa_t opacity = LV_OPA_COVER) {
  lv_draw_rect_dsc_t descriptor;
  lv_draw_rect_dsc_init(&descriptor);
  descriptor.bg_color = color;
  descriptor.bg_opa = opacity;
  descriptor.radius = 2;
  markTraffic(x, y, x + width, y + height);
  lv_canvas_draw_rect(s_target, x, y, width, height, &descriptor);
}

void drawText(int x, int y, int max_width, const char* text, lv_color_t color,
              const lv_font_t* font = &lv_font_montserrat_12,
              lv_text_align_t align = LV_TEXT_ALIGN_LEFT) {
  lv_draw_label_dsc_t descriptor;
  lv_draw_label_dsc_init(&descriptor);
  descriptor.color = color;
  descriptor.font = font;
  descriptor.align = align;
  if (s_target == s_traffic) {
    lv_point_t size;
    lv_txt_get_size(&size, text, font, 0, 0, max_width, LV_TEXT_FLAG_NONE);
    markTraffic(x - 1, y - 1, x + max_width + 1, y + size.y + 1);
  }
  lv_canvas_draw_text(s_target, x, y, max_width, &descriptor, text);
}

geo::Offset offsetFor(float lat, float lon) {
  return geo::offsetFrom(services::location::lat(), services::location::lon(), lat,
                         lon);
}

bool project(float lat, float lon, bool clamp_to_rim, int* x, int* y,
             float* distance = nullptr) {
  const geo::Offset offset = offsetFor(lat, lon);
  if (distance != nullptr) {
    *distance = offset.distance_km;
  }
  const float outer_km = ::ui::radar::rangeCurrent().outer_km;
  if (outer_km <= 0.01f) {
    return false;
  }
  float east = offset.east_km;
  float north = offset.north_km;
  float radius = kRadarRadius;
  bool inside = offset.distance_km <= outer_km;
  if (!inside && clamp_to_rim && offset.distance_km > 0.01f) {
    east /= offset.distance_km;
    north /= offset.distance_km;
    radius = kRimRadius;
    *x = kRadarCenter + static_cast<int>(lroundf(east * radius));
    *y = kRadarCenter - static_cast<int>(lroundf(north * radius));
    return false;
  }
  const float pixels_per_km = radius / outer_km;
  *x = kRadarCenter + static_cast<int>(lroundf(east * pixels_per_km));
  *y = kRadarCenter - static_cast<int>(lroundf(north * pixels_per_km));
  return inside;
}

// Colour has one job on the radar: how high. Warm is low and cool is high, as
// on tar1090 and the flight-tracking sites, so arrivals and overflights
// separate at a glance.
struct AltitudeStop {
  int32_t feet;
  uint8_t red;
  uint8_t green;
  uint8_t blue;
};
// Red is left out: it means an emergency.
constexpr AltitudeStop kAltitudeStops[] = {
    {0, 0xFF, 0x9E, 0x2C},     {3000, 0xFF, 0xD2, 0x3F},
    {10000, 0x9B, 0xE5, 0x52}, {20000, 0x35, 0xDC, 0xC8},
    {30000, 0x5F, 0xB4, 0xFF}, {40000, 0xCB, 0xA6, 0xFF},
};
constexpr size_t kAltitudeStopCount =
    sizeof(kAltitudeStops) / sizeof(kAltitudeStops[0]);
constexpr uint32_t kColorOnGround = 0x93A4B5;
constexpr uint32_t kColorNoAltitude = 0xD5DEE6;

lv_color_t altitudeColor(int32_t feet) {
  const AltitudeStop* low = &kAltitudeStops[0];
  if (feet <= low->feet) {
    return lv_color_make(low->red, low->green, low->blue);
  }
  for (size_t i = 1; i < kAltitudeStopCount; ++i) {
    const AltitudeStop* high = &kAltitudeStops[i];
    if (feet <= high->feet) {
      const int32_t span = high->feet - low->feet;
      const int32_t part = feet - low->feet;
      auto mix = [&](uint8_t from, uint8_t to) {
        return static_cast<uint8_t>(from + (static_cast<int32_t>(to) - from) * part / span);
      };
      return lv_color_make(mix(low->red, high->red), mix(low->green, high->green),
                           mix(low->blue, high->blue));
    }
    low = high;
  }
  return lv_color_make(low->red, low->green, low->blue);
}

lv_color_t aircraftColor(const services::adsb::Aircraft& aircraft) {
  if (aircraft.on_ground) {
    return lv_color_hex(kColorOnGround);
  }
  if (!aircraft.altitude_valid) {
    return lv_color_hex(kColorNoAltitude);
  }
  return altitudeColor(aircraft.alt_ft);
}

// Keeps aircraft tags and airport names off something drawn on the backdrop.
void reserveBackdropBox(int left, int top, int right, int bottom) {
  if (s_airport_label_count < kMaxRadarLabelBoxes) {
    s_airport_label_boxes[s_airport_label_count++] = {
        static_cast<int16_t>(left), static_cast<int16_t>(top),
        static_cast<int16_t>(right), static_cast<int16_t>(bottom)};
  }
}

void drawRadarGrid(const ThemeColors& colors) {
  s_airport_label_count = 0;
  const lv_color_t muted = lv_color_hex(kColorMuted);
  lv_canvas_fill_bg(s_target, colors.background, LV_OPA_COVER);
  // The outer ring is the stated range and the others divide it evenly, so
  // every ring is a round number of kilometres.
  for (int ring = 1; ring <= kRangeRings; ++ring) {
    const bool outer = ring == kRangeRings;
    drawCircle(kRadarCenter, kRadarCenter, kRadarRadius * ring / kRangeRings,
               colors.grid, outer ? 2 : 1, outer ? LV_OPA_COVER : LV_OPA_70);
  }
  // No crosshair through the traffic: bearing ticks inside the outer ring,
  // every 10 degrees and longer every 30, and a small mark at the centre.
  for (int degrees = 0; degrees < 360; degrees += 10) {
    const float angle = degrees * 0.01745329252f;
    const float outer = kRadarRadius - 2.0f;
    const float inner = kRadarRadius - (degrees % 30 == 0 ? 10.0f : 6.0f);
    const lv_point_t tick[] = {
        {static_cast<lv_coord_t>(lroundf(kRadarCenter + sinf(angle) * inner)),
         static_cast<lv_coord_t>(lroundf(kRadarCenter - cosf(angle) * inner))},
        {static_cast<lv_coord_t>(lroundf(kRadarCenter + sinf(angle) * outer)),
         static_cast<lv_coord_t>(lroundf(kRadarCenter - cosf(angle) * outer))}};
    drawLine(tick, 2, colors.grid, 1,
             degrees % 30 == 0 ? LV_OPA_COVER : LV_OPA_70);
  }
  constexpr int kCentreArm = 9;
  constexpr int kCentreGap = 5;
  for (int side = -1; side <= 1; side += 2) {
    const lv_point_t across[] = {
        {static_cast<lv_coord_t>(kRadarCenter + side * kCentreGap), kRadarCenter},
        {static_cast<lv_coord_t>(kRadarCenter + side * kCentreArm), kRadarCenter}};
    const lv_point_t up[] = {
        {kRadarCenter, static_cast<lv_coord_t>(kRadarCenter + side * kCentreGap)},
        {kRadarCenter, static_cast<lv_coord_t>(kRadarCenter + side * kCentreArm)}};
    drawLine(across, 2, colors.label, 1, LV_OPA_70);
    drawLine(up, 2, colors.label, 1, LV_OPA_70);
  }
  drawDot(kRadarCenter, kRadarCenter, 2, colors.label);

  struct Cardinal {
    const char* letter;
    int left;
    int top;
  };
  constexpr int kLetterInset = 13;
  const Cardinal cardinals[] = {
      {"N", kRadarCenter - 12, kRadarCenter - kRadarRadius + kLetterInset},
      {"S", kRadarCenter - 12, kRadarCenter + kRadarRadius - kLetterInset - 19},
      {"W", kRadarCenter - kRadarRadius + kLetterInset - 4, kRadarCenter - 10},
      {"E", kRadarCenter + kRadarRadius - kLetterInset - 20, kRadarCenter - 10},
  };
  for (const Cardinal& cardinal : cardinals) {
    drawText(cardinal.left, cardinal.top, 24, cardinal.letter, colors.label,
             &lv_font_montserrat_16, LV_TEXT_ALIGN_CENTER);
    reserveBackdropBox(cardinal.left + 2, cardinal.top, cardinal.left + 22,
                       cardinal.top + 19);
  }

  // Each ring says how far out it is, on the south-east diagonal where it
  // crosses neither a cardinal nor the usual east-west traffic.
  const bool miles = ::ui::radar::useMiles();
  const float outer_distance =
      ::ui::radar::rangeCurrent().outer_km / (miles ? 1.609344f : 1.0f);
  const lv_font_t* font = &lv_font_montserrat_12;
  for (int ring = 1; ring <= kRangeRings; ++ring) {
    const float distance = outer_distance * ring / kRangeRings;
    char text[16];
    const bool whole = fabsf(distance - roundf(distance)) < 0.05f;
    snprintf(text, sizeof(text), whole ? "%.0f%s" : "%.1f%s", distance,
             ring == kRangeRings ? (miles ? " mi" : " km") : "");
    const int width =
        lv_txt_get_width(text, strlen(text), font, 0, LV_TEXT_FLAG_NONE) + 6;
    const int along = static_cast<int>(
        lroundf(kRadarRadius * ring / static_cast<float>(kRangeRings) * 0.70711f));
    const int left = kRadarCenter + along - width / 2;
    const int top = kRadarCenter + along - 7;
    drawBox(left, top, width, 14, colors.background);
    drawText(left, top, width, text, muted, font, LV_TEXT_ALIGN_CENTER);
    reserveBackdropBox(left, top, left + width, top + 14);
  }

  // Altitude key, in a corner the circle leaves free.
  constexpr int kKeyLeft = 6;
  constexpr int kKeyTop = 400;
  constexpr int kKeyHeight = 72;
  constexpr int32_t kKeyTopFeet = 40000;
  for (int row = 0; row < kKeyHeight; ++row) {
    const int32_t feet = kKeyTopFeet * (kKeyHeight - 1 - row) / (kKeyHeight - 1);
    const lv_point_t line[] = {
        {kKeyLeft, static_cast<lv_coord_t>(kKeyTop + row)},
        {kKeyLeft + 5, static_cast<lv_coord_t>(kKeyTop + row)}};
    drawLine(line, 2, altitudeColor(feet), 1);
  }
  drawText(kKeyLeft + 10, kKeyTop - 3, 44, "FL400", muted, font);
  drawText(kKeyLeft + 10, kKeyTop + kKeyHeight / 2 - 7, 44, "FL200", muted, font);
  drawText(kKeyLeft + 10, kKeyTop + kKeyHeight - 12, 44, "0 ft", muted, font);
  reserveBackdropBox(kKeyLeft, kKeyTop - 4, kKeyLeft + 56, kKeyTop + kKeyHeight + 2);
}

void drawAirportsAndRunways(const ThemeColors& colors) {
  s_airport_hit_count = 0;
  if (!::ui::radar::showRunways()) {
    return;
  }
  const float outer_km = ::ui::radar::rangeCurrent().outer_km;
  const float center_lat = services::location::lat();
  const float center_lon = services::location::lon();
  const int32_t center_lat_e7 = static_cast<int32_t>(lroundf(center_lat * 1e7f));
  const int32_t center_lon_e7 = static_cast<int32_t>(lroundf(center_lon * 1e7f));
  const int64_t lat_margin_e7 =
      static_cast<int64_t>(ceilf(outer_km / 110.5f * 1e7f));
  const float longitude_scale =
      std::max(0.05f, fabsf(cosf(center_lat * 0.01745329252f)));
  const int64_t lon_margin_e7 = std::min<int64_t>(
      1800000000LL,
      static_cast<int64_t>(ceilf(outer_km / (110.5f * longitude_scale) * 1e7f)));
  const float pixels_per_km = static_cast<float>(kRadarRadius) / outer_km;
  // Reading the whole flash table here is what used to displace a frame on
  // the panel at every refresh; see airport_catalog::latitudeCells().
  const int16_t* latitude_cells = data::airport_catalog::latitudeCells();
  const data::airport_catalog::LatitudeBand latitude_band =
      data::airport_catalog::latitudeBand(center_lat, outer_km);

  for (size_t i = 0; i < data::large_airports::kAirportCount; ++i) {
    if (latitude_cells != nullptr && !latitude_band.contains(latitude_cells[i])) {
      continue;
    }
    const auto& airport = data::large_airports::kAirports[i];
    const int64_t lat_delta =
        static_cast<int64_t>(airport.lat_e7) - center_lat_e7;
    if (lat_delta < -lat_margin_e7 || lat_delta > lat_margin_e7) {
      continue;
    }
    int64_t lon_delta = static_cast<int64_t>(airport.lon_e7) - center_lon_e7;
    if (lon_delta > 1800000000LL) {
      lon_delta -= 3600000000LL;
    } else if (lon_delta < -1800000000LL) {
      lon_delta += 3600000000LL;
    }
    if (lon_delta < -lon_margin_e7 || lon_delta > lon_margin_e7) {
      continue;
    }
    const geo::Offset offset = offsetFor(airport.lat_e7 * 1e-7f,
                                         airport.lon_e7 * 1e-7f);
    if (offset.distance_km > outer_km) {
      continue;
    }
    const int x = kRadarCenter +
                  static_cast<int>(lroundf(offset.east_km * pixels_per_km));
    const int y = kRadarCenter -
                  static_cast<int>(lroundf(offset.north_km * pixels_per_km));

    if (s_airport_hit_count < kMaxAirportHits) {
      s_airport_render[s_airport_hit_count++] = {
          {static_cast<int16_t>(x), static_cast<int16_t>(y),
           static_cast<uint16_t>(i)},
          offset.distance_km};
    }
  }

  std::sort(s_airport_render, s_airport_render + s_airport_hit_count,
            [](const AirportRender& lhs, const AirportRender& rhs) {
              const auto& left =
                  data::large_airports::kAirports[lhs.hit.airport_index];
              const auto& right =
                  data::large_airports::kAirports[rhs.hit.airport_index];
              if (left.priority != right.priority) {
                return left.priority > right.priority;
              }
              return lhs.distance_km < rhs.distance_km;
            });

  const uint8_t minimum_label_priority =
      ::ui::radar::rangeCurrent().ring3_km <= 25.0f ? 1 : 2;
  for (size_t render_index = 0; render_index < s_airport_hit_count;
       ++render_index) {
    const AirportHit& hit = s_airport_render[render_index].hit;
    s_airport_hits[render_index] = hit;
    const auto& airport = data::large_airports::kAirports[hit.airport_index];
    const size_t runway_end = static_cast<size_t>(airport.runway_start) +
                              static_cast<size_t>(airport.runway_count);
    for (size_t runway_index = airport.runway_start;
         runway_index < runway_end &&
         runway_index < data::large_airports::kRunwayCount; ++runway_index) {
      const auto& runway = data::large_airports::kRunways[runway_index];
      int x0 = 0;
      int y0 = 0;
      int x1 = 0;
      int y1 = 0;
      project(runway.le_lat_e7 * 1e-7f, runway.le_lon_e7 * 1e-7f, false, &x0,
              &y0);
      project(runway.he_lat_e7 * 1e-7f, runway.he_lon_e7 * 1e-7f, false, &x1,
              &y1);
      const lv_point_t points[] = {
          {static_cast<lv_coord_t>(x0), static_cast<lv_coord_t>(y0)},
          {static_cast<lv_coord_t>(x1), static_cast<lv_coord_t>(y1)}};
      drawLine(points, 2, colors.runway, 2, LV_OPA_80);
    }

    drawDot(hit.x, hit.y, airport.priority >= 3 ? 4 : 3, colors.airport);
    const bool selected = s_selected_airport == hit.airport_index;
    if (airport.priority >= minimum_label_priority || selected) {
      const char* code = airportShortIdent(airport.ident);
      // Size the box from the rendered text; a narrower box wraps the code.
      const int label_width =
          lv_txt_get_width(code, strlen(code), &lv_font_montserrat_12, 0,
                           LV_TEXT_FLAG_NONE) + 2;
      const bool right_side = hit.x < kRadarCenter;
      const int label_x = right_side ? hit.x + 6 : hit.x - 6;
      const LabelBox box = {
          static_cast<int16_t>(right_side ? label_x : label_x - label_width),
          static_cast<int16_t>(hit.y - 17),
          static_cast<int16_t>(right_side ? label_x + label_width : label_x),
          static_cast<int16_t>(hit.y + 1)};
      if (reserveLabelBox(box, s_airport_label_boxes, &s_airport_label_count,
                          selected)) {
        drawText(right_side ? label_x : label_x - label_width, hit.y - 14,
                 label_width, code, selected ? colors.selected : colors.airport,
                 &lv_font_montserrat_12,
                 right_side ? LV_TEXT_ALIGN_LEFT : LV_TEXT_ALIGN_RIGHT);
      }
    }
  }
}

// Symbols are sized by the ADS-B emitter category, A1 (light) to A5 (heavy),
// so a Cessna and a 777 do not look alike.
float symbolScale(const services::adsb::Aircraft& aircraft) {
  if (aircraft.category[0] != 'A') {
    return 1.0f;
  }
  switch (aircraft.category[1]) {
    case '1':
      return 0.72f;
    case '2':
      return 0.86f;
    case '4':
      return 1.15f;
    case '5':
      return 1.3f;
    default:
      return 1.0f;
  }
}

void formatAltitude(const services::adsb::Aircraft& aircraft, char* text,
                    size_t length, bool with_unit = true) {
  // Barometric altitude can read below zero near sea level.
  const long feet = std::max<long>(0, static_cast<long>(aircraft.alt_ft));
  const char* unit = with_unit ? " ft" : "";
  if (aircraft.on_ground) {
    snprintf(text, length, "GND");
  } else if (!aircraft.altitude_valid) {
    snprintf(text, length, "---");
  } else if (feet >= 18000) {
    // Flight levels above the transition altitude, as a controller would say it.
    snprintf(text, length, "FL%03ld", (feet + 50) / 100);
  } else if (feet >= 1000) {
    snprintf(text, length, "%ld,%03ld%s", feet / 1000, feet % 1000, unit);
  } else {
    snprintf(text, length, "%ld%s", feet, unit);
  }
}

const char* trendSymbol(const services::adsb::Aircraft& aircraft) {
  if (aircraft.on_ground) {
    return "";
  }
  if (aircraft.vertical_rate_fpm > 300) {
    return " " LV_SYMBOL_UP;
  }
  if (aircraft.vertical_rate_fpm < -300) {
    return " " LV_SYMBOL_DOWN;
  }
  return "";
}

// Where each visible aircraft is on the radar this frame and how it is drawn.
struct Plot {
  int16_t x;
  int16_t y;
  bool inside;  // Within the outer ring rather than pinned to the rim.
  float scale;
  lv_color_t color;
};
Plot s_plots[services::adsb::kMaxAircraft];
size_t s_plot_count = 0;

void drawTrail(const model::AircraftView& view, const Plot& plot, bool pinned) {
  if (!settings::get().show_trails) {
    return;
  }
  const model::HistoryView history = model::historyFor(view.aircraft.hex);
  if (history.count == 0) {
    return;
  }
  lv_point_t points[model::kHistoryPointCount + 1];
  uint32_t count = 0;
  auto add = [&](float lat, float lon) {
    int x = 0;
    int y = 0;
    float distance = 0;
    project(lat, lon, false, &x, &y, &distance);
    if (distance <= ::ui::radar::rangeCurrent().outer_km * 1.15f) {
      points[count++] = {static_cast<lv_coord_t>(x), static_cast<lv_coord_t>(y)};
    }
  };
  for (size_t i = 0; i < history.count; ++i) {
    add(history.points[i].lat, history.points[i].lon);
  }
  // Reported positions end a little behind where the aircraft is drawn now.
  add(view.aircraft.lat, view.aircraft.lon);
  if (count >= 2) {
    drawLine(points, count, plot.color, pinned ? 2 : 1,
             pinned ? LV_OPA_90 : LV_OPA_60);
  }
}

// A line to where the aircraft will be in a minute at its present speed, two
// minutes for the one that is pinned. It stops at the outer ring.
void drawLeader(const model::AircraftView& view, const Plot& plot, bool pinned) {
  if (view.aircraft.on_ground || !view.aircraft.motion_valid) {
    return;
  }
  const float ahead_km = view.aircraft.gs_knots * 1.852f / 3600.0f *
                         (pinned ? 120.0f : 60.0f);
  float length = ahead_km / ::ui::radar::rangeCurrent().outer_km *
                 static_cast<float>(kRadarRadius);
  const float nose = 14.0f * plot.scale;
  const float track = view.aircraft.track_deg * 0.01745329252f;
  const float east = sinf(track);
  const float north = cosf(track);
  auto outside = [&](float along) {
    const float dx = plot.x + east * along - kRadarCenter;
    const float dy = plot.y - north * along - kRadarCenter;
    return dx * dx + dy * dy > static_cast<float>(kRadarRadius * kRadarRadius);
  };
  for (int step = 0; step < 6 && outside(length); ++step) {
    length *= 0.75f;
  }
  if (length < nose + 4.0f || outside(length)) {
    return;
  }
  const lv_point_t line[] = {
      {static_cast<lv_coord_t>(plot.x + lroundf(east * nose)),
       static_cast<lv_coord_t>(plot.y - lroundf(north * nose))},
      {static_cast<lv_coord_t>(plot.x + lroundf(east * length)),
       static_cast<lv_coord_t>(plot.y - lroundf(north * length))}};
  drawLine(line, 2, plot.color, pinned ? 2 : 1, pinned ? LV_OPA_COVER : LV_OPA_70);
}

void drawSymbol(const model::AircraftView& view, const Plot& plot) {
  const float angle = view.aircraft.nose_deg * 0.01745329252f;
  const float sin_a = sinf(angle);
  const float cos_a = cosf(angle);
  const int x = plot.x;
  const int y = plot.y;
  const float scale = plot.scale;
  auto rotatePoint = [&](float local_x, float local_y) -> lv_point_t {
    local_x *= scale;
    local_y *= scale;
    return {static_cast<lv_coord_t>(lroundf(x + local_x * cos_a + local_y * sin_a)),
            static_cast<lv_coord_t>(lroundf(y + local_x * sin_a - local_y * cos_a))};
  };

  lv_draw_rect_dsc_t polygon;
  lv_draw_rect_dsc_init(&polygon);
  polygon.bg_color = plot.color;
  polygon.bg_opa = LV_OPA_COVER;
  // LVGL fills convex outlines only: its edge walk never terminates on a
  // concave one. Every silhouette is therefore built from convex parts that
  // overlap slightly so no seam shows between them.
  auto fill = [&](std::initializer_list<lv_point_t> outline) {
    markTraffic(outline.begin(), outline.size(), 1);
    lv_canvas_draw_polygon(s_target, outline.begin(), outline.size(), &polygon);
  };

  switch (aircraftSymbolKind(view.aircraft)) {
    case AircraftSymbolKind::Helicopter: {
      // Tail boom, then cabin.
      fill({rotatePoint(-2, -4), rotatePoint(-1, -11), rotatePoint(1, -11),
            rotatePoint(2, -4)});
      fill({rotatePoint(0, 10), rotatePoint(-4, 5), rotatePoint(-4, -2),
            rotatePoint(-2, -5), rotatePoint(2, -5), rotatePoint(4, -2),
            rotatePoint(4, 5)});
      const lv_point_t rotor[] = {rotatePoint(-12, 2), rotatePoint(12, 2)};
      drawLine(rotor, 2, plot.color, 2);
      const lv_point_t tail_rotor[] = {rotatePoint(-4, -10), rotatePoint(4, -10)};
      drawLine(tail_rotor, 2, plot.color, 1);
      break;
    }
    case AircraftSymbolKind::Glider: {
      // Wings, then fuselage.
      fill({rotatePoint(-2, 1), rotatePoint(-13, -1), rotatePoint(-13, -3),
            rotatePoint(13, -3), rotatePoint(13, -1), rotatePoint(2, 1)});
      fill({rotatePoint(0, 11), rotatePoint(-2, 1), rotatePoint(-2, -2),
            rotatePoint(0, -10), rotatePoint(2, -2), rotatePoint(2, 1)});
      break;
    }
    case AircraftSymbolKind::Balloon: {
      drawCircle(x, y - 2, 7, plot.color, 3);
      const lv_point_t basket[] = {rotatePoint(-2, -8), rotatePoint(2, -8)};
      drawLine(basket, 2, plot.color, 3);
      break;
    }
    case AircraftSymbolKind::Drone: {
      const lv_point_t diagonal_one[] = {rotatePoint(-8, 8), rotatePoint(8, -8)};
      const lv_point_t diagonal_two[] = {rotatePoint(8, 8), rotatePoint(-8, -8)};
      drawLine(diagonal_one, 2, plot.color, 2);
      drawLine(diagonal_two, 2, plot.color, 2);
      drawDot(x, y, 3, plot.color);
      break;
    }
    case AircraftSymbolKind::Ground: {
      // Parked and taxiing traffic is context, not the subject: a small mark.
      fill({rotatePoint(-4, 5), rotatePoint(4, 5), rotatePoint(4, -5),
            rotatePoint(-4, -5)});
      break;
    }
    case AircraftSymbolKind::FixedWing:
    default: {
      // Wings, both halves of the tailplane, then fuselage.
      fill({rotatePoint(-2, 4), rotatePoint(-11, 0), rotatePoint(-11, -3),
            rotatePoint(11, -3), rotatePoint(11, 0), rotatePoint(2, 4)});
      fill({rotatePoint(0, -8), rotatePoint(-7, -11), rotatePoint(-7, -13),
            rotatePoint(0, -11)});
      fill({rotatePoint(0, -8), rotatePoint(0, -11), rotatePoint(7, -13),
            rotatePoint(7, -11)});
      fill({rotatePoint(0, 13), rotatePoint(-2, 4), rotatePoint(-3, -9),
            rotatePoint(0, -11), rotatePoint(3, -9), rotatePoint(2, 4)});
      break;
    }
  }
}

bool boxesOverlap(const LabelBox& a, const LabelBox& b) {
  return a.left < b.right && a.right > b.left && a.top < b.bottom && a.bottom > b.top;
}

// True if a tag could go here: on the radar, and not over another tag, an
// airport's name, or another aircraft.
bool tagSpaceFree(const LabelBox& box, size_t own_plot) {
  if (box.left < 1 || box.top < 1 || box.right > kRadarSize - 2 ||
      box.bottom > kRadarSize - 2) {
    return false;
  }
  for (size_t i = 0; i < s_aircraft_label_count; ++i) {
    if (boxesOverlap(box, s_aircraft_label_boxes[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < s_airport_label_count; ++i) {
    if (boxesOverlap(box, s_airport_label_boxes[i])) {
      return false;
    }
  }
  for (size_t i = 0; i < s_plot_count; ++i) {
    if (i == own_plot || !s_plots[i].inside) {
      continue;
    }
    const int16_t reach = static_cast<int16_t>(lroundf(10.0f * s_plots[i].scale));
    const LabelBox symbol = {static_cast<int16_t>(s_plots[i].x - reach),
                             static_cast<int16_t>(s_plots[i].y - reach),
                             static_cast<int16_t>(s_plots[i].x + reach),
                             static_cast<int16_t>(s_plots[i].y + reach)};
    if (boxesOverlap(box, symbol)) {
      return false;
    }
  }
  return true;
}

// The data tag: callsign over altitude. It goes beside the symbol, on the
// side the aircraft is coming from so that it stays off the leader, and tries
// the corners before giving up. `force` places it regardless, for the
// aircraft the detail card is about.
void drawTag(const model::AircraftView& view, size_t plot_index, bool force,
             bool pinned, const ThemeColors& colors) {
  const Plot& plot = s_plots[plot_index];
  const lv_font_t* font = &lv_font_montserrat_12;
  char altitude[20];
  formatAltitude(view.aircraft, altitude, sizeof(altitude));
  char second_line[28];
  snprintf(second_line, sizeof(second_line), "%s%s", altitude,
           trendSymbol(view.aircraft));
  const char* callsign = view.aircraft.callsign;
  const int width =
      std::max(lv_txt_get_width(callsign, strlen(callsign), font, 0,
                                LV_TEXT_FLAG_NONE),
               lv_txt_get_width(second_line, strlen(second_line), font, 0,
                                LV_TEXT_FLAG_NONE)) + 2;
  const int line_height = font->line_height;
  const int height = line_height * 2;
  const int reach = static_cast<int>(lroundf(12.0f * plot.scale)) + 4;

  struct Spot {
    int left;
    int top;
    bool right_aligned;
  };
  const Spot east = {plot.x + reach, plot.y - height / 2, false};
  const Spot west = {plot.x - reach - width, plot.y - height / 2, true};
  const Spot north_east = {plot.x + reach - 5, plot.y - reach - height + 5, false};
  const Spot south_east = {plot.x + reach - 5, plot.y + reach - 5, false};
  const Spot north_west = {plot.x - reach - width + 5, plot.y - reach - height + 5, true};
  const Spot south_west = {plot.x - reach - width + 5, plot.y + reach - 5, true};
  const bool heading_east = sinf(view.aircraft.track_deg * 0.01745329252f) > 0.0f;
  const Spot spots[] = {
      heading_east ? west : east,             heading_east ? north_west : north_east,
      heading_east ? south_west : south_east, heading_east ? east : west,
      heading_east ? north_east : north_west, heading_east ? south_east : south_west,
  };
  auto boxFor = [&](const Spot& spot) {
    return LabelBox{static_cast<int16_t>(spot.left - 1), static_cast<int16_t>(spot.top),
                    static_cast<int16_t>(spot.left + width + 1),
                    static_cast<int16_t>(spot.top + height)};
  };

  const Spot* chosen = nullptr;
  for (const Spot& spot : spots) {
    if (tagSpaceFree(boxFor(spot), plot_index)) {
      chosen = &spot;
      break;
    }
  }
  if (chosen == nullptr) {
    if (!force) {
      return;
    }
    chosen = &spots[0];
  }
  if (s_aircraft_label_count < kMaxRadarLabelBoxes) {
    s_aircraft_label_boxes[s_aircraft_label_count++] = boxFor(*chosen);
  }
  const lv_text_align_t align =
      chosen->right_aligned ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_LEFT;
  lv_color_t name_color = colors.label;
  if (pinned) {
    name_color = colors.selected;
  } else if (view.aircraft.military) {
    name_color = lv_color_hex(kColorWarning);
  }
  // A backing keeps the tag readable over trails, leaders and rings.
  drawBox(chosen->left - 2, chosen->top, width + 4, height, colors.background,
          LV_OPA_70);
  drawText(chosen->left, chosen->top, width, callsign, name_color, font, align);
  drawText(chosen->left, chosen->top + line_height, width, second_line, plot.color,
           font, align);
}

void drawAircraft(const ThemeColors& colors) {
  s_aircraft_hit_count = 0;
  s_aircraft_label_count = 0;
  const model::AircraftView* visible = model::visibleAircraft();
  const size_t count = std::min<size_t>(model::visibleAircraftCount(),
                                        services::adsb::kMaxAircraft);
  const model::AircraftView* pinned = model::selectedAircraft();
  const model::AircraftView* focus = cardAircraft();

  s_plot_count = count;
  for (size_t i = 0; i < count; ++i) {
    int x = 0;
    int y = 0;
    Plot& plot = s_plots[i];
    plot.inside = project(visible[i].aircraft.lat, visible[i].aircraft.lon, true, &x, &y);
    plot.x = static_cast<int16_t>(x);
    plot.y = static_cast<int16_t>(y);
    plot.scale = symbolScale(visible[i].aircraft);
    plot.color = aircraftColor(visible[i].aircraft);
    AircraftHit& hit = s_aircraft_hits[s_aircraft_hit_count++];
    hit.x = x;
    hit.y = y;
    strncpy(hit.hex, visible[i].aircraft.hex, sizeof(hit.hex) - 1);
  }

  // Back to front: trails, leaders, symbols, then the marks and tags on top.
  for (size_t i = 0; i < count; ++i) {
    drawTrail(visible[i], s_plots[i], &visible[i] == pinned);
  }
  for (size_t i = 0; i < count; ++i) {
    if (s_plots[i].inside) {
      drawLeader(visible[i], s_plots[i], &visible[i] == pinned);
    }
  }
  for (size_t i = 0; i < count; ++i) {
    const Plot& plot = s_plots[i];
    if (plot.inside) {
      drawSymbol(visible[i], plot);
    } else {
      // Beyond the outer ring: a hollow mark on the rim, in its direction.
      drawCircle(plot.x, plot.y, 3, plot.color, &visible[i] == pinned ? 2 : 1,
                 LV_OPA_80);
    }
    const int ring = static_cast<int>(lroundf(17.0f * plot.scale));
    if (visible[i].aircraft.emergency) {
      drawCircle(plot.x, plot.y, ring + 4, lv_color_hex(kColorDanger), 2);
    }
    if (&visible[i] == pinned) {
      drawCircle(plot.x, plot.y, ring, colors.selected, 2);
    } else if (&visible[i] == focus) {
      drawCircle(plot.x, plot.y, ring, colors.label, 1, LV_OPA_60);
    }
  }

  if (!settings::get().show_labels) {
    return;
  }
  // The aircraft the card describes get their tags first and unconditionally;
  // the rest take what room is left, nearest first.
  for (size_t i = 0; i < count; ++i) {
    if (s_plots[i].inside && (&visible[i] == pinned || &visible[i] == focus)) {
      drawTag(visible[i], i, true, &visible[i] == pinned, colors);
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (s_plots[i].inside && &visible[i] != pinned && &visible[i] != focus) {
      drawTag(visible[i], i, false, false, colors);
    }
  }
}

// Everything the backdrop depends on. It is repainted when any of it changes.
struct BackdropKey {
  float outer_km;
  double center_lat;
  double center_lon;
  settings::Theme theme;
  bool runways;
  bool miles;
  int selected_airport;

  bool operator==(const BackdropKey& other) const {
    return outer_km == other.outer_km && center_lat == other.center_lat &&
           center_lon == other.center_lon && theme == other.theme &&
           runways == other.runways && miles == other.miles &&
           selected_airport == other.selected_airport;
  }
};
BackdropKey s_backdrop_key = {};
bool s_backdrop_valid = false;

void rebuildBackdrop(const ThemeColors& colors) {
  s_target = s_backdrop;
  drawRadarGrid(colors);
  drawAirportsAndRunways(colors);
  memcpy(s_canvas_buffer, s_backdrop_buffer,
         LV_CANVAS_BUF_SIZE_TRUE_COLOR(kRadarSize, kRadarSize));
  s_traffic_tiles = 0;
  lv_obj_invalidate(s_canvas);
}

// Calls visit(first_column, last_column, row) for each horizontal run of set
// tiles.
template <typename Visitor>
void forEachTileRun(uint64_t tiles, Visitor visit) {
  for (int row = 0; row < kTilesPerSide; ++row) {
    const uint8_t bits = static_cast<uint8_t>(tiles >> (row * kTilesPerSide));
    int start = -1;
    for (int column = 0; column <= kTilesPerSide; ++column) {
      const bool set = column < kTilesPerSide && ((bits >> column) & 1) != 0;
      if (set && start < 0) {
        start = column;
      } else if (!set && start >= 0) {
        visit(start, column - 1, row);
        start = -1;
      }
    }
  }
}

// True while nothing LVGL draws overlaps the radar. Then changed tiles can go
// to the panel as they are; otherwise LVGL has to composite the whole canvas
// with whatever is on top of it.
bool radarUncovered() {
  return lv_scr_act() == s_main_screen &&
         lv_obj_has_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN) &&
         lv_obj_has_flag(s_airport_overlay, LV_OBJ_FLAG_HIDDEN) &&
         lv_obj_get_child_cnt(lv_layer_top()) == 0;
}

void redrawTraffic(const ThemeColors& colors) {
  const uint64_t previous = s_traffic_tiles;
  forEachTileRun(previous, [](int first, int last, int row) {
    const size_t bytes = (last - first + 1) * kTileSize * sizeof(lv_color_t);
    for (int y = row * kTileSize; y < (row + 1) * kTileSize; ++y) {
      const size_t offset = static_cast<size_t>(y) * kRadarSize + first * kTileSize;
      memcpy(s_canvas_buffer + offset, s_backdrop_buffer + offset, bytes);
    }
  });

  s_traffic_tiles = 0;
  s_target = s_traffic;
  drawAircraft(colors);

  const uint64_t changed = previous | s_traffic_tiles;
  if (changed == 0) {
    return;
  }
  if (!radarUncovered()) {
    lv_obj_invalidate(s_canvas);
    return;
  }
  lv_area_t canvas;
  lv_obj_get_coords(s_canvas, &canvas);
  forEachTileRun(changed, [&](int first, int last, int row) {
    const size_t offset =
        static_cast<size_t>(row * kTileSize) * kRadarSize + first * kTileSize;
    display::blit(canvas.x1 + first * kTileSize, canvas.y1 + row * kTileSize,
                  (last - first + 1) * kTileSize, kTileSize,
                  reinterpret_cast<const uint16_t*>(s_canvas_buffer + offset),
                  kRadarSize);
  });
}

// Changes whenever any aircraft would be drawn at a different pixel.
uint32_t trafficSignature() {
  uint32_t hash = 2166136261UL;
  const model::AircraftView* visible = model::visibleAircraft();
  const size_t count = model::visibleAircraftCount();
  for (size_t i = 0; i < count; ++i) {
    int x = 0;
    int y = 0;
    project(visible[i].aircraft.lat, visible[i].aircraft.lon, true, &x, &y);
    hash = (hash ^ static_cast<uint32_t>(x * kRadarSize + y)) * 16777619UL;
  }
  return hash;
}

void redrawRadar() {
  if (s_canvas == nullptr) {
    return;
  }
  const ThemeColors colors = themeColors();
  const BackdropKey key = {::ui::radar::rangeCurrent().outer_km,
                           services::location::lat(),
                           services::location::lon(),
                           settings::get().theme,
                           ::ui::radar::showRunways(),
                           ::ui::radar::useMiles(),
                           s_selected_airport};
  if (!s_backdrop_valid || !(key == s_backdrop_key)) {
    rebuildBackdrop(colors);
    s_backdrop_key = key;
    s_backdrop_valid = true;
  }
  const uint32_t t1 = millis();
  redrawTraffic(colors);
  s_traffic_signature = trafficSignature();
  s_traffic_cost_ms = millis() - t1;
}

// Setting a label's text repaints it even when the text is the same.
void setLabelText(lv_obj_t* label, const char* text) {
  if (strcmp(lv_label_get_text(label), text) != 0) {
    lv_label_set_text(label, text);
  }
}

void updateRangeLabel() {
  char range[16];
  ::ui::radar::formatCurrentRing3Label(range, sizeof(range));
  setLabelText(s_range_label, range);
}

// Changing a style repaints the object even when the value is the same.
void setTextColor(lv_obj_t* label, lv_color_t color) {
  if (lv_obj_get_style_text_color(label, LV_PART_MAIN).full != color.full) {
    lv_obj_set_style_text_color(label, color, 0);
  }
}

void setFillColor(lv_obj_t* object, lv_color_t color) {
  if (lv_obj_get_style_bg_color(object, LV_PART_MAIN).full != color.full) {
    lv_obj_set_style_bg_color(object, color, 0);
  }
}

void setBorder(lv_obj_t* object, lv_color_t color, lv_coord_t width) {
  if (lv_obj_get_style_border_color(object, LV_PART_MAIN).full != color.full) {
    lv_obj_set_style_border_color(object, color, 0);
  }
  if (lv_obj_get_style_border_width(object, LV_PART_MAIN) != width) {
    lv_obj_set_style_border_width(object, width, 0);
  }
}

const char* compassPoint(float bearing_deg) {
  static const char* const kPoints[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  return kPoints[static_cast<int>(lroundf(bearing_deg / 45.0f)) & 7];
}

void formatAltitudeWithTrend(const services::adsb::Aircraft& aircraft, char* text,
                             size_t length, bool with_unit = true) {
  char altitude[20];
  formatAltitude(aircraft, altitude, sizeof(altitude), with_unit);
  snprintf(text, length, "%s%s", altitude, trendSymbol(aircraft));
}

// The traffic list: everything in view except the aircraft on the card,
// nearest first.
void updateTrafficList() {
  const model::AircraftView* visible = model::visibleAircraft();
  const size_t count = model::visibleAircraftCount();
  const model::AircraftView* on_card = cardAircraft();
  size_t row = 0;
  for (size_t i = 0; i < count && row < kTrafficRows; ++i) {
    if (&visible[i] == on_card) {
      continue;
    }
    const services::adsb::Aircraft& aircraft = visible[i].aircraft;
    // Changing the flag repaints the row even when it is already that way.
    if (lv_obj_has_flag(s_traffic_buttons[row], LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_clear_flag(s_traffic_buttons[row], LV_OBJ_FLAG_HIDDEN);
    }
    strncpy(s_traffic_hex[row], aircraft.hex, sizeof(s_traffic_hex[row]) - 1);
    const lv_color_t color = aircraftColor(aircraft);
    setFillColor(s_traffic_bars[row], color);
    setLabelText(s_traffic_names[row], aircraft.callsign);
    setTextColor(s_traffic_names[row],
                 lv_color_hex(aircraft.military ? kColorWarning : kColorText));
    setLabelText(s_traffic_types[row], aircraft.type);
    char text[28];
    // No unit here: the column is narrow and the card and key give it.
    formatAltitudeWithTrend(aircraft, text, sizeof(text), false);
    setLabelText(s_traffic_altitudes[row], text);
    setTextColor(s_traffic_altitudes[row], color);
    formatDistance(visible[i].distance_km, text, sizeof(text));
    setLabelText(s_traffic_distances[row], text);
    ++row;
  }
  const size_t accounted = row + (on_card != nullptr ? 1 : 0);
  for (; row < kTrafficRows; ++row) {
    s_traffic_hex[row][0] = '\0';
    if (!lv_obj_has_flag(s_traffic_buttons[row], LV_OBJ_FLAG_HIDDEN)) {
      lv_obj_add_flag(s_traffic_buttons[row], LV_OBJ_FLAG_HIDDEN);
    }
  }
  char more[24] = "";
  if (count > accounted) {
    snprintf(more, sizeof(more), "+%u more", static_cast<unsigned>(count - accounted));
  }
  setLabelText(s_traffic_more, more);
}

// When and how near the aircraft will pass the centre if it holds its track.
void formatApproach(const model::AircraftView& view, char* text, size_t length) {
  text[0] = '\0';
  const services::adsb::Aircraft& aircraft = view.aircraft;
  if (aircraft.on_ground) {
    snprintf(text, length, "On the ground");
    return;
  }
  if (!aircraft.motion_valid || aircraft.gs_knots < 40.0f) {
    return;
  }
  const geo::Approach approach = geo::closestApproach(
      view.distance_km, view.bearing_deg, aircraft.track_deg, aircraft.gs_knots);
  if (!approach.closing) {
    snprintf(text, length, "Moving away");
    return;
  }
  if (approach.seconds > 20.0f * 60.0f) {
    return;  // Too far ahead for a straight line to mean much.
  }
  const int seconds = static_cast<int>(lroundf(approach.seconds));
  if (approach.distance_km < 0.5f) {
    snprintf(text, length, "Overhead in %d:%02d", seconds / 60, seconds % 60);
    return;
  }
  char distance[16];
  formatDistance(approach.distance_km, distance, sizeof(distance));
  snprintf(text, length, "Passes %s %s in %d:%02d", distance,
           compassPoint(approach.bearing_deg), seconds / 60, seconds % 60);
}

void showAircraftOnCard(const model::AircraftView& view, bool pinned) {
  const services::adsb::Aircraft& aircraft = view.aircraft;
  const settings::State& state = settings::get();
  const bool near_home = state.alerts_enabled && !aircraft.on_ground &&
                         view.distance_km <= state.proximity_km;
  uint32_t accent = kColorAccent;
  const char* caption = pinned ? "TRACKING" : "NEAREST";
  if (aircraft.emergency) {
    accent = kColorDanger;
    caption = "EMERGENCY";
  } else if (near_home) {
    accent = kColorWarning;
    caption = pinned ? "TRACKING, NEAR HOME" : "NEAR HOME";
  }
  setLabelText(s_card_caption, caption);
  setTextColor(s_card_caption, lv_color_hex(accent));
  setBorder(s_card, lv_color_hex(accent == kColorAccent ? kColorBorder : accent),
            accent == kColorAccent ? 1 : 2);

  char text[96];
  if (aircraft.emergency && aircraft.squawk[0] != '\0') {
    snprintf(text, sizeof(text), "SQUAWK %s", aircraft.squawk);
    setTextColor(s_card_badge, lv_color_hex(kColorDanger));
  } else if (aircraft.military) {
    snprintf(text, sizeof(text), "MILITARY");
    setTextColor(s_card_badge, lv_color_hex(kColorWarning));
  } else {
    snprintf(text, sizeof(text), "%s", aircraft.registration);
    setTextColor(s_card_badge, lv_color_hex(kColorMuted));
  }
  setLabelText(s_card_badge, text);
  setLabelText(s_card_title, aircraft.callsign);
  setLabelText(s_card_type, aircraft.type);

  // Routes are fetched in the background and remembered, so asking is free.
  // A pinned aircraft is asked about at once. The nearest one is asked about
  // only if it is a numbered flight (private aircraft have no filed route to
  // find) and has stayed nearest for a moment, so that traffic trading places
  // does not turn into a stream of requests.
  if (strcmp(s_card_hex, aircraft.hex) != 0) {
    strncpy(s_card_hex, aircraft.hex, sizeof(s_card_hex) - 1);
    s_card_since_ms = millis();
  }
  if (pinned || (data::airlines::isFlightNumber(aircraft.callsign) &&
                 millis() - s_card_since_ms >= kRouteDwellMs)) {
    services::route::request(aircraft.callsign, aircraft.lat, aircraft.lon);
  }
  services::route::Info route = {};
  const bool route_known = services::route::find(aircraft.callsign, &route);

  // Who and what: the airline from the route lookup if there is one, else
  // from the callsign, then the aircraft type in words.
  const char* airline = route_known && route.route_found && route.airline[0] != '\0'
                            ? route.airline
                            : data::airlines::nameForCallsign(aircraft.callsign);
  if (airline != nullptr && aircraft.description[0] != '\0') {
    snprintf(text, sizeof(text), "%s  " LV_SYMBOL_BULLET "  %s", airline,
             aircraft.description);
  } else {
    snprintf(text, sizeof(text), "%s",
             airline != nullptr ? airline : aircraft.description);
  }
  setLabelText(s_card_subtitle, text);

  // Where from and to, when the route is known. Otherwise the line says how
  // the aircraft is flying, which is the next best answer to "where is it
  // going".
  char hint[64] = "";
  text[0] = '\0';
  if (route_known && route.route_found) {
    if (route.midpoint[0] != '\0') {
      snprintf(text, sizeof(text), "%s  " LV_SYMBOL_RIGHT "  %s  " LV_SYMBOL_RIGHT "  %s",
               route.origin, route.midpoint, route.destination);
    } else {
      snprintf(text, sizeof(text), "%s  " LV_SYMBOL_RIGHT "  %s", route.origin,
               route.destination);
    }
  } else if (pinned && !route_known) {
    snprintf(hint, sizeof(hint), "Looking up the route...");
  } else if (aircraft.on_ground || !aircraft.motion_valid) {
    hint[0] = '\0';
  } else {
    const long rate = static_cast<long>(aircraft.vertical_rate_fpm);
    const long steps = labs(rate) / 100 * 100;
    if (steps >= 300) {
      snprintf(hint, sizeof(hint), "Heading %s, %s %ld ft/min",
               compassPoint(aircraft.track_deg), rate > 0 ? "climbing" : "descending",
               steps);
    } else {
      snprintf(hint, sizeof(hint), "Heading %s, level", compassPoint(aircraft.track_deg));
    }
  }
  setLabelText(s_card_route, text);
  setLabelText(s_card_hint, hint);

  formatAltitudeWithTrend(aircraft, text, sizeof(text));
  setLabelText(s_card_altitude, text);
  setTextColor(s_card_altitude, aircraftColor(aircraft));
  snprintf(text, sizeof(text), "%.0f kt", aircraft.gs_knots);
  setLabelText(s_card_speed, text);
  char distance[16];
  formatDistance(view.distance_km, distance, sizeof(distance));
  snprintf(text, sizeof(text), "%s %s", distance, compassPoint(view.bearing_deg));
  setLabelText(s_card_position, text);
  formatApproach(view, text, sizeof(text));
  setLabelText(s_card_approach, text);
}

void showAirportOnCard(int index) {
  const auto& airport = data::large_airports::kAirports[index];
  const geo::Offset offset = offsetFor(airport.lat_e7 * 1e-7f, airport.lon_e7 * 1e-7f);
  uint16_t longest_m = 0;
  const size_t runway_end = static_cast<size_t>(airport.runway_start) +
                            static_cast<size_t>(airport.runway_count);
  for (size_t i = airport.runway_start;
       i < runway_end && i < data::large_airports::kRunwayCount; ++i) {
    longest_m = std::max(longest_m, data::large_airports::kRunways[i].length_m);
  }
  setLabelText(s_card_caption, "AIRPORT");
  setTextColor(s_card_caption, lv_color_hex(kColorAccent));
  setBorder(s_card, lv_color_hex(kColorBorder), 1);
  setLabelText(s_card_badge, "");
  const char* short_ident = airportShortIdent(airport.ident);
  setLabelText(s_card_title, short_ident);
  setLabelText(s_card_type, short_ident != airport.ident ? airport.ident : "");
  setLabelText(s_card_subtitle, data::airport_catalog::name(index));
  setLabelText(s_card_route, "");
  setLabelText(s_card_hint,
               (airport.flags & data::large_airports::kAirportScheduledService)
                   ? "Scheduled service"
                   : (airport.priority >= 2 ? "Regional field" : "Local field"));
  char text[64];
  char distance[16];
  formatDistance(offset.distance_km, distance, sizeof(distance));
  snprintf(text, sizeof(text), "%s %s", distance, compassPoint(offset.bearing_deg));
  setLabelText(s_card_altitude, text);
  setTextColor(s_card_altitude, lv_color_hex(kColorText));
  setLabelText(s_card_speed, "");
  snprintf(text, sizeof(text), "%u %s", static_cast<unsigned>(airport.runway_count),
           airport.runway_count == 1 ? "runway" : "runways");
  setLabelText(s_card_position, text);
  snprintf(text, sizeof(text), "Longest %u m", static_cast<unsigned>(longest_m));
  setLabelText(s_card_approach, longest_m > 0 ? text : "");
}

void updateSelectedPanel() {
  if (s_selected_airport >= 0 &&
      s_selected_airport < static_cast<int>(data::large_airports::kAirportCount)) {
    showAirportOnCard(s_selected_airport);
    return;
  }
  const model::AircraftView* pinned = model::selectedAircraft();
  const model::AircraftView* shown = pinned != nullptr ? pinned : model::focusAircraft();
  if (shown != nullptr) {
    showAircraftOnCard(*shown, pinned != nullptr);
    return;
  }
  setLabelText(s_card_caption, "LISTENING");
  setTextColor(s_card_caption, lv_color_hex(kColorMuted));
  setBorder(s_card, lv_color_hex(kColorBorder), 1);
  setLabelText(s_card_badge, "");
  setLabelText(s_card_title, "Clear skies");
  setLabelText(s_card_type, "");
  setLabelText(s_card_subtitle, "Nothing in the air within range");
  setLabelText(s_card_route, "");
  setLabelText(s_card_hint, "");
  setLabelText(s_card_altitude, "");
  setLabelText(s_card_speed, "");
  setLabelText(s_card_position, "");
  setLabelText(s_card_approach, "");
}

// The strip normally counts the traffic. It turns into a warning only for
// something the picture cannot be trusted without: no Wi-Fi, no data, or an
// aircraft squawking an emergency.
void updateStatusStrip() {
  const model::Alert& alert = model::currentAlert();
  const unsigned long success = services::adsb::lastSuccessMs();
  const unsigned long age_seconds = success == 0 ? 0 : (millis() - success) / 1000UL;
  char left[80];
  char right[24] = "";
  uint32_t fill = kColorScreen;
  uint32_t ink = kColorText;
  if (WiFi.status() != WL_CONNECTED) {
    snprintf(left, sizeof(left), "Wi-Fi offline, reconnecting");
    fill = kColorWarning;
  } else if (success != 0 && age_seconds >= kStaleDataSeconds) {
    snprintf(left, sizeof(left), "No data for %lus: %s", age_seconds,
             services::adsb::lastError());
    fill = kColorWarning;
  } else if (alert.active && alert.emergency) {
    snprintf(left, sizeof(left), "%s", alert.text);
    fill = kColorDanger;
  } else {
    const unsigned count = static_cast<unsigned>(model::visibleAircraftCount());
    snprintf(left, sizeof(left), "%u aircraft", count);
    char clock[8] = "";
    const time_t now = time(nullptr);
    if (now > 1700000000) {  // Set from the network; otherwise it reads 1970.
      struct tm utc = {};
      gmtime_r(&now, &utc);
      snprintf(clock, sizeof(clock), "%02d:%02dZ", utc.tm_hour, utc.tm_min);
    }
    snprintf(right, sizeof(right), LV_SYMBOL_WIFI "  %s", clock);
  }
  if (fill != kColorScreen) {
    ink = 0x071221;
  }
  setFillColor(s_status_strip, lv_color_hex(fill));
  setLabelText(s_status_left, left);
  setTextColor(s_status_left, lv_color_hex(ink));
  setLabelText(s_status_right, right);
  setTextColor(s_status_right,
               lv_color_hex(s_status_rssi < kWeakSignalDbm ? kColorWarning : kColorMuted));
}

// After a tap changes what the card is about.
void selectionChanged() {
  updateSelectedPanel();
  updateTrafficList();
  redrawRadar();
}

void cardClicked(lv_event_t*) {
  // Tapping the card lets go of whatever it was pinned to.
  if (model::selectedAircraft() == nullptr && s_selected_airport < 0) {
    return;
  }
  model::clearSelection();
  s_selected_airport = -1;
  selectionChanged();
}

void trafficClicked(lv_event_t* event) {
  const size_t row = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
  if (row >= kTrafficRows || s_traffic_hex[row][0] == '\0') {
    return;
  }
  s_selected_airport = -1;
  model::selectAircraft(s_traffic_hex[row]);
  selectionChanged();
}

void radarClicked(lv_event_t*) {
  lv_point_t point;
  lv_indev_get_point(lv_indev_get_act(), &point);
  int best_distance_sq = 18 * 18;
  const AircraftHit* best_aircraft = nullptr;
  for (size_t i = 0; i < s_aircraft_hit_count; ++i) {
    const int dx = point.x - s_aircraft_hits[i].x;
    const int dy = point.y - s_aircraft_hits[i].y;
    const int distance_sq = dx * dx + dy * dy;
    if (distance_sq < best_distance_sq) {
      best_distance_sq = distance_sq;
      best_aircraft = &s_aircraft_hits[i];
    }
  }
  if (best_aircraft != nullptr) {
    s_selected_airport = -1;
    model::selectAircraft(best_aircraft->hex);
    selectionChanged();
    return;
  }

  best_distance_sq = 20 * 20;
  const AirportHit* best_airport = nullptr;
  for (size_t i = 0; i < s_airport_hit_count; ++i) {
    const int dx = point.x - s_airport_hits[i].x;
    const int dy = point.y - s_airport_hits[i].y;
    const int distance_sq = dx * dx + dy * dy;
    if (distance_sq < best_distance_sq) {
      best_distance_sq = distance_sq;
      best_airport = &s_airport_hits[i];
    }
  }
  model::clearSelection();
  s_selected_airport = best_airport == nullptr ? -1 : best_airport->airport_index;
  selectionChanged();
}

void rangePreviousClicked(lv_event_t*) {
  ::ui::radar::rangePrevious();
  updateRangeLabel();
  redrawRadar();
  if (s_radar_changed_fn != nullptr) {
    s_radar_changed_fn();
  }
}

void rangeNextClicked(lv_event_t*) {
  ::ui::radar::rangeNext();
  updateRangeLabel();
  redrawRadar();
  if (s_radar_changed_fn != nullptr) {
    s_radar_changed_fn();
  }
}

void syncOverlayScrim() {
  const bool settings_open = !lv_obj_has_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
  const bool picker_open = !lv_obj_has_flag(s_airport_overlay, LV_OBJ_FLAG_HIDDEN);
  if (!settings_open && !picker_open) {
    lv_obj_add_flag(s_overlay_scrim, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(s_overlay_scrim, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_overlay_scrim);
  if (settings_open) {
    lv_obj_move_foreground(s_settings_overlay);
  }
  if (picker_open) {
    lv_obj_move_foreground(s_airport_overlay);
  }
}

void closeSettings(lv_event_t*) {
  lv_obj_add_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
  syncOverlayScrim();
}

void applySwitchState(lv_obj_t* object, bool enabled) {
  if (enabled) {
    lv_obj_add_state(object, LV_STATE_CHECKED);
  } else {
    lv_obj_clear_state(object, LV_STATE_CHECKED);
  }
}

void settingsSwitchChanged(lv_event_t* event) {
  lv_obj_t* object = lv_event_get_target(event);
  const bool enabled = lv_obj_has_state(object, LV_STATE_CHECKED);
  if (object == s_labels_switch) settings::setShowLabels(enabled);
  if (object == s_trails_switch) settings::setShowTrails(enabled);
  if (object == s_runways_switch) ::ui::radar::setShowRunways(enabled);
  if (object == s_ground_switch) settings::setShowGround(enabled);
  if (object == s_logging_switch) {
    settings::setLoggingEnabled(enabled);
    if (enabled && !logger::available()) {
      logger::init();
      lv_label_set_text(s_sd_status, logger::status());
    }
  }
  if (object == s_alerts_switch) settings::setAlertsEnabled(enabled);
  if (object == s_units_switch) ::ui::radar::setUseMiles(enabled);
  model::update();
  refreshData();
}

void brightnessChanged(lv_event_t* event) {
  const int value = lv_slider_get_value(s_brightness_slider);
  char text[12];
  snprintf(text, sizeof(text), "%d%%", value);
  lv_label_set_text(s_brightness_value, text);
  display::setBrightness(value);
  if (event != nullptr && lv_event_get_code(event) == LV_EVENT_RELEASED) {
    settings::setBrightness(value);
  }
}

void proximityChanged(lv_event_t* event) {
  const int value = lv_slider_get_value(s_proximity_slider);
  char text[16];
  if (::ui::radar::useMiles()) {
    snprintf(text, sizeof(text), "%.1f mi", value / 1.609344f);
  } else {
    snprintf(text, sizeof(text), "%d km", value);
  }
  lv_label_set_text(s_proximity_value, text);
  if (event != nullptr && lv_event_get_code(event) == LV_EVENT_RELEASED) {
    settings::setProximityKm(value);
    model::update();
    updateSelectedPanel();
    updateStatusStrip();
  }
}

void dropdownChanged(lv_event_t* event) {
  lv_obj_t* object = lv_event_get_target(event);
  const uint16_t selected = lv_dropdown_get_selected(object);
  if (object == s_altitude_dropdown) {
    settings::setAltitudeBand(static_cast<settings::AltitudeBand>(selected));
    model::update();
    refreshData();
  } else if (object == s_refresh_dropdown) {
    const size_t last = sizeof(settings::kRefreshChoices) /
                            sizeof(settings::kRefreshChoices[0]) - 1;
    settings::setRefreshSeconds(
        settings::kRefreshChoices[std::min<size_t>(selected, last)]);
  } else if (object == s_theme_dropdown) {
    settings::setTheme(static_cast<settings::Theme>(selected));
    redrawRadar();
  }
}

void wifiResetConfirmation(lv_event_t* event) {
  lv_obj_t* box = lv_event_get_current_target(event);
  const char* choice = lv_msgbox_get_active_btn_text(box);
  if (choice != nullptr && strcmp(choice, "Reset") == 0) {
    wifiResetCredentialsAndReboot();
    return;
  }
  lv_msgbox_close(box);
}

void wifiResetClicked(lv_event_t*) {
  static const char* buttons[] = {"Cancel", "Reset", ""};
  lv_obj_t* box = lv_msgbox_create(nullptr, "Reset Wi-Fi?",
                                   "Saved Wi-Fi, location, and units will be cleared.",
                                   buttons, true);
  lv_obj_set_width(box, 420);
  lv_obj_add_event_cb(box, wifiResetConfirmation, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_center(box);
}

void openSettings(lv_event_t*) {
  const settings::State& state = settings::get();
  lv_slider_set_value(s_brightness_slider, state.brightness_percent, LV_ANIM_OFF);
  lv_slider_set_value(s_proximity_slider, state.proximity_km, LV_ANIM_OFF);
  applySwitchState(s_labels_switch, state.show_labels);
  applySwitchState(s_trails_switch, state.show_trails);
  applySwitchState(s_runways_switch, ::ui::radar::showRunways());
  applySwitchState(s_ground_switch, state.show_ground);
  applySwitchState(s_logging_switch, state.logging_enabled);
  applySwitchState(s_alerts_switch, state.alerts_enabled);
  applySwitchState(s_units_switch, ::ui::radar::useMiles());
  lv_dropdown_set_selected(s_altitude_dropdown,
                           static_cast<uint16_t>(state.altitude_band));
  lv_dropdown_set_selected(s_theme_dropdown, static_cast<uint16_t>(state.theme));
  lv_dropdown_set_selected(
      s_refresh_dropdown,
      std::find(std::begin(settings::kRefreshChoices),
                std::end(settings::kRefreshChoices), state.refresh_seconds) -
          std::begin(settings::kRefreshChoices));
  lv_label_set_text(s_sd_status, logger::status());
  lv_label_set_text(s_home_airport_value,
                    services::location::hasSelectedAirport()
                        ? airportShortIdent(services::location::selectedAirport())
                        : "My location");
  brightnessChanged(nullptr);
  proximityChanged(nullptr);
  lv_obj_clear_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_settings_overlay);
  syncOverlayScrim();
}

const char* airportTier(const data::large_airports::Airport& airport) {
  if (airport.priority >= 4) return "MAJOR";
  if (airport.priority >= 3) return "SERVED";
  if (airport.priority == 2) return "REGIONAL";
  return "LOCAL";
}

void updateAirportPickerRows(const char* heading) {
  lv_label_set_text(s_airport_picker_heading, heading);
  const int best = data::airport_catalog::bestNearby(
      services::location::userLat(), services::location::userLon());
  for (size_t row = 0; row < kAirportPickerRows; ++row) {
    if (row >= s_airport_picker_match_count) {
      lv_obj_add_flag(s_airport_result_buttons[row], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(s_airport_result_buttons[row], LV_OBJ_FLAG_HIDDEN);
    const auto& match = s_airport_picker_matches[row];
    const auto& airport =
        data::large_airports::kAirports[match.airport_index];
    char distance[16];
    formatDistance(match.distance_km, distance, sizeof(distance));
    char line[176];
    snprintf(line, sizeof(line), "%s%s  %.80s\n%s from saved location  |  %s",
             match.airport_index == best ? "* " : "",
             airportShortIdent(airport.ident),
             data::airport_catalog::name(match.airport_index), distance,
             airportTier(airport));
    lv_label_set_text(s_airport_result_labels[row], line);
  }
}

// Puts the results back and stops waiting for a phone.
void hideLocatePanel() {
  if (!lv_obj_has_flag(s_locate_panel, LV_OBJ_FLAG_HIDDEN)) {
    lv_obj_add_flag(s_locate_panel, LV_OBJ_FLAG_HIDDEN);
  }
  phone_location::end();
}

void hideAirportKeyboard() {
  lv_obj_add_flag(s_airport_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_state(s_airport_search, LV_STATE_FOCUSED);
}

void showNearbyAirports(lv_event_t*) {
  hideAirportKeyboard();
  hideLocatePanel();
  s_airport_picker_match_count = data::airport_catalog::nearest(
      services::location::userLat(), services::location::userLon(),
      s_airport_picker_matches, kAirportPickerRows);
  updateAirportPickerRows("NEAREST AIRPORTS  (* automatic default)");
}

void showAirportKeyboard(lv_event_t*) {
  hideLocatePanel();
  lv_obj_clear_flag(s_airport_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_keyboard_set_textarea(s_airport_keyboard, s_airport_search);
  lv_obj_add_state(s_airport_search, LV_STATE_FOCUSED);
  lv_obj_move_foreground(s_airport_keyboard);
}

void airportKeyboardEvent(lv_event_t* event) {
  const lv_event_code_t code = lv_event_get_code(event);
  if (code == LV_EVENT_CANCEL) {
    hideAirportKeyboard();
    return;
  }
  if (code != LV_EVENT_READY) {
    return;
  }
  const char* query = lv_textarea_get_text(s_airport_search);
  s_airport_picker_match_count = data::airport_catalog::search(
      query, services::location::userLat(), services::location::userLon(),
      s_airport_picker_matches, kAirportPickerRows);
  hideAirportKeyboard();
  char heading[80];
  snprintf(heading, sizeof(heading), "SEARCH RESULTS FOR \"%.36s\"", query);
  updateAirportPickerRows(heading);
}

void closeAirportPicker(lv_event_t*) {
  hideAirportKeyboard();
  hideLocatePanel();
  lv_obj_add_flag(s_airport_overlay, LV_OBJ_FLAG_HIDDEN);
  syncOverlayScrim();
}

void useSavedLocationClicked(lv_event_t*) {
  services::location::useUserLocation();
  model::reset();
  closeAirportPicker(nullptr);
  lv_obj_add_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
  syncOverlayScrim();
  refreshData();
  if (s_radar_changed_fn != nullptr) {
    s_radar_changed_fn();
  }
}

void airportResultClicked(lv_event_t* event) {
  const size_t row = reinterpret_cast<size_t>(lv_event_get_user_data(event));
  if (row >= s_airport_picker_match_count) {
    return;
  }
  const uint16_t index = s_airport_picker_matches[row].airport_index;
  const auto& airport = data::large_airports::kAirports[index];
  services::location::selectAirport(airport.ident, airport.lat_e7 * 1e-7,
                                    airport.lon_e7 * 1e-7);
  model::reset();
  closeAirportPicker(nullptr);
  lv_obj_add_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
  syncOverlayScrim();
  refreshData();
  if (s_radar_changed_fn != nullptr) {
    s_radar_changed_fn();
  }
}

// "Find me" sets the radar's location from a phone's GPS. The radar serves a
// page that asks the phone for it and shows a QR code for that page; see
// phone_location.h. The picker carries on underneath until an answer comes.
void findMeClicked(lv_event_t*) {
  hideAirportKeyboard();
  char url[64];
  switch (phone_location::begin(url, sizeof(url))) {
    case phone_location::Start::NoWifi:
      lv_label_set_text(s_airport_picker_heading,
                        "FIND ME NEEDS WI-FI  -  the radar is not connected");
      return;
    case phone_location::Start::Failed:
      lv_label_set_text(s_airport_picker_heading,
                        "FIND ME COULD NOT START  -  tap Find me to try again");
      return;
    case phone_location::Start::Ready:
      break;
  }
  lv_qrcode_update(s_locate_qr, url, strlen(url));
  lv_label_set_text(s_locate_status, "Waiting for your phone...");
  lv_label_set_text(s_airport_picker_heading, "FIND ME");
  lv_obj_clear_flag(s_locate_panel, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_locate_panel);
}

void cancelFindMe(lv_event_t*) { showNearbyAirports(nullptr); }

void openAirportPicker(lv_event_t*) {
  char title[64];
  snprintf(title, sizeof(title), "HOME AIRPORT  |  CURRENT: %s",
           services::location::hasSelectedAirport()
               ? airportShortIdent(services::location::selectedAirport())
               : "USER LOCATION");
  lv_label_set_text(s_airport_picker_title, title);
  lv_textarea_set_text(s_airport_search, "");
  lv_obj_clear_flag(s_airport_overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(s_airport_overlay);
  syncOverlayScrim();
  showNearbyAirports(nullptr);
}

lv_obj_t* addButton(lv_obj_t* parent, int x, int y, int width, int height,
                    const char* text, lv_event_cb_t callback, void* user_data = nullptr) {
  lv_obj_t* button = lv_btn_create(parent);
  lv_obj_set_pos(button, x, y);
  lv_obj_set_size(button, width, height);
  lv_obj_set_style_bg_color(button, lv_color_hex(kColorPanelAlt), 0);
  lv_obj_set_style_bg_color(button, lv_color_hex(kColorAccent), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(button, lv_color_hex(kColorBorder), 0);
  lv_obj_set_style_border_width(button, 1, 0);
  lv_obj_set_style_radius(button, 7, 0);
  lv_obj_set_style_shadow_width(button, 0, 0);
  lv_obj_t* label = addLabel(button, text, &lv_font_montserrat_16);
  lv_obj_center(label);
  if (callback != nullptr) {
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);
  }
  return button;
}

void createAirportPickerOverlay() {
  s_airport_overlay = lv_obj_create(s_main_screen);
  lv_obj_set_pos(s_airport_overlay, 20, 10);
  lv_obj_set_size(s_airport_overlay, 760, 460);
  setPanelStyle(s_airport_overlay, 0x071221);
  lv_obj_set_style_border_width(s_airport_overlay, 2, 0);
  lv_obj_set_style_border_color(s_airport_overlay, lv_color_hex(kColorAccent), 0);
  lv_obj_clear_flag(s_airport_overlay, LV_OBJ_FLAG_SCROLLABLE);

  s_airport_picker_title =
      addLabel(s_airport_overlay, "HOME AIRPORT", &lv_font_montserrat_20);
  lv_obj_set_pos(s_airport_picker_title, 10, 0);
  addButton(s_airport_overlay, 680, 0, 54, 42, LV_SYMBOL_CLOSE, closeAirportPicker);

  s_airport_search = lv_textarea_create(s_airport_overlay);
  lv_obj_set_pos(s_airport_search, 10, 48);
  lv_obj_set_size(s_airport_search, 250, 44);
  lv_textarea_set_one_line(s_airport_search, true);
  lv_textarea_set_max_length(s_airport_search, 40);
  lv_textarea_set_placeholder_text(s_airport_search, "Code, airport name, or city");
  lv_obj_add_event_cb(s_airport_search, showAirportKeyboard, LV_EVENT_CLICKED,
                      nullptr);
  addButton(s_airport_overlay, 268, 48, 86, 44, "Search", showAirportKeyboard);
  addButton(s_airport_overlay, 362, 48, 86, 44, "Nearby", showNearbyAirports);
  addButton(s_airport_overlay, 456, 48, 104, 44, "Find me", findMeClicked);
  addButton(s_airport_overlay, 568, 48, 166, 44, "Use location",
            useSavedLocationClicked);

  s_airport_picker_heading = addLabel(
      s_airport_overlay, "NEAREST AIRPORTS", &lv_font_montserrat_12,
      kColorAccent);
  lv_obj_set_pos(s_airport_picker_heading, 10, 102);

  for (size_t row = 0; row < kAirportPickerRows; ++row) {
    s_airport_result_buttons[row] = lv_btn_create(s_airport_overlay);
    lv_obj_set_pos(s_airport_result_buttons[row], 10, 124 + row * 60);
    lv_obj_set_size(s_airport_result_buttons[row], 724, 54);
    lv_obj_set_style_bg_color(s_airport_result_buttons[row],
                              lv_color_hex(kColorPanelAlt), 0);
    lv_obj_set_style_bg_color(s_airport_result_buttons[row],
                              lv_color_hex(kColorAccent), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(s_airport_result_buttons[row], 0, 0);
    lv_obj_set_style_radius(s_airport_result_buttons[row], 5, 0);
    lv_obj_set_style_pad_all(s_airport_result_buttons[row], 5, 0);
    s_airport_result_labels[row] = addLabel(
        s_airport_result_buttons[row], "--", &lv_font_montserrat_12);
    // Fixed two-line height; see the selected-target body label.
    lv_obj_set_size(s_airport_result_labels[row], 710, 30);
    lv_label_set_long_mode(s_airport_result_labels[row], LV_LABEL_LONG_DOT);
    lv_obj_align(s_airport_result_labels[row], LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_add_event_cb(s_airport_result_buttons[row], airportResultClicked,
                        LV_EVENT_CLICKED, reinterpret_cast<void*>(row));
  }

  // "Find me" covers the results: the code on a white tile, which phone
  // cameras need, and what to do with it beside it.
  s_locate_panel = lv_obj_create(s_airport_overlay);
  lv_obj_set_pos(s_locate_panel, 10, 122);
  lv_obj_set_size(s_locate_panel, 724, 300);
  setPanelStyle(s_locate_panel, kColorPanelAlt);
  lv_obj_set_style_pad_all(s_locate_panel, 0, 0);
  lv_obj_clear_flag(s_locate_panel, LV_OBJ_FLAG_SCROLLABLE);
  constexpr int kCodeSize = 232;
  constexpr int kQuietZone = 14;
  lv_obj_t* tile = lv_obj_create(s_locate_panel);
  lv_obj_remove_style_all(tile);
  lv_obj_set_pos(tile, 18, 18);
  lv_obj_set_size(tile, kCodeSize + 2 * kQuietZone, kCodeSize + 2 * kQuietZone);
  lv_obj_set_style_bg_color(tile, lv_color_white(), 0);
  lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(tile, 8, 0);
  s_locate_qr = lv_qrcode_create(tile, kCodeSize, lv_color_black(), lv_color_white());
  lv_obj_center(s_locate_qr);
  constexpr int kTextLeft = 18 + kCodeSize + 2 * kQuietZone + 24;
  constexpr int kTextWidth = 724 - kTextLeft - 20;
  lv_obj_t* locate_title =
      addLabel(s_locate_panel, "Scan this with your phone", &lv_font_montserrat_24);
  lv_obj_set_pos(locate_title, kTextLeft, 18);
  lv_obj_t* locate_steps = addLabel(
      s_locate_panel,
      "1.  Open the link from the phone's camera.\n"
      "2.  The phone warns that the connection is not private. Choose "
      "Show Details (or Advanced), then visit the site anyway.\n"
      "3.  Tap Send my location, and allow it.",
      &lv_font_montserrat_16);
  lv_obj_set_pos(locate_steps, kTextLeft, 58);
  lv_obj_set_width(locate_steps, kTextWidth);
  lv_obj_set_style_text_line_space(locate_steps, 5, 0);
  // The warning needs explaining: people are right not to click through one.
  lv_obj_t* locate_note = addLabel(
      s_locate_panel,
      "The warning is expected: the page comes from the radar itself, which "
      "signs its own certificate. Use the same Wi-Fi as the radar.",
      &lv_font_montserrat_14, kColorMuted);
  lv_obj_set_pos(locate_note, kTextLeft, 180);
  lv_obj_set_width(locate_note, kTextWidth);
  s_locate_status =
      addLabel(s_locate_panel, "", &lv_font_montserrat_16, kColorAccent);
  lv_obj_set_pos(s_locate_status, kTextLeft, 246);
  addButton(s_locate_panel, 724 - 20 - 110, 236, 110, 44, "Cancel", cancelFindMe);
  lv_obj_add_flag(s_locate_panel, LV_OBJ_FLAG_HIDDEN);

  s_airport_keyboard = lv_keyboard_create(s_airport_overlay);
  // A new keyboard is bottom-aligned, which would turn y into an offset below
  // the overlay's bottom edge. Anchor it top-left like everything else here.
  lv_obj_align(s_airport_keyboard, LV_ALIGN_TOP_LEFT, 0, 210);
  lv_obj_set_size(s_airport_keyboard, 740, 224);
  lv_obj_add_event_cb(s_airport_keyboard, airportKeyboardEvent, LV_EVENT_ALL,
                      nullptr);
  lv_obj_add_flag(s_airport_keyboard, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_airport_overlay, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* addSettingSwitch(lv_obj_t* parent, int x, int y, const char* label_text) {
  addLabel(parent, label_text, &lv_font_montserrat_16, kColorText);
  lv_obj_t* label = lv_obj_get_child(parent, lv_obj_get_child_cnt(parent) - 1);
  lv_obj_set_pos(label, x, y + 5);
  lv_obj_t* toggle = lv_switch_create(parent);
  lv_obj_set_pos(toggle, x + 190, y);
  lv_obj_set_size(toggle, 54, 30);
  lv_obj_add_event_cb(toggle, settingsSwitchChanged, LV_EVENT_VALUE_CHANGED, nullptr);
  return toggle;
}

void createSettingsOverlay() {
  s_settings_overlay = lv_obj_create(s_main_screen);
  lv_obj_set_pos(s_settings_overlay, 50, 25);
  lv_obj_set_size(s_settings_overlay, 700, 430);
  setPanelStyle(s_settings_overlay, 0x071221);
  lv_obj_set_style_border_width(s_settings_overlay, 2, 0);
  lv_obj_set_style_border_color(s_settings_overlay, lv_color_hex(kColorAccent), 0);
  lv_obj_clear_flag(s_settings_overlay, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* title = addLabel(s_settings_overlay, "RADAR SETTINGS", &lv_font_montserrat_24);
  lv_obj_set_pos(title, 10, 4);
  addButton(s_settings_overlay, 620, 0, 54, 42, LV_SYMBOL_CLOSE, closeSettings);

  addLabel(s_settings_overlay, "Brightness", &lv_font_montserrat_16, kColorMuted);
  lv_obj_set_pos(lv_obj_get_child(s_settings_overlay, 2), 16, 58);
  s_brightness_slider = lv_slider_create(s_settings_overlay);
  lv_obj_set_pos(s_brightness_slider, 120, 60);
  lv_obj_set_size(s_brightness_slider, 170, 18);
  lv_slider_set_range(s_brightness_slider, 10, 100);
  lv_obj_add_event_cb(s_brightness_slider, brightnessChanged, LV_EVENT_VALUE_CHANGED,
                      nullptr);
  lv_obj_add_event_cb(s_brightness_slider, brightnessChanged, LV_EVENT_RELEASED, nullptr);
  s_brightness_value = addLabel(s_settings_overlay, "75%", &lv_font_montserrat_14);
  lv_obj_set_pos(s_brightness_value, 300, 58);

  addLabel(s_settings_overlay, "Home alert", &lv_font_montserrat_16, kColorMuted);
  lv_obj_set_pos(lv_obj_get_child(s_settings_overlay, 5), 360, 58);
  s_proximity_slider = lv_slider_create(s_settings_overlay);
  lv_obj_set_pos(s_proximity_slider, 465, 60);
  lv_obj_set_size(s_proximity_slider, 130, 18);
  lv_slider_set_range(s_proximity_slider, 1, 25);
  lv_obj_add_event_cb(s_proximity_slider, proximityChanged, LV_EVENT_VALUE_CHANGED,
                      nullptr);
  lv_obj_add_event_cb(s_proximity_slider, proximityChanged, LV_EVENT_RELEASED, nullptr);
  s_proximity_value = addLabel(s_settings_overlay, "5 km", &lv_font_montserrat_14);
  lv_obj_set_pos(s_proximity_value, 605, 58);

  s_labels_switch = addSettingSwitch(s_settings_overlay, 16, 105, "Aircraft labels");
  s_trails_switch = addSettingSwitch(s_settings_overlay, 16, 145, "Flight trails");
  s_runways_switch = addSettingSwitch(s_settings_overlay, 16, 185, "Airport runways");
  s_ground_switch = addSettingSwitch(s_settings_overlay, 16, 225, "Ground aircraft");

  s_logging_switch = addSettingSwitch(s_settings_overlay, 360, 105, "microSD logging");
  s_alerts_switch = addSettingSwitch(s_settings_overlay, 360, 145, "Awareness alerts");
  s_units_switch = addSettingSwitch(s_settings_overlay, 360, 185, "Miles instead of km");
  addLabel(s_settings_overlay, "Home airport", &lv_font_montserrat_16, kColorText);
  lv_obj_set_pos(lv_obj_get_child(s_settings_overlay,
                                  lv_obj_get_child_cnt(s_settings_overlay) - 1),
                 360, 232);
  s_home_airport_value =
      addLabel(s_settings_overlay, "Auto", &lv_font_montserrat_16, kColorAccent);
  lv_obj_set_pos(s_home_airport_value, 482, 232);
  addButton(s_settings_overlay, 585, 220, 90, 42, "Change", openAirportPicker);

  addLabel(s_settings_overlay, "Altitude", &lv_font_montserrat_14, kColorMuted);
  lv_obj_set_pos(lv_obj_get_child(s_settings_overlay, lv_obj_get_child_cnt(s_settings_overlay) - 1),
                 16, 282);
  s_altitude_dropdown = lv_dropdown_create(s_settings_overlay);
  lv_dropdown_set_options(s_altitude_dropdown,
                          "All altitudes\nBelow 10,000 ft\n10,000-30,000 ft\nAbove 30,000 ft");
  lv_obj_set_pos(s_altitude_dropdown, 16, 305);
  lv_obj_set_size(s_altitude_dropdown, 210, 42);
  lv_obj_add_event_cb(s_altitude_dropdown, dropdownChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  addLabel(s_settings_overlay, "Refresh", &lv_font_montserrat_14, kColorMuted);
  lv_obj_set_pos(lv_obj_get_child(s_settings_overlay, lv_obj_get_child_cnt(s_settings_overlay) - 1),
                 245, 282);
  s_refresh_dropdown = lv_dropdown_create(s_settings_overlay);
  // One entry per settings::kRefreshChoices value, in the same order.
  lv_dropdown_set_options(s_refresh_dropdown,
                          "2 seconds\n3 seconds\n5 seconds\n10 seconds");
  lv_obj_set_pos(s_refresh_dropdown, 245, 305);
  lv_obj_set_size(s_refresh_dropdown, 145, 42);
  lv_obj_add_event_cb(s_refresh_dropdown, dropdownChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  addLabel(s_settings_overlay, "Theme", &lv_font_montserrat_14, kColorMuted);
  lv_obj_set_pos(lv_obj_get_child(s_settings_overlay, lv_obj_get_child_cnt(s_settings_overlay) - 1),
                 410, 282);
  s_theme_dropdown = lv_dropdown_create(s_settings_overlay);
  lv_dropdown_set_options(s_theme_dropdown, "Night\nGreen scope\nHigh contrast");
  lv_obj_set_pos(s_theme_dropdown, 410, 305);
  lv_obj_set_size(s_theme_dropdown, 155, 42);
  lv_obj_add_event_cb(s_theme_dropdown, dropdownChanged, LV_EVENT_VALUE_CHANGED, nullptr);

  lv_obj_t* reset_wifi =
      addButton(s_settings_overlay, 572, 300, 104, 48, "Reset Wi-Fi", wifiResetClicked);
  lv_obj_set_style_text_font(lv_obj_get_child(reset_wifi, 0), &lv_font_montserrat_14, 0);
  s_sd_status = addLabel(s_settings_overlay, "microSD", &lv_font_montserrat_14,
                         kColorMuted);
  lv_obj_set_pos(s_sd_status, 16, 372);
  lv_obj_t* note = addLabel(s_settings_overlay,
                            "Wi-Fi/location portal: plane-radar.local",
                            &lv_font_montserrat_14, kColorMuted);
  lv_obj_set_pos(note, 330, 372);

  lv_obj_add_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN);
}

void createMainScreen() {
  s_main_screen = lv_obj_create(nullptr);
  lv_obj_remove_style_all(s_main_screen);
  lv_obj_set_style_bg_color(s_main_screen, lv_color_hex(kColorScreen), 0);
  lv_obj_set_style_bg_opa(s_main_screen, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_main_screen, LV_OBJ_FLAG_SCROLLABLE);

  s_canvas = lv_canvas_create(s_main_screen);
  s_canvas_buffer = static_cast<lv_color_t*>(heap_caps_malloc(
      LV_CANVAS_BUF_SIZE_TRUE_COLOR(kRadarSize, kRadarSize),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  s_backdrop_buffer = static_cast<lv_color_t*>(heap_caps_malloc(
      LV_CANVAS_BUF_SIZE_TRUE_COLOR(kRadarSize, kRadarSize),
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_canvas_buffer == nullptr || s_backdrop_buffer == nullptr) {
    return;
  }
  lv_canvas_set_buffer(s_canvas, s_canvas_buffer, kRadarSize, kRadarSize,
                       LV_IMG_CF_TRUE_COLOR);
  s_backdrop = lv_canvas_create(s_main_screen);
  lv_canvas_set_buffer(s_backdrop, s_backdrop_buffer, kRadarSize, kRadarSize,
                       LV_IMG_CF_TRUE_COLOR);
  lv_obj_add_flag(s_backdrop, LV_OBJ_FLAG_HIDDEN);
  s_traffic = lv_canvas_create(s_main_screen);
  lv_canvas_set_buffer(s_traffic, s_canvas_buffer, kRadarSize, kRadarSize,
                       LV_IMG_CF_TRUE_COLOR);
  lv_obj_add_flag(s_traffic, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_pos(s_canvas, 0, 0);
  lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_canvas, radarClicked, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* sidebar = lv_obj_create(s_main_screen);
  // Position and size are local LVGL styles. Remove the theme first so these
  // values are not erased and the sidebar does not jump back over the radar.
  lv_obj_remove_style_all(sidebar);
  lv_obj_set_pos(sidebar, kSidebarX, 0);
  lv_obj_set_size(sidebar, kSidebarWidth, 480);
  lv_obj_set_style_bg_color(sidebar, lv_color_hex(kColorScreen), 0);
  lv_obj_set_style_bg_opa(sidebar, LV_OPA_COVER, 0);
  lv_obj_clear_flag(sidebar, LV_OBJ_FLAG_SCROLLABLE);

  s_status_strip = lv_obj_create(sidebar);
  lv_obj_remove_style_all(s_status_strip);
  lv_obj_set_pos(s_status_strip, 4, 4);
  lv_obj_set_size(s_status_strip, 312, 28);
  lv_obj_set_style_radius(s_status_strip, 6, 0);
  lv_obj_set_style_bg_color(s_status_strip, lv_color_hex(kColorScreen), 0);
  lv_obj_set_style_bg_opa(s_status_strip, LV_OPA_COVER, 0);
  lv_obj_clear_flag(s_status_strip, LV_OBJ_FLAG_SCROLLABLE);
  s_status_left = addLabel(s_status_strip, "Starting...", &lv_font_montserrat_14);
  lv_obj_align(s_status_left, LV_ALIGN_LEFT_MID, 8, 0);
  s_status_right = addLabel(s_status_strip, "", &lv_font_montserrat_14, kColorMuted);
  lv_obj_align(s_status_right, LV_ALIGN_RIGHT_MID, -8, 0);

  constexpr int kCardInset = 12;
  constexpr int kCardTextWidth = 312 - 2 * kCardInset - 2;
  s_card = lv_obj_create(sidebar);
  lv_obj_set_pos(s_card, 4, 36);
  lv_obj_set_size(s_card, 312, 176);
  setPanelStyle(s_card, kColorPanelAlt);
  lv_obj_set_style_pad_all(s_card, 0, 0);
  lv_obj_clear_flag(s_card, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(s_card, cardClicked, LV_EVENT_CLICKED, nullptr);
  s_card_caption = addLabel(s_card, "", &lv_font_montserrat_12, kColorAccent);
  lv_obj_set_pos(s_card_caption, kCardInset, 6);
  s_card_badge = addLabel(s_card, "", &lv_font_montserrat_12, kColorMuted);
  lv_obj_align(s_card_badge, LV_ALIGN_TOP_RIGHT, -kCardInset, 6);
  s_card_title = addLabel(s_card, "", &lv_font_montserrat_28);
  lv_obj_set_pos(s_card_title, kCardInset, 20);
  s_card_type = addLabel(s_card, "", &lv_font_montserrat_20, kColorMuted);
  lv_obj_align(s_card_type, LV_ALIGN_TOP_RIGHT, -kCardInset, 26);
  // LV_LABEL_LONG_DOT cuts text to the label's size, so both get one fixed.
  s_card_subtitle = addLabel(s_card, "", &lv_font_montserrat_14, kColorMuted);
  lv_obj_set_pos(s_card_subtitle, kCardInset, 54);
  lv_obj_set_size(s_card_subtitle, kCardTextWidth, 18);
  lv_label_set_long_mode(s_card_subtitle, LV_LABEL_LONG_DOT);
  s_card_route = addLabel(s_card, "", &lv_font_montserrat_20, kColorAccent);
  lv_obj_set_pos(s_card_route, kCardInset, 75);
  s_card_hint = addLabel(s_card, "", &lv_font_montserrat_12, kColorMuted);
  lv_obj_set_pos(s_card_hint, kCardInset, 80);
  lv_obj_set_size(s_card_hint, kCardTextWidth, 16);
  lv_label_set_long_mode(s_card_hint, LV_LABEL_LONG_DOT);
  s_card_altitude = addLabel(s_card, "", &lv_font_montserrat_24);
  lv_obj_set_pos(s_card_altitude, kCardInset, 101);
  s_card_speed = addLabel(s_card, "", &lv_font_montserrat_18);
  lv_obj_align(s_card_speed, LV_ALIGN_TOP_RIGHT, -kCardInset, 106);
  s_card_position = addLabel(s_card, "", &lv_font_montserrat_16);
  lv_obj_set_pos(s_card_position, kCardInset, 132);
  s_card_approach = addLabel(s_card, "", &lv_font_montserrat_14, kColorMuted);
  lv_obj_set_pos(s_card_approach, kCardInset, 153);

  constexpr int kRowHeight = 36;
  lv_obj_t* list_panel = lv_obj_create(sidebar);
  lv_obj_set_pos(list_panel, 4, 216);
  lv_obj_set_size(list_panel, 312, 200);
  setPanelStyle(list_panel);
  lv_obj_set_style_pad_all(list_panel, 0, 0);
  lv_obj_clear_flag(list_panel, LV_OBJ_FLAG_SCROLLABLE);
  for (size_t row = 0; row < kTrafficRows; ++row) {
    lv_obj_t* button = lv_btn_create(list_panel);
    s_traffic_buttons[row] = button;
    lv_obj_set_pos(button, 2, 2 + static_cast<int>(row) * kRowHeight);
    lv_obj_set_size(button, 306, kRowHeight - 1);
    lv_obj_set_style_bg_color(button,
                              lv_color_hex(row % 2 ? kColorPanelAlt : kColorPanel), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kColorAccent), LV_STATE_PRESSED);
    lv_obj_set_style_radius(button, 4, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    // A bar in the aircraft's altitude colour ties the row to its symbol.
    lv_obj_t* bar = lv_obj_create(button);
    s_traffic_bars[row] = bar;
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 4, 21);
    lv_obj_align(bar, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_style_radius(bar, 2, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    s_traffic_names[row] = addLabel(button, "", &lv_font_montserrat_16);
    lv_obj_align(s_traffic_names[row], LV_ALIGN_LEFT_MID, 18, 0);
    s_traffic_types[row] = addLabel(button, "", &lv_font_montserrat_12, kColorMuted);
    lv_obj_align(s_traffic_types[row], LV_ALIGN_LEFT_MID, 118, 1);
    s_traffic_altitudes[row] = addLabel(button, "", &lv_font_montserrat_16);
    lv_obj_align(s_traffic_altitudes[row], LV_ALIGN_RIGHT_MID, -74, 0);
    s_traffic_distances[row] = addLabel(button, "", &lv_font_montserrat_14, kColorMuted);
    lv_obj_align(s_traffic_distances[row], LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_add_event_cb(button, trafficClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(row));
    lv_obj_add_flag(button, LV_OBJ_FLAG_HIDDEN);
  }
  s_traffic_more = addLabel(list_panel, "", &lv_font_montserrat_12, kColorMuted);
  lv_obj_align(s_traffic_more, LV_ALIGN_BOTTOM_MID, 0, -2);

  lv_obj_t* zoom_out = addButton(sidebar, 4, 420, 58, 56, LV_SYMBOL_MINUS,
                                 rangePreviousClicked);
  lv_obj_t* range_panel = lv_obj_create(sidebar);
  lv_obj_set_pos(range_panel, 66, 420);
  lv_obj_set_size(range_panel, 104, 56);
  setPanelStyle(range_panel, kColorPanelAlt);
  s_range_label = addLabel(range_panel, "10km", &lv_font_montserrat_18);
  lv_obj_center(s_range_label);
  lv_obj_t* zoom_in = addButton(sidebar, 174, 420, 58, 56, LV_SYMBOL_PLUS,
                                rangeNextClicked);
  lv_obj_t* settings_button =
      addButton(sidebar, 236, 420, 80, 56, LV_SYMBOL_SETTINGS, openSettings);
  for (lv_obj_t* button : {zoom_out, zoom_in, settings_button}) {
    lv_obj_set_style_text_font(lv_obj_get_child(button, 0), &lv_font_montserrat_20, 0);
  }

  s_overlay_scrim = lv_obj_create(s_main_screen);
  lv_obj_remove_style_all(s_overlay_scrim);
  lv_obj_set_pos(s_overlay_scrim, 0, 0);
  lv_obj_set_size(s_overlay_scrim, display::kWidth, display::kHeight);
  lv_obj_set_style_bg_color(s_overlay_scrim, lv_color_hex(kColorScreen), 0);
  lv_obj_set_style_bg_opa(s_overlay_scrim, LV_OPA_COVER, 0);
  lv_obj_add_flag(s_overlay_scrim, LV_OBJ_FLAG_HIDDEN);
  createSettingsOverlay();
  createAirportPickerOverlay();
}

void createStatusScreen() {
  s_status_screen = lv_obj_create(nullptr);
  lv_obj_remove_style_all(s_status_screen);
  lv_obj_set_style_bg_color(s_status_screen, lv_color_hex(kColorScreen), 0);
  lv_obj_set_style_bg_opa(s_status_screen, LV_OPA_COVER, 0);
  s_status_title = addLabel(s_status_screen, "PLANE RADAR", &lv_font_montserrat_28);
  lv_obj_align(s_status_title, LV_ALIGN_CENTER, 0, -45);
  s_status_detail = addLabel(s_status_screen, "Starting...", &lv_font_montserrat_18,
                             kColorMuted);
  lv_obj_set_width(s_status_detail, 700);
  lv_obj_set_style_text_align(s_status_detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(s_status_detail, LV_ALIGN_CENTER, 0, 25);
}

}  // namespace

bool init() {
  if (!display::ready()) {
    return false;
  }
  createMainScreen();
  if (s_canvas_buffer == nullptr || s_backdrop_buffer == nullptr) {
    return false;
  }
  createStatusScreen();
  s_initialized = true;
  showStatus("PLANE RADAR", "Starting services...");
  return true;
}

void showRadar() {
  if (!s_initialized) {
    return;
  }
  refreshData();
  refreshStatus();
  lv_scr_load(s_main_screen);
}

void showStatus(const char* title, const char* detail, bool warning) {
  if (!s_initialized) {
    return;
  }
  lv_label_set_text(s_status_title, title == nullptr ? "PLANE RADAR" : title);
  lv_label_set_text(s_status_detail, detail == nullptr ? "" : detail);
  lv_obj_set_style_text_color(s_status_title,
                              lv_color_hex(warning ? kColorWarning : kColorText), 0);
  lv_scr_load(s_status_screen);
  display::loop();
}

void refreshData() {
  if (!s_initialized) {
    return;
  }
  updateRangeLabel();
  updateTrafficList();
  updateSelectedPanel();
  redrawRadar();
  // With the rest, so that everything that changes does so in one go.
  s_status_rssi = WiFi.RSSI();
  refreshStatus();
}

void refreshStatus() {
  if (!s_initialized) {
    return;
  }
  updateStatusStrip();
  // A code that nobody scanned stops working after a few minutes. Take it
  // down then, rather than leave something on screen that cannot work.
  if (!lv_obj_has_flag(s_locate_panel, LV_OBJ_FLAG_HIDDEN) &&
      phone_location::expired()) {
    showNearbyAirports(nullptr);
    lv_label_set_text(s_airport_picker_heading,
                      "FIND ME TIMED OUT  -  tap Find me for a new code");
  }
}

void phoneLocationReceived() {
  if (!s_initialized) {
    return;
  }
  // The same as choosing "Use location": the saved position is already new.
  useSavedLocationClicked(nullptr);
}

void refreshRoute() {
  if (!s_initialized) {
    return;
  }
  updateSelectedPanel();
}

void refreshMotion() {
  if (!s_initialized) {
    return;
  }
  // The radar is mostly covered while a panel is open; leave it alone.
  if (!lv_obj_has_flag(s_settings_overlay, LV_OBJ_FLAG_HIDDEN) ||
      !lv_obj_has_flag(s_airport_overlay, LV_OBJ_FLAG_HIDDEN)) {
    return;
  }
  if (trafficSignature() != s_traffic_signature) {
    redrawRadar();
  }
}

unsigned long motionFrameMs() {
  // About a pixel and a half per frame for a 300-knot target: four frames a
  // second zoomed right in, one a second from 25 km out.
  const float interval = ::ui::radar::rangeCurrent().outer_km * 46.0f;
  const unsigned long paced =
      static_cast<unsigned long>(std::max(250.0f, std::min(1000.0f, interval)));
  // A crowded radar takes longer to repaint; never spend more than about a
  // quarter of the time doing it, so touch stays responsive.
  return std::max<unsigned long>(paced, std::min<uint32_t>(s_traffic_cost_ms * 4, 3000));
}

void setRadarChangedFn(RadarChangedFn callback) {
  s_radar_changed_fn = callback;
}

}  // namespace crowpanel::ui
