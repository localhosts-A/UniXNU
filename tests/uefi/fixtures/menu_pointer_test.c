// SPDX-License-Identifier: BSD-2-Clause-Patent
// Real EDK2 ABI + production Menu.c/MenuPointer.c. GOP/Boot Services are models.
#include "Internal.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

EFI_GUID gEfiSimplePointerProtocolGuid = {.Data1=1}, gEfiDevicePathProtocolGuid = {.Data1=2}, gEfiSimpleTextInProtocolGuid = {.Data1=3};
EFI_BOOT_SERVICES *gBS; EFI_SYSTEM_TABLE *gST;
static EFI_BOOT_SERVICES Services; static EFI_SYSTEM_TABLE System;
enum { Width = 320, Height = 240 };
static struct { UINT64 Before; EFI_GRAPHICS_OUTPUT_BLT_PIXEL Pixels[Width * Height]; UINT64 After; } Video;
static EFI_GRAPHICS_OUTPUT_PROTOCOL Gop;
static EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE GopMode;
static EFI_GRAPHICS_OUTPUT_MODE_INFORMATION Info;
static EFI_SIMPLE_POINTER_PROTOCOL Mouse;
static EFI_SIMPLE_POINTER_MODE MouseMode;
static EFI_SIMPLE_TEXT_INPUT_PROTOCOL Keyboard;
static EFI_DEVICE_PATH_PROTOCOL Path;
static BOOLEAN Present, Keys, HaveMouse, HaveKey, ApplyError;
static UINTN Reads, Blts, FailBlt, Step, Steps, EntriesDrawn;
static EFI_STATUS MouseError;
static EFI_STATUS CreateError, RegisterError;
static EFI_EVENT_NOTIFY WatchNotify;
static VOID *WatchContext;
static BOOLEAN WatchActive, ChangeDuringRead;
static EFI_SIMPLE_POINTER_STATE State;
static EFI_INPUT_KEY Key;
typedef struct { BOOLEAN Mouse, Down; INT32 X, Y, Z; UINT16 Scan; CHAR16 Char; } INPUT;
static INPUT Script[100];
#define PTR(n) ((EFI_HANDLE)(UINTN)(n))

VOID *EFIAPI AllocatePool (UINTN Size) { return malloc (Size); }
VOID *EFIAPI AllocateZeroPool (UINTN Size) { return calloc (1, Size); }
VOID EFIAPI FreePool (VOID *Value) { free (Value); }
VOID *EFIAPI CopyMem (VOID *To, CONST VOID *From, UINTN Size) { return memcpy (To, From, Size); }
VOID *EFIAPI ZeroMem (VOID *To, UINTN Size) { return memset (To, 0, Size); }
UINT64 EFIAPI MultU64x32 (UINT64 A, UINT32 B) { return A * B; }
UINTN EFIAPI UnicodeSPrint (CHAR16 *Buffer, UINTN Size, CONST CHAR16 *Format, ...) {
  (VOID)Format; if (Size >= sizeof *Buffer) { Buffer[0] = 0; } return 0;
}
VOID EFIAPI UniXNULog (CONST CHAR8 *Format, ...) { (VOID)Format; }
static EFI_STATUS EFIAPI Locate (EFI_LOCATE_SEARCH_TYPE Type, EFI_GUID *Guid, VOID *Search, UINTN *Count, EFI_HANDLE **Handles) {
  (VOID)Search; assert (Type == ByProtocol);
  *Count = Guid == &gEfiSimplePointerProtocolGuid ? (Present ? 2U : 1U) : (Keys ? 1U : 0U);
  if (*Count == 0) { return EFI_NOT_FOUND; }
  *Handles = AllocatePool (*Count * sizeof **Handles); assert (*Handles);
  if (Guid == &gEfiSimplePointerProtocolGuid) {
    (*Handles)[0] = PTR (1); if (Present) { (*Handles)[1] = PTR (2); }
  } else { assert (Guid == &gEfiSimpleTextInProtocolGuid); (*Handles)[0] = PTR (3); }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Protocol (EFI_HANDLE Handle, EFI_GUID *Guid, VOID **Out) {
  if (Guid == &gEfiDevicePathProtocolGuid && (Handle == PTR (3) || (Present && Handle == PTR (2)))) { *Out = &Path; return EFI_SUCCESS; }
  if (Guid == &gEfiSimplePointerProtocolGuid && Present && Handle == PTR (2)) { *Out = &Mouse; return EFI_SUCCESS; }
  return EFI_UNSUPPORTED;
}
static EFI_STATUS EFIAPI GetMouse (EFI_SIMPLE_POINTER_PROTOCOL *This, EFI_SIMPLE_POINTER_STATE *Out) {
  assert (This == &Mouse && Present); Reads++;
  if (ChangeDuringRead) { assert (WatchActive); WatchNotify (PTR (6), WatchContext); ChangeDuringRead = FALSE; }
  if (EFI_ERROR (MouseError)) { return MouseError; }
  if (!HaveMouse) { return EFI_NOT_READY; }
  *Out = State; HaveMouse = FALSE; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI GetKey (EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Out) {
  assert (This == &Keyboard && Keys);
  if (!HaveKey) { return EFI_NOT_READY; }
  *Out = Key; HaveKey = FALSE; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Create (UINT32 Type, EFI_TPL Tpl, EFI_EVENT_NOTIFY Notify, VOID *Context, EFI_EVENT *Event) {
  assert (Tpl == TPL_CALLBACK);
  if (Type == EVT_NOTIFY_SIGNAL) {
    if (EFI_ERROR (CreateError)) { *Event = NULL; return CreateError; }
    assert (!WatchActive && Notify && Context); WatchActive = TRUE; WatchNotify = Notify; WatchContext = Context; *Event = PTR (6);
  } else { assert (Type == EVT_TIMER && Notify == NULL && Context == NULL); *Event = PTR (4); }
  return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Register (EFI_GUID *Guid, EFI_EVENT Event, VOID **Token) {
  assert (Guid == &gEfiSimplePointerProtocolGuid && Event == PTR (6) && WatchActive);
  *Token = PTR (7); return RegisterError;
}
static EFI_STATUS EFIAPI Timer (EFI_EVENT Event, EFI_TIMER_DELAY Type, UINT64 Time) {
  (VOID)Time; assert (Event == PTR (4) && (Type == TimerPeriodic || Type == TimerCancel)); return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Close (EFI_EVENT Event) {
  if (Event == PTR (6)) { assert (WatchActive); WatchActive = FALSE; WatchNotify = NULL; WatchContext = NULL; }
  else { assert (Event == PTR (4)); } return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Stall (UINTN Time) { assert (Time <= 500000); return EFI_SUCCESS; }
static EFI_STATUS EFIAPI Wait (UINTN Count, EFI_EVENT *Events, UINTN *Index) {
  assert (Count > 0 && Count <= 2 && Events[0] == PTR (4));
  if (Step == Steps) { return EFI_ABORTED; }
  INPUT *Input = &Script[Step++];
  State = (EFI_SIMPLE_POINTER_STATE){Input->X, Input->Y, Input->Z, Input->Down, FALSE}; HaveMouse = Input->Mouse;
  Key = (EFI_INPUT_KEY){Input->Scan, Input->Char}; HaveKey = Input->Scan != 0 || Input->Char != 0;
  *Index = 0; return EFI_SUCCESS;
}
static EFI_STATUS EFIAPI Blt (EFI_GRAPHICS_OUTPUT_PROTOCOL *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Buffer,
  EFI_GRAPHICS_OUTPUT_BLT_OPERATION Op, UINTN Sx, UINTN Sy, UINTN Dx, UINTN Dy, UINTN W, UINTN H, UINTN Delta) {
  assert (This == &Gop && Op != EfiBltVideoToVideo); Blts++;
  BOOLEAN Fail = FailBlt && Blts == FailBlt;
  if (Fail && !ApplyError) { return EFI_DEVICE_ERROR; }
  UINTN Pitch = Delta ? Delta / sizeof *Buffer : W;
  for (UINTN Y = 0; Y < H; Y++) {
    for (UINTN X = 0; X < W; X++) {
      if (Op == EfiBltVideoToBltBuffer) {
        assert (Sx + X < Width && Sy + Y < Height && Dx + X < Pitch);
        Buffer[(Dy + Y) * Pitch + Dx + X] = Video.Pixels[(Sy + Y) * Width + Sx + X];
      } else {
        assert (Dx + X < Width && Dy + Y < Height);
        Video.Pixels[(Dy + Y) * Width + Dx + X] = Op == EfiBltVideoFill ? Buffer[0] : Buffer[(Sy + Y) * Pitch + Sx + X];
      }
    }
  }
  assert (Video.Before == 0x12345678 && Video.After == 0x87654321);
  return Fail ? EFI_DEVICE_ERROR : EFI_SUCCESS;
}
VOID UniXNUUiDrawFrame (UNIXNU_UI *Ui, UNIXNU_CONFIG *Config) {
  Ui->MenuTop = 40; Ui->MenuLeft = 32; Ui->MenuWidth = 200; Ui->RowHeight = 30; Ui->CardHeight = 26;
  Ui->Margin = 20; Ui->FirstVisible = 0; Ui->VisibleRows = MIN (Config->EntryCount, (UINTN)2);
}
VOID UniXNUUiDrawEntry (UNIXNU_UI *Ui, UNIXNU_CONFIG *Config, UINTN Index, BOOLEAN Selected) {
  (VOID)Selected; assert (Index < Config->EntryCount);
  if (Index >= Ui->FirstVisible && Index - Ui->FirstVisible < Ui->VisibleRows) { EntriesDrawn |= (UINTN)1 << Index; }
}
VOID UniXNUUiDrawStatus (UNIXNU_UI *Ui, UINTN Line, CONST CHAR16 *Text) { (VOID)Ui; (VOID)Line; (VOID)Text; }
static void Reset (UNIXNU_UI *Ui, UNIXNU_CONFIG *Config) {
  memset (&Services, 0, sizeof Services); memset (&System, 0, sizeof System);
  gBS = &Services; gST = &System;
  Services.LocateHandleBuffer = Locate; Services.HandleProtocol = Protocol; Services.CreateEvent = Create;
  Services.SetTimer = Timer; Services.CloseEvent = Close; Services.WaitForEvent = Wait; Services.Stall = Stall;
  Services.RegisterProtocolNotify = Register;
  Present = Keys = TRUE; HaveMouse = HaveKey = ApplyError = FALSE; MouseError = EFI_SUCCESS;
  Reads = Blts = FailBlt = Step = Steps = EntriesDrawn = 0;
  CreateError = RegisterError = EFI_SUCCESS; WatchActive = ChangeDuringRead = FALSE; WatchNotify = NULL; WatchContext = NULL;
  Mouse = (EFI_SIMPLE_POINTER_PROTOCOL){0}; Mouse.GetState = GetMouse; Mouse.Mode = &MouseMode;
  MouseMode = (EFI_SIMPLE_POINTER_MODE){8, 8, 0, TRUE, FALSE};
  Keyboard = (EFI_SIMPLE_TEXT_INPUT_PROTOCOL){0}; Keyboard.ReadKeyStroke = GetKey; Keyboard.WaitForKey = PTR (5); System.ConIn = &Keyboard;
  Gop = (EFI_GRAPHICS_OUTPUT_PROTOCOL){0}; Gop.Blt = Blt; Gop.Mode = &GopMode;
  GopMode.Info = &Info; Info.HorizontalResolution = Width; Info.VerticalResolution = Height;
  Video.Before = 0x12345678; Video.After = 0x87654321;
  for (UINTN I = 0; I < Width * Height; I++) { Video.Pixels[I] = (EFI_GRAPHICS_OUTPUT_BLT_PIXEL){(UINT8)I, (UINT8)(I >> 8), 37, 0}; }
  memset (Ui, 0, sizeof *Ui); Ui->Mode = UniXNUUiGraphics; Ui->Width = Width; Ui->Height = Height; Ui->Scale = 2; Ui->Gop = &Gop;
  memset (Config, 0, sizeof *Config); Config->EntryCount = 6; Config->Timeout = 1; Config->FallbackTimeout = 1;
  static UNIXNU_ENTRY Entries[6]; memset (Entries, 0, sizeof Entries);
  for (UINTN I = 0; I < 6; I++) { Entries[I].Title = L"entry"; }
  Config->Entries = Entries; UniXNUUiDrawFrame (Ui, Config);
}
static void Queue (BOOLEAN MouseInput, BOOLEAN Down, INT32 X, INT32 Y, INT32 Z, UINT16 Scan, CHAR16 Char) {
  assert (Steps < ARRAY_SIZE (Script)); Script[Steps++] = (INPUT){MouseInput, Down, X, Y, Z, Scan, Char};
}
static BOOLEAN Poll (UNIXNU_UI *Ui, UNIXNU_MENU_POINTER *P, BOOLEAN Down, INT32 X, INT32 Y, UINTN *Hover, UINTN *Click) {
  State = (EFI_SIMPLE_POINTER_STATE){X, Y, 0, Down, FALSE}; HaveMouse = TRUE; INT32 Scroll;
  return UniXNUMenuPointerPoll (Ui, 6, P, Hover, Click, &Scroll);
}
static void PointerCases (void) {
  UNIXNU_UI Ui; UNIXNU_CONFIG Config; UNIXNU_MENU_POINTER P; UINTN Hover, Click;
  Reset (&Ui, &Config);
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL *Before = malloc (sizeof Video.Pixels); assert (Before);
  memcpy (Before, Video.Pixels, sizeof Video.Pixels);
  assert (UniXNUMenuPointerInit (&Ui, &P)); assert (P.Drawn && !P.SeenUp);
  UniXNUMenuPointerHide (&Ui, &P); assert (!memcmp (Before, Video.Pixels, sizeof Video.Pixels));
  UINTN Points[][2] = {{0,0},{319,0},{0,239},{319,239},{304,224},{310,233},{100,100}};
  for (UINTN I = 0; I < ARRAY_SIZE (Points); I++) {
    P.X = Points[I][0]; P.Y = Points[I][1]; UniXNUMenuPointerDraw (&Ui, &P); assert (P.Drawn);
    UniXNUMenuPointerHide (&Ui, &P); assert (!memcmp (Before, Video.Pixels, sizeof Video.Pixels));
  }
  P.X = 160; P.Y = 50;
  Poll (&Ui, &P, TRUE, 0, 0, &Hover, &Click); assert (P.Armed == MAX_UINTN && Click == MAX_UINTN);
  Poll (&Ui, &P, FALSE, 0, 0, &Hover, &Click); assert (P.SeenUp && Click == MAX_UINTN);
  Poll (&Ui, &P, TRUE, 0, 0, &Hover, &Click); assert (P.Armed == 0);
  Poll (&Ui, &P, TRUE, -1000, 0, &Hover, &Click); assert (P.X == 0 && P.Armed == MAX_UINTN);
  Poll (&Ui, &P, FALSE, 80, 0, &Hover, &Click); assert (Click == MAX_UINTN);
  Poll (&Ui, &P, TRUE, 0, 15, &Hover, &Click); assert (Hover == 1 && P.Armed == 1);
  Poll (&Ui, &P, FALSE, 0, 0, &Hover, &Click); assert (Click == 1);
  // Reinstall may reuse both the handle and object addresses between polls.
  WatchNotify (PTR (6), WatchContext);
  Poll (&Ui, &P, TRUE, 0, 0, &Hover, &Click); assert (P.Armed == MAX_UINTN);
  Poll (&Ui, &P, FALSE, 0, 0, &Hover, &Click); assert (Click == MAX_UINTN);
  Poll (&Ui, &P, TRUE, 0, 0, &Hover, &Click); assert (P.Armed == 1);
  ChangeDuringRead = TRUE;
  Poll (&Ui, &P, FALSE, 0, 0, &Hover, &Click); assert (Click == MAX_UINTN);
  Present = FALSE; UINTN PreviousReads = Reads;
  Poll (&Ui, &P, TRUE, 0, 0, &Hover, &Click); assert (!P.Available && !P.SeenUp && Reads == PreviousReads);
  Present = TRUE; Poll (&Ui, &P, TRUE, 0, 0, &Hover, &Click); assert (P.Armed == MAX_UINTN);
  MouseError = EFI_DEVICE_ERROR; Poll (&Ui, &P, FALSE, 0, 0, &Hover, &Click); assert (!P.SeenUp && !P.Down);
  MouseError = EFI_SUCCESS; Poll (&Ui, &P, FALSE, 0, 0, &Hover, &Click); assert (Click == MAX_UINTN);
  Ui.Scale = MAX_UINTN; Poll (&Ui, &P, FALSE, MAX_INT32, MIN_INT32, &Hover, &Click); assert (P.X == Width - 1 && P.Y == 0);
  Poll (&Ui, &P, FALSE, MIN_INT32, MAX_INT32, &Hover, &Click); assert (P.X == 0 && P.Y == Height - 1);
  Ui.Scale = 2; P.X = 100; P.Y = 100; FailBlt = Blts + 2; ApplyError = TRUE;
  UniXNUMenuPointerDraw (&Ui, &P); assert (!P.Enabled && !P.Drawn); assert (!memcmp (Before, Video.Pixels, sizeof Video.Pixels));
  UniXNUMenuPointerClose (&Ui, &P); assert (!WatchActive);
  free (Before);
  Reset (&Ui, &Config); CreateError = EFI_OUT_OF_RESOURCES;
  assert (!UniXNUMenuPointerInit (&Ui, &P) && !P.Enabled && !WatchActive);
  Reset (&Ui, &Config); RegisterError = EFI_DEVICE_ERROR;
  assert (!UniXNUMenuPointerInit (&Ui, &P) && !P.Enabled && !WatchActive);
  Reset (&Ui, &Config); Ui.FirstVisible = 3;
  assert (UniXNUUiHitTest (&Ui, 6, 32, 40) == 3);
  assert (UniXNUUiHitTest (&Ui, 6, 231, 67) == 3);
  assert (UniXNUUiHitTest (&Ui, 6, 232, 40) == MAX_UINTN);
  assert (UniXNUUiHitTest (&Ui, 6, 40, 68) == MAX_UINTN);
  assert (UniXNUUiHitTest (&Ui, 4, 40, 70) == MAX_UINTN);
  Ui.Portrait = TRUE; assert (UniXNUUiHitTest (&Ui, 6, 20, 40) == 3);
  assert (UniXNUUiHitTest (&Ui, 6, 299, 65) == 3); assert (UniXNUUiHitTest (&Ui, 6, 40, 66) == MAX_UINTN);
  assert (UniXNUUiHitTest (&Ui, 6, 300, 40) == MAX_UINTN);
  UniXNUUiSelectEntry (&Ui, &Config, 3, 5); assert (Ui.FirstVisible == 4 && EntriesDrawn == ((1U << 4) | (1U << 5)));
  UniXNUUiSelectEntry (&Ui, &Config, 5, 0); assert (Ui.FirstVisible == 0);
}
static void MenuCases (void) {
  UNIXNU_UI Ui; UNIXNU_CONFIG Config; Reset (&Ui, &Config);
  Queue (FALSE, FALSE, 0, 0, 0, SCAN_DOWN, 0); Queue (FALSE, FALSE, 0, 0, 0, 0, CHAR_CARRIAGE_RETURN);
  assert (UniXNUMenuRun (&Ui, &Config, TRUE) == 1 && Step == Steps);
  assert (!WatchActive);
  if (!FixedPcdGetBool (PcdUniXNUPointerInputEnabled)) {
    UNIXNU_MENU_POINTER P; assert (!UniXNUMenuPointerInit (&Ui, &P) && !P.Enabled); return;
  }
  Reset (&Ui, &Config); Queue (TRUE, FALSE, 0, -16, 0, 0, 0);
  for (UINTN I = 0; I < 20; I++) { Queue (FALSE, FALSE, 0, 0, 0, 0, 0); }
  Queue (TRUE, TRUE, 0, 0, 0, 0, 0); Queue (TRUE, FALSE, 0, 0, 0, 0, 0);
  assert (UniXNUMenuRun (&Ui, &Config, TRUE) == 1 && Step == Steps);
  Reset (&Ui, &Config); Queue (TRUE, FALSE, 0, 0, -1, 0, 0);
  Queue (FALSE, FALSE, 0, 0, 0, 0, CHAR_CARRIAGE_RETURN);
  assert (UniXNUMenuRun (&Ui, &Config, TRUE) == 1);
  UNIXNU_ENTRY Entry = {0}; Entry.Confirm = L"confirm?"; UINTN Choice;
  Reset (&Ui, &Config); Queue (TRUE, FALSE, 0, -35, 0, 0, 0);
  Queue (TRUE, TRUE, 0, 0, 0, 0, 0); Queue (TRUE, FALSE, 0, 0, 0, 0, 0);
  assert (UniXNUMenuConfirm (&Ui, &Entry, &Choice) && Choice == 0 && Step == Steps);
  assert (!WatchActive);
  Reset (&Ui, &Config); Queue (TRUE, FALSE, 0, -16, 0, 0, 0);
  Queue (TRUE, TRUE, 0, 0, 0, 0, 0); Queue (TRUE, FALSE, 0, 0, 0, 0, 0);
  assert (!UniXNUMenuConfirm (&Ui, &Entry, &Choice) && Step == Steps);
  Reset (&Ui, &Config); Queue (TRUE, TRUE, 0, -35, 0, 0, 0); Queue (TRUE, FALSE, 0, 0, 0, 0, 0);
  Queue (FALSE, FALSE, 0, 0, 0, SCAN_ESC, 0);
  assert (!UniXNUMenuConfirm (&Ui, &Entry, &Choice) && Step == Steps);
  CHAR16 *Titles[] = {L"first", L"second"}; Entry.ConfirmCount = 2; Entry.ConfirmTitles = Titles;
  Reset (&Ui, &Config); Queue (FALSE, FALSE, 0, 0, 0, SCAN_UP, 0); Queue (FALSE, FALSE, 0, 0, 0, SCAN_UP, 0);
  Queue (TRUE, FALSE, 0, -35, 0, 0, 0); Queue (TRUE, TRUE, 0, 0, 0, 0, 0); Queue (TRUE, FALSE, 0, 0, 0, 0, 0);
  assert (UniXNUMenuConfirm (&Ui, &Entry, &Choice) && Choice == 0 && Step == Steps);
  assert (!WatchActive);
  Entry.ConfirmTitles = NULL; assert (!UniXNUMenuConfirm (&Ui, &Entry, &Choice));
}
int main (void) {
  if (FixedPcdGetBool (PcdUniXNUPointerInputEnabled)) { PointerCases (); }
  MenuCases (); puts ("production menu/pointer model PASS"); return 0;
}
