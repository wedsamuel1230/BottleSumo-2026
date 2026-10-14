cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/firmware/bottlesumo_idf/components"
    "${CMAKE_CURRENT_LIST_DIR}/firmware/bottlesumo_idf/main")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/firmware/bottlesumo_idf/sdkconfig.defaults")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bottlesumo)
