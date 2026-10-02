#pragma once

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <memory_resource>
#include <new>

namespace exchange::core {

    // A std::pmr memory resource backed by one fixed buffer allocated up front.
    //
    // Small blocks (the node types of std::pmr::map / multimap / unordered_map)
    // are recycled through per-size free lists, so a container that inserts and
    // erases forever stays inside the arena as long as its live size is bounded.
    // That is the difference from std::pmr::monotonic_buffer_resource, which
    // never reuses freed memory and therefore runs dry under order churn.
    //
    // Large blocks (a hash table's bucket array) are bump-allocated and not
    // recycled; they are expected to be allocated once at start-up.
    //
    // The arena never falls back to the heap. Running out of space is treated
    // as a sizing error and aborts.
    class FixedArenaResource final : public std::pmr::memory_resource {
        static constexpr std::size_t GRANULE = alignof(std::max_align_t);
        static constexpr std::size_t MAX_SMALL_BYTES = 256;
        static constexpr std::size_t CLASS_COUNT = MAX_SMALL_BYTES / GRANULE;

        struct FreeNode {
            FreeNode *next;
        };

    public:
        explicit FixedArenaResource(std::size_t bytes)
            : capacity_(round_up(bytes == 0 ? GRANULE : bytes, GRANULE)),
              storage_(static_cast<std::byte *>(
                  ::operator new(capacity_, std::align_val_t{GRANULE}))) {
            // Touch every page now so the first inserts do not page-fault.
            std::memset(storage_, 0, capacity_);
        }

        ~FixedArenaResource() override {
            ::operator delete(storage_, std::align_val_t{GRANULE});
        }

        FixedArenaResource(const FixedArenaResource &) = delete;
        FixedArenaResource &operator=(const FixedArenaResource &) = delete;
        FixedArenaResource(FixedArenaResource &&) = delete;
        FixedArenaResource &operator=(FixedArenaResource &&) = delete;

        [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

        // Bytes handed out from the bump pointer so far (high-water mark).
        [[nodiscard]] std::size_t high_water_mark() const noexcept { return offset_; }

    private:
        [[nodiscard]] static constexpr std::size_t round_up(std::size_t value, std::size_t multiple) noexcept {
            return ((value + multiple - 1U) / multiple) * multiple;
        }

        void *do_allocate(std::size_t bytes, std::size_t alignment) override {
            if(alignment > GRANULE) [[unlikely]] {
                fail("over-aligned allocation is not supported");
            }

            const std::size_t rounded = round_up(bytes == 0 ? 1 : bytes, GRANULE);
            if(rounded <= MAX_SMALL_BYTES) {
                FreeNode *&head = free_lists_[(rounded / GRANULE) - 1U];
                if(head != nullptr) {
                    FreeNode *node = head;
                    head = node->next;
                    return node;
                }
            }

            if(rounded > capacity_ - offset_) [[unlikely]] {
                fail("arena exhausted; increase the capacity it was sized from");
            }

            void *block = storage_ + offset_;
            offset_ += rounded;
            return block;
        }

        void do_deallocate(void *pointer, std::size_t bytes, std::size_t) override {
            const std::size_t rounded = round_up(bytes == 0 ? 1 : bytes, GRANULE);
            if(rounded > MAX_SMALL_BYTES) {
                return;
            }

            FreeNode *&head = free_lists_[(rounded / GRANULE) - 1U];
            head = ::new (pointer) FreeNode{ head };
        }

        [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource &other) const noexcept override {
            return this == &other;
        }

        [[noreturn]] static void fail(const char *message) noexcept {
            std::fputs("FixedArenaResource: ", stderr);
            std::fputs(message, stderr);
            std::fputc('\n', stderr);
            std::abort();
        }

        std::size_t capacity_;
        std::byte *storage_;
        std::size_t offset_{0};
        std::array<FreeNode *, CLASS_COUNT> free_lists_{};
    };
}
