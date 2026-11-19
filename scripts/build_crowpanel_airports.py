#!/usr/bin/env python3
"""Build the 7-inch panel's airport and runway table from OurAirports.

Large airports are included worldwide, and small, medium and large ones in the
United States. The round display's smaller table is built by
build_large_airports.py and is left alone by this script.
"""

from __future__ import annotations

import csv
import io
import json
import unicodedata
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_H = ROOT / "include" / "crowpanel" / "airports.h"
OUT_CPP = ROOT / "src" / "crowpanel" / "airports_data.cpp"

AIRPORTS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "airports.csv"
)
RUNWAYS_URL = (
    "https://raw.githubusercontent.com/davidmegginson/ourairports-data/main/"
    "runways.csv"
)

US_AIRPORT_TYPES = {"small_airport", "medium_airport", "large_airport"}

AirportRow = tuple[str, str, int, int, str, str, bool]


def display_ident(row: dict[str, str]) -> str | None:
    """Choose a compact identifier that fits the embedded four-character field."""
    for key in ("gps_code", "local_code", "ident"):
        value = (row.get(key) or "").strip().upper()
        if 2 <= len(value) <= 4 and value.replace("-", "").isalnum():
            return value
    return None


def fetch_csv(url: str) -> list[dict[str, str]]:
    with urllib.request.urlopen(url, timeout=60) as resp:
        text = resp.read().decode("utf-8")
    return list(csv.DictReader(io.StringIO(text)))


def airport_label(row: dict[str, str]) -> str:
    """Return compact searchable ASCII text suitable for firmware flash."""
    name = (row.get("name") or row.get("ident") or "Airport").strip()
    municipality = (row.get("municipality") or "").strip()
    if municipality and municipality.casefold() not in name.casefold():
        name = f"{name} - {municipality}"
    ascii_name = unicodedata.normalize("NFKD", name).encode("ascii", "ignore").decode()
    return " ".join(ascii_name.split())[:96]


def airport_priority(airport_type: str, scheduled: bool, longest_runway_m: int) -> int:
    """Assign 1=local, 2=regional, 3=served/medium, 4=large."""
    if airport_type == "large_airport":
        return 4
    if airport_type == "medium_airport" or scheduled:
        return 3
    if longest_runway_m >= 1200:
        return 2
    return 1


def coord_e7(s: str | None) -> int | None:
    if not s or not s.strip():
        return None
    return int(round(float(s) * 1e7))


def is_h_designator(s: str) -> bool:
    if not s or s[0] != "H":
        return False
    rest = s[1:]
    if not rest:
        return True
    if rest[0] in "-_":
        return True
    return rest.isdigit()


def is_helipad(row: dict[str, str]) -> bool:
    le = (row.get("le_ident") or "").strip().upper()
    he = (row.get("he_ident") or "").strip().upper()
    if not is_h_designator(le) and not is_h_designator(he):
        return False
    try:
        length_ft = int(row.get("length_ft") or 0)
    except ValueError:
        length_ft = 0
    if is_h_designator(le) and is_h_designator(he):
        return True
    return length_ft < 2500


def build_dataset() -> tuple[
    list[AirportRow],
    list[tuple[int, int, int, int, int, int]],
]:
    airports = fetch_csv(AIRPORTS_URL)
    runways = fetch_csv(RUNWAYS_URL)

    selected_idents: dict[str, tuple[str, int, int, bool, str, str, bool]] = {}
    for a in airports:
        source_ident = (a.get("ident") or "").strip()
        airport_type = a.get("type") or ""
        is_global_large = airport_type == "large_airport"
        is_us_airport = a.get("iso_country") == "US" and airport_type in US_AIRPORT_TYPES
        if not is_global_large and not is_us_airport:
            continue
        ident = display_ident(a)
        if ident is None:
            continue
        lat = coord_e7(a.get("latitude_deg"))
        lon = coord_e7(a.get("longitude_deg"))
        if lat is None or lon is None:
            continue
        selected_idents[source_ident] = (
            ident,
            lat,
            lon,
            is_global_large,
            airport_label(a),
            airport_type,
            a.get("scheduled_service") == "yes",
        )

    # Keep the worldwide large-airport base first and append U.S. regional/local
    # coverage. Runway records retain the source identifier for their join.
    airport_rows = sorted(
        (source_ident, ident, lat, lon, name, airport_type, scheduled)
        for source_ident, (
            ident, lat, lon, is_global_large, name, airport_type, scheduled
        ) in selected_idents.items()
        if is_global_large
    )
    airport_rows.extend(
        sorted(
            (source_ident, ident, lat, lon, name, airport_type, scheduled)
            for source_ident, (
                ident, lat, lon, is_global_large, name, airport_type, scheduled
            ) in selected_idents.items()
            if not is_global_large
        )
    )
    airport_index = {
        source_ident: idx
        for idx, (source_ident, _ident, _lat, _lon, _name, _type, _scheduled)
        in enumerate(airport_rows)
    }

    segments: list[tuple[int, int, int, int, int, int]] = []
    for r in runways:
        if r.get("closed") == "1":
            continue
        source_ident = (r.get("airport_ident") or "").strip()
        if source_ident not in airport_index:
            continue
        if is_helipad(r):
            continue
        try:
            length_ft = int(r.get("length_ft") or 0)
        except ValueError:
            continue
        if length_ft <= 0:
            continue
        le_lat = coord_e7(r.get("le_latitude_deg"))
        le_lon = coord_e7(r.get("le_longitude_deg"))
        he_lat = coord_e7(r.get("he_latitude_deg"))
        he_lon = coord_e7(r.get("he_longitude_deg"))
        if None in (le_lat, le_lon, he_lat, he_lon):
            continue
        length_m = int(round(length_ft * 0.3048))
        segments.append(
            (
                airport_index[source_ident],
                le_lat,
                le_lon,
                he_lat,
                he_lon,
                length_m,
            )
        )

    segments.sort(key=lambda row: (row[0], -row[5]))
    return airport_rows, segments


def render_header(airport_count: int, segment_count: int) -> str:
    return "\n".join(
        [
            "// Generated by scripts/build_crowpanel_airports.py — do not edit.",
            "#pragma once",
            "",
            "// The 7-inch panel's airport table: large airports worldwide, and small,",
            "// medium and large ones in the United States, with names and a priority for",
            "// each. The round display keeps the smaller table in data/large_airports.h;",
            "// the two share a namespace and a build has one or the other.",
            "",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            "namespace data::large_airports {",
            "",
            "struct Airport {",
            "  char ident[5];",
            "  int32_t lat_e7;",
            "  int32_t lon_e7;",
            "  uint16_t runway_start;",
            "  uint16_t runway_count;",
            "  uint16_t longest_runway_m;",
            "  uint8_t priority;",
            "  uint8_t flags;",
            "};",
            "",
            "constexpr uint8_t kAirportScheduledService = 0x01;",
            "",
            "struct Runway {",
            "  uint16_t airport_idx;",
            "  int32_t le_lat_e7;",
            "  int32_t le_lon_e7;",
            "  int32_t he_lat_e7;",
            "  int32_t he_lon_e7;",
            "  uint16_t length_m;",
            "};",
            "",
            f"constexpr size_t kAirportCount = {airport_count};",
            f"constexpr size_t kRunwayCount = {segment_count};",
            "",
            "extern const Airport kAirports[];",
            "extern const char* const kAirportNames[];",
            "extern const Runway kRunways[];",
            "",
            "}  // namespace data::large_airports",
            "",
        ]
    )


def render_cpp(
    airport_rows: list[AirportRow],
    segments: list[tuple[int, int, int, int, int, int]],
) -> str:
    lines = [
        "// Generated by scripts/build_crowpanel_airports.py — do not edit.",
        '#include "crowpanel/airports.h"',
        "",
        "namespace data::large_airports {",
        "",
        "const Airport kAirports[] = {",
    ]
    runway_ranges = [[0, 0, 0] for _airport in airport_rows]
    for runway_index, segment in enumerate(segments):
        airport_index = segment[0]
        if runway_ranges[airport_index][1] == 0:
            runway_ranges[airport_index][0] = runway_index
        runway_ranges[airport_index][1] += 1
        runway_ranges[airport_index][2] = max(
            runway_ranges[airport_index][2], segment[5]
        )
    for airport_index, (
        _source, ident, lat, lon, _name, airport_type, scheduled
    ) in enumerate(airport_rows):
        runway_start, runway_count, longest_runway_m = runway_ranges[airport_index]
        priority = airport_priority(airport_type, scheduled, longest_runway_m)
        flags = 1 if scheduled else 0
        lines.append(
            f'  {{"{ident}", {lat}, {lon}, {runway_start}, {runway_count}, '
            f'{longest_runway_m}, {priority}, {flags}}},'
        )
    lines += [
        "};",
        "",
        "const char* const kAirportNames[] = {",
    ]
    for _source, _ident, _lat, _lon, name, _type, _scheduled in airport_rows:
        lines.append(f"  {json.dumps(name)},")
    lines += [
        "};",
        "",
        "const Runway kRunways[] = {",
    ]
    for airport_idx, le_lat, le_lon, he_lat, he_lon, length_m in segments:
        lines.append(
            f"  {{{airport_idx}, {le_lat}, {le_lon}, {he_lat}, {he_lon}, {length_m}}},"
        )
    lines += [
        "};",
        "",
        "}  // namespace data::large_airports",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    airport_rows, segments = build_dataset()
    header = render_header(len(airport_rows), len(segments))
    cpp = render_cpp(airport_rows, segments)

    OUT_H.parent.mkdir(parents=True, exist_ok=True)
    OUT_CPP.parent.mkdir(parents=True, exist_ok=True)
    OUT_H.write_text(header, encoding="utf-8")
    OUT_CPP.write_text(cpp, encoding="utf-8")
    print(
        f"wrote {OUT_H.name} + {OUT_CPP.name} "
        f"({len(segments)} segments, {len(airport_rows)} airports)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
