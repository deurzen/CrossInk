add_executable(AtomicFileTest
  AtomicFileTest.cpp
  FakeHalStorage.cpp
  ${REPO_ROOT}/lib/AtomicFile/AtomicFile.cpp
)

target_include_directories(AtomicFileTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/fakes
  ${REPO_ROOT}/lib/AtomicFile
)

target_link_libraries(AtomicFileTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AtomicFileTest)
