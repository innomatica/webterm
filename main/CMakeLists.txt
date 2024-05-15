idf_component_register(SRCS "main.c" "wifi_manager.c" "rest_server.c"
                    INCLUDE_DIRS "include")

if(CONFIG_WEBTERM_WEB_DEPLOY_SF)
	set(WEB_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../www/${CONFIG_WEBTERM_WEB_SRC_DIR}")
    if(EXISTS ${WEB_SRC_DIR}/dist)
        spiffs_create_partition_image(www ${WEB_SRC_DIR}/dist FLASH_IN_PROJECT)
    else()
		message(FATAL_ERROR "${WEB_SRC_DIR}/dist doesn't exit. Please run 'npm run build' in ${WEB_SRC_DIR}")
    endif()
endif()
