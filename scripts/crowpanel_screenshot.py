#!/usr/bin/env python3
"""Save what the CrowPanel is currently displaying as a PNG.

Needs firmware built with -DCROWPANEL_SERIAL_DEBUG=1 and pyserial:

    ~/.platformio/penv/bin/python scripts/crowpanel_screenshot.py \
        /dev/cu.usbserial-XXXX screen.png

The capture is the panel's scan-out buffer, so colours and layout are exactly
what the firmware is driving the LCD with. Anything else the board prints while
the script waits is echoed to stderr.

--reset reboots the board first. --steps drives the UI before the capture, for
example --steps "wait 20; tap 756 448; wait 1" opens Settings once the radar is
up ("tap X Y" presses the screen, "wait SECONDS" pauses). "lcd" prints and resets
the scan-out health counters: frames whose vsync interrupt was serviced late
and frames where the display's DMA stream ran dry.
"""

import argparse
import base64
import re
import struct
import sys
import time
import zlib

import serial


def echo(data):
    """Show log output from the board, minus screenshot data lines."""
    text = re.sub(rb"@@D[A-Za-z0-9+/]*\n?", b"", data).decode("utf-8", "replace")
    if text.strip():
        sys.stderr.write(text)


def capture(port, timeout):
    port.write(b"\nshot\n")
    received = b""
    deadline = time.time() + timeout
    while b"@@SHOT" not in received or b"\n" not in received.split(b"@@SHOT", 1)[1]:
        if time.time() > deadline:
            echo(received)
            raise TimeoutError("no @@SHOT from the board")
        received += port.read(4096)
    log, body = received.split(b"@@SHOT", 1)
    echo(log)
    header, body = body.split(b"\n", 1)
    fields = header.split()
    if len(fields) != 2:
        raise RuntimeError(f"board answered: @@SHOT{header.decode('ascii', 'replace')}")
    width, height = int(fields[0]), int(fields[1])

    # Data lines are written atomically; log lines may appear between them.
    deadline = time.time() + 180
    end = re.compile(rb"@@END ([0-9a-f]{8})")
    while not end.search(body):
        if time.time() > deadline:
            raise TimeoutError("no @@END from the board")
        body += port.read(4096)
    expected = int(end.search(body).group(1), 16)
    echo(body[:end.search(body).start()])
    records = b"".join(base64.b64decode(line)
                       for line in re.findall(rb"@@D([A-Za-z0-9+/]+)\n", body))

    actual = 2166136261
    for value in records:
        actual = ((actual ^ value) * 16777619) & 0xFFFFFFFF
    pixels = bytearray()
    for offset in range(0, len(records) - len(records) % 3, 3):
        value = records[offset + 1] | records[offset + 2] << 8
        red, green, blue = value >> 11, (value >> 5) & 0x3F, value & 0x1F
        pixels += bytes((red * 255 // 31, green * 255 // 63,
                         blue * 255 // 31)) * records[offset]
    if actual != expected or len(pixels) != width * height * 3:
        raise RuntimeError("screenshot was corrupted in transit; run it again")
    return width, height, bytes(pixels)


def write_png(path, width, height, rgb):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(
            ">I", zlib.crc32(body))

    rows = b"".join(b"\x00" + rgb[y * width * 3:(y + 1) * width * 3]
                    for y in range(height))
    with open(path, "wb") as output:
        output.write(b"\x89PNG\r\n\x1a\n")
        output.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2,
                                                0, 0, 0)))
        output.write(chunk(b"IDAT", zlib.compress(rows, 9)))
        output.write(chunk(b"IEND", b""))


def parse_steps(text, error):
    """Turn a --steps string into a list of steps, calling error() on a bad one."""
    steps = []
    for step in filter(None, (part.split() for part in text.split(";"))):
        if step[0] == "tap" and len(step) == 3:
            steps.append(("tap", int(step[1]), int(step[2])))
        elif step == ["lcd"]:
            steps.append(("lcd",))
        elif step[0] == "send" and len(step) > 1:
            steps.append(("send", " ".join(step[1:])))
        elif step[0] == "wait" and len(step) == 2:
            steps.append(("wait", float(step[1])))
        else:
            error(f"unknown step: {' '.join(step)}")
    return steps


def run_steps(port, steps):
    for step in steps:
        if step[0] == "tap":
            port.write(f"\ntap {step[1]} {step[2]}\n".encode())
        elif step[0] == "lcd":
            port.write(b"\nlcd\n")
        elif step[0] == "send":
            port.write(f"\n{step[1]}\n".encode())
        else:
            end = time.time() + step[1]
            while time.time() < end:
                sys.stderr.write(port.read(4096).decode("utf-8", "replace"))


def open_port(name, reset=False):
    port = serial.Serial()
    port.port = name
    port.baudrate = 115200
    port.timeout = 0.2
    port.dtr = False  # Leave GPIO0/EN alone so opening the port does not reboot.
    port.rts = False
    port.open()
    if reset:
        port.rts = True
        time.sleep(0.15)
        port.rts = False
    return port


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("port")
    parser.add_argument("output", nargs="?",
                        help="PNG to write; omit to only run --steps")
    parser.add_argument("--reset", action="store_true",
                        help="reboot the board before capturing")
    parser.add_argument("--steps", default="",
                        help='";"-separated "tap X Y", "wait SECONDS" and "lcd" '
                             "steps to run before capturing")
    arguments = parser.parse_args()
    steps = parse_steps(arguments.steps, parser.error)

    port = open_port(arguments.port, arguments.reset)
    try:
        run_steps(port, steps)
        if arguments.output is None:
            time.sleep(0.5)
            sys.stderr.write(port.read(65536).decode("utf-8", "replace"))
            return
        last_error = None
        for _ in range(3):
            try:
                width, height, rgb = capture(port, timeout=10)
                break
            except (RuntimeError, TimeoutError) as error:
                last_error = error
                port.reset_input_buffer()
        else:
            sys.exit(f"screenshot failed: {last_error}")
    finally:
        port.close()
    write_png(arguments.output, width, height, rgb)
    print(f"{arguments.output}: {width}x{height}")


if __name__ == "__main__":
    main()
