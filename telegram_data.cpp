//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include "telegram_data.hpp"

td_api::int53 getMessageSenderID(std::shared_ptr<td_api::message>& message) {
  td_api::int53 senderID;
  td_api::downcast_call(*message->sender_id_,
    overload {
      [&senderID](td_api::messageSenderUser &user) {
        senderID = user.user_id_;
      },
      [&senderID](td_api::messageSenderChat &chat) {
        senderID = chat.chat_id_;
      }
    }
  );
  return senderID;
}

std::string getMessageText(std::shared_ptr<td_api::message>& message) {
  if (message->content_->get_id() == td_api::messageText::ID) {
    return static_cast<td_api::messageText&>(
      *message->content_
    ).text_->text_;
  }
  // TODO: Add more message types
  return "";
}

std::string downloadMessageData(std::shared_ptr<td_api::message>& message) {
  if (message->content_->get_id() == td_api::messageText::ID) {
    // no data
  }
  // TODO: Add more message types
  return "";
}