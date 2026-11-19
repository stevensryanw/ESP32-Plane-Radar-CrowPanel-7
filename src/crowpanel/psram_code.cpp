#include "crowpanel/psram_code.h"

#include <Arduino.h>
#include <esp32s3/rom/cache.h>
#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_spi_flash.h>
#include <soc/cache_memory.h>
#include <soc/soc.h>

#include <cstring>

// Why this exists: flash, PSRAM and the panel's pixel stream share one SPI
// bus. The stream is read from PSRAM as the panel scans and cannot wait.
// PSRAM reads by the CPU fit in around it; flash reads do not, and each time
// the CPU fetches code or constants from flash while drawing there is a
// chance the stream runs dry and the rest of that frame comes out shifted.
// See "Checking the pixel stream" in docs/CROWPANEL.md.
//
// ESP-IDF can run an application from PSRAM instead (its options
// CONFIG_SPIRAM_FETCH_INSTRUCTIONS and CONFIG_SPIRAM_RODATA), but the Arduino
// core is built without them. This does the same thing from inside the
// application: copy every flash page the application is mapped from into
// PSRAM, then point the memory map at the copies.
//
// It leans on how the ESP32-S3 maps external memory, which the framework does
// not expose as an API: one table of 512 entries, one per 64 KB page, each
// naming a flash page or a PSRAM page. The first entries serve the
// instruction bus (from 0x42000000) and the rest the data bus (entry n at
// 0x3C000000 + n pages); neither bus may use the other's entries.

extern "C" {
// From the linker script: the address ranges the application's code and
// constant data occupy.
extern const uint8_t _instruction_reserved_start[];
extern const uint8_t _instruction_reserved_end[];
extern const uint8_t _rodata_reserved_start[];
extern const uint8_t _rodata_reserved_end[];
}

namespace crowpanel::psram_code {
namespace {

constexpr uint32_t kPageBytes = MMU_PAGE_SIZE;
constexpr uint32_t kPageNumberMask = MMU_INVALID - 1;  // The bits below the flags.
constexpr uint32_t kMostPages = 64;  // 4 MB, which is all the flash there is.

// Read while the flash cache is off, so in internal RAM, not constant data.
uint16_t s_entries[kMostPages];
uint32_t s_entry_count = 0;

volatile uint32_t* mapTable() {
  return reinterpret_cast<volatile uint32_t*>(DR_REG_MMU_TABLE);
}

bool mapsFlash(uint32_t entry) {
  return (entry & (MMU_INVALID | MMU_TYPE)) == MMU_ACCESS_FLASH;
}

bool mapsPsram(uint32_t entry) {
  return (entry & (MMU_INVALID | MMU_TYPE)) == MMU_ACCESS_SPIRAM;
}

// Notes the table entries behind an address range on one of the two buses.
void addRange(const uint8_t* start, const uint8_t* end, uint32_t bus_start) {
  const uint32_t first = reinterpret_cast<uint32_t>(start);
  const uint32_t last = reinterpret_cast<uint32_t>(end) - 1;
  if (last < first) {
    return;
  }
  for (uint32_t entry = (first - bus_start) / kPageBytes;
       entry <= (last - bus_start) / kPageBytes; ++entry) {
    bool known = false;
    for (uint32_t i = 0; i < s_entry_count; ++i) {
      known = known || s_entries[i] == entry;
    }
    if (!known && s_entry_count < kMostPages && mapsFlash(mapTable()[entry])) {
      s_entries[s_entry_count++] = static_cast<uint16_t>(entry);
    }
  }
}

// The moment of the switch. The flash guard parks the other core and turns
// the cache off, exactly as for a flash write, so nothing is fetched through
// the table while it changes. Until the guard ends, this function and
// everything it touches has to be in internal RAM, so it must not be inlined
// into its caller, which is not.
void IRAM_ATTR __attribute__((noinline)) switchToCopies(uint32_t first_copy_entry) {
  const spi_flash_guard_funcs_t* guard = spi_flash_guard_get();
  volatile uint32_t* table = reinterpret_cast<volatile uint32_t*>(DR_REG_MMU_TABLE);
  guard->start();
  for (uint32_t i = 0; i < s_entry_count; ++i) {
    table[s_entries[i]] = table[first_copy_entry + i];
  }
  guard->end();
}

}  // namespace

size_t relocate() {
  if (!psramFound()) {
    return 0;
  }
  const uint32_t started_ms = millis();
  // The OTA code works out which partition is running from the memory map,
  // once, and remembers the answer. Have it do that while the map still
  // leads back to flash.
  esp_ota_get_running_partition();

  s_entry_count = 0;
  addRange(_instruction_reserved_start, _instruction_reserved_end,
           IRAM0_CACHE_ADDRESS_LOW);
  addRange(_rodata_reserved_start, _rodata_reserved_end, DRAM0_CACHE_ADDRESS_LOW);
  if (s_entry_count == 0) {
    Serial.println("psram_code: nothing mapped from flash");
    return 0;
  }

  const size_t bytes = static_cast<size_t>(s_entry_count) * kPageBytes;
  uint8_t* copies = static_cast<uint8_t*>(
      heap_caps_aligned_alloc(kPageBytes, bytes, MALLOC_CAP_SPIRAM));
  if (copies == nullptr) {
    Serial.printf("psram_code: no room for %u pages\n",
                  static_cast<unsigned>(s_entry_count));
    return 0;
  }
  // The allocation is page-aligned, so it sits behind a run of whole table
  // entries of its own, which are what the application's entries will become.
  const uint32_t first_copy_entry =
      (reinterpret_cast<uint32_t>(copies) - DRAM0_CACHE_ADDRESS_LOW) / kPageBytes;
  for (uint32_t i = 0; i < s_entry_count; ++i) {
    if (!mapsPsram(mapTable()[first_copy_entry + i])) {
      Serial.println("psram_code: allocation is not PSRAM pages");
      heap_caps_free(copies);
      return 0;
    }
  }

  // Read each page from the flash chip by its address there. Code pages
  // cannot be read through the data bus, which may not use their entries.
  for (uint32_t i = 0; i < s_entry_count; ++i) {
    const uint32_t flash_page = mapTable()[s_entries[i]] & kPageNumberMask;
    if (spi_flash_read(flash_page * kPageBytes,
                       copies + static_cast<size_t>(i) * kPageBytes,
                       kPageBytes) != ESP_OK) {
      Serial.println("psram_code: could not read flash");
      heap_caps_free(copies);
      return 0;
    }
  }
  // Constant data can be read both ways. If the two disagree, the table does
  // not mean what this code takes it to mean, so leave everything as it is.
  const uint32_t first_data_entry =
      (reinterpret_cast<uint32_t>(_rodata_reserved_start) - DRAM0_CACHE_ADDRESS_LOW) /
      kPageBytes;
  for (uint32_t i = 0; i < s_entry_count; ++i) {
    if (s_entries[i] < first_data_entry) {
      continue;
    }
    const void* mapped = reinterpret_cast<const void*>(
        DRAM0_CACHE_ADDRESS_LOW + static_cast<uint32_t>(s_entries[i]) * kPageBytes);
    if (memcmp(copies + static_cast<size_t>(i) * kPageBytes, mapped, kPageBytes) != 0) {
      Serial.println("psram_code: flash pages are not where the map says");
      heap_caps_free(copies);
      return 0;
    }
  }
  // The copies are still partly in the data cache. Code is fetched through
  // the other cache, straight from PSRAM, so they must be written out first.
  Cache_WriteBack_Addr(reinterpret_cast<uint32_t>(copies), bytes);

  switchToCopies(first_copy_entry);

  for (uint32_t i = 0; i < s_entry_count; ++i) {
    if (!mapsPsram(mapTable()[s_entries[i]])) {
      Serial.println("psram_code: the memory map did not change");
      return 0;
    }
  }
  Serial.printf("psram_code: running from PSRAM, %u pages (%u KB) copied in %u ms\n",
                static_cast<unsigned>(s_entry_count),
                static_cast<unsigned>(bytes / 1024),
                static_cast<unsigned>(millis() - started_ms));
  return s_entry_count;
}

}  // namespace crowpanel::psram_code
