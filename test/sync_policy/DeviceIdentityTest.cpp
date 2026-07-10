#include <Crc32.h>
#include <DeviceIdentity.h>
#include <HalStorage.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <string>

namespace {
using namespace DeviceSync;

struct FakePlatform {
  std::array<uint8_t, DeviceIdentity::FACTORY_MAC_SIZE> mac{0x10, 0x20, 0x30, 0x40, 0x50, 0x60};
  uint8_t randomSeed = 0x80;
  size_t randomCalls = 0;
  bool failMac = false;
  bool failRandom = false;
  bool failHash = false;
};

bool readFactoryMac(void* context, uint8_t output[DeviceIdentity::FACTORY_MAC_SIZE]) {
  auto* platform = static_cast<FakePlatform*>(context);
  if (platform->failMac) return false;
  std::memcpy(output, platform->mac.data(), platform->mac.size());
  return true;
}

bool fillRandom(void* context, void* output, const size_t length) {
  auto* platform = static_cast<FakePlatform*>(context);
  ++platform->randomCalls;
  if (platform->failRandom) return false;
  auto* bytes = static_cast<uint8_t*>(output);
  for (size_t i = 0; i < length; ++i) bytes[i] = static_cast<uint8_t>(platform->randomSeed + i);
  return true;
}

bool fakeSha256(void* context, const void* data, const size_t length, uint8_t output[DeviceIdentity::SHA256_SIZE]) {
  auto* platform = static_cast<FakePlatform*>(context);
  if (platform->failHash) return false;
  const auto* bytes = static_cast<const uint8_t*>(data);
  uint32_t state = 2166136261U;
  for (size_t i = 0; i < length; ++i) state = (state ^ bytes[i]) * 16777619U;
  for (size_t i = 0; i < DeviceIdentity::SHA256_SIZE; ++i) {
    state = state * 1664525U + 1013904223U;
    output[i] = static_cast<uint8_t>(state >> 24);
  }
  return true;
}

DeviceIdentity::Platform callbacks(FakePlatform& platform) {
  return DeviceIdentity::Platform{&platform, readFactoryMac, fillRandom, fakeSha256};
}

bool isZero(const DeviceId& id) {
  uint8_t combined = 0;
  for (const uint8_t byte : id) combined |= byte;
  return combined == 0;
}

class DeviceIdentityTest : public ::testing::Test {
 protected:
  void SetUp() override { Storage.reset(); }
};

TEST(Crc32Test, MatchesStandardVectorAcrossChunks) {
  Crc32 crc;
  crc.update("1234", 4);
  crc.update("56789", 5);
  EXPECT_EQ(crc.value(), 0xCBF43926U);
}

TEST(DeviceIdentityPlatformTest, SimulatorProvidesMacRandomAndSha256) {
  const DeviceIdentity::Platform platform = DeviceIdentity::systemPlatform();
  uint8_t mac[DeviceIdentity::FACTORY_MAC_SIZE] = {};
  uint8_t random[DeviceIdentity::SALT_SIZE] = {};
  uint8_t digest[DeviceIdentity::SHA256_SIZE] = {};
  ASSERT_TRUE(platform.readFactoryMac(platform.context, mac));
  ASSERT_TRUE(platform.fillRandom(platform.context, random, sizeof(random)));
  ASSERT_TRUE(platform.sha256(platform.context, "abc", 3, digest));

  EXPECT_FALSE(std::all_of(std::begin(mac), std::end(mac), [](const uint8_t byte) { return byte == 0; }));
  EXPECT_FALSE(std::all_of(std::begin(random), std::end(random), [](const uint8_t byte) { return byte == 0; }));
  static constexpr uint8_t ABC_SHA256[] = {0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
                                           0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
                                           0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
  EXPECT_EQ(std::memcmp(digest, ABC_SHA256, sizeof(digest)), 0);
}

TEST_F(DeviceIdentityTest, CreatesThenLoadsStableIdentityWithoutNewRandomness) {
  FakePlatform platform;
  DeviceId created{};
  ASSERT_EQ(DeviceIdentity::loadOrCreate(created, callbacks(platform)), DeviceIdentity::LoadResult::Created);
  EXPECT_FALSE(isZero(created));
  EXPECT_EQ(platform.randomCalls, 1u);
  EXPECT_TRUE(Storage.exists(DeviceIdentity::IDENTITY_PATH));

  DeviceId loaded{};
  ASSERT_EQ(DeviceIdentity::loadOrCreate(loaded, callbacks(platform)), DeviceIdentity::LoadResult::Loaded);
  EXPECT_EQ(loaded, created);
  EXPECT_EQ(platform.randomCalls, 1u);
}

TEST_F(DeviceIdentityTest, SameSaltOnDifferentHardwareProducesDifferentIdentity) {
  FakePlatform firstPlatform;
  DeviceId first{};
  ASSERT_EQ(DeviceIdentity::loadOrCreate(first, callbacks(firstPlatform)), DeviceIdentity::LoadResult::Created);

  FakePlatform secondPlatform;
  secondPlatform.mac[5] ^= 0x01;
  DeviceId second{};
  ASSERT_EQ(DeviceIdentity::loadOrCreate(second, callbacks(secondPlatform)), DeviceIdentity::LoadResult::Loaded);
  EXPECT_NE(second, first);
}

TEST_F(DeviceIdentityTest, CorruptCommittedIdentityFailsClosed) {
  FakePlatform platform;
  DeviceId original{};
  ASSERT_EQ(DeviceIdentity::loadOrCreate(original, callbacks(platform)), DeviceIdentity::LoadResult::Created);
  std::string corrupt = *Storage.getFile(DeviceIdentity::IDENTITY_PATH);
  corrupt[20] ^= 0x80;
  Storage.reset();
  Storage.setFile(DeviceIdentity::IDENTITY_PATH, corrupt);

  DeviceId output;
  output.fill(0xFF);
  EXPECT_EQ(DeviceIdentity::loadOrCreate(output, callbacks(platform)), DeviceIdentity::LoadResult::Invalid);
  EXPECT_TRUE(isZero(output));
  EXPECT_EQ(*Storage.getFile(DeviceIdentity::IDENTITY_PATH), corrupt);
}

TEST_F(DeviceIdentityTest, PreservesExtendedFutureFormat) {
  FakePlatform platform;
  DeviceId original{};
  ASSERT_EQ(DeviceIdentity::loadOrCreate(original, callbacks(platform)), DeviceIdentity::LoadResult::Created);
  std::string future = *Storage.getFile(DeviceIdentity::IDENTITY_PATH);
  future[4] = 2;
  future[5] = 0;
  future.append(64, '\xA5');
  Storage.reset();
  Storage.setFile(DeviceIdentity::IDENTITY_PATH, future);

  DeviceId output;
  output.fill(0xFF);
  EXPECT_EQ(DeviceIdentity::loadOrCreate(output, callbacks(platform)), DeviceIdentity::LoadResult::Unsupported);
  EXPECT_TRUE(isZero(output));
  EXPECT_EQ(*Storage.getFile(DeviceIdentity::IDENTITY_PATH), future);
}

TEST_F(DeviceIdentityTest, EveryFirstWritePowerCutEventuallyCreatesSameIdentity) {
  FakePlatform baselinePlatform;
  DeviceId baseline{};
  ASSERT_EQ(DeviceIdentity::loadOrCreate(baseline, callbacks(baselinePlatform)), DeviceIdentity::LoadResult::Created);
  const size_t mutationCount = Storage.mutationCount();
  ASSERT_GT(mutationCount, 0u);

  for (size_t cut = 1; cut <= mutationCount; ++cut) {
    Storage.reset();
    FakePlatform platform;
    Storage.cutPowerAfterMutation(cut);
    DeviceId interrupted{};
    try {
      DeviceIdentity::loadOrCreate(interrupted, callbacks(platform));
    } catch (const FakePowerLoss&) {
    }
    Storage.disablePowerCut();

    DeviceId recovered{};
    const DeviceIdentity::LoadResult result = DeviceIdentity::loadOrCreate(recovered, callbacks(platform));
    EXPECT_TRUE(result == DeviceIdentity::LoadResult::Created || result == DeviceIdentity::LoadResult::Loaded) << cut;
    EXPECT_EQ(recovered, baseline) << cut;
  }
}

TEST_F(DeviceIdentityTest, PlatformFailuresDoNotPersistOrExposeIdentity) {
  FakePlatform platform;
  platform.failRandom = true;
  DeviceId output;
  output.fill(0xFF);

  EXPECT_EQ(DeviceIdentity::loadOrCreate(output, callbacks(platform)), DeviceIdentity::LoadResult::PlatformError);
  EXPECT_TRUE(isZero(output));
  EXPECT_FALSE(Storage.exists(DeviceIdentity::IDENTITY_PATH));
}

}  // namespace
