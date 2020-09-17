/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#ifndef GALOIS_LIBGALOIS_GALOIS_LARGEARRAY_H_
#define GALOIS_LIBGALOIS_GALOIS_LARGEARRAY_H_

#include <utility>

#include "galois/Galois.h"
#include "galois/ParallelSTL.h"
#include "galois/config.h"
#include "galois/substrate/NumaMem.h"

namespace galois {

/**
 * Large array of objects with proper specialization for void type and
 * supporting various allocation and construction policies.
 *
 * @tparam T value type of container
 */
template <typename T>
class LargeArray {
  enum class AllocType { Blocked, Local, Interleaved, Floating };

  substrate::LAptr real_data_;
  T* data_{};
  size_t size_{};

  void Allocate(size_t n, AllocType t) {
    assert(!data_);
    size_ = n;
    switch (t) {
    case AllocType::Blocked:
      real_data_ =
          substrate::largeMallocBlocked(n * sizeof(T), runtime::activeThreads);
      break;
    case AllocType::Interleaved:
      real_data_ = substrate::largeMallocInterleaved(
          n * sizeof(T), runtime::activeThreads);
      break;
    case AllocType::Local:
      real_data_ = substrate::largeMallocLocal(n * sizeof(T));
      break;
    case AllocType::Floating:
      real_data_ = substrate::largeMallocFloating(n * sizeof(T));
      break;
    default:
      assert(false);
    };

    data_ = reinterpret_cast<T*>(real_data_.get());
  }

public:
  typedef T raw_value_type;
  typedef T value_type;
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef value_type& reference;
  typedef const value_type& const_reference;
  typedef value_type* pointer;
  typedef const value_type* const_pointer;
  typedef pointer iterator;
  typedef const_pointer const_iterator;
  const static bool has_value = true;

  // Extra indirection to support incomplete T's
  struct size_of {
    const static size_t value = sizeof(T);
  };

  /**
   * Wraps existing buffer in LargeArray interface.
   */
  LargeArray(void* d, size_t s) : data_(reinterpret_cast<T*>(d)), size_(s) {}

  LargeArray() = default;

  LargeArray(LargeArray&& o) noexcept
      : real_data_(std::move(o.real_data_)), data_(o.data_), size_(o.size_) {}

  LargeArray& operator=(LargeArray&& o) {
    auto tmp = std::move(o);
    std::swap(real_data_, tmp.real_data_);
    std::swap(data_, tmp.data_);
    std::swap(size_, tmp.size_);
    return *this;
  }

  LargeArray(const LargeArray&) = delete;
  LargeArray& operator=(const LargeArray&) = delete;

  ~LargeArray() {
    destroy();
    deallocate();
  }

  const_reference at(difference_type x) const { return data_[x]; }
  reference at(difference_type x) { return data_[x]; }
  const_reference operator[](size_type x) const { return data_[x]; }
  reference operator[](size_type x) { return data_[x]; }
  void set(difference_type x, const_reference v) { data_[x] = v; }
  size_type size() const { return size_; }
  iterator begin() { return data_; }
  const_iterator begin() const { return data_; }
  iterator end() { return data_ + size_; }
  const_iterator end() const { return data_ + size_; }

  //! [allocatefunctions]
  //! Allocates interleaved across NUMA (memory) nodes.
  void allocateInterleaved(size_type n) { Allocate(n, AllocType::Interleaved); }

  /**
   * Allocates using blocked memory policy
   *
   * @param  n         number of elements to allocate
   */
  void allocateBlocked(size_type n) { Allocate(n, AllocType::Blocked); }

  /**
   * Allocates using Thread Local memory policy
   *
   * @param  n         number of elements to allocate
   */
  void allocateLocal(size_type n) { Allocate(n, AllocType::Local); }

  /**
   * Allocates using no memory policy (no pre alloc)
   *
   * @param  n         number of elements to allocate
   */
  void allocateFloating(size_type n) { Allocate(n, AllocType::Floating); }

  /**
   * Allocate memory to threads based on a provided array specifying which
   * threads receive which elements of data.
   *
   * @tparam RangeArray The type of the threadRanges array; should either
   * be uint32_t* or uint64_t*
   * @param num Number of elements to allocate space for
   * @param ranges An array specifying how elements should be split
   * among threads
   */
  template <typename RangeArray>
  void allocateSpecified(size_type num, RangeArray& ranges) {
    assert(!data_);

    real_data_ = substrate::largeMallocSpecified(
        num * sizeof(T), runtime::activeThreads, ranges, sizeof(T));

    size_ = num;
    data_ = reinterpret_cast<T*>(real_data_.get());
  }
  //! [allocatefunctions]

  template <typename... Args>
  void construct(Args&&... args) {
    for (T *ii = data_, *ei = data_ + size_; ii != ei; ++ii) {
      new (ii) T(std::forward<Args>(args)...);
    }
  }

  template <typename... Args>
  void constructAt(size_type n, Args&&... args) {
    new (&data_[n]) T(std::forward<Args>(args)...);
  }

  //! Allocate and construct
  template <typename... Args>
  void create(size_type n, Args&&... args) {
    allocateInterleaved(n);
    construct(std::forward<Args>(args)...);
  }

  void deallocate() {
    real_data_.reset();
    data_ = 0;
    size_ = 0;
  }

  void destroy() {
    if (!data_) {
      return;
    }
    galois::ParallelSTL::destroy(data_, data_ + size_);
  }

  // The following methods are not shared with void specialization
  const_pointer data() const { return data_; }
  pointer data() { return data_; }
};

//! Void specialization
template <>
class LargeArray<void> {
public:
  LargeArray(void*, size_t) {}
  LargeArray() = default;
  ~LargeArray() = default;

  LargeArray(const LargeArray&) = delete;
  LargeArray& operator=(const LargeArray&) = delete;
  LargeArray(LargeArray&&) = delete;
  LargeArray& operator=(LargeArray&&) = delete;

  typedef void raw_value_type;
  typedef void* value_type;
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef void* reference;
  typedef void* const_reference;
  typedef void** pointer;
  typedef void** const_pointer;
  typedef pointer iterator;
  typedef const_pointer const_iterator;
  const static bool has_value = false;

  struct size_of {
    const static size_t value = 0;
  };

  const_reference at(difference_type) const { return nullptr; }
  reference at(difference_type) { return nullptr; }
  const_reference operator[](size_type) const { return nullptr; }
  template <typename T>
  void set(difference_type, T) {}
  size_type size() const { return 0; }
  iterator begin() { return nullptr; }
  const_iterator begin() const { return nullptr; }
  iterator end() { return nullptr; }
  const_iterator end() const { return nullptr; }

  void allocateInterleaved(size_type) {}
  void allocateBlocked(size_type) {}
  void allocateLocal(size_type) {}
  void allocateFloating(size_type) {}
  template <typename RangeArray>
  void allocateSpecified(size_type, RangeArray) {}

  template <typename... Args>
  void construct(Args&&...) {}
  template <typename... Args>
  void constructAt(size_type, Args&&...) {}
  template <typename... Args>
  void create(size_type, Args&&...) {}

  void deallocate() {}
  void destroy() {}

  const_pointer data() const { return nullptr; }
  pointer data() { return nullptr; }
};

}  // namespace galois
#endif
