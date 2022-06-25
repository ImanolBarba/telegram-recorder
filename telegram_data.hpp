//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#ifndef TELEGRAM_DATA_HPP
#define TELEGRAM_DATA_HPP

#include "telegram_recorder.hpp"

td_api::int53 getMessageSenderID(std::shared_ptr<td_api::message>& message);
std::string getMessageText(std::shared_ptr<td_api::message>& message);
std::string getMessageOrigin(std::shared_ptr<td_api::message>& message);
td::td_api::file* getMessageContentFileReference(td_api::object_ptr<td_api::MessageContent>& message);

#endif