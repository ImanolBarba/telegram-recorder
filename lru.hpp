//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#ifndef LRU_HPP
#define LRU_HPP

#include <map>
#include <vector>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

template<class K, class V>
class LRU {
  public:
    LRU(unsigned int size) : size(size) {};
    V* get(K key);
    unsigned int numItems();
    void put(K key, V value);
    void evict(K key);

  private:
    std::map<K, V> data;
    std::vector<K> itemList;
    unsigned int size;
};

template<class K, class V>
V* LRU<K, V>::get(K key) {
  if(this->data.find(key) == this->data.end()) {
    return NULL;
  }
  return &this->data[key];
}

template<class K, class V>
unsigned int LRU<K, V>::numItems() {
  return this->itemList.size();
}

template<class K, class V>
void LRU<K, V>::put(K key, V value) {
  if(this->data.find(key) != this->data.end()) {
    for(unsigned int i = 0; i < this->numItems(); i++) {
      if(this->itemList[i] == key) {
        this->itemList.erase(this->itemList.begin() + i);
        this->itemList.insert(this->itemList.begin(), key);
        this->data[key] = std::move(value);
        return;
      }
    }
    SPDLOG_ERROR("Inconsistency between cache map and list");
  } else {
    this->itemList.insert(this->itemList.begin(), key);
    this->data[key] = std::move(value);
    for(unsigned int i = this->numItems(); i > this->size; i--) {
      this->data.erase(this->itemList[i-1]);
      this->itemList.pop_back();
    }
  }
}

template<class K, class V>
void LRU<K, V>::evict(K key) {
  if(this->data.find(key) != this->data.end()) {
    for(unsigned int i = 0; i < this->itemList.size(); ++i) {
      if(this->itemList[i] == key) {
        this->itemList.erase(this->itemList.begin() + i);
        this->data.erase(key);
        return;
      }
    }
  } else {
    SPDLOG_WARN("Asked to evict key {}, which is not cached", key);
  }
}

#endif