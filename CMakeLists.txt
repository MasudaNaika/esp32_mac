cmake_minimum_required(VERSION 3.16)

# Component resolution happens before sdkconfig.defaults is loaded. Keep the
# publish project on the Waveshare ESP32-S3 target during that phase as well.
set(IDF_TARGET esp32s3 CACHE STRING "ESP-IDF target")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32_mac)
