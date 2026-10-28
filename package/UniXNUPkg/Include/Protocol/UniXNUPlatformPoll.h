/** @file
  Optional foreground polling hook for platform services used by UniXNU.

  Copyright (c) 2026, UniXNU contributors. All rights reserved.
  SPDX-License-Identifier: BSD-2-Clause-Patent
**/
#pragma once
#include <Uefi.h>

#define UNIXNU_PLATFORM_POLL_PROTOCOL_REVISION  1U

extern EFI_GUID  gUniXNUPlatformPollProtocolGuid;

typedef struct _UNIXNU_PLATFORM_POLL_PROTOCOL UNIXNU_PLATFORM_POLL_PROTOCOL;

/**
  A platform service may publish this protocol to receive foreground polling
  opportunities while the UniXNU menu is waiting for input. The caller invokes
  Poll only at TPL_APPLICATION. Implementations must keep the no-work path
  bounded and must not depend on polling continuing after the menu returns.
**/
struct _UNIXNU_PLATFORM_POLL_PROTOCOL {
  UINT32      Revision;
  UINT32      Size;
  EFI_STATUS  (EFIAPI *Poll)(IN UNIXNU_PLATFORM_POLL_PROTOCOL *This);
};
