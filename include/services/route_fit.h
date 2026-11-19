#pragma once

#include <algorithm>
#include <cmath>

namespace services::route {

struct Place {
  double lat;
  double lon;
};

/** Great-circle distance in kilometres. */
inline double distanceKm(const Place& a, const Place& b) {
  constexpr double kRadians = 0.017453292519943295;
  constexpr double kEarthRadiusKm = 6371.0;
  const double lat_a = a.lat * kRadians;
  const double lat_b = b.lat * kRadians;
  const double half_lat = (lat_b - lat_a) / 2.0;
  const double half_lon = (b.lon - a.lon) * kRadians / 2.0;
  const double h = std::sin(half_lat) * std::sin(half_lat) +
                   std::cos(lat_a) * std::cos(lat_b) * std::sin(half_lon) *
                       std::sin(half_lon);
  return 2.0 * kEarthRadiusKm * std::asin(std::sqrt(std::min(1.0, h)));
}

/**
 * Whether an aircraft at `aircraft` could be flying the leg from `from` to
 * `to`: going via where it is must not be much longer than going direct.
 *
 * Route databases are keyed by callsign, and callsigns are reused, so a
 * lookup can return a route on the other side of the continent. This is the
 * check that catches those. It allows for holds, vectors and a curved track:
 * a detour of 150 km, or 8% of the leg if that is more. (A quarter of the leg
 * was too generous: on a coast-to-coast route it accepted an aircraft 400 km
 * beyond one end of it.)
 */
inline bool onLeg(const Place& aircraft, const Place& from, const Place& to) {
  const double direct = distanceKm(from, to);
  const double via = distanceKm(from, aircraft) + distanceKm(aircraft, to);
  return via - direct <= std::max(150.0, direct * 0.08);
}

}  // namespace services::route
