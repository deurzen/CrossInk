#include <gtest/gtest.h>

#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "SwitchingFileReader.h"

namespace {

struct FakeStorage;

class FakeFile {
 public:
  uint64_t fileSize64() const { return data_ ? data_->size() : 0; }

  bool seek(const size_t offset) {
    if (!data_ || offset > data_->size()) return false;
    position_ = offset;
    return true;
  }

  int read(void* output, const size_t length) {
    if (!data_ || position_ + length > data_->size()) return -1;
    std::memcpy(output, data_->data() + position_, length);
    position_ += length;
    return static_cast<int>(length);
  }

  bool close();
  bool isOpen() const { return data_ != nullptr; }

  void attach(FakeStorage* storage, const std::vector<uint8_t>* data) {
    storage_ = storage;
    data_ = data;
    position_ = 0;
  }

 private:
  FakeStorage* storage_ = nullptr;
  const std::vector<uint8_t>* data_ = nullptr;
  size_t position_ = 0;
};

struct FakeStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  uint32_t opens = 0;
  uint32_t closes = 0;
};

bool FakeFile::close() {
  if (!data_) return false;
  ++storage_->closes;
  storage_ = nullptr;
  data_ = nullptr;
  position_ = 0;
  return true;
}

bool openFake(void* context, const char* path, FakeFile& file) {
  auto& storage = *static_cast<FakeStorage*>(context);
  ++storage.opens;
  const auto item = storage.files.find(path);
  if (item == storage.files.end()) return false;
  file.attach(&storage, &item->second);
  return true;
}

}  // namespace

TEST(SwitchingFileReader, ReusesOneHandleUntilTheSourceChanges) {
  FakeStorage storage{{{"/a", {1, 2, 3, 4}}, {"/b", {5, 6, 7}}}};
  dictionary::io_metrics::Counters metrics;
  dictionary::io::SwitchingFileReader<FakeFile> reader(&storage, openFake);

  uint64_t size = 0;
  ASSERT_TRUE(reader.fileSize("/a", 1, &metrics, size));
  EXPECT_EQ(size, 4U);
  uint8_t output[2]{};
  ASSERT_TRUE(reader.readAt("/a", 1, size, 0, output, sizeof(output), &metrics));
  EXPECT_EQ(output[0], 1);
  ASSERT_TRUE(reader.readAt("/a", 1, size, 2, output, sizeof(output), &metrics));
  EXPECT_EQ(output[0], 3);
  EXPECT_EQ(storage.opens, 1U);
  EXPECT_EQ(storage.closes, 0U);
  EXPECT_EQ(metrics.openAttempts, 1U);
  EXPECT_EQ(metrics.sourceSwitches, 0U);
  EXPECT_EQ(metrics.readCalls, 2U);
  EXPECT_EQ(metrics.bytesRead, 4U);

  ASSERT_TRUE(reader.fileSize("/b", 2, &metrics, size));
  EXPECT_EQ(size, 3U);
  EXPECT_EQ(storage.opens, 2U);
  EXPECT_EQ(storage.closes, 1U);
  EXPECT_EQ(metrics.openAttempts, 2U);
  EXPECT_EQ(metrics.sourceSwitches, 1U);

  reader.close();
  EXPECT_EQ(storage.closes, 2U);
  EXPECT_EQ(reader.activeSourceToken(), 0U);
}

TEST(SwitchingFileReader, FailedSwitchLeavesNoReaderOpenAndCanRetry) {
  FakeStorage storage{{{"/a", {1, 2, 3, 4}}}};
  dictionary::io_metrics::Counters metrics;
  dictionary::io::SwitchingFileReader<FakeFile> reader(&storage, openFake);

  uint64_t size = 0;
  ASSERT_TRUE(reader.fileSize("/a", 1, &metrics, size));
  EXPECT_FALSE(reader.fileSize("/missing", 2, &metrics, size));
  EXPECT_EQ(reader.activeSourceToken(), 0U);
  EXPECT_EQ(storage.closes, 1U);

  ASSERT_TRUE(reader.fileSize("/a", 1, &metrics, size));
  EXPECT_EQ(size, 4U);
  EXPECT_EQ(storage.opens, 3U);
  EXPECT_EQ(metrics.openAttempts, 3U);
  EXPECT_EQ(metrics.sourceSwitches, 2U);
}
