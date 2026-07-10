add_executable(DictionaryPackageTest
  DictionaryPackageTest.cpp
  ${REPO_ROOT}/lib/Dictionary/Crc32.cpp
  ${REPO_ROOT}/lib/Dictionary/DictionaryPackage.cpp
  ${REPO_ROOT}/lib/Dictionary/DefinitionPager.cpp
)

target_include_directories(DictionaryPackageTest PRIVATE
  ${REPO_ROOT}/lib/Dictionary
)

target_link_libraries(DictionaryPackageTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryPackageTest)
