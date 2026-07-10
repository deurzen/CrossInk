add_executable(DictionaryInstallTest
  DictionaryInstallTest.cpp
  ${REPO_ROOT}/lib/Dictionary/DictionaryInstaller.cpp
  ${REPO_ROOT}/lib/Dictionary/DictionaryPackage.cpp
  ${REPO_ROOT}/lib/Dictionary/Crc32.cpp
)

target_include_directories(DictionaryInstallTest PRIVATE
  ${REPO_ROOT}/lib/Dictionary
)

target_link_libraries(DictionaryInstallTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(DictionaryInstallTest)
