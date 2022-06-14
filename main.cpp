//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <iostream>

#include "telegram_recorder.hpp"

int main(int argc, char** argv) {
  TelegramRecorder recorder;
  // TODO: start new thread
  recorder.start();

  // TODO: Implement basic interactive mode
  // TODO: Graceful shutdown
}