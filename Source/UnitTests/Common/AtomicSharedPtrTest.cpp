// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <thread>

#include <gtest/gtest.h>

#include "Common/AtomicSharedPtr.h"

TEST(AtomicSharedPtr, FallbackRetainsLoadedSnapshot)
{
  Common::AtomicSharedPtr<const int, false> value;
  EXPECT_EQ(value.load(), nullptr);
  value.store(std::make_shared<const int>(1));
  const auto old = value.load();
  value.store(std::make_shared<const int>(2));
  EXPECT_EQ(*old, 1);
  EXPECT_EQ(*value.load(), 2);
}

TEST(AtomicSharedPtr, FallbackDestroysOldValueOutsideMutex)
{
  Common::AtomicSharedPtr<const int, false> value;
  bool destroyed = false;
  value.store(std::shared_ptr<const int>(new int(1), [&](const int* old) {
    EXPECT_EQ(value.load(), nullptr);
    destroyed = true;
    delete old;
  }));
  value.store(nullptr);
  EXPECT_TRUE(destroyed);
}

TEST(AtomicSharedPtr, FallbackSupportsConcurrentPublication)
{
  Common::AtomicSharedPtr<const int, false> value;
  value.store(std::make_shared<const int>(0));
  std::thread publisher([&] {
    for (int i = 1; i <= 1000; ++i)
      value.store(std::make_shared<const int>(i));
  });
  for (int i = 0; i < 1000; ++i)
  {
    const auto snapshot = value.load();
    EXPECT_NE(snapshot, nullptr);
    if (!snapshot)
      continue;
    EXPECT_GE(*snapshot, 0);
    EXPECT_LE(*snapshot, 1000);
  }
  publisher.join();
  EXPECT_EQ(*value.load(), 1000);
}
