add_executable(SwitchingFileReaderTest
  SwitchingFileReaderTest.cpp
  ${REPO_ROOT}/lib/Dictionary/ContextualRuntimeFormat.cpp
  ${REPO_ROOT}/lib/Dictionary/DictionaryPackage.cpp
  ${REPO_ROOT}/lib/Dictionary/Crc32.cpp
)

target_include_directories(SwitchingFileReaderTest PRIVATE
  ${REPO_ROOT}/src/dictionary
  ${REPO_ROOT}/lib/Dictionary
)

target_link_libraries(SwitchingFileReaderTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(SwitchingFileReaderTest)
