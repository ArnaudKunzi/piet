// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 M.K. Kim
// cecon — copy-on-write column buffer.
//
// A CowVec<T> is EITHER "borrowed" (a const span into another allocation — in
// practice the source Dataset's read-only mmap; the clean state) OR "owned" (its
// own std::vector<T>; the dirty state). Reads are transparent and byte-for-byte
// identical to reading a std::vector. The FIRST mutable access materializes the
// borrow into the owned vector (copy-on-write), so a column that is only read
// never pays the copy — this is what lets Frame::from_dataset stop deep-copying
// every column up front (io mining #2).
//
// Lifetime contract: a borrowed span must outlive the CowVec. Frame borrows from
// the Dataset it was built from and the caller keeps that Dataset alive until the
// Frame is written/destroyed (true at every call site; see Frame::from_dataset).
//
// Copy vs move: COPY materializes (deep-copies into the destination's own vector)
// so a borrow can NEVER propagate to a derived/other object — only the Frame that
// borrowed from the mmap ever holds a borrow. MOVE preserves the borrow (the
// source column simply relocates, e.g. when the column vector reallocates).
//
// The public surface is the exact subset of std::vector<T> that the data layer
// uses, so CowVec is a drop-in for the former `std::vector<double>`/`<int32_t>`
// FrameColumn members with no change at the ~150 read/write sites.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace cecon {

template <class T>
class CowVec {
public:
    CowVec() = default;
    CowVec(const CowVec& o) { assign_copy(o); }
    CowVec(CowVec&& o) noexcept { move_from(o); }
    CowVec& operator=(const CowVec& o) {
        if (this != &o) assign_copy(o);
        return *this;
    }
    CowVec& operator=(CowVec&& o) noexcept {
        if (this != &o) move_from(o);
        return *this;
    }
    // Whole-buffer replace from a std::vector — becomes owned (drops any borrow).
    CowVec& operator=(std::vector<T>&& v) {
        own_ = std::move(v);
        bptr_ = nullptr;
        bn_ = 0;
        return *this;
    }
    CowVec& operator=(const std::vector<T>& v) {
        own_ = v;
        bptr_ = nullptr;
        bn_ = 0;
        return *this;
    }

    // Borrow a read-only span (the clean state). `p` must outlive this CowVec.
    void borrow(const T* p, int64_t n) {
        own_.clear();
        bptr_ = p;
        bn_ = n;
    }
    bool borrowed() const { return bptr_ != nullptr; }
    // Bytes this CowVec's OWNED buffer actually holds from the allocator. Not the
    // same as size() once a caller compacts in place (keep_if does): the process is
    // charged for capacity, so a census keyed on size() would under-report exactly
    // the buffers that in-place compaction keeps alive.
    std::size_t capacity() const { return bptr_ ? 0 : own_.capacity(); }

    // If borrowed, copy the span into the owned vector and drop the borrow. Idempotent.
    void materialize() {
        if (bptr_) {
            own_.assign(bptr_, bptr_ + bn_);
            bptr_ = nullptr;
            bn_ = 0;
        }
    }

    // ---- reads (const) — transparent over borrowed/owned ----
    std::size_t size() const {
        return bptr_ ? static_cast<std::size_t>(bn_) : own_.size();
    }
    bool empty() const { return size() == 0; }
    const T* data() const { return bptr_ ? bptr_ : own_.data(); }
    const T& operator[](std::size_t i) const { return bptr_ ? bptr_[i] : own_[i]; }
    const T* begin() const { return data(); }
    const T* end() const { return data() + size(); }
    // Implicit view AS a std::vector, so CowVec is a drop-in wherever a
    // `const std::vector<T>&` is expected. Materializes a borrow on demand (a lazy,
    // value-identical copy) so the returned reference is always backed by own_.
    operator const std::vector<T>&() const {
        if (bptr_) const_cast<CowVec*>(this)->materialize();
        return own_;
    }

    // ---- mutable access — copy-on-write (materialize first) ----
    T* data() {
        materialize();
        return own_.data();
    }
    T& operator[](std::size_t i) {
        materialize();
        return own_[i];
    }
    T* begin() {
        materialize();
        return own_.data();
    }
    T* end() {
        materialize();
        return own_.data() + own_.size();
    }

    void resize(std::size_t n) {
        materialize();
        own_.resize(n);
    }
    void resize(std::size_t n, T val) {
        materialize();
        own_.resize(n, val);
    }
    void assign(const T* first, const T* last) {
        bptr_ = nullptr;
        bn_ = 0;
        own_.assign(first, last);
    }
    void assign(std::size_t n, T val) {
        bptr_ = nullptr;
        bn_ = 0;
        own_.assign(n, val);
    }
    void push_back(T v) {
        materialize();
        own_.push_back(v);
    }
    void reserve(std::size_t n) {
        materialize();
        own_.reserve(n);
    }
    void clear() {
        bptr_ = nullptr;
        bn_ = 0;
        own_.clear();
    }
    // Iterator-range insert (the only insert form the data layer uses; always an
    // append at end()). `pos` is a pointer obtained from this->end()/data().
    template <class It>
    void insert(const T* pos, It first, It last) {
        std::ptrdiff_t off = pos - data();  // data() materializes; pos is relative to own_
        if (off < 0) off = static_cast<std::ptrdiff_t>(own_.size());
        own_.insert(own_.begin() + off, first, last);
    }

private:
    void assign_copy(const CowVec& o) {  // copy = deep materialize (never inherit a borrow)
        own_.assign(o.data(), o.data() + o.size());
        bptr_ = nullptr;
        bn_ = 0;
    }
    void move_from(CowVec& o) {  // move = keep the borrow if any
        own_ = std::move(o.own_);
        bptr_ = o.bptr_;
        bn_ = o.bn_;
        o.bptr_ = nullptr;
        o.bn_ = 0;
    }

    std::vector<T> own_;
    const T* bptr_ = nullptr;
    int64_t bn_ = 0;
};

}  // namespace cecon
