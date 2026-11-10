# Host build of the HMI application with a headless LVGL display.
# It drives the real UI code against simulated feeders and writes screenshots,
# so the XML screens can be checked without the board.
#
#   cmake -S hmi/sim -B build/sim -DLVGL_DIR=/path/to/lvgl-9.4.0 && cmake --build build/sim
#   build/sim/feeder_sim <output dir>
cmake_minimum_required(VERSION 3.16)
project(feeder_sim C)

set(LVGL_DIR "" CACHE PATH "LVGL 9.4.0 source tree")
if(NOT EXISTS "${LVGL_DIR}/lvgl.h")
    message(FATAL_ERROR "Set -DLVGL_DIR to an LVGL 9.4.0 checkout")
endif()

set(CMAKE_C_STANDARD 11)
set(HMI ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(CORE ${HMI}/../shared/lib/feeder_core)
find_package(Python3 REQUIRED)

file(GLOB_RECURSE LVGL_SRC ${LVGL_DIR}/src/*.c)
add_library(lvgl STATIC ${LVGL_SRC})
target_include_directories(lvgl PUBLIC ${LVGL_DIR} ${HMI}/include)
target_compile_definitions(lvgl PUBLIC LV_CONF_INCLUDE_SIMPLE)

file(GLOB UI_XML ${HMI}/ui/*.xml)
set(XML_GEN ${CMAKE_CURRENT_BINARY_DIR}/ui_xml_gen.c)
add_custom_command(OUTPUT ${XML_GEN}
    COMMAND ${Python3_EXECUTABLE} ${HMI}/tools/gen_ui_xml.py ${HMI}/ui ${XML_GEN}
    DEPENDS ${UI_XML} ${HMI}/tools/gen_ui_xml.py)

add_executable(feeder_sim
    sim_main.c
    ${HMI}/lib/feeder_app/src/app.c
    ${HMI}/lib/feeder_app/src/ui.c
    ${CORE}/src/core.c
    ${XML_GEN})
target_include_directories(feeder_sim PRIVATE ${HMI}/lib/feeder_app/include ${CORE}/include)
target_compile_options(feeder_sim PRIVATE -Wall -Wextra)
target_link_libraries(feeder_sim PRIVATE lvgl m)
