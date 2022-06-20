//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <mutex>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

#include "telegram_data.hpp"
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
                              "content_file_id INTEGER,"
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
                              "profile_pic_file_id INTEGER"
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
                              "pic_file_id INTEGER"
                            ");";
    SPDLOG_DEBUG("Executing SQL: {}", statement);
    rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
    if (rc != SQLITE_OK ) {
      SPDLOG_ERROR("Error creating chats table: {}", errMsg);
        sqlite3_free(errMsg);
        return false;
    }
  }
  if(!checkTableExists(this->db, std::move("files"))) {
    std::string statement = "CREATE TABLE files("
                              "file_id INTEGER PRIMARY KEY,"
                              "downloaded_as TEXT"
                            ");";
    SPDLOG_DEBUG("Executing SQL: {}", statement);
    rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
    if (rc != SQLITE_OK ) {
      SPDLOG_ERROR("Error creating files table: {}", errMsg);
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

bool TelegramRecorder::writeMessageToDB(std::shared_ptr<td_api::message>& message) {
  SPDLOG_DEBUG("Writing message {} from chat {} to DB", message->id_, message->chat_id_);
  int rc;
  char* errMsg = NULL;

  int32_t msgType = message->content_->get_id();
  td_api::int53 senderID = getMessageSenderID(message);
  std::string text = getMessageText(message);
  
  td_api::int32 fileID = 0;
  try {
    td_api::file* f = getMessageContentFileReference(message);
    if(f) {
      fileID = f->id_;
      this->downloadFile(*f);
    }
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
  statement += (fileID == 0 ? "NULL" : std::to_string(fileID)) + ",";
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

std::unique_ptr<TelegramChat> TelegramRecorder::retrieveChatFromDB(td_api::int53 chatID) {
  std::string statement = "SELECT name, about, pic_file_id FROM chats WHERE chat_id='" + std::to_string(chatID) + "';";
  char *errMsg = NULL;
  int rc;
  TelegramChat *chat = NULL;
  bool exists = false;

  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(this->db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
      SPDLOG_ERROR("Error preparing statement: {}", sqlite3_errmsg(db));
      return std::unique_ptr<TelegramChat>(chat);
  }
  SPDLOG_DEBUG("Executing SQL: {}", statement);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    exists = true;
    chat = new TelegramChat;
    chat->chatID = chatID;
    chat->name = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    chat->about = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    chat->profilePicFileID = sqlite3_column_int(stmt, 2);
  }
  if (rc != SQLITE_DONE) {
    SPDLOG_ERROR("Error executing SQL: {}", sqlite3_errmsg(db));
  }
  sqlite3_finalize(stmt);
  return std::unique_ptr<TelegramChat>(chat);
}

std::unique_ptr<TelegramUser> TelegramRecorder::retrieveUserFromDB(td_api::int53 userID) {
  std::string statement = "SELECT fullname, username, bio, profile_pic_file_id FROM users WHERE user_id='" + std::to_string(userID) + "';";
  char *errMsg = NULL;
  int rc;
  TelegramUser *user = NULL;
  bool exists = false;

  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(this->db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
      SPDLOG_ERROR("Error preparing statement: {}", sqlite3_errmsg(db));
      return std::unique_ptr<TelegramUser>(user);
  }
  SPDLOG_DEBUG("Executing SQL: {}", statement);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    exists = true;
    user = new TelegramUser;
    user->userID = userID;
    user->fullName = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    user->userName = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1)));
    user->bio = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2)));
    user->profilePicFileID = sqlite3_column_int(stmt, 3);
  }
  if (rc != SQLITE_DONE) {
    SPDLOG_ERROR("Error executing SQL: {}", sqlite3_errmsg(db));
  }
  sqlite3_finalize(stmt);
  return std::unique_ptr<TelegramUser>(user);
}

bool TelegramRecorder::writeUserToDB(std::unique_ptr<TelegramUser>& user) {
  SPDLOG_DEBUG("Writing user {} to DB", user->userID);
  int rc;
  char* errMsg = NULL;

  std::string statement = "INSERT INTO users ("
                            "user_id,"
                            "fullname,"
                            "username,"
                            "bio,"
                            "profile_pic_file_id"
                          ") VALUES "
                          "(";
  statement += std::to_string(user->userID) + ",";
  statement += "'" + user->fullName + "',";
  statement += "'" + user->userName + "',";
  statement += "'" + user->bio + "',";
  statement += std::to_string(user->profilePicFileID);
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

bool TelegramRecorder::writeChatToDB(std::unique_ptr<TelegramChat>& chat) {
  SPDLOG_DEBUG("Writing chat {} to DB", chat->chatID);
  int rc;
  char* errMsg = NULL;

  std::string statement = "INSERT INTO chats ("
                            "chat_id,"
                            "name,"
                            "about,"
                            "pic_file_id"
                          ") VALUES "
                          "(";
  statement += std::to_string(chat->chatID) + ",";
  statement += "'" + chat->name + "',";
  statement += "'" + chat->about + "',";
  statement += std::to_string(chat->profilePicFileID);
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

bool TelegramRecorder::writeFileToDB(td_api::int32 fileID, std::string& downloadedAs) {
  SPDLOG_DEBUG("Writing file {} to DB", fileID);
  int rc;
  char* errMsg = NULL;

  std::string statement = "INSERT INTO files ("
                            "file_id,"
                            "downloaded_as"
                          ") VALUES "
                          "(";
  statement += std::to_string(fileID) + ",";
  statement += "'" + downloadedAs + "'";
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