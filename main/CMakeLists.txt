idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS ".")

set(UI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ui")
if(EXISTS ${UI_DIR}/dist)
    spiffs_create_partition_image(www ${UI_DIR}/dist FLASH_IN_PROJECT)
else()
    message(FATAL_ERROR "${UI_DIR}/dist doesn't exist")
endif()
