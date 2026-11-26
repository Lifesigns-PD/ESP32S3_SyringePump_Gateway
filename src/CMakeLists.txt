# The src folder is the "main" component of the project; in ESP-IDF the main component automatically
# requires every other component (esp_http_server, esp_wifi, esp_eth, nvs_flash, mdns ...), so no REQUIRES list is needed.
file(GLOB app_sources ${CMAKE_SOURCE_DIR}/src/*.c ${CMAKE_SOURCE_DIR}/src/*.cpp)

idf_component_register(SRCS ${app_sources}
                       INCLUDE_DIRS ".")
