//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
//
// Distributed under BSD 3-Clause License. See LICENSE.

#ifndef TELEGRAM_RECORDER_HPP
#define TELEGRAM_RECORDER_HPP

#include <atomic>
#include <condition_variable>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>

#include <sqlite3.h>
#include <td/telegram/td_api.hpp>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

#include "config.hpp"
#include "lru.hpp"

#define USER_CACHE_SIZE 32
#define CHAT_CACHE_SIZE 32

namespace td_api = td::td_api;

using TDAPIObjectPtr = td_api::object_ptr<td_api::Object>;

// C++17 overload pattern
template<class... Ts> struct overload : Ts... { using Ts::operator()...; };
template<class... Ts> overload(Ts...) -> overload<Ts...>;

typedef struct TelegramUser {
  td_api::int53 userID;
  std::string fullName;
  std::string userName;
  std::string bio;
  std::string profilePicFileID;
} TelegramUser;

typedef struct TelegramChat {
  td_api::int53 chatID;
  td_api::int53 groupID;
  std::string name;
  std::string about;
  std::string profilePicFileID;
} TelegramChat;

class TelegramRecorder {
  public:
    TelegramRecorder();
    void start();
    void stop();

  private:
    void runRecorder();
    bool loadConfig();
    void restart();
    void sendQuery(
      td_api::object_ptr<td_api::Function> func,
      std::function<void(TDAPIObjectPtr)> handler
    );
    void processResponse(td::ClientManager::Response response);
    void processUpdate(TDAPIObjectPtr update);
    auto createAuthQueryHandler();
    void onAuthStateUpdate();
    void checkAuthError(TDAPIObjectPtr object);
    void enqueueMessageToRead(std::shared_ptr<td_api::message>& message);
    void enqueueMessageToWrite(std::shared_ptr<td_api::message>& message);
    void runMessageReader();
    void markMessageAsRead(td_api::int53 chatID, td_api::int53 messageID);
    bool writeMessageToDB(std::shared_ptr<td_api::message>& message);
    std::unique_ptr<TelegramChat> retrieveChatFromDB(td_api::int53 chatID);
    std::unique_ptr<TelegramUser> retrieveUserFromDB(td_api::int53 userID);
    void retrieveAndWriteChatFromTelegram(td_api::int53 chatID);
    bool updateGroupData(TDAPIObjectPtr groupData, td_api::int53 groupID);
    void retrieveAndWriteUserFromTelegram(td_api::int53 userID);
    bool writeUserToDB(std::unique_ptr<TelegramUser>& user);
    bool writeChatToDB(std::unique_ptr<TelegramChat>& chat);
    bool writeFileToDB(std::string& fileID, std::string& downloadedAs, const std::string& originID);
    void updateMessageText(td_api::int53 chatID, td_api::int53 messageID);
    bool updateMessageContent(std::string compoundMessageID, td_api::object_ptr<td_api::MessageContent>& newContent);
    void downloadFile(td_api::file& file, std::string& originID);
    void runDBWriter();
    bool initDB();

    std::unique_ptr<td::ClientManager> clientManager;
    std::int32_t clientID{0};
    td_api::object_ptr<td_api::AuthorizationState> authState;
    bool authorized{false};
    bool needRestart{false};
    std::uint64_t currentQueryID{0};
    std::uint64_t authQueryID{0};
    std::map<std::uint64_t, std::function<void(TDAPIObjectPtr)>> handlers;
    std::atomic<bool> exitFlag{false};
    std::map<td_api::int53, std::vector<std::shared_ptr<td_api::message>>> toReadMessageQueue;
    std::map<td_api::int53, std::vector<std::shared_ptr<td_api::message>>> toWriteMessageQueue;
    std::mutex toReadQueueMutex;
    std::mutex toWriteQueueMutex;
    std::condition_variable messagesAvailableToWrite;
    ConfigParams config;
    sqlite3 *db;
    LRU<td_api::int53, std::unique_ptr<TelegramUser>> userCache{USER_CACHE_SIZE};
    LRU<td_api::int53, std::unique_ptr<TelegramChat>> chatCache{CHAT_CACHE_SIZE};
};

#endif
