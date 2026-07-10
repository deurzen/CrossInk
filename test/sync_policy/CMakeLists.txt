add_executable(SyncPolicyTest
  SyncPolicyTest.cpp
  ${REPO_ROOT}/lib/DeviceSync/SyncPolicy.cpp
)

target_include_directories(SyncPolicyTest PRIVATE
  ${REPO_ROOT}/lib/DeviceSync
)

target_link_libraries(SyncPolicyTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SyncPolicyTest)
