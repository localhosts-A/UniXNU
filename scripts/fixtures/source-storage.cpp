// SPDX-License-Identifier: BSD-2-Clause-Patent
// QEMU-only volatile transport for testing the real source IOStorageFamily.
// There is no physical-device, DMA, MMIO or persistence path in this fixture.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" __attribute__((noinline, used))
uint64_t source_storage_test_checkpoint(uint64_t result) {
  asm volatile("" : "+r"(result) :: "memory");
  return result;
}

class SourceStorageFixture : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(SourceStorageFixture)
  static constexpr UInt64 kBlocks = 64;
  UInt64 blockSize = 0;
  uint8_t *memory = nullptr;
  IOLock *gate = nullptr;
  bool online = false, readOnly = false, failSync = false;
  unsigned partialDirection = 0;
  UInt64 writes = 0, syncs = 0;

  static void worker(void *argument, wait_result_t) {
    auto self = static_cast<SourceStorageFixture *>(argument);
    UInt64 result = self->exercise();
    IOLog("SOURCE-STORAGE: case=%u checks=%llx volatile=1 physical-ufs=0\n",
          self->readOnly ? 2U : 1U, result);
    source_storage_test_checkpoint((0x31534600ULL << 32) | ((self->readOnly ? 2ULL : 1ULL) << 16) | result);
    self->release();
    thread_terminate(current_thread());
  }

  IOMedia *copyMedia() {
    OSIterator *drivers = getChildIterator(gIOServicePlane);
    if (!drivers) return nullptr;
    IOMedia *found = nullptr;
    while (auto entry = drivers->getNextObject()) {
      auto driver = OSDynamicCast(IOBlockStorageDriver, entry);
      if (!driver || driver->getProvider() != this) continue;
      OSIterator *children = driver->getChildIterator(gIOServicePlane);
      if (!children) continue;
      while (auto child = children->getNextObject()) {
        auto media = OSDynamicCast(IOMedia, child);
        if (media && media->getProvider() == driver && media->isWhole()) {
          media->retain();
          found = media;
          break;
        }
      }
      children->release();
      if (found) break;
    }
    drivers->release();
    return found;
  }

  UInt64 exercise() {
    IOMedia *media = nullptr;
    for (unsigned attempt = 0; attempt < 500 && !media; ++attempt) {
      media = copyMedia();
      if (!media) IOSleep(10);
    }
    if (!media) return 0;
    UInt64 checks = 0;
    IOStorage *storage = media; // Exercise synchronous wrappers and real dispatch.
    auto input = IOBufferMemoryDescriptor::withCapacity(blockSize, kIODirectionIn);
    auto output = IOBufferMemoryDescriptor::withCapacity(blockSize, kIODirectionOut);
    bool opened = false;
    if (media->getSize() == blockSize * kBlocks && media->getPreferredBlockSize() == blockSize &&
        media->isWritable() == !readOnly && input && output) {
      opened = storage->open(this, 0, readOnly ? kIOStorageAccessReader : kIOStorageAccessReaderWriter);
    }
    if (opened) {
      checks |= 1;
      auto expected = static_cast<uint8_t *>(output->getBytesNoCopy());
      for (UInt64 i = 0; i < blockSize; ++i) expected[i] = static_cast<uint8_t>((i * 17 + 91) & 255);
      UInt64 actual = 0;
      IOReturn status;
      if (readOnly) {
        // The transport seeded LBA 8 before registering; no write API creates it.
        status = storage->read(this, blockSize * 8, input, nullptr, &actual);
        if (status == kIOReturnSuccess && actual == blockSize &&
            memcmp(input->getBytesNoCopy(), expected, blockSize) == 0) checks |= 2;
        status = storage->write(this, blockSize * 8, output, nullptr, &actual);
        if (status == kIOReturnLockedWrite) {
          IOLockLock(gate);
          if (writes == 0) checks |= 4;
          IOLockUnlock(gate);
        }
        status = storage->read(this, blockSize * kBlocks, input, nullptr, &actual);
        if (status == kIOReturnBadArgument) checks |= 8;
      } else {
        status = storage->write(this, blockSize * 8, output, nullptr, &actual);
        if (status == kIOReturnSuccess && actual == blockSize) {
          actual = 0;
          status = storage->read(this, blockSize * 8, input, nullptr, &actual);
          if (status == kIOReturnSuccess && actual == blockSize &&
              memcmp(input->getBytesNoCopy(), expected, blockSize) == 0) checks |= 2;
        }
        IOLockLock(gate); UInt64 before = writes; IOLockUnlock(gate);
        status = storage->write(this, blockSize * kBlocks, output, nullptr, &actual);
        IOLockLock(gate);
        if (status == kIOReturnBadArgument && writes == before) checks |= 4;
        before = syncs;
        IOLockUnlock(gate);
        status = storage->synchronize(this, 0, 0);
        IOLockLock(gate);
        if (status == kIOReturnSuccess && syncs == before + 1) checks |= 8;
        failSync = true;
        IOLockUnlock(gate);
        status = storage->synchronize(this, 0, 0);
        IOLockLock(gate);
        if (status == kIOReturnIOError && syncs == before + 2) checks |= 16;
        failSync = false;
        partialDirection = 2;
        IOLockUnlock(gate);
        actual = 0;
        status = storage->write(this, blockSize * 8, output, nullptr, &actual);
        if (status == kIOReturnIOError && actual == blockSize / 2) checks |= 32;
        IOLockLock(gate); partialDirection = 1; IOLockUnlock(gate);
        actual = 0;
        status = storage->read(this, blockSize * 8, input, nullptr, &actual);
        if (status == kIOReturnIOError && actual == blockSize / 2) checks |= 64;
      }
      storage->close(this);
    }
    if (input) input->release();
    if (output) output->release();
    media->release();
    return checks;
  }

public:
  bool start(IOService *provider) override {
    UInt32 mode = 0;
    if (!PE_parse_boot_argn("source-storage-test", &mode, sizeof mode) || (mode != 1 && mode != 2)) return false;
    IORegistryEntry *root = IORegistryEntry::fromPath("/", gIODTPlane);
    if (!root) return false;
    auto model = OSDynamicCast(OSData, root->getProperty("model"));
    const char expected[] = "qemu-entry-fixture";
    bool qemu = model && model->getLength() == sizeof expected &&
                memcmp(model->getBytesNoCopy(), expected, sizeof expected) == 0;
    root->release();
    if (!qemu || !IOBlockStorageDevice::start(provider)) return false;
    readOnly = mode == 2;
    blockSize = readOnly ? 512 : 4096;
    gate = IOLockAlloc();
    memory = static_cast<uint8_t *>(IOMallocZero(blockSize * kBlocks));
    if (!gate || !memory) return false;
    if (readOnly) {
      for (UInt64 i = 0; i < blockSize; ++i) memory[8 * blockSize + i] = static_cast<uint8_t>((i * 17 + 91) & 255);
    }
    online = true;
    registerService();
    retain();
    thread_t thread = THREAD_NULL;
    if (kernel_thread_start(worker, this, &thread) != KERN_SUCCESS) {
      release();
      source_storage_test_checkpoint(0);
    } else thread_deallocate(thread);
    return true;
  }
  void stop(IOService *provider) override {
    if (gate) { IOLockLock(gate); online = false; IOLockUnlock(gate); }
    IOBlockStorageDevice::stop(provider);
  }
  void free() override {
    if (memory) IOFree(memory, blockSize * kBlocks);
    if (gate) IOLockFree(gate);
    IOBlockStorageDevice::free();
  }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                          IOStorageAttributes *, IOStorageCompletion *completion) override {
    if (!buffer || !completion || !completion->action || !count || block >= kBlocks || count > kBlocks - block ||
        buffer->getLength() != count * blockSize ||
        (buffer->getDirection() != kIODirectionIn && buffer->getDirection() != kIODirectionOut)) return kIOReturnBadArgument;
    IOLockLock(gate);
    bool writing = buffer->getDirection() == kIODirectionOut;
    if (!online || (writing && readOnly)) { IOLockUnlock(gate); return kIOReturnNotPermitted; }
    UInt64 size = count * blockSize;
    bool partial = block == 8 && count == 1 && partialDirection == (writing ? 2U : 1U);
    if (partial) { size /= 2; partialDirection = 0; }
    UInt64 actual = writing ? buffer->readBytes(0, memory + block * blockSize, size)
                           : buffer->writeBytes(0, memory + block * blockSize, size);
    if (writing) ++writes;
    IOLockUnlock(gate);
    IOStorage::complete(completion, partial || actual != size ? kIOReturnIOError : kIOReturnSuccess, actual);
    return kIOReturnSuccess;
  }
  IOReturn doSynchronize(UInt64 block, UInt64 count, IOStorageSynchronizeOptions) override {
    if (block >= kBlocks || count > kBlocks - block) return kIOReturnBadArgument;
    IOLockLock(gate); ++syncs;
    IOReturn status = !online ? kIOReturnNotReady : failSync ? kIOReturnIOError : kIOReturnSuccess;
    IOLockUnlock(gate);
    return status;
  }
  IOReturn doEjectMedia() override { return kIOReturnUnsupported; }
  IOReturn doFormatMedia(UInt64) override { return kIOReturnUnsupported; }
  UInt32 doGetFormatCapacities(UInt64 *, UInt32) const override { return 0; }
  char *getVendorString() override { return const_cast<char *>("UniXNU"); }
  char *getProductString() override { return const_cast<char *>("Source storage RAM fixture"); }
  char *getRevisionString() override { return const_cast<char *>("1"); }
  char *getAdditionalDeviceInfoString() override { return const_cast<char *>("QEMU volatile test"); }
  IOReturn reportBlockSize(UInt64 *value) override { *value = blockSize; return kIOReturnSuccess; }
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = kBlocks - 1; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = readOnly; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    *present = online; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn getWriteCacheState(bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
};
OSDefineMetaClassAndStructors(SourceStorageFixture, IOBlockStorageDevice)
