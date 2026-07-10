#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace DeviceSync {

constexpr size_t DEVICE_ID_SIZE = 16;
constexpr size_t SESSION_ID_SIZE = 16;
using DeviceId = std::array<uint8_t, DEVICE_ID_SIZE>;
using SessionId = std::array<uint8_t, SESSION_ID_SIZE>;

enum class Category : uint8_t {
  BookContent,
  EpubProgress,
  XtcTxtProgress,
  Bookmarks,
  Clippings,
  PortableBookSettings,
  ReadingStats,
  FinishedState,
  RecentBooks,
  GlobalReaderPreferences,
  OpdsDefinitions,
  FontPackages,
  SleepImages,
  Screenshots,
  Count,
};

constexpr size_t CATEGORY_COUNT = static_cast<size_t>(Category::Count);

enum class Direction : uint8_t {
  Disabled,
  SendOnly,
  ReceiveOnly,
  Bidirectional,
};

constexpr bool allowsOutbound(const Direction direction) {
  return direction == Direction::SendOnly || direction == Direction::Bidirectional;
}

constexpr bool allowsInbound(const Direction direction) {
  return direction == Direction::ReceiveOnly || direction == Direction::Bidirectional;
}

}  // namespace DeviceSync
