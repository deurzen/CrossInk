add_executable(LexemeStateTest
  LexemeStateTest.cpp
  ${REPO_ROOT}/lib/Dictionary/LexemeStateStore.cpp
  ${REPO_ROOT}/lib/Dictionary/Crc32.cpp
)

target_include_directories(LexemeStateTest PRIVATE
  ${REPO_ROOT}/lib/Dictionary
)

target_link_libraries(LexemeStateTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(LexemeStateTest)
