#pragma once

#include <array>
#include <cstddef>
#include <memory>

namespace exchange::core {

    template <typename T, std::size_t N>

    class MemoryPool {
        static_assert(N > 0, "MemoryPool capacity must be positive.");
        static_assert(sizeof(T) >= sizeof(void *), "Object storage must fit an intrusive free-list pointer.");

        struct alignas((alignof(T) > 64U)a ? alignof(T) : 64U) Slot {
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

        T *allocate() noexcept {
            if(free_list_head_ == nullptr) [[unlikely]] {
                return nullptr;
            }

            FreeNode *head = free_list_head_;
            free_list_head_ = free_list_head_->next;
            ++allocated_count_;
            return reinterpret_cast<T *>(node);
        }

        void deallocate(T *object) noexcept {
            if(object == nullptr) [[unlikely]] {
                return;
            }

            std::destroy_at(object);
            auto *node = reinterpret_cast<FreeNode *>(object);
            node->next = free_list_head_;
            free_list_head_ = node;
            --allocated_count_;
        }


    private:
        void initialize_free_list() noexcept {
            free_list_head = nullptr;
            for(std::size_t index = 0; index < N; ++index) {
                auto *node = reinterpret_cast<FreeNode *>(slots_[index].storage.data());
            }
        }

        std::unique_ptr<Slot[]> slots_;

        FreeNode *free_list_head_{nullptr};

        std::size_t allocated_count_{0};
    };
}