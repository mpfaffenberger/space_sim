"""wcnews_layout.py — frame index → (az, el, mirror_h) mapping for the
canonical 1993 Privateer sprite-sheet layout used by all 3-Space ships
on the wcnews wcpedia.

Reverse-engineered from TALMIL on the "3Space Ship Sprite Archive"
page (37 frames per ship). Two known anchors from Mike, by direct
inspection of the pixel art:

    frame 15 = nose head-on (az=180, el=0)        [confirmed]
    frame 21 = dead astern   (az=  0, el=0)       [confirmed]

Frame 15 → 21 is six steps = 180° rotation, so the el=0 ring rotates
30° per frame. That makes the equator a single 12-frame ring covering
0..330° at 30° steps, with no horizontal mirroring needed there.

Off-equator rings are drawn as 7 frames covering yaws 0°→180° at 30°
steps; the engine mirrors horizontally at runtime to cover 210°→330°
(classic WC art-saving trick — symmetric ships, free pixels).

The bottom polar cap (el=-60) is given a coarser 4-frame layout because
gameplay rarely shows a ship from straight underneath; that's how 7 +
7 + 12 + 7 + 4 sums to exactly the 37 frames the wiki ships:

    el=+60 ring : frames 0..6   (7 yaws × 30°,  mirror)
    el=+30 ring : frames 7..13  (7 yaws × 30°,  mirror)
    el=  0 ring : frames 14..25 (12 yaws × 30°, no mirror, full 360°)
        14: yaw 150°   15: 180° (nose)   16: 210°   17: 240°
        18: yaw 270°   19: 300°          20: 330°   21:   0° (rear)
        22: yaw  30°   23:  60°          24:  90°   25: 120°
    el=-30 ring : frames 26..32 (7 yaws × 30°,  mirror)
    el=-60 ring : frames 33..36 (4 yaws × 60°,  mirror)

The mapping below records (az_deg, el_deg, mirror_h_at_runtime). The
mirror flag is *always False here* — for any non-equator yaw the engine
will detect the request for az ∈ (180°, 360°) and pull the matching
0..180° frame with a horizontal flip. The flag is reserved for future
use should we ever want to author the mirror-half explicitly.
"""

from __future__ import annotations

from typing import Dict, Tuple

FRAMES_PER_SHIP = 37

# (az_deg, el_deg, mirror_h_authored)
FrameSpec = Tuple[int, int, bool]


def _build_mapping() -> Dict[int, FrameSpec]:
    out: Dict[int, FrameSpec] = {}

    # el=+60 ring (7 frames covering yaws 0..180 at 30° steps)
    for i in range(7):
        out[i] = (i * 30, 60, False)

    # el=+30 ring (7 frames)
    for i in range(7):
        out[7 + i] = (i * 30, 30, False)

    # el=0 ring (12 frames covering full 360°). Frame 15 anchors at yaw 180°.
    EQUATOR_START = 14
    EQUATOR_AZ_AT_START = 150
    for i in range(12):
        out[EQUATOR_START + i] = ((EQUATOR_AZ_AT_START + i * 30) % 360, 0, False)

    # el=-30 ring (7 frames)
    for i in range(7):
        out[26 + i] = (i * 30, -30, False)

    # el=-60 cap (4 frames at coarser 60° steps)
    for i in range(4):
        out[33 + i] = (i * 60, -60, False)

    assert set(out.keys()) == set(range(FRAMES_PER_SHIP)), out.keys()
    # Anchor sanity checks — these should never break silently.
    assert out[15][:2] == (180, 0), out[15]
    assert out[21][:2] == (0,   0), out[21]
    return out


_FRAME_TO_AZ_EL: Dict[int, FrameSpec] = _build_mapping()


def frame_to_az_el(frame_idx: int) -> FrameSpec:
    """Return (az_deg, el_deg, mirror_h_authored) for a wcnews frame index."""
    return _FRAME_TO_AZ_EL[frame_idx]


def all_frames() -> Dict[int, FrameSpec]:
    """Return a copy of the full mapping, frame_idx → spec."""
    return dict(_FRAME_TO_AZ_EL)
