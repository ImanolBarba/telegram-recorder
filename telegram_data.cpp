//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <filesystem>

#include "hash.hpp"
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
  } else if(message->content_->get_id() == td_api::messageVideo::ID) {
    td_api::messageVideo& msgVideo = static_cast<td_api::messageVideo&>(*message->content_);
    return msgVideo.caption_->text_;
  } else if(message->content_->get_id() == td_api::messagePhoto::ID) {
    td_api::messagePhoto& msgPhoto = static_cast<td_api::messagePhoto&>(*message->content_);
    return msgPhoto.caption_->text_;
  } else if(message->content_->get_id() == td_api::messageDocument::ID) {
    td_api::messageDocument& msgDoc = static_cast<td_api::messageDocument&>(*message->content_);
    return msgDoc.caption_->text_;
  }
  return "";
}

unsigned int getLargestPhotoIndex(td_api::array<td_api::object_ptr<td_api::photoSize>>& photoSizes) {
  unsigned int largestSize = 0;
  unsigned int largestIndex = 0;
  for(unsigned int i = 0; i < photoSizes.size(); ++i) {
    unsigned int picSize = photoSizes[i]->height_ * photoSizes[i]->width_;
    if(picSize > largestSize) {
      largestSize = picSize;
      largestIndex = i;
    }
  }
  return largestIndex;
}

td::td_api::file* getMessageContentFileReference(td_api::object_ptr<td_api::MessageContent>& content) {
  if (content->get_id() == td_api::messageText::ID) {
    // no data
  } else if(content->get_id() == td_api::messageVideo::ID) {
    td_api::messageVideo& msgVideo = static_cast<td_api::messageVideo&>(*content);
    return msgVideo.video_->video_.get();
  } else if(content->get_id() == td_api::messagePhoto::ID) {
    td_api::messagePhoto& msgPhoto = static_cast<td_api::messagePhoto&>(*content);
    unsigned int largestIndex = getLargestPhotoIndex(msgPhoto.photo_->sizes_);
    return msgPhoto.photo_->sizes_[largestIndex]->photo_.get();
  } else if(content->get_id() == td_api::messageDocument::ID) {
    td_api::messageDocument& msgDoc = static_cast<td_api::messageDocument&>(*content);
    return msgDoc.document_->document_.get();
  }
  return NULL;
}

void TelegramRecorder::downloadFile(td_api::file& file, std::string& originID) {
  SPDLOG_INFO("Enqueuing download for file ID {}", file.id_);
  td_api::object_ptr<td_api::downloadFile> downloadFile = td_api::make_object<td_api::downloadFile>();
  downloadFile->file_id_ = file.id_;
  downloadFile->priority_ = 1;
  downloadFile->offset_ = 0;
  downloadFile->limit_ = 0;
  downloadFile->synchronous_ = true;
  this->sendQuery(std::move(downloadFile), [this, id = file.id_, originID](TDAPIObjectPtr object) {
    if(object) {
      if(object->get_id() == td_api::error::ID) {
        td_api::object_ptr<td_api::error> err = td::move_tl_object_as<td_api::error>(object);
        SPDLOG_ERROR("Download for file ID {} failed: {}", id, err->message_);
        return;
      }
      SPDLOG_INFO("Download for file ID {} completed", id);
      td_api::object_ptr<td_api::file> f = td::move_tl_object_as<td_api::file>(object);
      if(!f->local_->is_downloading_completed_) {
        SPDLOG_ERROR("Download for file ID {} didn't complete successfully", id);
        return;
      }
      if(f->local_->path_ == "") {
        SPDLOG_ERROR("File ID {} isn't locally available", id);
        return;
      }
      std::string downloadPath = std::filesystem::path(this->config.downloadFolder) / std::filesystem::path(f->local_->path_).filename();
      try {
        std::filesystem::copy_file(f->local_->path_, downloadPath, std::filesystem::copy_options::skip_existing);
        std::string fileIDStr = std::to_string(f->id_) + ":" + originID;
        std::string fileID = SHA256(fileIDStr.c_str(), fileIDStr.size());
        this->writeFileToDB(fileID, downloadPath, originID);
      } catch(std::filesystem::filesystem_error& e) {
        SPDLOG_ERROR("Unable to copy file {}: {}", downloadPath, e.what());
      }
    }
  });
}