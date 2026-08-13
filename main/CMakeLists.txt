idf_component_register(SRCS "main.c"
                    INCLUDE_DIRS "."
					REQUIRES driver App OTA_Update Audio_Stream bsp middlewares)

spiffs_create_partition_image(storage ../html FLASH_IN_PROJECT)