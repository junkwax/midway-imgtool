# Current Status: Adobe Features

All proposed Adobe-style creative tools from `ADOBE_FEATURES_PROPOSAL.md` are now in.

## Implemented Features
1. **Auto-Sprite Chopper (Smart Partitioning)** — Operations menu. Slices marked images into a grid, trims empty space, recalculates ANIX/ANIY.
2. **Magic Wand Tool (`W`)** — Selects contiguous blocks of pixels of the same color.
3. **Lasso Tool (`L`)** — Freehand selection.
4. **Background Eraser Tool (`E`)** — Replaces selected color with transparency.
5. **Clone Stamp Tool (`C`)** — `Alt+Click` to anchor, then click and drag to clone pixels.
6. **Smart Palette Remapper (`Rm`)** — Paint over an image to replace specific colors.
7. **Animation Timeline & Playback** — Bottom panel with Play/Stop, FPS slider, and a horizontal frame strip.
8. **Drag-and-Drop Timeline Reordering** — Drag frame buttons in the timeline strip to reorder. Playhead index is fixed up across the move. "Reset Sequence" rebuilds the default order from marked frames. Stale indices are pruned automatically when the file changes.
9. **Smart Alignment ("Snap to Content")** — Hold `Shift` while dragging a pasted floating selection to snap each edge of the paste rect to the non-transparent content bbox of the underlying sprite (5px threshold, all four sides per axis).

## Next Steps
Feature set from the proposal is complete. Open the door to the next round of work (additional tooling, polish, or whatever's next on the roadmap).
