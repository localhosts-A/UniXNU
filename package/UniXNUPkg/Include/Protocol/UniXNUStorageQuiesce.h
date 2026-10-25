// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define UNIXNU_STORAGE_QUIESCE_REVISION 1ULL

typedef struct _UNIXNU_STORAGE_QUIESCE_PROTOCOL UNIXNU_STORAGE_QUIESCE_PROTOCOL;

// Optional platform storage ownership handoff, called at TPL_APPLICATION after
// all boot files/volumes are closed and before the final memory map. A provider
// must close new submissions, finish outstanding commands within a bounded
// deadline, synchronize completed writes with a real device command, stop its
// queues/controller, and revoke all firmware DMA before returning EFI_SUCCESS.
// Only exact EFI_SUCCESS authorizes entry; warning/error statuses prohibit it.
// Never replay an uncertain write or reopen
// submissions. Repeated calls must be safe and must not repeat completed work;
// successful quiescence remains in effect across ExitBootServices retries.
// No firmware buffer/descriptor pointers are passed to the next owner. That
// owner must allocate its own queues/mappings and initialize the controller.
typedef EFI_STATUS (EFIAPI *UNIXNU_STORAGE_QUIESCE)(
  IN UNIXNU_STORAGE_QUIESCE_PROTOCOL *This
  );

struct _UNIXNU_STORAGE_QUIESCE_PROTOCOL {
  UINT64 Revision;
  UNIXNU_STORAGE_QUIESCE Quiesce;
};

extern EFI_GUID gUniXNUStorageQuiesceProtocolGuid;
