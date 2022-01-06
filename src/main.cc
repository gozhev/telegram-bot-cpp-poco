#include <csignal>

#include "TelegramBot.h"

static volatile ::std::sig_atomic_t g_quit = 0;

static void SignalHandler(int)
{
	g_quit = 1;
}

int main(int, char**)
{
	struct ::sigaction sa {};
	sa.sa_handler = SignalHandler;
	::sigemptyset(&sa.sa_mask);
	::sigaction(SIGINT, &sa, nullptr);

	auto stop_pred = []() noexcept { return g_quit; };
	auto err = TelegramBot::NoError();
	TelegramBot bot{err};
	if (err) {
		return -1;
	}
	bot.Run(stop_pred, err);
	if (err) {
		return -1;
	}
	return 0;
}

// vim: set ts=4 sw=4 noet :
