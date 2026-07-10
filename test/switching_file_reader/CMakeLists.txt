add_executable(SwitchingFileReaderTest
  SwitchingFileReaderTest.cpp
)

target_include_directories(SwitchingFileReaderTest PRIVATE
  ${REPO_ROOT}/src/dictionary
)

target_link_libraries(SwitchingFileReaderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SwitchingFileReaderTest)
