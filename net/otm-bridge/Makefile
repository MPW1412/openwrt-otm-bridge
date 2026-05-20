include $(TOPDIR)/rules.mk

PKG_NAME:=otm-bridge
PKG_VERSION:=0.9
PKG_RELEASE:=1

include $(INCLUDE_DIR)/package.mk

define Package/otm-bridge
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=opentrafficmap.org C-ITS MQTT bridge
  DEPENDS:=+libpcap +libmosquitto-ssl +ca-bundle +iw-full +tcpdump-mini
endef

define Package/otm-bridge/description
 Capture raw 802.11(p) frames from a Linux monitor interface and
 publish them to opentrafficmap.org's MQTT ingest (cits1.opentrafficmap.org),
 mimicking the wire format used by the ESP32-C5 reference receiver.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(MAKE) -C $(PKG_BUILD_DIR) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS) $(TARGET_CPPFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define Package/otm-bridge/conffiles
/etc/config/otm-bridge
endef

define Package/otm-bridge/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/otm-bridge $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/otm-bridge.config $(1)/etc/config/otm-bridge
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/otm-bridge.init $(1)/etc/init.d/otm-bridge
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) ./files/uci-defaults/99-otm-setup $(1)/etc/uci-defaults/99-otm-setup
endef

$(eval $(call BuildPackage,otm-bridge))
