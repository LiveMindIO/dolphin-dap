// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <utility>

namespace Common
{
// Keep the C++20 specialization where available. Apple's libc++ does not yet
// provide it; a separate mutex gives readers an owned, atomically acquired value
// without relying on the deprecated shared_ptr atomic free functions.
#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
inline constexpr bool HAS_ATOMIC_SHARED_PTR = true;
#else
inline constexpr bool HAS_ATOMIC_SHARED_PTR = false;
#endif

template <typename T, bool = HAS_ATOMIC_SHARED_PTR>
class AtomicSharedPtr;

template <typename T>
class AtomicSharedPtr<T, false>
{
public:
  std::shared_ptr<T> load() const
  {
    std::lock_guard lock(m_mutex);
    return m_value;
  }

  void store(std::shared_ptr<T> value)
  {
    // Release the previous value outside the mutex, just like atomic<shared_ptr>.
    std::lock_guard lock(m_mutex);
    m_value.swap(value);
  }

private:
  mutable std::mutex m_mutex;
  std::shared_ptr<T> m_value;
};

#if defined(__cpp_lib_atomic_shared_ptr) && __cpp_lib_atomic_shared_ptr >= 201711L
template <typename T>
class AtomicSharedPtr<T, true>
{
public:
  std::shared_ptr<T> load() const { return m_value.load(); }
  void store(std::shared_ptr<T> value) { m_value.store(std::move(value)); }

private:
  std::atomic<std::shared_ptr<T>> m_value;
};
#endif
}  // namespace Common
