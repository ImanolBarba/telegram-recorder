//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <atomic>
#include <iostream>
#include <thread>

#include <signal.h>
#include <unistd.h>

#include "telegram_recorder.hpp"

namespace {
  volatile sig_atomic_t exitFlag;
}

void signalHandler(int signal) {
  exitFlag = 1;
}

int main(int argc, char** argv) {
  struct sigaction newAction;
  newAction.sa_handler = signalHandler;
  sigemptyset (&newAction.sa_mask);
  newAction.sa_flags = 0;
  sigaction (SIGINT, &newAction, NULL);
  sigaction (SIGTERM, &newAction, NULL);

  TelegramRecorder recorder;
  recorder.start();

  while(!exitFlag) {
    sleep(1);
  }

  recorder.stop();

  // TODO: Implement basic interactive mode
  // TODO: Logging system
}