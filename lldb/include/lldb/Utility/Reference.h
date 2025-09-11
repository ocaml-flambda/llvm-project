//===-- Reference.h ---------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLDB_UTILITY_REFERENCE_H
#define LLDB_UTILITY_REFERENCE_H

#include <cassert>
#include <memory>

namespace lldb_private {

/// A reference type that can be updated.
/// 
/// This class provides a stable reference to an object that can be updated
/// atomically. All references are always initialized with a value and
/// should not be null after construction.
///
/// INVARIANTS:
/// - All references are ALWAYS initialized with a value (never null)
/// - The contained value can be replaced via set() method
template <typename T>
class Reference {
private:
  std::unique_ptr<T> m_value;
  
public:
  /// Create a reference with an initial value
  explicit Reference(std::unique_ptr<T> initial_value) 
    : m_value(std::move(initial_value)) {}
  
  /// Update the value
  void set(std::unique_ptr<T> value) {
    m_value = std::move(value);
  }
  
  /// Get the value (always valid)
  T* get() const {
    T* ptr = m_value.get();
    assert(ptr && "Reference must always contain a value");
    return ptr;
  }
};

} // namespace lldb_private

#endif // LLDB_UTILITY_REFERENCE_H
