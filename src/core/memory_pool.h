#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <new>

namespace exchange::core {

    // Fixed-capacity object pool. All N slots live in one contiguous,
    // cache-line-aligned allocation made up front; allocate() / deallocate()
    // are O(1) pointer operations on an intrusive free list.
    //
    // allocate() returns raw storage: the caller constructs the object with
    // std::construct_at. deallocate() destroys the object and recycles the slot.
    template <typename T, std::size_t N>
    class MemoryPool {
        static_assert(N > 0, "MemoryPool capacity must be positive.");
        static_assert(sizeof(T) >= sizeof(void *), "Object storage must fit an intrusive free-list pointer.");

        struct alignas((alignof(T) > 64U) ? alignof(T) : 64U) Slot {
            std::array<std::byte, sizeof(T)> storage{};
        };

        struct FreeNode {
            FreeNode *next;
        };

    public:
        MemoryPool() : slots_(std::make_unique<Slot[]>(N)) {
            initialize_free_list();
        }

        MemoryPool(const MemoryPool &) = delete;
        MemoryPool &operator=(const MemoryPool &) = delete;
        MemoryPool(MemoryPool &&) = delete;
        MemoryPool &operator=(MemoryPool &&) = delete;

        [[nodiscard]] T *allocate() noexcept {
            if(free_list_head_ == nullptr) [[unlikely]] {
                return nullptr;
            }

            FreeNode *head = free_list_head_;
            free_list_head_ = head->next;
            ++allocated_count_;
            return reinterpret_cast<T *>(head);
        }

        void deallocate(T *object) noexcept {
            if(object == nullptr) [[unlikely]] {
                return;
            }

            std::destroy_at(object);
            free_list_head_ = ::new (static_cast<void *>(object)) FreeNode{ free_list_head_ };
            --allocated_count_;
        }

        [[nodiscard]] std::size_t allocated() const noexcept {
            return allocated_count_;
        }

        [[nodiscard]] std::size_t available() const noexcept {
            return N - allocated_count_;
        }

        [[nodiscard]] static constexpr std::size_t capacity() noexcept {
            return N;
        }

        // True if the pointer refers to a slot inside this pool.
        [[nodiscard]] bool owns(const T *object) const noexcept {
            const auto *address = reinterpret_cast<const std::byte *>(object);
            const auto *first = reinterpret_cast<const std::byte *>(slots_.get());
            const auto *last = first + (N * sizeof(Slot));
            return address >= first && address < last;
        }

    private:
        void initialize_free_list() noexcept {
            // Link back to front so the first allocation hands out slot 0 and
            // early allocations walk the array in address order.
            free_list_head_ = nullptr;
            for(std::size_t index = N; index > 0; --index) {
                void *storage = slots_[index - 1].storage.data();
                free_list_head_ = ::new (storage) FreeNode{ free_list_head_ };
            }
        }

        std::unique_ptr<Slot[]> slots_;

        FreeNode *free_list_head_{nullptr};

        std::size_t allocated_count_{0};
    };
}
