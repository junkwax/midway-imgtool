/*************************************************************
 * platform/ui_ramp_transfer.h
 * "Transfer to Palette by Ramps..." dialog: move sprites onto another
 * palette whose ramps sit in different slots (MK3 art onto an MK2 palette),
 * pairing ramp-to-ramp so no color jumps materials. Mapping logic lives in
 * palette_transfer.h; this file is the UI and the undoable commit.
 *************************************************************/
#ifndef UI_RAMP_TRANSFER_H
#define UI_RAMP_TRANSFER_H

/* Open with the selected sprite's palette as the source. The target defaults
   to the first marked palette other than the source, else the selected
   palette when it differs. */
void OpenRampTransferDialog(void);

/* Draw it. Safe to call every frame; does nothing while hidden. */
void DrawRampTransferDialog(void);

#endif /* UI_RAMP_TRANSFER_H */
