cmake_minimum_required(VERSION 3.22.1)
project(lordsbot C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The C bot lives outside the Gradle module so it stays a single source of
# truth shared with the desktop/Termux build. Files are listed explicitly
# rather than globbed, because a glob would silently pick up a stray .c and
# break the NDK build in a way that is annoying to diagnose.
set(BOT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/bot)

set(BOT_SOURCES
    ${BOT_DIR}/main.c
    ${BOT_DIR}/connection.c
    ${BOT_DIR}/log.c
    ${BOT_DIR}/protocol.c
    ${BOT_DIR}/des.c
    ${BOT_DIR}/map_point.c
    ${BOT_DIR}/command.c
    ${BOT_DIR}/config.c
    ${BOT_DIR}/auto.c
    ${BOT_DIR}/waves.c
    ${BOT_DIR}/bank.c
)

foreach(src ${BOT_SOURCES})
    if(NOT EXISTS ${src})
        message(FATAL_ERROR "Missing bot source: ${src}")
    endif()
endforeach()

add_library(bot SHARED jni.c ${BOT_SOURCES})

target_include_directories(bot PRIVATE ${BOT_DIR})
target_compile_options(bot PRIVATE -O2 -fvisibility=hidden -DVERSION="\"1.0.2\"")
target_compile_definitions(bot PRIVATE _GNU_SOURCE)

find_library(log-lib log)
target_link_libraries(bot ${log-lib})
