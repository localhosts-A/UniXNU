/** @file
  Opt-in graphics-menu pointer input. Protocols are reacquired on every poll so
  a USB detach never leaves a cached driver pointer/event. No protocol Reset:
  only an observed button-up can arm a new press/release on the same entry.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#include "Internal.h"

STATIC VOID EFIAPI PointerChanged (EFI_EVENT Event, VOID *Context) {
  (VOID)Event; ((UNIXNU_MENU_POINTER *)Context)->Changed = TRUE;
}

STATIC BOOLEAN ValidUi (UNIXNU_UI *Ui) {
  return Ui->Mode == UniXNUUiGraphics && Ui->Gop != NULL && Ui->Gop->Blt != NULL &&
    Ui->Gop->Mode != NULL && Ui->Gop->Mode->Info != NULL && Ui->Width > 0 && Ui->Height > 0 &&
    Ui->Width == Ui->Gop->Mode->Info->HorizontalResolution && Ui->Height == Ui->Gop->Mode->Info->VerticalResolution;
}

STATIC EFI_SIMPLE_POINTER_PROTOCOL *FindPointer (UNIXNU_MENU_POINTER *Pointer, EFI_HANDLE *Owner) {
  EFI_HANDLE *Handles; UINTN Count; EFI_SIMPLE_POINTER_PROTOCOL *Found = NULL;
  *Owner = NULL;
  if (EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiSimplePointerProtocolGuid, NULL, &Count, &Handles))) { return NULL; }
  for (UINTN I = 0; I < Count; I++) {
    EFI_SIMPLE_POINTER_PROTOCOL *Protocol; VOID *Path;
    // Skip console splitters; consume one physical input stream, never both.
    if (EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiDevicePathProtocolGuid, &Path)) || Path == NULL ||
        EFI_ERROR (gBS->HandleProtocol (Handles[I], &gEfiSimplePointerProtocolGuid, (VOID **)&Protocol)) ||
        Protocol->GetState == NULL || Protocol->Mode == NULL || !Protocol->Mode->LeftButton ||
        Protocol->Mode->ResolutionX == 0 || Protocol->Mode->ResolutionY == 0) { continue; }
    if (Found == NULL || Handles[I] == Pointer->Owner) { Found = Protocol; *Owner = Handles[I]; }
    if (Handles[I] == Pointer->Owner) { break; }
  }
  FreePool (Handles); return Found;
}

UINTN UniXNUUiHitTest (UNIXNU_UI *Ui, UINTN Count, UINTN X, UINTN Y) {
  if (!ValidUi (Ui) || Ui->RowHeight == 0 || Ui->FirstVisible >= Count || Y < Ui->MenuTop ||
      X >= Ui->Width || Y >= Ui->Height) { return MAX_UINTN; }
  UINTN Left = Ui->Portrait ? Ui->Margin : Ui->MenuLeft;
  if (Ui->Portrait && Ui->Margin > Ui->Width / 2) { return MAX_UINTN; }
  UINTN Width = Ui->Portrait ? Ui->Width - 2 * Ui->Margin : Ui->MenuWidth;
  if (X < Left || X - Left >= Width) { return MAX_UINTN; }
  UINTN Row = (Y - Ui->MenuTop) / Ui->RowHeight;
  UINTN Height = Ui->Portrait ? Ui->CardHeight : (Ui->RowHeight > Ui->Scale ? Ui->RowHeight - Ui->Scale : 0);
  if (Row >= Ui->VisibleRows || Row >= Count - Ui->FirstVisible || (Y - Ui->MenuTop) % Ui->RowHeight >= Height) { return MAX_UINTN; }
  return Ui->FirstVisible + Row;
}

VOID UniXNUUiSelectEntry (UNIXNU_UI *Ui, UNIXNU_CONFIG *Config, UINTN Previous, UINTN Selected) {
  if (Selected >= Config->EntryCount || Ui->VisibleRows == 0) { return; }
  UINTN First = Ui->FirstVisible;
  if (Selected < First) { Ui->FirstVisible = Selected; }
  else if (Selected - First >= Ui->VisibleRows) { Ui->FirstVisible = Selected - Ui->VisibleRows + 1; }
  if (First != Ui->FirstVisible) {
    UINTN Rows = MIN (Ui->VisibleRows, Config->EntryCount - Ui->FirstVisible);
    for (UINTN I = 0; I < Rows; I++) { UniXNUUiDrawEntry (Ui, Config, Ui->FirstVisible + I, Ui->FirstVisible + I == Selected); }
  } else if (Selected != Previous) {
    UniXNUUiDrawEntry (Ui, Config, Previous, FALSE); UniXNUUiDrawEntry (Ui, Config, Selected, TRUE);
  }
}

VOID UniXNUMenuPointerHide (UNIXNU_UI *Ui, UNIXNU_MENU_POINTER *Pointer) {
  if (!Pointer->Drawn) { return; }
  Pointer->Drawn = FALSE;
  if (!ValidUi (Ui) || EFI_ERROR (Ui->Gop->Blt (Ui->Gop, Pointer->Saved, EfiBltBufferToVideo,
        0, 0, Pointer->SavedX, Pointer->SavedY, Pointer->SavedWidth, Pointer->SavedHeight,
        UNIXNU_POINTER_SIDE * sizeof (Pointer->Saved[0])))) {
    Pointer->Enabled = Pointer->Available = FALSE; Pointer->Armed = MAX_UINTN;
  }
}

VOID UniXNUMenuPointerDraw (UNIXNU_UI *Ui, UNIXNU_MENU_POINTER *Pointer) {
  if (!Pointer->Enabled || !Pointer->Available || Pointer->Drawn) { return; }
  if (!ValidUi (Ui) || Pointer->X >= Ui->Width || Pointer->Y >= Ui->Height) {
    Pointer->Enabled = Pointer->Available = FALSE; return;
  }
  UINTN W = MIN ((UINTN)UNIXNU_POINTER_SIDE, Ui->Width - Pointer->X);
  UINTN H = MIN ((UINTN)UNIXNU_POINTER_SIDE, Ui->Height - Pointer->Y);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL Image[UNIXNU_POINTER_SIDE * UNIXNU_POINTER_SIDE];
  UINTN Delta = UNIXNU_POINTER_SIDE * sizeof (Image[0]);
  EFI_STATUS Status = Ui->Gop->Blt (Ui->Gop, Pointer->Saved, EfiBltVideoToBltBuffer, Pointer->X, Pointer->Y, 0, 0, W, H, Delta);
  if (EFI_ERROR (Status)) { Pointer->Enabled = Pointer->Available = FALSE; return; }
  CopyMem (Image, Pointer->Saved, sizeof Image);
  for (UINTN Y = 0; Y < H; Y++) {
    for (UINTN X = 0; X < W; X++) {
      BOOLEAN Head = Y < 12 && X <= Y / 2;
      BOOLEAN Tail = Y >= 10 && Y < 16 && X >= 4 && X <= 6;
      if (Head || Tail) {
        UINT8 Value = ((Head && X > 0 && X < Y / 2 && Y < 11) || (Tail && X == 5)) ? 255 : 0;
        Image[Y * UNIXNU_POINTER_SIDE + X] = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL){Value, Value, Value, 0};
      }
    }
  }
  Pointer->SavedX = Pointer->X; Pointer->SavedY = Pointer->Y; Pointer->SavedWidth = W; Pointer->SavedHeight = H;
  // Retain the captured rectangle even after an uncertain drawing failure.
  Pointer->Drawn = TRUE;
  Status = Ui->Gop->Blt (Ui->Gop, Image, EfiBltBufferToVideo, 0, 0, Pointer->X, Pointer->Y, W, H, Delta);
  if (EFI_ERROR (Status)) { UniXNUMenuPointerHide (Ui, Pointer); Pointer->Enabled = Pointer->Available = FALSE; }
}

BOOLEAN UniXNUMenuPointerInit (UNIXNU_UI *Ui, UNIXNU_MENU_POINTER *Pointer) {
  ZeroMem (Pointer, sizeof *Pointer); Pointer->Armed = MAX_UINTN;
  Pointer->Enabled = FixedPcdGetBool (PcdUniXNUPointerInputEnabled) && ValidUi (Ui);
  if (!Pointer->Enabled) { return FALSE; }
  EFI_STATUS Status = gBS->CreateEvent (EVT_NOTIFY_SIGNAL, TPL_CALLBACK, PointerChanged, Pointer, &Pointer->NotifyEvent);
  VOID *Registration;
  if (!EFI_ERROR (Status)) { Status = gBS->RegisterProtocolNotify (&gEfiSimplePointerProtocolGuid, Pointer->NotifyEvent, &Registration); }
  if (EFI_ERROR (Status)) {
    if (Pointer->NotifyEvent != NULL) { gBS->CloseEvent (Pointer->NotifyEvent); Pointer->NotifyEvent = NULL; }
    Pointer->Enabled = FALSE; return FALSE;
  }
  Pointer->X = Ui->Width / 2; Pointer->Y = Ui->Height / 2;
  EFI_HANDLE Owner;
  Pointer->Available = FindPointer (Pointer, &Owner) != NULL;
  Pointer->Owner = Owner;
  UniXNUMenuPointerDraw (Ui, Pointer);
  return Pointer->Enabled && Pointer->Available;
}

VOID UniXNUMenuPointerClose (UNIXNU_UI *Ui, UNIXNU_MENU_POINTER *Pointer) {
  UniXNUMenuPointerHide (Ui, Pointer);
  if (Pointer->NotifyEvent != NULL) { gBS->CloseEvent (Pointer->NotifyEvent); Pointer->NotifyEvent = NULL; }
  Pointer->Enabled = Pointer->Available = FALSE; Pointer->Owner = NULL; Pointer->Armed = MAX_UINTN;
}

BOOLEAN UniXNUMenuPointerPoll (UNIXNU_UI *Ui, UINTN Count, UNIXNU_MENU_POINTER *Pointer, UINTN *Hover, UINTN *Click, INT32 *Scroll) {
  *Hover = *Click = MAX_UINTN; *Scroll = 0;
  if (!Pointer->Enabled || !ValidUi (Ui)) { Pointer->Available = FALSE; return FALSE; }
  EFI_HANDLE Owner;
  EFI_SIMPLE_POINTER_PROTOCOL *Protocol = FindPointer (Pointer, &Owner);
  if (Owner != Pointer->Owner || Protocol == NULL || Pointer->Changed) {
    Pointer->Owner = Owner; Pointer->SeenUp = Pointer->Down = FALSE; Pointer->Armed = MAX_UINTN;
    Pointer->Changed = FALSE;
  }
  Pointer->Available = Protocol != NULL;
  if (Protocol == NULL) { return FALSE; }
  EFI_SIMPLE_POINTER_STATE State;
  EFI_STATUS Status = Protocol->GetState (Protocol, &State);
  if (Pointer->Changed) { Pointer->SeenUp = Pointer->Down = FALSE; Pointer->Armed = MAX_UINTN; Pointer->Changed = FALSE; }
  if (EFI_ERROR (Status)) {
    if (Status != EFI_NOT_READY) { Pointer->SeenUp = Pointer->Down = FALSE; Pointer->Armed = MAX_UINTN; }
    return FALSE;
  }
  BOOLEAN Active = State.RelativeMovementX != 0 || State.RelativeMovementY != 0 || State.RelativeMovementZ != 0 || State.LeftButton != Pointer->Down;
  // One report count is one pixel at scale1; cap gain and use wide signed math.
  INT64 Gain = (INT64)MIN (MAX (Ui->Scale, (UINTN)1), (UINTN)4);
  INT64 X = (INT64)Pointer->X + (INT64)State.RelativeMovementX * Gain;
  INT64 Y = (INT64)Pointer->Y + (INT64)State.RelativeMovementY * Gain;
  Pointer->X = X < 0 ? 0 : (UINTN)MIN ((UINT64)X, (UINT64)Ui->Width - 1);
  Pointer->Y = Y < 0 ? 0 : (UINTN)MIN ((UINT64)Y, (UINT64)Ui->Height - 1);
  UINTN Hit = UniXNUUiHitTest (Ui, Count, Pointer->X, Pointer->Y);
  if (Active) { *Hover = Hit; }
  *Scroll = State.RelativeMovementZ;
  if (State.LeftButton) {
    if (!Pointer->Down && Pointer->SeenUp) { Pointer->Armed = Hit; }
    else if (Pointer->Armed != Hit) { Pointer->Armed = MAX_UINTN; }
  } else {
    if (Pointer->Down && Pointer->Armed != MAX_UINTN && Pointer->Armed == Hit) { *Click = Hit; }
    Pointer->Armed = MAX_UINTN; Pointer->SeenUp = TRUE;
  }
  Pointer->Down = State.LeftButton;
  return Active;
}
