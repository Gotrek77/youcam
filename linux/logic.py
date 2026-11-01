"""Pure logic shared by the Linux app. Run `python3 logic.py` for the self-check.

Coordinates are X11/GDK style: origin top left, y grows downwards.
"""
import math
import os
import time

MIN_WIDTH, MAX_WIDTH = 120, 960
DEFAULT_WIDTH = 280
EDGE_MARGIN = 32
MIN_REGION = 40
SHAPES = {"circle": ("Circle", 1.0), "square": ("Square", 1.0), "rectangle": ("Rectangle (16:9)", 16 / 9)}
SIZES = [("Small", 180), ("Medium", 280), ("Large", 400), ("Extra Large", 560)]
GAIN_DB_RANGE = (-36.0, 12.0)


def overlay_size(width, shape, screen):
    """screen = (x, y, w, h). Returns (w, h) clamped to limits and screen."""
    aspect = SHAPES[shape][1]
    limit = min(MAX_WIDTH, screen[2], screen[3] * aspect)
    safe = width if math.isfinite(width) else DEFAULT_WIDTH
    w = round(min(max(safe, MIN_WIDTH), limit))
    return w, round(w / aspect)


def constrain(frame, screen):
    x, y, w, h = frame
    sx, sy, sw, sh = screen
    return max(sx, min(x, sx + sw - w)), max(sy, min(y, sy + sh - h)), w, h


def resized(frame, width, shape, screen):
    """Keep the top-left corner still when changing size or shape."""
    w, h = overlay_size(width, shape, screen)
    return constrain((frame[0], frame[1], w, h), screen)


def initial(width, shape, screen):
    """Bottom-right corner of the screen, with a margin."""
    w, h = overlay_size(width, shape, screen)
    sx, sy, sw, sh = screen
    return constrain((sx + sw - w - EDGE_MARGIN, sy + sh - h - EDGE_MARGIN, w, h), screen)


def normalize_rect(x0, y0, x1, y1):
    return min(x0, x1), min(y0, y1), abs(x1 - x0), abs(y1 - y0)


def region_fractions(rect, monitor):
    """Monitor-local rect in logical pixels -> fractions (left, top, width, height).

    Fractions survive HiDPI scaling: the portal stream may be any pixel size."""
    x, y, w, h = rect
    mw, mh = monitor[2], monitor[3]
    if w < MIN_REGION or h < MIN_REGION:
        return None
    return x / mw, y / mh, w / mw, h / mh


def valid_fractions(f):
    return (isinstance(f, (list, tuple)) and len(f) == 4
            and all(isinstance(v, (int, float)) and math.isfinite(v) for v in f)
            and f[0] >= 0 and f[1] >= 0 and f[2] > 0 and f[3] > 0
            and f[0] + f[2] <= 1.0001 and f[1] + f[3] <= 1.0001)


def crop_pixels(fractions, frame_w, frame_h):
    """Fractions -> videocrop (left, right, top, bottom); output size kept even for x264."""
    fl, ft, fw, fh = fractions
    left, top = round(fl * frame_w), round(ft * frame_h)
    w = max(2, int(fw * frame_w) // 2 * 2)
    h = max(2, int(fh * frame_h) // 2 * 2)
    w, h = min(w, frame_w - left), min(h, frame_h - top)
    return left, frame_w - left - w, top, frame_h - top - h


def next_recording_path(directory, now=None):
    stamp = time.strftime("%d%b-%H.%M.%S", time.localtime(now))
    base = os.path.join(directory, f"YouCam-Recording-{stamp}")
    path, suffix = base + ".mp4", 2
    while os.path.exists(path):
        path, suffix = f"{base}-{suffix}.mp4", suffix + 1
    return path


def db_to_gain(db):
    lo, hi = GAIN_DB_RANGE
    return 10 ** (min(hi, max(lo, db)) / 20)


def demo():
    screen = (0, 0, 1920, 1080)
    assert overlay_size(10, "circle", screen) == (120, 120)
    assert overlay_size(5000, "rectangle", screen) == (960, 540)
    assert overlay_size(float("nan"), "square", screen) == (280, 280)
    assert initial(280, "circle", screen) == (1920 - 280 - 32, 1080 - 280 - 32, 280, 280)
    assert constrain((1900, -50, 200, 200), screen) == (1720, 0, 200, 200)
    assert resized((100, 100, 280, 280), 400, "rectangle", screen) == (100, 100, 400, 225)
    assert normalize_rect(50, 60, 10, 20) == (10, 20, 40, 40)
    assert region_fractions((0, 0, 10, 10), screen) is None
    f = region_fractions((480, 270, 960, 540), screen)
    assert valid_fractions(f) and not valid_fractions([0.5, 0.5, 0.6, 0.1])
    # HiDPI: same fractions on a 2x stream crop the same logical area.
    assert crop_pixels(f, 3840, 2160) == (960, 960, 540, 540)
    left, right, top, bottom = crop_pixels((0.1, 0.1, 0.333, 0.333), 1001, 999)
    assert (1001 - left - right) % 2 == 0 and (999 - top - bottom) % 2 == 0
    assert crop_pixels((0.9, 0.9, 0.1, 0.1), 100, 100) == (90, 0, 90, 0)
    assert abs(db_to_gain(0) - 1) < 1e-9 and abs(db_to_gain(100) - db_to_gain(12)) < 1e-9
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        first = next_recording_path(d, 0)
        open(first, "w").close()
        assert next_recording_path(d, 0) == first[:-4] + "-2.mp4"
    print("logic ok")


if __name__ == "__main__":
    demo()
