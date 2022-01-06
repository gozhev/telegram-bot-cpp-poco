include make.mk/make.mk

$.cxxflags += \
	-std=c++17 \
	-O2 \
	#

$.cxxflags += \
	-Wall \
	-Wextra \
	-Wpedantic \
	-Werror \
	#

$.ldlibs += \
	-lPocoFoundation \
	-lPocoNet \
	-lPocoNetSSL \
	-lPocoJSON \
	-lPocoData \
	-lPocoDataMySQL \
	-lPocoUtil \
	#

$(cc_binary)
	name = telegram-bot
	srcs = \
		src/main.cc \
		src/TelegramBot.cc \
		#
$;
