SENSOR_DAEMON_VERSION = 1.0
SENSOR_DAEMON_SITE = $(BR2_EXTERNAL_project_base_PATH)/package/sensor_daemon
SENSOR_DAEMON_SITE_METHOD = local

define SENSOR_DAEMON_BUILD_CMDS
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -Wextra \
		-o $(@D)/sensor_daemon $(@D)/sensor_daemon.c
endef

define SENSOR_DAEMON_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/sensor_daemon \
		$(TARGET_DIR)/usr/bin/sensor_daemon
endef

$(eval $(generic-package))
