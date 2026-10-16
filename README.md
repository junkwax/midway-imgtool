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
indexed LBM/TGA import and export with reviewed sprite sheets, sprite resize/canvas-size/trim/rotate/flip with reviewed batch geometry and parent/child restoration, reviewed LOD library loading and TBL/IRW/ANILST exports, PNG import/export, reviewed sprite-sheet/GIF imports,
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
currently accepts validated IMG layouts `0x60a`–`0x631` and `0x634`–`0x654`.
Older WIMP libraries preserve their original format on Save / Save As, including
stored sequences/scripts, point tables and alternate palettes. The status bar
shows their format. **Every Save and Save As keeps the original IMG version and
record layout. Studio offers no format conversion.** A save refuses edits that
cannot fit that format and leaves the destination intact.
Pre-`0x60a` and transitional `0x632` / `0x633` layouts, additional legacy image variants, ASM animation,
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

**Sprite > Batch rename...** previews names for Marked sprites, Shown by search,
or All sprites in the current library. Choose numbered names, prepend text, or
append text plus a number, with a starting number and optional zero padding.
Rows follow library order. Uncheck a row to keep its current name; the remaining
previewed numbers stay fixed. The review checks the final names against the whole
library, including excluded sprites, using case-insensitive matching. Overlong,
empty, non-ASCII or spaced names block Apply instead of being silently shortened.

**Apply names** changes included rows in one Undo step; Cancel discards the
preview. Pixels, palettes, anchors, point tables, raw trailing name bytes and
embedded animation references remain intact. Existing IMGs retain their version
and record layout. External ASM / TBL references are not rewritten, and new names
can change name-based LOAD2 allocation. Save the IMG through its normal save flow.

**Sprite > Copy / move to another library...** transfers sprites to another open IMG
library. Open both libraries, then start from the source and choose Marked sprites,
Shown by search, or All sprites. Review copied names, palette assignments and the
source preview; uncheck rows to exclude them. An optional prefix resolves name
conflicts. Names must fit 15 ASCII characters without spaces; existing sprites are
never replaced and names are never silently shortened.

Required primary and alternate palettes are copied once per source palette.
**Reuse identical palettes** reuses existing destination palettes only when colors
and metadata match exactly; disable it to keep the copied palettes independent.
Pixel indices, padding, anchors, point tables, baselines and opaque sprite/palette
metadata are retained. Alternate table references are remapped, with default
palette references and bytes past the terminator retained. Unrepresentable
alternate indices or metadata unsupported by the destination layout block the row.
Both libraries must remain open and unchanged while the review is active.

**Copy sprites** appends the included rows and required palettes in one destination
Undo step. Copies have new stable identities; source data and history are unchanged.
The destination keeps its existing IMG version and record layout. Apply never
saves automatically. This pass transfers listed sprites and palettes only, without
source sequences/scripts. Use the separate sprite clipboard below when the
source library needs to close before pasting.

**Move from source** is optional; Copy is the default. Move appends the reviewed
sprites to the destination and removes them from the source together, after
preparing both libraries and their Undo entries. Sprites used by embedded
sequences or current open scenes are protected; **Exclude referenced sprites**
lets you keep them in the source. At least one source sprite must remain.
Remaining source sequence indices are remapped, while scripts and source palettes
stay in place. Embedded animation records are not transferred by this workflow.
External game references and closed scene projects are not scanned.

Each library has its own Undo step: undo each separately to reverse the entire
move. Save both libraries separately when ready. Neither is saved automatically,
and each retains its original IMG version and record layout. A stale review,
incompatible destination, or preparation failure leaves both libraries unchanged.

**File > Copy IMG animations to library...** (also **Animation > Copy to another
library...** in IMG animations mode) transfers embedded sequences and script call
lists between two open libraries. Select source records; choosing a script adds
its called sequences automatically. Clear choices or uncheck the script to remove
those dependencies. An animation-name prefix resolves collisions without replacing
existing records. Each resulting name must fit 16 printable ASCII characters.

By default, the destination uses its existing referenced sprite artwork. The review
matches unique sprite names without case or outer spaces. **Destination sprite
prefix** matches prefixed artwork, such as source `SENTINEL3` to destination
`ART_SENTINEL3`; it does not rename sprites. For differently named or duplicate
sprites, open that row's **Destination sprite** dropdown and search by name or
exact sprite index. Each choice shows its index and dimensions, with anchors in
the tooltip. Explicit choices distinguish sprites even when their visible names
are identical; multiple source sprites can deliberately share one destination.
Unresolved rows block Apply. Enable **Copy missing artwork** below, or copy artwork
first with the sprite-transfer tools.

**Manual** choices survive prefix edits and animation selection changes.
Changing the destination library clears them. Choose **Automatic name match** in
a dropdown to reset that row, or **Reset sprite choices** to clear all overrides.
The binding table shows exact sprite indices and dimensions; anchor/geometry
differences are marked. Bindings do not compare pixel content.

**Copy missing artwork** is optional and off by default. It appends only sprites
required by the selected animations (including script dependencies) whose unique
names are absent from the destination. The destination sprite prefix also names
new sprites. Existing matches and explicit bindings stay in place; ambiguous
names still need a manual choice. Repeated animation references copy a sprite
once. Other sprites, including child pieces not explicitly referenced, stay out.

New sprite rows are labelled **Copy**, with their future destination indices.
The summary lists sprite/palette additions; hover it to review required palette
names, indices, reuse and color counts. Primary and active alternate palettes are
remapped; only palettes with identical stored metadata and colors are reused.
New buffers include pixels, baselines, anchors, point tables and alternate tables.
Unsupported names, layers, palette references, destination limits or layout fields
block Apply. Existing sprites and palettes are never replaced.

Artwork, palettes and animation records are prepared together before changing
the destination and become one **Copy animations and artwork** Undo step. Cancel,
stale reviews and failed preparation add nothing. Undo/Redo restores/removes the
whole transfer, including stable sprite IDs. Copying records with existing artwork
also prepares its history before publishing changes.

Copy appends sequences before the destination script block, keeps existing
sequence indices and existing script calls intact, and remaps incoming sprite and
sequence calls. Stored timing (including zero ticks), offsets, flags, negative
sentinels and opaque bytes stay intact; a name prefix overwrites only its visible
name and terminator. Both libraries are checked again at Apply. One destination
Undo step removes the additions. The destination opens in IMG animations mode
for inspection; the source library and existing destination artwork stay unchanged.

Copy never saves automatically and never changes either IMG version or record
layout. Unsupported far-pointer fields are blocked for older destinations.
Sequences with active damage-table bindings are not supported in this transfer
pass; damage tables and external engine/runtime bindings are not transferred.
Script runtime execution is still outside Studio's current playback scope.

**File > Copy IMG animations to clipboard...** captures selected embedded records,
called sequences and all required sprite artwork/palettes into an independent
snapshot. You can close the source library afterward. Capture keeps original
names and does not change source artwork, history or saved files. Choose at least
one record; source sprite names must be unique. Unsupported damage bindings and
other transfer constraints still apply. Cancel or failed capture keeps the
previous animation clipboard.

**File > Paste IMG animations from clipboard...** opens the transfer review
against any open library. All captured records start selected and **Copy missing
artwork** starts enabled. You can select fewer records, change prefixes, choose
existing destination sprites and review palette additions before pasting. The
usual name/layout/reference checks and one destination Undo step apply. Save
separately. The destination always retains its original IMG version and layout.

The **Animation** menu offers these commands in IMG animations mode.
**Edit > Clear animation clipboard** releases the session copy. Clipboard contents
last until replaced, cleared or Studio closes; they do not use the OS clipboard
or persist across restarts. Pixel, palette and sprite clipboards stay separate.
An already-open paste review retains its snapshot until that review closes.

**File > Split IMG animations to new library...** (also **Animation > Split to
new library...** in IMG animations mode) creates a new IMG tab from selected
native sequences/scripts. Scripts include their called sequences. The review
shows exactly which records, sprites and palettes will be included; Select all
and Clear choices adjust the set. Original names are retained. Only referenced
sprites and their primary/active alternate palettes are copied, with incoming
indices remapped. Unreferenced artwork stays in the source.

**Create library** opens the result in IMG animations mode. It is initially
unsaved, with no creation Undo entry; subsequent edits use normal Undo/Redo.
Closing it prompts for unsaved changes. Save it when ready. The new library
inherits the source IMG version and record layout, including supported legacy
reserve metadata. The source library, its saved files, history and all clipboards
stay unchanged.

Choose at least one animation that references artwork. Empty sequences can be
included alongside it; a new library containing only records without artwork
is refused. Source sprite names must be unique, and damage-bound sequences and
other transfer limits still apply. This extracts native IMG records; scene
projects and external game bindings remain separate.

**Sprite > Split to new library...** creates a separate IMG tab from Marked,
Shown by search, or All sprites, with optional child pieces and per-row exclusions.
The default is **Copy**, leaving the source unchanged. Enable **Move from source**
to remove the included sprites after the new library is ready. Names stay intact;
only required primary/alternate palettes are copied. The new library inherits the
source IMG version and record layout, including older layouts.

With Copy, enable **Include IMG animations** to keep sequences that use only
included sprites, plus scripts whose called sequences are all included. This
never adds extra artwork: change the sprite scope, child-piece option or row
exclusions to cover a complete animation. **Animation records: copied / skipped**
shows every source record and explains skipped records. Empty sequences, scripts
without sequence calls, outside/unresolved references and unsupported damage
bindings are skipped. Invalid or colliding names in included records block the
review. Pixel data, raw record metadata and timing are retained, with sprite and
sequence indices remapped. Extra reviewed sprites stay in the new library even
when no animation references them. The source stays unchanged. This option is
available with Copy; Move continues to protect source animation references.
For manual record selection or empty records alongside artwork, use **File >
Split IMG animations to new library...** instead.

Move protects sprites used by embedded sequences or current open Studio scenes.
**Exclude referenced sprites** keeps them in the source; per-row labels and the
selected preview show those uses. Open scene references are checked again when
applying. At least one sprite must remain in the source. External game references
and closed projects are not scanned. Remaining embedded sprite indices are
remapped; source palettes and embedded sequences/scripts stay in the source.
Move does not transfer sequences or scripts to the new library.

The new tab starts unsaved and participates in the normal close/quit save guard.
Move is one source Undo step: Undo restores the source sprites while the new
library remains open. Copy does not add source history. Save the new IMG when
ready, and save the source too after Move. No files are written automatically.
Source palette cleanup stays a separate explicit operation.

**Sprite > Copy sprites to clipboard...** captures Marked, Shown by search, or
All sprites after review. Include child pieces is optional; uncheck any rows to
leave them out. A successful copy replaces the previous sprite clipboard. Cancel
or a failed capture keeps it. Copy leaves the source artwork and history untouched.

The snapshot owns its sprite pixels, padding, baselines, anchors, point tables,
raw name metadata, primary palettes and required alternate palettes. Source edits
and closing its tab cannot change what was copied. It stays in memory for this
Studio session, separate from pixel Ctrl+C/Ctrl+V and palette-color Copy/Paste.
It is not the Windows clipboard and does not survive closing Studio.

Open or create the destination and choose **Sprite > Paste sprites from
clipboard...**. Review names, a prefix, exact palette reuse and per-row exclusions.
The captured set stays fixed; copy again to choose another scope. Name collisions
and metadata unsupported by the destination's existing IMG layout block affected
rows. Paste appends sprites and required palettes in one destination Undo step,
with new sprite IDs, unchanged pixel indices and remapped alternate references.
You can paste repeatedly or into the same library using a prefix to avoid clashes.
Neither Paste nor capture saves automatically, and the destination IMG version
and record layout never change. Sequences, scripts and external game references
are not captured. **Clear sprite clipboard** (Sprite or Edit menu) releases the
snapshot without clearing the pixel or palette clipboards.

Both bulk copy and bulk deletion offer **Include child pieces** (off by default).
This adds direct children of sprites in the chosen scope using the classic naming
rules (`BASE1A`, `BASE_1A`, `BASE_1`, `BASE1`) and matching source-library labels.
The review counts added pieces and labels each with its parent. It does not add
siblings or recurse into descendants; explicitly scoped children can bring their
own direct children. Ambiguous parents are skipped and reported, with names on
hover. Select those sprites explicitly if needed. Each row can still be excluded;
excluding a parent does not silently exclude its reviewed children. These are
name-based editing relationships, with no new metadata stored in the IMG.

**Sprite > Delete multiple sprites...** reviews Marked sprites, Shown by search,
or All sprites with a sprite preview and per-row exclusions. Referenced rows show
their embedded sequence or open Studio scene; **Exclude referenced sprites** keeps
them automatically. Remove references first if those sprites must be deleted.
Open scene uses are checked again when deleting, including changes made after the
review opened. At least one sprite must remain in the library.

**Delete selected sprites** removes the included rows in one Undo step and remaps
remaining embedded sprite indices. Script sequence indices, palettes and surviving
sprite identities are retained. Undo restores deleted pixels, metadata and the
previous selection. Save and Save As retain the loaded IMG version and record
layout. The review deletes only listed rows, including child pieces added with
the explicit option. External ASM/TBL files and closed projects are not scanned.
Deletion can change game build allocations; Apply never saves automatically.

**Palette > Assign to multiple sprites...** reviews a destination palette for
Marked sprites, Shown by search, or All sprites. **Match source colors** maps each
opaque color to the nearest opaque destination slot; transparency stays at index
zero. **Keep pixel indices** changes the palette reference without changing any
pixel bytes, for intentional recoloring or correcting an old palette assignment.
A destination that cannot hold the retained indices is blocked. Select a row to
compare its primary-palette appearance before and after, and exclude rows as needed.

Matching that changes indices is blocked for sprites with active alternate
palettes; retaining indices preserves those alternate appearances. Matching can
approximate colors, so review the preview and select **Allow approximate colors**
when needed. The destination palette's colors and order are never modified. The
PPP indicator reports newly crossed LOAD2 color-count limits; palette-size changes
can also affect packing, so use the packing report before a game build.

**Apply palettes** commits all included, compatible sprites in one Undo step.
Cancel discards the review. Whole sprites are edited independently of canvas
selections. Anchors, points, alternate palettes, raw names, baselines and embedded
animation references are retained. Existing IMG version/layout stays fixed. Apply
does not save the library or change game build settings.

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

**File > Import indexed LBM / TGA...** reviews one sprite with its full source
canvas, a new palette in the source color order or matching to an existing palette,
and an editable primary anchor. Click the preview to place the anchor; source LBM
hotspots are used when available. Import adds a sprite/palette in one undo step.
LBM/IFF and indexed TGA drag-and-drop use this review. These imports never convert
the IMG file format. New sprites start with unused secondary-anchor sentinels.

The indexed reader supports PBM and 1–8-plane ILBM, raw / ByteRun1 rows, transparent
colors and ILBM mask planes; indexed TGA supports raw / RLE, 8-bit pixel indices,
nonzero palette origins, 15/16/24/32-bit color maps and both image origins.
Unmasked artwork treats index zero as IMG transparency. Explicit mask/alpha may
move an opaque color zero to an available slot; the review reports this. Colors
reduce to RGB555 without reordering by frequency. Partial alpha, 256 distinct
opaque entries plus IMG transparency, HAM / Extra Halfbrite, IFF ANIM and destination
plane merging are rejected. Full-color TGA still uses the sprite-sheet review.

**File > Export selected LBM... / TGA...** reviews an indexed output with the
source palette order and visible pixel indices. LBM writes uncompressed IFF PBM
with transparent index zero and a GRAB primary-anchor hotspot. TGA writes an
uncompressed indexed image with palette alpha; IMG animation anchors have no TGA
representation. Export leaves IMG metadata, format, dirty state and history intact.
Rebuild after settings, artwork or destination changes; replacement is atomic and
cannot target an open IMG. These selected-sprite commands write one sprite; the
separate sheet exporter below handles marks.

**File > Export TGA sprite sheet...** reviews Marked, Shown by search, All or
Selected sprites. Exclude rows without changing marks; inspect rectangle positions,
primary-anchor crosses and face order. Scroll to zoom, drag with the middle/right
button to pan, or use Fit preview. Full canvases pack tallest first with editable
spacing; no sprites are cropped, scaled or silently omitted.

Identical palettes retain their indices. Mixed palettes merge the exact used
RGB555 colors, preserving opaque black separately from transparent index zero.
More than 255 opaque colors blocks indexed output; choose **RGBA sheet** without
ANF or export smaller groups. RGBA keeps every sprite's colors and transparency.

**Classic ANF face map** writes a companion `.ANF` with the primary anchors and
face rectangles in library order. This mode uses a 256-wide, bottom-up indexed
TGA with 15-bit palette words, sprites up to 256×256, at most 1000 faces and 6000
sheet rows. Its count-minus-one and face ordering follow the original DOS writer.
TGA-only output allows widths up to 8192 and four million sheet pixels, with a
maximum height of 8192. TGA does not carry sprite names or IMG anchor metadata.

Build the review after settings, source or destination changes. Both output files
are prepared before replacement, and a failed second replacement restores the
first when it still matches Studio's output. Two separate files cannot be globally
atomic against a crash, power loss or concurrent external changes; an unsuccessful
restore retains the backup and reports its path. The exporter never changes IMG
format, pixels, metadata, history or marks. This is a new packed sheet layout,
not preservation of previously allocated ROM addresses.

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

**File > Export pipeline** opens a review for **TBL image table**, **IRW raw pixels**,
or **ANILST sprite list** in the current library. Choose Marked, Shown by search,
All, or Selected whole sprites; rows stay in library order. Without marks the first
review starts with the selected sprite. Per-row inclusion is separate from marks
and canvas pixel selection. Options and destination changes require **Build review**;
row exclusion rebuilds the output. Source changes require an explicit rebuild.
The table shows depth, SAG and blocked reasons, with assembly or binary previews.
The destination is shown before **Export file** / **Replace output**. Existing-output
changes after review are rejected; a successful write replaces one file atomically.
Source data, flags, dirty state, history and marks stay unchanged. The library keeps
its last successful export settings/scope/exclusions for this session; canceled
settings do not replace them.

TBL and IRW share one **new uncompressed layout**. Export them separately with the
same included sprites, ROM base and packing options to keep SAGs consistent. Fixed
1-8 bpp blocks indices that would be truncated; Auto palette size and Auto used
indices choose a depth for each sprite. Word / Nibble / Packed alignment inserts
16-bit / 4-bit / no inter-sprite gaps; the complete IRW stream ends on a word.
The ROM load base must be word-aligned, and overflowing addresses block export.
Dual-bank addressing sets the IRW bank and applies LOAD2's +/-0x02000000 TBL SAG
adjustment. The review's IRW view shows both physical and table addresses.
TBL uses the runtime depth control word, supports signed primary anchors, optional
MK3 secondary anchors and an optional palette pointer on **every** included record.
Invalid/duplicate sprite labels, conflicting palette labels and invalid visible
indices are blocked. Referenced palette definitions are exported separately.

IRW uses the actual DOS layout: a 44-byte header (16-bit version checksum/magic),
one 24-byte record and little-endian packed words with their 32-bit checksum.
This matches shipped MK2 IRWs and the existing `makevrom.py` parser. Studio's
single record also supports sprites starting between words under Packed/Nibble
alignment. ANILST reproduces the sequential `.asg 1,N` / `.word N,index` list;
it does not export embedded animation sequences or scripts.

These exports do **not** preserve existing ROM allocations or reproduce LOAD2's
compression, deduplication, scaling, multipart boxes or manifest directives.
Use LOAD2 for those builds. Classic's exporters keep their existing behavior.
Source reviews, outputs and existing-destination snapshots are bounded to 128 MiB;
geometry is bounded as for direct Studio editing. The export cannot target the
source IMG or another open library; source-path aliases are checked as well.

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
file is replaced on each launch. Unhandled Windows memory faults also record the
read/write/execute address and save a uniquely named `startup-crash-*.dmp` in that
folder. Dumps remain available across subsequent launches. Release builds produce
`imgtool-studio.pdb` beside the executable for interpreting those crash stacks.
SDL/window/renderer failures display their reason and log location. Hidden smoke
checks write diagnostics beside their PNG instead.

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

**Palette > Remove unused palettes...** reviews cleanup across the whole library.
Palettes referenced by any sprite or active alternate-palette entry are protected.
Unused palettes start selected for removal; uncheck any to keep them, or use
**Keep all** / **Select all unused**. **Only unused** hides protected rows. Select
a row to inspect its colors; the table shows current and resulting palette indices.

**Remove selected palettes** uses one Undo step and remaps all retained primary
and alternate references. Pixel indices, colors, palette sizes/depths, sprite IDs,
anchors, points and embedded sequences/scripts stay intact. Default alternate
references and bytes after their terminator are preserved. Invalid palette data
or unresolved references block cleanup. The library must keep at least one palette.
Apply never saves automatically, and Save retains the original IMG version and
record layout. External palette-name references in ASM/TBL files or closed
projects are not scanned; keep palettes still needed by those sources.

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
the current tick on the chosen background, including enabled reference scenery,
without editor guides or ghosts.

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

**View > Stage reference...** loads a matching BDD/BDB pair behind animation,
embedded-sequence, and scene previews. Choose either file, select a plane, then
use **Align plane to floor**, camera X/Y and opacity to place the scenery.
Dropping a BDD/BDB opens this review. Failed loads retain the previous scenery;
missing placements/colors are reported. These are owned read-only reference
pixels. Settings stay in the library session, appear in scene PNG exports, and
must be reloaded after reopening a project. Game parallax, camera offsets and
runtime floor layers require game source and are not reconstructed here.

**View > Link animation anchors...** selects reference and target sprites from
any open libraries. Click matching opaque features, or drag from Reference to
Target; choose each facing, mirror convention and the target staging offset.
The aligned preview leaves artwork untouched until **Apply target anchor**.
The target receives one undoable primary-anchor edit; its pixels, secondary
anchors and the reference remain intact. **Undo target**, **Redo target** and
**Save target IMG** address that library even when it is not the active tab.
Escape resets the points. Changed sources require picking the features again;
coordinates outside signed 16-bit range are refused. No sequence propagation.

**Sprite > Point table...** (also in the inspector) edits the IMG's existing
optional ten-point block. Enable/disable the table, type signed X/Y coordinates,
drag handles, or right-click to place the selected point. Connecting points is
only a preview aid. **Apply** is one undo step; **Cancel** leaves stored data
unchanged. **Reload stored points** refreshes a stale review. Points can sit
outside the sprite; zoom and middle-drag pan help locate them. These are generic
IMG coordinates, not MK2 strike boxes. Saves keep the original IMG layout.

**File > Compare existing TBL...** (or dropping a TBL) compares dimensions,
primary/secondary anchors and palette labels for marked, shown, all or selected
sprites. Select a row to inspect individual differences and reveal its sprite;
**Copy report** copies the findings. Runtime DMA flags and ROM SAG addresses are
informational because IMG metadata cannot verify them. Parser warnings remain
visible; duplicate names/labels and overflowing decimal words are refused.
The comparison reads a TBL snapshot: **Compare / refresh** rereads the file and
current IMG scope. Neither file, marks nor artwork history are modified.

**Tools > MK2 strike tables...** opens a separate ASM source workspace. Load
`MKSTK.ASM`, browse/filter records or fighter tables, and edit box offsets/sizes,
routine/sound operands, hit/block damage and score. Shared alias labels are listed
because their source data changes together. Incomplete/unusual declarations stay
visible with editing disabled; extended fighter-table names are supported.

Select a sprite from any open library for a manual preview. Its object origin,
facing and IMG mirror convention determine placement; the box follows `JOY.ASM`
`strike_check`: normal X = origin + offset - width, flipped X = origin - offset,
Y = origin + offset. Drag corners to resize the preview, then **Apply record**
commits one source-history step. **Undo source**, **Redo source** and Ctrl+Z/Y
address the ASM document. Escape resets the unapplied preview. Previewing and
editing strikes leave IMG pixels, anchors, history and format untouched.

**Save source...** / **Save source As...** review changed lines and the destination
before **Write source**. Unedited bytes, comments, indentation, BOM, mixed line
endings and the final newline survive. External file changes require a new review
or Save As; a failed write retains the existing destination. Close, source
replacement and application quit offer Save/Discard/Keep editing for applied
unsaved ASM changes. Numeric fields are checked against their native word/long
ranges. Symbolic operands are preserved; this does not evaluate includes,
conditional assembly, expressions or game script control flow. Active-frame
timing and authoring new phase lists remain separate migration work.

**Tools > ASM animations...** loads a read-only character/exported animation ASM.
Open the move's IMG libraries first, or use **Open IMG library...** in the viewer.
Browse/filter animation labels, step frames with the arrows, or play/pause with
Space. Multipart pieces resolve across all open libraries by stable identities.
Choose a preferred library when sprite names repeat; other ambiguous/missing
pieces are listed rather than silently substituted. **Feet to floor**, object
origin, facing, pan and zoom affect the preview only.

The viewer reads frame groups, label aliases, word adjustments, horizontal/vertical
flips, segments, source jumps and exported local anchor tables. Jumps traverse each
source block once; preview Loop repeats the collected frames. Runtime calls are
reported without running them; unknown/conditional commands stop with a partial
preview warning. Includes and conditional assembly are not evaluated. Preview
hold ticks override timing, including steppable no-sleep frames; source segments
do not determine when game code calls each stage.

With strike tables loaded, select a suggested or manual strike association and
show its overlay. **Edit associated strike...** opens that source record with the
animation preview. In the strike editor, **Animate** and the animation selector
replace manual sprite preview; box resize/apply/source undo keep working while
you step frames. Associations do not prove active-frame timing. Both viewers leave IMG pixels,
anchors, history and disk format untouched.

**Tools > Reactions...** opens the reaction view of the same ASM inspector.
Load a character ASM and its IMG libraries, then browse Hit, Stagger, Knockdown,
Thrown, Getup and Death groups. Search matches labels, source comments, thrower
names and classification reasons. Family and **Show other animations** filters
help inspect entries the name-based suggestions missed. The selected entry shows
its classification reason; these groups do not establish game behavior.

The **Frame anchors** table lists each frame's pieces, stored signed anchors and
placement relative to object origin. Select a row to scrub, then **Open selected
sprite** to edit its IMG anchor in the artwork workspace. Reopen Reactions to
check the result; edits and Undo refresh the table. Facing and each library's
`ani2` / `ganiof` mirror convention use the same placement as the preview, including
source offsets and vertical flips. Missing and ambiguous pieces remain visible;
choose a preferred library when names repeat. Library changes refresh resolution.
Very large animations show at most 32768 table rows with an explicit notice.

Browsing changes no IMG data or source files. Opening artwork for edits uses its
normal history and save flow; its existing IMG version/layout stays fixed. Source
ASM is read-only. The preview does not run game routines, infer attack/reaction
synchronization, or save reaction classifications. Use the All animations view
for strike associations and the full source preview controls.

**Tools > Fatality source...** keeps up to 16 ASM sources open in this Studio
session. Use **Add source** for MOVES.ASM, button-combo or character animation
files, then switch with the source selector. Applied edits, Undo/Redo and the last
selected block stay with each file. The source list marks modified files and
shows an unsaved count. Adding an already open path or file alias selects its
existing document without rereading it or losing edits.

Search labels/routines/triggers or filter block types/body endings. **Open linked
button combo** prefers matching combos in the current source, then a unique match
in another open source. Multiple matches require an explicit source/line choice;
missing combos remain visible. No files are loaded automatically to guess a match.

The label line stays fixed. Edit the indented body, then **Apply block** to put
that change in the source's own Undo history. **Reset block** discards the review;
choosing another block or source also resets unapplied text. Existing labels cannot be
renamed or new labels introduced in the body. Inline data on a label line is
read-only in this pass. Body edits can insert or remove instructions/data lines;
this is raw source editing, without assembler or game-code validation.

**Save source...** and **Save As...** review the destination's actual text against
the applied edits before writing. Apply or Reset pending text first. In-place
saves reject external source changes; every write rechecks its destination and
uses a temporary sibling file before replacement. Close, reload and quit protect
applied unsaved edits. **Reload source** rereads the selected file after Save or
Discard; **Close source** removes that document after its guard completes. Done
hides the editor after handling the active file; other loaded sources stay in
memory. Quit checks every modified source, including inactive files. Each source
saves separately; Save As cannot replace another open source. Source Undo/Redo
remains independent of IMG history.
Untouched bytes, BOMs and line endings stay intact; edited bodies use their label
line's ending, and no-op reviews preserve mixed endings exactly.

**Inspect animation...** snapshots the applied text in all open fatality sources
and expands the selected animation in the bounded ASM inspector. Jumps and
multipart frame groups prefer definitions in the referring file, then a unique
match in another open source. Missing jumps and ambiguous definitions stop with
a partial-preview message; source names remain case-sensitive. Local anchor
tables use the same lookup and are skipped if ambiguous. Frames show their
operand's source filename and line, with the full path on hover. Open the needed
IMG libraries to resolve artwork independently of the ASM definitions.

The snapshot includes applied unsaved source edits. Returning to editing, Undo,
reload or closing a source does not alter a preview already captured; inspect
again to refresh it. **Back to fatality source** returns to editing. Preview is
limited to 16 sources, 4 MiB per file / 16 MiB total, 200000 total operands and the
existing expansion limits. Unsupported code stays visible as a preview limitation.
Nothing here assembles code, follows includes, runs routines, or infers
attacker/victim timing. The source collection is session-only, without a project
manifest or batch Save. Asset staging remains separate. IMG artwork and its
existing version/layout remain unchanged.

**Review artwork...** in the ASM / Reactions preview lists all artwork symbols
used by the selected expanded animation, with use counts, first frame/source
locations and matching open IMG libraries. Filter by name or **Unresolved only**;
**Copy unresolved names** copies the complete animation's missing, ambiguous and
unavailable choices as one list, independent of the visible filter.

Select a symbol to choose among matching sprites, jump to its first frame, or
open its resolved sprite in the artwork workspace. A per-symbol choice overrides
the preferred-library rule for this loaded source preview and is shared with the
canvas and reaction anchor table. **Automatic** restores normal lookup. Choices
use stable library/sprite IDs: moving a sprite keeps the choice, while closing its
library, deleting it or changing its name makes the choice visibly unavailable.
Studio does not silently substitute another match. Undo can restore a valid choice;
reopening a library gets a new identity and requires choosing again.

Choices remain while browsing animations in the same loaded source. Reloading
the ASM or capturing a new source snapshot clears them; **Refresh** in the artwork
review rebuilds the inventory and retains choices. They are session-only and do not rewrite ASM,
IMG names, pixels or file formats. Artwork edits use the normal IMG Undo/Save.
Partial ASM previews explicitly list only the pieces the inspector expanded.
**Prepare artwork library...** captures every resolved piece into an owned review,
combining sprites from the chosen open libraries. Resolve missing, ambiguous or
unavailable choices first. All contributing IMGs must use the same version; the
new library inherits that original layout. Mixed versions are refused. Partial
ASM previews require **Include expanded frames only** before preparing the review.

Review each sprite, its source library and palette/anchor details, then choose
**Create unsaved IMG**. The review stays fixed if a source changes or closes;
return to the artwork list and prepare again to capture newer edits. Creation
opens a separate tab with no placeholder sprite or creation Undo step. Save As
writes it through the normal IMG save flow. Source files, edits, selections and
Undo histories are unchanged by collection. Back to artwork review cancels the
prepared snapshot without creating a tab.

Pixels, names including raw trailing name bytes, primary/secondary anchors, point
tables, baselines and alternate appearances are retained. Required palettes are
reused only when colors and metadata match exactly; otherwise they are copied,
with references remapped. The review supports up to 2000 required symbols and the
usual IMG copy limits. It includes artwork only: ASM, embedded sequences/scripts,
external game references and build outputs are separate. Unopened libraries are
not searched automatically.

**Tools > Strike associations...** (or **Manage / save associations...** in the ASM
viewer) manages explicit move associations. Studio reads the original
`MKSTK.imgtool` sidecar beside `MKSTK.ASM` automatically. Choose **None** to disable
a suggestion, or **Suggest from names** / **Use suggestion** to remove an override.
In the sidecar, `a_move = stk_move` stores a pair and `a_move = -` disables it.
The classic viewer also treats `-` as a bound label with no matching strike.

Association changes have their own Undo/Redo, reviewed **Save associations...**,
and **Reload from disk...**. Saves preserve comments, unknown lines, unrelated
characters, BOM, whitespace and line endings; external changes require reloading.
Unusual assignments stay visible and cannot be rewritten through the picker.
Assignments to missing strike labels suppress the overlay rather than silently
falling back to a different box. Switching animation ASMs retains the strike
source's associations; switching strike ASMs keeps each sidecar's unsaved edits
available in the workspace's file selector. Strike Save As selects a separate
sidecar for the new source path; existing sidecar edits remain available.
Quit offers Save/Discard/Keep editing for each changed sidecar, independently of
ASM and IMG changes. Neither IMG format nor source ASM changes when saving
associations.

**Tools > Strike phase inspector...** (or **Inspect phase countdown...** in the
strike editor) previews the per-phase lookup declared in the loaded `MKSTK.ASM`.
Choose a character-table slot, a base move slot and a manually supplied active
window. Step or play the countdown; zero removes the strike overlay. The selected
box is red, with the base box shown in gray for comparison. **Edit selected
strike...** opens that box in the existing source editor. With the stkphase
preview assumption enabled, **Edit phase rows...** stages changes to existing
thresholds and strike slots. Preview the new countdown selection, then Apply or
Cancel. Thresholds must decrease strictly and end at zero; slots must resolve to
complete strike records for every active consumer of the list under the chosen
preview flags. Shared lists show affected character/base slots. Validation refuses
unresolved consumers, symbolic or conditional rows, and row-count changes.

Applied phase changes share the ASM source undo/redo history with box edits.
**Review source save...** opens the existing reviewed save dialog; no file is
written by Apply. Unchanged operands, comments, whitespace, line endings, BOM and
DOS EOF are retained. This edits word pairs in ASM, never the IMG format.

**Assume stkphase enabled** and **Assume wunit enabled** affect the preview only.
Both start off; they do not read or modify build settings. Conditional strike
slots follow those choices, including appended entries and inline slot labels.
For the existing Liu Kang low-kick example, enabling phase preview selects
`stk_lklokick_early` (slot 0x17 = 23) at countdown 6..4 and `stk_lklokick` at 3..1.
With phase preview off, slot 23 instead names `stk_lkdubknee` because the early
row is excluded. The resolver observes native word/long layout, the signed -1
long directory sentinel and the source's `PHASE_MAX` scan limit; malformed data,
symbolic values, unknown conditions and external tables report unavailable results.

Choose a sprite or an ASM artwork frame independently, with facing/origin/pan/zoom.
The tool does not infer startup, active timing, recovery, hit interruption or
which artwork frame the game draws during the active window. Includes and assembly
assertions are not evaluated. Opening, stepping or changing preview flags leaves
source ASM, association files, IMG data, history and format untouched. Applied
strike-box and phase-row edits use the existing ASM history and reviewed save
workflow. Build settings stay unchanged; validation uses the explicit preview
profile and does not establish behavior for other profiles.

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
