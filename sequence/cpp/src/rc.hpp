#pragma once
#include <utility>

// Non-atomic single-threaded reference-counted pointer.
// Drop-in for std::shared_ptr<T> in single-threaded code.
// Uses a single heap allocation: object and refcount are co-located.
template<typename T>
class LocalRc {
    struct Block { T val; int ref; };
    Block* p_;

public:
    LocalRc() noexcept : p_(nullptr) {}

    // Factory: single heap allocation, forwards args to T's constructor
    template<typename... Args>
    static LocalRc make(Args&&... args) {
        LocalRc r;
        r.p_ = new Block{T(std::forward<Args>(args)...), 1};
        return r;
    }

    LocalRc(const LocalRc& o) noexcept : p_(o.p_) { if (p_) ++p_->ref; }
    LocalRc(LocalRc&& o)      noexcept : p_(o.p_) { o.p_ = nullptr; }

    LocalRc& operator=(LocalRc o) noexcept { std::swap(p_, o.p_); return *this; }

    ~LocalRc() { if (p_ && --p_->ref == 0) delete p_; }

    T*   get()        const noexcept { return p_ ? &p_->val : nullptr; }
    T&   operator*()  const noexcept { return p_->val; }
    T*   operator->() const noexcept { return &p_->val; }
    explicit operator bool() const noexcept { return p_ != nullptr; }

    bool operator==(const LocalRc& o) const noexcept { return p_ == o.p_; }
    bool operator!=(const LocalRc& o) const noexcept { return p_ != o.p_; }
};
