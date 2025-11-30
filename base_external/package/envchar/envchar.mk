ENVCHAR_VERSION = 1.0
ENVCHAR_SITE = $(BR2_EXTERNAL_project_base_PATH)/package/envchar
ENVCHAR_SITE_METHOD = local

define ENVCHAR_BUILD_CMDS
	$(MAKE) -C $(LINUX_DIR) \
		M=$(@D) \
		KERNELDIR=$(LINUX_DIR) \
		ARCH=$(KERNEL_ARCH) \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		modules
endef

define ENVCHAR_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0644 $(@D)/envchar.ko \
		$(TARGET_DIR)/lib/modules/$(LINUX_VERSION_PROBED)/envchar.ko
endef

$(eval $(generic-package))
