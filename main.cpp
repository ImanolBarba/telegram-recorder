//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <atomic>
#include <iostream>
#include <thread>

#include <getopt.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_TRACE
#include <spdlog/spdlog.h>
#include <spdlog/sinks/daily_file_sink.h>

#include "telegram_recorder.hpp"

#define VERSION "1.0"

namespace {
  volatile sig_atomic_t exitFlag;
}

static struct option longopts[] = {
    { "verbose",  no_argument,  NULL, 'v'},
    { "help",     no_argument,  NULL, 'h'},
    { "version",  no_argument,  NULL, 'V'},
    { NULL,       0,            NULL, 0  }
};

void signalHandler(int signal) {
  exitFlag = signal;
}

void printVersion(const char* argv) {
    std::cout << argv << " " << VERSION << std::endl;
}

void printHelp(const char* argv, bool longVersion = true) {
    std::cout << argv << "  [-v --verbose | -h --help | -V --version]" << std::endl;
    if(longVersion) {
        std::cout << " -v | --verbose Increase log level to DEBUG" << std::endl;
        std::cout << " -h | --help    Show this help" << std::endl;
        std::cout << " -V | --version Show current program version" << std::endl;
    }
}

int main(int argc, char** argv) {
  struct sigaction newAction;
  newAction.sa_handler = signalHandler;
  sigemptyset(&newAction.sa_mask);
  newAction.sa_flags = 0;
  sigaction(SIGINT, &newAction, NULL);
  sigaction(SIGTERM, &newAction, NULL);

  // INFO - 2022-06-16 19:15:43,006 [bot.py:251 doRelist() TID:140272650397440] whatever
  spdlog::set_pattern("%l - %Y-%m-%d %H:%M:%S %z [%s:%# %!() TID:%t] %^%v%$");
  spdlog::sinks::daily_file_sink_mt* fileSink = new spdlog::sinks::daily_file_sink_mt("tgrec.log", 0, 0);
  spdlog::default_logger_raw()->sinks().push_back(std::shared_ptr<spdlog::sinks::daily_file_sink_mt>(fileSink));

  int longIndex = 0;
  int c;

  while ((c = getopt_long(argc, argv, "Vhv", longopts, &longIndex)) != -1) {
    if(c == 'V') {
      printVersion(argv[0]);
      return 0;
    } else if(c == 'h') {
      printHelp(argv[0], true);
      return 0;
    } else if(c == 'v') {
      spdlog::default_logger_raw()->set_level(spdlog::level::debug);
      SPDLOG_DEBUG("Verbose mode enabled");
    } else {
      SPDLOG_ERROR("Unrecognised argument: {}",  argv[optind-1]);
      printHelp(argv[0], false);
      return 1;
    }
  }
  if(optind != argc) {
    SPDLOG_ERROR("Unrecognised argument: {}", argv[optind]);
    printHelp(argv[0], false);
    return 1;
  }

  SPDLOG_INFO("Starting Telegram Recorder...");
  TelegramRecorder recorder;
  recorder.start();

  while(!exitFlag) {
    sleep(1);
  }

  if(exitFlag != 1) {
    SPDLOG_INFO("Caught signal {}", exitFlag);
  }

  SPDLOG_INFO("Stopping Telegram Recorder...");
  recorder.stop();

  SPDLOG_INFO("Terminating...");
}