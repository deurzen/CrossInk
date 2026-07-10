add_executable(BookLanguageFormatTest
  BookLanguageFormatTest.cpp
  ${REPO_ROOT}/lib/Dictionary/BookLanguageFormat.cpp
  ${REPO_ROOT}/lib/Dictionary/Crc32.cpp
)

target_include_directories(BookLanguageFormatTest PRIVATE
  ${REPO_ROOT}/lib/Dictionary
)

target_link_libraries(BookLanguageFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookLanguageFormatTest)
