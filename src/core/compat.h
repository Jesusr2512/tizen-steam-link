#pragma once

#include <cassert>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>

#if defined(__has_include)
#if __has_include(<optional>) && __cplusplus >= 201703L
#include <optional>
#define TSL_HAS_STD_OPTIONAL 1
#endif
#if __has_include(<string_view>) && __cplusplus >= 201703L
#include <string_view>
#define TSL_HAS_STD_STRING_VIEW 1
#endif
#endif

#if !defined(TSL_HAS_STD_OPTIONAL)
namespace std {

struct nullopt_t {
    explicit constexpr nullopt_t(int) {}
};
static constexpr nullopt_t nullopt{0};

template <typename T>
class optional {
public:
    optional() noexcept : has_(false) {}
    optional(nullopt_t) noexcept : has_(false) {}

    optional(const T &v) : has_(true) {
        new (&storage_) T(v);
    }

    optional(T &&v) : has_(true) {
        new (&storage_) T(std::move(v));
    }

    optional(const optional &other) : has_(other.has_) {
        if (has_) {
            new (&storage_) T(*other);
        }
    }

    optional(optional &&other) noexcept : has_(other.has_) {
        if (has_) {
            new (&storage_) T(std::move(*other));
        }
    }

    ~optional() {
        reset();
    }

    optional &operator=(nullopt_t) noexcept {
        reset();
        return *this;
    }

    optional &operator=(const T &v) {
        if (has_) {
            **this = v;
        } else {
            new (&storage_) T(v);
            has_ = true;
        }
        return *this;
    }

    optional &operator=(T &&v) {
        if (has_) {
            **this = std::move(v);
        } else {
            new (&storage_) T(std::move(v));
            has_ = true;
        }
        return *this;
    }

    optional &operator=(const optional &other) {
        if (this != &other) {
            if (other.has_) {
                *this = *other;
            } else {
                reset();
            }
        }
        return *this;
    }

    optional &operator=(optional &&other) noexcept {
        if (this != &other) {
            if (other.has_) {
                *this = std::move(*other);
            } else {
                reset();
            }
        }
        return *this;
    }

    void reset() noexcept {
        if (has_) {
            ptr()->~T();
            has_ = false;
        }
    }

    bool has_value() const noexcept { return has_; }
    explicit operator bool() const noexcept { return has_; }

    T &value() {
        assert(has_);
        return *ptr();
    }
    const T &value() const {
        assert(has_);
        return *ptr();
    }

    T &operator*() { return *ptr(); }
    const T &operator*() const { return *ptr(); }
    T *operator->() { return ptr(); }
    const T *operator->() const { return ptr(); }

private:
    T *ptr() { return reinterpret_cast<T *>(&storage_); }
    const T *ptr() const { return reinterpret_cast<const T *>(&storage_); }

    typename aligned_storage<sizeof(T), alignof(T)>::type storage_;
    bool has_;
};

} // namespace std
#endif

#if !defined(TSL_HAS_STD_STRING_VIEW)
namespace std {

class string_view {
public:
    using size_type = size_t;
    enum : size_type { npos = static_cast<size_type>(-1) };

    static constexpr size_type cstrlen(const char *s) noexcept {
        size_type n = 0;
        if (s) {
            while (s[n] != '\0') ++n;
        }
        return n;
    }

    constexpr string_view() noexcept : data_(""), size_(0) {}
    constexpr string_view(const char *s, size_type count) noexcept : data_(s), size_(count) {}
    constexpr string_view(const char *s) noexcept : data_(s ? s : ""), size_(cstrlen(s)) {}
    string_view(const string &s) noexcept : data_(s.data()), size_(s.size()) {}

    constexpr const char *data() const noexcept { return data_; }
    constexpr size_type size() const noexcept { return size_; }
    constexpr size_type length() const noexcept { return size_; }
    constexpr bool empty() const noexcept { return size_ == 0; }

    constexpr char operator[](size_type pos) const noexcept { return data_[pos]; }
    constexpr char front() const noexcept { return data_[0]; }
    constexpr char back() const noexcept { return data_[size_ - 1]; }

    constexpr const char *begin() const noexcept { return data_; }
    constexpr const char *end() const noexcept { return data_ + size_; }

    void remove_prefix(size_type n) noexcept {
        data_ += n;
        size_ -= n;
    }

    void remove_suffix(size_type n) noexcept {
        size_ -= n;
    }

    string_view substr(size_type pos = 0, size_type count = npos) const noexcept {
        if (pos > size_) pos = size_;
        size_type rcount = size_ - pos;
        if (count < rcount) rcount = count;
        return string_view(data_ + pos, rcount);
    }

    size_type find(char c, size_type pos = 0) const noexcept {
        for (size_type i = pos; i < size_; ++i) {
            if (data_[i] == c) return i;
        }
        return npos;
    }

    int compare(string_view other) const noexcept {
        size_type rlen = size_ < other.size_ ? size_ : other.size_;
        int ret = rlen == 0 ? 0 : memcmp(data_, other.data_, rlen);
        if (ret == 0) {
            if (size_ < other.size_) return -1;
            if (size_ > other.size_) return 1;
        }
        return ret;
    }

    explicit operator string() const {
        return string(data_, size_);
    }

private:
    const char *data_;
    size_type size_;
};

inline bool operator==(string_view a, string_view b) noexcept {
    return a.size() == b.size() && (a.size() == 0 || memcmp(a.data(), b.data(), a.size()) == 0);
}
inline bool operator!=(string_view a, string_view b) noexcept {
    return !(a == b);
}
inline bool operator<(string_view a, string_view b) noexcept {
    return a.compare(b) < 0;
}

} // namespace std
#endif
