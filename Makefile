TARGET := telegram-bot

SRCS := \
	main.cc \
	TelegramBot.cc \

CXXFLAGS := \
	-std=c++17 \

LDFLAGS := \
	-lPocoFoundation \
	-lPocoNet \
	-lPocoNetSSL \
	-lPocoJSON \
	-lPocoData \
	-lPocoDataMySQL \
	-lPocoUtil \

.PHONY: all
all:
	g++ -o $(TARGET) $(LDFLAGS) $(CXXFLAGS) $(SRCS)
