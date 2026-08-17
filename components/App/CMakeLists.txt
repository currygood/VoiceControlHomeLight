file(GLOB children RELATIVE ${COMPONENT_DIR} *)
set(MID_INCLUDE_DIRS "")

foreach(child ${children})
    if(IS_DIRECTORY ${COMPONENT_DIR}/${child})
        list(APPEND MID_INCLUDE_DIRS ${child})
    endif()
endforeach()
  
file(GLOB_RECURSE SOURCES_C "*.c")
file(GLOB_RECURSE SOURCES_CPP "*.cpp")
list(APPEND SOURCES ${SOURCES_C} ${SOURCES_CPP})
 
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${MID_INCLUDE_DIRS}
    REQUIRES driver Audio_Stream middlewares esp-sr esp_http_client esp_websocket_client json lwip mqtt
)

# 如果需要使用esp-sr的tts再添加以下代码
# target_add_binary_data(${COMPONENT_LIB}
#     "${COMPONENT_DIR}/../../managed_components/espressif__esp-sr/esp-tts/esp_tts_chinese/esp_tts_voice_data_xiaole.dat"
#     BINARY
#     RENAME_TO tts_voice_xiaole)

component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
add_compile_options(-fdiagnostics-color=always)
add_compile_options(-Wno-missing-braces)