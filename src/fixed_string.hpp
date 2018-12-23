#ifndef BLABBER_FIXED_STRING_HPP
#define BLABBER_FIXED_STRING_HPP

#include "common.hpp"

#include <prequel/binary_format.hpp>

#include <algorithm>
#include <cstring>
#include <string_view>

namespace blabber {

/*
 * A fixed string of maximum size N. Strings are either terminated by NULs
 * or take up the entire array. In other words, when the string size is less
 * than `N`, then the remaining characters in the internal array are all 0.
 *
 * TODO: Move this into the main prequel project. It's useful.
 */
template<u32 N>
class fixed_cstring {
public:
    static constexpr u32 max_size = N;

public:
    fixed_cstring() { std::memset(m_data, 0, max_size); }

    explicit fixed_cstring(std::string_view str) {
        if (str.size() > max_size)
            throw std::runtime_error("String is too long.");

        std::memcpy(m_data, str.data(), str.size());
        std::memset(m_data + str.size(), 0, max_size - str.size());
    }

    const char* begin() const { return m_data; }
    const char* end() const { return m_data + size(); }
    const char* data() const { return m_data; }

    u32 size() const {
        // Index of first 0 byte (or max_size).
        return std::find(std::begin(m_data), std::end(m_data), 0) - std::begin(m_data);
    }

    static constexpr auto get_binary_format() {
        return prequel::binary_format(&fixed_cstring::m_data);
    }

    friend bool operator<(const fixed_cstring& lhs, const fixed_cstring& rhs) {
        return std::lexicographical_compare(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    }

    friend bool operator==(const fixed_cstring& lhs, const fixed_cstring& rhs) {
        return std::equal(lhs.begin(), lhs.end(), rhs.begin(), rhs.end());
    }

    friend bool operator!=(const fixed_cstring& lhs, const fixed_cstring& rhs) {
        return !(lhs == rhs);
    }

private:
    // Unset bytes at the end are zero.
    // String is not null terminated (i.e. all max_size bytes can be used).
    char m_data[max_size];
};

} // namespace blabber

#endif // BLABBER_FIXED_STRING_HPP
