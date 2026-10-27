// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <Uefi.h>

#define UNIXNU_STORAGE_QUIESCE_REVISION 1ULL

typedef struct _UNIXNU_STORAGE_QUIESCE_PROTOCOL UNIXNU_STORAGE_QUIESCE_PROTOCOL;

// Optional platform storage ownership handoff before Darwin boot. A provider
// closes submissions, drains outstanding commands, synchronizes completed
// writes, stops its controller and revokes firmware DMA before returning exact
// EFI_SUCCESS. A failed or uncertain operation must not be replayed.
typedef EFI_STATUS (EFIAPI *UNIXNU_STORAGE_QUIESCE)(
  IN UNIXNU_STORAGE_QUIESCE_PROTOCOL *This
  );

struct _UNIXNU_STORAGE_QUIESCE_PROTOCOL {
  UINT64 Revision;
  UNIXNU_STORAGE_QUIESCE Quiesce;
};

extern EFI_GUID gUniXNUStorageQuiesceProtocolGuid;
