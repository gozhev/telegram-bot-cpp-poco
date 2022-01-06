#pragma once

#include <array>
#include <ctime>
#include <iostream>
#include <iterator>
#include <sstream>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Poco/Data/MySQL/Connector.h>
#include <Poco/Data/Session.h>
#include <Poco/Dynamic/Var.h>
#include <Poco/Exception.h>
#include <Poco/JSON/JSON.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/JSON/Stringifier.h>
#include <Poco/Net/AcceptCertificateHandler.h>
#include <Poco/Net/Context.h>
#include <Poco/Net/HTTPMessage.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPSClientSession.h>
#include <Poco/Net/HTTPSStreamFactory.h>
#include <Poco/Net/InvalidCertificateHandler.h>
#include <Poco/Net/SSLManager.h>
#include <Poco/StreamCopier.h>
#include <Poco/URI.h>
#include <Poco/URIStreamOpener.h>

class TelegramBot {
public:
	using Error = bool;
	static Error NoError() noexcept { return Error{false}; }

	explicit TelegramBot(Error& error) noexcept;
	template<typename T, typename = ::std::enable_if_t<noexcept(::std::declval<T>()())>>
		void Run(T stop, Error& error) noexcept;

	TelegramBot(TelegramBot const&) = delete;
	TelegramBot& operator=(TelegramBot const&) = delete;

private:
	static constexpr char const* API_URL = "https://api.telegram.org";
	static constexpr char const* EMOJI_NUMBERS[] = {
		"0️⃣", "1️⃣", "2️⃣", "3️⃣", "4️⃣", "5️⃣", "6️⃣", "7️⃣", "8️⃣", "9️⃣", "🔟"};
	static constexpr char const* DAY_NAMES[] = {"ПН", "ВТ", "СР", "ЧТ", "ПТ", "СБ", "ВС"};
	static constexpr char const* MONTH_NAMES[] = {
			"Январь", "Февраль", "Март", "Апрель", "Май", "Июнь",
			"Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь"};
	static constexpr int DAYS_PER_WEEK = 7;

	using ChatId = ::std::size_t; // 52 bits at most
	using MessageId = ChatId;
	using CallbackQueryId = ::std::string;
	using DateId = ::std::string;

	struct Date {
		int year{};
		int month{};
		int day{};

		struct Hash {
			::std::size_t operator()(Date const& x) const;
		};
		bool operator<(Date const& rhs) const;
		bool operator==(Date const& rhs) const;

		static Date From(::std::string_view s);
		static Date From(::std::tm const& tm);
		static Date From(::Poco::Data::Date const& pd);
		template<typename T> T To() const;
	};

	struct Keyboard {
		::std::pair<Date, bool> first_date{}; // date,is_gap
		enum class Mode {
			VIEW, EDIT
		} mode{Mode::VIEW};
		int n_cols{4};

		Keyboard() = default;
		explicit Keyboard(Date const& date);

		void SetCenter(Date const& date);
		void MoveMonth(int shift);
		void MoveWeek(int shift);
		void ToStartOfWeek();

		Date const& FirstDate() const { return first_date.first; };
		Date LastDate() const;

		Mode GetMode() const { return mode; }
		void SetMode(Mode m) { mode = m; }

		::std::vector<::std::pair<Date, bool>> GenerateGrid() const;

	private:
		void Advance(bool back = false);
	};

	struct Key {
		enum class Type : int {
			EMPTY, CLOSE, EDIT, CANCEL, SAVE,
			NEXT_W, PREV_W, NEXT_M, PREV_M, TODAY, MONTH, DAY
		} type{Type::EMPTY};
		union Data {
			Date date{};
			int index;
		} data{};
	};

	struct CallbackData {
		Keyboard kb{};
		Key key{};

		static ::std::string Serialize(Keyboard const& kb);
		static ::std::string Serialize(::std::string_view kb, Key const& key);
		bool Parse(::std::string_view s);
	};

	struct User {
		ChatId user_id {};
		::std::string first_name{};
		::std::string last_name{};
		::std::string username{};
	};

	struct UserData {
		::std::unordered_map<Date, bool, Date::Hash> selection{}; // date,delete
	};

	::std::string api_token_{};
	::std::string db_host_{};
	::std::string db_port_{};
	::std::string db_user_{};
	::std::string db_password_{};
	::std::string db_database_{};

	::std::string base_path_{};
	::std::size_t last_update_id_{};

	::std::map<Date, ::std::unordered_map<ChatId, bool>> date_cache_{};

	::std::unordered_map<ChatId, UserData> user_data_{};
	::std::unordered_map<ChatId, User> user_cache_{};

	::Poco::Net::Context::Ptr context_{};
	::Poco::Net::SSLManager::InvalidCertificateHandlerPtr cert_handler_{};
	::std::unique_ptr<::Poco::Net::HTTPSClientSession> api_session_{};
	::std::unique_ptr<::Poco::Data::Session> db_session_{};

	bool IsUserRegistered(ChatId user_id) const;
	void RegisterUser(ChatId user_id) const;
	bool PopInvite(::std::string const& invite, ChatId& user_id) const;
	void PushInvite(::std::string const& invite, ChatId user_id) const;
	void UpdateDataBase();
	void ReadDataBase(Date const& first_date, Date const& last_date);
	User const& GetUserCaching(ChatId user_id);
	auto RecacheUser(ChatId user_id) -> decltype(user_cache_)::iterator;
	void DiscardSelection(ChatId user_id);
	void LoadSelection(ChatId user_id, Date const& from, Date const& to);
	void StoreSelection(ChatId user_id);
	::Poco::Dynamic::Var GenerateKeyboard(Keyboard const& kb, ChatId user_id);
	bool ParseCallbackData(::std::string_view data_str, CallbackData& data);
	void ProcessCallbackQuery(::Poco::Dynamic::Var const& callback_query_dv);
	void ProcessMessage(::Poco::Dynamic::Var const& message_dv);
	void ProcessUpdate(::Poco::JSON::Object::Ptr update);
	void Send(::std::string_view method, ::Poco::Dynamic::Var const& json);
	::Poco::Dynamic::Var Receive();
	::Poco::Dynamic::Var SendMessage(::std::string_view method, ::Poco::Dynamic::Var const& req);

	void HandleUpdates(Error& error) noexcept;

	static ::std::string GenerateInviteToken();
	static ::std::string UnderlineUtf8String(::std::string const& s);

	static ::std::tm Today() {
		::std::time_t now = ::std::time(nullptr);
		return *::std::gmtime(&now);
	}

	static ::std::string GenerateBasePath(::std::string_view token) {
		return ::std::string{"/bot"}.append(token);
	}

	static ::std::string GenerateMethodPath(::std::string_view base_url,
			::std::string_view method) {
		return ::std::string{base_url}.append("/").append(method.data());
	}
};

template<> ::std::string TelegramBot::Date::To() const;
template<> ::std::tm TelegramBot::Date::To() const;
template<> ::Poco::Data::Date TelegramBot::Date::To() const;

template<typename T, typename = ::std::enable_if_t<noexcept(::std::declval<T>()())>>
	inline void TelegramBot::Run(T stop, Error& error) noexcept
{
	auto err = Error{false};
	while (!stop()) {
		HandleUpdates(err);
		if (err) {
			error = err;
			break;
		}
	}
	return;
}

inline bool TelegramBot::Date::operator==(Date const& rhs) const
{
	return (year == rhs.year) && (month == rhs.month) && (day == rhs.day);
}

inline bool TelegramBot::Date::operator<(Date const& rhs) const
{
	return (year < rhs.year) ||
		(year == rhs.year && ((month < rhs.month) ||
			(month == rhs.month && day < rhs.day)));
}

inline ::std::size_t TelegramBot::Date::Hash::operator()(Date const& x) const
{
	return ::std::hash<int>()(x.year) ^
		(::std::hash<int>()(x.month) << 1) ^
		(::std::hash<int>()(x.day) >> 1);
}

// vim: set ts=4 sw=4 noet :
