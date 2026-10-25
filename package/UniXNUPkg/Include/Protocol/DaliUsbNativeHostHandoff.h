/** @file
  Fail-closed pre-final-memory-map handoff for an installed Dali native USB host.

  SPDX-License-Identifier: BSD-2-Clause-Patent
**/

#ifndef DALI_USB_NATIVE_HOST_HANDOFF_H_
#define DALI_USB_NATIVE_HOST_HANDOFF_H_

#include <Uefi.h>

#define DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL_GUID \
  { 0x28fb0d50, 0xe819, 0x497d, { 0x90, 0x53, 0xdc, 0xb1, 0x2e, 0xa9, 0x41, 0xfd } }

#define DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL_REVISION  0x00010000

typedef struct _DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL;

typedef
EFI_STATUS
(EFIAPI *DALI_USB_NATIVE_HOST_PREPARE_HANDOFF)(
  IN DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL *This
  );

/// The host refuses new submissions and freshly proves DMA/controller halt.
struct _DALI_USB_NATIVE_HOST_HANDOFF_PROTOCOL {
  UINT32 Revision;
  DALI_USB_NATIVE_HOST_PREPARE_HANDOFF PrepareHandoff;
};

extern EFI_GUID gDaliUsbNativeHostHandoffProtocolGuid;

#endif
