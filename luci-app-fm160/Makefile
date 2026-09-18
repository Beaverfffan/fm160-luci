# SPDX-License-Identifier: GPL-2.0-or-later
#
# luci-app-fm160 - LuCI (JavaScript) front end for fm160d.
#
# This package deliberately includes $(INCLUDE_DIR)/package.mk instead of the
# LuCI feed's luci.mk: luci.mk is reached with a relative include
# ("include ../../luci.mk") which only resolves for packages sitting two levels
# inside the luci feed.  A third-party package dropped into <tree>/package/
# would resolve that to <tree>/luci.mk and fail.  package.mk keeps the package
# self-contained; the only thing given up is automatic .po -> .lmo i18n
# compilation, which this package does not use yet.

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-fm160
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=Beaverfffan
PKGARCH:=all

include $(INCLUDE_DIR)/package.mk

define Package/luci-app-fm160
  SECTION:=luci
  CATEGORY:=LuCI
  SUBMENU:=3. Applications
  TITLE:=LuCI interface for the Fibocom FM160
  DEPENDS:=+luci-base +fm160d
  PKGARCH:=all
endef

define Package/luci-app-fm160/description
 LuCI JavaScript interface for a single Fibocom FM160 module: module status,
 signal and cell information, and an AT console.  Dialing, SMS, band/cell
 locking and GNSS pages are added by the following milestones.
endef

define Build/Prepare
	mkdir -p $(PKG_BUILD_DIR)
endef

define Build/Configure
endef

define Build/Compile
endef

# htdocs/ is plain LuCI JavaScript and root/ holds menu.d + acl.d, so there is
# nothing to compile -- the package is arch-independent.
define Package/luci-app-fm160/install
	$(CP) ./root/* $(1)/
	$(INSTALL_DIR) $(1)/www
	$(CP) ./htdocs/* $(1)/www/
endef

$(eval $(call BuildPackage,luci-app-fm160))
