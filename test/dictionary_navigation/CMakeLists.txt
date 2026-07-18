add_executable(DictionaryNavigationTest
  DictionaryNavigationTest.cpp
  ${REPO_ROOT}/lib/Dictionary/DictionaryNavigation.cpp
)

target_include_directories(DictionaryNavigationTest PRIVATE
  ${REPO_ROOT}/lib/Dictionary
)

target_link_libraries(DictionaryNavigationTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryNavigationTest)
