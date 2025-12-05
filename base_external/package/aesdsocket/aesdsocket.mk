AESDSOCKET_VERSION = 1.0
AESDSOCKET_SITE = $(BR2_EXTERNAL_project_base_PATH)/package/aesdsocket
AESDSOCKET_SITE_METHOD = local

define AESDSOCKET_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) $(TARGET_LDFLAGS) \
		-pthread -Wall -Wextra \
		-o $(@D)/aesdsocket $(@D)/aesdsocket.c
endef

define AESDSOCKET_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/aesdsocket \
		$(TARGET_DIR)/usr/bin/aesdsocket
endef

$(eval $(generic-package))
