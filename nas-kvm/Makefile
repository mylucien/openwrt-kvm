include $(TOPDIR)/rules.mk

PKG_NAME:=nas-kvm
PKG_VERSION:=0.4.0
PKG_RELEASE:=1
PKG_LICENSE:=MIT

include $(INCLUDE_DIR)/package.mk

define Package/nas-kvm
  SECTION:=utils
  CATEGORY:=Utilities
  TITLE:=NAS KVM daemon (HDMI + CH9329 keyboard/mouse)
  DEPENDS:=+kmod-video-uvc +kmod-usb-serial-ch341 +kmod-usb-acm +ustreamer
endef

define Package/nas-kvm/description
  Remote KVM for a NAS behind the ONT: serves a web UI, relays keyboard and
  mouse events to a CH9329 over a serial cable and runs a boot-menu/BIOS key macro.
  Video is served by ustreamer.
endef

define Package/nas-kvm/conffiles
/etc/config/nas-kvm
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
	$(CP) ./src/* $(PKG_BUILD_DIR)/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -Wall -O2 -o $(PKG_BUILD_DIR)/nas-kvmd \
		$(PKG_BUILD_DIR)/kvmd.c $(PKG_BUILD_DIR)/ch9329.c $(PKG_BUILD_DIR)/hid_map.c \
		$(PKG_BUILD_DIR)/sha1b64.c $(TARGET_LDFLAGS)
endef

define Package/nas-kvm/install
	$(INSTALL_DIR) $(1)/usr/sbin $(1)/usr/bin $(1)/etc/init.d $(1)/etc/config $(1)/etc/uci-defaults $(1)/www/kvm
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/nas-kvmd $(1)/usr/sbin/
	$(INSTALL_BIN) ./files/usr/bin/nas-kvm-ctl $(1)/usr/bin/
	$(INSTALL_BIN) ./files/etc/init.d/nas-kvm $(1)/etc/init.d/
	$(INSTALL_CONF) ./files/etc/config/nas-kvm $(1)/etc/config/
	$(INSTALL_BIN) ./files/etc/uci-defaults/90-nas-kvm $(1)/etc/uci-defaults/
	$(INSTALL_DATA) ./files/www/kvm/index.html $(1)/www/kvm/
endef

$(eval $(call BuildPackage,nas-kvm))
