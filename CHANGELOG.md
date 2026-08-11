# Changelog

All notable changes to midway-imgtool. Tags carry the version, e.g. `v2.3.0`.

The workflow at [.github/workflows/c-cpp.yml](.github/workflows/c-cpp.yml)
extracts the section matching the pushed tag and uses it as the GitHub
Release body. Keep new entries near the top of the file under a new
`## [vX.Y.Z]` header — anchor exactly as `## [v2.3.0]` (square brackets
included) so the extractor matches.

## [v3.23.0] — Playback speed that matches the hardware, and stance-levelled anipoints

### New timelines no longer play at 54.7 fps

Playback is driven at the hardware tick rate, and every new timeline frame held
for exactly one tick — so a freshly built sequence ran at 54.7 fps, which is not
a speed any MK2 animation uses. World View had already been fixed to default to
4, but the Image timeline had its own hardcoded `1` and the two had no reason to
ever agree again.

Both now share `kDefaultTimelineHold` (4 ticks, ~13.7 fps at 54.7 Hz), so the
two previews cannot drift apart.

The tick model itself was already right and has not changed: ticks are what you
write into the ASM, and 54.7 Hz is fixed hardware, so expressing the default as
an fps number would have been a step backwards. What was missing was the
arithmetic. The Image timeline's Hold tooltip claimed "At 12 FPS, Hold 3 lasts
0.25 seconds" — a rate the timeline was not running at. It now reads the live
rate and spells out the real result in both ms and fps.

### Level Unset Anipoints from Stance (Operations menu)

Frames added to a shipped library arrive with `anix/aniy = 0,0`, anchoring them
to their own top-left corner. Played back they bob against the original art,
because each frame's pixels sit at a different height inside its own box —
BOSS8's four `BGEYESHOT` frames drift 19px between them.

The shipped `STANCE` frame knows where the ground is, so it supplies the line.
The command finds it, measures its ground line (bottom of its opaque art
relative to its anipoint), and stands every unset same-palette frame on it.

- Grouping is by **palette**, not sprite size. In real libraries the character's
  frames share one palette while effects carry their own (BOSS8: pal 3 boss,
  pal 4 spark), whereas no two sprites in the file share a size — matching on
  size would have matched nothing.
- Frames on a **different** palette are effects with no feet, so they are
  centred on their own opaque pixels rather than stood on the ground line.
- Only frames still at `0,0` are touched. `BGHAMMERTOP1-6` agree with each other
  on a ground line 15px below the stance's; whether that is wrong is a judgement
  call for a human, not a bulk edit, so they are left alone and counted in the
  result message.

The reference is the stance specifically rather than whichever ground line the
most frames agree on: the stance ships with the game, the frames being fixed
were added afterwards, and a majority of added frames being wrong together does
not make them right.

Logic lives in `platform/anipoint_level.{h,cpp}` as a pure module with unit
coverage, including the BOSS8 shape and the case where an already-correct frame
must not be reported as a change.

### The eraser and stamp shortcuts that were never wired

`E` and `C` were announced as the Smart Eraser and Clone Stamp shortcuts when
those tools landed, and documented that way ever since. They were never bound.
Both tools have been toolbar-only the whole time, and the in-app help had
quietly corrected itself to say so — `(no shortcut) Smart Eraser, Clone Stamp,
Smart Remap` — leaving the changelog as the only place still claiming
otherwise.

Both bare keys were free (only `Ctrl+E`, `Ctrl+C`, and `Ctrl+Shift+C` were
taken), so the advertised bindings now exist rather than the claim being
withdrawn:

- `E` toggles Smart Eraser, `C` toggles Clone Stamp, matching how `P`, `G`,
  `V`, `R`, `W`, `L`, and `I` already behave — a second press returns to no
  tool.
- Neither takes a `g_pasted` guard. `H`, `V`, and `L` need one because they
  mean flip-horizontal, flip-vertical, and drop-to-layer while a paste is
  floating; eraser and stamp have no floating-paste meaning.
- Smart Remap, Blur, Smudge, and Content-Aware Erase stay toolbar-only. The
  in-app help now lists them under that heading explicitly instead of implying
  the list of keyless tools is shorter than it is.

Documented in the in-app help (`h`), the README key table, and the wiki.

### The help text is now the wiki's source of truth

The shortcut list existed in two places that had no way to agree, which is how
`E` and `C` stayed wrong for as long as they did. The wiki's Keyboard Reference
page is now generated from `g_help_text` by
`tools/wiki/gen_keyboard_reference.py`, and `.github/workflows/wiki-sync.yml`
republishes it on push to `SDL-main`. Only that one page is generated; the rest
of the wiki stays hand-written and browser-editable.

Generating from the help text immediately turned up things wrong with it:

- `Ctrl+Shift+C` (Copy to New Sprite) was bound and in the Edit menu but had
  never been listed.
- `Alt+PgUp / PgDn` and `Ctrl+Left / Right` abbreviated the second key of each
  pair, which reads as `PgDn` and `Right` being bare shortcuts.

The generator parses the fixed-column console layout rather than guessing at
whitespace — descriptions start at column 23, and keys that outgrow the column
push right. Two-space runs occur *inside* keys (`Alt+L  / Alt+S`) and *between*
key and description (`Alt+PgUp / Alt+PgDn  Move ...`), so neither a naive split
nor a fixed column works alone. A malformed entry fails the run instead of
publishing a mangled table.

## [v3.22.0] — Erase Copied Object, and tabs that agree with themselves

### Erase Copied Object (Operations menu)

Copy an object once, then subtract it from every frame it was composited into.
Built for the case where a sprite is reused verbatim under an effect —
UMK3FIRE.IMG holds a clean skeleton in UMK3SKEL18 and the same skeleton under
a flame in UMK3SKEL4..17 — and getting the flame on its own means removing the
skeleton from fourteen frames by hand.

Nothing has to line up first. Each frame is searched for the object by its own
pixels: samples taken from all over the object vote on where it would have to
be, votes are weighted toward colors the frame rarely uses, no single color may
cast many (so a large flat region cannot outvote everything else), the
best-supported placements are scored properly, and the winner is walked to its
exact position. Anipoints are not consulted, and the frames being different
sizes does not matter. About 1.3 ms per frame.

Only pixels whose palette index matches the object exactly are cleared, so
whatever was painted over it stays. That is the whole trick: where the flame
covers the skeleton the indices differ, so those pixels survive and the flame
comes out whole.

- Scope is marked frames, all frames, or just the selected one. The frame the
  clipboard came from is skipped by default — it matches itself perfectly and
  would be wiped clean.
- Every searched frame is listed with its match percentage, where the object
  was found, and how many pixels would go. Clicking a row jumps the editor to
  that frame. Rows are ticked by threshold and can be overridden by hand.
- Do not expect high percentages. Against the real UMK3FIRE art the frames that
  genuinely hold the skeleton score 21-35% — the flame recolors most of it —
  while frames that do not hold it sit under 12%. The gap is the signal, which
  is why the threshold defaults to 20% rather than something confident-looking.
- The whole sweep is one undo step.
- Matching and subtraction live in `platform/stamp_erase.{h,cpp}`, unit-tested
  including the heavy-occlusion case (three quarters of the object repainted,
  placement still exact). The search agrees with an exhaustive one on 24 of
  UMK3FIRE's 26 skeleton frames; the two it differs on are frames the object is
  not in.

### Document tabs stop disagreeing with the editor

The highlighted tab and the file actually being edited could end up being two
different files, with no way back short of clicking around.

Tab IDs were built from the tab's own label and its slot index, so both halves
moved constantly. The moment an edit added the `*` dirty marker, ImGui saw the
old tab disappear and a new one appear, dropped the selection, and fell back to
whichever tab it had highlighted least recently — the active document had not
changed, so nothing ever put the highlight back. Closing a tab renumbered every
slot after it and did the same thing, and a drag-reorder applied its
permutation twice, once inside ImGui and once to the backing store.

- Documents now carry a stable uid, minted once and never reused, and the tab
  is keyed on it alone (`###`). Renames, the dirty marker, neighbours closing,
  and reorders all leave a tab's identity untouched.
- Clicks and closes are recorded by uid too, so a drag that reorders and
  selects in the same frame no longer loses the click.
- Exactly one tab may claim the selection per frame; two used to, and the later
  submission silently won.
- Whatever ImGui is drawing as selected now wins on any frame we did not drive
  the selection ourselves, so a stray selection change resyncs instead of
  persisting.
- Undo no longer stamps another document's identity onto the open tab: the
  history stack is app-wide, and restoring a snapshot taken in a different tab
  used to hand two tabs the same ID.

## [v3.21.1] — Anim tab playback frame rate

Fixes juddering playback in the Anim workspace. The frame browser submitted
every row of the library on every frame — ~470 rows across a character's ten
IMGs — and resolved each one with `doc_get_img`, which walks the image list
from its head. Two more full passes ran for the marked count and the keyboard
handlers' nav check. The result was hundreds of thousands of pointer hops and
several hundred string allocations per frame, which dropped the render rate
below the animation's 54.7 Hz tick rate.

Ticks advance from elapsed real time, so the *timing* was right the whole
time; there simply were not enough rendered frames to show it, which reads as
judder rather than as wrong speed.

- The browser resolves rows through an index table built once per draw instead
  of an O(n) list walk per row.
- `ImGuiListClipper` submits only the rows actually on screen, with the
  keyboard selection force-included so stepping still scrolls to it.
- The marked-count and Mark Shown / Clear passes share the same table, and the
  nav check no longer touches IMGs at all.
- The Anim toolbar shows the render rate beside the tick rate, and warns when
  it falls below it, so under-sampling is distinguishable from bad timing.

## [v3.21.0] — World View stops fighting you

Headline: three things in World View were quietly broken rather than merely
awkward. A sprite anchored past the playfield edge was drawn out there but
could not be grabbed. Two drag gestures — the single-sprite anipoint drag and
the whole Link tool — could never fire at all. And playback ran at 54.7 frames
a second, because a frame's hold defaulted to one tick and one tick is one
video field on real hardware.

Alongside those: World View placement can now be baked into the IMG, draw
order belongs to the slot instead of the entry, the preview tick loops instead
of counting forever, and the lane row is grouped by scope rather than being one
25-widget wall.

### Reachability and dead gestures
- **The grab region is the canvas, not the playfield.** Sprites draw unclipped,
  so one anchored past the world edge is visible out there and has to stay
  reachable — it is usually the frame whose anipoint most needs fixing. The
  floating lane panel still wins wherever it overlaps.
- **Two drag gestures were unreachable.** Both gated on `!io.WantCaptureMouse`
  alone, and ImGui raises that flag for its own windows, the canvas included —
  so the condition was false wherever the gesture actually happens. This killed
  the World View single-sprite anipoint drag and the Link tool's
  reference-to-target drag outright. The correct idiom already existed on the
  pixel-editing path: block only when something is *on top of* the canvas. It
  is now a named helper, `CanvasInputBlocked`, used at all three sites.
- An in-progress drag already survived leaving the world; only starting one was
  blocked.

### Timing
- **Ticks/frame** sits beside the transport in both preview headers. The tick
  rate is hardware and does not move; the hold is authoring, and it is the same
  number you write into the ASM. Changing it rewrites every lane's per-frame
  hold and rewinds the tick clock. The tooltip does the arithmetic.
- **The default hold is 4**, or 13.7 fps. It was 1, which at MK2's 54.7 Hz field
  rate meant 54.7 fps — a speed no animation in the game plays at, and the
  reason a freshly marked set was unwatchable.
- Note that 12 fps is not expressible: 54.7/12 is 4.56 ticks and the machine
  only holds whole ticks. Hold 4 is 13.7 fps, hold 5 is 10.9 fps.
- **ASM-imported lanes stay at one tick**, and must: an ASM animation encodes a
  hold by repeating the frame label, so the repeats already carry the timing and
  a default hold would multiply it. SEQSCR records keep their authored ticks;
  the dummy body keeps its canned timing.
- **The preview tick loops.** Lanes already wrapped on their own length, but the
  global tick feeding Show@/Hide@/Stop@ climbed forever, so a scene that had
  visibly finished kept running and those absolute-tick fields drifted out of
  reach. It now wraps at the longest visible lane, extended past anything
  scheduled beyond it. It keeps counting when something deliberately parks the
  preview: a lane holding its last entry, a tick stop, or a ping-pong chain
  whose reversal is computed from the running tick.

### Baking placement into the IMG
- **Inherit Position from World View** and **Inherit All from World View Slot**,
  in Anipts Tools. A marked lane positions a frame at
  `origin - (anipoint + local dX/dY)`, and those deltas are preview state — a
  drag in World View looked right and then saved as nothing. These fold the
  delta into the sprite's own anipoint. Nothing moves on screen; the placement
  just becomes real.
- One IMG can appear in several entries, since duplicating an entry to place the
  same sprite twice is supported. Baking both would apply two deltas to one
  anipoint, so the first wins and the rest are reported rather than silently
  compounded.
- Both are disabled, with an explanation, when the selected sprite is not in a
  lane.

### Lane row
- **Z is per-slot.** Draw priority answers "which lane is in front", and a lane
  whose Z changed halfway through would pop through the one it overlaps. The
  control moved up to the slot row and writes every entry at once; the per-entry
  array stays because the ASM export annotates each entry.
- **Grouped by scope.** A `Row...` menu holds the structural edits — split,
  duplicate, reset, delete — which are the destructive ones and were sitting one
  stray click from "move entry later". A `Timing/FX` popup holds Show@, Hide@,
  vX, vY, StopY and the whole Dual group; it shows a marker when the entry is
  using any of them, so nothing goes quietly missing behind it. The entry row
  drops from roughly 25 inline widgets to 12.

### View tabs
- The World View display toggles moved off the document tab strip, where they
  read as files and outlived the view that used them, onto the canvas view-mode
  tab bar — trailing, and only while World View owns the canvas, the rule the
  backdrop button already followed.
- **Their labels no longer change width.** "Onion" growing to "Onion: On"
  re-laid out the whole trailing group, so every button jumped out from under
  the cursor on click. State is carried by the selected tint and named in the
  tooltip.

## [v3.20.0] — the ROM readout tells the truth, and the tabs agree with each other

Headline: the Sprite panel's DMA ROM figure is now a port of LOAD2's own
packaging math rather than an estimate of it. The old readout assumed one byte
per kept pixel — that is 8bpp, and MK2 ships `PPP> 6`, so it overstated every
packed sprite by a third before the run-length quantization was even
considered. `zcom_analysis()` is now reproduced statement for statement from
`doc/load2/zcom.c`, with unit tests whose expected values are derived from the
original so the port fails the build if it drifts.

Alongside it: the canvas view tabs and the sidebar tabs stop disagreeing about
what you are working on, and the anim point fields become something you can
actually aim at.

### ROM cost
- **`platform/dma_pack.{h,cpp}`** — new pure module. `DmaAnalyzeSprite()` ports
  zero compression including both clamps the estimates skipped: a field encodes
  at most 15 run units (so a 28-pixel margin needs unit 2, not unit 1), and no
  compressed line may keep fewer than `ZCOMPIXELS` pixels, which hands margin
  back on small art in a wide frame. `DmaSuperBpp()` ports the `do_superbpp`
  auto-packing ladder.
- Faithful to the original's quirks, deliberately: fully transparent art packs
  at 2bpp, because `max == 0` misses `== 1` and lands on `< 4`. Documented at
  each site rather than corrected — a model that silently disagrees with the
  packager is worse than no model.
- **The Sprite panel** now reports `raw @ Nbpp` and packed bytes with a percent
  saved, and names the DMA control word and both run units on hover. When
  compression is refused it says which rule refused it (too narrow, taller than
  LOAD2's 256-line `zero_array`, or simply larger compressed than raw).
- **A `fits Nbpp` line** appears when the art does not need its declared depth.
  Crossing a power-of-two boundary cuts every frame drawn from that palette,
  and nothing else in the pipeline comes close — 41 colors is 6bpp art, and
  getting under 32 buys a whole bit per pixel.
- `AutoChopBppForImage` became `Load2BppForImage` in `ui_autochop.h`, so the
  panel and the chopper cannot answer "what depth will this pack at"
  differently. Both honor `PPP>` with the palette-depth override.
- Unchanged on purpose: the approximations in `load2_verify.cpp` and
  `ui_autochop.cpp`. Both document themselves as approximate and both are used
  for deltas and for ranking options against each other, where that is fine.
  Unifying them would move SAG-misalign verdicts, which is not a side effect a
  readout fix should have.

### View tabs
- **Anim and Animation are one mode again.** Selecting the canvas `Anim` tab
  brings the sidebar `Animation` panel forward, and selecting that panel puts
  the canvas on `Anim`.
- This also fixes the same sync for `Link`, which never worked: it hung off
  `IsItemActivated()` inside the tab body, and ImGui queues tab selection, so
  the frame a tab is clicked is never the frame its body runs. Both directions
  now key off the selection transition instead.
- Switching to `Animation` while the Link workspace is open leaves the canvas
  alone, rather than kicking you out of Link to satisfy the pairing.

### Anim points
- **Anipts Tools opens by default** on the Sprite tab.
- The five full-width drag bars become two captioned rows of compact fields —
  `Point 1 (anchor)` with X and Y, `Point 2 (optional)` with X, Y and Z — so
  the grouping shows which numbers move together and the widgets stop reading
  as scroll bars. Hover either heading for what the values mean.
- Behavior is unchanged: drag to scrub, double or ctrl-click to type, Left and
  Right nudge a pixel at a time, and World View still reserves those keys for
  frame flicking.

## [v3.19.0] — the Anim workspace, a character-wide frame library, and real game timing

Headline: sequence and script editing has its own workspace instead of riding
along inside World View, and it edits the IMG for real. The entry table now
offers exactly the four values a SEQSCR ENTRY can hold — target index, tick
hold, dX, dY — and writes them back into the record. Everything World View
layered on top (show/hide ticks, motion vectors, Z order, dual instances, the
auto-chain generators) was preview state with nowhere to live in the file, and
offering it in a SEQSCR editor invited edits that quietly evaporated on save.

Alongside it: a frame browser that spans a character's whole numbered IMG set,
previews at the machine's real 54.7 Hz, three new paint tools, and two fixes
for bugs that could lose work.

### Sequence / Script workspace
- **New "Anim" canvas mode**, beside Image / World / Link. The loaded record
  animates in a world-sized viewport, the entry under the playhead shows 1:1 in
  a corner inspector with its anipoint crosshair, and the record list plus its
  entry table sit underneath. It takes the whole canvas rect — no palette strip
  or sprite timeline competing for the space.
- **Removed from World View.** The embedded SEQSCR lane, its tables, and its
  header controls are gone; World View is marked rows and ASM lanes again.
- **Edits persist.** `SeqScrReplaceEntries` rewrites a record's ENTRY array,
  carrying the three spare words across by position so Midway metadata on
  entries that stayed put survives. Typed values, row reorders, duplicates,
  deletes, and viewport drags all reconcile into the blob.
- **Sequence ENTRY arrays are stored back to front** — blob entry 0 is the last
  displayed frame; scripts store forward. The decoder and the ASM exporter both
  already knew this; the new write path mirrors it. Getting this wrong would
  have silently reversed every sequence it touched.
- **Untouched records are never rewritten.** The decoder normalises as it reads
  (a 0-tick entry becomes a 1-tick hold, deltas get clamped), so the sync
  compares against a baseline captured at load rather than against the blob.
  Opening a sequence no longer dirties the document.
- **Game placement.** The stock (200, 20) anchor hangs an animation off the top
  of the playfield. The anchor Y is now derived from the first drawable frame so
  its feet land on the floor, and each entry offsets from there by its own
  dX/dY — how the sequence actually reads on screen. Right-click the toggle for
  Floor Y / Stand X.
- The entry table sizes itself to its row count, so a short sequence is fully
  visible without scrolling.

### Frame library
- **The Animation panel now browses a character's whole IMG set.** Opening
  `CAGE3.IMG` discovers `CAGE1`–`CAGE10` in the same folder, opens the ones that
  are not loaded, and restores your active tab so browsing does not move your
  editing focus. 468 frames for Cage, 406 for Kang.
- Parents only — chopped pieces are folded away and their parent carries a `*`.
- Up/Down walk the list, each highlight previews in the corner box, Space marks.
  Marking uses the IMG's own mark bit, so a selection made here is the same one
  World View rows and TBL export already act on.
- **Add Marked to Sequence** / **New Sequence from Marked**. Frames from a
  sibling IMG go into the preview and the ASM export but not the blob — a
  SEQSCR index cannot name a sprite outside its own file — and both the table
  and the panel say so rather than writing an index that resolves elsewhere.
- Marked pieces of one chopped drawing (`BGBIGFIST1A/1B/1C/1D`) now reassemble
  into a single multi-piece lane entry instead of four separate frames.
- Subframe groups start collapsed. A group with no record of its own in the IMG
  gets the folder icon, a piece count, and a right-click menu, instead of
  rendering as an unlabelled gap.

### Timing
- **Previews run at 54.7 Hz**, the rate MK2 drives its game logic on the
  TMS34010. A SEQSCR tick is one of those, so a hold of N ticks now previews at
  the duration it will have in game. World View, the Anim workspace, the sprite
  timeline, GIF export, and the ASM viewer all default to it, each with a
  "Game" button to snap back. The ASM viewer's 30 fps ceiling was raised so it
  can reach game speed at all.

### Paint tools
- **Blur**, **Smudge**, and a **content-aware eraser**. All three read indices,
  resolve them to the palette's 15-bit colors, do the arithmetic in RGB, and map
  back to the nearest index — averaging index *numbers* lands on whatever color
  happens to sit between two slots, which in a hand-authored MK2 palette is
  usually an unrelated hue.
- Blur and smudge neither read transparency as a color nor write over it, so a
  stroke near an edge softens the art without bleeding the silhouette outward.
- The content-aware eraser clears its brush and then diffuses color inward from
  the ring outside it, so a patch of texture closes over instead of leaving a
  hole. Where the neighbourhood is entirely transparent it stays transparent —
  erasing at a sprite's edge still trims the silhouette.
- **Reverse Color Order** joins the RGB complement invert: same colors, reversed
  across their indices, so a ramp runs the other way.
- **Clear Region in All Marked Frames** — right-click inside a marquee. The rect
  applies in each sprite's own pixel space, clipped to its bounds, honouring
  lasso and wand masks, as one undo step.

### Fixes
- **Closing a tab could close the wrong file.** The pending close stored an
  index captured when the prompt opened, then closed that index after you
  answered — but anything that opens or reorders tabs in between shifts them,
  and the new sibling-IMG auto-open opens ten at once. It now resolves the
  target by document pointer, and the prompt names the file.
- **A modal could lock the app.** ImGui persists window positions in
  `imgui.ini`, so a dialog once dragged off-screen — or last shown on a larger
  display — reopens outside the viewport: invisible, but still capturing input.
  Confirm dialogs now re-centre on appearing, and a watchdog recovers if one
  fails to draw, cancelling the queued action rather than running it unseen.
  `imgui.ini` is no longer tracked.

### Smaller things
- Document tabs and canvas view-mode tabs are tinted apart; stacked in the same
  corner they read as one strip otherwise.
- Canvas backdrop cycles Checker / Pink / Green / Blue from the Image tab — a
  flat key makes stray fringe pixels obvious.
- The zoom percentage is gone from the menu bar corner.
- Saving auto-completes the mode's extension when the typed name has none, and
  respects one you supplied.
- The active-swatch pop-out gains a hue dial and palette-wide H/S/L controls.

## [v3.18.0] — mirror-aware anipoints, canvas size, and cross-palette paste

Headline feature: a whole class of anipoint error is now visible at authoring
time. The MK1 "toasty" flame in MK2 shipped landing on the victim when Scorpion
stood on the left and ~286 px away when he stood on the right; the art's
anipoints sat outside their own sprites, and nothing in imgtool could show it
because the canvas only ever drew sprites unflipped. See
[doc/ANIPOINT_MIRROR_TODO.md](doc/ANIPOINT_MIRROR_TODO.md).

Alongside it, a round of editing work: a canvas can be resized without
resampling the art, `Del` clears a selection instead of deleting the sprite,
pasting between palettes keeps its real colors, sprites reorder by drag, and
the GIF importer's border trim actually trims.

### Anipoints
- **Mirror preview on the canvas** — a toolbar toggle (and `View > Mirror
  Preview`) draws where the sprite lands when the engine h-flips it, mirrored
  about the anipoint exactly as the hardware does. *Ghost* overlays the flipped
  placement on the normal one with the anipoint as the visible pivot; *Flipped
  only* fades the unflipped sprite instead. An anchor that looks fine unflipped
  and lands 224 px away flipped is now impossible to miss.
- **Mirror convention is explicit** — the engine has two, differing by one
  pixel: `MKUTIL.ASM` `ani2` (`w - x`, multipart) and `MKDISP.ASM` `ganiof`
  (`w - 1 - x`, single-part). Both listings are quoted in
  `platform/anipoint_mirror.h` and selectable under `View > Mirror Preview`.
  ani2 stays the default — that is what this tool has always committed — but it
  is no longer an unexplained literal, and every mirror site in the app now
  routes through one helper.
- **Signed centre-offset readout** — `Ctr off` in Sprite > Properties and
  Anipts Tools shows `c = anix - (sizex-1)/2`, the one number that answers "is
  this anchored on the art or beside it". It negates exactly under h-flip,
  which is why an offset hand-tuned for one facing can never be right for the
  other.
- **Anipoint-outside-box badge** — an amber `!` on image-list rows whose
  anipoint falls outside the sprite's own bounds, with the overshoot in the
  tooltip. A place to look, not a verdict: off-box does not mean wrong, and it
  cannot separate a good library from a bad one. `MKDEATH.ASM`'s spine-rip
  props sit 88 px off their own art and are *correct* — they ride
  `match_ani_points` and carry their whole offset in the anipoint by design,
  so `SPINERIP.IMG` badges all 18 of its frames. Toggle it off for libraries
  built that way under `View > Anipoint Warnings`. The mirror preview is what
  actually tells you whether an anchor is wrong.
- **Bulk numeric anipoint shift** — `Operations > Shift Anipoints by dX/dY...`
  applies an offset to the marked set, a name glob (`MK1FIRE*`), the selection,
  or everything, as one undo step, with a live preview of the count and the
  first frame's before/after. This is the edit that previously had to be
  scripted outside the tool.
- **Undo now covers the whole marked set** — *Mirror Marked Anipoints to
  Reverse* and *Align Marked Anipoints to Selected* took a snapshot that
  captured only the selected image, so undoing a 22-frame edit restored one
  frame. Both now take a full-document snapshot.

### World View
- **Reference figure** — an optional standing-fighter outline at the shared
  anchor, so "will this effect land on the victim?" is a look instead of a
  calculation. Toggle on the World View toolbar (right-click to size, place,
  and mirror it) or under `View`. It is a proportioned silhouette, not art.

### Editing
- **Del clears a selection instead of deleting the sprite** — with a marquee,
  wand, or lasso selection up, `Del` blanks the pixels inside it (mask
  selections erase their real shape, not their bounding box), matching every
  paint program. `Backspace` does the same. Deleting the whole frame out from
  under a live selection was never what that keystroke meant. It only applies
  while the selection is actually drawn, so a stale one can't swallow the key;
  `Shift+Del` remains the unconditional delete-sprite escape hatch, and `Del`
  still deletes the sprite or palette with nothing selected. Also in
  `Edit > Clear Selection Contents`.
- **Canvas Size** — `Image > Canvas Size...` (also on the image-list and
  Operations menus) grows or crops the frame *without touching the art*: the
  sprite keeps its exact pixels and dimensions, unlike `Resize Sprite...` which
  resamples. A 3x3 anchor picker places the existing art, relative mode takes
  "+8 px" instead of a finished dimension, and the dialog warns before a
  shrink that would clip the art. Anipoints and the hitbox travel with the art
  so the sprite's registration doesn't silently shift.
- **Arrow keys nudge a fresh paste instead of changing sprites** — after
  `Paste as New Sprite` (or a canvas resize) the art is already committed, so
  there was no floating rect for the arrows to move and they walked the image
  list off the sprite that had just been created. They now slide the art inside
  its own canvas (Shift = 10 px), clamped so nothing is pushed off an edge, and
  each nudge is one undo step. Esc or selecting another sprite hands the arrows
  back to list navigation.
- **Paste as New Sprite drops the source marquee** — the selection that
  produced the clipboard is in the *source* sprite's coordinate space, so it was
  drawing a meaningless highlight over the brand-new frame.
- **Drag sprites in the image list to reorder them** — grab a row and drop it
  on another to move it there, instead of walking it with Alt+PgUp/PgDn one
  step at a time. Dropping a parent row carries its subframes, since they are
  grouped by name rather than by position. Disabled under a Name or Size sort,
  where the visible order isn't the file order and the row would jump elsewhere
  the moment the list re-sorted.
- **Reordering no longer re-points the animation timeline** — the timeline (and
  the composite-preview pair, and the thumbnail cache) address sprites by index,
  so moving a sprite silently made every timeline slot refer to whatever sprite
  slid into its place. Drag-and-drop and Alt+PgUp/PgDn now share one reorder
  primitive that remaps those indices, verified exhaustively against
  erase-then-insert semantics for every list size and move pair.
- **Bulk trim is reachable from where you'd look for it** — cropping every
  marked sprite to its opaque bounds already existed, but only as `Crop Marked
  to Content` in the Image menu, while the single-sprite version appeared in
  three places under three different names. It is now `Trim Marked Bounds` on
  the Image menu, the image-list right-click menu, and `Operations...`, each
  showing the marked count, sharing one implementation, and reporting how many
  of the marked set actually changed instead of a bare number.

### Import
- **GIF "Trim Border" actually trims now** — it tested alpha alone, and
  `stb_image` only reports a pixel transparent when the GIF declares a
  transparent index. Animations exported from video or a screen capture declare
  none, so every pixel came back opaque, the content box was the whole frame,
  and the checkbox was a silent no-op on most real files (verified: three
  sample GIFs here have exactly zero transparent pixels between them).
  Transparency is still preferred when the file has it; otherwise the trim now
  works against the color the frames' own border is made of, found by sampling
  the outer ring rather than the whole image so a sprite can't nominate its own
  body as background. A **Border Tolerance** slider (default 12) covers GIF
  quantization dithering a "flat" background — at 0 the dither itself reads as
  content. The box is still the union across every imported frame, so trimming
  can never break the frames' registration with each other.
- **The import toast says what the trim did** — the old message just omitted
  the word "trimmed", so "there was nothing to remove" and "the option did
  nothing" looked identical. It now reports the before/after size and the
  background color it trimmed against, or why it couldn't.

### Palette
- **Pasting across palettes can keep its real colors** — a paste from a
  different palette remapped every pixel to the nearest color the target
  already had, which is only necessary when the target is full. It usually is
  not: a 6bpp palette addresses 64 indices, and one holding 42 colors has 22
  going spare. The paste now copies the colors it needs into those free
  indices first, so the art lands in its own colors and the nearest-match
  remap only handles what genuinely didn't fit. It only fills slots the
  palette's declared depth already covers and that no sprite in the document is
  drawing with, so nothing already on screen is recolored. Toggle under
  `Edit > Add Pasted Colors to Palette`.
- **Paste copied colors at the end of a palette** — the existing paste lands
  swatches on their original indices, which is right when the two palettes are
  variants of each other and wrong when the colors are just extra shades being
  collected. `Paste Colors at End` appends them after the target's last color
  in source-index order (so a ramp keeps its dark-to-light run), leaving every
  existing index untouched. On the swatch menu, a palette row's menu, and
  `Operations... > Clipboard & Files`.

### Export
- **Compare against an existing .TBL** — `File > Export > Compare Against
  TBL...` diffs a checked-in table against the open library field by field
  (size, anipoints, palette) instead of overwriting it, since MK2's `.TBL`
  files are hand-maintained with no build step to regenerate them. Also
  headless: `imgtool-cli --compare-tbl <input.img> <existing.tbl>` prints a
  report and exits 1 on drift, so a build can gate on it. SAG and flags are
  not compared — LOAD2 owns ROM addresses, and a table's flags word is a DMA
  control word rather than the editor's mark/loaded/changed bitfield.

## [v3.17.0] — World View PNG export, body-part splitting, and palette depth fixes

Feature release: the World View can be exported as PNG stills or a numbered
sequence, sprites can be cut into head/arms/torso/legs, palette colors can be
copied between palettes at their original indices, and imported palettes now
declare the bit depth their color count actually needs.

### World View
- **World View PNG export** — write the composited scene, with every visible
  lane in its normal draw order, straight to a PNG. Two entry points: the
  current tick, or one numbered file per tick across the whole sequence
  (`<name>_0000.PNG` onward, capped at 600 frames). Available from
  `File > Export` and from `Save PNG` / `Save PNG Seq` in the World View panel.
  Options: crop to visible content (a sequence shares one crop rect so frames
  stay registered) and, off by default, keep the on-screen per-lane
  translucency instead of compositing every lane opaque.

### Import
- **PNG palette matching is no longer order-dependent** — a multi-file
  palette-match import resolves the target palette once, before the first file,
  instead of re-reading the selection between files (each import selects the
  image it just created, so the target could drift mid-batch). The toast now
  names the palette that was matched and reports files that failed to decode.
- **Dragging PNGs onto the window can match the palette** — drops previously
  always built a new palette per file with no way to ask otherwise. They now
  follow the new *Match to Active Palette* checkbox in the Import PNG dialog.
- **Import dialogs no longer pre-select the open IMG** — multi-select import
  dialogs seeded their file list with the current document's `.IMG` name, so a
  Ctrl-click batch carried a bogus first entry that silently failed to import.

### Palette depth (BPP)
- **Imported palettes now declare the depth their color count needs** — every
  import stamped `bitspix` 8 regardless, so a quantized 41-color PNG claimed
  8bpp when it is 6bpp art, and the TBL/IRW/LOAD2 exports (which pack pixels at
  `bitspix`) padded every pixel accordingly. PNG, GIF, TGA, and LBM imports now
  derive it as `ceil(log2(numc))`. `.PAL`/`.ACT` palette import never set the
  field at all and left it 0, which reads as an invalid palette downstream.
- **Recalculate BPP** — repairs palettes already carrying a wrong value, on the
  selected palette or all of them, without touching a single color. In
  `Operations... > Recalculate BPP on All Palettes` (which shows how many are
  wrong) and on a palette row's right-click menu (which shows the current and
  correct value). The debug panel's `BITSPIX` line now flags a mismatch inline.
- **Growth no longer under-declares depth** — growing a palette past what its
  declared depth can address (merging in colors, the inner-stroke ramp,
  `ensure_palette_numc`) now widens `bitspix` to fit. Depth is only ever
  narrowed by the explicit Recalculate command.
- **Fixed: IRW export ignored its own computed depth** — `WriteIrwFromMarked`
  resolved a per-record bpp for both auto modes and then packed with the raw
  parameter instead, writing 0 bits per pixel for *Auto (Image Data)* and
  `(unsigned)-1` for *Auto (Palette Size)*. Fixed-bpp exports were unaffected.

### Palette
- **Copy/paste multiple palette colors at the same indices** — Ctrl/Shift+click
  several swatches, *Copy Selected Colors*, then paste them into another
  palette at their original index positions. Because sprite pixels reference
  indices rather than colors, keeping the positions is what makes the pasted
  colors land where the art expects them. A too-short target palette is grown
  rather than dropping the tail colors, and pasting from a palette row's
  right-click menu writes that palette in place without changing the palette
  selection or the current sprite's palette assignment. With nothing
  multi-selected, the copy falls back to the single highlighted swatch.
  Available from a swatch's right-click menu, a palette row's right-click menu,
  and `Operations... > Clipboard & Files`.

### Sprite editing
- **Cookie cutter returns the pixels it removed** — after cutting, the
  clipboard holds the target pixels that were erased, along with that frame's
  palette snapshot, so they remap correctly when pasted elsewhere.
- **Split Body Parts** — detects head, arms, torso, and legs from a sprite's
  silhouette (narrowest rows for the neck and waist, the dense central column
  run for the torso core), then shows them as draggable, resizable boxes over
  the sprite so a bad guess can be corrected before it commits. Splitting
  creates one child IMG per part with the parent's anipoint rebased into each,
  optionally trimming each part to its own pixels and erasing the cut regions
  from the original. Under `Operations > Split Body Parts...`.

## [v3.16.5] — Animation authoring, GIF export, and sprite editing tools

Feature release focused on animation setup and frame-to-frame sprite editing.

### Animation and anipoints
- **Animated GIF export** — exports the current timeline with Hold timing,
  configurable FPS, transparency, looping, ping-pong, per-frame palettes, and
  optional primary-anipoint alignment.
- **Group anipoint editor** — set X, Y, or both across marked frames, using the
  first marked frame as the size reference with leading-edge, center, or
  trailing-edge compensation.
- **SEQSCR authoring improvements** — append all marked frames to a sequence in
  image-list order and move entries up/down while preserving the raw blob.
- **Safe secondary-point defaults** — all new IMG records start with the proper
  `FFFF/FFFF/FFFF` unused sentinel; duplicate helper logic now uses the canonical
  anipoint implementation.

### Sprite editing and World View
- **Sprite cookie cutter** — capture a frame's tight opaque silhouette, place it
  over another frame or IMG, and erase through that shape with undo support.
- **Floating-paste navigation guard** — arrow keys move the paste instead of
  changing frames; Shift+Arrow nudges by ten pixels.
- **World View fixes** — onion skin follows timeline order and the prior frame's
  palette; single-frame mode has independent Borders and Anipt toggles.

### Documentation
- Added built-in reference material for MK2 frame origins, primary/secondary
  anipoints, frame alignment, and embedded SEQSCR sequence/script editing.

## [v3.15.8] — World View ping-pong timing

Patch release: World View lane timing can now freeze at a specific tick, chain
ping-pong timing has an explicit reverse delay, and the standard toolbar no
longer covers the animation timeline or palette.

### World View
- **Stop at tick** — each World View lane now has a `Stop@` tick control that
  freezes that lane at the requested global preview tick without stopping other
  lanes.
- **Ping-pong reverse delay** — each slot now has a `PongDelay` value used by
  chain Ping Pong to hold the fully extended pose before the reverse pass.
- **True chain reversal** — chain Ping Pong now retracts links in complete
  reverse order instead of replaying the forward order.
- **Current Y readout** — World View status text now shows the currently rendered
  sprite Y bounds, including timed motion and mirrored/compound lanes.

### UI
- **Toolbar bottom dock clearance** — the left toolbar now reserves space for
  the standard animation timeline and palette, so it no longer draws over those
  views.

## [v3.15.1] — Numbered palette copies

Patch release: duplicating or clean-copying a palette now derives the new name
from the source instead of a generic counter.

### Palettes
- **Numbered copy names** — Duplicate Palette and Clean Copy Palette now number
  off the source name, e.g. `LKALT_P` -> `LKALT1_P` -> `LKALT2_P`, keeping the
  `_p`/`_P` palette suffix at the end and advancing the counter on re-duplicates
  (so duplicating `LKALT1_P` yields `LKALT2_P`, not `LKALT11_P`). Names with no
  suffix just get the number appended; the 9-char name limit is respected by
  truncating the stem. Blank names still fall back to `PAL<n>`.

## [v3.15.0] — Tab reorder, palette color grouping, add sequences/scripts

Feature release: document tabs can be reordered and scrolled properly, palettes
can be grouped by color family, and new animation sequences/scripts can be
created from the right panel.

### Document tabs
- **Scroll arrows work** — the active tab was being re-asserted as selected every
  frame, which continuously re-scrolled the bar back to it and fought the
  left/right scroll arrows when more tabs were open than fit. The active tab is
  now only force-selected when it actually changes, so the arrows scroll freely.
- **Drag to reorder** — document tabs can be dragged into any order, and the new
  order persists in the backing store (not just visually for one frame).

### Palettes
- **Group Like Colors** — a new palette operation (Palette menu, right-click, and
  Operations → Utilities) that cleans up unused colors and then clusters similar
  colors so each hue family sits together as its own dark-to-light ramp, instead
  of the single global brightness ramp produced by Clean Up Palette.

### Animation
- **Add Sequence / Add Script** — the Sequences and Scripts sections in the right
  panel each gained a "+ Add" button that appends a new empty record to the IMG's
  SEQSCR blob (new sequences insert at the end of the sequence block so existing
  indices stay valid; new scripts append at the end). Works even on an IMG with
  no existing animation blob. Fill in entries with Edit... in the Anim
  Scripts/Seqs editor. Undoable.

## [v3.14.2] — Readable open zoom, Script Table arrow nav, credits

Patch release: sprites open at a workable magnification, World View
script/sequence tables can be scrubbed with the keyboard, and reverse-
engineering credit is added.

### Zoom
- **Readable initial zoom** — newly opened/selected sprites now default to at
  least 300% (capped at the fit scale so the whole sprite stays visible) instead
  of dropping to 100% for tall/medium sprites. Manual and sticky zoom are
  unchanged.

### World View
- **Up/Down step the Script Table** — when a script or sequence table is loaded
  in World View, the Up/Down arrows now cycle through its entries (loading each
  target sprite into the editor, with wraparound) instead of walking the main
  image list. Falls back to normal image-list navigation when no table is
  active.

### Credits
- **Reverse-engineering credit** — the About dialog and README now credit the
  agentic AI reverse engineering by [Asure007](https://github.com/Asure/),
  including work from [midway-loadimg](https://github.com/Asure/midway-loadimg).

## [v3.14.1] — Layout polish, opacity preview, sticky zoom

Patch release with UI layout fixes, a live opacity-gradient preview, and
persistent zoom across sprite changes.

### UI
- **Toolbar fills its column** — the left toolbar background now extends down to
  the bottom palette bar (or the screen bottom in World View) instead of cutting
  off partway, sizing itself dynamically to the available height.
- **Docked World View panel stretches to the bottom** — the animation/data panel
  below the world canvas now fills the remaining height down to the screen
  bottom, so all docked sequence/script data is shown with no black gap.

### Opacity gradient
- **Live preview** — the Opacity Gradient dialog now shows a checkerboard-backed
  preview of the selected sprite with the dissolve applied, updating live as the
  direction, opacity, and seed are changed.

### Zoom
- **Sticky zoom** — a manually chosen zoom level is now carried across sprite and
  palette selection changes instead of snapping back to the half-fit default.
  Pressing Fit clears the pinned zoom and restores the default.

## [v3.14.0] — Anim Scripts/Seqs editor, opacity gradients, World View motion

Feature release adding a WIMP sequence/script (SEQSCR) browser and editor,
dithered opacity gradients backed by per-IMG alternate palettes, and per-tick
motion staging in the World View.

### Anim Scripts / Seqs
- **SEQSCR browser** — a new "Anim Scripts / Seqs" window lists the embedded
  sequence and script records parsed from the SEQSCR blob, with separate
  Sequences and Scripts panes.
- **World View integration** — records can be loaded straight into a dedicated
  embedded sequence/script lane for playback alongside marked sprites.
- **ASM export** — each record can be copied or saved as generated animation
  ASM, and the view/edit data panel exposes the raw record fields.
- **Damage table preservation** — the raw damage-table refs (`damtbl`) are now
  kept verbatim across load and save so existing assets round-trip unchanged.

### Opacity gradients
- **Opacity Gradient dialog** — apply a dithered opacity gradient to the
  selected sprite or all marked sprites, backed by a 16-byte alternate palette
  table (`opaltbl`) stored per IMG and preserved through copy/paste and save.

### World View
- **Per-tick motion** — marked-sequence entries now support per-tick visual
  motion deltas (`motion_dx` / `motion_dy`) so lanes can drift across the scene
  during playback.
- **Boundary overlay toggle** — the arcade world boundary overlay can be shown
  or hidden while staging animation.
- **Y-anchor warnings** — lanes and dual instances flag a bad Y anchor so
  mis-positioned sprites are easier to spot.
- **Half-fit zoom** — a half-fit zoom helper makes it easier to inspect the
  zoomed world canvas.

## [v3.13.1] — Subframe naming patch

Patch release fixing the parent-aware subframe naming added in v3.13.0.

### Fixes
- **Manual Auto-Chop parent preservation** — breaking a numbered parent such as
  `EDHANG1` no longer renames the parent to `EDHANG` before creating pieces.
  New children now correctly become `EDHANG1A`, `EDHANG1B`, and so on.
- **Context-aware numeric grouping** — bare numbered names such as `EDHANG1`
  only group under `EDHANG` when a real `EDHANG` parent sprite exists. Lettered
  children such as `EDHANG1A` still group directly under `EDHANG1`.

## [v3.13.0] — World View timing controls and parent-aware subframes

Feature release focused on more flexible World View animation staging and
cleaner generated subframe names.

### World View
- **Show and hide ticks** — sequence entries now support both `Show@` and
  `Hide@` timing so held subframes can appear for a bounded tick window.
- **Held subframe preview** — timed subframes can remain on screen alongside
  the active frame instead of being replaced immediately by the next frame.
- **Expanded rows** — marked World View playback now supports up to ten rows,
  with split-row controls for breaking one IMG's marked sequence into multiple
  editable lanes.
- **Testing visibility controls** — per-row eye buttons hide individual rows,
  and a border toggle hides all sprite bounds while checking staged animation.
- **Zoomed world canvas** — the logical arcade world stays 400x254 while the
  rendered view zooms in for easier inspection.

### Subframes
- **Parent-aware Auto-Chop names** — breaking `EDHANG1` now creates
  `EDHANG1A`, `EDHANG1B`, while breaking `EDHANG` creates `EDHANG1`,
  `EDHANG2`.
- **Image-list grouping** — the subframe parser now recognizes both numbered
  and lettered generated children so they fold under the expected parent.

## [v3.12.0] — World View draw staging and build-cache cleanup

Feature release focused on more faithful World View staging for multi-sprite
animations, plus fixes for exported TGA atlases and repeatable Windows builds.

### World View
- **Per-entry draw priority** — marked-sequence frames can now carry a `Z`
  value so overlapping lanes can be staged in the intended front-to-back order.
- **Dual sprite instances** — a sequence entry can draw a second copy of the
  same sprite with its own local anipoint delta and draw priority, useful for
  effects or mirrored animation beats.
- **ASM export annotations** — generated animation source now includes `z=`,
  `dual`, and `*_dual_anipts` data so staged preview intent survives export.
- **Cleaner sequence editing** — duplicate, move, delete, and marked-set sync
  now keep every per-entry timing, offset, visibility, mirror, Z, and dual field
  aligned together.

### Fixes
- **TGA atlas packing** — fixed a row free-space accounting bug that could let
  later glyphs overlap earlier glyphs in exported sheets such as `MK2FONT1.TGA`.
- **ASM animation import** — ignores generated `*_dual_anipts` tables when
  scanning ASM files for real animation labels.
- **Windows build reuse** — `build.bat` and `build.ps1` now detect stale CMake
  caches from another source tree or architecture and clean the build directory
  before configuring.
- **App artwork** — refreshed `imgtool.png` with the latest application graphic.

## [v3.11.0] — Overlay split complete and editor workflow polish

Feature release completing the long-running ImGui overlay split while keeping
the editor behavior intact, plus several small workflow improvements that landed
during the refactor.

### Overlay Refactor
- **Overlay split complete** — the former monolithic `imgui_overlay.cpp` now
  delegates to focused UI modules for canvas, modals, palette editing, main
  layout, tools, timeline, undo, auto-chop, and shared state.
- **Reusable logic modules** — palette math, color operations, image edge/stroke
  analysis, sprite resizing, anipoint helpers, IMG utilities, and World View
  texture rendering now live in isolated units with targeted tests where
  practical.
- **Cleaner release tree** — removed the completed refactor plan and a
  workspace-specific scratch signature script from the shipped source.

### Editor Workflow
- **Global shortcuts and layout components** — keyboard shortcuts and shared UI
  layout helpers are consolidated so extracted modules behave consistently.
- **Toolbars and operations menus** — the side panel button grids were replaced
  with denser toolbars and grouped operation dropdowns.
- **Palette cleanup tools** — added a move-selected-colors-to-end operation with
  pixel remapping, and kept the bottom palette bar docked predictably.

### Build And Contributors
- **Automatic app versioning** — release builds now derive the in-app version
  from the pushed `v*` tag at CMake configure time, with dev builds falling back
  to a `-dev+sha` version string.
- **Contributor setup** — added `CONTRIBUTING.md`, `build.sh`, and
  `CMakePresets.json` to make fresh-clone builds and CI reproduction easier
  across platforms.
- **Expanded verification** — the extracted pure modules are covered by the
  standard CTest suite, with the MK2 roundtrip test still available when the
  external ASM fixture is configured.

## [v3.10.0] — World View ASM autoload and sequence polish

Feature release focused on making multi-IMG character animations easier to load,
inspect, and refine in World View.

### World View
- **Preserve edited sequences** — marked-tab sequence edits now survive marked
  set changes: ordering, duplicate entries, local anipoints, per-frame delay,
  visibility, and mirror flags are kept for still-marked frames.
- **Less obstructive sequence panel** — the sequence editor stretches across the
  bottom of the canvas so it covers fewer sprites while staging animations.
- **On-canvas source tags** — each rendered lane is labeled with its source
  animation/file and current frame to make multi-tab layouts easier to read.

### ASM Animation Viewer
- **Automatic multi-IMG loading** — selecting an ASM animation can locate and
  open every referenced sprite IMG, then resolve the animation globally across
  all open tabs.
- **Cross-document sprite pieces** — composited ASM frames now track the owning
  document for each sprite piece, fixing animations split across character IMG
  libraries.
- **Better save defaults** — saving generated ASM now proposes the generated
  animation label instead of the currently active IMG name.

### Palette Tools
- **Quicker alignment workflow** — Color Tools now sit above the anipoint panel
  so palette edits and sprite alignment controls stay close together.

## [v3.9.0] — World View ASM animations, fatalities, and undo hardening

Feature release adding a MK2 ASM animation pipeline to World View, per-sprite
non-destructive layers, smarter palette merging, and a sweep of undo fixes.

### Palette Merge
- **Merge options dialog** — merging marked palettes now always opens a choice
  + live-preview dialog with two toggles: **Grow target** (append the source
  colors sprites actually use into the target's free slots for a lossless
  merge) and **Perceptual match** (luma-weighted nearest-color selection).
- **Swatch mapping preview** — shows each source color over its mapped target
  color; green border marks colors added losslessly, amber/red mark drift, and
  a readout reports `target grows N -> M (+K)` plus any overflow.

### Paste And Layers
- **Flip floating paste** — `H` / `V` mirror a floating paste in place before
  committing it with Enter.
- **Per-sprite overlay layer** — drop a floating paste onto a sprite as a
  non-destructive layer (`L` or Edit -> Drop Paste to Layer). A Sprite Layer
  panel offers visibility, offset, flip, flatten, and delete. Layers travel
  with their sprite through undo and flatten onto the pixels only on save.

### World View
- **Reorder and duplicate frames** — the marked-tab sequence editor gained
  `<` / `>` to move a frame earlier/later, alongside duplicate / delete / reset.
- **Top-anchored origin** — the World View Y origin now defaults to 20.
- **Per-frame flip** — lanes honor a per-frame mirror (used by ASM `ani_flip`).
- **Dummy faces the player** — the decap dummy body now defaults to facing the
  attacker; anipoints stay pinned to the shared origin regardless of facing.

### ASM Animation Viewer
- **Load character ASM** — parse a MK2 per-character ASM (e.g. `MKRD.ASM`),
  list its animations in a dropdown, and play any of them against the loaded
  IMG with play/fps/scrub controls and a missing-symbol / opcode report.
- **Opcode-aware parsing** — frame timing (repeated frames), `ani_adjustx`/`xy`
  local anipoints, `ani_flip`, and `ani_jump` loops are interpreted.
- **Auto-find the IMG** — on load, the matching IMG is located by scanning the
  ASM folder, sibling `data/` dirs, open tabs, and `IMGDIR`; if none match it
  prompts you to locate the IMG, then re-resolves.
- **Play in World View lane** — render the parsed animation as its own World
  View lane with correct anipoint placement, ticks, local anipoints, and loop.
- **Save / Load ASM** — save the generated animation tables to a `.ASM` file
  and reload them later (including round-tripped local-anipoint tables).

### Fatalities
- **Opponent lane** — load a second character ASM (one-click Johnny Cage, or
  any opponent ASM) plus its IMG into a dedicated lane that defaults to facing
  the player, for staging fatality interactions.

### Undo
- **Cut / paste are undoable** — cut, paste-commit, and paste-as-new-image now
  record proper pixel/document history.
- **Geometry and structural ops fixed** — crop/trim, duplicate, add, delete
  unused, reorder, rename, toggle point table, clear extra data, and set
  palette of marked now take full-document snapshots (crop previously used a
  metadata-only snapshot that could read out of bounds on undo).

## [v3.8.0] — Canvas zoom, paste compositing, and UI cleanup

Feature release for a cleaner editing workspace, more predictable canvas
navigation, and Photoshop-style paste placement controls.

### Canvas And Zoom
- **Centered zoom behavior** — zoom in/out now stays centered on the canvas
  instead of drifting toward the cursor or stale pan offsets.
- **Higher zoom ceiling** — sprite zoom now reaches 128x for close pixel
  cleanup work.
- **Mouse-wheel canvas scroll** — the wheel pans vertically by default, while
  `Ctrl+wheel` still zooms from the canvas center.
- **Pan clamping** — when a sprite fits on an axis, that axis recenters instead
  of leaving the art stranded off-center.

### Paste And Transform
- **Visible floating paste preview** — pasted sprites start centered on the
  target and render at full opacity by default so resize/placement work is
  visible before commit.
- **Moveable transform box** — while Free Transform is active, dragging inside
  the box moves the floating paste; handles still scale and the top dot rotates.
- **Paste blend controls** — floating paste now exposes opacity plus Normal,
  Dissolve, Darken, Multiply, Color Burn, Linear Burn, Lighten, Screen, Color
  Dodge, Overlay, Soft Light, Hard Light, Difference, and Exclusion modes.
- **Overwrite-by-default commit** — Normal 100% paste overwrites opaque source
  pixels exactly; palette index 0 remains transparent.

### UI Layout
- **Two-column toolbar** — grey editing tools now sit in the left toolbar column,
  while blue IMGTOOL actions (mark, points, hitbox, resize, zoom, undo/redo)
  sit in the right column.
- **Right-panel ordering** — `Color` is now always open directly beneath
  `Palettes`, followed by `Anipts` for quick alignment edits.
- **External rotate buttons** — sprite rotate buttons sit beside the sprite in
  their original side-by-side layout instead of covering the artwork.

## [v3.7.0] — Sprite sheet imports and actor likeness tools

Feature release for faster sprite cleanup, palette-safe paste/import workflows,
and deterministic actor-likeness transfer between marked sprites.

### Sprite Editing
- **Marked likeness transfer** — one marked source sprite can now apply its
  actor/costume likeness to the selected target while preserving the target
  pose, silhouette, and digitized shading.
- **Material-aware likeness pass** — transfer now separates head/skin, blue
  robe, lower cloth, dark edge, and gold accent regions, adds source
  microtexture, and cleans bad green/olive palette outliers.
- **Softer hard-stroke removal** — edge cleanup now works from the true outer
  edge, removes the harsh 1px stroke, and repaints saved edge pixels from the
  nearest strong inward color instead of chewing into the sprite.
- **Blank-area marquee start** — dragging from transparent canvas space now
  starts a selection cutout workflow automatically.

### Canvas And Paste
- **Sprite zoom controls** — toolbar buttons, View menu items, mouse wheel,
  `Ctrl+=`, `Ctrl+-`, and `Ctrl+0` now zoom in/out or fit the current sprite.
- **Palette-matched paste** — copied pixels carry source palette metadata and
  remap to the destination palette when pasted into a different palette.
- **External rotate handle** — pasted/free-transform sprites now rotate from an
  outside-corner handle so the controls do not cover the art.

### Import And File Dialog
- **Sprite sheet import** — PNG/JPG/TGA sheets can be split into individual
  palette-matched frames using island detection, padding, and tight crop
  options.
- **Sprite sheet debug CLI** — `--debug-spritesheet` writes detection artifacts
  and reports accepted frames for tuning sheet import behavior.
- **GIF/PNG preview thumbnails** — the file picker now builds previews from
  highlighted image files directly, not only after clicking the filename.
- **Multi-file image imports** — PNG, matched-PNG, GIF, and sprite sheet import
  dialogs support selecting multiple files in one pass.

### Tests And Repo Hygiene
- **Sprite sheet regression test** — CMake now builds and registers a
  `sprite_sheet_import_test` target for the detector.
- **Ignored scratch outputs** — local prototype output and test scratch folders
  are ignored by git.

## [v3.6.1] — Palette readout cleanup

Patch release for making the new palette usage information easier to read.

### Palette Editing
- **Readable 8bpp palette bar** — the bottom palette area now reserves a clear
  header band for selected color, RGB, usage counts, free colors, low-use
  counts, and marker legend instead of drawing text over the swatches.
- **Full 256-color grid fit** — the palette grid now adapts column count and
  swatch size to show full 8bpp palettes cleanly across window sizes.

## [v3.6.0] — Fatality workspace and palette forensics

Workflow release for editing MK2 fatality source, finding spare palette
slots, and building multi-actor World View timing drafts.

### MK2 Fatality Lab
- **Source-backed fatality browser** — new helpers parse MK2 source files for
  fatality command blocks, input combo tables, and animation labels while
  preserving the original assembly line buffers for save.
- **Fatality staging workspace** — staged fatality assets can open attacker and
  victim IMG sets into marked World View lanes for side-by-side timing work.

### World View
- **Marked-lane sequence editor** — each marked IMG tab lane can now duplicate
  or remove sequence entries, edit per-entry delay ticks, and scrub directly
  to an entry by clicking its thumbnail.
- **Local anipoint deltas** — sequence entries carry `dAX` / `dAY` offsets, so
  repeated uses of the same sprite can have different preview anipoints
  without editing the sprite record itself.
- **Visibility gates** — `Show@` hides an entry until the selected global tick,
  useful for actor/object reveals partway through a synced fatality preview.
- **Richer ASM draft export** — World View ASM export now includes hidden-frame
  placeholders and aligned `*_local_anipts` tables for local anipoint data.

### Palette Editing
- **Palette usage indicators** — the active palette strip marks unused nonzero
  colors and low-use colors across sprites that reference the palette, with
  tooltips showing counts and nearest used-color candidates.
- **Similar-region remap** — selected pixels from tools like Magic Wand can be
  remapped across matching same-palette sprites, helping separate shared
  regions such as hair and pants without hand-editing every frame.
- **Palette cleanup on sprite delete** — deleting sprites now removes palettes
  that are no longer referenced by any remaining sprite and remaps later
  palette indices safely.
- **Palette merge preview** — marked-palette merges can be inspected before
  applying, including source-to-target color mapping and drift.

### Sprite Editing
- **Hard-stroke remover** — marked sprites can automatically remove thin
  1-2px high-contrast outline/matte strokes around transparent edges.
- **Copy/Cut to new sprite** — selected regions can be copied or cut directly
  into newly created sprites.
- **Safer secondary anipoints** — imports and resize/mirror helpers now use the
  proper unused secondary-anipoint sentinel instead of treating zero as
  missing data.
- **Subframe name shortening** — auto-chopped child names preserve suffix room
  more reliably when parent names are near the IMG name-length limit.

## [v3.5.0] — Palette safety and subframe polish

Follow-up polish for sprite hierarchy display and safer palette operations.

### Image List
- **Parent row alignment** — real parent sprites stay aligned with normal image
  rows and keep the image icon while subframes indent underneath.
- **Subframe hierarchy icon** — child pieces use a compact hierarchy glyph
  instead of the normal image icon.

### Palette Editing
- **Palette undo coverage** — palette add, delete, duplicate, paste, import,
  rename, assignment, merge, cleanup, downscale, HSL/RGB edits, and #0 remap
  tools now push real undo actions that restore palette records and affected
  sprite pixels together.
- **Palette delete remap fix** — deleting a palette now preserves image palette
  assignments by remapping affected images to the adjacent surviving palette
  and decrementing later indices.
- **Palette merge quality check** — merging marked palettes now previews color
  drift, transparency drift, invalid source indices, and LOAD2 PPP risk before
  applying the merge.
- **Safer palette merge remap** — nonzero sprite pixels prefer nonzero target
  palette slots to avoid accidental transparent holes.

### Sprite Editing
- **File dialog keyboard focus** — Up/Down and timeline arrow shortcuts no
  longer leak through while a modal file dialog or popup is open.
- **Rotate icon orientation** — canvas rotate glyphs now visually match the
  clockwise/counterclockwise actions more closely.
- **Horizontal subframe defaults** — Break into Subframes defaults to a taller
  slice height, matching the horizontal split pattern seen in MK2 character
  data.
- **Subframe split preview** — Break into Subframes now overlays the generated
  piece bounds on the canvas and reports a live LOAD2 ZCOM bit estimate while
  tuning grid size and trimming.

## [v3.4.0] — Sprite import, subframes, and palette inheritance

Workflow release for batch-importing art, organizing chopped sprites, and
moving palette work between related libraries.

### Sprite Editing
- **Multi-file PNG/GIF import** — the custom file dialog now supports
  Ctrl-click and Shift-click selection for PNG/GIF import modes.
- **Canvas rotate controls** — opened sprites show small clockwise and
  counterclockwise rotate buttons in the top-right of the canvas.
- **Bulk rename** — marked sprites can be renamed sequentially, with optional
  tailing of existing names.
- **Break into Subframes** — marked sprites, or the selected sprite when none
  are marked, can be split into Midway-style A/B/C pieces with anipoints
  recalculated for in-game alignment.

### Image List
- **Visual subframe folders** — sprites named like `BASE1A`, `BASE1B`, and
  `BASE1C` are grouped as collapsible visual folders without writing hierarchy
  data back into the IMG file.
- **Image sorting** — the image panel can sort by original order, name, or
  sprite size.

### Palette Editing
- **Palette inheritance** — selected palettes can inherit colors from a marked
  source palette, remapping sprites to the nearest matching source colors.
- **Duplicate palette merge** — byte-identical duplicate palettes can be merged
  into the first matching palette while remapping sprites safely.
- **Single palette mark button** — palette controls now include a selected
  palette mark toggle alongside mark-all controls.

## [v3.3.0] — Palette bpp downscale and transform tools

Focused art-editing release for reducing palette budgets and tightening
sprite/timeline alignment workflows.

### Palette Editing
- **Palette downscale preview** — selected palettes can be downscaled to
  8, 7, 6, 5, or 4 bpp with a side-by-side sprite preview before applying.
- **Smart reduction logic** — used colors are preserved exactly when they fit
  the target bpp; otherwise the reducer uses weighted color merging and remaps
  every sprite that references the palette.

### Sprite Editing
- **Lossless sprite transforms** — selected sprites can rotate or flip while
  keeping pixels, anipoints, and hitbox state aligned.

### Animation Timeline
- **Composite pair controls** — paired timeline sprites can stay grouped during
  playback/scrubbing, with lock toggles and drag-to-adjust anipoint alignment
  in the composite preview.

## [v3.2.0] — Timeline composite preview

Focused animation-alignment workflow release.

### Animation Timeline
- **Anipoint composite preview** — Ctrl-click two frames in the animation
  timeline to show a read-only canvas preview of both sprites combined by their
  primary animation points, with secondary animation points and per-frame
  outlines drawn for registration checks.

### Sprite Alignment
- **Reverse-facing anipoint mirror** — marked sprites can mirror their primary
  and active secondary X anipoints as `width - x`, matching World View's
  view-only reverse-facing comparison.

### Palette Editing
- **Transparent color relocation** — palette index 0's RGB value can be copied
  into a safe opaque slot, with optional remap tools for the current selection
  or sprite so formerly transparent pixels can be painted visibly.

## [v3.1.0] — Sprite resize, IMG tabs, and World View pairing

Workflow release for editing multiple IMG libraries side by side and comparing
animation alignment across tabs.

### Sprite Editing
- **Resize Sprite** — added a resize dialog with width/height and percent
  scaling controls, optional aspect-ratio locking, nearest-neighbor palette-ID
  preservation, quality remap mode, and a quality-plus-smallest-bytes mode that
  trims transparent bounds.
- **Anim-point batch push** — selected Anim Point sliders now nudge by one with
  Left/Right, and a new push button copies the selected sprite's anim points to
  regex-matched same-name sprites across open tabs, e.g. `^..HEADHOLE1$`.
- **Geometry-safe undo** — sprite-resize operations now capture full image
  state, including dimensions, anipoints, palette metadata, and pixel data, so
  geometry edits undo cleanly without disturbing normal paint undo.
- **LOAD2 baseline dimensions** — load-time baseline width/height are tracked
  with the pristine pixel snapshot so resized sprites are reported accurately by
  the packing verifier and drift overlay.

### Multi-IMG Tabs
- **IMG tabs** — File > Open now opens each IMG/LOD into its own tab, reusing
  the initial empty tab when possible and activating an already-open tab when
  the same path is opened again.
- **Per-tab dirty state** — unsaved-change prompts now operate per open
  document, including tab close and quit flows, while the editor keeps the
  legacy `g_doc` access pattern through a new document container.
- **Native ImGui tab strip** — the document strip uses ImGui tabs and trailing
  tab buttons while avoiding the earlier selection tug-of-war by switching
  documents only on tab activation.

### World View
- **Marked-tab playback** — World View can play marked animations from the
  active tab together with the first other marked tab, anchored to the same
  world origin for anipoint comparison.
- **View-only mirroring** — mirror toggles for the active and paired World View
  sprites let opposite-facing animations be compared without modifying image
  pixels or anipoints.
- **World View tab controls** — Onion, Marked, Mirror Active, and Mirror Paired
  controls are exposed as separate trailing tab buttons instead of living in the
  View dropdown.

## [v3.0.0] — MK3 IMG support

Major compatibility release for Mortal Kombat 3 / Ultimate MK3-era IMG
workflows.

### IMG Compatibility
- **Long project paths** — expanded the document path buffer and removed
  hardcoded 64-byte path copies, so MK3 assets in normal source-tree paths
  open directly from the GUI and CLI.
- **MK3 WIMP 0x0634 libraries** — verified `MKPOWER.IMG` loads, exports,
  and runs through the LOAD2 packing verifier from its original project path.
- **LOD `PPP> 0` handling** — `.LOD` parsing now distinguishes an explicit
  `PPP> 0` from a missing `PPP>` directive, preserving MK3 manifests that
  deliberately use palette/native bpp.

## [v2.10.0] — Sprite workflow tools and CLI packaging

Focused workflow release for faster sprite extraction and scriptable build
pipelines. No IMG file-format changes.

### Sprite Editing
- **Cut to New Sprite** — `Ctrl+Shift+X` cuts the current marquee/lasso/wand
  selection into a freshly-created sprite, preserving palette and anipoint
  alignment from the source frame.
- **Paste as New Sprite** — `Ctrl+Shift+V` creates a new sprite directly from
  the pixel clipboard, using tight transparent bounds and unique derived
  names.
- **Duplicate and Trim buttons** — the Images panel now exposes one-click
  `Dup`, `Trim`, and `Paste+` controls. Trim removes dead transparent border
  space from the selected sprite without dirtying already-tight frames.

### Command Line Interface
- **Console CLI target** — builds now produce `imgtool-cli`, a console
  subsystem binary for scripts and CI. Release artifacts include it on
  Windows, Linux, and macOS.
- **Stricter headless commands** — CLI commands now reject unknown options,
  report missing inputs and failed IMG loads, and print a full command
  reference from `--help`.
- **LOD automation polish** — `--build-lod` accepts `--override-dir=DIR`,
  validates that all referenced IMGs loaded, and handles relative output paths
  safely after internal directory changes.

## [v2.9.0] — Headless CLI, GIF import, palette workflows

Automation and art-cleanup release. Adds a headless command-line mode
for build pipelines, GIF and palette import/export, new paint workflows
for variant-heavy sprites, and a round of release-hardening fixes. No
IMG file-format changes.

### Command Line Interface
- **Open on launch** — `imgtool [file]` opens an `.img`, `.png`,
  `.tga`, `.lbm`, or `.gif` file immediately on application start.
- **Headless data exports** — `--export-tbl <input.img> <output.tbl>`
  writes MK2/MK3 assembly tables with `--mk3`, `--include-pal`,
  `--padding`, `--align-16`, `--dual-bank`, `--bank=N`, and
  `--base=HEX`; `--export-irw <input.img> <output.irw>` writes raw
  ROM layouts with `--bpp=N`, `--no-align`, and `--base=HEX`;
  `--export-anilst <input.img> <output.asm>` writes assembly animation
  lists.
- **Headless format conversions** — `--export-png <input.img>
  <output_dir>` unpacks sprites to individual PNG files, while
  `--build-tga <input.img> <output.tga>` packs all marked sprites into a
  single TGA spritesheet.
- **LOD and validation tools** — `--build-lod <manifest.lod>
  <output.img>` builds an IMG directly from a `.lod` manifest, and
  `--verify-load2 <input.img>` runs the LOAD2 packing verifier with
  non-zero exit status on breaking alignment issues.
- **Usage dialog** — `--help`, `-h`, and `/?` show a native usage
  message for GUI launches.

### Import / Export
- **GIF import** — File > Import > GIF File loads single-frame or
  multi-frame GIFs into IMG sprites, quantizes all imported frames into
  a shared 15-bit palette, and exposes blend mode plus opacity controls
  for compositing animation frames before import.
- **Palette import/export** — raw Midway `.PAL` data and Adobe `.ACT`
  RGB palettes can be imported as new palettes or exported from the
  active palette. Palette dialogs remember their own directory and
  default export names from the selected palette.
- **GIF-aware path dispatch** — drag/drop and launch-by-path now route
  `.gif` files through the same import path as the file dialog.
- **File dialog preview** — the existing PNG/TGA thumbnail pane now
  previews GIFs via `stb_image`.

### Paint And Selection Tools
- **Paint Bucket tool** — new `G` toolbar mode fills the clicked color
  with the current swatch. Tolerance is palette-index based, with a
  contiguous flood mode or global replace mode.
- **Variant Paint tool** — new `V` toolbar mode paints only the current
  image's palette appearance while preserving the original color in all
  other palettes through automatically-created shadow slots. The same
  logic can be applied to the current selection from the Operations menu
  or palette properties.
- **Overlay frame extraction** — selected opaque pixels can be split or
  copied into a new transparent overlay frame that preserves the source
  sprite's geometry, anipoints, palette, flags, and alternate-palette
  metadata. Split mode clears the source pixels and participates in
  pixel undo.

### Palette Workflow
- **Clean Up Palette** replaces the older unused-color delete action
  with a cleanup pass that preserves transparent index 0, removes unused
  entries, sorts active colors into visible brightness ramps, and remaps
  every sprite using that palette.
- **Clean Copy Palette** creates a new sorted/trimmed palette without
  remapping existing sprites, useful when comparing a cleaned ramp
  against the original palette.
- **Palette adjustment commits** — RGB/HSL edits now commit their
  preview baseline when changing image, palette, or swatch selection, so
  subsequent resets and multi-select changes do not resurrect stale
  preview data. Applying a palette also clears stale SDL swatches past
  `pal->numc`.

### Hardening
- **CodeQL fixes** — corrected a real `src_filename` overflow in chopped
  image creation, widened several allocation/mask-size multiplications
  to `size_t`, and escaped a literal percent sign in an ImGui tooltip
  format string.
- **GIF decoder fix** — patched the bundled `stb_image` GIF loader so
  the two-frame-back pointer references the correct output layer during
  animated GIF decoding.
- **Repository hygiene** — stopped tracking the local `last.md` scratch
  file; the existing `*.md` ignore now covers it.

## [v2.8.0] — Editor polish, parser correctness, layout-independent shortcuts

Twenty-commit round focused on tightening everything Phase 7 introduced.
No IMG file-format changes. Two real parser bugs in the MK2 editor were
caught and fixed by a new round-trip test; the editor gains undo / redo
/ reload / search / auto-character-pick / unsaved-quit guard; pencil
becomes a fully discoverable tool; menus and Help are aligned with what
the code actually does.

### MK2 strike-table editor
- **Undo / Redo** — per-record snapshot stack inside `mk2::Document`
  (bounded to 200 entries, drag-coalescing). Ctrl+Z and Ctrl+Shift+Z
  fire only while the panel has focus so they don't hijack the main
  canvas's pixel-undo path. Toolbar buttons mirror the keyboard.
- **Reload button** — re-reads MKSTK.ASM from the previously-resolved
  source path (not the input box, so a stray edit there can't redirect
  the reload). Drops in-memory edits and undo/redo history.
- **Browse… button + last-dir memory** — native Win32 file picker
  filtered to `.ASM`. Picked directory persists per-category alongside
  the existing IMG/PNG/TGA/LBM dirs. Defaults to empty path (was
  hardcoded to one developer's machine).
- **Move filter** — case-insensitive substring search box in the
  middle pane so finding a move across ~200 records doesn't require
  scrolling.
- **Auto-pick character from IMG filename** — prefix table maps the
  loaded sprite's filename (CAGE / HATHED / KANG / NINJAS / RAID /
  TSUNG / JAXPRO / NUJAX / MKJXARMS / KAT / BIGGORO / GOROSIZE) to the
  two-letter character code MKSTK.ASM uses. Silent no-op when nothing
  matches.
- **Unsaved-changes guard on quit** — separate modal from the IMG dirty
  flow since MK2 writes a different file (MKSTK.ASM).
- **Stale-status auto-clear** — "Loaded N moves" / "Saved" disappears
  on the first edit after a clean baseline; error messages stay sticky
  until the next action resolves them.

### MK2 parser correctness
- **Case-insensitive `.word` / `.long` regex** — MKSTK.ASM has 11
  uppercase `.WORD` lines (e.g. on `stk_jxuppercut`). Previous parser
  silently dropped them, shifted subsequent fields by a slot, and lost
  the trailing `sound` field on 43 records.
- **Stacked `stk_*` labels** — multiple consecutive `stk_*` labels with
  no `.word`/`.long` between them (e.g. `stk_sazap1` immediately
  followed by `stk_sazap4`) are aliases for the same strike record.
  Previously the first label allocated empty records and consumed the
  data for the second. Now tracks `pending_aliases`: every
  freshly-labeled record that hasn't seen field data yet receives the
  same field references when data finally arrives.
- **`mk2_roundtrip_test`** — new CMake test target (guarded by
  `-DMK2_MKSTK_PATH=…`). Loads, verifies every record has all 8 fields
  tracked, then does load → save A → load A → save B and asserts
  A == B byte for byte. Both parser bugs above were caught by this
  test on first run.

### Pencil tool
- **Explicit `ActiveTool::Pencil` mode** with toolbar button and the
  Photoshop `P` shortcut. Toolbar Open/Save icons removed (File menu
  and Ctrl+O / Ctrl+S unchanged).
- **`[` / `]` shrink / grow brush radius** (1..16). Photoshop convention.
- **Cursor preview tracks the selected palette color** — brush ring for
  radius > 1, offset crosshair for radius = 1. Both have a black halo so
  they stay legible against same-colored pixels.
- **Per-stroke pixel undo / redo** — replaces the previous single-slot
  swap (which silently overwrote itself on each new stroke and had no
  redo). Up to 32 strokes back, with branch-on-edit. Covers Pencil,
  Clone Stamp, Smart Eraser, Smart Remap, Shift+click Flood Fill.
- **Undo/Redo unification** — toolbar buttons, Edit menu items, and the
  Ctrl+Z / Ctrl+Y shortcuts all route through `DoUndo` / `DoRedo` so the
  controls correctly light up after a paint stroke.

### Menu and shortcut audit
- **Wired previously-vestigial palette shortcuts as real:** `[` Set
  Palette for Marked, `]` Set for Image, `*` Merge Marked (typed
  character — works on any keyboard layout), `Shift+R` Rename Palette,
  `Del` Delete Palette. `[` and `]` yield to Pencil brush size when
  Pencil is active.
- **Wired four previously-vestigial Image shortcuts as real:** `Ctrl+R`
  Rename Image, `Ctrl+P` Toggle Point Table, `Alt+PgUp` / `Alt+PgDn`
  Move Up / Down in list.
- **Stripped misleading hints** that advertised keys without bindings:
  `a` Append, `i` Set ID from 2nd List, `Alt+C` Clear Extra Data,
  `Left`/`Right` Jump Prev/Next Marked, `Tab` World View.
- **Help modal rewritten** to match what's actually bound. The old
  text mixed DOS-tool keys (`d/D/F11/F12` zoom, `t` true-palette,
  `Ctrl+Y`/`Ctrl+Z` for anim-point clears that now collide with
  Redo/Undo, Eyedropper as E when it's actually I, `'`/`/` palette
  nav, `i` ID-from-2nd-list, `Alt+U/D/L/R` and `Ctrl+U/D/L/R` anipoint
  moves) with one inverted (M/m which sets/clears all marks — the
  hint was backward). New Help also has a dedicated MK2 Strike-Table
  Editor section.
- **README keys table synced** to the same ground truth as Help.

### Canvas polish
- **World View hides overlays** — anipoint crosshair, IMG hitbox, and
  MK2 hitbox box all suppress while World View is on. World View
  re-anchors the sprite relative to its anipoint, so on-sprite markers
  would otherwise float detached from the playfield rectangle.
- **About modal** uses a 2-column table for the key/value rows
  (Version / Built / Commit / Dear ImGui / SDL2). ImGui's proportional
  font made the previous space-padded labels drift.

### New IMG creation
- **New-image dialog** prompts for width and height before allocating
  (1..1024 clamped). Replaces the hardcoded 32x32. Defaults persist
  between opens. Enter commits, Esc cancels.
- **File → New** bootstrap still uses 32x32 to keep the path identical.

### Source hygiene
- **Trademarked character names stripped from source** — the MK2
  auto-pick map now uses only the two-letter codes MKSTK.ASM itself
  defines (no fighter names in code or comments). README, About
  dialog, and CHANGELOG keep factual references under nominative fair
  use.

## [v2.7.0] — Phase 7: MK2 hitbox editor, Pencil tool, New IMG bootstrap

Phase 7 wrap-up. Adds the long-planned MK2 strike-table editor (parses
and writes `MKSTK.ASM` directly, with a magenta on-canvas hitbox
overlay), a proper Pencil tool with the Adobe `P` shortcut, and makes
File → New leave you in a usable starting state. No IMG file-format
changes.

### MK2 strike-table editor
- New **Tools → MK2 Hitboxes (MKSTK.ASM)...** panel. Parses
  `mk2-main/src/MKSTK.ASM` directly as the source of truth — no
  `stk.bin` or `MKSTK.LST` dependency. Three-pane layout:
  character → move → fields.
- Edit fields by number (x/y/w/h, score) or by raw token
  (`strike_routine`, `sound` — preserves symbolic identifiers like
  `sf_squeeze`). `damage` is split into hit and block bytes.
- **Save** rewrites `MKSTK.ASM` in place, preserving comments,
  indentation, and symbolic literals. Hex literals stay hex,
  decimal stays decimal.
- **Magenta hitbox overlay** on the main canvas shows the selected
  move's collision box at `(x_offset, y_offset, x_size, y_size)`
  with drag-to-resize corner handles that write straight back to the
  `.ASM` line buffer. Overlay stays visible while the MK2 panel has
  focus. The IMG-embedded hitbox overlay auto-hides when an MK2
  move is selected so the two systems don't pile on top of each
  other.
- Implemented in [platform/mk2_hitbox.h](platform/mk2_hitbox.h) and
  [platform/mk2_hitbox.cpp](platform/mk2_hitbox.cpp).

### Pencil tool
- New explicit **Pencil tool** on the toolbar with the `P` shortcut
  (Photoshop convention). Single click paints one pixel at the
  current palette index; drag paints a continuous line. Shift+click
  flood-fills (carried over from the previous no-tool paint mode).
- Same paint code path as the v2.6 "no active tool" behavior, just
  promoted to a real `ActiveTool::Pencil` mode so it's discoverable
  and the toolbar highlight tells you which tool is active.

### New IMG from scratch
- **File → New** now bootstraps a usable starting document — one
  default 256-color palette and one 32×32 blank image — instead of
  leaving you with an empty void where palette and image lists were
  both empty.
- Undo and dirty state are reset after the bootstrap so the new doc
  starts pristine.

### Palette additions polish
- **Add Palette** / **Duplicate Palette** are now reachable from the
  Palette menu and the right-click context menu (previously only on
  the button row).
- Both now push undo, auto-select the new palette, and commit it onto
  the active image. New palettes get a unique `PAL%d` default name
  instead of an empty name.

### Toolbar cleanup
- Removed the **Open** and **Save** icons from the left toolbar.
  Loading and saving stays accessible via the File menu and the
  `Ctrl+O` / `Ctrl+S` shortcuts. Lets the toolbar focus on actual
  paint/selection tools.

## [v2.6.0] — Drag-drop, palette highlight, Eyedropper, multi-doc plumbing

Workflow polish round plus the structural refactor that unlocks multiple
open IMGs in a future release. No file format changes; everything saves
and loads bit-identical to v2.5.0.

### Drag and drop file open
- **Drop `.img` onto the window** to open it (routes through the same
  unsaved-changes guard as File → Open). Drop `.png`, `.tga`, or `.lbm`
  to import into the active document.
- **Empty workspace bootstrap** — dropping a PNG/TGA/LBM with no IMG
  open silently creates a fresh IMG first, giving the import a palette
  context to land in. No more "open something first" friction.

### Palette → canvas highlight
- Multi-selecting palette swatches (Ctrl/Shift-click in the swatch grid)
  now **dims every pixel on the canvas that isn't one of the selected
  colors**, leaving the matching pixels at full brightness. Answers
  "where does this color live in the sprite?" at a glance.
- Single-color Alt-click isolation still works the same way; the new
  multi-select path is the same render code, generalized.
- Dim wash changed from pure black to muted indigo so it stays visible
  against dark fighter sprites where the old wash blended in.

### Eyedropper tool
- New explicit **Eyedropper tool** on the toolbar with the `I` shortcut
  (Photoshop convention). Left-click in this mode picks the underlying
  palette color and highlights it in the swatch grid.
- Right-click eyedropper still works in any tool mode (unchanged).

### Anipoint crosshair
- Anipoints are now drawn as **DOS-style + crosshairs** instead of the
  v2.5 solid circles. Primary anipoint is white, secondary is cyan,
  hover state brightens to yellow. Matches the registration-mark idiom
  of the original 1992 tool.
- When onion-skin is on, the **previous frame's anipoint** shows
  underneath as a dim gray crosshair — the same registration reference
  the DOS tool drew.
- Bug fix: anipoint drag no longer paints pencil strokes underneath
  the moving cursor.
- Bug fix: phantom diagonal line drawn from anipoint 1 to off-screen
  when the secondary anipoint sentinel was (-1, -1) — was casting the
  signed -1 to unsigned 0xFFFF.

### Toolbar polish
- **Icon refresh** — Lasso, Magic Wand, Smart Eraser, Clone Stamp,
  Smart Remap, Eyedropper, and Hitbox toggles now use Material Symbols
  glyphs that actually match their tool (lasso loop, sparkle wand,
  eraser block, control-point duplicate, palette, slanted dropper,
  activity zone).
- Icons vertically centered in their buttons (Material Symbols' empty
  descender space was biasing them upward).

### Internal: document refactor
- Per-document state (`img_p`, `pal_p`, image/palette counts, selection
  indices, file paths, sequence/script memory) is now a `Document`
  struct in [platform/document.h](platform/document.h) reached through
  a single `g_doc` pointer. ~550 callsites across 8 files renamed from
  bare globals to `g_doc->X`.
- Sets the stage for multi-tab editing — Phase 3 is just swapping
  `g_doc` between entries in a tabs container.

### Cleanup
- Em-dashes and other non-ASCII glyphs in tooltips were rendering as
  `?` in the default ImGui font; replaced with ASCII equivalents.
- Removed obsolete `/alternatename` linker pragmas that aliased C-side
  `_fpath_s` to long-gone ASM symbols.

## [v2.5.0] — Paste pipeline overhaul, Free Transform, file dialog

Major round centered on the paste workflow plus a much-improved file
dialog. The headline: pasting is now an Adobe-style scale-and-place
operation, and the file dialog grew thumbnails, sort options, and
double-click-to-open.

### Free Transform & paste pipeline
- **Ctrl+T Free Transform** — eight handles (4 corners + 4 edge midpoints)
  scale the floating paste. Aspect-locked by default; click the chain
  icon to unlock, or hold Shift to invert the lock for one drag.
  Enter / Ctrl+T commits (nearest-neighbor resample of the clipboard);
  Esc reverts.
- **Paste auto-enters transform** so every Ctrl+V drops you straight
  into resize mode. Clicking outside the rect is a one-shot
  commit-transform-and-apply-paste, matching Photoshop.
- **Tight-bbox crop on cut / copy** — the clipboard is automatically
  trimmed to its non-transparent content. A small motif inside a large
  marquee no longer pastes off-center.
- **Scale-to-fit on paste** — if the clipboard is bigger than the
  target sprite, nearest-neighbor downscale to fit (preserving aspect
  ratio). Toast tells the user what happened.
- **Center snap + passive centering guide** — drag the floating paste
  near the sprite's center to snap (Shift held), and a magenta guide
  always lights up when the paste rect's center is exactly on the
  sprite's center axis even without Shift.

### Adobe-standard shortcuts
- `Ctrl+A` Select All, `Ctrl+D` Deselect, `Ctrl+Shift+I` Invert Selection
- `Ctrl+J` Duplicate (image or floating paste), `Ctrl+E` Merge Down
- `Ctrl+T` Free Transform, `Shift+Del` Delete image (replaces old Ctrl+D)
- All five new entries land in the Edit menu with the correct accelerator
  labels and proper enabled/disabled state.

### File dialog
- **Preview thumbnails** for highlighted PNG and TGA files (192px,
  nearest-neighbor scaled, cached by full path). IMG and LBM previews
  are a follow-up — their loaders would need refactoring to write into
  a sandbox buffer first.
- **Sort by Name / Date / Size** with asc/desc toggle. Directories
  always sort first regardless of key. Persistent across opens.
- **Double-click to open** matches native OS file dialogs.
- **Per-category last-dir** memory — IMG / PNG / TGA / LBM each
  remember their own folder. Switching from "Save IMG" to "Import PNG"
  now lands in the PNG folder, not the IMG folder.

### Timeline
- **Ping-Pong playback** — new checkbox next to Onion. Plays forward
  then reverse and loops (e.g. frames 1→7→1→7→…) instead of wrapping.

### Quality of life
- **Image list auto-scrolls** to keep the selected sprite in view when
  navigating with Up/Down past the viewport edge.
- **Palette-list click commits** — clicking a palette now writes it
  onto the active sprite's palnum, so the choice sticks across sprite
  switches. Previously it was preview-only.
- **About dialog** shows the version, build date, git commit, ImGui
  version, and SDL2 version. Window title now reads `IMGTOOL v2.5.0`.

### Cleanup
- `IT/it.c` → `platform/main.cpp`, converted to C++. Dead code stripped:
  `mempool_*` externs, unused SIGINT handler, six write-only env-var
  buffers, two unused float conversion helpers.
- `IT/` directory removed entirely; `vcpkg.json` removed (unused by
  the build); `it.hlp` removed (the `h` key uses the embedded help text).
- macOS `_NSGetExecutablePath` properly used instead of the Linux-only
  `/proc/self/exe` fallback.

## [v2.4.0] — PNG import rewrite & palette workflow

Targeted fixes to the parts of the tool that were silently destructive or
silently lossy. The headline: PNG import is no longer broken.

### PNG import
- **Median-cut quantization** in 15-bit RGB space. Previously the import
  capped the unique-color histogram at 4096 entries and picked the
  palette by raw frequency, with exact-match-only pixel mapping. Any PNG
  with anti-aliasing or smooth gradients lost most of its colors to
  index 0 (transparent). Now: full histogram across all 32768 possible
  15-bit colors, pixel-weighted bucket splits on the widest channel,
  and every opaque pixel maps to the nearest palette entry by Euclidean
  RGB distance. The per-source-color lookup is cached so megapixel PNGs
  stay fast.
- **Import (Match to Active Palette)** is now actually wired up. The
  function existed but had no dispatch — the menu item did nothing.
  Now imports a PNG and maps every pixel into the currently-selected
  image's palette, no new palette created.

### Palette workflow
- **Cross-file palette clipboard.** Copy / Paste buttons next to
  Add/Merge/Dup/Del. The clipboard is malloc-backed (not pool-backed)
  so it survives File→Open — lift a palette out of one IMG and paste
  it into another.
- **Palette-list click now commits.** Clicking a palette in the list,
  or Up/Down with palette-nav active, now writes the choice onto the
  active sprite's `palnum`. Previously it was preview-only; switching
  sprite snapped the selection back to the new sprite's palette,
  making the click appear to "reset". Undoable, dirty-tracked.
- **Reset → Reset HSL.** Renamed with a tooltip clarifying that the
  button resets the three sliders and the palette baseline, not the
  whole palette.

### Unsaved-changes guard
- **Confirm dialog now also fires on File→Open / Open Recent / Open LOD**
  via a PendingAction queue. Previously palette / HSL edits could be
  silently thrown away by reflex-opening another IMG. Quit-time
  behavior is unchanged.
- **Import paths now mark dirty.** Append / Load LBM / Load TGA /
  Import PNG / Import PNG Match all previously left the dirty flag
  alone, so freshly-imported work could disappear on the next
  File→Open. Fixed.

### UI polish
- **Properties panel column alignment.** New `LabeledValue()` helper
  renders label + value via `SameLine(col_x)` instead of hand-padded
  format strings. Fixes the DMA ROM row that was drifting two columns
  left of the others, and survives future label changes.

### Internal
- **`mark_dirty()` helper** replaces 25 scattered `g_dirty = true`
  writes. Single entry point for future side-effects (auto-backup
  hook, dirty-bit tracing, etc).



A large round of pixel-art tools and palette workflow. Highlights:

### Tools
- **Lasso (L)** — real freehand selection. Drag a polygon, release, get
  a mask-aware pixel selection that copy/paste honors.
- **Magic Wand (W)** — tolerance slider + contiguous/global toggle.
- **Smart Eraser (E)** — formerly "Background Eraser", now a chroma-key
  remove with tolerance and an optional defringe pass that averages
  the 1px halo of blue/green-screen spill on digitized actors.
- **Clone Stamp (C)** — Alt+click to anchor, round-disc brush with
  radius slider, cyan source crosshair, white brush ring at cursor.
- **Smart Remap** — paint over a color to swap it for the selected
  index; respects a tolerance slider; target resets on mouse release.
- **Auto-Sprite Chopper** — slice marked images into a hardware grid,
  trim transparent borders, recalculate ANIX/ANIY. Suffix-overflow
  bug for >26 columns and >9 rows fixed.

### Animation Timeline
- Bottom strip with Play/Stop, FPS slider, frame thumbnails (per-image
  texture cache), drag-and-drop reorder.
- `K` plays/pauses (Premiere/DaVinci convention; Spacebar stays bound
  to toggle-mark on current image).
- `Ctrl+Left/Right` nudges the playhead frame within the timeline order.
- **Onion-skin toggle** — ghosts prev (cool blue) and next (warm orange)
  frames behind the live sprite, anipoint-aligned.

### Palette workflow
- **Saturation slider** (-100..+100%) and **Lightness slider** (-100..+100%)
  alongside the existing Hue slider. Make a yellow more yellow, fade
  toward white, drop toward black.
- **Multi-select honored.** Ctrl-click toggles a swatch's membership;
  Shift-click extends a range; HSL sliders affect only the selected
  subset (or the whole palette if nothing is selected). Hint text
  above the sliders calls out which mode you're in.
- All three sliders rebuild from a baseline snapshot each tick, so
  Reset is just "sliders to 0" and saturation +50 then -50 returns
  exactly to original.
- **Color isolation overlay** — Alt+click a swatch to dim every pixel
  that isn't that index. Click again to release.
- **Dirty indicator** on the menu bar. Amber `● filename` when the
  file has unsaved changes; plain grey filename when clean.

### Selections, paste, snap
- **Snap-to-Content.** Hold Shift while dragging a pasted floating
  selection — the paste rect snaps to the underlying sprite's
  non-transparent bbox. Threshold is screen-pixel scaled (so it feels
  the same at every zoom). Magenta cross-canvas guides flash when a
  snap is locked.

### One-shot operations (Operations menu)
- **Crop Marked to Content** — trim each marked image to its
  non-transparent bbox; anipoints adjusted so the on-screen position
  is unchanged.
- **Defringe Marked Edges** — one-pass 8-neighborhood average over
  every opaque pixel that touches a transparent neighbor. Kills the
  bluescreen halo on digitized actors.
- **Align Marked Anipoints to Selected** — set every marked image's
  anipoint to match the currently selected image's anipoint. For
  anchoring heads, hands, weapon hilts across many frames.

### Refactor
- 11 modal dialogs (Rename, LOAD2 Verify, Histogram, Auto-Chop, Bulk
  Restore, Debug Info, New IMG, Unsaved Changes, Help, About,
  Verbose Log) lifted out of `imgui_overlay_render` into named
  helpers. Render function shed 634 lines.

### Bug fixes
- Clone Stamp source no longer leaks across image switches (was
  reading pixels at coords from the previous image).
- Lasso polygon and snap bbox cache reset when switching images.
- Timeline strip height fixed (was clipping the bottom of thumbnails
  after the strip went from numeric buttons to 48px tiles).
- Onion-skin thumbnails no longer paint a checkerboard over
  transparent regions (the checker was useful only for the timeline
  strip; the texture is now honestly transparent on palette index 0).
- Palette multi-select is cleared when switching images or palettes,
  matching user intent of "this is a fresh context".
- Ctrl+click on an already-selected swatch now visibly deselects
  (the white "current color" border no longer hides the disappearance
  of the yellow "selected" border; Ctrl+click also no longer
  reassigns the active color, matching Photoshop's convention).

## [v2.2.0] and earlier

See git history. CHANGELOG started at v2.3.0.
