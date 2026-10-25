/** @file
  Menu loop: input, countdown and idle rotation.

  Degradation rules (see README):
  - No input device: the timeout always runs (fallback_timeout replaces a
    negative timeout) and the highlight rotates through the entries.
  - Any key press stops both the countdown and the rotation.
  - The countdown always boots the default entry; rotation is display only.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#include "Internal.h"

#define TICK_MS             100
#define DEFAULT_ROTATE_SEC  2

#ifndef SCAN_SUSPEND
#define SCAN_SUSPEND  0x0102
#endif

//
// Physical input = a SimpleTextIn instance that sits on a device path; the
// ConSplitter's virtual handle has none.
//
STATIC
UINTN
CountInputDevices (
  VOID
  )
{
  EFI_STATUS  Status;
  EFI_HANDLE  *Handles;
  UINTN       Count;
  UINTN       Index;
  UINTN       Physical;
  VOID        *Path;

  if (gST->ConIn == NULL) {
    return 0;
  }

  Status = gBS->LocateHandleBuffer (ByProtocol, &gEfiSimpleTextInProtocolGuid, NULL, &Count, &Handles);
  if (EFI_ERROR (Status)) {
    return 0;
  }

  Physical = 0;
  for (Index = 0; Index < Count; Index++) {
    if (!EFI_ERROR (gBS->HandleProtocol (Handles[Index], &gEfiDevicePathProtocolGuid, &Path))) {
      Physical++;
    }
  }

  FreePool (Handles);
  return Physical;
}

STATIC
VOID
DrawCountdown (
  IN UNIXNU_UI      *Ui,
  IN UNIXNU_CONFIG  *Config,
  IN INTN       RemainingMs,
  IN BOOLEAN    Confirmation
  )
{
  CHAR16  Line[128];

  if (RemainingMs >= 0) {
    UnicodeSPrint (
      Line,
      sizeof (Line),
      L"Booting %s in %u s",
      Config->Entries[Config->DefaultIndex].Title,
      (UINT32)((RemainingMs + 999) / 1000)
      );
  } else {
    UnicodeSPrint (Line, sizeof (Line), Confirmation ? L"Choose an option" : L"Select an entry");
  }

  UniXNUUiDrawStatus (Ui, 0, Line);
}

STATIC UINTN
RunMenu (
  IN UNIXNU_UI      *Ui,
  IN UNIXNU_CONFIG  *Config,
  IN BOOLEAN    Countdown,
  IN BOOLEAN    Confirmation
  )
{
  EFI_STATUS     Status;
  EFI_EVENT      Timer;
  EFI_EVENT      Events[2];
  UINTN          EventCount;
  UINTN          Signaled;
  EFI_INPUT_KEY  Key;
  UINTN          Selected;
  UINTN          Previous;
  UINTN          Index;
  BOOLEAN        HasInput;
  BOOLEAN        UserActive;
  INTN           RemainingMs;
  UINTN          RotateMs;
  UINTN          RotateAccum;
  INTN           LastShownSecond;
  UINTN          Count;
  UNIXNU_MENU_POINTER Pointer;

  Count    = Config->EntryCount;
  if (Count == 0 || Config->DefaultIndex >= Count) { return 0; }
  Selected = Config->DefaultIndex;
  UniXNUUiDrawFrame (Ui, Config);
  if (FixedPcdGetBool (PcdUniXNUPointerInputEnabled)) { UniXNUUiSelectEntry (Ui, Config, Selected, Selected); }
  for (Index = 0; Index < Count; Index++) { UniXNUUiDrawEntry (Ui, Config, Index, Index == Selected); }
  HasInput = UniXNUMenuPointerInit (Ui, &Pointer) || (CountInputDevices () > 0);
  UniXNUMenuPointerHide (Ui, &Pointer);

  if (!Countdown) {
    RemainingMs = HasInput ? -1 : (INTN)Config->FallbackTimeout * 1000;
  } else if (Config->Timeout < 0) {
    RemainingMs = HasInput ? -1 : (INTN)Config->FallbackTimeout * 1000;
  } else {
    RemainingMs = Config->Timeout * 1000;
  }

  RotateMs = Config->RotateSeconds * 1000;
  if (!HasInput && (RotateMs == 0)) {
    RotateMs = DEFAULT_ROTATE_SEC * 1000;
  }

  UniXNUUiDrawStatus (
    Ui,
    1,
    Confirmation ? L"Up/Down: choose    Enter: confirm    Esc: cancel    Click: choose"
      : Pointer.Available ? L"Up/Down: select    Enter: boot    Mouse: point and click"
      : HasInput ? L"Up/Down: select    Enter: boot    1-9: boot entry    Esc: stop timer"
             : L"No input device: the default entry boots when the timer ends"
    );
  if (Ui->Notice[0] != L'\0') {
    UniXNUUiDrawStatus (Ui, 2, Ui->Notice);
  }

  UniXNULog (
    "menu: shown %u entries, default %u '%a', timeout %d ms, input %a, rotate %u ms",
    (UINT32)Count,
    (UINT32)Config->DefaultIndex,
    (Config->Entries[Config->DefaultIndex].Id != NULL) ? Config->Entries[Config->DefaultIndex].Id : "?",
    (INT32)RemainingMs,
    HasInput ? "yes" : "no",
    (UINT32)RotateMs
    );

  if (RemainingMs == 0) {
    UniXNULog ("menu: timeout 0, booting default");
    UniXNUMenuPointerClose (Ui, &Pointer);
    return Config->DefaultIndex;
  }

  Status = gBS->CreateEvent (EVT_TIMER, TPL_CALLBACK, NULL, NULL, &Timer);
  if (EFI_ERROR (Status)) {
    UniXNUMenuPointerClose (Ui, &Pointer);
    return Config->DefaultIndex;
  }

  Status = gBS->SetTimer (Timer, TimerPeriodic, EFI_TIMER_PERIOD_MILLISECONDS (TICK_MS));
  if (EFI_ERROR (Status)) { gBS->CloseEvent (Timer); UniXNUMenuPointerClose (Ui, &Pointer); return Config->DefaultIndex; }

  EventCount = 0;
  Events[EventCount++] = Timer;
  if (gST->ConIn != NULL) {
    Events[EventCount++] = gST->ConIn->WaitForKey;
  }

  UserActive      = FALSE;
  RotateAccum     = 0;
  LastShownSecond = -2;
  if (Pointer.Enabled) {
    UINTN Left = Ui->Portrait ? Ui->Margin : Ui->MenuLeft;
    UINTN Width = Ui->Portrait ? Ui->Width - 2 * Ui->Margin : Ui->MenuWidth;
    UINTN Height = Ui->Portrait ? Ui->CardHeight : Ui->RowHeight - Ui->Scale;
    UniXNULog ("menu: pointer-ready x=%u y=%u step=%u rows=%u first=%u cx=%u cy=%u confirm=%u",
      (UINT32)(Left + Width / 2), (UINT32)(Ui->MenuTop + Height / 2), (UINT32)Ui->RowHeight,
      (UINT32)Ui->VisibleRows, (UINT32)Ui->FirstVisible, (UINT32)Pointer.X, (UINT32)Pointer.Y, Confirmation);
  }
  for ( ; ;) {
    INTN  Second = (RemainingMs < 0) ? -1 : (RemainingMs + 999) / 1000;

    if (Second != LastShownSecond) {
      UniXNUMenuPointerHide (Ui, &Pointer);
      DrawCountdown (Ui, Config, RemainingMs, Confirmation);
      LastShownSecond = Second;
    }

    UniXNUMenuPointerDraw (Ui, &Pointer);
    Status = gBS->WaitForEvent (EventCount, Events, &Signaled);
    UniXNUMenuPointerHide (Ui, &Pointer);
    if (EFI_ERROR (Status)) {
      break;
    }

    Previous = Selected;
    UINTN Hover, Click; INT32 Scroll;
    BOOLEAN MouseActive = UniXNUMenuPointerPoll (Ui, Count, &Pointer, &Hover, &Click, &Scroll);
    BOOLEAN KeyActive = gST->ConIn != NULL && !EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key));
    if (KeyActive || MouseActive) {
      if (!UserActive) {
        UserActive = TRUE; RemainingMs = -1; Selected = Config->DefaultIndex;
      }
      if (MouseActive) {
        if (Scroll > 0) { Selected = (Selected + Count - 1) % Count; }
        else if (Scroll < 0) { Selected = (Selected + 1) % Count; }
        else if (Hover != MAX_UINTN) { Selected = Hover; }
        if (Click != MAX_UINTN) {
          Selected = Click; UniXNULog ("menu: click, choosing %u confirm=%u", (UINT32)Selected, Confirmation); break;
        }
      }
    }
    if (KeyActive) {
      if (Confirmation) {
        if (Key.ScanCode == SCAN_UP) { Selected = (Selected + Count - 1) % Count; }
        else if (Key.ScanCode == SCAN_DOWN) { Selected = (Selected + 1) % Count; }
        else if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN || Key.UnicodeChar == CHAR_LINEFEED) { break; }
        else { Selected = Config->DefaultIndex; break; }
      } else if ((Key.ScanCode == SCAN_UP) || (Key.UnicodeChar == L'k')) {
        Selected = (Selected + Count - 1) % Count;
      } else if ((Key.ScanCode == SCAN_DOWN) || (Key.UnicodeChar == L'j')) {
        Selected = (Selected + 1) % Count;
      } else if (Key.ScanCode == SCAN_HOME) {
        Selected = 0;
      } else if (Key.ScanCode == SCAN_END) {
        Selected = Count - 1;
      } else if ((Key.UnicodeChar >= L'1') && (Key.UnicodeChar <= L'9') && ((UINTN)(Key.UnicodeChar - L'1') < Count)) {
        Selected = Key.UnicodeChar - L'1';
        UniXNULog ("menu: key %c, booting %u", Key.UnicodeChar, (UINT32)Selected); break;
      } else if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.UnicodeChar == CHAR_LINEFEED) ||
                 (Key.UnicodeChar == L' ') || (Key.ScanCode == SCAN_RIGHT) || (Key.ScanCode == SCAN_SUSPEND)) {
        UniXNULog ("menu: enter, booting %u", (UINT32)Selected); break;
      }
    } else if (!MouseActive && Signaled == 0) {
      if (RemainingMs > 0) {
        RemainingMs -= TICK_MS;
        if (RemainingMs <= 0) {
          UniXNULog ("menu: timeout, booting default %u", (UINT32)Config->DefaultIndex);
          Selected = Config->DefaultIndex;
          break;
        }
      }

      if (!UserActive && (RotateMs > 0) && (Count > 1)) {
        RotateAccum += TICK_MS;
        if (RotateAccum >= RotateMs) {
          RotateAccum = 0;
          Selected    = (Selected + 1) % Count;
        }
      }
    }

    if (Selected != Previous) {
      if (FixedPcdGetBool (PcdUniXNUPointerInputEnabled)) { UniXNUUiSelectEntry (Ui, Config, Previous, Selected); }
      else { UniXNUUiDrawEntry (Ui, Config, Previous, FALSE); UniXNUUiDrawEntry (Ui, Config, Selected, TRUE); }
    }
  }

  UniXNUMenuPointerClose (Ui, &Pointer);
  gBS->SetTimer (Timer, TimerCancel, 0);
  gBS->CloseEvent (Timer);
  return Selected;
}

UINTN UniXNUMenuRun (UNIXNU_UI *Ui, UNIXNU_CONFIG *Config, BOOLEAN Countdown) {
  return RunMenu (Ui, Config, Countdown, FALSE);
}

BOOLEAN
UniXNUMenuConfirm (
  IN  UNIXNU_UI      *Ui,
  IN  UNIXNU_ENTRY   *Entry,
  OUT UINTN      *Choice
  )
{
  EFI_INPUT_KEY  Key;
  UINTN          Index;
  UINTN          Shown;
  UINTN          Items;
  CHAR16         Line[160];

  *Choice = 0;
  if (Entry->ConfirmCount > 0 && Entry->ConfirmTitles == NULL) { return FALSE; }
  UNIXNU_MENU_POINTER Pointer;
  BOOLEAN Mouse = UniXNUMenuPointerInit (Ui, &Pointer);
  UniXNUMenuPointerHide (Ui, &Pointer);
  if (Pointer.Enabled && (Mouse || CountInputDevices () > 0)) {
    UINTN Count = Entry->ConfirmCount > 0 ? Entry->ConfirmCount : 1;
    if (Count >= MAX_UINTN / sizeof (UNIXNU_ENTRY) - 1) { UniXNUMenuPointerClose (Ui, &Pointer); return FALSE; }
    UNIXNU_ENTRY *Entries = AllocateZeroPool ((Count + 1) * sizeof *Entries);
    if (Entries == NULL) { UniXNUMenuPointerClose (Ui, &Pointer); return FALSE; }
    for (UINTN I = 0; I < Count; I++) {
      Entries[I].Title = Entry->ConfirmCount > 0 ? Entry->ConfirmTitles[I] : L"Confirm"; Entries[I].Type = UniXNUEntryChoice;
    }
    Entries[Count].Title = L"Cancel"; Entries[Count].Type = UniXNUEntryChoice;
    UNIXNU_CONFIG Config; ZeroMem (&Config, sizeof Config);
    Config.Title = Entry->Confirm; Config.SourceLabel = L"Confirmation"; Config.Entries = Entries;
    Config.EntryCount = Count + 1; Config.DefaultIndex = Count; Config.Timeout = -1; Config.FallbackTimeout = 1;
    UNIXNU_UI Question = *Ui; Question.Info[0] = Question.Notice[0] = L'\0';
    // Drain the entry-selecting key/press; the new pointer context still needs
    // its own observed release before accepting a fresh complete click.
    gBS->Stall (500000);
    if (gST->ConIn != NULL) { while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {} }
    UINTN Hover, Click; INT32 Scroll;
    UniXNUMenuPointerPoll (Ui, 0, &Pointer, &Hover, &Click, &Scroll);
    UniXNUMenuPointerClose (Ui, &Pointer);
    UINTN Selected = RunMenu (&Question, &Config, FALSE, TRUE);
    FreePool (Entries);
    if (Selected >= Count) { return FALSE; }
    *Choice = Selected;
    UniXNULog ("menu: confirmed '%a' choice %u", Entry->Id != NULL ? Entry->Id : "?", (UINT32)*Choice);
    return TRUE;
  }
  UniXNUMenuPointerClose (Ui, &Pointer);
  if ((gST->ConIn == NULL) || (CountInputDevices () == 0)) {
    UniXNULog ("menu: confirmation needs an input device, cancelled");
    return FALSE;
  }

  //
  // With choices the items are the choices and then Cancel, and Cancel is shown first, so the first press cannot confirm anything.
  //
  Items = (Entry->ConfirmCount > 0) ? Entry->ConfirmCount + 1 : 0;
  Shown = Items > 0 ? Items - 1 : 0;

  UnicodeSPrint (Line, sizeof (Line), L"%s", Entry->Confirm);
  UniXNUUiDrawStatus (Ui, 0, Line);
  UniXNUUiDrawStatus (Ui, 2, Items > 0 ? L"Volume keys: choose     Power key: confirm" : L"Power key = confirm     any other key = cancel");

  //
  // The press that chose the entry, and anything typed since, must not answer the question.
  //
  gBS->Stall (500000);
  while (!EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
  }

  for ( ; ;) {
    if (Items > 0) {
      UnicodeSPrint (Line, sizeof (Line), L"<  %s  >", (Shown + 1 == Items) ? L"Cancel" : Entry->ConfirmTitles[Shown]);
      UniXNUUiDrawStatus (Ui, 1, Line);
    } else {
      UniXNUUiDrawStatus (Ui, 1, L"");
    }

    if (EFI_ERROR (gBS->WaitForEvent (1, &gST->ConIn->WaitForKey, &Index))) {
      return FALSE;
    }

    if (EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Key))) {
      continue;
    }

    if (Items > 0) {
      if (Key.ScanCode == SCAN_UP) {
        Shown = (Shown + Items - 1) % Items;
        continue;
      }

      if (Key.ScanCode == SCAN_DOWN) {
        Shown = (Shown + 1) % Items;
        continue;
      }
    }

    if ((Key.UnicodeChar == CHAR_CARRIAGE_RETURN) || (Key.UnicodeChar == CHAR_LINEFEED)) {
      if (Items > 0) {
        if (Shown + 1 == Items) {
          return FALSE;
        }

        *Choice = Shown;
      }

      UniXNULog ("menu: confirmed '%a' choice %u", (Entry->Id != NULL) ? Entry->Id : "?", (UINT32)*Choice);
      return TRUE;
    }

    return FALSE;
  }
}
