#!/usr/bin/env python3
"""Build the 7-inch panel's time-zone map and its list of zones to choose from.

The panel shows local time for wherever the radar is centred, and works that
out by itself rather than asking a service. This writes what it needs: a
coarse map of the world in which every cell names the rule in force there, and
the zones the settings screen offers by name. A rule is a POSIX TZ string such
as "EST5EDT,M3.2.0,M11.1.0", which the panel's C library turns into local time,
daylight saving included.

Two packages are needed that nothing else in this project uses:

    pip install timezonefinder tzdata

Run this again when a country changes its clocks, and reflash.
"""

from __future__ import annotations

import datetime
import multiprocessing
import os
import re
import time
import zoneinfo
from collections import Counter, deque
from importlib import resources
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_H = ROOT / "include" / "crowpanel" / "time_zone_data.h"
OUT_CPP = ROOT / "src" / "crowpanel" / "time_zone_data.cpp"

CELLS_PER_DEGREE = 4
COLUMNS = 360 * CELLS_PER_DEGREE
ROWS = 180 * CELLS_PER_DEGREE

# Each cell takes the zone most of a grid of points inside it falls in.
SAMPLES_PER_SIDE = 3

# Sea within this many cells of land keeps the land's zone, so that a place on
# the coast whose cell is mostly water is not put on ship's time.
COAST_REACH_CELLS = 2 * CELLS_PER_DEGREE

UTC_RULE = "UTC0"

# The zones the settings screen lists, each named after cities people know it
# by. Two entries may not share a rule.
#
# The screen puts the offset in front ("UTC-8  Los Angeles, Seattle"), and
# this many characters of that is what fits in its closed list.
LONGEST_CHOICE = 27
CHOICES = [
    ("American Samoa", "Pacific/Pago_Pago"),
    ("Honolulu", "Pacific/Honolulu"),
    ("Anchorage", "America/Anchorage"),
    ("Los Angeles, Seattle", "America/Los_Angeles"),
    ("Phoenix", "America/Phoenix"),
    ("Denver, Edmonton", "America/Denver"),
    ("Chicago, Winnipeg", "America/Chicago"),
    ("Mexico City, Regina", "America/Mexico_City"),
    ("New York, Toronto", "America/New_York"),
    ("Panama, Jamaica", "America/Panama"),
    ("Bogota, Lima", "America/Bogota"),
    ("Halifax", "America/Halifax"),
    ("Puerto Rico", "America/Puerto_Rico"),
    ("Caracas, La Paz", "America/Caracas"),
    ("Santiago", "America/Santiago"),
    ("St. John's", "America/St_Johns"),
    ("Rio, Buenos Aires", "America/Sao_Paulo"),
    ("Nuuk", "America/Nuuk"),
    ("Azores", "Atlantic/Azores"),
    ("Cape Verde", "Atlantic/Cape_Verde"),
    ("London", "Europe/London"),
    ("Dublin", "Europe/Dublin"),
    ("Lisbon", "Europe/Lisbon"),
    ("Reykjavik, Accra", "Atlantic/Reykjavik"),
    ("Casablanca", "Africa/Casablanca"),
    ("Paris, Berlin, Rome", "Europe/Paris"),
    ("Lagos", "Africa/Lagos"),
    ("Athens, Helsinki", "Europe/Athens"),
    ("Cairo", "Africa/Cairo"),
    ("Johannesburg", "Africa/Johannesburg"),
    ("Jerusalem", "Asia/Jerusalem"),
    ("Istanbul, Riyadh", "Europe/Istanbul"),
    ("Moscow", "Europe/Moscow"),
    ("Nairobi", "Africa/Nairobi"),
    ("Tehran", "Asia/Tehran"),
    ("Dubai", "Asia/Dubai"),
    ("Kabul", "Asia/Kabul"),
    ("Karachi", "Asia/Karachi"),
    ("Tashkent", "Asia/Tashkent"),
    ("Delhi, Mumbai", "Asia/Kolkata"),
    ("Kathmandu", "Asia/Kathmandu"),
    ("Dhaka", "Asia/Dhaka"),
    ("Yangon", "Asia/Yangon"),
    ("Bangkok, Hanoi", "Asia/Bangkok"),
    ("Jakarta", "Asia/Jakarta"),
    ("Beijing, Taipei", "Asia/Shanghai"),
    ("Hong Kong", "Asia/Hong_Kong"),
    ("Singapore", "Asia/Singapore"),
    ("Manila", "Asia/Manila"),
    ("Perth", "Australia/Perth"),
    ("Tokyo", "Asia/Tokyo"),
    ("Seoul", "Asia/Seoul"),
    ("Darwin", "Australia/Darwin"),
    ("Adelaide", "Australia/Adelaide"),
    ("Brisbane", "Australia/Brisbane"),
    ("Sydney, Melbourne", "Australia/Sydney"),
    ("Guam", "Pacific/Guam"),
    ("Noumea", "Pacific/Noumea"),
    ("Auckland", "Pacific/Auckland"),
    ("Fiji", "Pacific/Fiji"),
    ("Tonga", "Pacific/Tongatapu"),
]

# Zones whose rule is known not to match their clocks on some days, and why.
# Any other zone that does not match stops the build, so that a change to a
# country's clocks is looked at rather than passed over.
KNOWN_GAPS = {
    "America/Havana": "Cuba kept summer time on through the winter of 2026-27; "
                      "the rule is right again from March 2027",
}

# What the panel's C library (newlib) reads. It is stricter than the standard
# in two ways that matter here: names are 3 to 10 characters, and the time of
# day a change happens at cannot be negative.
_NAME = r"(?:[A-Za-z]{3,10}|<[-+0-9A-Za-z]{3,10}>)"
_CLOCK = r"\d{1,3}(?::\d{1,2}(?::\d{1,2})?)?"
_DAY = r"(?:M(?:1[0-2]|[1-9])\.[1-5]\.[0-6]|J\d{1,3}|\d{1,3})"
PANEL_READS = re.compile(
    rf"{_NAME}[-+]?{_CLOCK}"
    rf"(?:{_NAME}(?:[-+]?{_CLOCK})?,{_DAY}(?:/{_CLOCK})?,{_DAY}(?:/{_CLOCK})?)?"
)

# How far ahead each rule is compared with the zone it stands for.
CHECK_YEARS = 5

_finder = None


def _zone_file(name: str) -> bytes:
    return resources.files("tzdata.zoneinfo").joinpath(*name.split("/")).read_bytes()


def rule_for_zone(name: str) -> str:
    """The rule for a named zone, in a form the panel's C library reads."""
    # A zone file ends with the rule that holds from its last listed change on.
    rule = _zone_file(name).rstrip(b"\n").rsplit(b"\n", 1)[1].decode("ascii")
    # Greenland changes at "-1", an hour before midnight. The nearest the panel
    # can read is midnight, which makes it an hour late for that one hour.
    return re.sub(r"/-\d+(?::\d+)*", "/0", rule)


def sea_rule(column: int) -> str:
    """Ship's time: the whole hour nearest the sun, by longitude alone."""
    lon = -180 + (column + 0.5) / CELLS_PER_DEGREE
    hours_east = int(round(lon / 15))
    if hours_east == 0:
        return UTC_RULE
    sign = "+" if hours_east > 0 else "-"
    return f"<{sign}{abs(hours_east):02d}>{-hours_east}"


def _minutes_east(sign: str, hours: str, minutes: str | None) -> int:
    # The sign in a rule is the opposite of the usual one: west is positive.
    value = int(hours) * 60 + int(minutes or 0)
    return value if sign == "-" else -value


def winter_offset_minutes(rule: str) -> int:
    """Minutes east of UTC when daylight saving is not in force.

    That is the smaller of a rule's two offsets, whichever of them it calls
    standard: Ireland's rule names its summer time as the standard one.
    """
    offset = r"([-+]?)(\d+)(?::(\d+))?"
    match = re.match(rf"{_NAME}{offset}(?:{_NAME}(?:{offset})?)?", rule)
    if match is None:
        raise ValueError(f"no offset in {rule!r}")
    first = _minutes_east(match.group(1), match.group(2), match.group(3))
    if match.group(5) is None:
        return first  # No daylight saving, or the usual hour ahead.
    return min(first, _minutes_east(match.group(4), match.group(5), match.group(6)))


def offset_label(minutes: int) -> str:
    sign = "-" if minutes < 0 else "+"
    hours, rest = divmod(abs(minutes), 60)
    return f"UTC{sign}{hours}" + (f":{rest:02d}" if rest else "")


def _row_zones(row: int) -> list[str | None]:
    """The land zone of each cell in a row, or None where it is all sea."""
    global _finder
    if _finder is None:
        from timezonefinder import TimezoneFinder

        _finder = TimezoneFinder(in_memory=True)
    step = 1 / (CELLS_PER_DEGREE * SAMPLES_PER_SIDE)
    lats = [90 - row / CELLS_PER_DEGREE - (i + 0.5) * step
            for i in range(SAMPLES_PER_SIDE)]
    zones: list[str | None] = []
    for column in range(COLUMNS):
        west = -180 + column / CELLS_PER_DEGREE
        found: Counter[str] = Counter()
        for lat in lats:
            for i in range(SAMPLES_PER_SIDE):
                zone = _finder.timezone_at_land(lng=west + (i + 0.5) * step, lat=lat)
                if zone is not None:
                    found[zone] += 1
        zones.append(found.most_common(1)[0][0] if found else None)
    return zones


def spread_to_coast(grid: list[list[str | None]]) -> None:
    """Give sea cells near land the zone of the nearest land cell."""
    reach = [[0 if zone is not None else -1 for zone in row] for row in grid]
    queue = deque(
        (row, column)
        for row in range(ROWS)
        for column in range(COLUMNS)
        if grid[row][column] is not None
    )
    while queue:
        row, column = queue.popleft()
        if reach[row][column] == COAST_REACH_CELLS:
            continue
        for d_row, d_column in ((-1, 0), (1, 0), (0, -1), (0, 1)):
            next_row = row + d_row
            next_column = (column + d_column) % COLUMNS
            if 0 <= next_row < ROWS and reach[next_row][next_column] < 0:
                reach[next_row][next_column] = reach[row][column] + 1
                grid[next_row][next_column] = grid[row][column]
                queue.append((next_row, next_column))


def check_rule(name: str, rule: str) -> None:
    """Stop if a rule gives a different time from the zone it stands for."""
    zone = zoneinfo.ZoneInfo(name)
    start = datetime.datetime.now(datetime.timezone.utc).replace(
        hour=12, minute=0, second=0, microsecond=0)
    os.environ["TZ"] = rule
    time.tzset()
    wrong: list[str] = []
    for day in range(CHECK_YEARS * 366):
        moment = start + datetime.timedelta(days=day)
        by_rule = time.localtime(moment.timestamp()).tm_gmtoff
        by_zone = int(moment.astimezone(zone).utcoffset().total_seconds())
        if by_rule != by_zone:
            wrong.append(
                f"{moment:%Y-%m-%d}: rule UTC{by_rule / 3600:+g}, "
                f"zone UTC{by_zone / 3600:+g}")
    if not wrong:
        return
    summary = (f"{name}: rule {rule!r} is wrong on {len(wrong)} of the next "
               f"{CHECK_YEARS * 366} days, first {wrong[0]}, last {wrong[-1]}")
    if name not in KNOWN_GAPS:
        raise SystemExit(f"{summary}. If that is expected, say why in KNOWN_GAPS.")
    print(f"{summary} ({KNOWN_GAPS[name]})")


def build() -> tuple[list[str], list[list[tuple[int, int]]], list[tuple[str, int]]]:
    # Rules come from the tzdata package alone, not this computer's own copy.
    zoneinfo.reset_tzpath(to=[])

    with multiprocessing.Pool() as pool:
        grid = pool.map(_row_zones, range(ROWS), chunksize=8)
    spread_to_coast(grid)

    zone_rules: dict[str, str] = {}
    for name in sorted({zone for row in grid for zone in row if zone is not None}
                       | {zone for _label, zone in CHOICES}):
        rule = rule_for_zone(name)
        if PANEL_READS.fullmatch(rule) is None:
            raise SystemExit(f"{name}: the panel cannot read the rule {rule!r}")
        check_rule(name, rule)
        zone_rules[name] = rule

    cell_rules = [
        [sea_rule(column) if zone is None else zone_rules[zone]
         for column, zone in enumerate(row)]
        for row in grid
    ]
    choice_rules = [(label, zone_rules[zone]) for label, zone in CHOICES]
    shared = [rule for rule, count in Counter(r for _l, r in choice_rules).items()
              if count > 1]
    if shared:
        raise SystemExit(f"choices share a rule: {shared}")

    rules = sorted(
        {UTC_RULE} | {rule for row in cell_rules for rule in row}
        | {rule for _label, rule in choice_rules},
        key=lambda rule: (rule != UTC_RULE, winter_offset_minutes(rule), rule))
    if len(rules) > 256:
        raise SystemExit(f"{len(rules)} rules do not fit in one byte each")
    rule_id = {rule: index for index, rule in enumerate(rules)}

    rows: list[list[tuple[int, int]]] = []
    for row in cell_rules:
        runs: list[tuple[int, int]] = []
        for column, rule in enumerate(row):
            if not runs or runs[-1][1] != rule_id[rule]:
                runs.append((column, rule_id[rule]))
        rows.append(runs)
    if sum(len(runs) for runs in rows) > 65535:
        raise SystemExit("too many runs for a 16-bit row index")

    choices = [
        (f"{offset_label(minutes)}  {label}", rule_id[rule])
        for minutes, label, rule in sorted(
            (winter_offset_minutes(rule), label, rule) for label, rule in choice_rules)
    ]
    too_long = [label for label, _rule in choices if len(label) > LONGEST_CHOICE]
    if too_long:
        raise SystemExit(f"too long for the settings screen: {too_long}")
    return rules, rows, choices


def numbers(values: list[int], per_line: int = 16) -> list[str]:
    return [
        "  " + ", ".join(str(value) for value in values[start:start + per_line]) + ","
        for start in range(0, len(values), per_line)
    ]


def render_header(rules: list[str], choices: list[tuple[str, int]]) -> str:
    return "\n".join(
        [
            "// Generated by scripts/build_crowpanel_time_zones.py — do not edit.",
            "#pragma once",
            "",
            "// What the 7-inch panel needs to show local time: a coarse map of the",
            "// world's time zones, and the zones its settings screen lists. A rule is a",
            '// POSIX TZ string such as "EST5EDT,M3.2.0,M11.1.0", which the C library',
            "// turns into local time.",
            "",
            "#include <cstddef>",
            "#include <cstdint>",
            "",
            "namespace crowpanel::time_zone_data {",
            "",
            f"constexpr size_t kRuleCount = {len(rules)};",
            "// The longest rule, not counting the end of its string.",
            f"constexpr size_t kLongestRule = {max(len(rule) for rule in rules)};",
            "extern const char* const kRules[];",
            "",
            "// The map: columns run east from 180 W and rows south from 90 N.",
            f"constexpr int kCellsPerDegree = {CELLS_PER_DEGREE};",
            f"constexpr int kMapColumns = {COLUMNS};",
            f"constexpr int kMapRows = {ROWS};",
            "",
            "// Each row is a list of runs of cells that share a rule. Row r has runs",
            "// kRowFirstRun[r] up to, not including, kRowFirstRun[r + 1]. Run i starts",
            "// at column kRunColumn[i], lasts until the row's next run starts, and has",
            "// rule kRules[kRunRule[i]].",
            "extern const uint16_t kRowFirstRun[];",
            "extern const uint16_t kRunColumn[];",
            "extern const uint8_t kRunRule[];",
            "",
            "// A zone the settings screen offers, in order from west to east.",
            "struct Choice {",
            "  const char* label;",
            "  uint8_t rule;",
            "};",
            f"constexpr size_t kChoiceCount = {len(choices)};",
            "extern const Choice kChoices[];",
            "",
            "}  // namespace crowpanel::time_zone_data",
            "",
        ]
    )


def render_cpp(
    rules: list[str],
    rows: list[list[tuple[int, int]]],
    choices: list[tuple[str, int]],
) -> str:
    first_runs = [0]
    for runs in rows:
        first_runs.append(first_runs[-1] + len(runs))
    lines = [
        "// Generated by scripts/build_crowpanel_time_zones.py — do not edit.",
        '#include "crowpanel/time_zone_data.h"',
        "",
        "namespace crowpanel::time_zone_data {",
        "",
        "const char* const kRules[] = {",
    ]
    lines += [f'  "{rule}",' for rule in rules]
    lines += ["};", "", "const uint16_t kRowFirstRun[] = {"]
    lines += numbers(first_runs)
    lines += ["};", "", "const uint16_t kRunColumn[] = {"]
    lines += numbers([column for runs in rows for column, _rule in runs])
    lines += ["};", "", "const uint8_t kRunRule[] = {"]
    lines += numbers([rule for runs in rows for _column, rule in runs], per_line=24)
    lines += ["};", "", "const Choice kChoices[] = {"]
    lines += [f'  {{"{label}", {rule}}},' for label, rule in choices]
    lines += ["};", "", "}  // namespace crowpanel::time_zone_data", ""]
    return "\n".join(lines)


def main() -> int:
    rules, rows, choices = build()
    OUT_H.write_text(render_header(rules, choices), encoding="utf-8")
    OUT_CPP.write_text(render_cpp(rules, rows, choices), encoding="utf-8")
    run_count = sum(len(runs) for runs in rows)
    print(
        f"wrote {OUT_H.name} + {OUT_CPP.name} ({len(rules)} rules, "
        f"{run_count} runs, about {(run_count * 3 + (ROWS + 1) * 2) // 1024} KB, "
        f"{len(choices)} choices)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
