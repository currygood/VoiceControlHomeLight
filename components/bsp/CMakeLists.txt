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
    REQUIRES driver micro-opus esp_wifi nvs_flash lwip esp_http_server wifi_provisioning bt protocomm spiffs esp_partition mbedtls json
)

 
component_compile_options(-ffast-math -O3 -Wno-error=format=-Wno-format)
add_compile_options(-fdiagnostics-color=always)
add_compile_options(-Wno-missing-braces)