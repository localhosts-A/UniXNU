/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once
#include <stdint.h>
#define NML_ABI_VERSION 1u
#define NML_SERVICE_NAME "DaliMali"
#define NML_SELECTOR_CALL 0u
#define NML_PAGE_SIZE 4096u
enum nml_operation {
    NML_GET_INFO = 0, NML_BUFFER_ALLOC = 1, NML_BUFFER_FREE = 2,
    NML_BUFFER_IMPORT = 3, NML_ALIAS_CREATE = 4,
    NML_GROUP_CREATE = 5, NML_GROUP_DESTROY = 6,
    NML_QUEUE_BIND = 7, NML_QUEUE_KICK = 8, NML_QUEUE_UNBIND = 9,
    NML_POLL = 10, NML_WAIT_VALUE = 11, NML_EVENT_SIGNAL = 12,
    NML_HEAP_CREATE = 13, NML_HEAP_DESTROY = 14,
    NML_TIMESTAMP = 15, NML_FLUSH_ID = 16, NML_BUFFER_SYNC = 17
};
enum nml_status {
    NML_OK = 0, NML_INVALID = 1, NML_STATE = 2, NML_NO_MEMORY = 3,
    NML_BUSY = 4, NML_TIMEOUT = 5, NML_DEVICE_LOST = 6, NML_UNSUPPORTED = 7
};
enum nml_buffer_flags { NML_BUFFER_EXECUTABLE = 1u, NML_BUFFER_UNCACHED = 2u };
enum nml_capabilities {
    NML_CAP_HOST_IMPORT = 1u, NML_CAP_TILER_HEAP = 2u,
    NML_CAP_VALUE_WAIT = 4u, NML_CAP_RESIDENT_GROUPS = 8u
};
/* All sizes and addresses are bytes. No native pointer/size_t/bool fields.
 * Every request must set version/size and zero reserved fields. A connection
 * owns one GPU AS. Object/group handles and memory tokens are connection-local.
 * WAIT_VALUE succeeds only when an owned, aligned GPU memory value > value;
 * timeout_ns is capped at 50ms per call so close/reset cannot be starved.
 */
struct nml_request {
    uint32_t version, size, operation, flags;
    uint32_t object, parent;
    uint64_t offset, length, address, value, timeout_ns;
    uint64_t reserved[8];
};
struct nml_response {
    uint32_t version, size, status, flags;
    uint32_t object, auxiliary;
    uint64_t address, length, value, extra0, extra1;
    uint32_t words[48];
};
/* GET_INFO words, from actual registers/firmware after successful boot. */
enum nml_info_word {
    NML_INFO_GPU_ID, NML_INFO_L2_FEATURES, NML_INFO_CORE_FEATURES, NML_INFO_TILER_FEATURES,
    NML_INFO_MEM_FEATURES, NML_INFO_MMU_FEATURES, NML_INFO_ADDRESS_SPACES,
    NML_INFO_SHADER_LO, NML_INFO_SHADER_HI, NML_INFO_TILER_LO, NML_INFO_TILER_HI,
    NML_INFO_L2_LO, NML_INFO_L2_HI, NML_INFO_TEXTURE_0, NML_INFO_TEXTURE_1,
    NML_INFO_TEXTURE_2, NML_INFO_TEXTURE_3, NML_INFO_MAX_THREADS, NML_INFO_MAX_WORKGROUP,
    NML_INFO_THREAD_FEATURES, NML_INFO_TLS_ALLOC, NML_INFO_CSF_VERSION,
    NML_INFO_CSF_FEATURES, NML_INFO_GROUP_COUNT, NML_INFO_STREAM_COUNT,
    NML_INFO_STREAM_FEATURES, NML_INFO_CAPABILITIES, NML_INFO_VA_BITS, NML_INFO_PA_BITS
};
/* Memory tokens are returned by BUFFER_ALLOC/IMPORT (auxiliary) and QUEUE_BIND
 * (auxiliary: firmware output RO). Queue input is private anonymous staging in
 * the Darwin backend; KICK carries its validated insert value. No MMIO doorbell
 * or AS0 input memory is mapped to userspace. Unmap before freeing BOs.
 */
#ifdef __cplusplus
static_assert(sizeof(nml_request) == 128, "NativeMali request ABI");
static_assert(sizeof(nml_response) == 256, "NativeMali response ABI");
#endif
