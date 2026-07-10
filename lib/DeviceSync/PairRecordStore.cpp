#include "PairRecordStore.h"

#include <AtomicFile.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstring>
#include <limits>

#include "PairRecordCodec.h"

namespace DeviceSync {
namespace {
constexpr const char* LOG_MODULE = "PAIR_STORE";
constexpr char PEER_DIRECTORY[] = "/.crosspoint/device-sync/peers";
constexpr char PEER_PATH_PREFIX[] = "/.crosspoint/device-sync/peers/";
constexpr char HEX_DIGITS[] = "0123456789abcdef";

struct OperationContext {
  const DeviceId& expectedPeerDeviceId;
  const PairRecord* writeRecord = nullptr;
};

struct FileInput {
  HalFile& file;
};

bool readExact(void* context, void* data, const size_t length) {
  auto* input = static_cast<FileInput*>(context);
  return input->file.read(data, length) == static_cast<int>(length);
}

bool writeExact(void* context, const void* data, const size_t length) {
  auto* file = static_cast<HalFile*>(context);
  return file->write(data, length) == length;
}

PairRecordCodec::DecodeResult readRecord(const char* path, const DeviceId& expectedPeerDeviceId, PairRecord* record) {
  HalFile file;
  if (!Storage.openFileForRead(LOG_MODULE, path, file)) {
    LOG_ERR(LOG_MODULE, "Could not open pair record: %s", path);
    return PairRecordCodec::DecodeResult::Invalid;
  }

  const uint64_t fileSize = file.fileSize64();
  if (fileSize > std::numeric_limits<size_t>::max()) {
    LOG_ERR(LOG_MODULE, "Pair record is too large: %s", path);
    file.close();
    return PairRecordCodec::DecodeResult::Invalid;
  }

  FileInput fileInput{file};
  const CodecInput input{&fileInput, readExact, static_cast<size_t>(fileSize)};
  PairRecordCodec::DecodeResult result = record != nullptr ? PairRecordCodec::decode(input, *record)
                                                           : PairRecordCodec::validate(input, &expectedPeerDeviceId);
  if (result == PairRecordCodec::DecodeResult::Ok && record != nullptr &&
      record->peerDeviceId != expectedPeerDeviceId) {
    record->reset();
    result = PairRecordCodec::DecodeResult::Invalid;
  }
  if (!file.close()) {
    LOG_ERR(LOG_MODULE, "Could not close pair record: %s", path);
    if (record != nullptr) record->reset();
    return PairRecordCodec::DecodeResult::Invalid;
  }
  return result;
}

AtomicFile::ValidationResult validateRecordFile(const char* path, const void* context) {
  if (context == nullptr) return AtomicFile::ValidationResult::Invalid;
  const auto* operation = static_cast<const OperationContext*>(context);
  switch (readRecord(path, operation->expectedPeerDeviceId, nullptr)) {
    case PairRecordCodec::DecodeResult::Ok:
      return AtomicFile::ValidationResult::Valid;
    case PairRecordCodec::DecodeResult::Unsupported:
      return AtomicFile::ValidationResult::Unsupported;
    case PairRecordCodec::DecodeResult::Invalid:
      return AtomicFile::ValidationResult::Invalid;
  }
  return AtomicFile::ValidationResult::Invalid;
}

bool writeRecord(HalFile& file, const void* context) {
  if (context == nullptr) return false;
  const auto* operation = static_cast<const OperationContext*>(context);
  if (operation->writeRecord == nullptr) return false;
  const CodecOutput output{&file, writeExact};
  return PairRecordCodec::encode(*operation->writeRecord, output);
}

bool allZero(const DeviceId& id) {
  uint8_t combined = 0;
  for (const uint8_t byte : id) combined |= byte;
  return combined == 0;
}

}  // namespace

bool PairRecordStore::preparePaths(const DeviceId& peerDeviceId) {
  finalPath_[0] = '\0';
  tempPath_[0] = '\0';
  backupPath_[0] = '\0';
  if (allZero(peerDeviceId)) return false;

  constexpr size_t prefixLength = sizeof(PEER_PATH_PREFIX) - 1;
  constexpr size_t hexLength = DEVICE_ID_SIZE * 2;
  constexpr char finalSuffix[] = ".bin";
  constexpr size_t finalLength = prefixLength + hexLength + sizeof(finalSuffix) - 1;
  static_assert(finalLength + 1 == FINAL_PATH_BYTES);
  static_assert(finalLength + sizeof(".tmp") == SIDECAR_PATH_BYTES);

  std::memcpy(finalPath_, PEER_PATH_PREFIX, prefixLength);
  size_t offset = prefixLength;
  for (const uint8_t byte : peerDeviceId) {
    finalPath_[offset++] = HEX_DIGITS[byte >> 4];
    finalPath_[offset++] = HEX_DIGITS[byte & 0x0F];
  }
  std::memcpy(finalPath_ + offset, finalSuffix, sizeof(finalSuffix));

  std::memcpy(tempPath_, finalPath_, finalLength);
  std::memcpy(tempPath_ + finalLength, ".tmp", sizeof(".tmp"));
  std::memcpy(backupPath_, finalPath_, finalLength);
  std::memcpy(backupPath_ + finalLength, ".bak", sizeof(".bak"));
  return true;
}

PairRecordStore::LoadResult PairRecordStore::load(const DeviceId& peerDeviceId, PairRecord& record) {
  record.reset();
  if (!preparePaths(peerDeviceId)) return LoadResult::Invalid;

  const AtomicFile::Paths paths{finalPath_, tempPath_, backupPath_};
  const OperationContext operation{peerDeviceId, nullptr};
  const bool hasCandidate = Storage.exists(finalPath_) || Storage.exists(tempPath_) || Storage.exists(backupPath_);
  if (!hasCandidate) return LoadResult::Missing;
  if (!AtomicFile::recover(LOG_MODULE, paths, validateRecordFile, &operation)) {
    static constexpr size_t CANDIDATE_COUNT = 3;
    const char* candidates[CANDIDATE_COUNT] = {finalPath_, tempPath_, backupPath_};
    for (const char* path : candidates) {
      if (Storage.exists(path) &&
          readRecord(path, peerDeviceId, nullptr) == PairRecordCodec::DecodeResult::Unsupported) {
        return LoadResult::Unsupported;
      }
    }
    // An incomplete first pairing has no authoritative secret. Remove only
    // that invalid temp; a backup may represent an older committed pairing.
    if (!Storage.exists(finalPath_) && Storage.exists(tempPath_) && !Storage.exists(backupPath_) &&
        readRecord(tempPath_, peerDeviceId, nullptr) == PairRecordCodec::DecodeResult::Invalid &&
        Storage.remove(tempPath_)) {
      return LoadResult::Missing;
    }
    return LoadResult::Invalid;
  }
  if (!Storage.exists(finalPath_)) return LoadResult::Missing;

  switch (readRecord(finalPath_, peerDeviceId, &record)) {
    case PairRecordCodec::DecodeResult::Ok:
      return LoadResult::Loaded;
    case PairRecordCodec::DecodeResult::Unsupported:
      return LoadResult::Unsupported;
    case PairRecordCodec::DecodeResult::Invalid:
      return LoadResult::Invalid;
  }
  record.reset();
  return LoadResult::Invalid;
}

bool PairRecordStore::save(const PairRecord& record) {
  if (!record.valid() || !preparePaths(record.peerDeviceId)) return false;
  if (!Storage.ensureDirectoryExists(PEER_DIRECTORY)) {
    LOG_ERR(LOG_MODULE, "Could not create pair record directory: %s", PEER_DIRECTORY);
    return false;
  }

  const AtomicFile::Paths paths{finalPath_, tempPath_, backupPath_};
  const OperationContext operation{record.peerDeviceId, &record};
  return AtomicFile::write(LOG_MODULE, paths, writeRecord, validateRecordFile, &operation);
}

bool PairRecordStore::remove(const DeviceId& peerDeviceId) {
  if (!preparePaths(peerDeviceId)) return false;
  return AtomicFile::remove(LOG_MODULE, AtomicFile::Paths{finalPath_, tempPath_, backupPath_});
}

}  // namespace DeviceSync
