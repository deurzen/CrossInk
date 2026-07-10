find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME DictionaryBundleCompiler.Python
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_dictionary_bundle_compiler.py
)
