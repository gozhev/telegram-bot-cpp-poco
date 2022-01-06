ifndef _CC_BINARY_MK
_CC_BINARY_MK :=

$(call _Include,block)

$(block)
name = cc_binary

local_vars = \
	name \
	srcs \

inherit_vars = \
	cxx \
	cxxflags \
	cppflags \
	ldflags \
	ldlibs \

pre-before=

define before=
endef

define pre-after=
_BUILD_PREFIX := $(call get,build_dir)
_TARGET := $(_BUILD_PREFIX)$(name)
_OBJS := $(srcs:%=$(_BUILD_PREFIX)%.o)
endef

define after=
$(_TARGET): $(_OBJS) | $(_BUILD_PREFIX)
	$(cxx) -o $$@ $(ldflags) $$^ $(ldlibs)

$(_OBJS): $(_BUILD_PREFIX)%.cc.o: %.cc | $(_BUILD_PREFIX)
	$(cxx) -o $$@ $(cppflags) $(cxxflags) -c $$^

$(_BUILD_PREFIX):
	mkdir -p $$@

.PHONY: clean/$(name)
clean/$(name):
	rm -f $(_OBJS) |:
	rm -f $(_TARGET) |:
	rmdir -p $(_BUILD_PREFIX) |:

all: $(_TARGET)
clean: clean/$(name)

endef
$;

endif
