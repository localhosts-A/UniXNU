// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "NativeMaliCsf.h"
namespace native_mali {
void CsfInterface::reset() {
    valid_ = false; version_ = groupCount_ = bytes_ = 0; global_ = {}; ops_ = {};
}
bool CsfInterface::range(uint64_t address, uint64_t bytes) const {
    return !(address & 3) && bytes && address >= SharedBase && address <= SharedBase + uint64_t(bytes_) &&
           bytes <= SharedBase + uint64_t(bytes_) - address;
}
bool CsfInterface::read(uint32_t address, uint32_t *value) {
    return range(address, 4) && ops_.read(ops_.cookie, address, value);
}
bool CsfInterface::endpoint(uint32_t descriptor, CsfEndpoint *ep, uint32_t inBytes, uint32_t outBytes) {
    return read(descriptor, &ep->features) && read(descriptor + 4, &ep->input) &&
           read(descriptor + 8, &ep->output) && range(ep->input, inBytes) && range(ep->output, outBytes) &&
           (uint64_t(ep->input) + inBytes <= ep->output || uint64_t(ep->output) + outBytes <= ep->input);
}
CsfResult CsfInterface::discover(const CsfMemoryOps &ops, uint32_t bytes) {
    reset();
    if (!ops.read || !ops.write || bytes < 4096 || (bytes & 4095) || bytes > SharedLimit - SharedBase)
        return CsfResult::Invalid;
    ops_ = ops; bytes_ = bytes;
    if (!read(SharedBase, &version_)) { reset(); return CsfResult::Access; }
    if (!version_ || (version_ >> 24) > 1) { reset(); return CsfResult::Version; }
    uint32_t stride = 0;
    // GLB_FEATURES/INPUT_VA/OUTPUT_VA share the endpoint descriptor format.
    if (!endpoint(SharedBase + 4, &global_, 0x1000, 0x1000) ||
        !read(SharedBase + 0x10, &groupCount_) || !read(SharedBase + 0x14, &stride) ||
        groupCount_ < 3 || groupCount_ > MaxGroups || stride < 0x40 || (stride & 3) ||
        !range(SharedBase + 0x1000, uint64_t(groupCount_) * stride)) {
        reset(); return CsfResult::Layout;
    }
    for (size_t i = 0; i < groupCount_; ++i) {
        const uint32_t base = SharedBase + 0x1000 + uint32_t(i) * stride;
        auto &g = groups_[i]; g = {};
        uint32_t streamStride = 0;
        if (!endpoint(base, &g.endpoint, 0x68, 0x20) || !read(base + 0xc, &g.suspendBytes) ||
            !read(base + 0x10, &g.protectedSuspendBytes) || !read(base + 0x14, &g.streamCount) ||
            !read(base + 0x18, &streamStride) || g.streamCount < 8 || g.streamCount > MaxStreams ||
            streamStride < 12 || (streamStride & 3) ||
            uint64_t(0x40) + uint64_t(g.streamCount) * streamStride > stride) {
            reset(); return CsfResult::Layout;
        }
        for (size_t j = 0; j < g.streamCount; ++j) {
            if (!endpoint(base + 0x40 + uint32_t(j) * streamStride, &g.streams[j], 0x58, 0xd8)) {
                reset(); return CsfResult::Layout;
            }
        }
    }
    valid_ = true; return CsfResult::Ok;
}
CsfResult CsfInterface::write(const CsfEndpoint &ep, uint32_t offset, uint32_t value, uint32_t limit) {
    if (!valid_) return CsfResult::State;
    if ((offset & 3) || offset >= limit) return CsfResult::Invalid;
    return ops_.write(ops_.cookie, ep.input + offset, value) ? CsfResult::Ok : CsfResult::Access;
}
CsfResult CsfInterface::output(const CsfEndpoint &ep, uint32_t offset, uint32_t *value, uint32_t limit) {
    if (value) *value = 0;
    if (!valid_) return CsfResult::State;
    if (!value || (offset & 3) || offset >= limit) return CsfResult::Invalid;
    return read(ep.output + offset, value) ? CsfResult::Ok : CsfResult::Access;
}
CsfResult CsfInterface::globalWrite(uint32_t offset, uint32_t value) { return write(global_, offset, value, 0x1000); }
CsfResult CsfInterface::globalRead(uint32_t offset, uint32_t *v) { return output(global_, offset, v, 0x1000); }
CsfResult CsfInterface::groupWrite(size_t i, uint32_t offset, uint32_t value) {
    if (!group(i)) return CsfResult::Invalid;
    return write(groups_[i].endpoint, offset, value, 0x68);
}
CsfResult CsfInterface::groupRead(size_t i, uint32_t offset, uint32_t *v) {
    if (!group(i)) return CsfResult::Invalid;
    return output(groups_[i].endpoint, offset, v, 0x20);
}
CsfResult CsfInterface::streamWrite(size_t i, size_t j, uint32_t offset, uint32_t value) {
    if (!group(i) || j >= groups_[i].streamCount) return CsfResult::Invalid;
    return write(groups_[i].streams[j], offset, value, 0x58);
}
CsfResult CsfInterface::streamRead(size_t i, size_t j, uint32_t offset, uint32_t *v) {
    if (!group(i) || j >= groups_[i].streamCount) return CsfResult::Invalid;
    return output(groups_[i].streams[j], offset, v, 0xd8);
}
CsfResult CsfInterface::request(const CsfEndpoint &ep, uint32_t offset, uint32_t mask, uint32_t value, bool toggle) {
    if (!valid_) return CsfResult::State;
    if (!mask || (value & ~mask)) return CsfResult::Invalid;
    uint32_t req = 0, ack = 0;
    if (!read(ep.input + offset, &req) || !read(ep.output + offset, &ack)) return CsfResult::Access;
    if ((req ^ ack) & mask) return CsfResult::Busy;
    const uint32_t next = (req & ~mask) | (toggle ? ((~ack) & mask) : value);
    return ops_.write(ops_.cookie, ep.input + offset, next) ? CsfResult::Ok : CsfResult::Access;
}
CsfResult CsfInterface::globalRequest(uint32_t m, uint32_t v, bool t) { return request(global_, 0, m, v, t); }
CsfResult CsfInterface::groupRequest(size_t i, uint32_t m, uint32_t v, bool t) {
    return group(i) ? request(groups_[i].endpoint, 0, m, v, t) : CsfResult::Invalid;
}
CsfResult CsfInterface::streamRequest(size_t i, size_t j, uint32_t m, uint32_t v, bool t) {
    return group(i) && j < groups_[i].streamCount ? request(groups_[i].streams[j], 0, m, v, t) : CsfResult::Invalid;
}
CsfResult CsfInterface::notifyGroup(size_t i) {
    return group(i) ? request(global_, 8, uint32_t(1) << i, 0, true) : CsfResult::Invalid;
}
CsfResult CsfInterface::notifyStream(size_t i, size_t j) {
    return group(i) && j < groups_[i].streamCount ? request(groups_[i].endpoint, 8, uint32_t(1) << j, 0, true) : CsfResult::Invalid;
}
CsfResult CsfInterface::acknowledged(const CsfEndpoint &ep, uint32_t mask, bool *done) {
    if (done) *done = false;
    if (!valid_) return CsfResult::State;
    if (!done || !mask) return CsfResult::Invalid;
    uint32_t req = 0, ack = 0;
    if (!read(ep.input, &req) || !read(ep.output, &ack)) return CsfResult::Access;
    *done = !((req ^ ack) & mask); return CsfResult::Ok;
}
CsfResult CsfInterface::globalAcknowledged(uint32_t m, bool *d) { return acknowledged(global_, m, d); }
CsfResult CsfInterface::groupAcknowledged(size_t i, uint32_t m, bool *d) {
    return group(i) ? acknowledged(groups_[i].endpoint, m, d) : CsfResult::Invalid;
}
CsfResult CsfInterface::streamAcknowledged(size_t i, size_t j, uint32_t m, bool *d) {
    return group(i) && j < groups_[i].streamCount ? acknowledged(groups_[i].streams[j], m, d) : CsfResult::Invalid;
}
CsfResult CsfInterface::pending(const CsfEndpoint &ep, uint32_t mask, uint32_t *bits, bool accept) {
    if (!valid_) return CsfResult::State;
    if (!mask || !bits) return CsfResult::Invalid;
    uint32_t req = 0, ack = 0;
    if (!read(ep.input, &req) || !read(ep.output, &ack)) return CsfResult::Access;
    *bits = (req ^ ack) & mask;
    if (accept && *bits && !ops_.write(ops_.cookie, ep.input, (req & ~mask) | (ack & mask))) return CsfResult::Access;
    return CsfResult::Ok;
}
CsfResult CsfInterface::groupPending(size_t i, uint32_t m, uint32_t *b) {
    return group(i) ? pending(groups_[i].endpoint, m, b, false) : CsfResult::Invalid;
}
CsfResult CsfInterface::streamPending(size_t i, size_t j, uint32_t m, uint32_t *b) {
    return group(i) && j < groups_[i].streamCount ? pending(groups_[i].streams[j], m, b, false) : CsfResult::Invalid;
}
CsfResult CsfInterface::groupAccept(size_t i, uint32_t m) {
    uint32_t bits = 0; return group(i) ? pending(groups_[i].endpoint, m, &bits, true) : CsfResult::Invalid;
}
CsfResult CsfInterface::streamAccept(size_t i, size_t j, uint32_t m) {
    uint32_t bits = 0; return group(i) && j < groups_[i].streamCount ? pending(groups_[i].streams[j], m, &bits, true) : CsfResult::Invalid;
}
} // namespace native_mali
