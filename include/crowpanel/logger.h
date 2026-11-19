#pragma once

namespace crowpanel::logger {

/** Initialize the CrowPanel microSD slot. Safe to call without a card inserted. */
bool init();
bool available();
const char* status();

/** Append the current ADS-B snapshot when logging is enabled in settings. */
void logSnapshot();

}  // namespace crowpanel::logger
