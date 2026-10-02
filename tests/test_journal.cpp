#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <unistd.h>

#include "journal/crc32.h"
#include "journal/journal.h"
#include "test_framework.h"

using namespace exchange::journal;

namespace {

struct Sample {
  std::uint64_t id;
  std::int64_t value;
  std::uint32_t kind;
  std::uint32_t pad;

  bool operator==(const Sample &) const = default;
};

struct OtherLayout {
  std::uint64_t only;
};

Sample sample(std::uint64_t id) {
  return Sample{.id = id, .value = -static_cast<std::int64_t>(id) * 3, .kind = 7, .pad = 0};
}

struct TempFile {
  std::string path;
  explicit TempFile(const char *tag)
      : path("/tmp/sx_journal_test_" + std::string(tag) + "_" +
             std::to_string(::getpid())) {
    std::remove(path.c_str());
  }
  ~TempFile() { std::remove(path.c_str()); }
};

std::vector<Sample> read_all(const std::string &path, bool *damaged = nullptr,
                             std::uint64_t *valid = nullptr) {
  std::vector<Sample> out;
  Reader<Sample> reader;
  if (reader.open(path) != OpenStatus::OK) {
    return out;
  }
  Sample record{};
  while (reader.next(record)) {
    out.push_back(record);
  }
  if (damaged != nullptr) {
    *damaged = reader.tail_damaged();
  }
  if (valid != nullptr) {
    *valid = reader.valid_bytes();
  }
  return out;
}

long file_size(const std::string &path) {
  std::FILE *file = std::fopen(path.c_str(), "rb");
  if (file == nullptr) {
    return -1;
  }
  std::fseek(file, 0, SEEK_END);
  const long size = std::ftell(file);
  std::fclose(file);
  return size;
}

void patch_byte(const std::string &path, long offset, unsigned char value) {
  std::FILE *file = std::fopen(path.c_str(), "r+b");
  std::fseek(file, offset, SEEK_SET);
  std::fputc(value, file);
  std::fclose(file);
}

constexpr long FRAME = sizeof(Sample) + 4;

} // namespace

TEST(crc32_matches_the_standard_check_value) {
  const unsigned char text[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK_EQ(crc32(text, sizeof(text)), 0xCBF43926U);
  CHECK_EQ(crc32(text, 0), 0U);
}

TEST(records_round_trip) {
  TempFile file("roundtrip");
  {
    Writer<Sample> writer;
    REQUIRE(writer.open(file.path));
    for (std::uint64_t i = 0; i < 10'000; ++i) { // spans several buffer flushes
      CHECK(writer.append(sample(i)));
    }
    CHECK(writer.sync());
    CHECK_EQ(writer.records_written(), 10'000U);
  }

  bool damaged = true;
  const auto records = read_all(file.path, &damaged);
  REQUIRE_EQ(records.size(), 10'000U);
  CHECK(!damaged);
  for (std::uint64_t i = 0; i < records.size(); ++i) {
    CHECK(records[i] == sample(i));
  }
  CHECK_EQ(file_size(file.path), 16 + 10'000 * FRAME);
}

TEST(missing_and_foreign_files_are_reported) {
  TempFile file("missing");
  Reader<Sample> reader;
  CHECK(reader.open(file.path) == OpenStatus::MISSING);

  {
    Writer<Sample> writer;
    REQUIRE(writer.open(file.path));
    writer.append(sample(1));
  }
  Reader<OtherLayout> other;
  CHECK(other.open(file.path) == OpenStatus::BAD_HEADER);
  Writer<OtherLayout> other_writer;
  CHECK(!other_writer.open(file.path)); // refuses to append to a foreign layout
  CHECK_EQ(read_all(file.path).size(), 1U); // and leaves it untouched
}

TEST(reopening_appends) {
  TempFile file("append");
  for (std::uint64_t round = 0; round < 3; ++round) {
    Writer<Sample> writer;
    REQUIRE(writer.open(file.path));
    writer.append(sample(round * 2));
    writer.append(sample(round * 2 + 1));
  }
  const auto records = read_all(file.path);
  REQUIRE_EQ(records.size(), 6U);
  for (std::uint64_t i = 0; i < 6; ++i) {
    CHECK_EQ(records[i].id, i);
  }
}

TEST(torn_tail_is_ignored_and_then_overwritten) {
  TempFile file("torn");
  {
    Writer<Sample> writer;
    REQUIRE(writer.open(file.path));
    for (std::uint64_t i = 0; i < 5; ++i) {
      writer.append(sample(i));
    }
  }
  { // Simulate a crash half-way through writing a sixth record.
    std::FILE *raw = std::fopen(file.path.c_str(), "ab");
    const char junk[11] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};
    std::fwrite(junk, 1, sizeof(junk), raw);
    std::fclose(raw);
  }

  bool damaged = false;
  std::uint64_t valid = 0;
  CHECK_EQ(read_all(file.path, &damaged, &valid).size(), 5U);
  CHECK(damaged);
  CHECK_EQ(valid, static_cast<std::uint64_t>(16 + 5 * FRAME));

  {
    Writer<Sample> writer;
    REQUIRE(writer.open(file.path, valid));
    writer.append(sample(5));
  }
  damaged = true;
  const auto records = read_all(file.path, &damaged);
  REQUIRE_EQ(records.size(), 6U);
  CHECK(!damaged);
  CHECK_EQ(records[5].id, 5U);
  CHECK_EQ(file_size(file.path), 16 + 6 * FRAME);
}

TEST(corruption_stops_the_reader_at_the_damaged_record) {
  TempFile file("corrupt");
  {
    Writer<Sample> writer;
    REQUIRE(writer.open(file.path));
    for (std::uint64_t i = 0; i < 8; ++i) {
      writer.append(sample(i));
    }
  }
  patch_byte(file.path, 16 + 3 * FRAME + 2, 0xFF); // inside the fourth record

  bool damaged = false;
  std::uint64_t valid = 0;
  const auto records = read_all(file.path, &damaged, &valid);
  CHECK_EQ(records.size(), 3U);
  CHECK(damaged);

  // Recovery keeps the good prefix and continues from there.
  {
    Writer<Sample> writer;
    REQUIRE(writer.open(file.path, valid));
    writer.append(sample(100));
  }
  const auto after = read_all(file.path, &damaged);
  REQUIRE_EQ(after.size(), 4U);
  CHECK(!damaged);
  CHECK_EQ(after[3].id, 100U);
}

TEST_MAIN()
