#pragma once

#include <cstdint>

namespace services::route {

struct Info {
  char callsign[9];
  /** Airport codes, IATA where the source has one. Empty if not found. */
  char origin[5];
  char midpoint[5];
  char destination[5];
  char airline[32];
  /** The lookup found both an origin and a destination. */
  bool route_found;
};

/**
 * Start the background task that does the lookups. Call once from setup().
 * Lookups run there, with their own connection to ADSBDB's public route
 * endpoint, so none of this ever holds up the caller.
 */
void begin();

/**
 * Ask for a callsign's route. Returns at once. If the answer is already known
 * find() has it; otherwise it is fetched in the background and updated()
 * reports when it has arrived. Asking again for the same callsign costs
 * nothing, so it is fine to call this every time the callsign is drawn.
 *
 * `lat` and `lon` are where the aircraft is. A route that does not pass
 * anywhere near there belongs to another flight using the same callsign and
 * is treated as not found.
 */
void request(const char* callsign, double lat, double lon);

/**
 * Copy what is known about a callsign's route into `info`. False while
 * nothing is known: it was never asked for, or the answer is not in yet.
 * True with `route_found` false means it was looked up and has no route.
 */
bool find(const char* callsign, Info* info);

/** True once after each lookup that has finished since the last call. */
bool updated();

/**
 * Stop looking routes up, or carry on again. A lookup holds a TLS session's
 * worth of memory while it runs, which something else may need. Pausing waits
 * for a lookup already under way to finish, a few seconds at most, so that
 * the memory is free on return. Requests made while paused are dropped.
 */
void pause(bool paused);

}  // namespace services::route
