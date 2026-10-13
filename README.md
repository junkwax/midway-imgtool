# midway-imgtool

Sprite and palette editor for the IMG container files used by 1990s Midway arcade games — Mortal Kombat II/3, NBA Jam, NBA Hangtime, and others. Loads, edits, and saves `.IMG` libraries plus their LBM, TGA, and PNG siblings, preserving proprietary point-table and animation data from the original tools.

![imgtool image](https://raw.githubusercontent.com/junkwax/midway-imgtool/SDL-main/imgtool.png)

> **Status:** pure C/C++ port. Originally a 1992 DOS tool with ~34,000 lines of x86 assembly — now fully rewritten in C++ with SDL2 + Dear ImGui. No assembler, no DOSBox, no virtualization. Runs on Windows (x64/x86), Linux (x64), and macOS (Intel + Apple Silicon).

---

## Studio preview (separate application)

`imgtool-studio.exe` is a new artwork and animation-alignment editor. Classic
`imgtool.exe` keeps its existing interface and workflows; `imgtool-cli.exe`
remains the automation entry point. All three are built together.

Studio opens with a compact home screen: Open IMG, New library, Explore sample,
a saved-project shortcut, and recent libraries with folder paths and hover details.
Unavailable recent files can be removed from the list without deleting files.
Studio has a red-accented workspace with a searchable thumbnail library,
resizable canvas and inspector, new/rename/duplicate/delete/reorder commands,
sprite resize/canvas-size/trim/rotate/flip with reviewed batch geometry and parent/child restoration, reviewed LOD library loading, PNG import/export, reviewed sprite-sheet/GIF imports,
pencil/erase/clone/variant/blur/smudge/heal/defringe, line previews, color replacement/matching fill and rectangle/wand/lasso selection tools, palette-aware
copy/paste with a placement preview, shared palette editing, and animation
anchor controls with engine-aware flip previews. The Animation & alignment
workspace adds a fixed playfield, floor guide, onion skin, multipart
reconstruction, timeline playback/scrubbing, and references from open libraries.
Studio projects add authored actor lanes and cross-file frames, saved timing,
placement and facing, an independent scene history, and current-scene PNG export.
Each open library owns its
undo history, including geometry, sprite/palette metadata, and selection.
Stable sprite identities keep preview references attached when the list changes.
Reordering updates embedded sequence image indices; deletion refuses to remove
a referenced sprite. Saves validate a temporary file before replacing the destination;
failed saves leave the document dirty, and external file changes are detected.

This is an **artwork editing preview**, not feature parity with classic. It
currently accepts validated IMG layouts from version `0x634` through `0x654`.
Older layouts and legacy-format imports, ASM animation,
full script execution, advanced WAX rules, and game-source editing remain in classic.
Studio exposes embedded sequence frames and script calls while preserving their
other metadata.

**Side toolbar and color wheel:** the left rail contains the 18 migrated pixel
tools as icons with names, shortcuts and instructions on hover. The active tool
uses Studio's red highlight; its settings remain in the inspector. A hue wheel
and saturation/value triangle sit directly below the tools, editing the active
palette index. The swatch shows that index; **0** toggles to transparency and back
to the last opaque index. Index zero is locked against color edits.

Click the swatch or **Edit...** for larger RGB/HSV/hex controls. Colors are stored
as RGB555 and shared-palette edits affect every sprite using the entry. Each drag
is one undo step; Escape or Undo cancels an unfinished wheel drag. Palette state
belongs to each library and follows sprite/swatch changes. Pending pixel previews
disable wheel edits. The existing indexed swatches remain below the canvas.
Keep the executable's **assets** folder beside it for the bundled toolbar icons;
short labels provide a fallback when the font is unavailable.

**Painting:** Pencil/Erase/Line/Replace/Clone/Variant/Blur/Smudge/Heal/Defringe share round or square brushes, a radius
control and **[ / ]** size shortcuts. Fast strokes interpolate continuously.
**Line (K)** previews a straight stroke without changing the IMG. Hold Shift to
snap horizontal, vertical or diagonal; release to apply one undo step, or press
Escape/Undo to cancel (including while Shift is held). The preview reports the
number of changing pixels. A changed source discards the pending line.

**Replace (B)** picks its source index at the start of a stroke and replaces only
matching pixels under the brush with the selected palette index. Crossing another
color does not change the source. Enable **All matching pixels** for a click to
replace throughout the sprite or active selection. **Fill (G)** and **Smart erase
(U)** support connected or global matching; smart erase writes transparency.
By default matching uses the exact index. Enable **Match palette colors** to use
RGB555 color distance and tolerance, including duplicate colors in different
slots. Transparency remains separate from opaque black. All tools restrict writes
to the selection; connected fills cannot travel through excluded pixels.
These edits retain palette words, anchors, points and other sprite metadata.
**Clone (C)** uses **Alt-click** to choose a source on the current sprite, then
copies it under the brush as you drag. A gold marker shows the sampled position;
red indicates sampling outside the sprite, where pixels are skipped. Each stroke
freezes the source, so overlapping repairs do not feed back into themselves.
**Aligned strokes** keeps the offset between strokes; disable it to start each
stroke at the chosen source. **Copy transparent pixels** explicitly allows source
transparency to erase the destination (off by default). Clone writes respect the
selection, including holes; sampling can read outside the selection. Release
commits one undo step; Escape restores the unfinished stroke and keeps its
selection. Switching sprites or changing dimensions clears the source. This first
clone workflow samples within one sprite; cross-sprite copying uses the clipboard.
**Variant (T)** recolors painted pixels in the current palette while preserving
those pixels' colors under the library's other palettes. Choose an opaque target
swatch, drag to preview, then **Apply/Enter** or **Cancel/Escape**. **Preview selected
pixels** applies the same review to a rectangle/wand/lasso selection. Transparent
pixels stay transparent and selection holes stay untouched.

The review reports changing pixels, reused/assigned slots, palettes that change
or grow, and color counts crossing the chosen LOAD2 PPP packing limit. **Show
original** and the palette dropdown compare appearances without changing the IMG.
Slots unused by all current sprites and their saved baselines may be repurposed;
existing painted colors stay intact. Missing alternate color entries are treated
as black. Palette growth raises addressing depth as needed. No suitable slots
blocks the whole edit. Apply changes pixels and palettes together in one undo
step; cancel/Undo discards the pending preview. Source or workspace changes close
an unapplied preview.

**Blur (O)** softens neighboring colors under the brush. Every stroke samples one
frozen source and applies each covered pixel once; start another stroke to build
up the effect. **Smudge (S)** pulls paint from the previous brush position as you
drag. Fast drags process each pixel interval; the initial click alone does not
change pixels. Both tools have a **Strength** control and blend actual RGB555
colors, then choose an existing opaque palette entry. They preserve transparent
pixels and sprite outlines, and restrict both sampling and painting to the active
selection, including holes. Sparse palettes can limit the visible result. Palette
entries, geometry, anchors and other sprite metadata stay intact. Release commits
one undo step; Escape restores the unfinished stroke and keeps its selection.
**Heal (J)** replaces a brushed blemish by carrying surrounding colors inward.
The accumulated brushed area is recomputed from one frozen source per stroke;
repairs do not sample earlier changes. **Fill depth** controls how far color can
travel from the rim. Pixels without usable surrounding colors stay unchanged.
This is local color diffusion using the existing palette.

**Defringe (F)** cleans outline color spill from the sprite interior. Drag to
clean locally, or click **Clean all edges / Clean selected edges** in the
inspector. **Edge width** chooses a 1-4 pixel band; canvas borders count as edges.
Thin features without an interior color source stay unchanged. Both repair tools
preserve transparency and geometry, and selections limit sampling and writes.
Release groups a stroke into one undo step; Escape cancels it and keeps its mask.

**Smart erase (U)** now has **Clean erased edges** enabled by default. In the
same undo step as erasure, surviving adjacent pixels adopt safe neighboring
colors. Cleanup only uses local colors outside the erased/matching region and
inside the selection; it does not erase additional pixels. Turn it off for a plain
matching erase. Clicking existing transparency does nothing.

**Batch geometry:** library checkboxes mark sprites independently of the active
sprite and pixel selection. **Marks** can mark every sprite shown by search or
clear membership; the adjacent count shows active marks. Marks belong to each
open library and follow stable identities through reorder and Undo. They are
session view state and do not write IMG flags or persist after closing a library.

Click **Batch...** or **Sprite > Batch geometry...**, then choose Marked sprites,
Shown by search, or All sprites in the current library. The review supports trim,
resize by percent or exact dimensions (nearest/palette-aware smooth), canvas
size with nine placements, and five rotate/flip operations. Anchor/point options
follow the existing single-sprite commands. Trim and canvas placement rebase
anchors and point tables; entirely transparent trims keep their original canvas.

Select a row to compare **Before / After** at the same world anchor target.
The table shows old/new sizes and cropped visible pixel counts; primary anchors
appear below it, with secondary/point details on hover. Uncheck rows to exclude
them. Finish editing a control to refresh its preview; Apply waits for matching
settings and valid limits. **Apply batch** changes all enabled rows in one Undo
step. **Cancel / Escape** discards the preview. These operations edit whole
sprites. Palettes, saved baselines, raw names and embedded references are retained;
geometry changes can affect LOAD2 packing. Pipeline exports remain pending.

**Sprite > Restore child sprites...** reviews parent-to-child restoration in the
current library. Choose Marked children, Shown by search, or All sprites; parents
can be outside the child scope. **Name rule** uses one capture to find each parent:
`^(.+)[A-Z]$` maps `WALK1A` to `WALK1`. Matching ignores ASCII case, and ambiguous
parent names are reported. Patterns are limited to 128 characters, one capture,
character classes and simple wildcards; counted repeats and repeated groups are
not supported. **One parent** instead offers a searchable picker showing dimensions
and library position. Build the preview after changing settings.

Four modes preserve the original restoration meanings:

- **Reconstruct opaque pixels** copies differing parent pixels only where both
  parent and child are opaque, preserving both silhouettes.
- **Propagate parent edits (Diff)** copies indices that differ from the parent's
  load-time baseline, including transparency. It needs a baseline of the same
  dimensions; new or resized parents may not have one. Palette color changes alone
  are not pixel-index changes.
- **Replace child from parent** clears the child's visible pixels, then copies
  the anchor-aligned parent overlap. Uncovered child art is cleared.
- **Fill child transparency** copies opaque parent pixels into transparent child
  pixels, retaining existing child detail.

Every row shows parent, coverage and changed pixels. Palette mismatches, missing
or ambiguous parents, missing Diff baselines, self matches and no overlap are
excluded. Partial pairs start unchecked; tick a row to include it deliberately.
Changing restoration settings resets partial inclusion. **Before / After** compares
owned results, and **Changes** highlights edits in gold and erasure in pink.
**Parent context** draws child bounds in gold and uncovered areas in red. Added
and erased pixel counts show when silhouette changes may affect LOAD2 packing.

**Apply restoration** commits all included children in one Undo step. Frozen
parent snapshots make the result independent of processing order, even when a
parent is another child in the batch. **Cancel / Escape** discards the review;
source changes require a fresh preview. Geometry, anchors, palette words, points,
alternate tables, saved baselines, raw names, padding and embedded references
remain intact. Whole child sprites are edited; pixel selections remain view state.
Cross-library restoration and unchecked force-copy across palettes remain outside
this workflow.

**Artwork selections:** use **R/W/L** for rectangle, wand and lasso. Choose
Replace/Add/Subtract/Intersect in the inspector, or hold Shift to add, Alt to
subtract, Shift+Alt to intersect. Wand matches palette colors with adjustable
tolerance and optional connectivity; transparency stays separate from opaque
black. **Ctrl+A** selects all, **Ctrl+D** deselects, **Ctrl+Shift+I** inverts,
and **Select artwork** selects nontransparent pixels. Paint, erase and fill
stay inside the selection, including holes and disconnected areas.

**Ctrl+C / Ctrl+X / Delete** copy, cut and clear selected pixels. A masked copy
preserves selected transparency, which clears destination pixels when pasted;
unselected holes leave them untouched. The paste preview tints those transparent
pixels. Copying without a selection keeps the earlier transparent-overlay behavior.
**Alt+arrows** nudges selected pixels by one; **Shift+Alt+arrows** by ten.
Moves keep anchors and geometry unchanged and require the entire selection to
fit the canvas. Escape restores the prior selection during an unfinished drag.
Selections are session view state; pixel commands have their own undo steps.
**Move (V)** drags selected pixels into a floating preview. **Ctrl+T**, **Edit >
Transform selection...**, or the inspector's **Move / transform...** opens the
same controls. Drag corners to resize, edit numeric position/size, flip, rotate in
90-degree steps, reset, fit to the canvas, or enable **Duplicate instead of move**.
Nearest-pixel scaling preserves palette indices and samples the original selection
on every change; repeated preview resizing does not progressively resample it.
Holes stay excluded and selected transparency clears the destination. The canvas
shows the complete result, including the remaining source and overlapping pixels.

**Enter / Apply** commits one undo step; **Escape / Cancel / Ctrl+Z** discards the
preview. While previewing, arrows move it by one pixel and Shift+arrows by ten.
The whole transformed selection must fit the sprite canvas. Primary/secondary
anchors, points, alternate palettes, geometry and animation metadata stay in place.
Changing the source or workspace discards an unapplied preview; other editing
commands wait until Apply or Cancel. Clipboard paste still uses its separate
click-to-place preview. Arbitrary-angle rotation and smooth selection sampling
remain outside this workflow.

**Sprite > Extract selection...** reviews the selected shape as a new sprite,
retaining holes as transparency. **Sprite > Split body parts...** starts from
suggested head/arms/torso/legs regions. Pick a part, move its box or drag its
corners, edit bounds and names, and enable the parts to create. Undetected arms
start disabled with editable boxes. The heuristic is a starting point; adjust
it for the artwork. Source/Extracted pixels/Remaining source views show the scope.
Optional trimming removes transparent borders; **Cut from source** clears the
selected pixels after every child is ready. Copy is the default.

Children share the source palette and alternate palettes. Primary anchors are
rebased to preserve placement; secondary anchors, point tables and frame metadata
start empty. The source metadata and existing embedded references stay on the
source sprite. Duplicate names receive reviewed `_COPY` suffixes. One undo restores
the whole extraction. Overlapping regions are reported because they duplicate art
in multiple children. Appending sprites and cutting silhouettes affect LOAD2
packing; extraction does not rewrite game-source tables or animation references.

**File > Import sprite sheet / GIF...** opens a separate batch review. Sheets can
use automatic/island detection or a fixed grid with margins, gaps, padding and
optional border cleanup. Inspect the source rectangles or indexed result, choose
frames and a shared new/existing palette, name the batch and optionally create an
embedded sequence. Previous/Next and Play preview the reviewed frames. Settings
need **Rebuild preview** before **Import batch** becomes available. One batch is
one undo step; Cancel leaves artwork untouched. Raster/GIF drag-and-drop opens
the same review.

Cropped sheet anchors retain the original cell's center/floor. GIFs use one shared
crop to preserve frame registration; **Place shared anchor** lets you click the
result to set a common source-space anchor. GIF delays round to MK2 ticks; absent
delays use 100 ms and long holds become repeated sequence entries. Border cleanup
uses sampled edge colors: disable it when those colors belong to the artwork.
GIF border-color trimming does not remove interior background. This is indexed
image import; the digitize workflow below adds keying and material fitting.
Video capture feeds that same review as described below.
Input limits are 64 MiB encoded, four million pixels per frame, 2000 frames and
128 MiB decoded RGBA/library pixels. Grid extraction includes full cells only.

**File > Digitize frame(s)...** accepts a batch of PNG/raster frames or GIFs.
Use Source to sample the backdrop, rebuild the matte, then add materials and
click Materials to fill each region on frame zero. Later frames classify by
those learned hues; fill, brush and erase can correct any frame. Pink marks
opaque pixels without a material. Rebuilding the matte resets all masks while
retaining the material list. Keying honors source alpha; disable **Key backdrop**
to use it directly. Cleanup includes backdrop-blend removal, despill,
alpha-weighted downscaling, alpha cut and edge shrink.

**Fit and review** chooses linear/Lloyd ramps by measured fit, or uses the
method chosen for each material. Recoloring a material preserves its shading
and pixel indices. Existing palettes can supply detected blocks or explicit
start/count ranges; their colors are unchanged. Preview Source/Matte/Materials/
Result, set inclusion or a frame range, edit holds, and right-click to place a
shared floor point. Trimming rebases that floor into each sprite's coordinates.
Import requires complete masks unless **Allow transparent gaps** is explicitly
enabled. A palette budget overflow reports an error instead of dropping a
material. The sprites, optional sequence and new palette apply in one undo step.

Source loading, matte rebuilding, material fitting and later-frame reseeding run
in background jobs with progress and **Cancel processing**. Source/matte/material/
result views and frame selection can inspect the previous review while a job runs;
editing waits for completion. Cancellation or failure restores that review, and a
result is discarded if its library changed during processing. **Close review**
cancels work before closing. The fallback source picker accepts paths one per line.

**File > Capture video frames...** (or a video drop) opens a frame grabber.
Scrub or step the preview; **I/O** set In/Out and **P** toggles individual picks.
Capture an In/Out range every Nth frame or use the picked set. Wheel zoom,
middle-drag pan and **Fit** help position the crop; drag around the actor and
right-click to place the shared floor. Numeric crop/floor controls are also
available. **Preserve source timing** rounds intervals to MK2 ticks; turning it
off uses a fixed hold. **Deinterlace footage** is optional.

Metadata, preview and capture decode on a cancellable worker. Preview is scaled
for display; capture retains full crop resolution, including odd coordinates and
dimensions. Captures are limited to 2000 frames, four million pixels per frame and
128 MiB of RGBA. The first video stream is used consistently. Source size/time
checks reject changed files, and an incomplete capture reports missing frames.
Cancel never installs a partial batch. **Capture and digitize** opens the source
review; the IMG changes when its reviewed digitized batch is imported.

Requires `ffmpeg` and its sibling `ffprobe`: set `IMGTOOL_FFMPEG`, put them beside
Studio, or install them on PATH. No FFmpeg libraries are linked. Timeline positions
use average FPS; irregular-frame-rate footage should be normalized when exact
seeking matters. Studio reports estimated counts and differing rate hints.

**File > Open LOD libraries...** reviews a LOAD2 manifest before opening its
IMG references as separate Studio tabs. Open IMG / Ctrl+O, drag-drop, recent files
and a command-line LOD path reach the same review. The table shows line numbers,
resolved paths, sprite counts, carried-forward PPP and missing/invalid/repeated
libraries; selecting a row previews its first sprite. Paths relative to the
manifest are tried first, then the working folder and IMGDIR. The optional override
folder forces each basename into that folder; editing either folder requires
**Rebuild review**. UTF-8/BOM, quoted paths, spaces, mixed separators and comments
are supported. Unknown directives or invalid PPP report their source line.

PPP starts at LOAD2's default 0 (auto), and each `PPP>` changes subsequent libraries.
New tabs inherit that setting for packing checks. Existing open tabs keep their
current artwork, unsaved edits, histories and PPP. Repeated file paths open once;
the first occurrence supplies the setting. Missing/invalid rows start included,
so resolve them or explicitly exclude them before **Open libraries**. All included
files and the manifest are checked again before installing tabs; changed files
require rebuilding. Cancel leaves the workspace intact. Other LOAD2 directives
(including sprite-selection, packing/compression, ROM/ASM/table outputs) are shown
but do not execute. This is library loading, not a LOAD2 build or append/merge.
The review is bounded to 128 entries, a 1 MiB manifest, 64 MiB per IMG and 128 MiB
of accepted decoded sprite/baseline pixels. Larger libraries can be opened directly.

Build and launch on Windows:

```powershell
# Run from the repository folder
.\build-studio.ps1
.\launch-studio.ps1
```

You can also double-click `launch-studio.bat` in the repository. The launcher
always uses the current default build and verifies its executable and SDL2.dll.
Pass `-BuildRoot` when using a different build, or `-File path/to/library.IMG`
to open artwork. If Studio closes during startup, try
`.\launch-studio.ps1 -SoftwareRenderer` (or
`launch-studio.bat -SoftwareRenderer`) to bypass accelerated rendering.
The executable also accepts `--software-renderer` directly. Both modes use the
same editor and files. Startup stages, renderer name and native exception details
are written to `%APPDATA%\midway-imgtool\studio\startup.log` on Windows; the
file is replaced on each launch. SDL/window/renderer failures display their reason
and log location. Hidden smoke checks write their startup log beside their PNG.

The Studio helper uses x64 Release and cached SDL2 2.32.10. Its default output is
`build-studio-motion\build\Release\imgtool-studio.exe`; it also builds the separate
classic and CLI binaries. VS 2022 with the C++ build tools is required. Override
the output with `-BuildRoot .\my-studio-build`. If PowerShell blocks script execution,
run `powershell -ExecutionPolicy Bypass -File .\build-studio.ps1`.
The general `build.ps1` remains available with its original default output under
`%LOCALAPPDATA%\imgtool-build\build\Release` and `-Arch x86` support.

For an isolated workspace build, use `-BuildRoot .\build-studio`; the executable
is then `build-studio\build\Release\imgtool-studio.exe`. Keep `SDL2.dll` beside
the executable. On other platforms, build the CMake target `imgtool-studio`.

Open an IMG or choose **Explore sample**. Mouse wheel zooms, middle-drag pans,
and **Ctrl+0** fits the sprite. **P/E/G/I/R/H/W/L/V** select Pencil/Erase/Fill/Pick/
Rectangle/Pan/Wand/Lasso/Move; arrows browse sprites. **Ctrl+C/X/V** copies/cuts/pastes pixels,
**K/B/U** choose Line/Replace/Smart erase, **C** chooses Clone (Alt-click source), **T** chooses Variant paint,
**Ctrl+Z/Y** undoes/redoes, and **Ctrl+S** saves. Flip preview is read-only;
turn it off to edit. Palette edits explicitly show how many sprites share the
palette. Studio stores its own layout and recent files separately from classic.

Use **File > New IMG library** (**Ctrl+N**) to start with a blank sprite, or use
**Sprite > New sprite** to add one. **F2** renames, **Ctrl+J** duplicates, and the
Sprite menu or a library row's context menu exposes ordering and geometry actions.
The inspector's **Resize** button adjusts artwork; **Canvas size** moves/crops
content without resampling it. Resize and transform dialogs explicitly control
anchor/point adjustments. Geometry changes still need a LOAD2 compatibility check.

**File > Import PNG sprite** can generate a new palette using classic's RGB555
median-cut quantizer, or match the original PNG colors directly to an existing
palette. Alpha below 128 becomes transparent; opaque black remains an opaque
palette entry. PNG export writes the selected sprite with transparency. The
inspector can assign a palette by matching colors or keeping pixel indices,
and **Make palette private** copies/assigns it in one undo step so color edits
affect only that sprite.

Open **Palette > Manage palettes** or **Manage...** beside the swatches to browse,
create, rename, duplicate and transfer palettes between libraries. Copy/Paste in
the manager stores palette colors separately from the pixel clipboard; pasting
adds a new palette without assigning it. Import/export supports raw RGB555
`.PAL` and Adobe `.ACT`, including ACT color-count footers. ACT transparency must
be index zero or unspecified. Removing an unused palette updates sprite and
alternate-palette references; referenced palettes cannot be removed.

Click a swatch to select one color, **Ctrl-click** to toggle colors, or
**Shift-click** for a range. **All opaque** and **Used in preview** provide quick
selection. RGB555 and hue/saturation/lightness adjustments use a before/after
sprite preview. **Apply colors** commits one undoable edit to all sprites using
that palette; **Reset preview** or **Close (discard preview)** leaves stored colors
untouched. Index zero stays transparent. Browsing a palette does not assign it;
**Assign to preview sprite** explicitly matches colors or keeps pixel indices.
The preview displays existing indices, so matching on assignment can look different.
To merge sprites onto one palette, select the destination in the manager and
choose **Merge...**, then select the source. **Grow for exact colors**
adds used source colors up to the 256-color limit while keeping existing target
indices stable. The before/after preview uses the same remap as **Apply merge**;
the impact list reports changed indices, approximated colors, and sprites crossing
the chosen packing PPP limit. Approximation is disabled until **Allow approximate
colors** is explicitly checked. **Cancel merge** leaves the IMG unchanged.

Merge applies to the source palette's primary sprites. Sprites with active
alternate palettes are skipped, and their source palette remains available.
**Remove source palette after merge** is optional and works only when no references
remain; merging and removal undo together. The existing destination's sprites keep
their colors, and transparency remains index zero.

**Usage / compact...** shows a per-color histogram of primary and alternate
pixel references and a preview of the resulting palette. **Pack used colors
together** removes unused gaps while retaining used colors in their original
order; turning it off trims only an unused tail. Alternate references keep
indices fixed automatically, so those palettes use tail trimming. Declared depth
stays unchanged unless **Recompute declared color depth** is checked. The preview
reports color-count, index and packing changes; **Apply compaction** is one undo
step, and **Cancel compaction** leaves the IMG untouched. Used duplicate colors
remain separate, and no colors are approximated. General color-count resizing,
multi-source merging remain pending. Digitize imports provide material-mask and
footage-ramp fitting in their own source review.

Select swatches, then **Shading / gradients...** to recolor that selection.
**Single-color shading** combines a base color with each original swatch's
brightness, with strength, brightness and contrast controls. **Gradient** uses
2–11 editable stops, maps by original brightness or selected index order, and
supports seeding, reverse and absolute stop HSL shifts. Click a stop number and
either sprite preview to sample a color. **Apply ramp** commits one undo step;
**Cancel ramp** leaves stored colors unchanged. Pixel indices, palette size,
declared depth and index-zero transparency stay fixed. **Saved Studio gradients**
stores named presets separately from classic; saving an existing name replaces
that preset, and malformed preset files are left intact.

Choose **Animation & alignment** (or press **Tab**) for the World View. Its
sequence groups come from sprite names, with numeric frame ordering and recognized
subframes drawn together. They do not interpret embedded IMG or ASM scripts.
Use the thumbnail strip or scrub slider to seek; **Space** plays/pauses and arrows
step frames. Frame holds are measured in preview ticks, defaulting to four ticks
at MK2's 54.7068 Hz. A 60 Hz preview is also available.

Choose **IMG animations** to edit sequences actually stored in the library.
Sequences show frames in playback order, accounting for the reversed disk order.
Create/rename sequences and scripts; add, duplicate, reorder or delete entries;
choose a sprite or called sequence and edit stored ticks and animation-point deltas.
The thumbnail timeline plays/scrubs sequences at MK2's clock with flip preview.
Ticks retain the full 0–255 byte range; zero stays stored as zero and previews for
one tick. **Ctrl+Z/Y**, **F2**, **Ctrl+J** and **Ctrl+S** work on the IMG in this
workspace. Header fields, damage references and entry spare bytes are preserved.

Scripts show their stored call list; **Open sequence** previews the called sequence.
This does not execute script control flow. **Create scene snapshot** copies a
sequence's order, timing, offsets and facing into a separate composition. Snapshot
edits stay in that scene; they do not update the original embedded sequence.
Record deletion, advanced header editing and WAX rows driven by embedded records
remain pending.

**Feet to floor** places the current frame once; the anchor target then stays
fixed while you browse. Drag an opaque piece to adjust its primary IMG anchor.
**Drag the whole frame** adjusts all displayed pieces in one undo step. In the
Artwork workspace, Select can drag the yellow anchor handle. **Escape** cancels
an unfinished drag. Facing, onion skin, timing, playfield and reference placement
stay in the preview session until you create a project; actual artwork/anchor edits
save to IMG.

Use **Project > Create from current preview** to turn the current animation and
reference into an editable composition. **Add actor lane**, **Add frame** and
**Replace frame** in the inspector use sprites from any open library. Each frame
can have its own hold, offset, depth and X/Y flips; actor lanes add placement,
start tick, opacity, looping and hold-at-end controls. Dragging an actor changes
its scene offset while leaving its IMG anchors untouched. **Ctrl+Z/Y** acts on
the project in this workspace and on artwork in the Artwork workspace.

Expand **Motion & timing** for scheduled effects: **Show at** is inclusive and
**Hide at** is exclusive, in ticks from the actor's start. Zero Hide leaves the
end open; setting both to zero restores ordinary frame sequencing. Scheduled
entries can appear together. Motion X/Y is measured in screen pixels per tick
(positive right/down), with optional maximum travel. **Freeze actor at** stops
that actor's clock and motion while other actors continue. **Second copy** adds
another instance of the frame with its own offset and depth. These settings have
project undo and use the same playback and PNG evaluator. Replacing a frame's
artwork retains its timing, motion and placement settings.

**Follow frame timing** connects an effect actor to a particular frame in another
actor. Choose the source actor and frame, then **Follow frame**. The effect's
start follows that frame's first authored tick as you retime or reorder it;
**Delay after source** adds a delay without changing the effect's own frame rate.
Effects can follow other effects. This connects timing only: placement and
visibility remain the effect's own settings. **Detach** keeps its current start
as an independent actor. Both actions undo, and Studio protects source frames
and actors from deletion while another effect uses them.

Save referenced artwork with **Save referenced IMGs**, then **Save project** to
write an `.ISTUDIO` file. Scene saving checks that its sources are saved and match
their disk files; it never silently saves IMG edits. Projects remember sprite
identities, portable relative paths and fallback paths. Reopening stages all
dependencies first and offers **Locate IMG** for missing files. Unsaved scenes
have their own close/quit prompt, and libraries used by a project's history stay
open until that project is closed. **Project > Export current scene PNG** exports
the current tick on the chosen solid background, without editor guides or ghosts.

**Open project / import WAX** (**Ctrl+Shift+O**) reads native Studio projects and
classic WAX/WVP files containing explicit sprite rows. WAX import converts classic
anipoint offsets to screen offsets and preserves frame holds, facing, cross-file
references, depth order, solid background, visibility windows, capped motion,
freeze ticks and second copies. Uniform spawned-effect rows import as frame
timing bindings. Spawn rows moved by hand or with custom schedules/freeze rules
need to be detached in classic before importing. Substitution rules,
magnification, ASM and embedded sequences are
reported as unsupported. This imports the stored rows and origin; it does not
reconstruct runtime game placement or settings absent from the WAX file. Imports
save to `.ISTUDIO`; the original WAX is kept as the classic project. Studio reads
versions 1–4. It writes version 3 normally and version 4 when a frame exceeds
120 ticks, so older builds reject long-hold projects instead of shortening them.

The design rationale is in [the redesign assessment](UI_REDESIGN_AUDIT.md).

## What it does

Imgtool is a sprite-sheet editor for Midway's IMG format. Open a file, browse hundreds of frames, edit pixels and palettes, then save back out — bit-identical to what the original 1992 tool produced.

### Editing
- **Pixel tools** — pencil, eyedropper, flood fill, marquee, lasso, magic wand, clone stamp, smart eraser, smart remap, auto-sprite chopper
- **Floating paste with Free Transform** — Ctrl+V drops a floating selection that auto-enters scale mode; drag handles to resize (aspect-locked by default, chain icon unlocks), Enter commits
- **Adobe-standard shortcuts** — Ctrl+A select all, Ctrl+D deselect, Ctrl+Shift+I invert, Ctrl+J duplicate, Ctrl+E merge down, Ctrl+T free transform
- **Snap + center guides** — drag-to-move snaps to edges of the underlying content (Shift held) and shows live magenta guides when paste centers on the sprite's axis
- **32-level undo / redo** for pixel + animation point + hitbox edits

### Palettes
- **256-color palettes** with 15-bit RGB editing
- **HSL sliders** — hue / saturation / lightness with live preview, scoped to selected swatches or whole palette
- **Locate-on-canvas highlight** — multi-select swatches (Ctrl/Shift-click) and every pixel that isn't one of the selected colors dims on the canvas, so you can see where a color lives at a glance
- **Cross-file palette clipboard** — copy a palette from one IMG, paste it after opening another
- **Tools** — merge marked, delete unused colors, histogram, duplicate, rename, palette match for PNG import

### Animation
- **Animation points** — primary (X/Y) and secondary (X/Y/Z) anchors per sprite, rendered as DOS-style crosshairs, draggable on the canvas
- **Onion-skin registration** — when onion mode is on, the previous frame's anipoint shows as a dim gray crosshair underneath the active one, matching the original DOS tool
- **Hitbox editor** — drag-to-resize collision boxes with overlay rendering
- **Timeline strip** — Play/Stop, scrubbable thumbnails, drag-to-reorder, FPS slider, ping-pong playback, onion-skin ghosting
- **World View** — fixed playfield canvas (default 400×254) with sprite anchored at its anipoint, for aligning body parts across frames

### MK2 strike-table editor
- **Direct `MKSTK.ASM` editor** (`Tools → MK2 Hitboxes`) — parses the Mortal Kombat 2 source-of-truth strike-table file and edits in place. No `stk.bin` round-trip required; saves write back through the same `.ASM` line buffer, preserving comments, indentation, and symbolic literals like `sf_squeeze`.
- **3-pane navigator** — character → move → fields. Each strike record exposes `x_offset` / `y_offset` / `x_size` / `y_size`, `strike_routine` (raw token), `damage` (split into hit / block bytes), `score` (32-bit), and `sound`.
- **Magenta on-canvas overlay** — the selected move's collision box renders on the active MK2 sprite with drag-to-resize corner handles; coordinates flow straight back into the `.ASM`. The IMG-embedded hitbox overlay suppresses itself while an MK2 move is selected.
- **Per-record undo + unsaved-changes guard** on quit (separate from the IMG dirty flow).

### Import / Export
- **TGA, LBM, PNG** load and save. PNG import quantizes via median-cut in 15-bit RGB space, then maps every pixel by nearest color. Import-into-active-palette mode skips quantization.
- **GIF import** loads one frame or every frame, with blend mode and opacity controls before quantizing to an IMG palette.
- **Palette import/export** supports raw Midway `.PAL` data and Adobe `.ACT` RGB palettes.
- **Build TGA** from marked sprites, **Export PNG**, **ANILST** (assembly animation lists)
- **TBL export** — MK3-format with hardware padding (`/P`), 16-bit align (`/L`), dual-bank (`/E`)
- **IRW export** — raw binary ROM layouts with dynamic BPP (`/B`)
- **LOD parser** — read `.lod` manifests, resolve via `IMGDIR` or override (`/O`)

### Command line

Builds include the normal GUI executable plus `imgtool-cli`, a console-friendly
headless entry point for scripts and CI. The GUI executable also accepts the
same headless commands, but `imgtool-cli` gives normal stdout/stderr behavior
on Windows.

```bash
imgtool-cli --help
imgtool-cli --verify-load2 MK2MIL.IMG --ppp=6
imgtool-cli --export-tbl MK2MIL.IMG mk2mil.tbl --mk3 --include-pal --base=02000000
imgtool-cli --export-irw MK2MIL.IMG mk2mil.irw --bpp=auto --base=02000000
imgtool-cli --export-anilst MK2MIL.IMG anilst.asm
imgtool-cli --export-png MK2MIL.IMG png-out
imgtool-cli --build-tga MK2MIL.IMG marked.tga
imgtool-cli --build-lod MK2MIL.LOD rebuilt.img --override-dir=./img
```

Headless commands fail with a non-zero exit status on invalid arguments,
missing inputs, failed IMG loads, and LOAD2 breaking issues.

### File dialog
- **Drag and drop** `.img` / `.png` / `.gif` / `.tga` / `.lbm` files onto the window to open or import
- **Per-category last-dir** memory (IMG / PNG / GIF / palette / TGA / LBM each remember their own folder)
- **Preview thumbnails** for PNG, GIF, and TGA highlighted files
- **Sort** by name / date / size, asc or desc
- **Double-click to open**, **Recent Files** menu

### Validation
- **LOAD2 packing verifier** (`Tools → Verify LOAD2 Packing`) — cross-checks edits against LOAD2's destbits computation. Catches geometry drift, PPP-fallback palette overflow, per-row zero-shape drift that would misalign sprite addresses (SAGs) when the IMG is repacked into the game's IRW/ROM. Each break shows a per-row drift visualization on the sprite.
- **Bulk pixel restoration** — restore pixel data from a parent sprite across the entire file via regex or explicit selection. Three modes: Replace, Diff, Reconstruct-from-Parent (re-paints censored interior pixels using the master as ground truth).
- **Unsaved-changes prompt** before opening another file, so palette / hue edits aren't silently discarded

Press **`h`** in the app for the full reference — keyboard shortcuts, file formats, the Williams DMA #2 hardware document. See [CHANGELOG.md](CHANGELOG.md) for release-by-release feature history.

---

## Building from source

Requirements: **CMake 3.20+** and **SDL2 development libraries**. Visual Studio 2022 on Windows, GCC on Linux, Clang on macOS.

### Quick build (Windows) — auto-downloads everything

```cmd
build.bat              :: 64-bit (default)
build.bat x86          :: 32-bit
```
```powershell
.\build.ps1              # 64-bit (default)
.\build.ps1 -Arch x86    # 32-bit
```

Both scripts auto-download SDL2 and CMake to `%LOCALAPPDATA%\imgtool-build\`.
Output includes `imgtool.exe` and `imgtool-cli.exe`.

### Manual CMake build

**Windows (Visual Studio 2022):**
```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Linux:**
```bash
cmake -B build
cmake --build build
```

**macOS (Apple Silicon / Intel):**
```bash
brew install sdl2
cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build
```

The build copies `SDL2.dll` (Windows) and the Material Symbols icon font next to the executable.

---

## Common keys

| Key | Action |
|-----|--------|
| `Ctrl+O` / `Ctrl+S` | Open / Save IMG |
| `Alt+L` / `Alt+S` / `Ctrl+L` / `Ctrl+B` | Load LBM / Save LBM / Load TGA / Build TGA from marked |
| `Ctrl+Z` / `Ctrl+Y` | Undo / Redo (paint, anipoint, hitbox, palette) |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / Cut / Paste |
| `Ctrl+Shift+X` / `Ctrl+Shift+V` | Cut selection to new sprite / Paste as new sprite |
| `Ctrl+A` / `Ctrl+D` / `Ctrl+Shift+I` | Select All / Deselect / Invert |
| `Del` / `Backspace` (with a selection) | Clear the pixels inside the selection |
| `Arrows` (after Paste as New Sprite / canvas resize) | Nudge the art inside its canvas (`Shift` = 10px) |
| `Ctrl+J` / `Ctrl+E` / `Ctrl+T` | Duplicate / Merge Down / Free Transform |
| `P` / `R` / `W` / `L` / `I` | Pencil / Marquee / Magic Wand / Lasso / Eyedropper |
| `G` / `V` / `E` / `C` | Paint Bucket / Variant Paint / Smart Eraser / Clone Stamp |
| `[` / `]` (Pencil mode) | Shrink / grow brush radius |
| `[` / `]` (no paint tool) | Set palette for marked / for current image |
| `Shift+R` / `Shift+8` / `Del` | Rename / Merge marked into / Delete palette |
| `Space` | Mark / Unmark current image |
| `Shift+M` / `M` | Set all marks (type "M") / Clear all marks (type "m") |
| `Shift+Del` | Delete image |
| `Ctrl+R` / `Ctrl+P` | Rename image / Toggle point table |
| `Alt+PgUp` / `Alt+PgDn` | Move image up / down in list (or drag a row onto another) |
| `Tab` | Swap between image lists 1 and 2 |
| `K` | Toggle timeline play |
| `Ctrl+Left` / `Ctrl+Right` | Step prev / next frame |
| `;` | Least-squares size reduce on marked |
| `Up` / `Down` / `PgUp` / `PgDn` | Navigate image list (or palette list if last clicked) |
| `H` / `F9` | Help / Debug info |
| `Esc` | Cancel current action |
| Mouse wheel / Middle drag / Space+drag | Zoom / Pan / Pan |
| Right-click on canvas | Eyedrop |
| Shift + left-click | Flood fill |

Full keyboard reference in the in-app help (`h`).

---

## File format support

| Format | Load | Save | Notes |
|--------|------|------|-------|
| **IMG** | Yes | Yes | Primary format. Pre-2.x IMGs auto-converted on open. Up to 2000 sprites/palettes per file. |
| **TGA** | Yes | Yes | 8-bit color-mapped Truevision Targa. Bottom-up pixel order. |
| **LBM** | Yes | Yes | IFF/ILBM chunk format. CMAP + BODY with RLE decompression. |
| **PNG** | Yes | Yes | Median-cut quantization on import, nearest-color mapping. *Import (Match to Active Palette)* skips quantization. |

---

## Source structure

| Path | Purpose |
|------|---------|
| `platform/main.cpp` | Entry point, SDL2 window creation, main loop |
| `platform/imgui_overlay.cpp` | Main UI: menus, toolbar, canvas, panels, palette, modals |
| `platform/img_format.h` | IMG/PAL data structures, allocators, palette helpers |
| `platform/img_io.{h,cpp}` | File I/O: IMG load/save, TGA/LBM/PNG import/export |
| `platform/mk2_hitbox.{h,cpp}` | MK2 strike-table (MKSTK.ASM) parser/writer |
| `platform/shim_vid.c` | SDL2 renderer init, palette tables |
| `platform/shim_input.c` | Keyboard input, DOS scan code mapping |
| `platform/shim_file.c` | File system emulation, path remapping, old-format conversion |
| `platform/shim_dialog.c` | Native dialogs, directory persistence |
| `platform/globals.c` | Global state: image/palette lists, selection indices |
| `imgui/` | Dear ImGui v1.91.0 (SDL2 renderer backend only, trimmed) |
| `CMakeLists.txt` | Build config |

---

## Environment variables

| Variable | Purpose |
|----------|---------|
| `IMGDIR` | Default directory for `.img` files |
| `TGADIR` | Default directory for `.tga` files |
| `MODELS` | Default directory for model files |
| `ITUSR1` / `ITUSR2` / `ITUSR3` | User-defined shortcut directories |

---

## Status

**`SDL-main` branch** — actively maintained. The original DOSBox build on `main` and the older `sdl-experimental` branch have been retired.

**What works:** Full 2D sprite editing parity with the DOS tool, modern UI, file dialog with previews, all MK1/MK2/MK3/NBA-era IMG files load and save correctly, PNG import with proper quantization, cross-platform Windows/Linux/macOS, pure C/C++ build with no assembler required.

**What's missing:** 3D model editor (the original `it3d.asm` was stubbed; this tool is 2D-only).

---

## Credits

Portions of this tool are based on agentic AI reverse engineering by [Asure007](https://github.com/Asure/), including work from [midway-loadimg](https://github.com/Asure/midway-loadimg).

---

Issues and PRs welcome at https://github.com/junkwax/midway-imgtool.
