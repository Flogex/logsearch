#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace logsearch::analyzer {

//! Non-owning mutable byte span, pointing to a byte range in the document that is analyzed.
//! Pipeline stages can modify the bytes in place, but cannot grow or shrink the span.
// std::span<char> is only available in C++ 20.
class MutableSpan {
    char* data_;
    std::size_t size_;

public:
    constexpr MutableSpan(char* data, const std::size_t size) noexcept : data_(data), size_(size) {
    }

    //! Convenience: alias a non-const `std::string`'s buffer. Caller owns the
    //! string. The MutableSpan lives only as long as `s` is alive and not reallocated.
    explicit MutableSpan(std::string& s) noexcept : MutableSpan(s.data(), s.size()) {
    }

    [[nodiscard]] constexpr char* data() noexcept {
        return data_;
    }
    [[nodiscard]] constexpr const char* data() const noexcept {
        return data_;
    }
    [[nodiscard]] constexpr std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] constexpr bool empty() const noexcept {
        return size_ == 0;
    }

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return {data_, size_};
    }
};

} // namespace logsearch::analyzer
