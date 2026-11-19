#pragma once

#include <cstddef>

// "Find me": the radar's location, set from a phone's GPS.
//
// A phone browser gives its position only to a page loaded over HTTPS. So
// while "Find me" is on screen the radar runs a small HTTPS server of its
// own and shows a QR code for it. The page it serves asks the phone where it
// is and sends the answer back.
//
// HTTPS needs a certificate, and no authority will sign one for an address on
// a home network, so the radar makes its own. The phone cannot check a
// certificate like that and warns before it opens the page. That warning is
// the price of not depending on a server anywhere else.
namespace crowpanel::phone_location {

enum class Start {
  Ready,   // `url` holds the address to show as a QR code.
  NoWifi,  // There is no network for a phone to reach the radar on.
  Failed,  // The certificate or the server could not be set up.
};

/**
 * Start the server and accept one position, for ten minutes or until end().
 * The address includes a key made up for this occasion, and a position
 * arriving without it is refused, so this server cannot be used by anything
 * else on the network to move the radar.
 *
 * Blocks for a moment: a third of a second the first time, while the
 * certificate is made, and for as long as a route lookup already under way
 * takes to finish, since there is no memory for both.
 */
Start begin(char* url, size_t length);

/** Stop the server and let route lookups carry on. Safe if not running. */
void end();

/**
 * True if begin()'s ten minutes have passed and no phone answered. The code
 * on screen no longer works, so the screen showing it should close.
 */
bool expired();

/**
 * Call on every pass of the main loop. A position that has arrived is saved
 * here, on the loop's own task, and this returns true once for it. The caller
 * then centres the radar on the saved location.
 */
bool received();

}  // namespace crowpanel::phone_location
