cmake_minimum_required(VERSION 3.4 FATAL_ERROR)

project(TelegramRecorder VERSION 1.0 LANGUAGES CXX)

add_subdirectory(td)
find_library(libconfig libconfig)

add_executable(tgrec main.cpp telegram_recorder.cpp auth.cpp message_reader.cpp)
target_link_libraries(tgrec PRIVATE Td::TdStatic config++)
set_property(TARGET tgrec PROPERTY CXX_STANDARD 17)