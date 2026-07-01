#pragma once

#include <cstddef>
#include <duckdb/common/allocator.hpp>
#include <functional>
#include <limits>
#include <new>
#include <type_traits>

namespace logsearch::utils {

//! STL-compatible allocator that routes every allocation through a duckdb::Allocator (e.g. Allocator::Get(db)) so the
//! memory is accounted for by DuckDB.
template <class T>
class StlAllocator {
public:
    using value_type = T;

    // A duckdb::Allocator is a handle, so carry it across container copy/move/swap.
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    template <class U>
    struct rebind {
        using other = StlAllocator<U>;
    };

    // Implicit by design, so a container/string can be constructed directly from a duckdb::Allocator&.
    // The duckdb::Allocator must outlive every object using the StlAllocator wrapper.
    StlAllocator(duckdb::Allocator& allocator) noexcept // NOLINT(google-explicit-constructor): implicit is intended
        : allocator_(allocator) {
    }

    template <class U>
    StlAllocator(const StlAllocator<U>& other) noexcept // NOLINT(google-explicit-constructor): rebind conversion
        : allocator_(other.GetAllocator()) {
    }

    [[nodiscard]] T* allocate(const std::size_t n) {
        static_assert(alignof(T) <= alignof(std::max_align_t), "StlAllocator does not support over-aligned types.");
        if (n > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length();
        }
        return reinterpret_cast<T*>(allocator_.get().AllocateData(n * sizeof(T)));
    }

    void deallocate(T* p, const std::size_t n) noexcept {
        allocator_.get().FreeData(reinterpret_cast<duckdb::data_ptr_t>(p), n * sizeof(T));
    }

    [[nodiscard]] duckdb::Allocator& GetAllocator() const noexcept {
        return allocator_.get();
    }

    bool operator==(const StlAllocator& other) const noexcept {
        return &allocator_.get() == &other.allocator_.get();
    }
    bool operator!=(const StlAllocator& other) const noexcept {
        return !(*this == other);
    }

private:
    // Uses reference_wrapper instead a plain reference to make the class copy-assignable and swappable
    std::reference_wrapper<duckdb::Allocator> allocator_;
};

} // namespace logsearch::utils
