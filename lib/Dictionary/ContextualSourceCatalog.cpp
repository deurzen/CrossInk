#include "ContextualSourceCatalog.h"

#include <cstring>

namespace dictionary::contextual {
namespace {

bool hasNonzeroByte(const uint8_t* data, const size_t length) {
  for (size_t index = 0; index < length; ++index) {
    if (data[index] != 0) return true;
  }
  return false;
}

bool metadataMatches(const DefinitionMetadata& metadata, const uint8_t (&sourceUuid)[16],
                     const uint8_t (&canonicalUuid)[16], const uint32_t canonicalCount) {
  return std::memcmp(metadata.sourceUuid, sourceUuid, sizeof(metadata.sourceUuid)) == 0 &&
         std::memcmp(metadata.canonicalUuid, canonicalUuid, sizeof(metadata.canonicalUuid)) == 0 &&
         metadata.canonicalLexemeCount == canonicalCount;
}

}  // namespace

static_assert(sizeof(DefinitionSourceCatalog) <= 384,
              "three retained definition descriptors must remain below 384 bytes");

bool buildDefinitionSourceCatalog(const AttachmentRecord& attachments, const uint8_t (&expectedCanonicalUuid)[16],
                                  const uint32_t expectedCanonicalCount, const DefinitionMetadataLoader loader,
                                  void* loaderContext, DefinitionSourceCatalog& output, SourceCatalogError& error) {
  output = {};
  error = SourceCatalogError::NONE;
  if (!hasNonzeroByte(expectedCanonicalUuid, sizeof(expectedCanonicalUuid)) || expectedCanonicalCount == 0 ||
      attachments.sourceCount > kMaxAttachedSources || loader == nullptr) {
    error = SourceCatalogError::INVALID_INPUT;
    return false;
  }
  if (std::memcmp(attachments.canonicalUuid, expectedCanonicalUuid, sizeof(attachments.canonicalUuid)) != 0) {
    error = SourceCatalogError::CANONICAL_MISMATCH;
    return false;
  }

  output.attachmentGeneration = attachments.generation;
  output.attachedCount = attachments.sourceCount;
  for (uint8_t sourceIndex = 0; sourceIndex < attachments.sourceCount; ++sourceIndex) {
    DefinitionMetadata metadata;
    const auto& sourceUuid = *reinterpret_cast<const uint8_t (*)[16]>(attachments.sourceUuids[sourceIndex]);
    if (!loader(loaderContext, sourceUuid, metadata) ||
        !metadataMatches(metadata, sourceUuid, expectedCanonicalUuid, expectedCanonicalCount)) {
      ++output.skippedCount;
      continue;
    }
    output.sources[output.sourceCount++] = metadata;
  }
  return true;
}

const char* sourceCatalogErrorName(const SourceCatalogError error) {
  switch (error) {
    case SourceCatalogError::NONE:
      return "none";
    case SourceCatalogError::INVALID_INPUT:
      return "invalid input";
    case SourceCatalogError::CANONICAL_MISMATCH:
      return "canonical mismatch";
  }
  return "unknown";
}

}  // namespace dictionary::contextual
