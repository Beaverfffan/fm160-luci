# SPDX-License-Identifier: GPL-2.0-or-later
#
# luci-app-fm160 - LuCI (JavaScript) front end for fm160d.
#
# This package deliberately includes $(INCLUDE_DIR)/package.mk instead of the
# LuCI feed's luci.mk: luci.mk is reached with a relative include
# ("include ../../luci.mk") which only resolves for packages sitting two levels
# inside the luci feed.  A third-party package dropped into <tree>/package/
# would resolve that to <tree>/luci.mk and fail.  package.mk keeps the package
# self-contained.
#
# The one thing given up by that choice is luci.mk's automatic .po -> .lmo
# translation packaging, so the block at the bottom of this file re-implements
# it.  It is a re-implementation, not a copy, and the four things it has to get
# right are the four things luci.mk hardcodes -- all of them silent when wrong:
#
#   * the catalog directory is /usr/lib/lua/luci/i18n (LUCI_LIBRARYDIR, i.e.
#     /usr/lib/lua/luci + /i18n).  Not /usr/lib/luci/i18n.  dispatcher.uc
#     passes this path to load_catalog() and the controller hardcodes it in
#     admin/index.uc, so a file installed anywhere else is never found.
#   * the file name is <basename>.<language>.lmo, where <basename> is the
#     package name with the "luci-app-" prefix stripped (fm160), and
#     <language> is the *alias*, not the po directory name: po/zh_Hans is
#     installed as fm160.zh-cn.lmo.  load_catalog() globs "*.zh-cn.lmo", so a
#     wrong alias means no match and no error.
#   * the translation ships as its own hidden package, not as part of the app.
#   * that package's uci-defaults must create the luci.languages entry, because
#     dispatcher.uc decides an 'auto' language request by looking for the
#     *existence* of uci.languages.<lang>.

include $(TOPDIR)/rules.mk

PKG_NAME:=luci-app-fm160
PKG_VERSION:=0.1.0
PKG_RELEASE:=1
PKG_LICENSE:=GPL-2.0-or-later
PKG_MAINTAINER:=Beaverfffan
PKGARCH:=all

# po2lmo is built by luci-base's HostBuild and lands in $(STAGING_DIR_HOSTPKG)/bin.
PKG_BUILD_DEPENDS:=luci-base/host

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
 signal and cell information, band/cell locking with the carrier-aggregation
 view, and an AT console.  Dialing, SMS and GNSS pages are added by the
 following milestones.
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

# ---------------------------------------------------------------------------
# Translations
# ---------------------------------------------------------------------------
# Where LuCI actually looks.  dispatcher.uc:110 and controller/admin/index.uc:126
# both pass '/usr/lib/lua/luci/i18n' to load_catalog().
FM160_LUCI_LIBDIR:=/usr/lib/lua/luci

# One entry per shipped translation, "<po directory>:<lmo suffix>".  The po
# directory name is the locale in OpenWrt's spelling (zh_Hans); the .lmo suffix
# is LuCI's alias for it (zh-cn), which is what load_catalog() globs for.  Add a
# language by adding its po/ directory and one entry here -- in that order, and
# never one without the other.
#
# zh_Hans is the only translation that ships.  An earlier revision of this list
# also carried "zh_Hant:zh-tw", which was an intention and never a deliverable:
# there is no po/zh_Hant, so that entry asked the build for a translation package
# with nothing in it.  The two checks further down turn the mismatch into a hard
# error instead of an empty package, which is why it could not survive here --
# but an error that only appears once make reaches this directory is a poor way
# to find out, so tools/i18n/check.py asserts the same invariant from the
# repository alone, before anything is built.
FM160_TRANSLATIONS:=zh_Hans:zh-cn

# The label recorded in luci.languages.  Only the *existence* of that option is
# ever read (dispatcher.uc uses it to resolve 'auto' Accept-Language), never the
# value, but keep it the string the base translation uses so anything that ever
# does display it agrees.
FM160_LANG_TITLE.zh_Hans:=简体中文 (Simplified Chinese)

FM160_lang=$(word 1,$(subst :, ,$(1)))
FM160_alias=$(word 2,$(subst :, ,$(1)))

FM160_LS_LANGS:=$(foreach t,$(FM160_TRANSLATIONS),$(call FM160_lang,$(t)))
FM160_PO_LANGS:=$(sort $(filter-out templates,$(notdir $(wildcard ./po/*))))

# Both directions, because each one fails silently on its own: a listed locale
# with no po/ builds an empty package, and a po/ directory that is not listed
# sits in the tree looking maintained while never reaching a device.
$(if $(filter-out $(FM160_PO_LANGS),$(FM160_LS_LANGS)),\
  $(error luci-app-fm160: FM160_TRANSLATIONS lists $(filter-out $(FM160_PO_LANGS),$(FM160_LS_LANGS)) but po/<locale> does not exist))
$(if $(filter-out $(FM160_LS_LANGS),$(FM160_PO_LANGS)),\
  $(error luci-app-fm160: po/$(filter-out $(FM160_LS_LANGS),$(FM160_PO_LANGS)) exists but is not in FM160_TRANSLATIONS, so it would never be packaged))

# $(1) = lmo suffix / package suffix (the alias)
# $(2) = po directory name (the locale)
# $(3) = human label for the language
define BuildFm160Translation
  define Package/luci-i18n-fm160-$(1)
    SECTION:=luci
    CATEGORY:=LuCI
    SUBMENU:=3. Applications
    TITLE:=luci-app-fm160 - $(1) translation
    HIDDEN:=1
    DEPENDS:=luci-app-fm160
    PKGARCH:=all
  endef

  define Package/luci-i18n-fm160-$(1)/description
   $(3) translation for the FM160 LuCI application.
  endef

  define Package/luci-i18n-fm160-$(1)/install
	$$(INSTALL_DIR) $$(1)/etc/uci-defaults
	echo "uci set luci.languages.$(subst -,_,$(1))='$(3)'; uci commit luci" \
		> $$(1)/etc/uci-defaults/luci-i18n-fm160-$(1)
	$$(INSTALL_DIR) $$(1)$(FM160_LUCI_LIBDIR)/i18n
	$(foreach po,$(wildcard ./po/$(2)/*.po), \
		po2lmo $(po) \
			$$(1)$(FM160_LUCI_LIBDIR)/i18n/$(basename $(notdir $(po))).$(1).lmo;)
  endef

  $$(eval $$(call BuildPackage,luci-i18n-fm160-$(1)))
endef

$(foreach t,$(FM160_TRANSLATIONS),\
  $(eval $(call BuildFm160Translation,\
    $(call FM160_alias,$(t)),\
    $(call FM160_lang,$(t)),\
    $(or $(FM160_LANG_TITLE.$(call FM160_lang,$(t))),$(call FM160_lang,$(t))))))
