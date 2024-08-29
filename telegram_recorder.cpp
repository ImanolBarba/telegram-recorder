//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

// TODO: Video and voice chats

#include <filesystem>
#include <thread>

#include <time.h>
#include <unistd.h>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

#include "hash.hpp"
#include "telegram_data.hpp"
#include "telegram_recorder.hpp"

std::string join(std::vector<std::string>& vec, char separator = '.') {
    std::ostringstream o;
    auto cur = vec.begin();
    if (cur != vec.end()) {
        o << *cur++;
        for (; cur != vec.end(); ++cur)
            o << separator << *cur;
    }
    return o.str();
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
  this->sendQuery(td_api::make_object<td_api::getOption>("version"), checkAPICallSuccess("version"));
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
  this->sendQuery(td_api::make_object<td_api::logOut>(), checkAPICallSuccess("logOut"));
  this->sendQuery(td_api::make_object<td_api::close>(), checkAPICallSuccess("close"));
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
  this->sendQuery(td_api::make_object<td_api::getOption>("version"), checkAPICallSuccess("version"));
}

void TelegramRecorder::sendQuery(
  td_api::object_ptr<td_api::Function> func,
  std::function<void(TDAPIObjectPtr)> handler
) {
  this->tdapiQueryMutex.lock();
  ++this->currentQueryID;
  SPDLOG_DEBUG("Sending query type {} with ID {}", func->get_id(), this->currentQueryID);
  if(handler) {
    this->handlers.emplace(this->currentQueryID, std::move(handler));
  }
  this->clientManager->send(this->clientID, this->currentQueryID, std::move(func));
  this->tdapiQueryMutex.unlock();
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
        this->updateMessageContent(compoundMessageID, updateMessageContent.new_content_, td_api::int32(time(0)));
      },
      [this](td_api::updateMessageEdited& updateMessageEdited) {
        // Message was edited
        SPDLOG_DEBUG("Received update: updateMessageEdited");
        this->updateMessageText(updateMessageEdited.chat_id_, updateMessageEdited.message_id_, updateMessageEdited.edit_date_);
      },
      [this](td_api::updateUserFullInfo& updateUserFullInfo) {
        // Extended info of an user changed
        SPDLOG_DEBUG("Received update: updateUserFullInfo");
        this->retrieveAndWriteUserFromTelegram(updateUserFullInfo.user_id_);
      },
      [this](td_api::updateSupergroupFullInfo& updateSupergroupFullInfo) {
        // Extended info of a supergroup/channel changed
        // NOTE: This update is not being triggered when the description is 
        // changed, we only get this info if the picture changes and a couple 
        // more occasions
        SPDLOG_DEBUG("Received update: updateSupergroupFullInfo");
        this->updateGroupData(td::move_tl_object_as<td_api::Object>(updateSupergroupFullInfo.supergroup_full_info_), updateSupergroupFullInfo.supergroup_id_);
      },
      [this](td_api::updateBasicGroupFullInfo& updateBasicGroupFullInfo) {
        // Extended info of a group changed
        SPDLOG_DEBUG("Received update: updateBasicGroupFullInfo");
        this->updateGroupData(td::move_tl_object_as<td_api::Object>(updateBasicGroupFullInfo.basic_group_full_info_), updateBasicGroupFullInfo.basic_group_id_);
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
    if(!object) {
      SPDLOG_ERROR("NULL response received when calling getChat for chat ID {}", chatID);
      return;
    }
    if(object->get_id() == td_api::error::ID) {
      td_api::object_ptr<td_api::error> err = td::move_tl_object_as<td_api::error>(object);
      SPDLOG_ERROR("Retrieve chat info for chat ID {} failed: {}", chatID, err->message_);
      return;
    }
    std::shared_ptr<td_api::chat> c = std::shared_ptr<td_api::chat>(td::move_tl_object_as<td_api::chat>(object).release());
    std::string fileOrigin;
    std::string fileOriginID;
    if(c->photo_) {
      fileOrigin = std::to_string(c->id_);
      std::string fileIDStr = std::to_string(c->photo_->big_->id_) + ":" + fileOrigin;
      fileOriginID = SHA256(fileIDStr.c_str(), fileIDStr.size());
      this->downloadFile(*c->photo_->big_, fileOrigin);
    }
    
    TelegramChat* chat = new TelegramChat;
    chat->chatID = c->id_;
    chat->name = c->title_;
    chat->about = "";
    chat->profilePicFileID = fileOriginID;
    std::unique_ptr<TelegramChat> chatPtr = std::unique_ptr<TelegramChat>(chat);

    if(c->type_->get_id() == td_api::chatTypeSupergroup::ID) {
      td_api::object_ptr<td::td_api::chatTypeSupergroup> chatTypeSupergroup = td::move_tl_object_as<td_api::chatTypeSupergroup>(c->type_);
      td_api::object_ptr<td::td_api::getSupergroupFullInfo> getSupergroupFullInfo = td_api::make_object<td_api::getSupergroupFullInfo>();
      getSupergroupFullInfo->supergroup_id_ = chatTypeSupergroup->supergroup_id_;
      
      chatPtr->groupID = chatTypeSupergroup->supergroup_id_;
      this->writeChatToDB(chatPtr);
      this->chatCache.put(chat->chatID, std::move(chatPtr));
    
      this->sendQuery(std::move(getSupergroupFullInfo), [this, groupID = chatTypeSupergroup->supergroup_id_](TDAPIObjectPtr object) {
        if(!object) {
          SPDLOG_ERROR("NULL response received when calling getSupergroupFullInfo for group ID {}", groupID);
          return;
        }
        if(object->get_id() == td_api::error::ID) {
          td_api::object_ptr<td_api::error> err = td::move_tl_object_as<td_api::error>(object);
          SPDLOG_ERROR("Retrieve group info for group ID {} failed: {}", groupID, err->message_);
          return;
        }
        td_api::object_ptr<td::td_api::supergroupFullInfo> sgfi = td::move_tl_object_as<td_api::supergroupFullInfo>(object);
        this->updateGroupData(td::move_tl_object_as<td_api::Object>(sgfi), groupID);
      });
    } else if(c->type_->get_id() == td_api::chatTypeBasicGroup::ID) {
      td_api::object_ptr<td::td_api::chatTypeBasicGroup> chatTypeBasicGroup = td::move_tl_object_as<td_api::chatTypeBasicGroup>(c->type_);
      td_api::object_ptr<td::td_api::getBasicGroupFullInfo> getBasicGroupFullInfo = td_api::make_object<td_api::getBasicGroupFullInfo>();
      getBasicGroupFullInfo->basic_group_id_ = chatTypeBasicGroup->basic_group_id_;

      chatPtr->groupID = chatTypeBasicGroup->basic_group_id_;
      this->writeChatToDB(chatPtr);
      this->chatCache.put(chat->chatID, std::move(chatPtr));

      this->sendQuery(std::move(getBasicGroupFullInfo), [this, groupID = chatTypeBasicGroup->basic_group_id_](TDAPIObjectPtr object) {
        if(!object) {
          SPDLOG_ERROR("NULL response received when calling getBasicGroupFullInfo for group ID {}", groupID);
          return;
        }
        if(object->get_id() == td_api::error::ID) {
          td_api::object_ptr<td_api::error> err = td::move_tl_object_as<td_api::error>(object);
          SPDLOG_ERROR("Retrieve group info for group ID {} failed: {}", groupID, err->message_);
          return;
        }
        td_api::object_ptr<td::td_api::basicGroupFullInfo> bgfi = td::move_tl_object_as<td_api::basicGroupFullInfo>(object);
        this->updateGroupData(td::move_tl_object_as<td_api::Object>(bgfi), groupID);
      });
    } else {
      this->writeChatToDB(chatPtr);
      this->chatCache.put(chat->chatID, std::move(chatPtr));
    }
  });
}

void TelegramRecorder::retrieveAndWriteUserFromTelegram(td_api::int53 userID) {
  td_api::object_ptr<td_api::getUser> getUser = td_api::make_object<td_api::getUser>();
  getUser->user_id_ = userID;
  this->sendQuery(std::move(getUser), [this, userID](TDAPIObjectPtr object) {
    if(!object) {
      SPDLOG_ERROR("NULL response received when calling getUser for user ID {}", userID);
      return;
    }
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
      if(!object) {
        SPDLOG_ERROR("NULL response received when calling getUserFullInfo for user ID {}", u->id_);
        return;
      }
      if(object->get_id() == td_api::error::ID) {
      td_api::object_ptr<td_api::error> err = td::move_tl_object_as<td_api::error>(object);
      SPDLOG_ERROR("Retrieve user full info for user ID {} failed: {}", u->id_, err->message_);
      return;
    }
      std::string fileOriginID;
      std::string bio;
      if(object->get_id() == td_api::error::ID) {
        td_api::object_ptr<td_api::error> err = td::move_tl_object_as<td_api::error>(object);
        SPDLOG_ERROR("Retrieve full user info for user ID {} failed: {}", u->id_, err->message_);
      } else {
        td::tl::unique_ptr<td_api::userFullInfo> ufi = td::move_tl_object_as<td_api::userFullInfo>(object);
        std::string fileOrigin;
        bio = ufi->bio_->text_;
        if(u->profile_photo_ && u->profile_photo_->id_) {
          fileOrigin = std::to_string(u->id_);
          std::string fileIDStr = std::to_string(u->profile_photo_->big_->id_)  + ":" + fileOrigin;
          fileOriginID = SHA256(fileIDStr.c_str(), fileIDStr.size());
          this->downloadFile(*u->profile_photo_->big_, fileOrigin);
        }
      }
      TelegramUser* user = new TelegramUser;
      user->userID = u->id_;
      user->fullName = (u->last_name_ == "" ? u->first_name_ : (u->first_name_ + " " + u->last_name_));
      if (u->usernames_) {
          auto usernames = join(u->usernames_->active_usernames_);
          auto disabled_usernames = join(u->usernames_->disabled_usernames_);
          if (!disabled_usernames.empty())  {
              usernames += "#" + disabled_usernames;
          }
          user->userName = usernames;
      }
      user->profilePicFileID = fileOriginID;
      user->bio = bio;
      std::unique_ptr<TelegramUser> userPtr = std::unique_ptr<TelegramUser>(user);
      this->writeUserToDB(userPtr);
      this->userCache.put(user->userID, std::move(userPtr));
    });
  });
}