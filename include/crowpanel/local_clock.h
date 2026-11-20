#pragma once

#include <cstddef>

namespace crowpanel::local_clock {

/** Room for the longest text format() writes: "12:34 PM" and a ten-letter zone. */
constexpr size_t kTextLength = 24;

/**
 * The time-zone rule the settings ask for, as a POSIX TZ string: UTC, the
 * zone chosen by name, or the one in force where the radar is centred.
 */
const char* rule();

/**
 * Writes the time for the status strip, in the zone and style the settings
 * ask for: "14:05 EDT", "2:05 PM EDT", or "14:05Z" for UTC. Writes an empty
 * string until the clock has been set from the network.
 */
void format(char* text, size_t length);

}  // namespace crowpanel::local_clock
