//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <thread>

#include <unistd.h>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

#include "telegram_recorder.hpp"

// Return types from the client come as an equivalent of unique_ptr, we need to
// convert those to shared_ptr since we are enqueuing the objects in two
// different threads (dbwriter, msgreader)
template<class T>
std::shared_ptr<T> mkshared(td_api::object_ptr<T>& ptr) {
    std::shared_ptr<T> shared(ptr.release());
    return shared;
}

TelegramRecorder::TelegramRecorder() {
    td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(2));
    this->clientManager = std::make_unique<td::ClientManager>();
    this->clientID = this->clientManager->create_client_id();
}

void TelegramRecorder::start() {
   if(!this->loadConfig()) {
    SPDLOG_ERROR("Unable to load configuration file");
    return;
  }
  std::thread recorderThread(&TelegramRecorder::runRecorder, this);
  recorderThread.detach();
  std::thread readerThread(&TelegramRecorder::runMessageReader, this);
  readerThread.detach();
} 

void TelegramRecorder::runRecorder() {
  SPDLOG_DEBUG("Recorder thread started");
  this->sendQuery(td_api::make_object<td_api::getOption>("version"), {});
  while(!this->exitFlag.load()) {
    if (this->needRestart) {
      this->restart();
    } else if (!this->authorized) {
      this->processResponse(this->clientManager->receive(10));
    } else {
      bool updatesAvailable = false;
      do {
        updatesAvailable = false;
        auto response = this->clientManager->receive(0);
        if(response.object) {
          // TODO: Do this in new thread
          updatesAvailable = true;
          this->processResponse(std::move(response));
        }
      }while(updatesAvailable);
      sleep(1);
    }
  }
  SPDLOG_DEBUG("Recorder stopped");
  this->sendQuery(td_api::make_object<td_api::logOut>(), {});
  this->sendQuery(td_api::make_object<td_api::close>(), {});
}

void TelegramRecorder::stop() {
  this->exitFlag = true;
}

void TelegramRecorder::restart() {
  SPDLOG_INFO("Restarting recorder");
  this->clientManager.reset();
  this->clientManager = std::make_unique<td::ClientManager>();
  this->clientID = this->clientManager->create_client_id();
  this->authorized = false;
  this->needRestart = false;
  this->currentQueryID = 0;
  this->authQueryID = 0;
  this->handlers.clear();
  this->toReadMessageQueue.clear();
  this->sendQuery(td_api::make_object<td_api::getOption>("version"), {});
}

void TelegramRecorder::sendQuery(
  td_api::object_ptr<td_api::Function> func,
  std::function<void(TDAPIObjectPtr)> handler
) {
  ++this->currentQueryID;
  SPDLOG_DEBUG("Sending query type {} with ID {}", func->get_id(), this->currentQueryID);
  if(handler) {
    this->handlers.emplace(this->currentQueryID, std::move(handler));
  }
  this->clientManager->send(this->clientID, this->currentQueryID, std::move(func));
}

void TelegramRecorder::processResponse(td::ClientManager::Response response) {
  if(response.object) {
    if(!response.request_id) {
      // request_id value of 0 indicates an update from TDLib
      this->processUpdate(std::move(response.object));
      return;
    }
    SPDLOG_DEBUG("Processing response for request ID {}", response.request_id);
    auto it = this->handlers.find(response.request_id);
    if(it != this->handlers.end()) {
      // if a handler is found for the request ID, call it!
      it->second(std::move(response.object));
      this->handlers.erase(it);
    }
  } 
}

void TelegramRecorder::processUpdate(TDAPIObjectPtr update) {
  SPDLOG_DEBUG("Processing Telegram update type {}", update->get_id());
  td_api::downcast_call(
    *update,
    overload {
      [this](td_api::updateAuthorizationState& updateAutorizationState) {
        // Auth state changed
        this->authState = std::move(updateAutorizationState.authorization_state_);
        onAuthStateUpdate();
      },
      [this](td_api::updateNewChat& updateNewChat) {
        // A new chat has been loaded/created
        // TODO
      },
      [this](td_api::updateChatTitle& updateChatTitle) {
        // The title of a chat was changed
        // TODO
      },
      [this](td_api::updateUser& updateUser) {
        // Some data of a user has changed
        // TODO
      },
      [this](td_api::updateNewMessage& updateNewMessage) {
        // A new message was received
        std::string text;
        std::shared_ptr<td_api::message> message = mkshared(updateNewMessage.message_);
        if (message->content_->get_id() == td_api::messageText::ID) {
          text = static_cast<td_api::messageText&>(
            *message->content_
          ).text_->text_;
        }
        td_api::int53 senderID;
        td_api::downcast_call(*message->sender_id_,
          overload {
            [this, &senderID](td_api::messageSenderUser &user) {
              senderID = user.user_id_;
            },
            [this, &senderID](td_api::messageSenderChat &chat) {
              senderID = chat.chat_id_;
            }
          }
        );

        // TODO: get user/chat details
        // TODO: maintain user/cache LRU, falling back to SQL, falling back to TGAPI
        // TODO: enqueue sql writes

        SPDLOG_INFO("Got message: [chat_id: {}] [from: {}]: {}", message->chat_id_, senderID, text);
        this->enqueue(message);
      },
      [](auto& update) {}
    }
  );
}