"""Saves README screenshots of each feature from the scripted simulator demo (sim/src/demo.cpp).

Usage: python3 sim/make_screenshots.py   (needs the simulator build and Pillow; run from the project folder)
Writes PNGs to images/esp32-4848s040/. The times refer to the demo script, so update them if it changes.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGRAM = os.path.join(ROOT, ".pio", "build", "simulator", "program")
OUT = os.path.join(ROOT, "images", "esp32-4848s040")

# (video time in ms, file name): moments in the demo where each feature is on screen.
SHOTS = [
    (37500, "player"),
    (77500, "controls-hidden"),
    (42500, "queue"),
    (46800, "recently-played"),
    (52800, "track-details"),
    (58200, "devices"),
    (62500, "volume-greyed"),
    (67800, "playlists"),
    (83500, "device-info"),
    (87500, "settings"),
    (99500, "time-zone"),
    (106800, "settings-idle"),
    (116500, "clock"),
]


def main():
    if not os.path.exists(PROGRAM):
        sys.exit("Build the simulator first: pio run -e simulator")
    os.makedirs(OUT, exist_ok=True)
    work = tempfile.mkdtemp(prefix="spotify-shots-")
    env = dict(os.environ, SIM_DEMO="1", SIM_RECORD=os.devnull, SIM_CAPTIONS=os.devnull,
               SIM_SHOTS=",".join(f"{ms}:{name}" for ms, name in SHOTS), SIM_SHOTS_DIR=work)
    subprocess.run([PROGRAM], cwd=ROOT, env=env, check=True, stderr=subprocess.DEVNULL)
    for _, name in SHOTS:
        Image.open(os.path.join(work, name + ".bmp")).save(os.path.join(OUT, name + ".png"), optimize=True)
    print("Wrote", len(SHOTS), "screenshots to", OUT)


if __name__ == "__main__":
    main()
