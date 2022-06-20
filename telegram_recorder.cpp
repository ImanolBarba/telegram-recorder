//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

// TODO: Video and voice chats

#include <filesystem>
#include <thread>

#include <unistd.h>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

#include "hash.hpp"
#include "telegram_data.hpp"
#include "telegram_recorder.hpp"

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

  create_directory(std::filesystem::current_path() / this->config.downloadFolder);

  std::thread recorderThread(&TelegramRecorder::runRecorder, this);
  recorderThread.detach();
  std::thread readerThread(&TelegramRecorder::runMessageReader, this);
  readerThread.detach();
  std::thread writerThread(&TelegramRecorder::runDBWriter, this);
  writerThread.detach();
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
  this->messagesAvailableToWrite.notify_all();
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
      // Most of these are lazy solutions because I don't wanna have specific 
      // UPDATE queries for every single thing. Sorry!
      [this](td_api::updateAuthorizationState& updateAutorizationState) {
        // Auth state changed
        SPDLOG_DEBUG("Received update: updateAuthorizationState");
        this->authState = std::move(updateAutorizationState.authorization_state_);
        onAuthStateUpdate();
      },
      [this](td_api::updateNewChat& updateNewChat) {
        // A new chat has been loaded/created
        SPDLOG_DEBUG("Received update: updateNewChat");
        this->retrieveAndWriteChatFromTelegram(updateNewChat.chat_->id_);
      },
      [this](td_api::updateChatTitle& updateChatTitle) {
        // The title of a chat was changed
        SPDLOG_DEBUG("Received update: updateChatTitle");
        this->retrieveAndWriteChatFromTelegram(updateChatTitle.chat_id_);
      },
      [this](td_api::updateUser& updateUser) {
        // Some data of a user has changed
        SPDLOG_DEBUG("Received update: updateUser");
        this->retrieveAndWriteUserFromTelegram(updateUser.user_->id_);
      },
      [this](td_api::updateChatPhoto& updateChatPhoto) {
        // Chat photo was changed
        SPDLOG_DEBUG("Received update: updateChatPhoto");
        this->retrieveAndWriteChatFromTelegram(updateChatPhoto.chat_id_);
      },
      [this](td_api::updateMessageContent& updateMessageContent) {
        // Message content changed
        SPDLOG_DEBUG("Received update: updateMessageContent");
        std::string compoundMessageID = std::to_string(updateMessageContent.chat_id_) + ":" + std::to_string(updateMessageContent.message_id_);
        this->updateMessageContent(compoundMessageID, updateMessageContent.new_content_);
      },
      [this](td_api::updateMessageEdited& updateMessageEdited) {
        // Message was edited
        SPDLOG_DEBUG("Received update: updateMessageEdited");
        this->updateMessageText(updateMessageEdited.chat_id_, updateMessageEdited.message_id_);
      },
      [this](td_api::updateUserFullInfo& updateUserFullInfo) {
        // Extended info of an user changed
        SPDLOG_DEBUG("Received update: updateUserFullInfo");
        this->retrieveAndWriteUserFromTelegram(updateUserFullInfo.user_id_);
      },
      [this](td_api::updateSupergroupFullInfo& updateSupergroupFullInfo) {
        // Extended info of a supergroup/channel changed
        SPDLOG_DEBUG("Received update: updateSupergroupFullInfo");
        // See https://github.com/tdlib/td/issues/2023
        this->retrieveAndWriteChatFromTelegram(-1 * updateSupergroupFullInfo.supergroup_id_);
      },
      [this](td_api::updateBasicGroupFullInfo& updateBasicGroupFullInfo) {
        // Extended info of a group changed
        SPDLOG_DEBUG("Received update: updateBasicGroupFullInfo");
        // See https://github.com/tdlib/td/issues/2023
        this->retrieveAndWriteChatFromTelegram(-1 * updateBasicGroupFullInfo.basic_group_id_);
      },
      [this](td_api::updateNewMessage& updateNewMessage) {
        // A new message was received
        SPDLOG_DEBUG("Received update: updateNewMessage");
        std::shared_ptr<td_api::message> message = std::shared_ptr<td_api::message>(updateNewMessage.message_.release());

        std::unique_ptr<TelegramUser>* senderPtr = this->userCache.get(getMessageSenderID(message));
        if(!senderPtr) {
          std::unique_ptr<TelegramUser> sender = this->retrieveUserFromDB(getMessageSenderID(message));
          if(sender) {
            this->userCache.put(getMessageSenderID(message), std::move(sender));
          } else {
            this->retrieveAndWriteUserFromTelegram(getMessageSenderID(message));
          }
        }

        std::unique_ptr<TelegramChat>* chatPtr = this->chatCache.get(message->chat_id_);
        if(!chatPtr) {
          std::unique_ptr<TelegramChat> chat = this->retrieveChatFromDB(message->chat_id_);
          if(chat) {
            this->chatCache.put(message->chat_id_, std::move(chat));
          } else {
            this->retrieveAndWriteChatFromTelegram(message->chat_id_);
          }
        }
        this->enqueueMessageToRead(message);
        this->enqueueMessageToWrite(message);
      },
      [](auto& update) {}
    }
  );
}

void TelegramRecorder::retrieveAndWriteChatFromTelegram(td_api::int53 chatID) {
  td_api::object_ptr<td::td_api::getChat> getChat = td_api::make_object<td_api::getChat>();
  getChat->chat_id_ = chatID;
  this->sendQuery(std::move(getChat), [this, chatID](TDAPIObjectPtr object) {
    if(object) {
      if(object->get_id() == td_api::error::ID) {
        td_api::object_ptr<td_api::error> err = td::move_tl_object_as<td_api::error>(object);
        SPDLOG_ERROR("Retrieve chat info for chat ID {} failed: {}", chatID, err->message_);
        return;
      }
      std::shared_ptr<td_api::chat> c = std::shared_ptr<td_api::chat>(td::move_tl_object_as<td_api::chat>(object).release());
      auto chatExtraInfoCallback = [this, c](TDAPIObjectPtr object) {
        if(object) {
          std::string description;
          std::string fileOrigin;
          std::string fileOriginID;
          if(c->photo_) {
            fileOrigin = std::to_string(c->id_);
            std::string fileIDStr = std::to_string(c->photo_->big_->id_) + ":" + fileOrigin;
            fileOriginID = SHA256(fileIDStr.c_str(), fileIDStr.size());
            this->downloadFile(*c->photo_->big_, fileOrigin);
          }
          // TODO: For some weird reason, the description is always the one the group was created with, it doesn't get updated
          if(object->get_id() == td_api::basicGroupFullInfo::ID) {
            td::tl::unique_ptr<td_api::basicGroupFullInfo> bgfi = td::move_tl_object_as<td_api::basicGroupFullInfo>(object);
            description = bgfi->description_;
          } else if(object->get_id() == td_api::supergroupFullInfo::ID) {
            td::tl::unique_ptr<td_api::supergroupFullInfo> sgfi = td::move_tl_object_as<td_api::supergroupFullInfo>(object);
            description = sgfi->description_;
          }
          TelegramChat* chat = new TelegramChat;
          chat->chatID = c->id_;
          chat->name = c->title_;
          chat->about = description;
          chat->profilePicFileID = fileOriginID;
          std::unique_ptr<TelegramChat> chatPtr = std::unique_ptr<TelegramChat>(chat);
          this->writeChatToDB(chatPtr);
          this->chatCache.put(chat->chatID, std::move(chatPtr));
        }
      };
      if(c->type_->get_id() == td_api::chatTypeSupergroup::ID) {
        td_api::object_ptr<td::td_api::getSupergroupFullInfo> getSupergroupFullInfo = td_api::make_object<td_api::getSupergroupFullInfo>();
        // See https://github.com/tdlib/td/issues/2023
        getSupergroupFullInfo->supergroup_id_ = -1 * c->id_;
        this->sendQuery(std::move(getSupergroupFullInfo), chatExtraInfoCallback);
      } else {
        td_api::object_ptr<td::td_api::getBasicGroupFullInfo> getBasicGroupFullInfo = td_api::make_object<td_api::getBasicGroupFullInfo>();
        // See https://github.com/tdlib/td/issues/2023
        getBasicGroupFullInfo->basic_group_id_ = -1 * c->id_;
        this->sendQuery(std::move(getBasicGroupFullInfo), chatExtraInfoCallback);
      }
    }
  });
}

void TelegramRecorder::retrieveAndWriteUserFromTelegram(td_api::int53 userID) {
  td_api::object_ptr<td_api::getUser> getUser = td_api::make_object<td_api::getUser>();
  getUser->user_id_ = userID;
  this->sendQuery(std::move(getUser), [this, userID](TDAPIObjectPtr object) {
    if(object) {
      if(object->get_id() == td_api::error::ID) {
        td_api::object_ptr<td_api::error> err = td::move_tl_object_as<td_api::error>(object);
        SPDLOG_ERROR("Retrieve user info for user ID {} failed: {}", userID, err->message_);
        return;
      }
      std::shared_ptr<td_api::user> u = std::shared_ptr<td_api::user>(td::move_tl_object_as<td_api::user>(object).release());
      // To get the bio...
      td_api::object_ptr<td_api::getUserFullInfo> getUserFullInfo = td_api::make_object<td_api::getUserFullInfo>();
      getUserFullInfo->user_id_ = u->id_;
      this->sendQuery(std::move(getUserFullInfo), [this, u](TDAPIObjectPtr object) {
        if(object) {
          td::tl::unique_ptr<td_api::userFullInfo> ufi = td::move_tl_object_as<td_api::userFullInfo>(object);
          std::string fileOrigin;
          std::string fileOriginID;
          if(u->profile_photo_ && u->profile_photo_->id_) {
            fileOrigin = std::to_string(u->id_);
            std::string fileIDStr = std::to_string(u->profile_photo_->big_->id_)  + ":" + fileOrigin;
            fileOriginID = SHA256(fileIDStr.c_str(), fileIDStr.size());
            this->downloadFile(*u->profile_photo_->big_, fileOrigin);
          }
          TelegramUser* user = new TelegramUser;
          user->userID = u->id_;
          user->fullName = u->first_name_ + " " + u->last_name_;
          user->userName = u->username_;
          user->profilePicFileID = fileOriginID;
          user->bio = ufi->bio_;
          std::unique_ptr<TelegramUser> userPtr = std::unique_ptr<TelegramUser>(user);
          this->writeUserToDB(userPtr);
          this->userCache.put(user->userID, std::move(userPtr));
        }
      });
    }
  });
}