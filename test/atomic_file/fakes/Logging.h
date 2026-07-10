#pragma once

namespace fake_logging {
template <typename... Args>
void ignore(Args&&...) {}
}  // namespace fake_logging

#define LOG_DBG(...) fake_logging::ignore(__VA_ARGS__)
#define LOG_ERR(...) fake_logging::ignore(__VA_ARGS__)
