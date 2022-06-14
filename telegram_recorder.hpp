#include <functional>
#include <iostream>
#include <map>

#include <td/telegram/td_api.hpp>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

#ifndef TELEGRAM_RECORDER_HPP
#define TELEGRAM_RECORDER_HPP

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
  std::unique_ptr<td::ClientManager> clientManager;
  std::int32_t clientID{0};
  td_api::object_ptr<td_api::AuthorizationState> authState;
  bool authorized{false};
  bool needRestart{false};
  std::uint64_t currentQueryID{0};
  std::uint64_t authQueryID{0};
  std::map<std::uint64_t, std::function<void(TDAPIObjectPtr)>> handlers;
  bool exitFlag{false};
  int apiID;
  std::string apiHash;
  std::string firstName;
  std::string lastName;

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
};

#endif