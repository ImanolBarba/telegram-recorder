//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <functional>
#include <iostream>
#include <map>

#include <unistd.h>

#include <td/telegram/td_api.hpp>
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

#include <libconfig.h++>

#define DEFAULT_CONFIG_FILE "tgrec.conf"

// C++17 overload pattern
template<class... Ts> struct overload : Ts... { using Ts::operator()...; };
template<class... Ts> overload(Ts...) -> overload<Ts...>;

namespace td_api = td::td_api;

// TODO: Mark as read, simulate human behaviour
// TODO: Create sqlite db

class TelegramRecorder {
  public:
    TelegramRecorder() {
      td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(2));
      this->clientManager = std::make_unique<td::ClientManager>();
      this->clientID = this->clientManager->create_client_id();
      this->sendQuery(td_api::make_object<td_api::getOption>("version"), {});
    }

    void start() {
      if(!this->loadConfig()) {
        std::cerr << "Unable to load configuration file" << std::endl;
        return;
      }
      while(!this->exitFlag) {
        if (this->needRestart) {
          this->restart();
        } else if (!this->authorized) {
          this->processResponse(this->clientManager->receive(10));
        } else {
          std::cout << "Checking for updates..." << std::endl;
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
      this->sendQuery(td_api::make_object<td_api::logOut>(), {});
      this->sendQuery(td_api::make_object<td_api::close>(), {});
    }

    void stop() {
      this->exitFlag = true;
    }

 private:
  using TDAPIObjectPtr = td_api::object_ptr<td_api::Object>;

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

  bool loadConfig() {
    libconfig::Config cfg;
    
    try {
      cfg.readFile(DEFAULT_CONFIG_FILE);
    } catch(const libconfig::FileIOException &fioex) {
      std::cerr << "I/O error while reading file." << std::endl;
      return false;
    } catch(const libconfig::ParseException &pex) {
      std::cerr << "Parse error at " << pex.getFile() << ":" << pex.getLine()
                << " - " << pex.getError() << std::endl;
      return false;
    }

    try {
      const int apiIDConfValue = cfg.lookup("api_id");
      const std::string apiHashConfValue = cfg.lookup("api_hash");
      const std::string firstNameConfValue = cfg.lookup("first_name");
      const std::string lastNameConfValue = cfg.lookup("last_name");
      this->apiID = apiIDConfValue;
      this->apiHash = apiHashConfValue;
      this->firstName = firstNameConfValue;
      this->lastName = lastNameConfValue;
    } catch(const libconfig::SettingNotFoundException &nfex) {
      std::cerr << "Missing configuration parameters" << std::endl;
      return false;
    } catch(const libconfig::SettingTypeException &stex) {
      std::cerr << "Malformed config found" << std::endl;
      return false;
    }
    return true;
  }

  void restart() {
    this->clientManager.reset();
    *this = TelegramRecorder();
  }

  void sendQuery(td_api::object_ptr<td_api::Function> func,
                  std::function<void(TDAPIObjectPtr)> handler) {
    ++this->currentQueryID;
    if(handler) {
      this->handlers.emplace(this->currentQueryID, std::move(handler));
    }
    this->clientManager->send(this->clientID, this->currentQueryID, std::move(func));
  }

  void processResponse(td::ClientManager::Response response) {
    if(response.object) {
      if(!response.request_id) {
        // request_id value of 0 indicates an update from TDLib
        this->processUpdate(std::move(response.object));
        return;
      }
      auto it = this->handlers.find(response.request_id);
      if(it != this->handlers.end()) {
        // if a handler is found for the request ID, call it!
        it->second(std::move(response.object));
        this->handlers.erase(it);
      }
    } 
  }

  void processUpdate(TDAPIObjectPtr update) {
    td_api::downcast_call(
        *update,
        overload{
            [this](
                td_api::updateAuthorizationState& updateAutorizationState) {
                  // Auth state changed
              this->authState =
                  std::move(updateAutorizationState.authorization_state_);
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
              if (updateNewMessage.message_->content_->get_id() ==
                  td_api::messageText::ID) {
                text = static_cast<td_api::messageText&>(
                           *updateNewMessage.message_->content_)
                           .text_->text_;
              }
              td_api::int53 senderID;
              td_api::downcast_call(*updateNewMessage.message_->sender_id_,
                overload{
                  [this, &senderID](td_api::messageSenderUser &user) {
                    senderID = user.user_id_;
                  },
                  [this, &senderID](td_api::messageSenderChat &chat) {
                    senderID = chat.chat_id_;
                  }
                }
              );
              std::cout << "Got message: [chat_id:" << updateNewMessage.message_->chat_id_
                        << "] [from:" << senderID << "] [" << text << "]"
                        << std::endl;
            },
            [](auto& update) {}});
  }

  auto createAuthQueryHandler() {
    return [this, id = this->authQueryID](TDAPIObjectPtr object) {
      if(id == authQueryID) {
        checkAuthError(std::move(object));
      }
    };
  }

  void onAuthStateUpdate() {
    ++this->authQueryID;
    td_api::downcast_call(
        *this->authState,
        overload{
            [this](td_api::authorizationStateReady&) {
              this->authorized = true;
              std::cout << "Got authorization" << std::endl;
            },
            [this](td_api::authorizationStateLoggingOut&) {
              this->authorized = false;
              std::cout << "Logging out" << std::endl;
            },
            [this](td_api::authorizationStateClosing&) {
              std::cout << "Closing" << std::endl;
            },
            [this](td_api::authorizationStateClosed&) {
              this->authorized = false;
              this->needRestart = true;
              std::cout << "Terminated (Needs restart)" << std::endl;
            },
            [this](td_api::authorizationStateWaitCode&) {
              std::cout << "Enter authentication code: " << std::flush;
              std::string code;
              std::getline(std::cin, code);
              this->sendQuery(
                  td_api::make_object<td_api::checkAuthenticationCode>(code),
                  this->createAuthQueryHandler());
            },
            [this](td_api::authorizationStateWaitRegistration &) {
              // TODO: Read from config file using libconfig
              this->sendQuery(td_api::make_object<td_api::registerUser>(this->firstName, this->lastName),
                         this->createAuthQueryHandler());
            },
            [this](td_api::authorizationStateWaitPassword&) {
              std::cout << "Enter authentication password: " << std::flush;
              std::string password;
              std::getline(std::cin, password);
              this->sendQuery(
                  td_api::make_object<td_api::checkAuthenticationPassword>(
                      password),
                  this->createAuthQueryHandler());
            },
            [this](
                td_api::authorizationStateWaitOtherDeviceConfirmation& state) {
              std::cout << "Confirm this login link on another device: "
                        << state.link_ << std::endl;
            },
            [this](td_api::authorizationStateWaitPhoneNumber&) {
              std::cout << "Enter phone number: " << std::flush;
              std::string phoneNumber;
              std::getline(std::cin, phoneNumber);
              this->sendQuery(
                  td_api::make_object<td_api::setAuthenticationPhoneNumber>(
                      phoneNumber, nullptr),
                  this->createAuthQueryHandler());
            },
            [this](td_api::authorizationStateWaitEncryptionKey&) {
              // Default to an empty key (at this point who cares lmao)
              this->sendQuery(
                  td_api::make_object<td_api::checkDatabaseEncryptionKey>(
                      ""),
                  this->createAuthQueryHandler());
            },
            [this](td_api::authorizationStateWaitTdlibParameters&) {
              auto params = td_api::make_object<td_api::tdlibParameters>();
              params->database_directory_ = "tdlib";
              params->use_message_database_ = true;
              params->use_secret_chats_ = true;
              // TODO: Read from config file using libconfig
              params->api_id_ = this->apiID;
              params->api_hash_ = this->apiHash;
              params->system_language_code_ = "en";
              params->device_model_ = "Desktop";
              params->application_version_ = "1.0";
              params->enable_storage_optimizer_ = true;
              this->sendQuery(td_api::make_object<td_api::setTdlibParameters>(
                             std::move(params)),
                         this->createAuthQueryHandler());
            }});
  }

  void checkAuthError(TDAPIObjectPtr object) {
    if (object->get_id() == td_api::error::ID) {
      auto error = td::move_tl_object_as<td_api::error>(object);
      std::cout << "Error: " << to_string(error) << std::flush;
      this->onAuthStateUpdate();
    }
  }
};

int main(int argc, char** argv) {
  TelegramRecorder recorder;
  recorder.start();
}