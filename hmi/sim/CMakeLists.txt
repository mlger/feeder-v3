# Host builds of the HMI application against simulated feeders:
#  - feeder_sim: headless scripted test that also writes screenshots
#  - feeder_demo: the same UI as WebAssembly for the project web site
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

file(STRINGS ${HMI}/../VERSION APP_VERSION LIMIT_COUNT 1)
add_compile_definitions(APP_VERSION="${APP_VERSION}")

set(APP_SRC
    sim_world.c
    ${HMI}/lib/feeder_app/src/app.c
    ${HMI}/lib/feeder_app/src/ui.c
    ${HMI}/lib/feeder_app/src/ui_schedule.c
    ${HMI}/lib/feeder_app/src/ui_network.c
    ${HMI}/lib/feeder_app/src/ui_format.c
    ${HMI}/lib/feeder_app/src/ui_calibrate.c
    ${HMI}/lib/feeder_app/src/ui_feed.c
    ${CORE}/src/core.c
    ${CORE}/src/node.c
    ${XML_GEN})
set(APP_INC ${HMI}/lib/feeder_app/include ${CORE}/include)

if(EMSCRIPTEN)
    # Browser demo: emcmake cmake -S hmi/sim -B build/web -DLVGL_DIR=... -DCMAKE_BUILD_TYPE=Release
    # Produces feeder_demo.js and feeder_demo.wasm for web/demo/.
    add_executable(feeder_demo web_main.c ${APP_SRC})
    target_include_directories(feeder_demo PRIVATE ${APP_INC})
    target_link_libraries(feeder_demo PRIVATE lvgl)
    target_link_options(feeder_demo PRIVATE
        -sALLOW_MEMORY_GROWTH=1
        -sEXPORTED_FUNCTIONS=_main,_web_init,_web_frame,_web_pixels,_web_pointer)
else()
    add_executable(feeder_sim sim_main.c ${APP_SRC})
    target_include_directories(feeder_sim PRIVATE ${APP_INC})
    target_compile_options(feeder_sim PRIVATE -Wall -Wextra)
    target_link_libraries(feeder_sim PRIVATE lvgl m)
endif()
