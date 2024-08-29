//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

#include "telegram_recorder.hpp"

auto TelegramRecorder::createAuthQueryHandler() {
  return [this, id = this->authQueryID](TDAPIObjectPtr object) {
    if(!object) {
      SPDLOG_ERROR("NULL response received when calling auth query handler for ID {}", id);
      return;
    }
    if(id == authQueryID) {
      checkAuthError(std::move(object));
    }
  };
}

/**
 *       func(static_cast<authorizationStateWaitTdlibParameters &>(obj));
      return true;
    case authorizationStateWaitPhoneNumber::ID:
      func(static_cast<authorizationStateWaitPhoneNumber &>(obj));
      return true;
    case authorizationStateWaitEmailAddress::ID:
      func(static_cast<authorizationStateWaitEmailAddress &>(obj));
      return true;
    case authorizationStateWaitEmailCode::ID:
      func(static_cast<authorizationStateWaitEmailCode &>(obj));
      return true;
    case authorizationStateWaitCode::ID:
      func(static_cast<authorizationStateWaitCode &>(obj));
      return true;
    case authorizationStateWaitOtherDeviceConfirmation::ID:
      func(static_cast<authorizationStateWaitOtherDeviceConfirmation &>(obj));
      return true;
    case authorizationStateWaitRegistration::ID:
      func(static_cast<authorizationStateWaitRegistration &>(obj));
      return true;
    case authorizationStateWaitPassword::ID:
      func(static_cast<authorizationStateWaitPassword &>(obj));
      return true;
    case authorizationStateReady::ID:
      func(static_cast<authorizationStateReady &>(obj));
      return true;
    case authorizationStateLoggingOut::ID:
      func(static_cast<authorizationStateLoggingOut &>(obj));
      return true;
    case authorizationStateClosing::ID:
      func(static_cast<authorizationStateClosing &>(obj));
      return true;
    case authorizationStateClosed::ID:
      func(static_cast<authorizationStateClosed &>(obj));
 */

void TelegramRecorder::onAuthStateUpdate() {
  ++this->authQueryID;
  td_api::downcast_call(
    *this->authState,
    overload {
      [this](td_api::authorizationStateReady&) {
        this->authorized = true;
        SPDLOG_INFO("Got authorization");
      },
      [this](td_api::authorizationStateLoggingOut&) {
        this->authorized = false;
        SPDLOG_INFO("Logging out");
      },
      [this](td_api::authorizationStateClosing&) {
        SPDLOG_INFO("Closing");
      },
      [this](td_api::authorizationStateClosed&) {
        this->authorized = false;
        this->needRestart = true;
        SPDLOG_WARN("Authorisation terminated");
      },
      [this](td_api::authorizationStateWaitCode&) {
        std::cout << "Enter authentication code: " << std::flush;
        std::string code;
        std::getline(std::cin, code);
        this->sendQuery(
          td_api::make_object<td_api::checkAuthenticationCode>(code),
          this->createAuthQueryHandler()
        );
      },
      [this](td_api::authorizationStateWaitRegistration &) {
//        this->sendQuery(
//          td_api::make_object<td_api::registerUser>(this->config.firstName, this->config.lastName),
//          this->createAuthQueryHandler()
//        );
      },
      [this](td_api::authorizationStateWaitPassword&) {
        std::cout << "Enter authentication password: " << std::flush;
        std::string password;
        std::getline(std::cin, password);
        this->sendQuery(
          td_api::make_object<td_api::checkAuthenticationPassword>(password),
          this->createAuthQueryHandler()
        );
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
              phoneNumber,
              nullptr
          ),
          this->createAuthQueryHandler()
        );
      },
      [this](td_api::authorizationStateWaitEmailAddress&) {
          std::cout << "Enter email: " << std::flush;
          // TODO
      },
      [this](td_api::authorizationStateWaitEmailCode&) {
          std::cout << "Enter email: " << std::flush;
          // TODO
      },
      [this](td_api::authorizationStateWaitTdlibParameters&) {
        auto params = td_api::make_object<td_api::setTdlibParameters>();
        params->database_directory_ = "tdlib";
        params->use_message_database_ = true;
        params->use_secret_chats_ = true;
        params->api_id_ = this->config.apiID;
        params->api_hash_ = this->config.apiHash;
        params->system_language_code_ = "en";
        params->device_model_ = "Desktop";
        params->application_version_ = "1.0";
        this->sendQuery(
          td_api::make_object<td_api::setTdlibParameters>(std::move(*params)),
          this->createAuthQueryHandler()
        );
          return true;
      }
    }
  );
}

void TelegramRecorder::checkAuthError(TDAPIObjectPtr object) {
  if (object->get_id() == td_api::error::ID) {
    auto error = td::move_tl_object_as<td_api::error>(object);
    SPDLOG_ERROR("Authorisation Error: {}", to_string(error));
    this->onAuthStateUpdate();
  }
}