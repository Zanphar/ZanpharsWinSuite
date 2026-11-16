// comptr.h — minimal COM pointer RAII. No external dependencies.
#pragma once
#include <unknwn.h>

template <typename T>
class com_ptr {
public:
    com_ptr() = default;
    explicit com_ptr(T* p) : ptr_(p) {}
    ~com_ptr() { if (ptr_) ptr_->Release(); }

    com_ptr(const com_ptr&) = delete;
    com_ptr& operator=(const com_ptr&) = delete;

    com_ptr(com_ptr&& o) noexcept : ptr_(o.ptr_) { o.ptr_ = nullptr; }
    com_ptr& operator=(com_ptr&& o) noexcept {
        if (this != &o) { release(); ptr_ = o.ptr_; o.ptr_ = nullptr; }
        return *this;
    }

    T*   get() const        { return ptr_; }
    T**  operator&()        { release(); return &ptr_; }   // takes ownership on out-param
    T*   operator->() const { return ptr_; }
    T&   operator*() const  { return *ptr_; }
    T*   detach()           { T* p = ptr_; ptr_ = nullptr; return p; }
    operator T*() const     { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void release() { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }
    void reset()   { release(); }

private:
    T* ptr_ = nullptr;
};
