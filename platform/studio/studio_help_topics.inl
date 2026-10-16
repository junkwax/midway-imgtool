// Studio workflows plus retained classic technical reference; no external help files needed.
struct HelpTopic {const char *title,*text;};
static const HelpTopic help_topics[]={
    {"Getting started",R"STUDIO_HELP(Imgtool Studio edits Midway IMG sprite libraries and their indexed palettes. The original imgtool remains a separate application.

1. Use Ctrl+O to open an IMG, or Explore sample to learn without touching game assets.
2. Browse sprites in the library using Up/Down or PageUp/PageDown. Tap Space to mark a sprite; Shift+M marks all and M clears marks. Marks choose batch inputs and do not edit the file.
3. Choose a tool on the left; the color wheel edits the active RGB555 palette entry. Shared colors affect every sprite using that palette. Index zero remains transparent.
4. Use Tab to switch Artwork and Animation & alignment. Ctrl+Tab / Ctrl+Shift+Tab switches open IMG libraries. IMG animations edits stored sequences and scripts; Studio projects save separate .ISTUDIO compositions.
5. Ctrl+S saves; Ctrl+Shift+S chooses a destination. Save and Save As always retain the IMG's original version and record layout. Review LOAD2 packing before building game assets.

Each IMG and project keeps its own Undo history. Source editors have separate histories and Save controls. Closing or quitting protects applied unsaved edits. Escape cancels gestures and previews; File > Quit exits.

H or F1 opens this guide. Search checks topic titles and contents; Keyboard reference is generated from the actual command and tool bindings.)STUDIO_HELP"},
    {"Keyboard reference",R"STUDIO_HELP()STUDIO_HELP"},
    {"Selection & pixels",R"STUDIO_HELP(Each library keeps its own undo history. Shift adds to a selection; Alt subtracts; Shift+Alt intersects. Paint, erase and fill stay within selected pixels. Copy/cut preserve the shape, including selected transparency: tinted transparent pixels in the paste preview clear the destination. Copy without a selection leaves transparent pixels untouched when pasted. Nudging moves pixels without moving anchors; add Shift to move 10 pixels.)STUDIO_HELP"},
    {"Paint, line & fill",R"STUDIO_HELP(Line previews a round/square stroke: Shift snaps directions, release applies, Escape cancels. Replace keeps the color clicked at the stroke's start; All matching pixels replaces across the sprite or selection. Fill and Smart erase choose connected/global matching. Exact index is the default; Match palette colors includes duplicate slots and nearby shades at the chosen tolerance. Transparency stays separate from opaque black. Connected fills cannot cross selection holes.)STUDIO_HELP"},
    {"Clone stamp",R"STUDIO_HELP(Clone (C): Alt-click a source on the current sprite, then drag to copy pixels inside the selection. Each stroke freezes its source to prevent overlap feedback. Aligned strokes keep the offset; turn it off to start each stroke at the source. Copy transparent pixels explicitly enables erasing. Escape restores the unfinished stroke.)STUDIO_HELP"},
    {"Variant paint",R"STUDIO_HELP(Variant (V / T): choose an opaque palette color, drag to preview, or use Preview selected pixels. Recolors only the painted pixels in this palette while retaining their appearance under other palettes. Transparent pixels stay transparent. The review shows slot reuse, palette growth and LOAD2 PPP limits. Apply/Enter commits pixels and palettes in one undo step; Escape/Cancel/Undo discards the preview. Unused palette entries may be repurposed. No available slots blocks the complete edit.)STUDIO_HELP"},
    {"Blur & smudge",R"STUDIO_HELP(Blur (O) softens neighboring selected colors using one frozen source per stroke. Another stroke builds up the effect. Smudge (S) pulls colors along a drag, sampling the preceding brush position at pixel intervals. Both have round/square brushes and Strength controls, match results to existing opaque palette entries, preserve transparent outlines, and limit sampling and writes to the selection. Release commits one undo step; Escape restores the unfinished stroke and keeps its selection. Sparse palettes may limit the visible effect.)STUDIO_HELP"},
    {"Heal & defringe",R"STUDIO_HELP(Heal (J) replaces a brushed blemish by diffusing surrounding colors inward, using the frozen source and cumulative brushed area. Fill depth controls how far colors can travel. Defringe (F) cleans an outline band from the sprite interior; drag locally or Clean all/selected edges. Edge width is 1-4 pixels. Both use existing opaque palette colors, preserve transparency and keep unreachable repairs unchanged. Selection limits reads and writes. Release groups a stroke; Escape restores it and keeps the mask. Smart erase (E / U) has Clean erased edges enabled by default: surviving neighbors adopt safe local colors in the same undo step. Turn it off for a plain matching erase.)STUDIO_HELP"},
    {"Sprite overlay (L)",R"STUDIO_HELP(While placing a pixel paste or transforming clipboard pixels, press L or choose Edit > Keep paste as overlay. This keeps one separate indexed overlay on the current sprite. An existing overlay must be flattened or removed before adding another. Outside a paste, L selects Lasso.

The Sprite inspector controls visibility, X/Y position, one-pixel nudges (Shift = ten), horizontal/vertical flip, Flatten and Remove. Pasting as an overlay selects Move and the Overlay target automatically. With Move (Shift+V), choose Overlay or Base selection in the inspector. Drag a visible overlay inside its bounds; release applies one Undo step. Shift locks the dominant axis. Arrows move the overlay one pixel, Shift+arrows ten, without browsing frames. Escape or Undo cancels a drag without consuming history. Focus loss, workspace/sprite changes, modal dialogs and source edits cancel too. Space+drag still pans. Hidden overlays cannot be dragged; use coordinates to recover a completely off-canvas overlay. Base selection moves selected base pixels while the overlay stays in place, and both selection and line previews show the composite. Every overlay edit supports Undo/Redo. Negative positions and overhanging pixels are allowed; the canvas clips them. Duplicate sprite copies its overlay independently. Paint and pixel Copy operate on the base beneath it. Index zero always stays transparent on an overlay, including explicitly selected zeros that would clear pixels with ordinary Apply.

Save and Save As include the visible overlay as ordinary flattened pixels, keeping the original IMG version and layout. The live base and separate overlay remain editable until the library closes. Hidden overlays are excluded from output. Reopened IMG files contain flattened pixels and no editable layer. PNG, indexed LBM/TGA, TGA sheets and scene PNG export use visible appearance. LOAD2 checks use the flattened result. Geometry, region extraction, source palette remapping, compaction, variant color allocation and pipeline export require flattening or removing the relevant overlays first. Sprite copying between libraries also requires flattening the source overlay.)STUDIO_HELP"},

    {"Move & transform",R"STUDIO_HELP(Move (Shift+V) drags selected pixels into a floating preview. During Paste, Ctrl+T opens a transformed clipboard preview; H/V flips, arrows position, Enter or Ctrl+E applies, and Ctrl+J stamps while keeping another floating copy. Copy colors map to the destination palette; selection holes and explicitly selected transparent pixels remain distinct. Oversize clipboard transforms require increasing the canvas or Paste as New Sprite. Ctrl+T or Move / transform opens its inspector. Drag corners to resize, flip or rotate in 90-degree steps, or enable Duplicate instead of move. Enter or Apply commits one undo step; Escape, Cancel or Ctrl+Z discards the preview. Arrows move the preview by 1, Shift+arrows by 10. Keep it inside the sprite canvas. Anchors and metadata stay unchanged. Palette indices are retained with nearest-pixel sampling from the original selection. Changing the source or workspace cancels an unapplied preview.)STUDIO_HELP"},
    {"TGA sprite sheets",R"STUDIO_HELP(File > Export TGA sprite sheet reviews marked, shown, all or selected full sprites. Exclude rows without unmarking; inspect packed rectangles, anchors and face order, scroll to zoom and middle/right-drag to pan. Indexed output keeps matching palettes or combines exact used RGB555 colors; too many colors blocks instead of degrading art. RGBA supports per-sprite colors/alpha without ANF. Classic ANF stores anchors and faces in library order and requires a 256-wide indexed sheet, sprites up to 256x256, at most 1000 faces and 6000 rows. Both outputs are prepared first; second-replacement failure attempts to restore the first. Two files are not globally atomic against interruption or external changes. Build again after source or destination changes. Source IMG format, pixels, history and marks stay intact.)STUDIO_HELP"},
    {"TBL, IRW & ANILST",R"STUDIO_HELP(File > Export pipeline reviews TBL, IRW and ANILST output for marked, shown, all or selected whole sprites. Build after changing options or destination; exclude blocked rows before exporting. TBL SAGs and IRW pixels use the same uncompressed layout and depth settings, with Word/Nibble/Packed alignment and optional dual-bank addressing. TBL CTRL comes from bit depth, with optional MK3 secondary anchors and palette pointers. ANILST is a sequential sprite list. Source/destination changes require rebuilding; output replacement is atomic. These exports do not reproduce LOAD2 compression, deduplication, scaling or existing ROM allocation.)STUDIO_HELP"},
    {"LOD manifests",R"STUDIO_HELP(File > Open LOD libraries reviews LOAD2 manifests, paths and per-library PPP settings before opening separate IMG tabs. Rebuild after changing folders; resolve or exclude missing/invalid rows. Open tabs retain unsaved edits and current PPP. Other LOAD2 directives are listed but do not run: no ROM or table outputs are written. Cancel leaves libraries unchanged.)STUDIO_HELP"},
    {"Files & sprite commands",R"STUDIO_HELP(File > New IMG library (Ctrl+N), Import PNG sprite and Export selected PNG cover the basic file workflow. Sprite commands manage names (Ctrl+R / F2), copies (Ctrl+J), order and geometry. Right-click a library row for the same actions. The inspector assigns palettes or makes one private before recoloring. Structural edits and geometry changes undo with their metadata and selection.)STUDIO_HELP"},
    {"Animation & alignment",R"STUDIO_HELP(Animation & alignment (Tab): browse name-based frame groups, reconstruct subframes, scrub the timeline and press Space to play/pause. Arrows step frames. World View holds the anchor target fixed; drag a piece to edit its IMG anchor, or enable Drag the whole frame. Escape cancels the gesture. In Artwork, use Select to drag the yellow anchor handle.)STUDIO_HELP"},
    {"Studio projects & timing",R"STUDIO_HELP(Project > Create from current preview saves a composition as .ISTUDIO with its own undo history. Add actor lanes and cross-file frames, edit holds and positions, then save referenced IMGs before saving the project. Dragging an actor in a project edits scene placement, not IMG anchors. Motion & timing adds Show/Hide windows, velocity, maximum travel, a freeze tick and a second copy. Scheduled frames can overlap. Follow frame timing connects an effect to another actor's frame; retiming or reordering that frame moves the effect's start. Detach keeps the current start as an independent actor. This follows timing only, not position or visibility. Open project / import WAX accepts these row rules and uniform spawn runs; remaining unsupported rules are reported. Export current scene PNG uses the same scene positions and timing as the canvas.)STUDIO_HELP"},
    {"Batch geometry",R"STUDIO_HELP(Batch geometry: mark sprites with the library checkboxes; Marks can mark every shown sprite or clear membership. Batch... or Sprite > Batch geometry opens a review for Marked, Shown by search, or All sprites in this library. Trim, resize by percent/exact size, canvas size and rotate/flip share the existing geometry behavior. Select a row and compare Before/After at one anchor target; sizes, anchors and cropped pixel counts are shown. Uncheck rows to exclude them. Finish editing settings to refresh the preview. Apply changes all enabled rows in one Undo step; Cancel/Escape leaves the IMG intact. Marks follow stable sprite identities through reorder/Undo and remain session view state. The batch edits whole sprites. Empty trims keep their original canvas, and geometry can affect LOAD2 packing.)STUDIO_HELP"},
    {"Restore child sprites",R"STUDIO_HELP(Sprite > Restore child sprites reviews parent-to-child pixel restoration in this library. Scope Marked, Shown by search or All children; choose a searchable parent or use a name rule with one capture (WALK1A -> WALK1). Build preview after settings change. Reconstruct keeps both silhouettes; Diff copies parent pixel changes since load and needs an equal-size baseline; Replace clears the child and copies overlap; Fill writes only transparent child pixels. Palette mismatches, missing/ambiguous parents and absent baselines are excluded. Partial coverage starts unchecked; Parent context shows child bounds in gold and uncovered areas in red. Before/After and Changes compare actual edits. Apply is one Undo step; Cancel/Escape discards the preview. Anchors, geometry and metadata stay intact. Pixel selections do not limit whole-child restoration. New or resized parents may lack a usable Diff baseline; choose another mode. Cross-library restoration is not part of this workflow.)STUDIO_HELP"},
    {"Extract & split sprites",R"STUDIO_HELP(Sprite > Extract selection and Split body parts review child sprites before copying or cutting. Adjust part boxes and names, trim borders, and preview the remaining source. Children retain palette and primary-anchor alignment; existing animation references stay on the source. One Undo restores the entire extraction.)STUDIO_HELP"},
    {"Indexed LBM / TGA",R"STUDIO_HELP(File > Import indexed LBM / TGA reviews one full-canvas indexed sprite, keeps its source palette order or matches an existing palette, and lets you click the preview to place its anchor. LBM / IFF and indexed TGA drag-and-drop use the same review. File > Export selected LBM / TGA writes a separate indexed image without changing the IMG or history. LBM exports store the primary anchor as GRAB; TGA does not store IMG anchors. Settings and destination changes require rebuilding the review. Full-color TGA remains available through sprite-sheet import. File import also reviews sprite sheets and GIFs, digitizes source frames with material ramps, and captures video into the digitize review. Palette > Manage handles palette files, merging, compaction and gradients. File > Export pipeline writes reviewed TBL, IRW and ANILST files. Every existing IMG keeps its original version and record layout on Save / Save As. Edits that cannot fit that format cannot be saved. Pre-WIMP 5 and transitional 0x632 / 0x633 layouts, additional legacy image variants and game runtime and phase-list authoring remain on the migration list.)STUDIO_HELP"},
    {"ASM animations",R"STUDIO_HELP(Tools > ASM animations loads a read-only character ASM and resolves multipart frames across open IMG libraries. Choose a preferred library for duplicate names. Arrows step; Space plays; hold ticks override source timing. Flips, offsets, source segments and jumps are inspected; unsupported branches stop visibly and runtime calls are not executed. Feet to floor and object origin change preview placement only. Tools > MK2 strike tables edits an independent ASM document with its own Undo/Redo and reviewed Save. Animation source / Animate previews the move while you resize its strike box. Tools > Strike associations manages the classic .imgtool sidecar beside each strike source. Save associations remembers overrides, including None, with independent Undo/Redo and reviewed writes. Source switching retains unsaved sidecars; quit offers Save/Discard/Keep editing. Associations do not establish active-frame timing. IMG pixels, anchors, history and format stay unchanged.)STUDIO_HELP"},
    {"Copy / move libraries",R"STUDIO_HELP(Sprite > Copy / move to another library reviews sprites and required palettes between open libraries. Copy is the default. Enable Move from source to append them to the destination and remove them from the source. Both libraries are prepared before either changes. Each has its own Undo step; undo each separately to reverse the complete move, and Save both separately. Embedded-animation and open-scene references protect source sprites; Exclude referenced sprites keeps them. Keep at least one source sprite. Remaining native indices are remapped, source palettes are retained, and neither IMG format changes. External game references and closed projects are not scanned. Native animation records stay in their original library; use the animation transfer tools for records.)STUDIO_HELP"},
    {"Split IMG animations",R"STUDIO_HELP(File > Split IMG animations to new library creates a separate IMG tab with selected native sequences/scripts, called sequences and only the artwork/palettes they require. Animation > Split to new library offers the same review in IMG mode. Original names and the source IMG version/layout are retained; the source is unchanged. The new tab opens in IMG animations mode, has no creation Undo and participates in the unsaved-close guard. Save it when ready. Select an animation that references artwork; a library made only of empty records is not created. Unique source sprite names are required, and damage-bound sequences remain unsupported. This copies native IMG records, not scene projects or external game tables, and leaves every clipboard unchanged.)STUDIO_HELP"},
    {"Animation clipboard",R"STUDIO_HELP(File > Copy IMG animations to clipboard captures selected records, called sequences and their required artwork into an independent session snapshot. The source may close after copying. File > Paste IMG animations from clipboard reviews captured records against any open destination, with prefixes, manual sprite choices and Copy missing artwork enabled by default. Paste is one destination Undo step; Save separately. Animation menu offers the same commands in IMG animations mode. Edit > Clear animation clipboard releases it; Cancel or failed capture retains the previous clipboard. Pixel, palette and sprite clipboards are separate. Contents last until replaced, cleared or Studio closes. Capture retains names and requires unique source sprite names. Unsupported damage bindings and original-layout constraints still apply; IMG formats never change.)STUDIO_HELP"},
    {"Copy IMG animations",R"STUDIO_HELP(File > Copy IMG animations to library transfers sequences and scripts between open libraries. Selected scripts include their called sequences. Enable Copy missing artwork to append referenced sprites that have unique missing names and their required palettes. Existing matches stay unchanged; ambiguous names still need manual choices. The sprite prefix also names new artwork. The review labels new sprites Copy and lists palette additions. Without that option, referenced artwork must already exist. Unique names match automatically; use Destination sprite prefix for prefixed artwork, or search each destination dropdown by name or index to bind differently named or duplicate sprites. Manual choices survive prefix and animation selection changes; changing libraries clears them. Reset sprite choices or choose Automatic name match to restore matching. Review sprite indices and geometry, and use an animation prefix for record name conflicts. Apply preserves stored timing, offsets and opaque bytes, remaps incoming targets, and opens the destination for inspection. One destination Undo step includes all copied artwork and records, no autosave and no IMG format changes. Damage-bound sequences and incompatible older-layout fields are blocked; runtime script execution is not included.)STUDIO_HELP"},
    {"Sprite clipboard & split",R"STUDIO_HELP(Sprite > Copy to another library appends marked, shown or all sprites to a second open IMG. Review names, an optional prefix, palette assignments and exclusions. Exact palette reuse compares colors and metadata; otherwise required primary/alternate palettes are copied once. Copies keep pixel indices, anchors, points, baselines and metadata, with remapped alternate references. Name collisions, unsupported layouts and stale libraries block Apply. One undo removes destination additions; the source stays unchanged and both IMG formats stay fixed. This copies listed sprites without source sequences/scripts. Sprite > Split to new library creates a separate unsaved IMG tab with the source layout. Copy leaves the source unchanged; Move protects referenced sprites and removes eligible rows in one source undo step. Source Undo restores the sprites while the new tab remains. Save each library explicitly. Sprite > Copy sprites to clipboard captures an owned set that survives source edits and closure during this Studio session. Paste sprites from clipboard uses the same destination review and one undo step. Copy again to replace the snapshot, or clear it from Sprite/Edit. Pixel and palette-color clipboards remain separate. Include child pieces adds direct related sprites with parent labels; ambiguous matches are reported for explicit selection.)STUDIO_HELP"},
    {"Delete multiple sprites",R"STUDIO_HELP(Sprite > Delete multiple sprites reviews marked, shown or all sprites with a preview and exclusions. Embedded sequence and current open Studio scene references block deletion; Exclude referenced sprites keeps them automatically. Only listed rows are deleted and at least one sprite stays. Remaining embedded sprite indices are remapped while script sequence indices remain intact. One undo restores pixels, metadata, references and selection. IMG version and layout stay fixed. Child pieces are not added implicitly; external files and closed scenes are not scanned.)STUDIO_HELP"},
    {"Palette assignment & cleanup",R"STUDIO_HELP(Palette > Remove unused palettes reviews all primary and alternate uses, with per-palette keep choices and one undo step. Retained references are remapped, without changing colors, pixels or the IMG format. External palette-name references are not scanned. Palette > Assign to multiple sprites reviews marked, shown or all sprites with a before/after primary-palette preview. Match colors keeps transparency and maps opaque colors to opaque destination slots; approximate colors require review. Keep indices changes the palette reference without changing pixel bytes. Remaps that would alter active alternate-palette appearances are blocked; keep indices or exclude those sprites. Apply uses one undo step and keeps metadata, baselines and the IMG version/layout. Palettes are not resized or modified; check LOAD2 packing after palette-size changes.)STUDIO_HELP"},
    {"Batch rename",R"STUDIO_HELP(Sprite > Batch rename previews numbered names, prepended text or appended text plus numbers for marked, shown or all sprites. Names are limited to 15 printable ASCII characters without spaces. Excluded rows retain their current names and other preview numbers do not move. Collisions against the whole library block Apply. All changed names share one undo step; pixels, metadata and embedded references remain intact. IMG version and layout stay fixed. External ASM / TBL names are not rewritten, and naming changes can affect LOAD2 allocations.)STUDIO_HELP"},
    {"Split with IMG animations",R"STUDIO_HELP(Sprite > Split to new library offers Include IMG animations with Copy. It copies sequences using only included sprites and scripts whose called sequences are copied. Review copied/skipped records: empty records, outside references and unsupported damage bindings stay out. The sprite set never expands automatically; use Include child pieces or change exclusions to cover a complete animation. The source stays unchanged; the new IMG inherits its original layout and starts unsaved. Move from source is separate and cannot include animation records.)STUDIO_HELP"},
    {"Fatality source workspace",R"STUDIO_HELP(Tools > Fatality source keeps up to 16 ASM sources open: commands, scom_* combos, or animation data. Add source retains other files and their histories. The source selector restores the last block in each file. Reload source and Close source protect applied edits; Quit checks every modified source. Linked combos prefer the current file, then unique matches in other open sources; choose explicitly if several match. Save As cannot overwrite another open source. Each file saves separately. The label line stays fixed; edit indented body lines, Apply block, then review Save source. Source Undo/Redo is separate from IMG history. Switching blocks resets unapplied text. In-place saves reject external changes, and close/reload/quit protect applied unsaved edits. Inspect animation snapshots applied edits in all open sources, follows unique external jumps and frame groups, and reports ambiguous symbols. Review artwork lists every expanded symbol, counts its uses, locates its first frame, and offers explicit choices among matching sprites in open IMG libraries. Choices are shared with the canvas and reaction table; they last for the loaded source and reset on successful ASM load. Refreshing the artwork review keeps choices. Closed or renamed choices stay visibly unavailable until restored or chosen again. Frame locations identify their defining files. Back to fatality source returns to editing; inspect again to refresh the snapshot. This does not assemble code, follow includes, run game routines or infer attacker/victim timing. Existing IMG formats and artwork stay unchanged.)STUDIO_HELP"},
    {"Reactions",R"STUDIO_HELP(Tools > Reactions groups the loaded character ASM by Hit, Stagger, Knockdown, Throws, Getup and Death. Search includes thrower names and classification reasons; Show other animations exposes entries the suggestions missed. Choose a frame/piece row to scrub, then Open selected sprite to edit its IMG anchor. Frame anchors refresh after edits, Undo, library changes and facing/convention changes. Missing or ambiguous pieces stay explicit. Placement is relative to object origin using each library's mirror convention. Source ASM is read-only; game routines and attack/reaction timing are not executed or inferred.)STUDIO_HELP"},
    {"Strike phase inspector",R"STUDIO_HELP(Tools > Strike phase inspector shows which box the loaded source selects as a manually supplied active countdown decreases. Assume stkphase / wunit flags affect only this preview, including conditional slot numbering; build files stay unchanged. Red is the selected box, gray is its base reference. Edit selected strike opens the existing ASM editor. Edit phase rows stages existing thresholds and slots, previews the selection, and applies through the source undo history. Review source save opens the ASM diff before writing. Row counts stay fixed, thresholds decrease to zero, and all consumers must resolve under the chosen preview flags. Build settings and IMG format stay unchanged. Artwork frames are chosen independently; startup, recovery and hit interruption are not inferred.)STUDIO_HELP"},
    {"Frame origins & animation points",R"STUDIO_HELP(The frame origin is local pixel coordinate (0,0), the top-left of the rectangular buffer. Transparent padding is part of that rectangle. X increases right; Y increases down.

Primary animation point (AX/AY) is a signed offset from that origin and can lie outside the rectangle. At world anchor (WX,WY):
    sprite top-left = (WX - AX, WY - AY)
    pixel (x,y) = (WX + x - AX, WY + y - AY)

Choose the same physical feature in each frame to keep animation stable. In Artwork use Select (R) to drag the yellow anchor handle, edit inspector values, or use Animation & alignment to drag pieces around a fixed world anchor. Link animation anchors offers explicit reference and target points. Onion skin and facing preview help compare poses.

Secondary AX2/AY2/AZ2 is another point inside the same frame, not a link to another frame. Stored FFFF/FFFF/FFFF means unused; zero is a valid active point.

IMG animations stores sequence entries (sprite index, ticks, offsets and spare words) and script calls. Reordering the name-based preview does not write a stored animation. A Studio project edits scene placement separately from IMG anchors.

Horizontal mirroring uses the library's selected convention; engine paths may differ by one pixel. Preview both facings and verify the convention used by the game's routine.)STUDIO_HELP"},
    {"IMG, palettes & file formats",R"STUDIO_HELP(IMG is the primary binary little-endian sprite-library format. Each file includes a library header, image/palette records, pixel and palette blocks, and optional embedded sequences, scripts, point tables, alternate palettes and version-specific fields.

Modern IMAGE records are 50 bytes with a 16-byte name. PALETTE records are 26 bytes with a 10-byte name. Earlier layouts differ: Studio retains the loaded version and layout on both Save and Save As. Unsupported layouts are rejected, never converted.

In-memory indexed pixels use a four-byte-aligned row stride: (width + 3) & ~3. Palette colors are RGB555: five bits per red, green and blue channel. Index zero is transparent. Default palettes #0/#1/#2 are implicit on disk; stored palettes begin at #3. Names can contain preserved bytes after the null terminator, which LOAD2 may hash for allocation. Renaming can affect the resulting packing.

TGA: indexed color-mapped exchange, or reviewed RGBA sprite sheets. Classic ANF face metadata has stricter sheet size and frame-count constraints; see TGA sprite sheets.
LBM / IFF: indexed ILBM/PBM variants supported by the importer, with CMAP and BODY chunks and supported RLE compression. Unsupported variants report an error.
PNG: RGBA import/export, quantized to Midway indexed colors on import. Color zero exports with transparency.
GIF / video: frame capture and digitize/import reviews produce indexed sprites and optional IMG sequences. Video needs an ffmpeg executable.
TBL / IRW / ANILST: external game-pipeline exports, not replacement IMG formats. See export topics for packing limits.
ISTUDIO: a separate composition/project format with references to IMG assets. Save referenced IMGs separately.

Studio applies its own bounded import and allocation limits, with errors for unsupported geometry or layouts. The old help's 640x400 limit and automatic pre-2.x conversion instructions do not apply.)STUDIO_HELP"},
    {"DMA2 hardware reference",R"STUDIO_HELP(
        THE BRAND SPANKING NEW DMA (#2)

            KEEP ENTERPRISES, INC.

            JANUARY 1, 1992

            DOCUMENT REV. 1.5


    DMA # 2 - GENERAL INFORMATION

    - THE NEW DMA WILL INCORPORATE BACKWARD COMPATIBILITY TO THE OLD DMA
    IN BOTH PINOUT AND FUNCTIONALITY.

    - THE NEW FEATURES IN ADDITION TO THE OLD ARE AS FOLLOWS:

        1) VARIABLE PIXEL SIZE PROCESSING.  THE NEW DMA CAN PROCESS
           1 TO 8 BIT PIXELS THAT ARE STORED IN IMAGE MEMORY IN A
           SERIAL FASHION.

           EXAMPLE:  5 BIT PIXELS STORED INTO 8 BIT EPROM
           +---+---+---+---+---+---+---+---+
           |P1 |P1 |P1 |P1 |P1 |P2 |P2 |P2 |
           +---+---+---+---+---+---+---+---+
           |P2 |P2 |P3 |P3 |P3 |P3 |P3 |P4 |
           +---+---+---+---+---+---+---+---+
           |P4 |P4 |P4 |P4 |P5 |P5 |P5 |P5 |
           +---+---+---+---+---+---+---+---+
           |P5 |P6 |P6 |P6 |P6 |P6 |P7 |P7 |
           +---+---+---+---+---+---+---+---+

        2) THE NEW DMA CAN BE HALTED IN THE MIDDLE OF A TRANSFER
           AND THEN BE RESTARTED TO RESUME THE TRANSFER.  THIS IS
           ACCOMPLISHED BY WRITING A ZERO TO THE DMA GO BIT (BIT 15)
           IN THE CONTROL REGISTER.  IN THE OLD DMA, THIS WOULD KILL
           THE TRANSFER SO THAT IT COULD NOT BE RESTARTED.  TO KILL
           A TRANSFER IN THE NEW DMA, WRITE A ZERO TO THE DMA GO BIT 2
           TIMES IN A ROW.  TO RESTART A TRANSFER AFTER HALTING,
           WRITE A ONE TO THE DMA GO BIT IN THE CONTROL REGISTER.

        3) IN ADDITION TO THE CLIPPING ACHIEVED BY MANIPULATING THE
           OFFSET REGISTER, IN THE NEW DMA, A METHOD OF CLIPPING
           USING REGISTERED CLIP VALUES IS AVAILABLE. THE HOST CAN
           SPECIFY CLIP AMOUNTS TO THE DMA AND THE MATH NEEDED TO
           IMPLEMENT A TRANSFER IS DONE INTERNAL TO THE DMA.

        4) THE NEW DMA CAN DO A TRANSFER FROM THE IMAGE MEMORY TO THE
           BIT MAP WITH A SCALING EFFECT, I.E. THE IMAGE CAN BE
           SHRUNK OR ENLARGED.

        5) THE NEW DMA IMPLEMENTS A COMPRESSION MODE IN WHICH LEADING
           AND TRAILING ZERO DATA PIXELS CAN BE ENCODED IN A RUN LENGTH
           FASHION TO SAVE ON IMAGE MEMORY.

        6) OFF SCREEN CLIPPING CAN BE AUTOMATIC.  THERE ARE FOUR
           REGISTERS THAT SPECIFY THE WINDOW TO WHICH THE DMA CAN
           TRANSFER DATA.

        7) NOTE THAT THE CONTROL REGISTER AND THE OFFSET REGISTER
           HAVE BEEN SWAPPED SO THAT THE MOVE MULTIPLE INSTRUCTION
           CAN BE USED TO DOWNLOAD THE REGISTERS AND SET DMA GO
           EFFICIENTLY.

    DMA # 2 - INTERNAL REGISTERS R5-R0

        REGISTER # 7 - SOURCE VERTICAL SIZE REGISTER
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
        | 0 | 0 | 0 | 0 | 0 | 0 |           VERTICAL SIZE              |
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

        REGISTER # 6 - SOURCE HORIZONTAL SIZE REGISTER
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
        | 0 | 0 | 0 | 0 | 0 | 0 |          HORIZONTAL SIZE             |
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

        REGISTER # 5 - DESTINATION ADDRESS - Y
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
        | 0 | 0 | 0 | 0 | 0 | 0 | 0 |      DESTINATION Y COORDINATE     |
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

        REGISTER # 4 - DESTINATION ADDRESS - X
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
        | 0 | 0 | 0 | 0 | 0 | 0 | 0 |      DESTINATION X COORDINATE     |
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

        REGISTER # 3 - SOURCE ADDRESS - HIGH ORDER
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
        |               SOURCE ADDRESS UPPER 16 BITS                   |
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

        REGISTER # 2 - SOURCE ADDRESS - LOW ORDER
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
        |               SOURCE ADDRESS LOWER 16 BITS                   |
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

        REGISTER # 1 - CONTROL REGISTER   ** SEE NOTE 1 BELOW
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
        |DGO|  PIX SIZE |TM1|TM0|LM1|LM0|CMP|CLP|VFL|HFL|   PIXEL OPS   |
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

        REGISTER # 0 - OFFSET REGISTER / RCLIP-LCLIP VALUES
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
        |            OFFSET VALUE FOR OLD STYLE CLIPPING               |
        |   LEFT CLIP PIXELS VALUE     |    RIGHT CLIP PIXELS VALUE    |
        +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

        ** NOTE 1:
        DGO      - BIT 15     - DMA GO / DMA HALT
        PIX      - BITS 14-12 - PIXEL SIZE (0 = 8 BITS)
        TM1      - BIT 11     - DMA COMPRESS TRAIL PIX MULT BIT 1
        TM2      - BIT 10     - DMA COMPRESS TRAIL PIX MULT BIT 0
        LM1      - BIT 9      - DMA COMPRESS LEAD  PIX MULT BIT 1
        LM0      - BIT 8      - DMA COMPRESS LEAD  PIX MULT BIT 0
        CMP      - BIT 7      - DMA COMPRESS MODE
        CLP      - BIT 6      - DMA CLIP ON = 1 (USING U,D,L,R METHOD)
        VFL      - BIT 5      - VERTICAL FLIP (FLIP ABOUT X AXIS)
        HFL      - BIT 4      - HORIZONTAL FLIP (FLIP ABOUT Y AXIS)
        OPS      - BITS 3-0   - PIXEL CONSTANT/SUBSTITUTION OPS

        ** NOTE 2: IN COMPRESSION MODE, SCALING IS INHIBITED AND
                   CLIPPING IS INHIBITED.


    DMA # 2 - CLIPPING AN IMAGE

    OVERVIEW: AN IMAGE CAN BE TRANSFERRED TO THE BIT MAP IN ITS
    ENTIRETY OR A PORTION CAN BE "CLIPPED" BY SPECIFYING LEFT AND
    RIGHT CLIP AMOUNTS IN REGISTER 0, WITH BIT 6 (CLP) SET.

    IMPLEMENTATION:
            - THE OFFSET METHOD (OLD STYLE): REGISTER 0 UPPER BYTE =
              LEFT CLIP, LOWER BYTE = RIGHT CLIP.
            - THE REGISTER CLIP METHOD (NEW FEATURE): SET CLP BIT TO 1,
              USE REGISTERS 12 AND 13 FOR WINDOW BORDERS.


    DMA # 2 - TRANSFERRING A SCALED IMAGE

    OVERVIEW: AN IMAGE CAN BE SCALED BY TRAVERSING EACH LINE WITH A
    PREDETERMINED SAMPLE RATE.  RATIO IS 1:(INT + FRAC/256).
    REGISTER 11 = Y SCALE, REGISTER 10 = X SCALE.
    SCALE FACTORS: UPPER BYTE = INTEGER, LOWER BYTE = FRACTION.

    MAXIMUM SCALE FACTOR FOR SHRINK IN X DIRECTION:
    #BITS/PIXEL    INT  FRACTION
    -----------    ---- --------
         1          1F    FF
         2          10    00
         3          0A    AA
         4          08    00
         5          06    66
         6          05    55
         7          04    92
         8          04    00


    DMA # 2 - COMPRESSION OF LEADING AND TRAILING ZEROS

    TO SAVE IMAGE SPACE, LEADING AND TRAILING ZERO PIXELS ARE RUN-
    LENGTH ENCODED. THE FIRST BYTE OF EACH COMPRESSED LINE CONTAINS:
    UPPER NIBBLE = TRAILING ZERO COUNT, LOWER NIBBLE = LEADING ZERO
    COUNT. TMx/LMx BITS IN CONTROL REGISTER MULTIPLY THESE VALUES BY
    1, 2, 4, OR 8.

    IN COMPRESSION MODE, THE DMA DECODES THIS ON THE FLY IF:
      CMP (BIT 7) = 1, TMx, LMx BITS = 0.


    DMA # 2 - OFF SCREEN CLIPPING (WINDOWING)

    FOUR REGISTERS SPECIFY WINDOW BOUNDARIES (0-511).  SET CONFIG
    REGISTER BIT 5 = 0 FOR LEFT/RIGHT, = 1 FOR UPPER/LOWER.
    REGISTERS 12/13 HOLD LEFT/TOP AND RIGHT/BOTTOM LIMITS.


    DMA # 2 - LIMITATIONS

    SCALING: THERE ARE SIZE LIMITS. SEE TABLE ABOVE FOR SHRINK/GROW
    MAX/MIN IN X DIRECTION. 1-BIT PIXELS CAN SHRINK TO 1F.FF (1:31.996).
    COMPRESSION: CLIPPING AND SCALING ARE DISABLED IN COMPRESS MODE.
)STUDIO_HELP"},
    {"Classic shortcuts & migration",R"STUDIO_HELP(Original imgtool shortcuts are retained where Studio has the corresponding operation. Review dialogs replace immediate destructive actions:

Ctrl+Shift+C / X opens extraction review with Copy / Cut selected; Ctrl+Shift+V creates a new sprite from the pixel clipboard in one Undo step.
Ctrl+P opens point-table review, including enable/remove, instead of immediately toggling the stored block.
[ / ] changes brush size with a brush tool, otherwise opens marked / selected palette assignment review. Choose the destination palette in that review. Shift+R opens palette Rename. * opens palette Merge review. Palette manager owns Ctrl+C/V, Delete, Up/Down and Undo/Redo while its dialog is active. It does not change the IMG behind another modal.
; opens a marked-sprite 50% smooth reduction review; review size and quality before Apply. Ctrl+B opens marked TGA sheet export. Alt+L and Ctrl+L open indexed import; Alt+S exports selected LBM.

H opens Help; Shift+H selects Pan. V selects Variant Paint (T is also available); Shift+V selects Move. E selects Smart erase (U also works); Shift+E selects plain Erase. K plays/pauses; Shift+K selects Line. Tooltips and Keyboard reference show these bindings.

Tab switches Artwork / alignment. Ctrl+Tab switches libraries. Tap Space to mark in Artwork; hold Space and left-drag to pan without marking or painting. Space plays in animation workspaces. Studio supports explicit review before deleting sprites with Shift+Delete; Delete/Backspace clear a selected pixel region. Without a selection, Delete opens sprite deletion review. Escape cancels a gesture rather than quitting the application.

The classic floating-paste L command keeps an editable sprite overlay; see Sprite overlay (L) for session and save behavior. Ctrl+Left/Right reorders the temporary name-based preview, stored IMG entries in IMG animations, or project frames in a project. Only the latter two affect their own document histories. In Palette manager, Space marks the selected palette, Shift+M marks all and M clears marks. Select a destination and press * (or keypad multiply) to review all marked sources together.)STUDIO_HELP"},
    {"Palettes & color wheel",R"STUDIO_HELP(The left color wheel edits the selected palette entry. These colors are shared by every sprite using that palette. Index zero is transparent and locked. RGB555 stores 0-31 per channel; UI RGB/HSV/hex controls are quantized to those values. A wheel drag is one Undo step; Escape cancels it.

Palette > Manage palettes has separate staged color edits and a sprite preview. Browsing does not assign palettes. Apply colors changes all users in one Undo step. Reset or close discards unapplied colors. Choose Make palette private to isolate one sprite before recoloring.

Select swatches with click, Ctrl-toggle or Shift-range; All opaque selects every nonzero slot, while None leaves no colors selected. Invert selected previews the RGB555 complement, preserving stored high bits. Reverse selected order reverses complete color words across the selected slot numbers, including nonadjacent slots; it needs at least two selected colors. Both keep index zero locked and leave pixel indices, palette size/depth and assignments unchanged. They can build on current RGB/HSL previews or each other. Repeating either effect twice cancels that effect. Review the shared sprite appearance, then Apply colors for one Undo step; Reset or Close discards the preview. With no selected opaque slots, the effect buttons are disabled.

Ctrl+C copies every marked palette in library order, or the selected palette when nothing is marked. The search filter does not exclude marked palettes. Right-click Copy and choose Copy selected only to ignore marks. Ctrl+V / Paste appends the entire captured set in one Undo step, selects its first new palette, and leaves sprite and alternate assignments unchanged. Existing names are retained when unique; collisions receive numbered names. Colors (including stored high bits), declared depth and palette metadata travel with the capture; raw name tails stay intact unless a collision renames the palette. Clipboard contents survive source edits and closure until replaced, cleared or Studio closes. Failed capture keeps the previous clipboard; failed paste adds nothing. Clear beside Paste releases it without editing the IMG. Pixel, sprite and animation clipboards stay separate. Save separately; the IMG version and layout never change. These keys target palettes while the manager is open. Up/Down browses, Shift+R focuses Rename and Enter applies. Delete removes unused palettes only. Undo/Redo uses this IMG's palette history. Pending color, merge, ramp or usage previews block unrelated palette mutations.

Right-click a swatch or use Copy / paste colors to copy that color or the Ctrl/Shift-selected slots. This separate color clipboard owns RGB555 words and original indices; source edits/closure do not affect it. Original-index paste recolors shared users and can grow the palette with black gaps. Append retains existing colors; every copied color must fit within 256 slots or the whole paste is refused. One copied color can also replace an existing opaque slot. Destination index zero stays unchanged; copy its color to an opaque slot or append it instead. Review the before/after sprite, source-to-target indices, changed slots, shared primary/alternate references, palette growth, depth and LOAD2 PPP impact. Apply records one Undo step without remapping pixels, assignments or metadata; Cancel leaves artwork and clipboard unchanged. Clear copied colors releases only this clipboard. Pending previews block clipboard mutations; a changed library invalidates its review. Save separately in the original IMG format. Ctrl+C/V still targets whole palettes.

Mark palette rows with their checkboxes or Space; Shift+M marks all, M clears marks, and Mark shown respects the search filter. Select a destination, then * / keypad multiply or Merge marked reviews enabled sources together. Destination marks are ignored. Excluding a source leaves its mark intact. With no marks, Merge still reviews one source and destination. All sources use the final combined destination, with explicit approximation approval, one Undo step and per-source skipped/kept/removed summaries. Remove sources that become unused never removes an alternate-referenced palette. Unused marked sources stay unchanged. Marks are local view state and clear when a structural edit or history restore changes palette identities. Merge reports palette growth, changed indices, approximations and LOAD2 PPP impact. Usage / compact reviews unused/duplicate colors with retained references. Shading / gradients stages palette ramps. Approximate matches require review. Palette assignment offers Match colors or Keep indices, with alternate-palette constraints made explicit.)STUDIO_HELP"},
    {"Video capture & digitize",R"STUDIO_HELP(File > Capture video frames opens a frame grabber before digitize review. Scrub with Left/Right (Shift = ten), Home/End for first/last, I/O for In/Out, P to pick individual frames, and Space to play/pause. Keys pause during text entry and capture/probe jobs.

Choose an In/Out range with Every Nth frame or manual Picks. Drag a crop around the actor; right-click places a shared floor point. Wheel zooms, middle-drag pans, and Fit resets framing. Capture and digitize prepares frames without editing the IMG. The digitize review keys the background, seeds materials and fits palette ramps; import the reviewed batch to commit one Undo step.

The decoder runs an ffmpeg executable, found via IMGTOOL_FFMPEG, beside Studio, on PATH or common install folders. It is not linked as an FFmpeg library. Variable-rate source timing may need normalization for exact frame seeking. Cancel stops pending processing at its next safe boundary. Source video is unchanged.)STUDIO_HELP"},
    {"MK2 strike tables",R"STUDIO_HELP(Tools > MK2 strike tables opens a source-backed editor for MKSTK.ASM. Choose a character and move, then inspect its strike fields and preview box. Coordinates, size, routine token, hit/block damage, score and sound belong to the ASM source, not the IMG point table.

Changes are staged and reviewed before source save. Source Undo/Redo and Ctrl+S target this source document while its editor is open. Source histories and save destinations are independent of the IMG. Source reload/close/quit protect applied unsaved changes and saves reject external changes.

Strike associations binds source moves to artwork references for inspection. The strike phase inspector previews phase selections under explicit countdown/flag assumptions. These previews do not simulate startup, hit interruption, attacker/victim timing or game routines. IMG artwork and its binary format are unchanged by source inspection.)STUDIO_HELP"},
    {"Collect animation artwork",R"STUDIO_HELP(In ASM / Reactions, Review artwork inventories every expanded piece symbol. Choose explicitly among ambiguous open-library candidates. Collect artwork freezes resolved sprites and palettes into an owned review that survives source edits or closure. Partial previews require explicit consent.

Create library makes a separate unsaved IMG tab. Copy to existing IMG uses a destination review with prefixes and exclusions and one destination Undo step. It requires the same IMG version/layout; neither library is converted.

Reuse identical named sprites is opt-in. A unique existing name must match pixels, effective stored name bytes, palette contents/metadata, anchors, points, alternates and saved sprite metadata. Exact matches are reused, missing sprites appended, differing or ambiguous matches blocked. If everything is reused, no IMG data or Undo history changes. Prefixes affect copied names, not external ASM operands. No source file is saved automatically.)STUDIO_HELP"},
    {"About",R"STUDIO_HELP(Imgtool Studio is the separate modern editor for Midway arcade IMG artwork. Original imgtool began as Shawn Liptak's Williams Electronics DOS tool, now ported to C/C++, SDL2 and Dear ImGui.

IMG artwork, embedded animations, Studio projects and external ASM sources have distinct save destinations. Inspect previews and export reviews before writing game assets. Studio does not assemble game code or emulate the game runtime.)STUDIO_HELP"},
};
