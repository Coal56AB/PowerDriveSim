#pragma once
#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace pds {
// Small contiguous storage for per-step values. Typical graph recordings fit
// inline and therefore do not allocate memory separately for every time step.
template <class T, std::size_t InlineCapacity> class InlineVector {
    static_assert(InlineCapacity > 0);
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
    union Storage {
        T inline_values[InlineCapacity];
        T *heap;
        Storage() {}
        ~Storage() {}
    } storage_;
    std::uint32_t size_ = 0;
    std::uint32_t capacity_ = InlineCapacity;

    bool uses_heap() const noexcept { return capacity_ > InlineCapacity; }
    T *storage_data() noexcept { return uses_heap() ? storage_.heap : storage_.inline_values; }
    const T *storage_data() const noexcept { return uses_heap() ? storage_.heap : storage_.inline_values; }
    static std::uint32_t checked_size(std::size_t size) {
        if (size > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("InlineVector capacity");
        return static_cast<std::uint32_t>(size);
    }

  public:
    using value_type = T;
    using iterator = T *;
    using const_iterator = const T *;

    InlineVector() = default;
    InlineVector(std::initializer_list<T> values) {
        reserve(values.size());
        for (const auto value : values)
            push_back(value);
    }
    InlineVector(const InlineVector &other) {
        reserve(other.size());
        std::copy(other.begin(), other.end(), storage_data());
        size_ = other.size_;
    }
    InlineVector(InlineVector &&other) noexcept {
        if (other.uses_heap()) {
            storage_.heap = std::exchange(other.storage_.heap, nullptr);
            size_ = std::exchange(other.size_, 0);
            capacity_ = std::exchange(other.capacity_, static_cast<std::uint32_t>(InlineCapacity));
        } else {
            std::copy(other.begin(), other.end(), storage_.inline_values);
            size_ = std::exchange(other.size_, 0);
        }
    }
    ~InlineVector() {
        if (uses_heap())
            delete[] storage_.heap;
    }
    InlineVector &operator=(const InlineVector &other) {
        if (this != &other) {
            clear();
            reserve(other.size());
            std::copy(other.begin(), other.end(), storage_data());
            size_ = other.size_;
        }
        return *this;
    }
    InlineVector &operator=(InlineVector &&other) noexcept {
        if (this != &other) {
            if (uses_heap())
                delete[] storage_.heap;
            size_ = 0;
            capacity_ = InlineCapacity;
            if (other.uses_heap()) {
                storage_.heap = std::exchange(other.storage_.heap, nullptr);
                size_ = std::exchange(other.size_, 0);
                capacity_ = std::exchange(other.capacity_, static_cast<std::uint32_t>(InlineCapacity));
            } else {
                std::copy(other.begin(), other.end(), storage_.inline_values);
                size_ = std::exchange(other.size_, 0);
            }
        }
        return *this;
    }
    InlineVector &operator=(std::initializer_list<T> values) {
        clear();
        reserve(values.size());
        for (const auto value : values)
            push_back(value);
        return *this;
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::size_t capacity() const noexcept { return capacity_; }
    T *data() noexcept { return storage_data(); }
    const T *data() const noexcept { return storage_data(); }
    iterator begin() noexcept { return data(); }
    iterator end() noexcept { return data() + size_; }
    const_iterator begin() const noexcept { return data(); }
    const_iterator end() const noexcept { return data() + size_; }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }
    T &operator[](std::size_t index) noexcept { return data()[index]; }
    const T &operator[](std::size_t index) const noexcept { return data()[index]; }
    T &at(std::size_t index) {
        if (index >= size_)
            throw std::out_of_range("InlineVector index");
        return data()[index];
    }
    const T &at(std::size_t index) const {
        if (index >= size_)
            throw std::out_of_range("InlineVector index");
        return data()[index];
    }
    T &front() noexcept { return data()[0]; }
    const T &front() const noexcept { return data()[0]; }
    T &back() noexcept { return data()[size_ - 1]; }
    const T &back() const noexcept { return data()[size_ - 1]; }

    void reserve(std::size_t requested) {
        if (requested <= capacity_)
            return;
        const auto capacity = checked_size(requested);
        auto *replacement = new T[capacity];
        std::copy(begin(), end(), replacement);
        if (uses_heap())
            delete[] storage_.heap;
        storage_.heap = replacement;
        capacity_ = capacity;
    }
    void push_back(T value) {
        if (size_ == capacity_)
            reserve(std::max<std::size_t>(size_ + 1, std::size_t(capacity_) * 2));
        data()[size_++] = value;
    }
    void clear() noexcept { size_ = 0; }

    template <class Range> bool operator==(const Range &other) const {
        return size() == other.size() && std::equal(this->begin(), this->end(), other.begin());
    }
};
} // namespace pds
