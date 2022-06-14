#include <unistd.h>

#include <libconfig.h++>

#include "telegram_recorder.hpp"

#define DEFAULT_CONFIG_FILE "tgrec.conf"

TelegramRecorder::TelegramRecorder() {
    td::ClientManager::execute(td_api::make_object<td_api::setLogVerbosityLevel>(2));
    this->clientManager = std::make_unique<td::ClientManager>();
    this->clientID = this->clientManager->create_client_id();
}

void TelegramRecorder::start() {
  if(!this->loadConfig()) {
    std::cerr << "Unable to load configuration file" << std::endl;
    return;
  }
  this->sendQuery(td_api::make_object<td_api::getOption>("version"), {});
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

void TelegramRecorder::stop() {
  this->exitFlag = true;
}

bool TelegramRecorder::loadConfig() {
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

void TelegramRecorder::restart() {
  this->clientManager.reset();
  *this = TelegramRecorder();
}

void TelegramRecorder::sendQuery(
  td_api::object_ptr<td_api::Function> func,
  std::function<void(TDAPIObjectPtr)> handler
) {
  ++this->currentQueryID;
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
    auto it = this->handlers.find(response.request_id);
    if(it != this->handlers.end()) {
      // if a handler is found for the request ID, call it!
      it->second(std::move(response.object));
      this->handlers.erase(it);
    }
  } 
}

void TelegramRecorder::processUpdate(TDAPIObjectPtr update) {
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
        if (updateNewMessage.message_->content_->get_id() == td_api::messageText::ID) {
          text = static_cast<td_api::messageText&>(
            *updateNewMessage.message_->content_
          ).text_->text_;
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
      [](auto& update) {}
    }
  );
}