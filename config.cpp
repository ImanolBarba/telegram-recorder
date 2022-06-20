//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <libconfig.h++>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>

#include "config.hpp"
#include "telegram_recorder.hpp"

bool TelegramRecorder::loadConfig() {
  libconfig::Config cfg;

  try {
    cfg.readFile(DEFAULT_CONFIG_FILE);
  } catch(const libconfig::FileIOException &fioex) {
    SPDLOG_ERROR("I/O error while reading config file");
    return false;
  } catch(const libconfig::ParseException &pex) {
    SPDLOG_ERROR("Parse error at {}:{} - {}", pex.getFile(), pex.getLine(), pex.getError());
    return false;
  }

  try {
    this->config = {
      std::move(cfg.lookup("api_id")),
      std::move(cfg.lookup("api_hash")),
      std::move(cfg.lookup("first_name")),
      std::move(cfg.lookup("first_name")),
      std::move(cfg.lookup("download_folder")),
      {
        std::move(cfg.lookup("read_msg_frequency_mean")),
        std::move(cfg.lookup("read_msg_frequency_std_dev")),
        std::move(cfg.lookup("read_msg_min_wait_sec")),
        std::move(cfg.lookup("text_read_speed_wpm")),
        std::move(cfg.lookup("photo_read_speed_sec"))
      }
    };
  } catch(const libconfig::SettingNotFoundException &nfex) {
    SPDLOG_ERROR("Missing configuration parameters: {}", nfex.getPath());
    return false;
  } catch(const libconfig::SettingTypeException &stex) {
    SPDLOG_ERROR("Malformed config found: {}", stex.getPath());
    return false;
  }
  return true;
}