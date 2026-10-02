#pragma once

// An append-only journal of fixed-size records.
//
// File layout:
//   FileHeader                      (16 bytes)
//   [ Record bytes | CRC-32 ] ...   (sizeof(Record) + 4 bytes each)
//
// Records are written as raw memory, so a journal is only readable by a
// build with the same record layout and endianness; the header carries the
// record size to catch mismatches. A crash can leave a partial record at the
// end of the file. The reader stops at the first record that is short or
// fails its checksum and reports how many bytes were good, and the writer
// truncates to that point before appending again.

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "journal/crc32.h"

namespace exchange::journal {

inline constexpr std::array<char, 8> MAGIC = {'S', 'X', 'J', 'R', 'N', 'L', '0', '1'};
inline constexpr std::uint32_t FORMAT_VERSION = 1;

struct FileHeader {
  std::array<char, 8> magic;
  std::uint32_t version;
  std::uint32_t record_size;
};

static_assert(sizeof(FileHeader) == 16);

template <typename Record> class Writer {
  static_assert(std::is_trivially_copyable_v<Record>);

  static constexpr std::size_t FRAME_BYTES = sizeof(Record) + sizeof(std::uint32_t);
  static constexpr std::size_t BUFFER_BYTES = 64U * 1024U;

public:
  Writer() { buffer_.reserve(BUFFER_BYTES + FRAME_BYTES); }
  ~Writer() { close(); }

  Writer(const Writer &) = delete;
  Writer &operator=(const Writer &) = delete;

  static constexpr std::uint64_t KEEP_ALL = ~std::uint64_t{0};

  // Opens the journal for appending, creating it if needed. An existing file
  // must carry a matching header. The file is cut back to the last whole
  // record, or further to `valid_bytes` if given (use Reader::valid_bytes()
  // to drop records that failed their checksum).
  bool open(const std::string &path, std::uint64_t valid_bytes = KEEP_ALL) {
    close();
    fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd_ < 0) {
      return false;
    }

    struct stat info {};
    if (::fstat(fd_, &info) != 0) {
      return fail();
    }
    auto size = static_cast<std::uint64_t>(info.st_size);

    if (size < sizeof(FileHeader)) {
      // New (or hopelessly short) file: start over with a fresh header.
      FileHeader header{};
      header.magic = MAGIC;
      header.version = FORMAT_VERSION;
      header.record_size = sizeof(Record);
      if (::ftruncate(fd_, 0) != 0 || ::lseek(fd_, 0, SEEK_SET) < 0 ||
          !write_all(&header, sizeof(header))) {
        return fail();
      }
      size = sizeof(FileHeader);
    } else {
      FileHeader header{};
      if (::pread(fd_, &header, sizeof(header), 0) !=
              static_cast<ssize_t>(sizeof(header)) ||
          header.magic != MAGIC || header.version != FORMAT_VERSION ||
          header.record_size != sizeof(Record)) {
        return fail();
      }

      std::uint64_t keep =
          sizeof(FileHeader) +
          ((size - sizeof(FileHeader)) / FRAME_BYTES) * FRAME_BYTES;
      if (valid_bytes >= sizeof(FileHeader) && valid_bytes < keep) {
        keep = valid_bytes;
      }
      if (keep != size && ::ftruncate(fd_, static_cast<off_t>(keep)) != 0) {
        return fail();
      }
      size = keep;
    }

    if (::lseek(fd_, static_cast<off_t>(size), SEEK_SET) < 0) {
      return fail();
    }
    return true;
  }

  [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

  // Buffers one record. Data reaches the OS on flush() or when the buffer
  // fills, and the disk on sync().
  bool append(const Record &record) {
    if (fd_ < 0) {
      return false;
    }
    const auto *bytes = reinterpret_cast<const unsigned char *>(&record);
    const std::uint32_t checksum = crc32(bytes, sizeof(Record));
    const auto *checksum_bytes = reinterpret_cast<const char *>(&checksum);

    buffer_.insert(buffer_.end(), reinterpret_cast<const char *>(bytes),
                   reinterpret_cast<const char *>(bytes) + sizeof(Record));
    buffer_.insert(buffer_.end(), checksum_bytes, checksum_bytes + sizeof(checksum));
    ++records_written_;

    if (buffer_.size() >= BUFFER_BYTES) {
      return flush();
    }
    return true;
  }

  bool flush() {
    if (fd_ < 0) {
      return false;
    }
    if (buffer_.empty()) {
      return true;
    }
    const bool ok = write_all(buffer_.data(), buffer_.size());
    buffer_.clear();
    unsynced_ = true;
    return ok;
  }

  bool sync() {
    if (!flush()) {
      return false;
    }
    if (!unsynced_) {
      return true;
    }
    unsynced_ = false;
#if defined(__APPLE__)
    return ::fsync(fd_) == 0;
#else
    return ::fdatasync(fd_) == 0;
#endif
  }

  void close() {
    if (fd_ >= 0) {
      sync();
      ::close(fd_);
      fd_ = -1;
    }
    buffer_.clear();
  }

  [[nodiscard]] std::uint64_t records_written() const noexcept {
    return records_written_;
  }

private:
  bool fail() {
    ::close(fd_);
    fd_ = -1;
    return false;
  }

  bool write_all(const void *data, std::size_t size) {
    const char *cursor = static_cast<const char *>(data);
    while (size > 0) {
      const ssize_t written = ::write(fd_, cursor, size);
      if (written < 0) {
        if (errno == EINTR) {
          continue;
        }
        return false;
      }
      cursor += written;
      size -= static_cast<std::size_t>(written);
    }
    return true;
  }

  int fd_{-1};
  std::vector<char> buffer_;
  std::uint64_t records_written_{0};
  bool unsynced_{false};
};

enum class OpenStatus {
  OK,
  MISSING,    // no such file (a fresh start, not an error)
  BAD_HEADER, // not a journal, or written with a different record layout
  IO_ERROR,
};

template <typename Record> class Reader {
  static_assert(std::is_trivially_copyable_v<Record>);

  static constexpr std::size_t FRAME_BYTES = sizeof(Record) + sizeof(std::uint32_t);

public:
  Reader() = default;
  ~Reader() { close(); }

  Reader(const Reader &) = delete;
  Reader &operator=(const Reader &) = delete;

  OpenStatus open(const std::string &path) {
    close();
    file_ = std::fopen(path.c_str(), "rb");
    if (file_ == nullptr) {
      return errno == ENOENT ? OpenStatus::MISSING : OpenStatus::IO_ERROR;
    }

    FileHeader header{};
    const std::size_t got = std::fread(&header, 1, sizeof(header), file_);
    if (got == 0) {
      // Empty file: treat like a missing one, the writer will add a header.
      close();
      return OpenStatus::MISSING;
    }
    if (got != sizeof(header) || header.magic != MAGIC ||
        header.version != FORMAT_VERSION ||
        header.record_size != sizeof(Record)) {
      close();
      return OpenStatus::BAD_HEADER;
    }

    valid_bytes_ = sizeof(FileHeader);
    records_read_ = 0;
    tail_damaged_ = false;
    return OpenStatus::OK;
  }

  // Reads the next record. Returns false at the end of the file or at the
  // first damaged record; tail_damaged() tells the two apart.
  bool next(Record &record) {
    if (file_ == nullptr || tail_damaged_) {
      return false;
    }

    std::array<unsigned char, FRAME_BYTES> frame;
    const std::size_t got = std::fread(frame.data(), 1, frame.size(), file_);
    if (got == 0) {
      return false;
    }

    std::uint32_t stored = 0;
    std::memcpy(&stored, frame.data() + sizeof(Record), sizeof(stored));
    if (got != frame.size() || stored != crc32(frame.data(), sizeof(Record))) {
      tail_damaged_ = true;
      return false;
    }

    std::memcpy(&record, frame.data(), sizeof(Record));
    valid_bytes_ += FRAME_BYTES;
    ++records_read_;
    return true;
  }

  // File offset just past the last good record.
  [[nodiscard]] std::uint64_t valid_bytes() const noexcept { return valid_bytes_; }

  // True if reading stopped at a short or corrupt record rather than at EOF.
  [[nodiscard]] bool tail_damaged() const noexcept { return tail_damaged_; }

  [[nodiscard]] std::uint64_t records_read() const noexcept { return records_read_; }

  void close() {
    if (file_ != nullptr) {
      std::fclose(file_);
      file_ = nullptr;
    }
  }

private:
  std::FILE *file_{nullptr};
  std::uint64_t valid_bytes_{0};
  std::uint64_t records_read_{0};
  bool tail_damaged_{false};
};

} // namespace exchange::journal
