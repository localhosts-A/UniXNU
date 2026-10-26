// SPDX-License-Identifier: MIT
// Exercises global IDs, 64-bit addresses, loads, arithmetic and stores.
__attribute__((reqd_work_group_size(32, 1, 1))) kernel void
native_mali_scale(global uint *output, global const uint *input)
{
    const size_t index = get_global_id(0);
    output[index] = input[index] * 3u + 7u;
}
