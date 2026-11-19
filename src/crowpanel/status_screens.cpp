#include "ui/status_screens.h"

#include <cstdio>

#include "config.h"
#include "crowpanel/display.h"
#include "crowpanel/ui.h"

namespace {

char s_connecting_detail[96] = {};

}  // namespace

void statusScreenConnectingBegin(const char* ssid) {
  snprintf(s_connecting_detail, sizeof(s_connecting_detail),
           "Connecting to\n%s", ssid == nullptr ? "network" : ssid);
  crowpanel::ui::showStatus("CONNECTING", s_connecting_detail);
}

void statusScreenConnectingTick() { crowpanel::display::loop(); }

void statusScreenPortal() {
  char detail[192];
  snprintf(detail, sizeof(detail),
           "Join Wi-Fi network:  %s\nThen open:  %s\nor  http://%s",
           config::kPortalApName, config::kPortalHostUrl, config::kPortalIp);
  crowpanel::ui::showStatus("WI-FI SETUP", detail, true);
}

void statusScreenConnectFailed() {
  crowpanel::ui::showStatus(
      "CONNECTION FAILED",
      "Check the Wi-Fi password and signal.\nRestart to try again; Wi-Fi can also be reset from Settings.",
      true);
}

void statusScreenWifiReset() {
  crowpanel::ui::showStatus("WI-FI RESET", "Clearing settings and restarting...",
                            true);
}
