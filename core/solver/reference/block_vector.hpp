#pragma once
#include <algorithm>
#include <compare>
#include <cstddef>
#include <initializer_list>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace pds {
// Append-oriented random-access storage. Existing elements never move when the
// history grows, while reasonably large blocks retain cache locality for plots,
// export and analysis.
template <class T, std::size_t BlockSize = 1024> class BlockVector {
    static_assert(BlockSize > 0);
    std::vector<std::vector<T>> blocks_;
    std::size_t size_ = 0;

    void append_block() {
        blocks_.emplace_back();
        blocks_.back().reserve(BlockSize);
    }
    void shrink_to(std::size_t count) {
        while (size_ > count) {
            blocks_.back().pop_back();
            --size_;
            if (blocks_.back().empty())
                blocks_.pop_back();
        }
    }

  public:
    template <bool Const> class Iterator {
        using Owner = std::conditional_t<Const, const BlockVector, BlockVector>;
        Owner *owner_ = nullptr;
        std::size_t index_ = 0;
        friend class BlockVector;
        template <bool> friend class Iterator;
        Iterator(Owner *owner, std::size_t index) : owner_(owner), index_(index) {}

      public:
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = std::conditional_t<Const, const T *, T *>;
        using reference = std::conditional_t<Const, const T &, T &>;
        Iterator() = default;
        template <bool C = Const, std::enable_if_t<C, int> = 0>
        Iterator(const Iterator<false> &other) : owner_(other.owner_), index_(other.index_) {}
        reference operator*() const { return (*owner_)[index_]; }
        pointer operator->() const { return &**this; }
        reference operator[](difference_type offset) const { return (*owner_)[index_ + offset]; }
        Iterator &operator++() { ++index_; return *this; }
        Iterator operator++(int) { auto copy = *this; ++*this; return copy; }
        Iterator &operator--() { --index_; return *this; }
        Iterator operator--(int) { auto copy = *this; --*this; return copy; }
        Iterator &operator+=(difference_type offset) { index_ += offset; return *this; }
        Iterator &operator-=(difference_type offset) { index_ -= offset; return *this; }
        friend Iterator operator+(Iterator it, difference_type offset) { return it += offset; }
        friend Iterator operator+(difference_type offset, Iterator it) { return it += offset; }
        friend Iterator operator-(Iterator it, difference_type offset) { return it -= offset; }
        friend difference_type operator-(const Iterator &a, const Iterator &b) {
            return static_cast<difference_type>(a.index_) - static_cast<difference_type>(b.index_);
        }
        friend bool operator==(const Iterator &, const Iterator &) = default;
        friend auto operator<=>(const Iterator &a, const Iterator &b) { return a.index_ <=> b.index_; }
    };

    using iterator = Iterator<false>;
    using const_iterator = Iterator<true>;

    BlockVector() = default;
    BlockVector(std::initializer_list<T> values) { insert(end(), values.begin(), values.end()); }
    BlockVector(const BlockVector &) = default;
    BlockVector &operator=(const BlockVector &) = default;
    BlockVector(BlockVector &&other) noexcept
        : blocks_(std::move(other.blocks_)), size_(std::exchange(other.size_, 0)) {}
    BlockVector &operator=(BlockVector &&other) noexcept {
        if (this != &other) {
            blocks_ = std::move(other.blocks_);
            size_ = std::exchange(other.size_, 0);
        }
        return *this;
    }
    BlockVector &operator=(std::initializer_list<T> values) {
        clear();
        insert(end(), values.begin(), values.end());
        return *this;
    }

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }
    std::size_t capacity() const noexcept { return blocks_.size() * BlockSize; }
    T &operator[](std::size_t index) { return blocks_[index / BlockSize][index % BlockSize]; }
    const T &operator[](std::size_t index) const { return blocks_[index / BlockSize][index % BlockSize]; }
    T &at(std::size_t index) {
        if (index >= size_)
            throw std::out_of_range("BlockVector index");
        return (*this)[index];
    }
    const T &at(std::size_t index) const {
        if (index >= size_)
            throw std::out_of_range("BlockVector index");
        return (*this)[index];
    }
    T &front() { return (*this)[0]; }
    const T &front() const { return (*this)[0]; }
    T &back() { return (*this)[size_ - 1]; }
    const T &back() const { return (*this)[size_ - 1]; }
    iterator begin() noexcept { return {this, 0}; }
    iterator end() noexcept { return {this, size_}; }
    const_iterator begin() const noexcept { return {this, 0}; }
    const_iterator end() const noexcept { return {this, size_}; }
    const_iterator cbegin() const noexcept { return begin(); }
    const_iterator cend() const noexcept { return end(); }

    void reserve(std::size_t count) { blocks_.reserve(count / BlockSize + (count % BlockSize != 0)); }
    void push_back(const T &value) {
        const bool added = size_ % BlockSize == 0;
        if (added)
            append_block();
        try {
            blocks_.back().push_back(value);
        } catch (...) {
            if (added)
                blocks_.pop_back();
            throw;
        }
        ++size_;
    }
    void push_back(T &&value) {
        const bool added = size_ % BlockSize == 0;
        if (added)
            append_block();
        try {
            blocks_.back().push_back(std::move(value));
        } catch (...) {
            if (added)
                blocks_.pop_back();
            throw;
        }
        ++size_;
    }
    template <class Input> iterator insert(const_iterator position, Input first, Input last) {
        const auto index = position - cbegin();
        if (position != cend()) {
            std::vector<T> tail;
            tail.reserve(static_cast<std::size_t>(cend() - position));
            for (auto it = position; it != cend(); ++it)
                tail.push_back(*it);
            shrink_to(static_cast<std::size_t>(index));
            for (; first != last; ++first)
                push_back(*first);
            for (auto &value : tail)
                push_back(std::move(value));
        } else {
            for (; first != last; ++first)
                push_back(*first);
        }
        return begin() + index;
    }
    iterator erase(const_iterator first, const_iterator last) {
        const auto begin_index = static_cast<std::size_t>(first - cbegin());
        const auto end_index = static_cast<std::size_t>(last - cbegin());
        for (std::size_t target = begin_index, source = end_index; source < size_; ++target, ++source)
            (*this)[target] = std::move((*this)[source]);
        shrink_to(size_ - (end_index - begin_index));
        return begin() + static_cast<std::ptrdiff_t>(begin_index);
    }
    void clear() noexcept { blocks_.clear(); size_ = 0; }
};
} // namespace pds
