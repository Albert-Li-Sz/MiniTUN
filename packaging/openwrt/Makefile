include $(TOPDIR)/rules.mk

PKG_NAME:=minitun
PKG_VERSION:=0.3.0
PKG_RELEASE:=1

PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE
PKG_MAINTAINER:=MiniTun maintainers
PKG_BUILD_FLAGS:=no-mips16
PKG_BUILD_PARALLEL:=1

CLI11_SOURCE:=minitun-cli11-2.7.0.tar.gz
NLOHMANN_JSON_SOURCE:=minitun-nlohmann-json-3.12.0.tar.gz
SPDLOG_SOURCE:=minitun-spdlog-1.17.0.tar.gz
ASIO_SOURCE:=minitun-asio-1.38.2.tar.bz2

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

# MIPS toolchains keep some std::atomic operations in libatomic. The package
# dependency above covers runtime installation; pass the library explicitly
# so the cross-link step succeeds for both big- and little-endian targets.
ifneq ($(filter mips%,$(ARCH)),)
TARGET_LDFLAGS += -latomic
endif

define Package/minitun/default
  SECTION:=net
  CATEGORY:=Network
  URL:=https://github.com/LMTINSUZHOU/MiniTUN
  DEPENDS:=+libatomic +libopenssl +libpthread +libsqlite3 +libstdcpp
endef

define Package/minitun-client
  $(call Package/minitun/default)
  TITLE:=MiniTun reverse tunnel client
  DEPENDS+=+ca-bundle
  USERID:=minitun:minitun
endef

define Package/minitun-client/description
 MiniTun command-line client and persistent daemon for reverse TCP tunnels.
endef

define Package/minitun-server
  $(call Package/minitun/default)
  TITLE:=MiniTun reverse tunnel server
  USERID:=minitun-server:minitun-server
endef

define Package/minitun-server/description
 MiniTun public TLS server for reverse TCP tunnels.
endef

CMAKE_OPTIONS += \
	-DMINITUN_BUILD_FUZZERS=OFF \
	-DMINITUN_BUILD_PACKAGES=OFF \
	-DMINITUN_BUILD_TESTS=OFF \
	-DMINITUN_ENABLE_LTO=OFF \
	-DMINITUN_PACKAGE_VERSION=$(PKG_VERSION) \
	-DCMAKE_INSTALL_LIBEXECDIR=libexec \
	-DMINITUN_USE_SYSTEM_DEPS=OFF \
	-DFETCHCONTENT_FULLY_DISCONNECTED=ON \
	-DFETCHCONTENT_SOURCE_DIR_CLI11=$(PKG_BUILD_DIR)/third_party/cli11 \
	-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$(PKG_BUILD_DIR)/third_party/nlohmann-json \
	-DFETCHCONTENT_SOURCE_DIR_SPDLOG=$(PKG_BUILD_DIR)/third_party/spdlog \
	-DFETCHCONTENT_SOURCE_DIR_ASIO=$(PKG_BUILD_DIR)/third_party/asio

define Download/cli11
	FILE:=$(CLI11_SOURCE)
	URL:=https://github.com/CLIUtils/CLI11/archive/refs/tags/
	URL_FILE:=v2.7.0.tar.gz
	HASH:=7828464fabc29d361e1ccb2d8721e6e00e73ff5c5fad5135408a8c236508398a
	MIRROR:=0
endef

define Download/nlohmann-json
	FILE:=$(NLOHMANN_JSON_SOURCE)
	URL:=https://github.com/nlohmann/json/archive/refs/tags/
	URL_FILE:=v3.12.0.tar.gz
	HASH:=4b92eb0c06d10683f7447ce9406cb97cd4b453be18d7279320f7b2f025c10187
	MIRROR:=0
endef

define Download/spdlog
	FILE:=$(SPDLOG_SOURCE)
	URL:=https://github.com/gabime/spdlog/archive/refs/tags/
	URL_FILE:=v1.17.0.tar.gz
	HASH:=d8862955c6d74e5846b3f580b1605d2428b11d97a410d86e2fb13e857cd3a744
	MIRROR:=0
endef

define Download/asio
	FILE:=$(ASIO_SOURCE)
	URL:=https://downloads.sourceforge.net/project/asio/asio/1.38.2%20%28Stable%29/
	URL_FILE:=asio-1.38.2.tar.bz2
	HASH:=c04e0e66ac29741faad763a56f3c50196421d4b968009fc237c53314769bf8ad
	MIRROR:=0
endef

$(eval $(call Download,cli11))
$(eval $(call Download,nlohmann-json))
$(eval $(call Download,spdlog))
$(eval $(call Download,asio))

define Build/Prepare
	@if [ -z "$(MINITUN_SOURCE_DIR)" ] || \
		[ ! -f "$(MINITUN_SOURCE_DIR)/CMakeLists.txt" ]; then \
		echo "MINITUN_SOURCE_DIR must point to a MiniTun source checkout" >&2; \
		exit 1; \
	fi
	rm -rf $(PKG_BUILD_DIR)
	$(INSTALL_DIR) $(PKG_BUILD_DIR)
	$(CP) \
		$(MINITUN_SOURCE_DIR)/CMakeLists.txt \
		$(MINITUN_SOURCE_DIR)/LICENSE \
		$(MINITUN_SOURCE_DIR)/README.md \
		$(MINITUN_SOURCE_DIR)/apps \
		$(MINITUN_SOURCE_DIR)/cmake \
		$(MINITUN_SOURCE_DIR)/include \
		$(MINITUN_SOURCE_DIR)/packaging \
		$(MINITUN_SOURCE_DIR)/src \
		$(PKG_BUILD_DIR)/
	$(INSTALL_DIR) \
		$(PKG_BUILD_DIR)/third_party/cli11 \
		$(PKG_BUILD_DIR)/third_party/nlohmann-json \
		$(PKG_BUILD_DIR)/third_party/spdlog \
		$(PKG_BUILD_DIR)/third_party/asio
	$(TAR) -xf $(DL_DIR)/$(CLI11_SOURCE) \
		-C $(PKG_BUILD_DIR)/third_party/cli11 --strip-components=1
	$(TAR) -xf $(DL_DIR)/$(NLOHMANN_JSON_SOURCE) \
		-C $(PKG_BUILD_DIR)/third_party/nlohmann-json --strip-components=1
	$(TAR) -xf $(DL_DIR)/$(SPDLOG_SOURCE) \
		-C $(PKG_BUILD_DIR)/third_party/spdlog --strip-components=1
	$(TAR) -xf $(DL_DIR)/$(ASIO_SOURCE) \
		-C $(PKG_BUILD_DIR)/third_party/asio --strip-components=1
endef

define Package/minitun-client/conffiles
/etc/config/minitun
endef

define Package/minitun-client/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/minitun $(1)/usr/bin/minitun
	$(INSTALL_DIR) $(1)/usr/libexec/minitun
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/libexec/minitun/minitund \
		$(1)/usr/libexec/minitun/minitund
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/minitun.config $(1)/etc/config/minitun
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/minitun.init $(1)/etc/init.d/minitun
	$(INSTALL_DIR) $(1)/etc/minitun
endef

define Package/minitun-server/conffiles
/etc/config/minitun-server
endef

define Package/minitun-server/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/minitun-server \
		$(1)/usr/bin/minitun-server
	$(INSTALL_DIR) $(1)/etc/capabilities
	$(INSTALL_CONF) ./files/minitun-server.capabilities \
		$(1)/etc/capabilities/minitun-server.json
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) ./files/minitun-server.config \
		$(1)/etc/config/minitun-server
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) ./files/minitun-server.init \
		$(1)/etc/init.d/minitun-server
	$(INSTALL_DIR) $(1)/etc/minitun-server
endef

$(eval $(call BuildPackage,minitun-client))
$(eval $(call BuildPackage,minitun-server))
