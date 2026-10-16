cmake_minimum_required(VERSION 3.16)

set(PROJECT_NAME "esp32-web-debugger")

# Replaced with the contents of the `version` file by the CI workflow
set(PROJECT_VER "DEV-VERSION")

# sdkconfig.defaults + sdkconfig.defaults.<target> are picked up automatically
# by ESP-IDF, so the same tree builds for every supported chip:
#   idf.py set-target esp32s3 && idf.py build

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(${PROJECT_NAME})

spiffs_create_partition_image(spiffs data FLASH_IN_PROJECT)
