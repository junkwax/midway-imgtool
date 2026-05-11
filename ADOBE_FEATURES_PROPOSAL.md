# Adobe-Style Creative Features & Workflow Proposals

Based on a deep dive into the current feature set of `midway-imgtool`, the editor has successfully evolved from a low-level ROM packer into a capable, graphical pixel art tool. The recent additions of the Marquee tool, Photoshop-like cut/paste, and hierarchical layer-like image list have set a strong foundation. 

To further lean into the "Adobe-like" magic and enhance the artistic workflow, here are several proposed features—starting with the automatic sprite chopping you mentioned.

## 1. Auto-Sprite Chopper (Smart Partitioning)
**The Problem:** Original Midway arcade hardware (like the TMS34010) has maximum sprite size limitations (e.g., 256x256) and memory is at a premium. Large bosses (like Goro or Kintaro) or big background elements had to be manually chopped into pieces (`_1A`, `_1B`, `_1C`, etc.).
**The "Magic" Solution:** 
- A tool or default export option that takes a large, single-piece sprite and automatically partitions it into hardware-compliant chunks.
- **Smart Trimming:** The algorithm shouldn't just blindly cut a grid; it should trim the transparent bounds of each resulting chunk to save VRAM and generate the correct `ANIX`/`ANIY` (animation anchor points) so they assemble perfectly in-game.
- **Auto-Naming:** Automatically suffixes the chunks (1A, 1B) and links them via a generated `.TBL` or `.LOD` macro.

## 2. Magic Wand & Lasso Selection Tools
**The Problem:** Currently, the editor only supports a rectangular Marquee selection (`R`). Characters and sprites are organically shaped.
**The "Magic" Solution:**
- **Magic Wand (`W`):** Click to select contiguous blocks of a specific palette index. Add a "Tolerance" slider to select similar colors, or a "Contiguous" checkbox to select all pixels of that color globally across the sprite.
- **Lasso (`L`):** Freehand draw around a character's limb to select it for rotation or detachment. 
- *Integration:* These would tie directly into the existing floating paste buffer and clipboard mechanics.

## 3. Clone Stamp Tool
**The Problem:** Cleaning up digitized video frames (like MK actors) requires copying skin textures or fabric patterns from one area to another to cover up rigging or artifacts.
**The "Magic" Solution:**
- **Clone Stamp (`S`):** `Alt+Click` to set a source anchor, then paint elsewhere on the canvas to clone pixels. This is the industry standard for photographic touch-ups and is highly applicable to digitized arcade sprites.

## 4. Background Eraser / Magic Chroma Key
**The Problem:** When importing fresh digitized frames or PNGs, artists spend a lot of time manually erasing the blue/green screen background.
**The "Magic" Solution:**
- A "Magic Eraser" or "Remove Background" button. It analyzes the edges or a user-selected chroma color and automatically floods it with index 0 (transparency).
- Could include a simple edge-feathering or "defringe" algorithm to clean up blue spill on the edges of the actor, mapping those edge pixels to the nearest matching palette color of the character.

## 5. Timeline & Playback Panel
**The Problem:** The current "World View" allows manual frame-by-frame scrubbing with up/down arrows and onion skinning to align `ANIX`/`ANIY` points.
**The "Magic" Solution:**
- A dedicated **Animation Timeline** at the bottom of the screen (similar to Photoshop's frame animation panel or Aseprite).
- Hit `Spacebar` to play the animation loop in real-time at a targeted FPS (e.g., 55fps for MK).
- Easily drag and drop frames in the timeline to reorder them without changing the underlying `.IMG` file order just yet.

## 6. Smart Palette Remapper (Color Replace Brush)
**The Problem:** Altering palettes globally is handled by the hue shifter, but repainting specific areas with a different color ramp (e.g., changing Sub-Zero's blue gi to Scorpion's yellow without affecting blue ice in the same image) is tedious.
**The "Magic" Solution:**
- **Color Replace Brush:** Paint over the image, and it only affects pixels within a specific hue range, mapping them to the target palette indices.
- **Indexed Gradient Map:** Select a region (using the Magic Wand) and apply a gradient map that automatically snaps to the closest palette indices.

## 7. Smart Alignment ("Snap to Content")
**The Problem:** Aligning `1A` and `1B` pieces manually in World View can be pixel-hunting work.
**The "Magic" Solution:**
- When dragging an image or a pasted floating selection, hold `Shift` to snap the edges of the bounding box, or the literal non-transparent pixel edges, to adjacent sprites.

---
### Next Steps
The **Auto-Sprite Chopper** is the most highly specialized and valuable feature for this specific retro-hardware workflow. If you agree, we can begin architecting the `ChopImageToGrid()` algorithm and wire it into the Operations menu!