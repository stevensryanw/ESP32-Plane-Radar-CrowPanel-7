#!/usr/bin/env python3
"""Generate a support-free two-foot stand for the 7-inch CrowPanel case.

The STL convention is millimeters. Two disconnected, identical feet are placed
in one file so they can be sliced and printed together. The profile is extruded
as a single watertight shell per foot; no boolean/CAD dependency is required.
"""

from __future__ import annotations

import math
import sys
from collections import Counter
from pathlib import Path
from typing import Iterable, Sequence


Point2 = tuple[float, float]  # y, z
Point3 = tuple[float, float, float]  # x, y, z
Triangle = tuple[Point3, Point3, Point3]

# Primary fit and stance parameters (millimeters/degrees).
CASE_THICKNESS_MM = 14.0
FIT_CLEARANCE_MM = 0.60
SLOT_WIDTH_MM = CASE_THICKNESS_MM + FIT_CLEARANCE_MM
LEAN_DEGREES = 15.0

FOOT_WIDTH_MM = 32.0
BASE_DEPTH_MM = 82.0
BASE_HEIGHT_MM = 8.0
FRONT_SLOT_Y_MM = 28.0
FRONT_WALL_THICKNESS_MM = 7.0
REAR_WALL_THICKNESS_MM = 10.0
FRONT_WALL_HEIGHT_MM = 10.0
REAR_WALL_HEIGHT_MM = 44.0
TOP_CHAMFER_MM = 1.5
PART_GAP_MM = 18.0


def cross2(a: Point2, b: Point2, c: Point2) -> float:
    return (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0])


def polygon_area(points: Sequence[Point2]) -> float:
    return 0.5 * sum(
        points[i][0] * points[(i + 1) % len(points)][1]
        - points[(i + 1) % len(points)][0] * points[i][1]
        for i in range(len(points))
    )


def point_in_triangle(point: Point2, a: Point2, b: Point2, c: Point2) -> bool:
    epsilon = 1e-9
    return (
        cross2(a, b, point) >= -epsilon
        and cross2(b, c, point) >= -epsilon
        and cross2(c, a, point) >= -epsilon
    )


def triangulate_polygon(points: Sequence[Point2]) -> list[tuple[int, int, int]]:
    """Ear-clip a simple counter-clockwise polygon."""
    if len(points) < 3 or polygon_area(points) <= 0:
        raise ValueError("Profile must be a counter-clockwise simple polygon")

    remaining = list(range(len(points)))
    triangles: list[tuple[int, int, int]] = []
    while len(remaining) > 3:
        for position, current in enumerate(remaining):
            previous = remaining[position - 1]
            following = remaining[(position + 1) % len(remaining)]
            if cross2(points[previous], points[current], points[following]) <= 1e-9:
                continue
            if any(
                point_in_triangle(points[index], points[previous], points[current], points[following])
                for index in remaining
                if index not in (previous, current, following)
            ):
                continue
            triangles.append((previous, current, following))
            del remaining[position]
            break
        else:
            raise ValueError("Could not triangulate foot profile")
    triangles.append(tuple(remaining))
    return triangles


def foot_profile() -> list[Point2]:
    angle = math.radians(LEAN_DEGREES)
    slope = math.tan(angle)
    horizontal_slot = SLOT_WIDTH_MM / math.cos(angle)
    horizontal_front_wall = FRONT_WALL_THICKNESS_MM / math.cos(angle)
    horizontal_rear_wall = REAR_WALL_THICKNESS_MM / math.cos(angle)

    front_inner_bottom = FRONT_SLOT_Y_MM
    rear_inner_bottom = front_inner_bottom + horizontal_slot
    front_outer_bottom = front_inner_bottom - horizontal_front_wall
    rear_outer_bottom = rear_inner_bottom + horizontal_rear_wall

    front_top_z = BASE_HEIGHT_MM + FRONT_WALL_HEIGHT_MM
    rear_top_z = BASE_HEIGHT_MM + REAR_WALL_HEIGHT_MM
    front_prebevel_z = front_top_z - TOP_CHAMFER_MM
    rear_prebevel_z = rear_top_z - TOP_CHAMFER_MM

    front_inner_prebevel = front_inner_bottom + (front_prebevel_z - BASE_HEIGHT_MM) * slope
    front_inner_top = front_inner_bottom + FRONT_WALL_HEIGHT_MM * slope
    front_outer_top = front_outer_bottom + FRONT_WALL_HEIGHT_MM * slope
    rear_inner_prebevel = rear_inner_bottom + (rear_prebevel_z - BASE_HEIGHT_MM) * slope
    rear_inner_top = rear_inner_bottom + REAR_WALL_HEIGHT_MM * slope
    rear_outer_top = rear_outer_bottom + REAR_WALL_HEIGHT_MM * slope

    # Trace the complete base and both cheeks. The gap is open at the top and
    # has a flat stop at base height. Chamfers enlarge the mouth of the slot.
    return [
        (0.0, 0.0),
        (BASE_DEPTH_MM, 0.0),
        (BASE_DEPTH_MM, BASE_HEIGHT_MM),
        (rear_outer_bottom, BASE_HEIGHT_MM),
        (rear_outer_top, rear_top_z),
        (rear_inner_top + TOP_CHAMFER_MM, rear_top_z),
        (rear_inner_prebevel, rear_prebevel_z),
        (rear_inner_bottom, BASE_HEIGHT_MM),
        (front_inner_bottom, BASE_HEIGHT_MM),
        (front_inner_prebevel, front_prebevel_z),
        (front_inner_top - TOP_CHAMFER_MM, front_top_z),
        (front_outer_top, front_top_z),
        (front_outer_bottom, BASE_HEIGHT_MM),
        (0.0, BASE_HEIGHT_MM),
    ]


def extrude_profile(profile: Sequence[Point2], x0: float, x1: float) -> list[Triangle]:
    caps = triangulate_polygon(profile)
    at_x0 = [(x0, y, z) for y, z in profile]
    at_x1 = [(x1, y, z) for y, z in profile]
    triangles: list[Triangle] = []

    for a, b, c in caps:
        triangles.append((at_x0[c], at_x0[b], at_x0[a]))  # outward -X
        triangles.append((at_x1[a], at_x1[b], at_x1[c]))  # outward +X

    for index in range(len(profile)):
        following = (index + 1) % len(profile)
        a0, b0 = at_x0[index], at_x0[following]
        a1, b1 = at_x1[index], at_x1[following]
        triangles.append((a0, b0, b1))
        triangles.append((a0, b1, a1))
    return triangles


def subtract(a: Point3, b: Point3) -> Point3:
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross3(a: Point3, b: Point3) -> Point3:
    return (
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    )


def normal(triangle: Triangle) -> Point3:
    a, b, c = triangle
    raw = cross3(subtract(b, a), subtract(c, a))
    length = math.sqrt(sum(value * value for value in raw))
    if length <= 1e-12:
        raise ValueError("Degenerate triangle in mesh")
    return tuple(value / length for value in raw)


def quantized(point: Point3) -> tuple[int, int, int]:
    return tuple(round(value * 1_000_000) for value in point)


def validate_mesh(triangles: Sequence[Triangle]) -> None:
    edge_counts: Counter[tuple[tuple[int, int, int], tuple[int, int, int]]] = Counter()
    for triangle in triangles:
        normal(triangle)
        vertices = [quantized(point) for point in triangle]
        for index in range(3):
            edge = tuple(sorted((vertices[index], vertices[(index + 1) % 3])))
            edge_counts[edge] += 1
    bad_edges = [edge for edge, count in edge_counts.items() if count != 2]
    if bad_edges:
        raise ValueError(f"Mesh is not watertight: {len(bad_edges)} non-manifold edges")

    axes = list(zip(*(point for triangle in triangles for point in triangle)))
    bounds = tuple((min(axis), max(axis)) for axis in axes)
    expected_x_max = FOOT_WIDTH_MM * 2 + PART_GAP_MM
    expected = ((0.0, expected_x_max), (0.0, BASE_DEPTH_MM), (0.0, BASE_HEIGHT_MM + REAR_WALL_HEIGHT_MM))
    for actual_axis, expected_axis in zip(bounds, expected):
        if any(abs(actual - target) > 1e-6 for actual, target in zip(actual_axis, expected_axis)):
            raise ValueError(f"Unexpected mesh bounds: {bounds}")


def format_vertex(point: Point3) -> str:
    return " ".join(f"{value:.6f}" for value in point)


def write_ascii_stl(path: Path, triangles: Iterable[Triangle]) -> None:
    with path.open("w", encoding="ascii", newline="\n") as output:
        output.write("solid crowpanel_dual_feet_14mm\n")
        for triangle in triangles:
            nx, ny, nz = normal(triangle)
            output.write(f"  facet normal {nx:.8f} {ny:.8f} {nz:.8f}\n")
            output.write("    outer loop\n")
            for point in triangle:
                output.write(f"      vertex {format_vertex(point)}\n")
            output.write("    endloop\n")
            output.write("  endfacet\n")
        output.write("endsolid crowpanel_dual_feet_14mm\n")


def main() -> int:
    repo_root = Path(__file__).resolve().parents[1]
    output = Path(sys.argv[1]).resolve() if len(sys.argv) > 1 else repo_root / "stand" / "crowpanel_dual_feet_14mm.stl"
    profile = foot_profile()
    triangles = extrude_profile(profile, 0.0, FOOT_WIDTH_MM)
    second_x0 = FOOT_WIDTH_MM + PART_GAP_MM
    triangles.extend(extrude_profile(profile, second_x0, second_x0 + FOOT_WIDTH_MM))
    validate_mesh(triangles)
    output.parent.mkdir(parents=True, exist_ok=True)
    write_ascii_stl(output, triangles)
    print(
        f"Wrote {output}\n"
        f"  2 feet, {len(triangles)} triangles, bounds "
        f"{FOOT_WIDTH_MM * 2 + PART_GAP_MM:.1f} x {BASE_DEPTH_MM:.1f} x "
        f"{BASE_HEIGHT_MM + REAR_WALL_HEIGHT_MM:.1f} mm\n"
        f"  slot {SLOT_WIDTH_MM:.2f} mm normal to screen, lean {LEAN_DEGREES:.1f} degrees"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
