find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME ContextualDictionary.ZdlModelPin
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_zdl_model_pin.py
)

add_test(
  NAME ContextualDictionary.ZdlAdapter
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_zdl_adapter.py
)

add_test(
  NAME ContextualDictionary.ZdlBenchmark
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_zdl_benchmark.py
)

add_test(
  NAME ContextualDictionary.DwdsmorPin
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_dwdsmor_pin.py
)

add_test(
  NAME ContextualDictionary.DwdsmorAdapter
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_dwdsmor_adapter.py
)

add_test(
  NAME ContextualDictionary.FormInventory
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_form_inventory.py
)

add_test(
  NAME ContextualDictionary.AnalysisPolicy
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_analysis_policy.py
)

add_test(
  NAME ContextualDictionary.AnalyzerPipeline
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_analyzer_pipeline.py
)

add_test(
  NAME ContextualDictionary.Fusion
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_fusion.py
)

add_test(
  NAME ContextualDictionary.SeparableVerbs
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_separable_verbs.py
)

add_test(
  NAME ContextualDictionary.AmbiguityCorpus
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_ambiguity_corpus.py
)

add_test(
  NAME ContextualDictionary.Formats
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_contextual_formats.py
)

add_test(
  NAME ContextualDictionary.CanonicalLexicon
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_canonical_lexicon.py
)

add_test(
  NAME ContextualDictionary.EpubCompiler
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_contextual_epub_compiler.py
)

add_test(
  NAME ContextualDictionary.CompilerDifferential
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_contextual_compiler_differential.py
)

add_test(
  NAME ContextualDictionary.DefinitionSource
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_definition_source.py
)

add_test(
  NAME ContextualDictionary.DeDeDefinitions
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_de_de_definitions.py
)

add_test(
  NAME ContextualDictionary.DictCcDefinitions
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_dictcc_definitions.py
)

add_test(
  NAME ContextualDictionary.KaikkiDefinitions
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_kaikki_definitions.py
)

add_test(
  NAME ContextualDictionary.CoverageReport
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_coverage_report.py
)
