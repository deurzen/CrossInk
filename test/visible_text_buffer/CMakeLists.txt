add_executable(VisibleTextBufferTest
  VisibleTextBufferTest.cpp
  ${REPO_ROOT}/src/word_inbox/VisibleTextBuffer.cpp
)

target_include_directories(VisibleTextBufferTest PRIVATE
  ${REPO_ROOT}/src/word_inbox
)

target_link_libraries(VisibleTextBufferTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(VisibleTextBufferTest)
