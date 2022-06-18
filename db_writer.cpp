//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <mutex>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

#include "telegram_recorder.hpp"

bool checkTableExists(sqlite3* db, std::string tableName) {
  std::string statement = "SELECT COUNT(type) FROM sqlite_master WHERE type='table' AND name='" + tableName + "';";
  char *errMsg = NULL;
  int rc;
  bool exists = false;

  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
      SPDLOG_ERROR("Error preparing statement: {}", sqlite3_errmsg(db));
      return false;
  }
  SPDLOG_DEBUG("Executing SQL: {}", statement);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    if(sqlite3_column_int(stmt, 0)) {
      exists = true;
    }
  }
  if (rc != SQLITE_DONE) {
    SPDLOG_ERROR("Error executing SQL: {}", sqlite3_errmsg(db));
    return false;
  }
  sqlite3_finalize(stmt);
  return exists;
}

bool TelegramRecorder::initDB() {
  char* errMsg = NULL;
  int rc = sqlite3_open("tgrec.db", &this->db);
  if(rc) {
    SPDLOG_ERROR("Unable to open database: {}", sqlite3_errmsg(this->db));
    return false;
  }
  if(!checkTableExists(this->db, std::move("messages"))) {
    std::string statement = "CREATE TABLE messages("
                              "id TEXT PRIMARY KEY,"
                              "message TEXT,"
                              "message_type INTEGER,"
                              "content_file_id TEXT,"
                              "chat_id INTEGER,"
                              "sender_id INTEGER,"
                              "in_reply_of TEXT,"
                              "forwarded_from INTEGER"
                            ");"
                            "CREATE INDEX from_sender_in_chat ON messages (sender_id, chat_id);";
    SPDLOG_DEBUG("Executing SQL: {}", statement);
    rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
    if (rc != SQLITE_OK ) {
      SPDLOG_ERROR("Error creating messages table: {}", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
  }
  if(!checkTableExists(this->db, std::move("users"))) {
    std::string statement = "CREATE TABLE users("
                              "user_id INTEGER PRIMARY KEY,"
                              "fullname TEXT,"
                              "username TEXT,"
                              "bio TEXT,"
                              "profile_pic_file_id TEXT"
                            ");";
    SPDLOG_DEBUG("Executing SQL: {}", statement);
    rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
    if (rc != SQLITE_OK ) {
      SPDLOG_ERROR("Error creating users table: {}", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
  }
  if(!checkTableExists(this->db, std::move("chats"))) {
    std::string statement = "CREATE TABLE chats("
                              "chat_id INTEGER PRIMARY KEY,"
                              "name TEXT,"
                              "about TEXT,"
                              "pic_file_id TEXT"
                            ");";
    SPDLOG_DEBUG("Executing SQL: {}", statement);
    rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
    if (rc != SQLITE_OK ) {
      SPDLOG_ERROR("Error creating chats table: {}", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
  }
  return true;
}

void TelegramRecorder::runDBWriter() {
  SPDLOG_DEBUG("DB Writer thread started");
  this->initDB();
  std::unique_lock<std::mutex> lk(this->toWriteQueueMutex);
  while(true) {
    this->messagesAvailableToWrite.wait(lk, [this]{return (this->toWriteMessageQueue.size() != 0 || this->exitFlag.load());});
    SPDLOG_INFO("DB Writer woke up!");
    while(this->toWriteMessageQueue.size()) {
      std::vector<td_api::int53> chats;
      for(auto it = this->toWriteMessageQueue.begin(); it != this->toWriteMessageQueue.end(); ++it) {
        std::cerr << it->first << " ";
        chats.push_back(it->first);
      }
      std::cerr << std::endl;
      this->toWriteQueueMutex.unlock();
      for(td_api::int53& chat : chats) {
        this->toWriteQueueMutex.lock();
        for(auto& message : this->toWriteMessageQueue[chat]) {
          this->writeMessageToDB(message);
        }
        this->toWriteMessageQueue.erase(chat);
        this->toWriteQueueMutex.unlock();
      }
    }
    SPDLOG_INFO("Finished writing messages to DB!");
    if(this->exitFlag.load()) {
      break;
    }
  }
  sqlite3_close(this->db);
  SPDLOG_INFO("DB is closed");
}

void TelegramRecorder::enqueueMessageToWrite(std::shared_ptr<td_api::message>& message) {
  this->toWriteQueueMutex.lock();
  SPDLOG_DEBUG("Enqueueing message {} from chat {}", message->id_, message->chat_id_);
  if(this->toWriteMessageQueue.find(message->chat_id_) == this->toWriteMessageQueue.end()) {
    this->toWriteMessageQueue[message->chat_id_] = std::vector<std::shared_ptr<td_api::message>>();
  }
  this->toWriteMessageQueue[message->chat_id_].push_back(message);
  this->toWriteQueueMutex.unlock();
  this->messagesAvailableToWrite.notify_one();
}

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

bool TelegramRecorder::writeMessageToDB(std::shared_ptr<td_api::message>& message) {
  SPDLOG_DEBUG("Writing message {} from chat {} to DB", message->id_, message->chat_id_);
  int rc;
  char* errMsg = NULL;

  int32_t msgType = message->content_->get_id();
  td_api::int53 senderID = getMessageSenderID(message);
  std::string text = getMessageText(message);
  
  std::string downloadedDataPath;
  try {
    downloadedDataPath = downloadMessageData(message);  
  } catch(const std::runtime_error& e) {
    SPDLOG_WARN("Unable to download message data for message_id {} from chat_id {}. Storing anyway...", message->id_, message->chat_id_);
  }

  SPDLOG_INFO("Got message: [chat_id: {}] [from: {}]: {}", message->chat_id_, senderID, text);

  std::string statement = "INSERT INTO messages ("
                            "id,"
                            "message,"
                            "message_type,"
                            "content_file_id,"
                            "chat_id,"
                            "sender_id,"
                            "in_reply_of,"
                            "forwarded_from"
                          ") VALUES "
                          "(";
  statement += "'" + std::to_string(message->chat_id_) + ":" + std::to_string(message->id_) + "',";
  statement += "'" + text + "',";
  statement += std::to_string(msgType) + ",";
  statement += (downloadedDataPath == "" ? "NULL" : downloadedDataPath) + ",";
  statement += std::to_string(message->chat_id_) + ",";
  statement += std::to_string(senderID) + ",";
  statement += (message->reply_to_message_id_ ? ("'" + std::to_string(message->reply_in_chat_id_) + ":" + std::to_string(message->reply_to_message_id_) + "'") : "NULL") + ",";
  statement += (message->forward_info_ == NULL ? "NULL" : (std::to_string(message->forward_info_->from_chat_id_) + ":" + std::to_string(message->forward_info_->from_message_id_)));
  statement += ");";
  SPDLOG_DEBUG("Executing SQL: {}", statement);

  rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
  if (rc != SQLITE_OK ) {
    SPDLOG_ERROR("Error inserting data: {}", errMsg);
      sqlite3_free(errMsg);
      return false;
  }
  return true;
}