add_executable(BookLanguageFormatTest
  BookLanguageFormatTest.cpp
  BookLanguageReaderTest.cpp
  DefinitionPagerTest.cpp
  GrammarPresentationTest.cpp
  ${REPO_ROOT}/lib/Dictionary/BookLanguageFormat.cpp
  ${REPO_ROOT}/lib/Dictionary/BookLanguageReader.cpp
  ${REPO_ROOT}/lib/Dictionary/Crc32.cpp
  ${REPO_ROOT}/lib/Dictionary/DefinitionPager.cpp
  ${REPO_ROOT}/lib/Dictionary/GrammarPresentation.cpp
  ${REPO_ROOT}/lib/Dictionary/PageShortlist.cpp
)

target_include_directories(BookLanguageFormatTest PRIVATE
  ${REPO_ROOT}/lib/Dictionary
)

target_link_libraries(BookLanguageFormatTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookLanguageFormatTest)
