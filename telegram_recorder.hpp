//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#ifndef TELEGRAM_RECORDER_HPP
#define TELEGRAM_RECORDER_HPP

#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>

#include <td/telegram/td_api.hpp>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

namespace td_api = td::td_api;

using TDAPIObjectPtr = td_api::object_ptr<td_api::Object>;

// C++17 overload pattern
template<class... Ts> struct overload : Ts... { using Ts::operator()...; };
template<class... Ts> overload(Ts...) -> overload<Ts...>;

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
  void enqueue(std::shared_ptr<td_api::message>& message);
  void runMessageReader();
  void markMessageAsRead(td_api::int53 chatID, td_api::int53 messageID);

  std::unique_ptr<td::ClientManager> clientManager;
  std::int32_t clientID{0};
  td_api::object_ptr<td_api::AuthorizationState> authState;
  bool authorized{false};
  bool needRestart{false};
  std::uint64_t currentQueryID{0};
  std::uint64_t authQueryID{0};
  std::map<std::uint64_t, std::function<void(TDAPIObjectPtr)>> handlers;
  std::atomic<bool> exitFlag{false};
  int apiID;
  std::string apiHash;
  std::string firstName;
  std::string lastName;
  std::map<td_api::int53, std::vector<std::shared_ptr<td_api::message>>> toReadMessageQueue;
  std::mutex queueMutex;  
};

#endif