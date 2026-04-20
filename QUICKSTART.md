# Quickstart — your first 5 minutes with imgtool

This walks you through opening a real MK-era `.IMG` file, understanding what
you're looking at, and making your first edit. If you've never used imgtool
before, start here. The in-app help (`h`) is a keyboard reference — this
document is the *conceptual* guide.

---

## What is an IMG file?

An IMG file is a **container of multiple sprites** (images) plus one or more
**palettes** that colorize them. A single IMG can hold hundreds of frames —
e.g. `NINJAS10.IMG` probably holds every animation frame for a ninja
character. When the game is built, these are packed into graphics ROMs.

Think of imgtool as: "a sprite-sheet editor where the sheet is an IMG file."

---

## Launch it

Double-click `imgtool.exe` (or run it from a shell). You'll see a mostly-black
640×400 window. It's waiting for you to load something.

---

## Step 1 — Open a file

Press **`l`** (lower-case L). A file browser appears.

- **Up/Down** — scroll through files
- **Enter** — open a directory, or load the selected file
- **Backspace** — go up one directory
- **Esc** — cancel

Navigate to `C:\DOSGames` and load **`NINJAS10.IMG`** (a ninja sprite sheet is
a good first example — lots of frames, easy to recognize).

---

## Step 2 — Read the screen

Once loaded, the window splits into four regions:

```
+------------------------------------+-------------------+
|                                    |                   |
|                                    |                   |
|          MAIN IMAGE VIEW           |   (info area)     |
|            (320x200)               |                   |
|     current sprite is drawn here   |                   |
|                                    |                   |
+-----------------+------------------+-------------------+
|                 |                                      |
|  PALETTE LIST   |           IMAGE LIST                 |
|  (pal names)    |      (all sprites in this IMG)       |
|                 |                                      |
+-----------------+--------------------------------------+
```

- **Main view** (top) — the currently-selected sprite, drawn at 1:1.
- **Image list** (bottom-right) — every sprite in the file. Shows ~19 at a
  time. An asterisk `*` next to a name means the sprite is *marked* (selected
  for batch operations).
- **Palette list** (bottom-left) — the palettes defined in this IMG. A sprite
  is colored by whichever palette is attached to it.
- **Info area** (right of palette) — animation-point coordinates (X/Y/Z/ID)
  for the currently-selected sprite. These tell the game engine where the
  sprite's "anchor" is (e.g. the ninja's feet).

---

## Step 3 — Browse sprites

- **Up / Down** — move one sprite at a time. Watch the main view update.
- **Page Up / Page Down** — jump a full page in the list.
- **L / R** — jump to the next/previous *marked* sprite (none yet).

You should see ninja frames flipping through the main view. Each entry in the
image list is a named sprite (e.g. `STAND01`, `KICK02`, etc. — names come
from the original artists).

---

## Step 4 — Mark some sprites

Marking is how you select a group for batch operations (assign-palette,
export-to-TGA, delete, etc).

- **Space** — toggle mark on the current sprite. You'll see `*` appear.
- **M** (shift) — mark *all* sprites.
- **m** (lower) — clear all marks.

Try marking 3–4 sprites with Space.

---

## Step 5 — Zoom

The main view defaults to 1:1. You can zoom in to see individual pixels:

- **d** — double size
- **D** (shift) — halve size
- **F12 / F11** — fine zoom in/out

Zoom back to default before doing anything with hitboxes — they only work at
1:1 view.

---

## Step 6 — The palette list

Press **`'`** (apostrophe) or **`/`** to scroll through palettes in the
bottom-left pane. Each entry is a named palette — a set of 256 colors that
can be applied to a sprite.

- **`]`** — assign the currently-selected palette to the current sprite.
- **`[`** — assign the palette to *all marked sprites*.
- **`t`** — toggle "true palette" display (shows the sprite using its
  assigned palette vs. a generic one).

This is where most of the practical work happens: MK artists drew sprites
once, then re-colorized them with different palettes per character/variant.

---

## Step 7 — Two IMGs at once (Tab)

imgtool can hold **two IMG files open simultaneously**. The image list you
see is "list 1." The second list is hidden but live.

- **Tab** — swap between list 1 and list 2. The image list pane now shows
  the other file's sprites.
- Load a second IMG (e.g. `CAGE1.IMG`) after hitting Tab, and now you can
  swap back and forth with Tab.

Why two lists? To **copy animation-point IDs between files**, overlay one
sprite on another for alignment, or compare. The **`i`** key grabs the ID
from list 2's selected sprite and assigns it to list 1's current sprite.

The **`2`** key draws list 2's current sprite on top of list 1's — useful
for lining up attached props (e.g. a weapon on a hand).

---

## Step 8 — Save

Press **`s`** to save changes back to the IMG. A prompt appears. Accept with
Enter.

> **Heads up:** if the file was an ancient pre-2.x IMG, imgtool will have
> converted it on load (you'll have seen a message box). Saving writes it
> back in the modern format.

---

## Things that are not obvious

- **The main menu is hidden.** Right-click the **very top row of pixels**
  of the window to open it. That's how you get to File > New, export-all,
  global operations, etc. This is a quirk inherited from the DOS version.

- **"Anipts" (animation points)** aren't visible boxes — they're coordinate
  numbers shown in the info area. Alt+Arrow moves the sprite's main anchor
  point; Ctrl+Arrow moves a secondary anchor. See the `h` help for the full
  list.

- **Hitboxes** are a separate overlay system with its own two-row button
  panel. Only visible at 1:1 zoom. See the Hitboxes section of [README.md](README.md).

- **LBM / TGA** — imgtool can import/export individual sprites as `.LBM` or
  `.TGA` files:
  - `Alt+l` / `Alt+s` — LBM load/save
  - `Ctrl+l` / `Ctrl+s` — TGA load/save
  - `Ctrl+b` — build one big TGA from all marked sprites

---

## Your first real task: dump a TGA

1. `l` → load `NINJAS10.IMG`.
2. Arrow-down to an interesting frame.
3. **Space** to mark it. Mark 5–6 good frames.
4. **Ctrl+b** — builds one TGA containing all marked frames tiled together.
5. Save the TGA somewhere and open it in any image viewer.

That round-trip (open IMG → pick frames → export TGA) is the most common
workflow for anyone doing asset extraction.

---

## Where to next

- Press **`h`** in the app for the full keyboard reference.
- Read [README.md](README.md) for hitboxes, environment variables, and
  file-format notes.
- Read [BUILD.md](BUILD.md) if you want to build from source.
