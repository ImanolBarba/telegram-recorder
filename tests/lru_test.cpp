//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <gtest/gtest.h>

#include "lru.hpp"

TEST(LRUTest, PutAndGet) {
  LRU<std::string, int> cache(10);
  cache.put(std::move(std::string("1")), 1);
  EXPECT_EQ(1, cache.numItems());
  EXPECT_EQ(1, *cache.get(std::move(std::string("1"))));
  EXPECT_EQ(NULL, cache.get(std::move(std::string("2"))));
  cache.put(std::move(std::string("2")), 2);
  EXPECT_EQ(2, cache.numItems());
  EXPECT_EQ(2, *cache.get(std::move(std::string("2"))));

  cache.put(std::move(std::string("1")), 3);
  EXPECT_EQ(2, cache.numItems());
  EXPECT_EQ(3, *cache.get(std::move(std::string("1"))));
}

TEST(LRUTest, Evict) {
  LRU<std::string, int> cache(5);
  cache.put(std::move(std::string("1")), 1);
  cache.put(std::move(std::string("2")), 2);
  cache.evict(std::move(std::string("1")));
  EXPECT_EQ(1, cache.numItems());
  EXPECT_EQ(NULL, cache.get(std::move(std::string("1"))));
  EXPECT_EQ(2, *cache.get(std::move(std::string("2"))));
  
  cache.evict(std::move(std::string("3")));
  EXPECT_EQ(1, cache.numItems());
  EXPECT_EQ(2, *cache.get(std::move(std::string("2"))));

  cache.put(std::move(std::string("1")), 1);
  cache.put(std::move(std::string("3")), 3);
  cache.put(std::move(std::string("4")), 4);
  cache.put(std::move(std::string("5")), 5);
  cache.put(std::move(std::string("6")), 6);
  EXPECT_EQ(1, *cache.get(std::move(std::string("1"))));
  EXPECT_EQ(NULL, cache.get(std::move(std::string("2"))));
  EXPECT_EQ(3, *cache.get(std::move(std::string("3"))));
  EXPECT_EQ(4, *cache.get(std::move(std::string("4"))));
  EXPECT_EQ(5, *cache.get(std::move(std::string("5"))));
  EXPECT_EQ(6, *cache.get(std::move(std::string("6"))));
  EXPECT_EQ(5, cache.numItems());

  // should not evict because it's already there
  cache.put(std::move(std::string("6")), 6);
  EXPECT_EQ(1, *cache.get(std::move(std::string("1"))));

  // should evict last key
  cache.put(std::move(std::string("7")), 7);
  EXPECT_EQ(NULL, cache.get(std::move(std::string("1"))));

  // and the following
  cache.put(std::move(std::string("8")), 8);
  EXPECT_EQ(NULL, cache.get(std::move(std::string("3"))));
}