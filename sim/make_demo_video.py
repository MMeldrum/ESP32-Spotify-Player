"""Records the scripted simulator demo (sim/src/demo.cpp) and turns it into an MP4 with captions.

Usage: python3 sim/make_demo_video.py [output.mp4]
Needs: the simulator build (pio run -e simulator), ffmpeg and Pillow. Run from the project folder.
"""
import os
import subprocess
import sys
import tempfile

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROGRAM = os.path.join(ROOT, ".pio", "build", "simulator", "program")
FONT_CANDIDATES = [
    os.path.join(ROOT, ".pio", "libdeps", "esp32-4848s040", "lvgl", "scripts", "built_in_font", "Montserrat-Medium.ttf"),
    os.path.join(ROOT, ".pio", "libdeps", "simulator", "lvgl", "scripts", "built_in_font", "Montserrat-Medium.ttf"),
    "/System/Library/Fonts/Helvetica.ttc",
]

SCREEN, SCALE = 480, 2
WIDTH, MARGIN_TOP = 1080, 50
SCREEN_X = (WIDTH - SCREEN * SCALE) // 2
CAPTION_Y = MARGIN_TOP + SCREEN * SCALE + 30
HEIGHT = CAPTION_Y + 200
BACKGROUND = (18, 18, 18)


def font(size):
    for path in FONT_CANDIDATES:
        if os.path.exists(path):
            return ImageFont.truetype(path, size)
    return ImageFont.load_default()


def caption_image(text, path):
    img = Image.new("RGBA", (WIDTH, HEIGHT - CAPTION_Y), BACKGROUND + (255,))
    d = ImageDraw.Draw(img)
    f = font(36)
    # "|" separates lines; each is also wrapped to fit the width.
    lines = []
    for part in text.split("|") if text else []:
        words, line = part.split(), ""
        for word in words:
            trial = (line + " " + word).strip()
            if d.textlength(trial, font=f) > WIDTH - 80 and line:
                lines.append(line)
                line = word
            else:
                line = trial
        lines.append(line)
    title = text.startswith("ESP32 Spotify Player")
    step = 52
    y = (img.height - step * len(lines)) / 2
    for i, line in enumerate(lines):
        w = d.textlength(line, font=f)
        d.text(((WIDTH - w) / 2, y), line, font=f, fill=(29, 185, 84) if title and i == 0 else (235, 235, 235))
        y += step
    img.save(path)


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "demo.mp4")
    if not os.path.exists(PROGRAM):
        sys.exit("Build the simulator first: pio run -e simulator")
    work = tempfile.mkdtemp(prefix="spotify-demo-")
    raw = os.path.join(work, "raw.mp4")
    caps = os.path.join(work, "captions.txt")

    # Pass 1: record. The simulator writes raw RGB frames to stdout, ffmpeg stores them losslessly enough.
    env = dict(os.environ, SIM_DEMO="1", SIM_RECORD="-", SIM_CAPTIONS=caps)
    sim = subprocess.Popen([PROGRAM], cwd=ROOT, env=env, stdout=subprocess.PIPE)
    enc = subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{SCREEN}x{SCREEN}",
         "-r", "30", "-i", "-", "-c:v", "libx264", "-preset", "medium", "-crf", "8", "-pix_fmt", "yuv444p", raw],
        stdin=sim.stdout)
    sim.wait()
    if sim.returncode or enc.returncode:
        sys.exit("Recording failed")

    # Pass 2: screen at 2x above a caption strip, one caption image per scripted caption.
    entries = []
    with open(caps) as f:
        for line in f:
            start, end, text = line.rstrip("\n").split(" ", 2)
            if text.strip():
                entries.append((int(start) / 1000, int(end) / 1000, text))
    # The captions become one timed image sequence (concat demuxer), overlaid once under the screen.
    blank = os.path.join(work, "blank.png")
    caption_image("", blank)
    concat = os.path.join(work, "captions.ffconcat")
    with open(concat, "w") as f:
        f.write("ffconcat version 1.0\n")
        now = 0.0
        for i, (start, end, text) in enumerate(entries):
            if start > now:
                f.write(f"file '{blank}'\nduration {start - now:.3f}\n")
            png = os.path.join(work, f"cap{i}.png")
            caption_image(text, png)
            f.write(f"file '{png}'\nduration {end - start:.3f}\n")
            now = end
        f.write(f"file '{blank}'\nduration 1\nfile '{blank}'\n")  # the last entry's duration needs a successor
    # Both streams start at 0, and the recording decides the length (shortest=1), so captions stay in sync.
    graph = (f"[0:v]setpts=PTS-STARTPTS,scale={SCREEN * SCALE}:{SCREEN * SCALE}:flags=neighbor,"
             f"pad={WIDTH}:{HEIGHT}:{SCREEN_X}:{MARGIN_TOP}:color=0x121212[base];"
             f"[1:v]setpts=PTS-STARTPTS,fps=30,format=rgba[caps];"
             f"[base][caps]overlay=0:{CAPTION_Y}:shortest=1:eof_action=endall[out]")
    # Composite in 4:4:4 first: ffmpeg 8.1 produced an odd (1241 px) frame when converting to 4:2:0 inside
    # this graph. The separate pass makes the widely playable yuv420p file.
    composed = os.path.join(work, "composed.mp4")
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", raw, "-f", "concat", "-safe", "0", "-i", concat,
                    "-filter_complex", graph, "-map", "[out]", "-shortest", "-c:v", "libx264", "-preset", "medium",
                    "-crf", "10", "-pix_fmt", "yuv444p", composed], check=True)
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", composed, "-c:v", "libx264", "-preset", "slow",
                    "-crf", "18", "-pix_fmt", "yuv420p", "-movflags", "+faststart", out], check=True)
    print("Wrote", out)


if __name__ == "__main__":
    main()
