ifndef _MODULE_MK
_MODULE_MK :=

_MODULE := /
. = $(_MODULE)

_INHERIT_VARS :=

_GetVar = $(if $(2),$(if $($(2)$(1)),$($(2)$(1)),$(call _GetVar,$(1),$(dir $(patsubst %/,%,$(2))))))
get = $(call _GetVar,$(1),$(_MODULE))

endif
