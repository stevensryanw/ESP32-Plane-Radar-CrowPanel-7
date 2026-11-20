#include <Arduino.h>
#include <WiFi.h>

#include <algorithm>
#include <ctime>

#include "config.h"
#include "crowpanel/airport_catalog.h"
#include "crowpanel/airports.h"
#include "crowpanel/display.h"
#include "crowpanel/local_clock.h"
#include "crowpanel/logger.h"
#include "crowpanel/model.h"
#include "crowpanel/phone_location.h"
#include "crowpanel/psram_code.h"
#include "crowpanel/settings.h"
#include "crowpanel/ui.h"
#include "crowpanel/adsb_client.h"
#include "services/radar_location.h"
#include "services/route_lookup.h"
#include "services/wifi_setup.h"
#include "ui/radar_range.h"
#include "ui/status_screens.h"

namespace {

bool s_radar_visible = false;
bool s_immediate_fetch_pending = false;
unsigned long s_wifi_down_since = 0;
unsigned long s_last_reconnect_ms = 0;
unsigned long s_last_fetch_ms = 0;
unsigned long s_last_status_ms = 0;
unsigned long s_last_motion_ms = 0;

// adsb.fi allows one request per second; stay just under it.
constexpr unsigned long kMinFetchIntervalMs = 1100;

// Aircraft are redrawn between snapshots so that they glide along their
// track, but only once the firmware is running from PSRAM. While it runs from
// flash every repaint is a chance for the panel's picture to slip sideways
// for a frame (see "Checking the pixel stream" in docs/CROWPANEL.md), so then
// the screen changes only when data arrives.
bool s_glide_between_snapshots = false;

void serviceMotion() {
  const unsigned long now = millis();
  if (s_glide_between_snapshots && s_radar_visible &&
      now - s_last_motion_ms >= crowpanel::ui::motionFrameMs()) {
    s_last_motion_ms = now;
    const crowpanel::display::ActivityScope glide(
        crowpanel::display::kActivityGlide);
    crowpanel::model::advance(now);
    crowpanel::ui::refreshMotion();
  }
}

void serviceDuringNetwork() {
  const crowpanel::display::ActivityScope waiting(
      crowpanel::display::kActivityNetworkWait);
  crowpanel::display::loop();
  wifiLoop();
  serviceMotion();
}

void requestImmediateFetch() { s_immediate_fetch_pending = true; }

void chooseDefaultAirport() {
  if (!services::location::needsDefaultAirport()) {
    return;
  }
  const int index = data::airport_catalog::bestNearby(
      services::location::userLat(), services::location::userLon());
  if (index < 0) {
    services::location::useUserLocation();
    return;
  }
  const auto& airport = data::large_airports::kAirports[index];
  services::location::selectAirport(airport.ident, airport.lat_e7 * 1e-7,
                                    airport.lon_e7 * 1e-7);
}

// Called each time Wi-Fi comes up. It is given the zone already in force
// because configTime() would put the clock back on UTC, and setting a zone
// again and again costs a little memory each time.
void startClock() {
  configTzTime(crowpanel::local_clock::rule(), "pool.ntp.org", "time.nist.gov");
}

void showRadar() {
  crowpanel::ui::showRadar();
  s_radar_visible = true;
}

void fetchAircraft() {
  s_last_fetch_ms = millis();
  const float fetch_km = ::ui::radar::fetchRadiusKm();
  crowpanel::display::beginActivity(crowpanel::display::kActivityFetch);
  const bool fetched = services::adsb::fetchUpdate(
      services::location::lat(), services::location::lon(), fetch_km);
  crowpanel::display::endActivity(crowpanel::display::kActivityFetch);
  if (fetched) {
    const crowpanel::display::ActivityScope redraw(
        crowpanel::display::kActivityDataRedraw);
    crowpanel::model::update();
    crowpanel::logger::logSnapshot();
    crowpanel::ui::refreshData();
  } else {
    crowpanel::ui::refreshStatus();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Plane Radar - CrowPanel 7-inch");

  // Before anything is drawn: from here on the CPU no longer competes with
  // the panel for the flash chip.
  s_glide_between_snapshots = crowpanel::psram_code::relocate() > 0;

  if (!crowpanel::display::init()) {
    Serial.printf("Display startup failed: %s\n", crowpanel::display::lastError());
    return;
  }

  services::location::init();
  ::ui::radar::rangeInit();
  crowpanel::settings::init();
  crowpanel::display::setBrightness(
      crowpanel::settings::get().brightness_percent);

  if (!crowpanel::ui::init()) {
    Serial.println("CrowPanel UI allocation failed");
    return;
  }

  // Build the airport latitude index now, while a status screen is up: it
  // reads the whole flash table once, which displaces a frame on the panel.
  data::airport_catalog::latitudeCells();

  crowpanel::logger::init();
  bootButtonInit();
  services::adsb::setPollFn(serviceDuringNetwork);
  crowpanel::ui::setRadarChangedFn(requestImmediateFetch);
  services::route::begin();

  if (wifiShowsSetupScreenOnBoot()) {
    statusScreenPortal();
  }
  if (wifiSetupConnect()) {
    chooseDefaultAirport();
    startClock();
    showRadar();
    fetchAircraft();
  }
}

void loop() {
  crowpanel::display::loop();
  wifiLoop();

  if (!crowpanel::display::ready()) {
    delay(100);
    return;
  }

  const unsigned long now = millis();
  if (WiFi.status() != WL_CONNECTED) {
    if (s_wifi_down_since == 0) {
      s_wifi_down_since = now;
    }
    if (s_radar_visible) {
      crowpanel::ui::refreshStatus();
    }
    const unsigned long down_ms = now - s_wifi_down_since;
    if (down_ms >= config::kWifiDownGraceMs &&
        now - s_last_reconnect_ms >= config::kWifiReconnectIntervalMs) {
      s_last_reconnect_ms = now;
      s_radar_visible = false;
      if (wifiReconnect()) {
        s_wifi_down_since = 0;
        startClock();
        showRadar();
        fetchAircraft();
      } else {
        showRadar();
      }
    }
  } else {
    s_wifi_down_since = 0;
    if (!s_radar_visible) {
      showRadar();
    }
    const unsigned long interval = std::max(
        static_cast<unsigned long>(crowpanel::settings::get().refresh_seconds) *
            1000UL,
        kMinFetchIntervalMs);
    if (now - s_last_fetch_ms >= interval) {
      fetchAircraft();
    }
  }

  if (s_immediate_fetch_pending && WiFi.status() == WL_CONNECTED) {
    s_immediate_fetch_pending = false;
    crowpanel::model::reset();
    fetchAircraft();
  }

  serviceMotion();

  if (now - s_last_status_ms >= 1000UL) {
    s_last_status_ms = now;
    crowpanel::ui::refreshStatus();
  }
  if (crowpanel::phone_location::received()) {
    crowpanel::ui::phoneLocationReceived();
  }
  // Routes are looked up in the background; show one as soon as it is in.
  if (services::route::updated()) {
    crowpanel::ui::refreshRoute();
  }
  delay(5);
}
