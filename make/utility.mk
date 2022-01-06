ifndef _UTILITY_MK
_UTILITY_MK :=

_IncludeAll = $(eval $(foreach x,$(wildcard $(1)*.mk),$(NL)include $(x)))

define NL


endef

endif
