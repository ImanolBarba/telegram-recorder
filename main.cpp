//
// Copyright (c) 2022, Imanol-Mikel Barba Sabariego
// All rights reserved.
// 
// Distributed under BSD 3-Clause License. See LICENSE.

#include <iostream>

#include "telegram_recorder.hpp"

// TODO: Mark as read, simulate human behaviour
// TODO: Create sqlite db

int main(int argc, char** argv) {
  TelegramRecorder recorder;
  recorder.start();
}