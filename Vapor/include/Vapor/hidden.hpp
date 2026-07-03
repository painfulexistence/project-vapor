#pragma once
#include <type_traits>
#include <utility>

namespace Vapor {

// Wrap a field with Hidden<T> to exclude it from the inspector.
// Implicit conversions keep all existing usage sites unchanged.
template<typename T>
struct Hidden {
    T value{};
    operator T&()             { return value; }
    operator const T&() const { return value; }
    Hidden& operator=(const T& v) { value = v; return *this; }
    Hidden& operator=(T&& v)      { value = std::move(v); return *this; }
    T* operator&()             { return &value; }
    const T* operator&() const { return &value; }
};

template<typename T> struct is_hidden            : std::false_type {};
template<typename T> struct is_hidden<Hidden<T>> : std::true_type  {};
template<typename T> inline constexpr bool is_hidden_v = is_hidden<T>::value;

} // namespace Vapor
