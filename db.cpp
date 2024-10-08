//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <mutex>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

#include "hash.hpp"
#include "telegram_data.hpp"
#include "telegram_recorder.hpp"

bool checkTableExists(sqlite3* db, std::string tableName) {
  std::string statement = "SELECT COUNT(type) FROM sqlite_master WHERE type='table' AND name = ? ;";
  char *errMsg = NULL;
  int rc;
  bool exists = false;

  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing statement: {}", sqlite3_errmsg(db));
    return false;
  }

  rc = sqlite3_bind_text64(stmt, 1, tableName.c_str(), tableName.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
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
                              "timestamp INTEGER,"
                              "message TEXT,"
                              "message_type INTEGER,"
                              "content_file_id TEXT,"
                              "chat_id INTEGER,"
                              "sender_id INTEGER,"
                              "in_reply_of TEXT,"
                              "forwarded_from TEXT"
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
                              "usernames TEXT,"
                              "disabled_usernames TEXT,"
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
                              "group_id INTEGER,"
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
  if(!checkTableExists(this->db, std::move("files"))) {
    std::string statement = "CREATE TABLE files("
                              "file_id TEXT PRIMARY KEY,"
                              "downloaded_as TEXT,"
                              "origin_id TEXT"
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
          if (!message.get()) {
            SPDLOG_ERROR("Empty message in chat {}", chat);
            continue;
          }
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
  std::string origin = getMessageOrigin(message);
  std::string compoundMessageID = std::to_string(message->chat_id_) + ":" + std::to_string(message->id_);
  
  std::string fileOriginID;
  
  try {
    td_api::file* f = getMessageContentFileReference(message->content_);
    if(f) {
      std::string fileIDStr = std::to_string(f->id_) + ":" + compoundMessageID;
      fileOriginID = SHA256(fileIDStr.c_str(), fileIDStr.size());
      this->downloadFile(*f, compoundMessageID);
    }
  } catch(const std::runtime_error& e) {
    SPDLOG_WARN("Unable to download message data for message_id {} from chat_id {}. Storing anyway...", message->id_, message->chat_id_);
  }

  SPDLOG_INFO("Got message: [chat_id: {}] [from: {}]: {}", message->chat_id_, senderID, text);

  // We don't do REPLACE here because we rely on the hidden rowid column to preserve message order
  std::string statement = "INSERT INTO messages ("
                            "id,"
                            "timestamp,"
                            "message,"
                            "message_type,"
                            "content_file_id,"
                            "chat_id,"
                            "sender_id,"
                            "in_reply_of,"
                            "forwarded_from"
                          ") VALUES "
                          "( ?, ?, ?, ?, ?, ?, ?, ?, ?);";

  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  
  rc = sqlite3_bind_text64(stmt, 1, compoundMessageID.c_str(), compoundMessageID.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_int64(stmt, 2, message->date_);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_text64(stmt, 3, text.c_str(), text.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_int(stmt, 4, msgType);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  std::string contentFileID = (fileOriginID == "" ? "NULL" : fileOriginID);
  rc = sqlite3_bind_text64(stmt, 5, contentFileID.c_str(), contentFileID.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_int64(stmt, 6, message->chat_id_);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_int64(stmt, 7, senderID);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  std::string reply_to = "NULL";
  if (message->reply_to_.get() && message->reply_to_->get_id() == td_api::messageReplyToMessage::ID) {
    auto& replied_on = static_cast<td_api::messageReplyToMessage&>(*message->reply_to_);
    reply_to = std::to_string(replied_on.chat_id_) + ":" + std::to_string(replied_on.message_id_);
  }
  rc = sqlite3_bind_text64(stmt, 8, reply_to.c_str(), reply_to.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_text64(stmt, 9, origin.c_str(), origin.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }

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
  std::string statement = "SELECT name, group_id, about, pic_file_id FROM chats WHERE chat_id = ? ;";
  char *errMsg = NULL;
  int rc;
  TelegramChat *chat = NULL;
  bool exists = false;

  sqlite3_stmt *stmt;
  this->toWriteQueueMutex.lock();
  rc = sqlite3_prepare_v2(this->db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
      SPDLOG_ERROR("Error preparing statement: {}", sqlite3_errmsg(db));
      return std::unique_ptr<TelegramChat>(chat);
  }

  rc = sqlite3_bind_int64(stmt, 1, chatID);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return nullptr;
  }
  SPDLOG_DEBUG("Executing SQL: {}", statement);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    exists = true;
    chat = new TelegramChat;
    chat->chatID = chatID;
    chat->name = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    chat->groupID = sqlite3_column_int(stmt, 1);
    chat->about = sqlite3_column_text(stmt, 2) ? std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) : "";
    chat->profilePicFileID = sqlite3_column_text(stmt, 3) ? std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) : "";;
  }
  if (rc != SQLITE_DONE) {
    SPDLOG_ERROR("Error executing SQL: {}", sqlite3_errmsg(db));
  }
  sqlite3_finalize(stmt);
  this->toWriteQueueMutex.unlock();
  return std::unique_ptr<TelegramChat>(chat);
}

std::unique_ptr<TelegramUser> TelegramRecorder::retrieveUserFromDB(td_api::int53 userID) {
  std::string statement = "SELECT fullname, username, usernames, disabled_usernames, bio, profile_pic_file_id FROM users WHERE user_id = ? ;";
  char *errMsg = NULL;
  int rc;
  TelegramUser *user = NULL;
  bool exists = false;

  sqlite3_stmt *stmt;
  this->toWriteQueueMutex.lock();
  rc = sqlite3_prepare_v2(this->db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
      SPDLOG_ERROR("Error preparing statement: {}", sqlite3_errmsg(db));
      return std::unique_ptr<TelegramUser>(user);
  }

  rc = sqlite3_bind_int64(stmt, 1, userID);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return nullptr;
  }

  SPDLOG_DEBUG("Executing SQL: {}", statement);
  while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
    exists = true;
    user = new TelegramUser;
    user->userID = userID;
    user->fullName = std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0)));
    user->activeUserName = sqlite3_column_text(stmt, 1) ? std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) : ""; 
    user->userNames = sqlite3_column_text(stmt, 2) ? std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) : "";
    user->disabledUserNames = sqlite3_column_text(stmt, 3) ? std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) : "";
    user->bio = sqlite3_column_text(stmt, 4) ? std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))) : "";
    user->profilePicFileID = sqlite3_column_text(stmt, 5) ? std::string(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5))) : "";
  }
  if (rc != SQLITE_DONE) {
    SPDLOG_ERROR("Error executing SQL: {}", sqlite3_errmsg(db));
  }
  sqlite3_finalize(stmt);
  this->toWriteQueueMutex.unlock();
  return std::unique_ptr<TelegramUser>(user);
}

bool TelegramRecorder::writeUserToDB(std::unique_ptr<TelegramUser>& user) {
  SPDLOG_DEBUG("Writing user {} to DB", user->userID);
  int rc;
  char* errMsg = NULL;

  std::string statement = "REPLACE INTO users ("
                            "user_id,"
                            "fullname,"
                            "username,"
                            "usernames,"
                            "disabled_usernames,"
                            "bio,"
                            "profile_pic_file_id"
                          ") VALUES "
                          "(?, ?, ?, ?, ?, ?, ?);";
  
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  
  rc = sqlite3_bind_int64(stmt, 1, user->userID);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_text64(stmt, 2, user->fullName.c_str(), user->fullName.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  std::string activeUserName = (user->activeUserName == "" ? "NULL" : user->activeUserName);
  rc = sqlite3_bind_text64(stmt, 3, activeUserName.c_str(), activeUserName.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  std::string userNames = (user->userNames == "" ? "NULL" : user->userNames);
  rc = sqlite3_bind_text64(stmt, 4, userNames.c_str(), userNames.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  std::string disabledUserNames = (user->disabledUserNames == "" ? "NULL" : user->disabledUserNames);
  rc = sqlite3_bind_text64(stmt, 5, disabledUserNames.c_str(), disabledUserNames.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  std::string bio = (user->bio == "" ? "NULL" : user->bio);
  rc = sqlite3_bind_text64(stmt, 6, bio.c_str(), bio.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  std::string profilePicFileID = (user->profilePicFileID == "" ? "NULL" : user->profilePicFileID);
  rc = sqlite3_bind_text64(stmt, 7, profilePicFileID.c_str(), profilePicFileID.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }

  SPDLOG_DEBUG("Executing SQL: {}", statement);

  this->toWriteQueueMutex.lock();
  rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
  this->toWriteQueueMutex.unlock();
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

  std::string statement = "REPLACE INTO chats ("
                            "chat_id,"
                            "group_id,"
                            "name,"
                            "about,"
                            "pic_file_id"
                          ") VALUES "
                          "(?, ?, ?, ?, ?);";
  
  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  
  rc = sqlite3_bind_int64(stmt, 1, chat->chatID);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  if (!chat->groupID) {
    rc = sqlite3_bind_null(stmt, 2);
  } else {
    rc = sqlite3_bind_int64(stmt, 2, chat->groupID);
  }
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }

  rc = sqlite3_bind_text64(stmt, 3, chat->name.c_str(), chat->name.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  std::string about = (chat->about == "" ? "NULL" : chat->about);
  rc = sqlite3_bind_text64(stmt, 4, about.c_str(), about.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  std::string profilePicFileID = (chat->profilePicFileID == "" ? "NULL" : chat->profilePicFileID);
  rc = sqlite3_bind_text64(stmt, 5, profilePicFileID.c_str(), profilePicFileID.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  
  SPDLOG_DEBUG("Executing SQL: {}", statement);

  this->toWriteQueueMutex.lock();
  rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
  this->toWriteQueueMutex.unlock();
  if (rc != SQLITE_OK ) {
    SPDLOG_ERROR("Error inserting data: {}", errMsg);
    sqlite3_free(errMsg);
    return false;
  }
  return true;
}

bool TelegramRecorder::writeFileToDB(std::string& fileID, std::string& downloadedAs, const std::string& originID) {
  SPDLOG_DEBUG("Writing file {} to DB", fileID);
  int rc;
  char* errMsg = NULL;

  std::string statement = "REPLACE INTO files ("
                            "file_id,"
                            "downloaded_as,"
                            "origin_id"
                          ") VALUES "
                          "(?, ?, ?);";

  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }

  rc = sqlite3_bind_text64(stmt, 1, fileID.c_str(), fileID.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_text64(stmt, 2, downloadedAs.c_str(), downloadedAs.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_text64(stmt, 3, originID.c_str(), originID.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }

  SPDLOG_DEBUG("Executing SQL: {}", statement);

  this->toWriteQueueMutex.lock();
  rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
  this->toWriteQueueMutex.unlock();
  if (rc != SQLITE_OK ) {
    SPDLOG_ERROR("Error inserting data: {}", errMsg);
    sqlite3_free(errMsg);
    return false;
  }
  return true;
}

void TelegramRecorder::updateMessageText(td_api::int53 chatID, td_api::int53 messageID, td_api::int32 editDate) {
  td_api::object_ptr<td_api::getMessage> getMessage = td_api::make_object<td_api::getMessage>();
  getMessage->chat_id_ = chatID;
  getMessage->message_id_ = messageID;
  this->sendQuery(std::move(getMessage), [this, messageID, editDate](TDAPIObjectPtr object) {
    if(!object) {
      SPDLOG_ERROR("NULL response received when calling getMessage for message ID {}", messageID);
      return;
    }
    if(object->get_id() == td_api::error::ID) {
      td_api::object_ptr<td_api::error> err = td::move_tl_object_as<td_api::error>(object);
      SPDLOG_ERROR("Getting message {} failed: {}", messageID, err->message_);
      return;
    }
    std::shared_ptr<td_api::message> newMessage = std::shared_ptr<td_api::message>(td::move_tl_object_as<td_api::message>(object).release());
    std::string newText = getMessageText(newMessage);
    std::string compoundMessageID = std::to_string(newMessage->chat_id_) + ":" + std::to_string(newMessage->id_);
    SPDLOG_DEBUG("Updating message {}", compoundMessageID);

    int rc;
    char* errMsg = NULL;

    std::string statement = "UPDATE messages SET message = ?, timestamp = ? WHERE id = ?;";

    sqlite3_stmt *stmt;
    rc = sqlite3_prepare_v2(db, statement.c_str(), -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
      SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
      return;
    }

    rc = sqlite3_bind_text64(stmt, 1, newText.c_str(), newText.length(), SQLITE_STATIC, SQLITE_UTF8);
    if (rc != SQLITE_OK) {
      SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
      return;
    }
    rc = sqlite3_bind_int64(stmt, 2, editDate);
    if (rc != SQLITE_OK) {
      SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
      return;
    }
    rc = sqlite3_bind_text64(stmt, 3, compoundMessageID.c_str(), compoundMessageID.length(), SQLITE_STATIC, SQLITE_UTF8);
    if (rc != SQLITE_OK) {
      SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
      return;
    }

    SPDLOG_DEBUG("Executing SQL: {}", statement);

    this->toWriteQueueMutex.lock();
    rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
    this->toWriteQueueMutex.unlock();
    if (rc != SQLITE_OK ) {
      SPDLOG_ERROR("Error inserting data: {}", errMsg);
      sqlite3_free(errMsg);
    }

    if(!sqlite3_changes(this->db)) {
      SPDLOG_ERROR("No message was found with message ID: {}", compoundMessageID);
      return;
    }
  });
}

bool TelegramRecorder::updateMessageContent(std::string compoundMessageID, td_api::object_ptr<td_api::MessageContent>& newContent, td_api::int32 editDate) {
  SPDLOG_DEBUG("Updating content from message ID {}", compoundMessageID);

  td_api::file* f = getMessageContentFileReference(newContent);
  if(!f) {
    // No content to update
    return true;
  }

  std::string fileOrigin = std::to_string(f->id_) + ":" + compoundMessageID;
  std::string fileOriginID = SHA256(fileOrigin.c_str(), fileOrigin.size());
  this->downloadFile(*f, compoundMessageID);

  int rc;
  char* errMsg = NULL;

  
  std::string statement = "UPDATE messages SET content_file_id = ?, timestamp = ? WHERE id = ?;";

  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }

  rc = sqlite3_bind_text64(stmt, 1, fileOriginID.c_str(), fileOriginID.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_int64(stmt, 2, editDate);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_text64(stmt, 3, compoundMessageID.c_str(), compoundMessageID.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }

  SPDLOG_DEBUG("Executing SQL: {}", statement);

  this->toWriteQueueMutex.lock();
  rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
  this->toWriteQueueMutex.unlock();
  if (rc != SQLITE_OK ) {
    SPDLOG_ERROR("Error inserting data: {}", errMsg);
    sqlite3_free(errMsg);
    return false;
  }

  if(!sqlite3_changes(this->db)) {
    SPDLOG_ERROR("No message was found with message ID: {}", compoundMessageID);
    return false;
  }

  return true;
}

bool TelegramRecorder::updateGroupData(TDAPIObjectPtr groupData, td_api::int53 groupID) {
  int rc;
  char* errMsg = NULL;

  std::string description;

  if(groupData->get_id() == td_api::supergroupFullInfo::ID) {
    td_api::object_ptr<td_api::supergroupFullInfo> sgfi = td::move_tl_object_as<td_api::supergroupFullInfo>(groupData);
    SPDLOG_DEBUG("Updating group data for supergroup {}", groupID);
    description = sgfi->description_;
  } else if(groupData->get_id() == td_api::basicGroupFullInfo::ID) {
    td_api::object_ptr<td_api::basicGroupFullInfo> bgfi = td::move_tl_object_as<td_api::basicGroupFullInfo>(groupData);
    SPDLOG_DEBUG("Updating group data for basic group {}", groupID);
    description = bgfi->description_;
  } else {
    SPDLOG_ERROR("Unknown group data type to update: {}", groupData->get_id());
    return false;
  }

  std::string statement = "UPDATE chats SET about = ? WHERE group_id = ?;";

  sqlite3_stmt *stmt;
  rc = sqlite3_prepare_v2(db, statement.c_str(), -1, &stmt, NULL);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }

  rc = sqlite3_bind_text64(stmt, 1, description.c_str(), description.length(), SQLITE_STATIC, SQLITE_UTF8);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }
  rc = sqlite3_bind_int64(stmt, 2, groupID);
  if (rc != SQLITE_OK) {
    SPDLOG_ERROR("Error preparing SQL statement: {}", sqlite3_errmsg(db));
    return false;
  }

  SPDLOG_DEBUG("Executing SQL: {}", statement);

  this->toWriteQueueMutex.lock();
  rc = sqlite3_exec(this->db, statement.c_str(), 0, 0, &errMsg);
  this->toWriteQueueMutex.unlock();
  if (rc != SQLITE_OK ) {
    SPDLOG_ERROR("Error updating data: {}", errMsg);
    sqlite3_free(errMsg);
    return false;
  }

  if(!sqlite3_changes(this->db)) {
    SPDLOG_ERROR("No chat was found with group ID: {}", groupID);
    return false;
  }

  return true;

}