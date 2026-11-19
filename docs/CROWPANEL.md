# CrowPanel 7-inch firmware

Target hardware: **Elecrow CrowPanel Basic 7-inch HMI**, model DIS08070H, with
ESP32-S3-WROOM-1-N4R8, 800×480 RGB TFT, GT911 capacitive touch, and 8 MB OPI
PSRAM. This is not the newer CrowPanel Advance model.

## Features

The screen is a 480x480 radar and a 320-pixel sidebar.

Radar:

- Aircraft are coloured by altitude, warm low to cool high, with a key in the
  corner; red is kept for emergencies. Grey means on the ground
- Symbols by type (fixed-wing, helicopter, glider, balloon, drone, ground) and
  sized by weight class, so a light aircraft and a heavy do not look alike
- Each has a tag with its callsign and altitude (flight levels above
  18,000 ft) and an arrow when climbing or descending. Tags look for a free
  spot beside the symbol and are left off rather than overlapped
- A thin leader shows where each aircraft will be in a minute, and a trail
  where it has been
- Aircraft glide between snapshots: each one is projected along its reported
  track at its ground speed for the age of the report, so nothing jumps when
  new data arrives
- The outer ring is the stated range (5, 10, 25, or 50 km) and every ring is
  labelled; bearing ticks every 10 degrees
- Runways and airports for more than 15,000 fields: worldwide large airports
  plus U.S. small, medium, and large airports with runway geometry
- Traffic beyond the outer ring is marked on the rim in its direction

Sidebar:

- A card describes one aircraft in type large enough to read across a room:
  callsign, type, airline (from the callsign, for about 110 operators),
  aircraft description, altitude, speed, distance and compass direction, and
  when and how near it will pass ("Passes 1.3 km S in 2:31")
- With nothing pinned the card follows the nearest aircraft in the air. Tap an
  aircraft on the radar or in the list to pin it; tap the card or empty radar
  to let go. Tap an airport for its name, distance and runways
- The card shows where the flight is from and to ("LHR > JFK"). Routes are
  looked up in the background, without pausing the screen: at once for a
  pinned aircraft, and for the nearest one once a numbered flight has been
  nearest for a few seconds. With no route it says how the aircraft is flying
  instead ("Heading W, climbing 700 ft/min")
- The card's border turns amber when its aircraft is within the home-alert
  distance, and red for an emergency squawk
- The next five aircraft, nearest first, one line each: altitude colour,
  callsign, type, altitude, distance
- A status strip with the aircraft count, Wi-Fi and UTC time; it turns into a
  warning for lost Wi-Fi, stale data, or an emergency squawk

Settings (the gear) and data:

- 2, 3, 5, or 10 second data refresh (2 s by default; adsb.fi publishes a new
  snapshot every 2 s)
- Altitude and ground-aircraft filters; labels, trails and runways on or off
- Search for a home airport by code, name, or city from the touch keyboard,
  list the nearest ones, or centre on the exact saved coordinates
- **Find me** sets those coordinates from a phone's GPS: scan the code on the
  screen and tap once (see "Find me")
- Night, green-scope, and high-contrast radar themes; brightness; km or miles
- Military aircraft are named in amber
- Optional CSV logging to `/plane-radar.csv` on microSD, with NTP timestamps
- Wi-Fi, data-age, and HTTP-failure status, and the last good picture is kept

The home alert and emergency highlights are for hobbyist use only. This firmware is not a collision
avoidance, navigation, or flight-safety instrument.

Route lookup is best-effort and keyed by the live callsign. ADSBDB may not have
a route for general-aviation, charter, military, blocked, or recently changed
flights, and because callsigns are reused it sometimes returns another
flight's. A route is therefore shown only when the aircraft is somewhere along
it (`services/route_fit.h`); in testing about half were rejected.
A route can still be for the wrong direction of a flight that goes both ways.

## Build and upload

```bash
pio run -e crowpanel7
pio run -e crowpanel7 -t upload
pio device monitor -b 115200
```

`crowpanel7-debug` is the same firmware with the serial debug commands that
the screenshot and recording scripts below need. Those commands let whoever
holds the USB cable see and drive the screen, so the ordinary build leaves
them out.

The build output is `.pio/build/crowpanel7/firmware.bin`. Use a USB data cable
and a 5 V / 2 A-capable supply. The first boot opens the `PlaneRadar-Setup`
Wi-Fi portal when no credentials are stored.

The CrowPanel does not contain a GPS receiver. **Nearby** and the automatic
home-airport choice use the latitude and longitude saved in that Wi-Fi portal.
Open **Settings (the gear) → Change** beside **Home airport** to search, view nearest airports,
or switch back to the exact saved location. **Find me** replaces the saved
location with a phone's; see the next section. A range or center change
triggers a new ADS-B request immediately so the 50 km view is not left showing
the previous smaller-radius result.

The original round display remains available:

```bash
pio run -e supermini
```

## Find me

**Find me** borrows a phone's GPS. It shows a QR code; scanning it opens a
page served by the radar itself, with one button, and tapping that sends the
phone's position to the radar, which saves it and centres on it. The phone has
to be on the same Wi-Fi as the radar. Nothing outside the radar is involved.

Before it opens the page the phone warns that the connection is not private.
Choose **Show Details**, then **visit this website** (Safari), or
**Advanced**, then **Proceed** (Chrome). The warning is expected, and it
cannot be removed while the page comes from the radar:

- A phone browser gives its position only to a page loaded over HTTPS, so the
  radar has to serve HTTPS.
- HTTPS needs a certificate, and no certificate authority will sign one for
  an address on a home network. The radar makes its own, which the phone has
  no means of checking; hence the warning.

The certificate looks after itself. It is made the first time **Find me** is
opened after the radar starts, which takes about a third of a second. It names
the radar's address, is valid for a year, and is kept only in memory: the
radar makes a new one after a restart, if its address changes, or after 300
days of running. A phone that has accepted the warning may not ask again until
the certificate changes.

What stops anything else on the network from using **Find me** to move the
radar:

- The server runs only while the **Find me** screen is showing.
- The address in the code carries a 64-bit key that the radar makes up each
  time, and a position without it is refused. The key works once, and stops
  working after 10 minutes or when the screen is closed, at which point the
  code is taken down.

What it does not stop: someone who can already intercept traffic on your
network can pretend to be the radar. The phone cannot tell one self-signed
certificate from another, so it would show the same warning, and that device
would receive the position the phone sends. Nothing run from the radar alone
can prevent this; it is the cost of not using a certificate that an authority
has signed. A device that keeps connecting can also keep **Find me** busy,
since it serves one caller at a time.

The page makes one request, to the radar, and stores nothing. The server is
`src/crowpanel/phone_location.cpp` and the page is
`include/crowpanel/phone_location_page.h`.

Three things about that server are deliberate, and each was measured:

- It is not the SDK's `esp_https_server`. That one waits without limit for a
  caller to finish its TLS handshake, so a device that connected and said
  nothing stopped it answering anyone, and stopped it shutting down: Cancel
  froze the screen until the device went away. Here every wait has a limit (a
  silent caller is dropped after 6 s) and ends at once when the screen closes.
- It serves one caller at a time, and route lookups are paused while it runs.
  An open TLS session takes nearly 60 KB of internal RAM, and beside the
  aircraft feed's own session there is room for one. With two allowed, three
  phones at once left 7 KB free; with one, the least seen is 42 KB, and
  callers that arrive together are answered in turn.
- TLS's buffers stay in internal RAM. Moving them to PSRAM
  (`mbedtls_platform_set_calloc_free`) freed 33 KB per session and looked
  like the better answer, but 11 frames slipped in 45 connections with it and
  none without.

## Settings page on the network

As on the round display, the firmware serves its settings page on the home
network once Wi-Fi is connected (`http://plane-radar.local`, or the address
printed on Serial at start-up). Location, units and the runway overlay can be
changed there as well as on the touch screen.

That page is WiFiManager's portal. It has no password, and it includes pages
that upload new firmware and change or erase the Wi-Fi settings, so it is
meant for a network of your own. If the panel is going on a network you do not
trust, add `-DPLANE_RADAR_LAN_PORTAL=0` to the build flags to leave the page
out; every setting is on the touch screen too (location through **Find me** or
the airport picker, units, runways, and **Reset Wi-Fi**).

## Hardware revision and touch

The default build flag is `CROWPANEL_HW_REV=3`, matching current Basic-series
panels. The V3 board resets the LCD/touch lines through its PCA9557 expander.
The firmware probes GT911 addresses `0x5D` and `0x14`; the detected address is
printed on Serial.

If the back sticker says V1.0 or V2.0, change this line in `platformio.ini`:

```ini
-DCROWPANEL_HW_REV=3
```

to the matching revision, rebuild, and test. Older revisions bypass the V3
PCA9557 reset sequence.

If touch coordinates are mirrored or rotated, add one or more of these build
flags with value `1`:

```ini
-DCROWPANEL_TOUCH_SWAP_XY=1
-DCROWPANEL_TOUCH_INVERT_X=1
-DCROWPANEL_TOUCH_INVERT_Y=1
```

## First hardware test

1. Read the hardware revision from the rear sticker before flashing.
2. Power the panel from a known-good 5 V / 2 A source.
3. Flash `crowpanel7`, then open the 115200-baud serial monitor.
4. Confirm Serial reports 8 MB PSRAM, `display ready`, and a GT911 address.
5. Confirm the setup or connecting screen fills the complete 800×480 panel.
6. Tap all four corners. If targeting is mirrored, use the touch flags above.
7. Complete the Wi-Fi portal and confirm aircraft, data age, and signal strength
   appear in the sidebar.
8. Tap an aircraft, an airport, both range buttons, and every Settings control.
9. Temporarily turn off Wi-Fi and confirm the last picture remains with a stale
   or disconnected banner, then restores after reconnection.
10. Insert a FAT32 microSD card, enable logging, wait for an update, and verify
    `/plane-radar.csv` on a computer.

## Screenshots over serial

The USB-C port is a CH340 bridge on UART0, so `Serial` output appears on that
port (`ARDUINO_USB_CDC_ON_BOOT=0`). The `crowpanel7-debug` build
(`-DCROWPANEL_SERIAL_DEBUG=1`) also answers serial commands, which
`scripts/crowpanel_screenshot.py` uses to save the panel's scan-out buffer as
a PNG and to press the screen:

```bash
~/.platformio/penv/bin/python scripts/crowpanel_screenshot.py \
    /dev/cu.usbserial-XXXX screen.png --steps "tap 756 448; wait 2"
```

A screenshot cannot show a frame that the panel drew in the wrong place, so the
`lcd` step prints scan-out health counters instead (and resets them):

```bash
~/.platformio/penv/bin/python scripts/crowpanel_screenshot.py \
    /dev/cu.usbserial-XXXX --steps "lcd; wait 30; lcd"
```

`desync_frames` and `short_frames` count frames where the frame buffer stream
fell behind the panel (`short_frames` catches slips too small for the other),
`longest_update_ms` is the slowest screen update, and `longest_stall_ms` is the
longest the touch screen went unserviced. In normal running both frame
counters should stay at 0. `heap_free` is the internal RAM left and
`heap_low` the least there has been since start-up; `lvgl_used` of
`lvgl_pool` is how full LVGL's own memory pool is.

The ordinary `crowpanel7` build has none of these commands.

## Recording the screen over serial

Stills cannot show flicker or anything that moves wrongly.
`scripts/crowpanel_record.py` has the firmware keep every write to the frame
buffer for a few seconds, with when it happened and where the panel's scan was,
and replays them as a video with one frame per panel refresh (needs `ffmpeg`):

```bash
~/.platformio/penv/bin/python scripts/crowpanel_record.py \
    /dev/cu.usbserial-XXXX panel.mp4 --seconds 10
```

A write that landed while the panel was scanning through it is drawn torn for
that frame, as it would be on the glass, and the script reports how many did.
`--raw FILE` keeps the recording itself and `--replay FILE` renders a saved
one. `--steps "wait 2; tap 640 192"` presses the screen while recording. The
recording stops early when its PSRAM buffer fills, which a busy radar does in
3 or 4 seconds, and takes up to a minute to transfer.

It replays what the firmware drew, so it catches aircraft or labels that
flicker or jump. It cannot show anything the panel does with a correct frame
buffer; the frame counters above and the stress test below cover the known
case, and filming the screen covers the rest.

## Checking the pixel stream

Flash, PSRAM, and the panel's pixel stream share one SPI bus. The stream is
read from PSRAM at twice the pixel clock and cannot wait, and at 15 MHz it has
no headroom. PSRAM reads by the CPU fit in around it. Flash reads do not: each
one is a chance that the stream runs dry for a few microseconds, and then the
rest of that frame comes out shifted a few pixels sideways. A 60 fps phone
video shows it as a doubled image for a frame or two.

Firmware that runs from flash reads it constantly while drawing (code and
fonts), so the firmware does not run from flash. At startup
`crowpanel::psram_code::relocate()` copies every flash page the application is
mapped from into PSRAM and points the memory map at the copies. It takes
about 0.4 s and 2.5 MB of PSRAM, and prints
`psram_code: running from PSRAM, 39 pages` on Serial. If it cannot (no PSRAM,
no room) the firmware carries on from flash and stops repainting between
snapshots, to draw as seldom as it can. One thing is given up for this: the
code is then in memory that can be written, so a stray write into PSRAM could
alter it where a write to flash would have faulted.

The `lcd` counters only see a frame that fell a long way behind. `slip on`
counts every slipped frame, to the pixel: it keeps the last 40 pixels of the
bottom row white and times that edge against the row's data-enable. `slip`
prints and resets the counts, with a breakdown by what the firmware was doing
in each frame, and `slip off` removes the marker:

```bash
~/.platformio/penv/bin/python scripts/crowpanel_screenshot.py \
    /dev/cu.usbserial-XXXX \
    --steps "send slip on; wait 4; send slip; wait 120; send slip; send slip off"
```

Measured on a V3 panel with 20 to 30 aircraft on a 25 km radar:

| Running from | Updates                          | Slipped frames |
|--------------|----------------------------------|----------------|
| Flash        | Data every 1.1 s, glide repaints | 25 of 1,848    |
| Flash        | Data and screen every 2 s        | 21 of 3,697    |
| PSRAM        | Data and screen every 2 s        | 0 of 3,696     |
| PSRAM        | Data every 2 s, glide repaints   | 0 of 3,696     |

From flash, no frame slipped while the firmware was idle and almost none while
it was fetching and parsing; they slipped while it drew, about 4% of the
frames spent redrawing the radar and 13% of those in which LVGL drew sidebar
text. From PSRAM the only slips seen were single 4-pixel ones while the range
was being changed, presumably as the setting was saved, which is a real flash
write.

`stress r SECONDS` redraws the whole screen continuously while a second task
reads the firmware's constant data end to end; `stress f` is the reads alone
and `stress p` copies PSRAM to PSRAM. From PSRAM, `stress r` slips no frame
(0 of 1,017, counted to the pixel). From flash it slipped most of them (257
of 374 by the coarser `lcd` count).

A slower pixel clock also stops the slips, but do not use one. `send pclk KHZ
FRONT_PORCH` changes the clock and the idle clocks after each line until the
next reset. Running from flash, under `stress r`:

| Pixel clock | Clocks per line | Line rate | Frames slipped (`lcd`) |
|-------------|-----------------|-----------|------------------------|
| 15.00 MHz   | 928 (shipped)   | 16.2 kHz  | 257 of 374             |
| 14.43 MHz   | 889             | 16.2 kHz  | 65 of 377              |
| 14.10 MHz   | 889             | 15.9 kHz  | 7 of 370               |
| 13.80 MHz   | 889             | 15.5 kHz  | 0 of 1336              |
| 13.70 MHz   | 889             | 15.4 kHz  | 0 of 887               |

Both slower settings that were left running made the panel whine audibly:
12 MHz with 928 clocks per line, and 13.7 MHz with 889, which was worse. Why
is not known. Moving the sketch onto the Wi-Fi core so that only one core
touches the bus made slipping worse, not better.

ESP-IDF has build options that do what `psram_code` does
(`CONFIG_SPIRAM_FETCH_INSTRUCTIONS` and `CONFIG_SPIRAM_RODATA`), but the stock
Arduino core is built without them. `psram_code` relies on how the ESP32-S3
maps external memory and on ESP-IDF 4.4's flash guard, neither of which is a
public API. If a newer framework breaks it, expect the firmware to stop at
startup just after printing its name; those two build options are the
replacement.

## Expected serial startup

```text
Plane Radar - CrowPanel 7-inch
psram_code: running from PSRAM, 39 pages (2496 KB) copied in 364 ms
CrowPanel: PSRAM 8388608 bytes
CrowPanel: GT911 detected at 0x5D
CrowPanel: display ready
CrowPanel: microSD ready
```

The microSD line may instead say `not mounted`; that is non-fatal. If PSRAM is
not detected, do not continue debugging the UI—verify the `qio_opi` memory
setting and board selection first.

## Implementation notes

The port keeps to its own folders, `src/crowpanel/` and `include/crowpanel/`,
and leaves the round display's code as upstream has it. Where the two need
different versions of the same thing, the panel has its own and
`platformio.ini` leaves the other out of each build: the aircraft feed client
(`crowpanel/adsb_client`) and the airport table (`crowpanel/airports`, built
by `scripts/build_crowpanel_airports.py`). The shared files it adds to are
`radar_location`, `radar_range` and `wifi_setup`.

The RGB panel uses LovyanGFX with LVGL 8.4. LVGL renders through two 40-line
buffers allocated in PSRAM; the 480×480 radar canvas also lives in PSRAM. UI
objects remain persistent and are updated in place. ADS-B JSON uses an
ArduinoJson filter so wide-radius responses do not retain unused fields.

The radar is painted in two layers. The grid, airports, and runways are drawn
once into a backdrop buffer and repainted only when the range, center, theme,
or units change. Aircraft are drawn over a copy of it; each frame puts the
backdrop back only in the 60-pixel tiles that held traffic and sends only the
changed tiles to the panel.

Flash and PSRAM share one SPI bus, and the panel's frame buffer is streamed
from PSRAM continuously. Reading a large block of flash in one go (the airport
table is 365 KB) starves that stream for a moment and the panel draws the rest
of the frame in the wrong place. Range queries therefore test a RAM copy of
the airport latitudes first (`airport_catalog::latitudeCells()`), and the name
search pauses briefly every few entries. Keep that in mind before adding
anything else that walks a large flash table while the radar is showing.

The aircraft feed keeps one HTTPS connection open and reads the reply itself,
polling the UI while it waits, so a fetch does not freeze the touch screen for
a TLS handshake. Route lookups go to ADSBDB's public flight-route endpoint from
a task of their own on the Wi-Fi core, with their own TLS session, so they do
not hold the screen up either. The last 24 answers are remembered (six hours
for a route, half an hour for "none"), and requests are at least 1.5 s apart.

Neither connection checks the server's TLS certificate (`setInsecure()`, as in
the round-display firmware). The traffic is encrypted but the server is not
authenticated, so someone able to intercept the panel's traffic could supply
false aircraft or routes, and would see the position being asked about.
Replies are limited in size and their numbers are checked before use, so the
worst that false data does is put wrong information on the screen. Checking
certificates would need a CA bundle in the build and has not been done.

Internal RAM is the scarce resource: about 120 KB is free in normal running,
and every TLS session that is open or being set up takes 50 to 60 KB of it.
So the large tables (256 aircraft, their on-screen copies, and the trails)
are allocated in PSRAM, and no more than one TLS session besides the aircraft
feed's is ever open: a route lookup, or a phone using **Find me**, not both.
Up to 256 aircraft are parsed from a wide-radius response, and trails are
kept for the nearest 96 and the pinned one.

LVGL keeps its widgets in a fixed pool of its own, 64 KB here (`LV_MEM_SIZE`
in `platformio.ini`), of which about 45 KB is in use. If the pool runs out
LVGL stops dead, which looks like a frozen screen, so check `lvgl_used` after
adding a screen and raise the pool if it is getting close.

Each reply is dated by the snapshot's own timestamp (`now`), not by when it
arrived. adsb.fi takes a snapshot every 2 s, so a reply can carry one that is
already that old, or the same one as the reply before. Projecting every reply
forward from its arrival made aircraft step back each time that happened.
`services::SnapshotClock` works the snapshot's age out from the device's
NTP-set clock when that agrees with the replies, and otherwise from the
freshest reply seen. Serial prints the age with every fetch.

The RGB timing is Elecrow's: a 15 MHz pixel clock and 928 clocks per line.
The pixel stream has no headroom at that clock, and a slower one makes the
panel whine, so the firmware keeps flash off the shared bus instead by running
from PSRAM; see "Checking the pixel stream".
