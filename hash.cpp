  //
  // Copyright (c) 2022, Imanol-Mikel Barba Sabariego
  // All rights reserved.
  // 
  // Distributed under BSD 3-Clause License. See LICENSE.

#include <iomanip>
#include <sstream>

#include <stdint.h>

#include <openssl/sha.h>

std::string SHA256(const char* data, size_t dataLen) {
  unsigned char hash[SHA256_DIGEST_LENGTH] = { 0 };

  SHA256_CTX ctx;
  SHA256_Init(&ctx);
  SHA256_Update(&ctx, data, dataLen);
  SHA256_Final(hash, &ctx);
  
  std::stringstream sstream;
  sstream << std::hex << std::setfill('0');

  for (unsigned int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
    sstream << std::setw(2) << static_cast<unsigned int>(hash[i]);
  }

  return sstream.str();
}
