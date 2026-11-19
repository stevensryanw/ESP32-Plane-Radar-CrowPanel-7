#!/usr/bin/env python3
"""Record what the CrowPanel firmware sends to its LCD and replay it as video.

Needs firmware built with -DCROWPANEL_SERIAL_DEBUG=1, pyserial, and ffmpeg for
the video:

    ~/.platformio/penv/bin/python scripts/crowpanel_record.py \
        /dev/cu.usbserial-XXXX panel.mp4 --seconds 10

For that long the firmware keeps every write to the panel's frame buffer, with
when it happened and where the panel's scan was, then sends the lot. The video
has one frame per panel refresh and shows each row as it stood when the panel
scanned it, so a write that landed part-way down a refresh appears torn for
that frame, as it would on the glass. The script also reports how many writes
tore and how many frames the scan-out probe counted as displaced.

It cannot show anything the panel did on its own with a correct frame buffer;
for that, film the screen.

--raw keeps the recording itself, and --replay renders one saved that way.
--steps runs "tap X Y" and "wait SECONDS" steps while recording.
"""

import argparse
import base64
import json
import re
import shutil
import subprocess
import sys
import time

import crowpanel_screenshot as panel

MAGIC = b"CROWPANEL-REC 1\n"
WRITE_FIELDS = ("start", "end", "vsync", "scan_start", "scan_end", "x", "y",
                "width", "height", "kind")
KINDS = {0: "LVGL", 1: "radar tiles"}
DESCRIPTOR_BYTES = 4096 - 64  # Frame buffer covered by one DMA descriptor.
DESCRIPTOR_STEP = 12          # Distance between descriptor addresses.


class Recording:
    def __init__(self, meta, base, pixels):
        self.meta = meta
        self.width = meta["width"]
        self.height = meta["height"]
        self.duration = meta["duration_us"]
        self.base = base        # RGB565 little-endian, the frame at the start.
        self.writes = [dict(zip(WRITE_FIELDS, values)) for values in meta["writes"]]
        offset = 0
        for write in self.writes:
            size = write["width"] * write["height"] * 2
            write["pixels"] = pixels[offset:offset + size]
            offset += size
        self.events = meta["events"]
        self.line_us = meta["line_clocks"] * 1e6 / meta["pixel_clock"]
        self.frame_us = self.line_us * meta["frame_lines"]
        self.descriptors = (len(base) - 1) // DESCRIPTOR_BYTES + 1
        self.lead_lines = self._lead_lines()

    def save(self, path):
        with open(path, "wb") as output:
            output.write(MAGIC + json.dumps(self.meta).encode() + b"\n")
            output.write(self.base)
            for write in self.writes:
                output.write(write["pixels"])

    @classmethod
    def load(cls, path):
        with open(path, "rb") as source:
            if source.readline() != MAGIC:
                sys.exit(f"{path} is not a CrowPanel recording")
            meta = json.loads(source.readline())
            base = source.read(meta["width"] * meta["height"] * 2)
            return cls(meta, base, source.read())

    def scan_line(self, position):
        """Row the panel was reading for a scan position, to about 3 rows."""
        index = ((position - self.meta["sync"]) // DESCRIPTOR_STEP) % self.descriptors
        return index * DESCRIPTOR_BYTES / (self.width * 2)

    def _lead_lines(self):
        """Line periods from the vsync interrupt to the scan of row 0.

        Measured from the writes: each gives a time since vsync and a scan
        position, which pin down where row 0 falls.
        """
        leads = []
        for write in self.writes:
            row = self.scan_line(write["scan_start"])
            if 20 < row < self.height - 20:
                since_vsync = (write["start"] - write["vsync"]) % self.frame_us
                leads.append(since_vsync / self.line_us - row)
        if not leads:
            return self.meta["frame_lines"] - self.height
        leads.sort()
        return leads[len(leads) // 2]

    def row_at(self, moment):
        """Row being scanned at a time, or None during vertical blanking."""
        if not self.writes:
            return None
        anchor = self.writes[0]["vsync"]
        row = ((moment - anchor) % self.frame_us) / self.line_us - self.lead_lines
        return row if 0 <= row < self.height else None

    def torn(self, write):
        """True if the panel was scanning inside the write as it landed."""
        row = self.row_at(write["end"])
        return row is not None and write["y"] <= row < write["y"] + write["height"]

    def frames(self):
        """Yield (time, RGB565 frame) for each panel refresh, as scanned."""
        width = self.width
        state = bytearray(self.base)

        def apply(frame, write, first_row=0):
            span = write["width"] * 2
            for row in range(max(first_row, 0), write["height"]):
                at = ((write["y"] + row) * width + write["x"]) * 2
                frame[at:at + span] = write["pixels"][row * span:(row + 1) * span]

        anchor = self.writes[0]["vsync"] if self.writes else 0
        refresh = int((0 - anchor) // self.frame_us)
        next_write = 0
        while True:
            top = anchor + refresh * self.frame_us + self.lead_lines * self.line_us
            if top > self.duration:
                return
            bottom = top + self.height * self.line_us
            while (next_write < len(self.writes) and
                   self.writes[next_write]["end"] <= top):
                apply(state, self.writes[next_write])
                next_write += 1
            shown = state
            for write in self.writes[next_write:]:
                if write["end"] >= bottom:
                    break
                # Rows already scanned this refresh keep their old content.
                if shown is state:
                    shown = bytearray(state)
                scanned = int((write["end"] - top) / self.line_us)
                apply(shown, write, scanned + 1 - write["y"])
            yield top, bytes(shown)
            refresh += 1


def receive(port, seconds, steps):
    port.reset_input_buffer()
    port.write(f"\nrec {seconds}\n".encode())
    received = b""
    for step in steps:
        if step[0] == "wait":  # Keep what arrives: the dump may start early.
            end = time.time() + step[1]
            while time.time() < end:
                received += port.read(4096)
        else:
            panel.run_steps(port, [step])
    deadline = time.time() + seconds + 15
    while not re.search(rb"@@REC[^\n]*\n", received):
        if time.time() > deadline:
            panel.echo(received)
            sys.exit("no @@REC from the board")
        received += port.read(4096)
    log, body = received.split(b"@@REC", 1)
    panel.echo(log)
    if body.startswith(b"-ERROR"):
        sys.exit("the board has no memory free to record into")

    end = re.compile(rb"@@END ([0-9a-f]{8})")
    quiet_since = time.time()
    last_report = 0
    while not end.search(body, max(0, len(body) - 8192)):
        data = port.read(8192)
        if data:
            body += data
            quiet_since = time.time()
        elif time.time() - quiet_since > 15:
            sys.exit("the board stopped sending part-way through")
        if len(body) - last_report > 100000:
            last_report = len(body)
            sys.stderr.write(f"\rreceiving... {len(body) // 1000} kB")
    sys.stderr.write("\n")

    header, body = body.split(b"\n", 1)
    fields = [int(value) for value in header.split()]
    width, height, count, duration, pixel_clock, line_clocks, frame_lines, sync = fields

    def expand(records):
        pixels = bytearray()
        for offset in range(0, len(records), 3):
            pixels += records[offset + 1:offset + 3] * records[offset]
        return bytes(pixels)

    segments = [bytearray()]  # The starting frame, then one per write.
    writes = []
    events = []
    expected = None
    token = re.compile(rb"@@D([A-Za-z0-9+/]+)\n|@@W ([-0-9 ]+)\n|"
                       rb"@@A ([-0-9 ]+)\n|@@END ([0-9a-f]{8})")
    for match in token.finditer(body):
        if match.group(1):
            segments[-1] += base64.b64decode(match.group(1))
        elif match.group(2):
            writes.append([int(value) for value in match.group(2).split()])
            segments.append(bytearray())
        elif match.group(3):
            events.append([int(value) for value in match.group(3).split()])
        else:
            expected = int(match.group(4), 16)
            break

    actual = 2166136261
    for segment in segments:
        for value in segment:
            actual = ((actual ^ value) * 16777619) & 0xFFFFFFFF
    base = expand(segments[0])
    pixels = [expand(segment) for segment in segments[1:]]
    intact = (actual == expected and len(writes) == count and
              len(base) == width * height * 2 and
              all(len(write) == len(WRITE_FIELDS) for write in writes) and
              all(len(data) == write[7] * write[8] * 2
                  for write, data in zip(writes, pixels)))
    if not intact:
        sys.exit("the recording was corrupted in transit; run it again")
    meta = {"width": width, "height": height, "duration_us": duration,
            "pixel_clock": pixel_clock, "line_clocks": line_clocks,
            "frame_lines": frame_lines, "sync": sync, "writes": writes,
            "events": events}
    return Recording(meta, base, b"".join(pixels))


def write_video(recording, path):
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg is needed to write the video; use --raw to keep the "
                 "recording and render it later with --replay")
    meta = recording.meta
    rate = f"{meta['pixel_clock']}/{meta['line_clocks'] * meta['frame_lines']}"
    encoder = subprocess.Popen(
        ["ffmpeg", "-loglevel", "error", "-y", "-f", "rawvideo",
         "-pix_fmt", "rgb565le", "-s", f"{recording.width}x{recording.height}",
         "-framerate", rate, "-i", "-", "-c:v", "libx264", "-preset", "fast",
         "-crf", "12", "-pix_fmt", "yuv420p", path],
        stdin=subprocess.PIPE)
    count = 0
    for _, frame in recording.frames():
        encoder.stdin.write(frame)
        count += 1
    encoder.stdin.close()
    if encoder.wait() != 0:
        sys.exit("ffmpeg failed")
    return count


def summarize(recording):
    seconds = recording.duration / 1e6
    print(f"{seconds:.1f} s, {len(recording.writes)} writes to the panel, "
          f"{recording.frame_us / 1000:.1f} ms per refresh")
    for kind, name in KINDS.items():
        writes = [write for write in recording.writes if write["kind"] == kind]
        if writes:
            torn = sum(recording.torn(write) for write in writes)
            print(f"  {name}: {len(writes)} writes, {torn} landed while the panel "
                  "was scanning them")
    displaced = sum(1 for event in recording.events if event[1] == 1)
    short = sum(1 for event in recording.events if event[1] == 2)
    print(f"  frames the probe saw displaced: {displaced}, cut short: {short}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port", nargs="?", help="serial port of the panel")
    parser.add_argument("output", nargs="?", help="MP4 to write")
    parser.add_argument("--seconds", type=int, default=10,
                        help="how long to record, 1 to 60 (default 10)")
    parser.add_argument("--steps", default="",
                        help='";"-separated "tap X Y" and "wait SECONDS" steps '
                             "to run while recording")
    parser.add_argument("--raw", help="also save the recording itself here")
    parser.add_argument("--replay", help="render this saved recording instead "
                                         "of making a new one")
    arguments = parser.parse_args()

    if arguments.replay:
        # With --replay the only positional argument is the video.
        output = arguments.output or arguments.port
        recording = Recording.load(arguments.replay)
    else:
        if arguments.port is None or not 1 <= arguments.seconds <= 60:
            parser.error("give a serial port and 1 to 60 seconds")
        output = arguments.output
        steps = panel.parse_steps(arguments.steps, parser.error)
        port = panel.open_port(arguments.port)
        try:
            recording = receive(port, arguments.seconds, steps)
        finally:
            port.close()
        if arguments.raw:
            recording.save(arguments.raw)

    summarize(recording)
    if output:
        frames = write_video(recording, output)
        print(f"{output}: {frames} frames")


if __name__ == "__main__":
    main()
