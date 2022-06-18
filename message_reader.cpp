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

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

#include "telegram_recorder.hpp"

double getMessageReadTime(std::shared_ptr<td_api::message>& message) {
  // TODO
  // get message type
  // get message params according to type
  // if text: 250 wpm
  // if video, length
  // if photo, 5 sec
  return 1.0;
}

void TelegramRecorder::runMessageReader() {
  SPDLOG_DEBUG("Reader thread started");
  std::random_device rd;
  std::default_random_engine generator(rd());
  std::normal_distribution<double> distribution(
    this->config.humanParams.readMsgFrequencyMean,
    this->config.humanParams.readMsgFrequencyStdDev
  );
  while(!this->exitFlag.load()) {
    double nextActivityPeriod = distribution(generator);
    if(nextActivityPeriod < this->config.humanParams.readMsgMinWaitSec) {
      nextActivityPeriod = this->config.humanParams.readMsgMinWaitSec;
    }
    SPDLOG_DEBUG("Waiting {:0.3f} seconds until reading messages...", nextActivityPeriod);
    std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(nextActivityPeriod * 1000)));
    SPDLOG_INFO("Reading messages...");
    while(this->toReadMessageQueue.size()) {
      this->toReadQueueMutex.lock();
      std::vector<td_api::int53> chats;
      for(auto it = this->toReadMessageQueue.begin(); it != this->toReadMessageQueue.end(); ++it) {
        std::cerr << it->first << " ";
        chats.push_back(it->first);
      }
      std::cerr << std::endl;
      this->toReadQueueMutex.unlock();
      for(td_api::int53& chat : chats) {
        this->toReadQueueMutex.lock();
        td_api::object_ptr<td::td_api::openChat> openChat = td_api::make_object<td_api::openChat>();
        openChat->chat_id_ = chat;
        this->sendQuery(std::move(openChat), {});
        for(auto& message : this->toReadMessageQueue[chat]) {
          double timeToRead = getMessageReadTime(message);
          this->markMessageAsRead(chat, message->id_);
          std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<int>(timeToRead * 1000)));
        }
        td_api::object_ptr<td::td_api::closeChat> closeChat = td_api::make_object<td_api::closeChat>();
        closeChat->chat_id_ = chat;
        this->sendQuery(std::move(closeChat), {});
        this->toReadMessageQueue.erase(chat);
        this->toReadQueueMutex.unlock();
      }
    }
    SPDLOG_INFO("Finished reading messages!");
  }
}

void TelegramRecorder::enqueueMessageToRead(std::shared_ptr<td_api::message>& message) {
  this->toReadQueueMutex.lock();
  SPDLOG_DEBUG("Enqueueing message {} from chat {}", message->id_, message->chat_id_);
  if(this->toReadMessageQueue.find(message->chat_id_) == this->toReadMessageQueue.end()) {
    this->toReadMessageQueue[message->chat_id_] = std::vector<std::shared_ptr<td_api::message>>();
  }
  this->toReadMessageQueue[message->chat_id_].push_back(message);
  this->toReadQueueMutex.unlock();
}

void TelegramRecorder::markMessageAsRead(td_api::int53 chatID, td_api::int53 messageID) {
  SPDLOG_DEBUG("Marking message {} from chat {} as read", messageID, chatID);
  td_api::object_ptr<td::td_api::viewMessages> viewMessages = td_api::make_object<td_api::viewMessages>();
  viewMessages->chat_id_ = chatID;
  // Message thread IDs are conversations that happen in certain channel's posts (the ones that allow it).
  // TODO: include this feature at some point
  viewMessages->message_thread_id_ = 0;
  std::vector<td_api::int53> messages = {messageID};
  viewMessages->message_ids_ = std::move(messages);
  this->sendQuery(std::move(viewMessages), {});
}