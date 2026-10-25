// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>
#include <stddef.h>

namespace unixnu_ufs {
constexpr uint32_t UcdOffset = 4096, ResponseOffset = 512, PrdtOffset = 1024;
constexpr uint32_t DataOffset = 8192, DataMax = 131072, DmaBytes = DataOffset + DataMax;
constexpr uint32_t CommandTimeoutUs = 3000000;
enum class Status : uint32_t { Success, Argument, NotReady, Denied, Timeout, Hardware, Protocol, Target, Capacity, Cache, Partial };
enum class Cache : uint32_t { ToDevice, FromDevice };
struct Geometry { uint64_t blocks = 0; uint32_t blockBytes = 0; bool valid = false, writeProtected = true; };
struct Completion {
  Status status = Status::NotReady;
  uint32_t bytes = 0, residual = 0, polls = 0, interrupt = 0, ocs = 15;
  uint8_t transaction = 0, lun = 0, tag = 0, opcode = 0, target = 0;
  uint8_t senseKey = 0, asc = 0, ascq = 0;
};
struct Port {
  void *context = nullptr;
  bool (*read32)(void *, uint32_t offset, uint32_t *) = nullptr;
  bool (*write32)(void *, uint32_t offset, uint32_t) = nullptr;
  // Must perform ordered cache maintenance/barriers on the supplied CPU span.
  bool (*cache)(void *, void *cpu, uint32_t bytes, Cache) = nullptr;
  uint64_t (*micros)(void *) = nullptr;
  void (*delay)(void *, uint32_t us) = nullptr;
  // Platform verifies exclusive controller ownership and inactive MCQ first.
  bool (*exclusiveLegacy)(void *) = nullptr;
  // No default grant. Device/candidate/source/token/range binding belongs to
  // the platform policy. Called before copying data and again before submit.
  bool (*allowWrite)(void *, uint8_t lun, const Geometry &, uint64_t lba, uint32_t blocks) = nullptr;
};
struct Dma { void *cpu = nullptr; uint64_t device = 0, addressLimit = 0; uint32_t bytes = 0; };

// Caller serializes every method, including access to completion/geometry.
// DMA is exclusively allocated, prepared and owned by the caller; CPU and bus
// addresses need not match. This class never allocates, frees or maps memory.
class Transport {
public:
  Transport() = default;
  Transport(const Transport &) = delete;
  Transport &operator=(const Transport &) = delete;
  Status start(const Port &, const Dma &);
  Status initializeDevice();
  Status probe(uint8_t lun);
  Status read(uint8_t lun, uint64_t lba, uint32_t blocks, void *destination);
  Status write(uint8_t lun, uint64_t lba, uint32_t blocks, const void *source);
  Status synchronize(uint8_t lun);
  void close() { closed_ = true; }
  // Platform must stop its controller before releasing any exposed DMA span.
  bool releaseAllowed();
  bool poisoned() const { return poisoned_; }
  const Completion &completion() const { return last_; }
  const Geometry *geometry(uint8_t lun) const { return lun < 8 ? &geometry_[lun] : nullptr; }
private:
  Port port_{}; Dma dma_{}; Geometry geometry_[8]{}; Completion last_{};
  bool attempted_ = false, ready_ = false, initialized_ = false;
  bool closed_ = false, poisoned_ = false, exposed_ = false;
  uint8_t tag_ = 0;
  uint8_t *cpu() const { return static_cast<uint8_t *>(dma_.cpu); }
  Status fail(Status);
  bool reg(uint32_t offset, uint32_t &);
  bool put(uint32_t offset, uint32_t);
  bool wait(uint32_t offset, uint32_t expected);
  Status idle();
  Status prepare(uint8_t transaction, uint8_t lun, uint8_t opcode);
  Status submit(uint32_t direction, uint32_t bytes, uint64_t lba = 0, uint32_t blocks = 0);
  Status query(uint8_t opcode, uint8_t id, uint8_t index, uint16_t length = 0);
  Status scsi(uint8_t lun, const uint8_t *cdb, uint32_t length, uint32_t direction,
              uint32_t bytes, uint64_t lba = 0, uint32_t blocks = 0);
  Status transfer(bool writing, uint8_t lun, uint64_t lba, uint32_t blocks, void *buffer);
};
} // namespace unixnu_ufs
extern "C" uint32_t unixnu_ufs_transport_abi();
