#!/usr/bin/env python3
"""render_ship_atlas.py — orbit a camera around a ship and capture reference renders.

Requires the game to be running with dev_remote on port 47001 and the ship
placed at the world origin. Outputs cropped PNG screenshots + manifest.json
to the specified output directory.

Usage:
    python3 tools/render_ship_atlas.py --ship tarsus --radius 35 --outdir assets/ships/tarsus/renders

The 32 samples are a uniform 8-azimuth × 4-elevation grid on the viewing
sphere, matching the Privateer-grade sprite resolution discussed in design.
Conventions (verified empirically against the rendered references — DO NOT
trust intuition; the engine's coordinate system is sokol Y-down + ships
authored with +Z forward, which inverts both axes vs typical OpenGL):

    az = 0    →  camera IN FRONT of the ship  (looking at the nose head-on)
    az = 90   →  camera to the ship's PORT side (left flank visible)
    az = 180  →  camera BEHIND the ship       (looking at the rear engines)
    az = 270  →  camera to the ship's STARBOARD side (right flank visible)
    el > 0    →  camera BELOW the ship        (looking UP at the ventral hull)
    el < 0    →  camera ABOVE the ship        (looking DOWN at the dorsal hull)

For a long time the docstring claimed the OPPOSITE ("az=0 = stern view, el>0
= above"), and the prompt code in batch_generate_ship_sprites.py inherited
that lie. The first three centurion waves wasted real money before someone
eyeballed an actual reference image and noticed the cockpit kept appearing
on the underside. So: be empirical. Open a render. Look at it. Then prompt.
"""

import argparse
import json
import math
import os
import time
import urllib.request

API = "http://127.0.0.1:47001"
SHOT_PATH = "/tmp/np_shot.png"

# Legacy 32 sample grid: 8 azimuths × 4 elevations. For ship QA we now
# usually want the 80-sample grid: --az-count 16 --elevations -60,-30,0,30,60
AZIMUTHS   = [0, 45, 90, 135, 180, 225, 270, 315]
ELEVATIONS = [-67.5, -22.5, 22.5, 67.5]


def evenly_spaced_azimuths(count: int) -> list[float]:
    return [i * 360.0 / count for i in range(count)]


def angle_tag(value: float) -> str:
    if abs(value - round(value)) < 0.01:
        return f"{int(round(value)):+04d}"
    sign = "+" if value >= 0 else "-"
    return sign + f"{abs(value):04.1f}".replace(".", "p")


def az_tag(value: float) -> str:
    if abs(value - round(value)) < 0.01:
        return f"{int(round(value)):03d}"
    return f"{value:05.1f}".replace(".", "p")


def parse_elevations(s: str) -> list[float]:
    return [float(part.strip()) for part in s.split(",") if part.strip()]


def api_post(endpoint: str, data: dict | None = None) -> None:
    """Fire-and-forget POST to the game's dev_remote API."""
    url = f"{API}{endpoint}"
    body = json.dumps(data).encode() if data else b""
    req = urllib.request.Request(url, data=body, method="POST")
    with urllib.request.urlopen(req, timeout=5) as resp:
        resp.read()


def set_camera(x: float, y: float, z: float, yaw: float, pitch: float) -> None:
    """Teleport the camera to (x, y, z) facing (yaw, pitch) degrees."""
    api_post("/camera/set", {"x": x, "y": y, "z": z, "yaw": yaw, "pitch": pitch})


def take_screenshot() -> str:
    """Trigger a screenshot and return the path to the saved file."""
    # Remove stale file first
    if os.path.exists(SHOT_PATH):
        os.remove(SHOT_PATH)
    api_post("/screenshot")
    # Wait for the file to appear (written at end-of-frame)
    for _ in range(30):
        if os.path.exists(SHOT_PATH) and os.path.getsize(SHOT_PATH) > 0:
            return SHOT_PATH
        time.sleep(0.1)
    raise TimeoutError("screenshot never appeared")


def camera_euler_to_look_at_origin(px: float, py: float, pz: float):
    """Compute (yaw, pitch) in degrees so the camera at (px,py,pz) faces the origin.

    Matches the engine's quaternion-from-euler convention used by /camera/set:
      forward = (-sin(yaw)*cos(pitch), sin(pitch), -cos(yaw)*cos(pitch))
    """
    r = math.sqrt(px * px + py * py + pz * pz)
    yaw_deg   = math.degrees(math.atan2(px, pz))
    pitch_deg = math.degrees(math.asin(-py / r)) if r > 1e-6 else 0.0
    return yaw_deg, pitch_deg


def orbit_position(az_deg: float, el_deg: float, radius: float):
    """Camera position on the viewing sphere at (azimuth, elevation, radius).

    Convention:
      az=0   → camera on +Z axis (behind the ship, seeing stern)
      az=90  → camera on +X axis (seeing the port/left side)
      az=180 → camera on -Z axis (in front, seeing the nose)
      el>0   → camera above the ship
      el<0   → camera below the ship
    """
    az = math.radians(az_deg)
    el = math.radians(el_deg)
    x = radius * math.cos(el) * math.sin(az)
    y = radius * math.sin(el)
    z = radius * math.cos(el) * math.cos(az)
    return x, y, z


def crop_center(input_path: str, output_path: str, frac: float = 0.55, flip_y: bool = False) -> None:
    """Crop to the central `frac` of the image (removes HUD in corners).

    `flip_y` corrects the capture/reference orientation. The runtime sprite
    shader has its own texture-origin fix, but reference images are plain PNGs
    fed to the pixel generator, so they need to be upright on disk. Do it here,
    not in camera math, because camera roll is a delightful bucket of snakes.
    """
    from PIL import Image, ImageOps
    im = Image.open(input_path)
    w, h = im.size
    margin_x = int(w * (1 - frac) / 2)
    margin_y = int(h * (1 - frac) / 2)
    cropped = im.crop((margin_x, margin_y, w - margin_x, h - margin_y))
    if flip_y:
        cropped = ImageOps.flip(cropped)
    cropped.save(output_path, "PNG")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ship", default="tarsus", help="ship name (for filenames)")
    parser.add_argument("--radius", type=float, default=35.0,
                        help="orbit radius in game units (metres)")
    parser.add_argument("--outdir", default="assets/ships/tarsus/renders",
                        help="output directory for cropped PNGs + manifest")
    parser.add_argument("--settle", type=float, default=0.4,
                        help="seconds to wait after camera move before screenshot")
    parser.add_argument("--az-count", type=int, default=None,
                        help="Number of evenly-spaced azimuth samples, e.g. 16 for 22.5-degree steps")
    parser.add_argument("--elevations", default=None,
                        help="Comma-separated elevations, e.g. -60,-30,0,30,60 for an 80-sample atlas "
                             "with --az-count 16. Include 90 and/or -90 to also capture pole cells "
                             "(one canonical az=0 image per pole; all azimuths at the pole see the "
                             "same view by rotational symmetry).")
    parser.add_argument("--flip-y", action="store_true",
                        help="Vertically flip cropped reference PNGs so the ship is upright for image generation")
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)

    azimuths = evenly_spaced_azimuths(args.az_count) if args.az_count else AZIMUTHS
    elevations = parse_elevations(args.elevations) if args.elevations else ELEVATIONS

    # Pole elevations (el = ±90°) are rotational symmetry axes: from
    # directly above (or below) the ship, every azimuth sees the same
    # physical view, just rotated in image space. So we capture one
    # canonical az=0 image per pole instead of N redundant identical
    # screenshots. The engine's cap_up alignment math handles the
    # in-plane rotation at runtime regardless of which az was authored.
    #
    # Building the capture list up-front rather than nesting the loops
    # keeps the [idx/total] counter honest when elevations and pole
    # entries mix (an N×M product would lie about the total).
    captures: list[tuple[float, float]] = []
    for el in elevations:
        is_pole = abs(abs(el) - 90.0) < 0.01
        az_list = [0.0] if is_pole else azimuths
        for az in az_list:
            captures.append((az, el))
    total = len(captures)

    samples = []
    for idx0, (az, el) in enumerate(captures):
        idx = idx0 + 1
        px, py, pz = orbit_position(az, el, args.radius)
        yaw, pitch = camera_euler_to_look_at_origin(px, py, pz)

        print(f"[{idx:2d}/{total}]  az={az:5.1f}  el={el:+6.1f}  "
              f"cam=({px:+7.1f}, {py:+7.1f}, {pz:+7.1f})  "
              f"yaw={yaw:+7.1f}  pitch={pitch:+7.1f}")

        set_camera(px, py, pz, yaw, pitch)
        time.sleep(args.settle)

        shot = take_screenshot()
        time.sleep(0.15)   # let the file flush

        filename = f"{args.ship}_az{az_tag(az)}_el{angle_tag(el)}.png"
        outpath = os.path.join(args.outdir, filename)
        crop_center(shot, outpath, flip_y=args.flip_y)

        samples.append({
            "az": az, "el": el,
            "file": filename,
        })
        print(f"         → {outpath}")

    manifest = {
        "ship": args.ship,
        "samples": samples,
        "orbit_radius": args.radius,
        "azimuths": azimuths,
        "elevations": elevations,
        "count": len(samples),
    }
    manifest_path = os.path.join(args.outdir, "manifest.json")
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)
    print(f"\n✅ {len(samples)} renders saved to {args.outdir}/")
    print(f"   manifest: {manifest_path}")


if __name__ == "__main__":
    main()
