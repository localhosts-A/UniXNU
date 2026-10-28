// SPDX-License-Identifier: BSD-2-Clause-Patent
// QEMU-only volatile transport for testing the real source IOStorageFamily.
// There is no physical-device, DMA, MMIO or persistence path in this fixture.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOBSD.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOBlockStorageDriver.h>
#include <IOKit/storage/IOMedia.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSString.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

namespace {
constexpr UInt64 kWritableBlocks = 64;
constexpr UInt32 kFatTotalSectors = 66584;
constexpr UInt32 kFatReservedSectors = 32;
constexpr UInt32 kFatSectorsPerFat = 513;
constexpr UInt32 kFatCount = 2;
constexpr UInt32 kFatDataStart = kFatReservedSectors + kFatCount * kFatSectorsPerFat;
constexpr UInt32 kFatClusterCount = kFatTotalSectors - kFatDataStart;
constexpr char kFatFileContents[] = "Dali USB BOT FAT32 RAM read-only\n";

static IOLock *gSourceStoragePathLock = nullptr;
static char gSourceStorageDevicePath[64] = {};
static bool gSourceStorageDevicePathReady = false;

void put_le16(uint8_t *bytes, UInt16 value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8);
}

void put_le32(uint8_t *bytes, UInt32 value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8);
  bytes[2] = static_cast<uint8_t>(value >> 16);
  bytes[3] = static_cast<uint8_t>(value >> 24);
}

void initialize_fat32(uint8_t *media) {
  auto boot = media;
  boot[0] = 0xeb; boot[1] = 0x58; boot[2] = 0x90;
  memcpy(boot + 3, "DALIUSB ", 8);
  put_le16(boot + 11, 512);
  boot[13] = 1;
  put_le16(boot + 14, kFatReservedSectors);
  boot[16] = static_cast<uint8_t>(kFatCount);
  put_le16(boot + 17, 0);
  put_le16(boot + 19, 0);
  boot[21] = 0xf8;
  put_le16(boot + 22, 0);
  put_le16(boot + 24, 63);
  put_le16(boot + 26, 255);
  put_le32(boot + 28, 0);
  put_le32(boot + 32, kFatTotalSectors);
  put_le32(boot + 36, kFatSectorsPerFat);
  put_le16(boot + 40, 0);
  put_le16(boot + 42, 0);
  put_le32(boot + 44, 2);
  put_le16(boot + 48, 1);
  put_le16(boot + 50, 6);
  boot[64] = 0x80;
  boot[66] = 0x29;
  put_le32(boot + 67, 0x20261028);
  memcpy(boot + 71, "RAM FAT32  ", 11);
  memcpy(boot + 82, "FAT32   ", 8);
  put_le16(boot + 510, 0xaa55);

  auto fsInfo = media + 512;
  put_le32(fsInfo, 0x41615252);
  put_le32(fsInfo + 484, 0x61417272);
  put_le32(fsInfo + 488, kFatClusterCount - 2);
  put_le32(fsInfo + 492, 4);
  put_le32(fsInfo + 508, 0xaa550000);
  memcpy(media + 6 * 512, boot, 512);
  memcpy(media + 7 * 512, fsInfo, 512);

  for (UInt32 fatIndex = 0; fatIndex < kFatCount; ++fatIndex) {
    size_t fatOffset = static_cast<size_t>(kFatReservedSectors + fatIndex * kFatSectorsPerFat) * 512;
    put_le32(media + fatOffset, 0x0ffffff8);
    put_le32(media + fatOffset + 4, 0x0fffffff);
    put_le32(media + fatOffset + 8, 0x0fffffff);  // Root directory cluster 2.
    put_le32(media + fatOffset + 12, 0x0fffffff); // README.TXT cluster 3.
  }

  size_t rootOffset = static_cast<size_t>(kFatDataStart) * 512;
  memcpy(media + rootOffset, "README  TXT", 11);
  media[rootOffset + 11] = 0x20;
  put_le16(media + rootOffset + 20, 0);
  put_le16(media + rootOffset + 26, 3);
  put_le32(media + rootOffset + 28, sizeof(kFatFileContents) - 1);
  size_t fileOffset = static_cast<size_t>(kFatDataStart + 1) * 512;
  memcpy(media + fileOffset, kFatFileContents, sizeof(kFatFileContents) - 1);
}
} // namespace

extern "C" int source_storage_fixture_copy_bsd_path(char *path, size_t capacity) {
  if (!path || !capacity || !gSourceStoragePathLock) return 0;
  path[0] = '\0';
  IOLockLock(gSourceStoragePathLock);
  size_t length = gSourceStorageDevicePathReady ? strlen(gSourceStorageDevicePath) + 1 : 0;
  bool ready = length && length <= capacity;
  if (ready) strlcpy(path, gSourceStorageDevicePath, capacity);
  IOLockUnlock(gSourceStoragePathLock);
  return ready ? 1 : 0;
}

extern "C" int source_storage_fixture_copy_usb_bsd_path(char *path, size_t capacity) {
  if (!path || !capacity) return 0;
  path[0] = '\0';

  // The QEMU USB/FAT test must select the real Dali BOT service. Never fall
  // back to the RAM fixture: that would make the filesystem gate look green
  // without issuing any USB transfers.
  OSDictionary *matching = IOService::serviceMatching("DaliUsbStorage");
  if (!matching) return 0;
  OSIterator *devices = IOService::getMatchingServices(matching);
  matching->release();
  if (!devices) return 0;

  char candidate[64] = {};
  unsigned device_count = 0;
  unsigned whole_media_count = 0;
  while (auto object = devices->getNextObject()) {
    auto device = OSDynamicCast(IOBlockStorageDevice, object);
    if (!device) continue;
    ++device_count;
    if (device_count != 1) break;

    OSIterator *drivers = device->getChildIterator(gIOServicePlane);
    if (!drivers) continue;
    while (auto child = drivers->getNextObject()) {
      auto driver = OSDynamicCast(IOBlockStorageDriver, child);
      if (!driver || driver->getProvider() != device) continue;
      OSIterator *media_children = driver->getChildIterator(gIOServicePlane);
      if (!media_children) continue;
      while (auto media_object = media_children->getNextObject()) {
        auto media = OSDynamicCast(IOMedia, media_object);
        if (!media || media->getProvider() != driver || !media->isWhole()) continue;
        ++whole_media_count;
        if (whole_media_count != 1) break;
        auto name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
        if (!name || !name->getLength() ||
            name->getLength() >= sizeof(candidate) - 5) continue;
        int length = snprintf(candidate, sizeof(candidate), "/dev/%s",
                              name->getCStringNoCopy());
        if (length <= 0 || static_cast<size_t>(length) >= sizeof(candidate))
          candidate[0] = '\0';
      }
      media_children->release();
      if (whole_media_count > 1) break;
    }
    drivers->release();
  }
  devices->release();

  size_t length = strlen(candidate) + 1;
  if (device_count != 1 || whole_media_count != 1 || !candidate[0] ||
      length > capacity) return 0;
  strlcpy(path, candidate, capacity);
  return 1;
}

extern "C" __attribute__((noinline, used))
uint64_t source_storage_test_checkpoint(uint64_t result) {
  asm volatile("" : "+r"(result) :: "memory");
  return result;
}

class SourceStorageFixture : public IOBlockStorageDevice {
  OSDeclareDefaultStructors(SourceStorageFixture)
  UInt64 blockSize = 0;
  UInt64 blockCount = 0;
  uint8_t *memory = nullptr;
  IOLock *gate = nullptr;
  char publishedBsdPath[64] = {};
  bool online = false, readOnly = false, failSync = false, requireBsdPath = false;
  unsigned partialDirection = 0;
  UInt64 writes = 0, syncs = 0;

  bool publishBsdPath(IOMedia *media) {
    if (!media || !gSourceStoragePathLock) return false;
    auto name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    if (!name || !name->getLength() || name->getLength() >= sizeof(gSourceStorageDevicePath) - 5) return false;
    char path[sizeof(gSourceStorageDevicePath)] = {};
    int length = snprintf(path, sizeof(path), "/dev/%s", name->getCStringNoCopy());
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(path)) return false;
    IOLockLock(gSourceStoragePathLock);
    if (!gSourceStorageDevicePathReady) {
      strlcpy(gSourceStorageDevicePath, path, sizeof(gSourceStorageDevicePath));
      gSourceStorageDevicePathReady = true;
      IOLog("SOURCE-STORAGE: BSD device path published: %s\n", gSourceStorageDevicePath);
    }
    bool ready = gSourceStorageDevicePathReady && strcmp(gSourceStorageDevicePath, path) == 0;
    if (ready) strlcpy(publishedBsdPath, path, sizeof(publishedBsdPath));
    IOLockUnlock(gSourceStoragePathLock);
    return ready;
  }

  void clearBsdPath() {
    if (!gSourceStoragePathLock || !publishedBsdPath[0]) return;
    IOLockLock(gSourceStoragePathLock);
    if (gSourceStorageDevicePathReady &&
        strcmp(gSourceStorageDevicePath, publishedBsdPath) == 0) {
      gSourceStorageDevicePath[0] = '\0';
      gSourceStorageDevicePathReady = false;
    }
    publishedBsdPath[0] = '\0';
    IOLockUnlock(gSourceStoragePathLock);
  }

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
    bool bsdPathReady = false;
    for (unsigned attempt = 0; attempt < 500 && (!media || (requireBsdPath && !bsdPathReady)); ++attempt) {
      if (!media) media = copyMedia();
      if (media) {
        bsdPathReady = publishBsdPath(media);
      }
      if (!media || (requireBsdPath && !bsdPathReady)) IOSleep(10);
    }
    if (media && requireBsdPath && !bsdPathReady) {
      media->release();
      return 0;
    }
    if (!media) return 0;
    UInt64 checks = 0;
    IOStorage *storage = media; // Exercise synchronous wrappers and real dispatch.
    auto input = IOBufferMemoryDescriptor::withCapacity(blockSize, kIODirectionIn);
    auto output = IOBufferMemoryDescriptor::withCapacity(blockSize, kIODirectionOut);
    bool opened = false;
    if (media->getSize() == blockSize * blockCount && media->getPreferredBlockSize() == blockSize &&
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
        status = storage->read(this, blockSize * blockCount, input, nullptr, &actual);
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
        status = storage->write(this, blockSize * blockCount, output, nullptr, &actual);
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
    blockCount = readOnly ? kFatTotalSectors : kWritableBlocks;
    UInt32 msdosfsMode = 0;
    requireBsdPath = PE_parse_boot_argn("source-msdosfs-test", &msdosfsMode, sizeof msdosfsMode) &&
                     msdosfsMode == 1 && readOnly;
    if (!gSourceStoragePathLock) gSourceStoragePathLock = IOLockAlloc();
    if (!gSourceStoragePathLock) return false;
    IOLockLock(gSourceStoragePathLock);
    if (gSourceStorageDevicePathReady) {
      IOLockUnlock(gSourceStoragePathLock);
      return false;
    }
    gSourceStorageDevicePath[0] = '\0';
    IOLockUnlock(gSourceStoragePathLock);
    gate = IOLockAlloc();
    memory = static_cast<uint8_t *>(IOMallocZero(blockSize * blockCount));
    if (!gate || !memory) {
      if (memory) {
        IOFree(memory, blockSize * blockCount);
        memory = nullptr;
      }
      if (gate) {
        IOLockFree(gate);
        gate = nullptr;
      }
      return false;
    }
    if (readOnly) {
      initialize_fat32(memory);
      for (UInt64 i = 0; i < blockSize; ++i) memory[8 * blockSize + i] = static_cast<uint8_t>((i * 17 + 91) & 255);
    }
    IOLockLock(gate);
    online = true;
    IOLockUnlock(gate);
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
    clearBsdPath();
    IOBlockStorageDevice::stop(provider);
  }
  void free() override {
    clearBsdPath();
    if (memory) IOFree(memory, blockSize * blockCount);
    if (gate) IOLockFree(gate);
    IOBlockStorageDevice::free();
  }
  IOReturn doAsyncReadWrite(IOMemoryDescriptor *buffer, UInt64 block, UInt64 count,
                          IOStorageAttributes *, IOStorageCompletion *completion) override {
    if (!buffer || !completion || !completion->action || !count || block >= blockCount || count > blockCount - block ||
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
    if (block >= blockCount || count > blockCount - block) return kIOReturnBadArgument;
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
  IOReturn reportMaxValidBlock(UInt64 *value) override { *value = blockCount - 1; return kIOReturnSuccess; }
  IOReturn reportEjectability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportRemovability(bool *value) override { *value = false; return kIOReturnSuccess; }
  IOReturn reportWriteProtection(bool *value) override { *value = readOnly; return kIOReturnSuccess; }
  IOReturn reportMediaState(bool *present, bool *changed) override {
    bool isOnline = false;
    if (gate) {
      IOLockLock(gate);
      isOnline = online;
      IOLockUnlock(gate);
    }
    *present = isOnline; if (changed) *changed = false; return kIOReturnSuccess;
  }
  IOReturn getWriteCacheState(bool *value) override { *value = true; return kIOReturnSuccess; }
  IOReturn setWriteCacheState(bool) override { return kIOReturnUnsupported; }
};
OSDefineMetaClassAndStructors(SourceStorageFixture, IOBlockStorageDevice)
