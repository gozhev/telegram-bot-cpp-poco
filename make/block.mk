ifndef _BLOCK_MK
_BLOCK_MK :=

$(call _Include,utility)

define _PreBefore_block
_BLOCK := block
__MY_VARS := \
	name \
	local_vars \
	inherit_vars \
	pre-before \
	before \
	pre-after \
	after
endef

define _Before_block
$(foreach x,$(__MY_VARS),$(NL)undefine $(x))
endef

define _PreAfter_block
_INHERIT_VARS := $(sort $(_INHERIT_VARS) $(inherit_vars))
endef

define _After_block
define _PreBefore_$(name)
$(foreach x,$(local_vars) $(inherit_vars),$(NL)undefine $(x))
_BLOCK := $(name)
$(value pre-before)
endef
define _Before_$(name)
$(foreach x,$(inherit_vars),$(NL)$(x) = $$(call get,$(x)))
$(value before)
endef
define _PreAfter_$(name)
$(value pre-after)
endef
define _After_$(name)
$(value after)
$(foreach x,$(local_vars) $(inherit_vars),$(NL)undefine $(x))
endef
define $(name)
$$(eval $$(value _PreBefore_$(name)))
$$(eval $$(_Before_$(name)))
endef
$(foreach x,$(__MY_VARS),$(NL)undefine $(x))
endef

define block
$(eval $(value _PreBefore_block))
$(eval $(_Before_block))
endef

define ;
$(eval $(value _PreAfter_$(_BLOCK)))
$(eval $(_After_$(_BLOCK)))
endef

endif
