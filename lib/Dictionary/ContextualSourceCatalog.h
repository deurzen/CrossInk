#pragma once

#include <cstddef>
#include <cstdint>

#include "ContextualAttachments.h"
#include "ContextualRuntimeFormat.h"

namespace dictionary::contextual {

enum class SourceCatalogError : uint8_t {
  NONE = 0,
  INVALID_INPUT,
  CANONICAL_MISMATCH,
};

using DefinitionMetadataLoader = bool (*)(void* context, const uint8_t (&sourceUuid)[16], DefinitionMetadata& output);

struct DefinitionSourceCatalog {
  DefinitionMetadata sources[kMaxAttachedSources]{};
  uint32_t attachmentGeneration = 0;
  uint8_t attachedCount = 0;
  uint8_t sourceCount = 0;
  uint8_t skippedCount = 0;
};

// Builds a compact, attachment-ordered snapshot. Missing or incompatible
// sources are skipped without shifting the relative order of valid sources.
bool buildDefinitionSourceCatalog(const AttachmentRecord& attachments, const uint8_t (&expectedCanonicalUuid)[16],
                                  uint32_t expectedCanonicalCount, DefinitionMetadataLoader loader, void* loaderContext,
                                  DefinitionSourceCatalog& output, SourceCatalogError& error);

const char* sourceCatalogErrorName(SourceCatalogError error);

}  // namespace dictionary::contextual
