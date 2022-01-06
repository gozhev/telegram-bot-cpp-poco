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
	-lmemcached \
	#

$(cc_binary)
	name = telegram-bot
	srcs = \
		src/main.cc \
		src/TelegramBot.cc \
		#
$;


DESTDIR ?=
PREFIX ?= /usr/local

.PHONY: install
install:
	install --mode=644 -D --target-directory=$(DESTDIR)$(PREFIX)/share/telegram-bot telegram-bot.conf.sample 
	install --mode=644 -D --target-directory=$(DESTDIR)$(PREFIX)/lib/systemd/system telegram-bot.service 
	install --mode=755 -D --target-directory=$(DESTDIR)$(PREFIX)/bin build/telegram-bot
	install --mode=644 -D telegram-bot.conf.sample $(DESTDIR)/etc/telegram-bot/telegram-bot.conf

.PHONY: uninstall
uninstall:
	-rm --force $(DESTDIR)$(PREFIX)/share/telegram-bot/telegram-bot.conf.sample 
	-rm --force --dir $(DESTDIR)$(PREFIX)/share/telegram-bot
	-rm --force $(DESTDIR)$(PREFIX)/lib/systemd/system/telegram-bot.service 
	-rm --force $(DESTDIR)$(PREFIX)/bin/telegram-bot

.PHONY: purge
purge: uninstall
	-rm --force $(DESTDIR)/etc/telegram-bot/telegram-bot.conf
	-rm --force --dir $(DESTDIR)/etc/telegram-bot
