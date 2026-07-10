add_executable(SyncPolicyTest
  SyncPolicyTest.cpp
  SyncPolicyCodecTest.cpp
  SyncPolicyStoreTest.cpp
  DeviceIdentityTest.cpp
  PairRecordTest.cpp
  PairRecordStoreTest.cpp
  PairHandshakeTest.cpp
  ${REPO_ROOT}/lib/DeviceSync/Crc32.cpp
  ${REPO_ROOT}/lib/DeviceSync/DeviceIdentity.cpp
  ${REPO_ROOT}/lib/DeviceSync/DeviceIdentityPlatform.cpp
  ${REPO_ROOT}/lib/DeviceSync/PairRecord.cpp
  ${REPO_ROOT}/lib/DeviceSync/PairRecordCodec.cpp
  ${REPO_ROOT}/lib/DeviceSync/PairRecordStore.cpp
  ${REPO_ROOT}/lib/DeviceSync/PairHandshake.cpp
  ${REPO_ROOT}/lib/DeviceSync/PairHandshakePlatform.cpp
  ${REPO_ROOT}/lib/DeviceSync/SyncPolicy.cpp
  ${REPO_ROOT}/lib/DeviceSync/SyncPolicyCodec.cpp
  ${REPO_ROOT}/lib/DeviceSync/SyncPolicyStore.cpp
  ${REPO_ROOT}/lib/AtomicFile/AtomicFile.cpp
  ${REPO_ROOT}/test/atomic_file/FakeHalStorage.cpp
)

target_include_directories(SyncPolicyTest BEFORE PRIVATE
  ${REPO_ROOT}/test/atomic_file/fakes
  ${REPO_ROOT}/lib/DeviceSync
  ${REPO_ROOT}/lib/AtomicFile
)

find_package(OpenSSL REQUIRED)

target_compile_definitions(SyncPolicyTest PRIVATE SIMULATOR)

target_link_libraries(SyncPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  OpenSSL::Crypto
)

gtest_discover_tests(SyncPolicyTest)
