#pragma once

#include <cstddef>

namespace crowpanel::psram_code {

/**
 * Copy the firmware's code and constant data from flash into PSRAM and run
 * from the copy, so that nothing reads flash while the panel is being
 * scanned. Call once, early in setup(), after Serial is up.
 *
 * Returns the number of 64 KB pages moved, or 0 if it was not done, in which
 * case the firmware carries on from flash as usual.
 */
size_t relocate();

}  // namespace crowpanel::psram_code
