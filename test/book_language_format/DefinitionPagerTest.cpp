#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string_view>

#include "DefinitionPager.h"

namespace {

struct EntryFixture {
  std::array<uint8_t, 64> bytes{};
  size_t length = 0;
};

EntryFixture makeEntry(const std::string_view text) {
  EntryFixture fixture;
  fixture.bytes[0] = 1;
  fixture.bytes[1] = 0;
  fixture.bytes[2] = 1;
  fixture.bytes[3] = 0;
  fixture.bytes[4] = 1;
  fixture.bytes[5] = 0;
  fixture.bytes[6] = static_cast<uint8_t>(text.size());
  fixture.bytes[7] = 0;
  std::memcpy(fixture.bytes.data() + 8, text.data(), text.size());
  fixture.length = 8 + text.size();
  return fixture;
}

bool readEntry(void* context, const dictionary::EntrySlice& entry, const uint32_t relativeOffset, void* output,
               const size_t capacity, size_t& bytesRead, dictionary::RuntimeError& error) {
  const auto& fixture = *static_cast<EntryFixture*>(context);
  bytesRead = 0;
  error = dictionary::RuntimeError::NONE;
  if (entry.offset != 0 || entry.length != fixture.length || relativeOffset > fixture.length) return false;
  bytesRead = std::min(capacity, fixture.length - relativeOffset);
  std::memcpy(output, fixture.bytes.data() + relativeOffset, bytesRead);
  return true;
}

int byteWidth(void*, const std::string_view text) { return static_cast<int>(text.size()); }

}  // namespace

TEST(DefinitionPager, ContinuesSequentiallyAcrossBoundedPages) {
  EntryFixture fixture = makeEntry("one\ntwo\nthree");
  const dictionary::definition::EntryReader reader{&fixture, readEntry};
  const dictionary::EntrySlice slice{0, static_cast<uint32_t>(fixture.length)};
  const dictionary::definition::WidthMeasurer measurer{nullptr, byteWidth};
  dictionary::definition::Pager pager;
  dictionary::definition::Page page;
  dictionary::definition::PagerError error = dictionary::definition::PagerError::NONE;

  ASSERT_TRUE(pager.load(reader, slice, {}, measurer, 40, 1, page, error));
  EXPECT_EQ(page.lineText(0), "one");
  ASSERT_TRUE(page.hasNext);
  const auto second = page.next;

  ASSERT_TRUE(pager.load(reader, slice, second, measurer, 40, 1, page, error));
  EXPECT_EQ(page.lineText(0), "two");
  ASSERT_TRUE(page.hasNext);
  const auto third = page.next;

  ASSERT_TRUE(pager.load(reader, slice, third, measurer, 40, 1, page, error));
  EXPECT_EQ(page.lineText(0), "three");
  EXPECT_FALSE(page.hasNext);
}
