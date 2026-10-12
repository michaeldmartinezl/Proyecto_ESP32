cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# Apunta un nivel arriba para que este proyecto de pruebas encuentre
# el componente led_app_logic (vive en components/, dos niveles arriba).
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../..")
project(test_led_app_logic)