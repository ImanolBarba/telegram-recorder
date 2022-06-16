//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <chrono>
#include <random>
#include <thread>
#include <vector>

#include <stdlib.h>
#include <unistd.h>

#include "telegram_recorder.hpp"

// TODO: Add human activity parameters to config file

double getMessageReadTime(std::shared_ptr<td_api::message>& message) {
  // get message type
  // get message params according to type
  // if text: 250 wpm
  // if video, length
  // if photo, 5 sec
  return 1.0;
}

void TelegramRecorder::runMessageReader() {
  std::random_device rd;
  std::default_random_engine generator(rd());
  std::normal_distribution<> distribution(600, 200);
  while(!this->exitFlag.load()) {
    // TODO: Uncomment me
    /*
    double nextActivityPeriod = distribution(generator);
    if(nextActivityPeriod < 10.0) {
      nextActivityPeriod = 10.0;
    }
    */
    double nextActivityPeriod = 1.0;
    std::cerr << "Waiting " << nextActivityPeriod << " seconds until reading messages..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(nextActivityPeriod * 1000)));
    std::cerr << "Reading messages..." << std::endl;
    while(this->toReadMessageQueue.size()) {
      this->queueMutex.lock();
      std::vector<td_api::int53> chats;
      std::cerr << "Chats: ";
      for(auto it = this->toReadMessageQueue.begin(); it != this->toReadMessageQueue.end(); ++it) {
        std::cerr << it->first << " ";
        chats.push_back(it->first);
      }
      std::cerr << std::endl;
      this->queueMutex.unlock();
      for(td_api::int53& chat : chats) {
        std::cerr << "Reading all messages from chat " << chat << std::endl;
        this->queueMutex.lock();
        td_api::object_ptr<td::td_api::openChat> openChat = td_api::make_object<td_api::openChat>();
        openChat->chat_id_ = chat;
        this->sendQuery(std::move(openChat), {});
        for(auto& message : this->toReadMessageQueue[chat]) {
          double timeToRead = getMessageReadTime(message);
          this->markMessageAsRead(chat, message->id_);
          std::cerr << "Read message " << message->id_ << std::endl;
          std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(timeToRead * 1000)));
        }
        td_api::object_ptr<td::td_api::closeChat> closeChat = td_api::make_object<td_api::closeChat>();
        closeChat->chat_id_ = chat;
        this->sendQuery(std::move(closeChat), {});
        this->toReadMessageQueue.erase(chat);
        this->queueMutex.unlock();
      }
    }
    std::cerr << "Going to sleep..." << std::endl;
  }
}

void TelegramRecorder::enqueue(std::shared_ptr<td_api::message>& message) {
  this->queueMutex.lock();
  if(this->toReadMessageQueue.find(message->chat_id_) == this->toReadMessageQueue.end()) {
    this->toReadMessageQueue[message->chat_id_] = std::vector<std::shared_ptr<td_api::message>>();
  }
  this->toReadMessageQueue[message->chat_id_].push_back(message);
  this->queueMutex.unlock();
}

void TelegramRecorder::markMessageAsRead(td_api::int53 chatID, td_api::int53 messageID) {
  td_api::object_ptr<td::td_api::viewMessages> viewMessages = td_api::make_object<td_api::viewMessages>();
  viewMessages->chat_id_ = chatID;
  // Message thread IDs are conversations that happen in certain channel's posts (the ones that allow it).
  // TODO: include this feature at some point
  viewMessages->message_thread_id_ = 0;
  std::vector<td_api::int53> messages = {messageID};
  viewMessages->message_ids_ = std::move(messages);
  this->sendQuery(std::move(viewMessages), {});
}