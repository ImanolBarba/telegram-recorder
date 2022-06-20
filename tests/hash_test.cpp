//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <gtest/gtest.h>

#include "hash.hpp"

TEST(HashTest, SHA256Test) {
  std::string res1 = SHA256("", 0);
  EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", res1);
  std::string res2 = SHA256(res1.c_str(), res1.size());
  EXPECT_EQ("cd372fb85148700fa88095e3492d3f9f5beb43e555e5ff26d95f5a6adc36f8e6", res2);
}