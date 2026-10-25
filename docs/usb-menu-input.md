# Optional graphics-menu pointer input

`gUniXNUPkgTokenSpaceGuid.PcdUniXNUPointerInputEnabled` defaults to `FALSE`.
Platforms can opt in when their source pointer driver and graphics output are
available. Dali ties it to its explicit `DALI_USB_HOST_STACK=true` FV profile;
that profile still needs its native MT6991 resource/controller/DMA provider.

The boot manager consumes one physical SimplePointer stream, excluding virtual
console splitter handles. It reacquires protocols on each 100 ms menu tick and
registers install/reinstall notifications, clearing button history when a stream
changes even if its handle and allocation address are reused. It never resets
the input device. An observed released state must precede a fresh left press
and release over the same displayed entry. Dragging out cancels that click.
Movement takes over the countdown and selects the visible item; the wheel moves
the selection and scrolls long menus. Existing keyboard navigation remains.
Movement gain is one pixel per report count at scale1, capped at four.

The graphics cursor captures and restores its clipped background around every
redraw. Display-mode mismatch or BLT failure disables pointer input for that
menu. Notification events are closed before their local context goes away.
Headless/text mode retains keyboard input. The disabled build creates no pointer
notification, reads no pointer reports and retains the existing confirmation.

With the option enabled in graphics mode, confirmation is a separate choice
menu with Cancel selected initially. It discards the selecting key/old report,
creates fresh button history, and requires a new complete click or Enter on a
selected option. Escape/other keys cancel. Confirmation items do not display
boot-file type labels. Cancel returns to the original menu without restarting
its countdown. No confirmation item is passed to the boot dispatcher.

Production C model checks use real pinned EDK2 types with modeled GOP and Boot
Services. They cover enabled/disabled builds, edge clipping/background restore,
extreme signed movement, portrait/landscape hitboxes/gaps, scrolling, drag-out,
initial held buttons, disconnect/reinstall (including during GetState), failed
notification setup, countdown takeover and choice/cancel/keyboard behavior:

```sh
python3 -m unittest discover -s tests/uefi -p test_menu_pointer.py -v
CC=gcc python3 -m unittest discover -s tests/uefi -p test_menu_pointer.py -v
```

Set `DALI_USB_EDK2` if the isolated sibling `../UEFI-Dali/Mu_Basecore` is elsewhere.
The actual source UEFI USB QEMU gate also invokes the public `UniXNURun` entry,
using a fresh file-based configuration. Through source USB keyboard/mouse it
selects an entry, cancels, reselects with the keyboard, and confirms with a fresh
mouse click, in both landscape and portrait layouts. It preserves screenshots
and verifies the ordered production logs before testing BOT/FAT32 persistence.
Its explicit test-only ConIn binding does not qualify Dali console aggregation,
physical display/input, MT6991 or source XNU. `hardware_qualified=false`.
