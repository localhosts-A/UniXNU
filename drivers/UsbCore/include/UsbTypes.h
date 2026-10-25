/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef UNIXNU_USB_TYPES_H
#define UNIXNU_USB_TYPES_H
#ifdef UNIXNU_USB_EDK2
#include <Base.h>
typedef UINT8 uint8_t;
typedef UINT16 uint16_t;
typedef UINT32 uint32_t;
typedef UINT64 uint64_t;
typedef INT8 int8_t;
typedef INT16 int16_t;
typedef INT32 int32_t;
typedef INT64 int64_t;
typedef UINTN uintptr_t;
typedef UINTN size_t;
#ifndef __cplusplus
typedef _Bool bool;
#define true 1
#define false 0
#endif
#define UINT16_MAX MAX_UINT16
#define UINT32_MAX MAX_UINT32
#define UINT64_MAX MAX_UINT64
#define SIZE_MAX MAX_UINTN
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#endif
#endif
