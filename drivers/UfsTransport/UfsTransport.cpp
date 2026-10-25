// SPDX-License-Identifier: BSD-2-Clause-Patent
// Wire layout/completion concepts follow NativeUfs RootCommands/RootQueue,
// without its sheng addresses, SMMU setup, LUN0 or 4096-byte media assumptions.
#include "UfsTransport.h"
#include <string.h>

namespace unixnu_ufs {
// This core issues only transfer-list commands. Platform startup must consume
// its UIC/power events first; no task, UIC or unexpected interrupt is ignored.
static constexpr uint32_t UnexpectedInterrupts = ~uint32_t(1);
static uint32_t le32(const uint8_t *p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
static uint32_t be32(const uint8_t *p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
static uint64_t be64(const uint8_t *p) { return uint64_t(be32(p)) << 32 | be32(p + 4); }
static uint16_t be16(const uint8_t *p) { return uint16_t(p[0]) << 8 | p[1]; }
static void put32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (i * 8)); }
static void putBe32(uint8_t *p, uint32_t n) { for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(n >> (24 - i * 8)); }
static void putBe64(uint8_t *p, uint64_t n) { putBe32(p, uint32_t(n >> 32)); putBe32(p + 4, uint32_t(n)); }

Status Transport::fail(Status status) {
  if (!poisoned_) { poisoned_ = true; ready_ = false; last_.status = status; }
  return status; // Rejected later calls must not overwrite the first failure evidence.
}
bool Transport::reg(uint32_t offset, uint32_t &value) {
  return port_.read32(port_.context, offset, &value) && value != UINT32_MAX;
}
bool Transport::put(uint32_t offset, uint32_t value) { return port_.write32(port_.context, offset, value); }
bool Transport::wait(uint32_t offset, uint32_t expected) {
  uint64_t start = port_.micros(port_.context);
  for (uint32_t elapsed = 0; elapsed <= 100000; elapsed += 10) {
    uint32_t value;
    if (!reg(offset, value)) return false;
    if (value == expected) return true;
    if (elapsed == 100000 || port_.micros(port_.context) - start >= 100000) return false;
    port_.delay(port_.context, 10);
  }
  return false;
}
Status Transport::start(const Port &port, const Dma &dma) {
  if (attempted_) return Status::NotReady;
  if (!port.read32 || !port.write32 || !port.cache || !port.micros || !port.delay || !port.exclusiveLegacy ||
      !dma.cpu || (reinterpret_cast<uintptr_t>(dma.cpu) & 4095) || (dma.device & 4095) ||
      dma.bytes < DmaBytes || dma.bytes - 1 > UINTPTR_MAX - reinterpret_cast<uintptr_t>(dma.cpu) ||
      dma.device > dma.addressLimit || dma.bytes - 1 > dma.addressLimit - dma.device) return Status::Argument;
  port_ = port; dma_ = dma; attempted_ = true;
  if (!port_.exclusiveLegacy(port_.context)) return fail(Status::Denied);
  uint32_t hce, hcs, db, tasks, is;
  if (!reg(0x34, hce) || hce != 1 || !reg(0x30, hcs) || (hcs & 15) != 15 ||
      !reg(0x58, db) || db || !reg(0x78, tasks) || tasks || !reg(0x20, is) || (is & UnexpectedInterrupts)) return fail(Status::Hardware);
  if (!put(0x24, 0) || !put(0x60, 0) || !put(0x80, 0) || !wait(0x60, 0) || !wait(0x80, 0)) return fail(Status::Hardware);
  // Mark exposed before the possibly uncertain first base-register write.
  exposed_ = true;
  if (!put(0x50, uint32_t(dma.device)) || !put(0x54, uint32_t(dma.device >> 32)) ||
      !put(0x60, 1) || !wait(0x60, 1)) return fail(Status::Hardware);
  ready_ = true;
  Status status = idle();
  if (status == Status::Success) last_.status = status;
  return status;
}
Status Transport::idle() {
  if (!ready_ || closed_ || poisoned_) return Status::NotReady;
  uint32_t hce, db, tasks, is, low, high, run;
  if (!reg(0x34, hce) || hce != 1 || !reg(0x58, db) || db || !reg(0x78, tasks) || tasks ||
      !reg(0x20, is) || (is & UnexpectedInterrupts) || !reg(0x50, low) || low != uint32_t(dma_.device) ||
      !reg(0x54, high) || high != uint32_t(dma_.device >> 32) || !reg(0x60, run) || run != 1) return fail(Status::Hardware);
  return Status::Success;
}
Status Transport::prepare(uint8_t transaction, uint8_t lun, uint8_t opcode) {
  Status status = idle();
  if (status != Status::Success) return status;
  last_ = Completion{};
  last_.transaction = transaction; last_.lun = lun; last_.tag = tag_++; last_.opcode = opcode;
  memset(cpu(), 0, 32);
  memset(cpu() + UcdOffset, 0, PrdtOffset + 16);
  auto q = cpu() + UcdOffset;
  q[0] = transaction; q[2] = lun; q[3] = last_.tag;
  return Status::Success;
}
Status Transport::submit(uint32_t direction, uint32_t bytes, uint64_t lba, uint32_t blocks) {
  if (bytes > DataMax || (bytes & 3) || (bytes ? (direction != 1 && direction != 2) : direction != 0)) return fail(Status::Argument);
  auto trd = cpu(); auto q = cpu() + UcdOffset;
  uint64_t ucd = dma_.device + UcdOffset;
  // UFS storage CT=1 for SCSI, NOP and query (no separate query CT).
  put32(trd, 0x11000000U | (direction << 25));
  put32(trd + 8, 15); put32(trd + 16, uint32_t(ucd)); put32(trd + 20, uint32_t(ucd >> 32));
  put32(trd + 24, 128U | ((ResponseOffset / 4) << 16));
  put32(trd + 28, (bytes ? 1U : 0U) | ((PrdtOffset / 4) << 16));
  if (bytes) {
    auto prd = q + PrdtOffset; uint64_t data = dma_.device + DataOffset;
    put32(prd, uint32_t(data)); put32(prd + 4, uint32_t(data >> 32)); put32(prd + 8, 0); put32(prd + 12, bytes - 1);
  }
  if (!port_.cache(port_.context, cpu(), DataOffset + bytes, Cache::ToDevice)) return fail(Status::Cache);
  Status status = idle();
  if (status != Status::Success) return status;
  if (direction == 1 && (!port_.allowWrite || !port_.allowWrite(port_.context, last_.lun, geometry_[last_.lun], lba, blocks))) {
    last_.status = Status::Denied; return last_.status;
  }
  uint64_t start = port_.micros(port_.context);
  if (!put(0x20, 1) || !put(0x58, 1)) return fail(Status::Hardware);
  bool completed = false;
  for (uint32_t elapsed = 0; elapsed <= CommandTimeoutUs; elapsed += 10) {
    ++last_.polls;
    uint32_t db, is, hce;
    if (!reg(0x34, hce) || hce != 1 || !reg(0x58, db) || (db & ~1U) || !reg(0x20, is)) return fail(Status::Hardware);
    last_.interrupt |= is;
    if (last_.interrupt & UnexpectedInterrupts) return fail(Status::Hardware);
    if (!db) {
      if (!port_.cache(port_.context, cpu(), 32, Cache::FromDevice)) return fail(Status::Cache);
      last_.ocs = le32(cpu() + 8) & 255;
      if (last_.ocs != 15) { completed = true; break; }
    }
    if (elapsed == CommandTimeoutUs || port_.micros(port_.context) - start >= CommandTimeoutUs) break;
    port_.delay(port_.context, 10);
  }
  if (!completed) return fail(Status::Timeout);
  if (last_.ocs != 0) return fail(Status::Hardware);
  if (!port_.cache(port_.context, cpu() + UcdOffset, DataOffset + bytes - UcdOffset, Cache::FromDevice)) return fail(Status::Cache);
  auto r = q + ResponseOffset; uint16_t segment = be16(r + 10);
  if (r[0] != uint8_t(last_.transaction | 0x20) || r[2] != last_.lun || r[3] != last_.tag || r[8] || segment > 480) return fail(Status::Protocol);
  if (last_.transaction == 0) {
    if (r[6] || r[7] || segment) return fail(Status::Protocol);
  } else if (last_.transaction == 0x16) {
    if (r[5] != q[5] || memcmp(r + 12, q + 12, 4)) return fail(Status::Protocol);
    last_.target = r[6]; // Preserve QUERY result, including INVALID INDEX for absent LUNs.
    if (r[6]) { last_.status = Status::Target; return last_.status; }
    if (last_.opcode == 1) {
      uint16_t length = be16(r + 18);
      if (length > be16(q + 18) || length != segment || length < 2 || r[32] != length || r[33] != q[13]) return fail(Status::Protocol);
    } else if (segment || be16(r + 18) || be32(r + 20) > 1) return fail(Status::Protocol);
  } else {
    last_.target = r[7]; last_.residual = be32(r + 12);
    uint16_t sense = be16(r + 32);
    if (segment > 20 || sense > 18 || (sense && segment < sense + 2) || (r[1] & 0x40) || last_.residual > bytes) return fail(Status::Protocol);
    if (r[6] > 1 || (r[6] && !r[7])) return fail(Status::Protocol);
    if (sense >= 14 && ((r[34] & 0x7f) == 0x70 || (r[34] & 0x7f) == 0x71)) {
      last_.senseKey = r[36] & 15; last_.asc = r[46]; last_.ascq = r[47];
    } else if (sense >= 4 && ((r[34] & 0x7f) == 0x72 || (r[34] & 0x7f) == 0x73)) {
      last_.senseKey = r[35] & 15; last_.asc = r[36]; last_.ascq = r[37];
    }
    if (r[7]) { last_.status = Status::Target; return last_.status; }
    if (sense || (segment && segment != 2) || ((r[1] & 0x20) != 0) != (last_.residual != 0)) return fail(Status::Protocol);
    last_.bytes = bytes - last_.residual;
    if (last_.residual) return fail(Status::Partial);
  }
  last_.status = Status::Success; return last_.status;
}
Status Transport::query(uint8_t opcode, uint8_t id, uint8_t index, uint16_t length) {
  if ((opcode != 1 && opcode != 5 && opcode != 6) || (opcode == 1 ? length < 2 || length > 255 : length != 0) ||
      (opcode != 1 && (id != 1 || index))) return Status::Argument;
  Status status = prepare(0x16, 0, opcode);
  if (status != Status::Success) return status;
  auto q = cpu() + UcdOffset;
  q[5] = opcode == 6 ? 0x81 : 1; q[12] = opcode; q[13] = id; q[14] = index;
  q[18] = uint8_t(length >> 8); q[19] = uint8_t(length);
  return submit(0, 0);
}
Status Transport::scsi(uint8_t lun, const uint8_t *cdb, uint32_t length, uint32_t direction,
                       uint32_t bytes, uint64_t lba, uint32_t blocks) {
  Status status = prepare(1, lun, cdb[0]);
  if (status != Status::Success) return status;
  auto q = cpu() + UcdOffset;
  q[1] = uint8_t(1 | (direction == 1 ? 0x20 : direction == 2 ? 0x40 : 0));
  putBe32(q + 12, bytes); memcpy(q + 16, cdb, length);
  return submit(direction, bytes, lba, blocks);
}
Status Transport::initializeDevice() {
  if (initialized_) return Status::NotReady;
  Status status = prepare(0, 0, 0);
  if (status == Status::Success) status = submit(0, 0);
  if (status == Status::Success) status = query(6, 1, 0); // Single fDeviceInit SET, never replayed.
  if (status != Status::Success) return fail(status);
  uint64_t start = port_.micros(port_.context);
  for (uint32_t elapsed = 0; elapsed <= CommandTimeoutUs; elapsed += 1000) {
    status = query(5, 1, 0);
    if (status != Status::Success) return fail(status);
    if (be32(cpu() + UcdOffset + ResponseOffset + 20) == 0) { initialized_ = true; return Status::Success; }
    if (elapsed == CommandTimeoutUs || port_.micros(port_.context) - start >= CommandTimeoutUs) break;
    port_.delay(port_.context, 1000);
  }
  return fail(Status::Timeout);
}
Status Transport::readDeviceIdentity(uint8_t *buffer, uint32_t capacity, uint32_t &bytes) {
  bytes = 0;
  if (!buffer || capacity < 4) return Status::Argument;
  if (!initialized_ || !ready_ || closed_ || poisoned_) return Status::NotReady;

  // UFS 4.0 Device Descriptor: SerialName is byte 22 and ManufacturerId is
  // bytes 24..25. The string descriptor is returned verbatim so a platform can
  // hash it without interpreting potentially malformed UTF-16 data.
  Status status = query(1, 0, 0, 255);
  if (status != Status::Success) return status;
  const auto device = cpu() + UcdOffset + ResponseOffset + 32;
  uint8_t serialIndex = device[22];
  if (device[0] < 26 || device[1] != 0) return fail(Status::Protocol);
  if (!serialIndex || serialIndex == 0xff) return Status::Denied;
  uint8_t manufacturer[2] = {device[24], device[25]};

  status = query(1, 5, serialIndex, 255);
  if (status != Status::Success) return status;
  const auto serial = cpu() + UcdOffset + ResponseOffset + 32;
  uint8_t length = serial[0];
  if (length < 4 || serial[1] != 5 || ((length - 2) & 1) || uint32_t(length) + 2 > capacity) return fail(Status::Protocol);
  uint8_t nonzero = 0;
  for (uint32_t i = 2; i < length; ++i) nonzero |= serial[i];
  if (!nonzero) return Status::Denied;

  // Binding input = big-endian ManufacturerId + complete serial descriptor.
  // Keeping the descriptor header/index in the digest prevents aliasing a
  // different UFS string slot; only the digest should leave the platform.
  buffer[0] = manufacturer[0]; buffer[1] = manufacturer[1];
  memcpy(buffer + 2, serial, length);
  bytes = uint32_t(length) + 2;
  return Status::Success;
}
Status Transport::probe(uint8_t lun) {
  if (lun >= 8) return Status::Argument;
  if (!initialized_) return Status::NotReady;
  geometry_[lun] = Geometry{};
  Status status = query(1, 2, lun, 255); // Unit descriptor; no LUN/sector default.
  if (status != Status::Success) return status;
  auto unit = cpu() + UcdOffset + ResponseOffset + 32;
  if (unit[0] < 35 || unit[2] != lun || unit[3] > 1 || unit[5] > 2) return fail(Status::Protocol);
  if (!unit[3]) return Status::NotReady;
  bool wp = unit[5] != 0;
  uint8_t exponent = unit[10]; uint64_t unitBlocks = be64(unit + 11);
  uint8_t cdb[16] = {};
  // Initial reset UNIT ATTENTION may require acknowledgement. Only this known
  // completed CHECK CONDITION is retried, only for no-data TEST UNIT READY.
  // There is no automatic retry of a data read/write, sync, query or timeout.
  for (unsigned attempt = 0; attempt < 3; ++attempt) {
    status = scsi(lun, cdb, 6, 0, 0);
    if (status == Status::Success) break;
    if (status != Status::Target || last_.target != 2 || last_.senseKey != 6 || last_.asc != 0x29 || last_.ascq) return status;
  }
  if (status != Status::Success) return status;
  cdb[0] = 0x25;
  status = scsi(lun, cdb, 10, 2, 8);
  if (status != Status::Success) return status;
  uint64_t last = be32(cpu() + DataOffset); uint32_t size = be32(cpu() + DataOffset + 4);
  if (last == UINT32_MAX) {
    memset(cdb, 0, sizeof cdb); cdb[0] = 0x9e; cdb[1] = 0x10; cdb[13] = 32;
    status = scsi(lun, cdb, 16, 2, 32);
    if (status != Status::Success) return status;
    last = be64(cpu() + DataOffset); size = be32(cpu() + DataOffset + 8);
  }
  // Compare descriptor geometry as well as SCSI capacity; neither is a default.
  if (exponent >= 32 || size != (uint32_t(1) << exponent) || last == UINT64_MAX || unitBlocks != last + 1 ||
      size < 512 || size > 65536 || (size & (size - 1)) || last + 1 > UINT64_MAX / size) return fail(Status::Capacity);
  geometry_[lun] = Geometry{last + 1, size, true, wp};
  return Status::Success;
}
Status Transport::transfer(bool writing, uint8_t lun, uint64_t lba, uint32_t blocks, void *buffer) {
  if (lun >= 8 || !buffer || !blocks) return Status::Argument;
  const auto &g = geometry_[lun];
  if (!initialized_ || !g.valid || !ready_ || closed_ || poisoned_) return Status::NotReady;
  if (lba >= g.blocks || blocks > g.blocks - lba || blocks > DataMax / g.blockBytes) return Status::Argument;
  if (writing && (g.writeProtected || !port_.allowWrite || !port_.allowWrite(port_.context, lun, g, lba, blocks))) return Status::Denied;
  Status status = idle();
  if (status != Status::Success) return status;
  uint32_t bytes = blocks * g.blockBytes;
  if (writing) memcpy(cpu() + DataOffset, buffer, bytes);
  uint8_t cdb[16] = {};
  bool wide = lba > UINT32_MAX || blocks - 1 > UINT32_MAX - lba;
  cdb[0] = uint8_t(writing ? (wide ? 0x8a : 0x2a) : (wide ? 0x88 : 0x28));
  if (wide) { putBe64(cdb + 2, lba); putBe32(cdb + 10, blocks); }
  else { putBe32(cdb + 2, uint32_t(lba)); cdb[7] = uint8_t(blocks >> 8); cdb[8] = uint8_t(blocks); }
  status = scsi(lun, cdb, wide ? 16 : 10, writing ? 1 : 2, bytes, lba, blocks);
  if (writing && status != Status::Success && status != Status::Denied) status = fail(status);
  if (!writing && (status == Status::Success || status == Status::Partial)) memcpy(buffer, cpu() + DataOffset, last_.bytes);
  return status;
}
Status Transport::read(uint8_t lun, uint64_t lba, uint32_t blocks, void *out) { return transfer(false, lun, lba, blocks, out); }
Status Transport::write(uint8_t lun, uint64_t lba, uint32_t blocks, const void *in) { return transfer(true, lun, lba, blocks, const_cast<void *>(in)); }
Status Transport::synchronize(uint8_t lun) {
  if (lun >= 8 || !geometry_[lun].valid || !initialized_) return Status::NotReady;
  uint8_t cdb[10] = {0x35};
  Status status = scsi(lun, cdb, 10, 0, 0);
  return status == Status::Success ? status : fail(status);
}
bool Transport::releaseAllowed() {
  if (!exposed_) return true;
  uint32_t hce;
  return closed_ && reg(0x34, hce) && hce == 0;
}
} // namespace unixnu_ufs
extern "C" __attribute__((used, noinline)) uint32_t unixnu_ufs_transport_abi() { return 1; }
