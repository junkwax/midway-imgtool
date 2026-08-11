/*************************************************************
 * platform/ui_stamp_erase.h
 * "Erase Copied Object" dialog: locate the clipboard's art inside other
 * frames by its own pixels and subtract it from each one.
 *************************************************************/
#ifndef UI_STAMP_ERASE_H
#define UI_STAMP_ERASE_H

/* True when the clipboard holds something worth searching for — the menu item
   is disabled otherwise. */
bool StampEraseAvailable(void);

void OpenStampEraseDialog(void);
void DrawStampEraseDialog(void);

#endif /* UI_STAMP_ERASE_H */
