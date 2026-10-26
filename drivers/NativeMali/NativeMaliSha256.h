// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace native_mali {
// FIPS180-4, no allocation/I/O. A null input is valid only for zero bytes.
void sha256(const uint8_t *, size_t, uint8_t digest[32]);
}
