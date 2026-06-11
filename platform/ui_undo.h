/*************************************************************
 * platform/ui_undo.h
 * Undo/Redo subsystem interfaces.
 *************************************************************/
#ifndef UI_UNDO_H
#define UI_UNDO_H

#include "img_format.h"

struct DocSnapshot;

void ClearDocumentRedoStack(void);
bool doc_undo_push(void);
void DoUndo(void);
void DoRedo(void);

#endif /* UI_UNDO_H */
