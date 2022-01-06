#include "TelegramBot.h"

#include <regex>
#include <sstream>
#include <fstream>

#include <Poco/TextEncoding.h>
#include <Poco/TextIterator.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Util/PropertyFileConfiguration.h>

namespace pj = ::Poco::JSON;
namespace pn = ::Poco::Net;
namespace pdy = ::Poco::Dynamic;
namespace pu = ::Poco::Util;
namespace pd = ::Poco::Data;
namespace pd_k = ::Poco::Data::Keywords;

TelegramBot::TelegramBot(Error& error)
{
	try {
		auto conf = ::Poco::AutoPtr<pu::PropertyFileConfiguration>{
			new pu::PropertyFileConfiguration{"telegram-bot.conf"}};
		api_token_ = conf->getString("api.token");
		db_host_ = conf->getString("db.host");
		db_port_ = conf->getString("db.port");
		db_database_ = conf->getString("db.database");
		db_user_ = conf->getString("db.user");
		db_password_ = conf->getString("db.password");
	} catch (Poco::NotFoundException const& e) {
		error = Error{true};
		return;
	}

	base_path_ = GenerateBasePath(api_token_);

	pn::HTTPSStreamFactory::registerFactory();
	pn::initializeSSL();

	cert_handler_ = new pn::AcceptCertificateHandler(false);
	context_ = new pn::Context(pn::Context::CLIENT_USE, "");
	pn::SSLManager::instance().initializeClient(0, cert_handler_, context_);

	const auto uri = ::Poco::URI{API_URL};
	api_session_ = ::std::make_unique<pn::HTTPSClientSession>(uri.getHost(), uri.getPort(), context_);
	api_session_->setKeepAlive(true);

	pd::MySQL::Connector::registerConnector();
	::std::stringstream connstr_ss {};
	connstr_ss <<
		"host=" << db_host_ << ";" <<
		"port=" << db_port_ << ";" <<
		"db=" << db_database_ << ";" <<
		"user=" << db_user_ << ";" <<
		"password=" << db_password_ << ";" <<
		"compress=true;" <<
		"auto-reconnect=true";
	db_session_ = ::std::make_unique<pd::Session>("MySQL", connstr_ss.str());

	*db_session_ << "CREATE TABLE IF NOT EXISTS Users (UserId BIGINT PRIMARY KEY)", pd_k::now;
	*db_session_ << "CREATE TABLE IF NOT EXISTS Dates (Date DATE PRIMARY KEY)", pd_k::now;
	*db_session_ << "CREATE TABLE IF NOT EXISTS Attendances ("
		"Date DATE REFERENCES Dates(Date), "
		"UserId BIGINT REFERENCES Users(UserId), "
		"PRIMARY KEY (Date, UserId))", pd_k::now;
	*db_session_ << "CREATE TABLE IF NOT EXISTS Invites ("
		"Invite VARCHAR(64) PRIMARY KEY, "
		"Initiator BIGINT REFERENCES Users(UserId))", pd_k::now;
}

void TelegramBot::UpdateDataBase()
{
	for (auto& [date, users] : date_cache_) {
		auto db_date = date.To<pd::Date>();
		*db_session_ << "INSERT INTO Dates VALUES(?) ON DUPLICATE KEY UPDATE Date=Date",
			pd_k::bind(db_date),
			pd_k::now;
		for (auto iuser = users.begin(); iuser != users.end();) {
			auto& [user_id, remove] = *iuser;
			if (remove) {
				*db_session_ << "DELETE FROM Attendances WHERE Date=? AND UserId=?",
					pd_k::bind(db_date),
					pd_k::bind(user_id),
					pd_k::now;
				iuser = users.erase(iuser);
			} else {
				*db_session_ << "INSERT INTO Users VALUES(?) ON DUPLICATE KEY UPDATE UserId=UserId",
					pd_k::bind(user_id),
					pd_k::now;
				*db_session_ << "INSERT INTO Attendances VALUES(?, ?) ON DUPLICATE KEY UPDATE Date=Date",
					pd_k::bind(db_date),
					pd_k::bind(user_id),
					pd_k::now;
				++iuser;
			}
		}
	}
}

void TelegramBot::ReadDataBase(Date const& first_date, Date const& last_date)
{
	auto db_first = first_date.To<pd::Date>();
	auto db_last = last_date.To<pd::Date>();
	pd::Statement select(*db_session_);
	select << "SELECT * FROM Attendances WHERE ? <= Date AND Date <= ?",
		pd_k::bind(db_first),
		pd_k::bind(db_last),
		pd_k::now;
	pd::RecordSet rs(select);
	date_cache_.clear();
	for (auto& row : rs) {
		auto db_date = row.get(0).extract<pd::Date>();
		auto date = Date::From(db_date);
		ChatId user_id {};
		row.get(1).convert(user_id);
		date_cache_[date].insert({user_id, false});
		::std::cout << "row" << date.To<::std::string>() << ::std::endl;
	}
}

template<> pd::Date TelegramBot::Date::To() const
{
	return {year, month, day};
}

TelegramBot::Date TelegramBot::Date::From(pd::Date const& pd)
{
	return {pd.year(), pd.month(), pd.day()};
}

template<> ::std::string TelegramBot::Date::To() const
{
	::std::stringstream result {};
	result <<
		::std::to_string(year) << "." <<
		::std::to_string(month) << "." <<
		::std::to_string(day);
	return result.str();
}

TelegramBot::Date TelegramBot::Date::From(::std::string_view s)
{
	::std::regex const re { "([0-9]+).([0-9]+).([0-9]+)" };
	::std::match_results<::std::string_view::const_iterator> match{};
	if (!::std::regex_match(s.cbegin(), s.cend(), match, re)) {
		::std::cerr << "invalid date string: " << s << ::std::endl;
		return {};
	}
	Date d{};
	d.year = ::std::stoi(match[1]);
	d.month = ::std::stoi(match[2]);
	d.day = ::std::stoi(match[3]);
	return d;
}

template<> ::std::tm TelegramBot::Date::To() const
{
	::std::tm tm {};
	tm.tm_mday = day;
	tm.tm_mon = month - 1;
	tm.tm_year = year - 1900;
	return tm;
}

TelegramBot::Date TelegramBot::Date::From(::std::tm const& tm)
{
	Date d{};
	d.day = tm.tm_mday;
	d.month = tm.tm_mon + 1;
	d.year = tm.tm_year + 1900;
	return d;
}

::std::string TelegramBot::CallbackData::Serialize(Keyboard const& kb)
{
	::std::stringstream result {};
	result <<
		::std::to_string(kb.first_date.first.year) << "." <<
		::std::to_string(kb.first_date.first.month) << "." <<
		::std::to_string(kb.first_date.first.day) << "," <<
		::std::to_string(static_cast<int>(kb.first_date.second)) << "," <<
		::std::to_string(static_cast<int>(kb.mode)) << "," <<
		::std::to_string(static_cast<int>(kb.n_cols));
	return result.str();
}

::std::string TelegramBot::CallbackData::Serialize(::std::string_view kb, Key const& key)
{
	::std::stringstream result {};
	result << kb << "," <<
		::std::to_string(static_cast<int>(key.type)) << ",";
	if (key.type == Key::Type::DAY) {
		result <<
			::std::to_string(key.data.date.year) << "." <<
			::std::to_string(key.data.date.month) << "." <<
			::std::to_string(key.data.date.day);
	}
	return result.str();
}

bool TelegramBot::CallbackData::Parse(::std::string_view s)
{
	::std::regex const re{
		"([0-9]+).([0-9]+).([0-9]+)," // date
			"([0-9]+)," // gap
			"([\\-+]?[0-9]+)," // mode
			"([0-9]+)," // n_cols
			"([0-9]+)," // key.type,
			"(.*)" // key.data,
	};
	::std::match_results<::std::string_view::const_iterator> match{};

	if (!::std::regex_match(s.cbegin(), s.cend(), match, re)) {
		::std::cerr << "invalid callback data: " << s << ::std::endl;
		return false;
	}

	kb.first_date.first.year = ::std::stoi(match[1]);
	kb.first_date.first.month = ::std::stoi(match[2]);
	kb.first_date.first.day = ::std::stoi(match[3]);
	kb.first_date.second = static_cast<bool>(::std::stoi(match[4]));
	kb.mode = static_cast<Keyboard::Mode>(::std::stoi(match[5]));
	kb.n_cols = ::std::stoi(match[6]);

	key.type = static_cast<Key::Type>(::std::stoi(match[7]));
	auto& key_data = match[8];
	if (key.type == Key::Type::DAY) {
		::std::regex const re { "([0-9]+).([0-9]+).([0-9]+)" };
		::std::match_results<::std::string_view::const_iterator> match{};
		if (!::std::regex_match(key_data.first, key_data.second, match, re)) {
			::std::cerr << "invalid callback key data: " << s << ::std::endl;
			return false;
		}
		key.data.date.year = ::std::stoi(match[1]);
		key.data.date.month = ::std::stoi(match[2]);
		key.data.date.day = ::std::stoi(match[3]);
	}

	return true;
}

::std::string TelegramBot::UnderlineUtf8String(::std::string const& s)
{
	auto underlined = ::std::string{};
	static constexpr int UTF8_MAX_BYTES = 4;
	::std::array<uint8_t, UTF8_MAX_BYTES> buf{};
	::Poco::UTF8Encoding enc{};
	auto ich = ::Poco::TextIterator{s, enc};
	auto iend = ::Poco::TextIterator{s};
	for (; ich != iend; ++ich) {
		int n_bytes = enc.convert(*ich, buf.data(), UTF8_MAX_BYTES);
		underlined.append(reinterpret_cast<char*>(buf.data()), n_bytes);
		underlined.append("̲");
	}
	return underlined;
}

void TelegramBot::StoreSelection(ChatId user_id)
{
	auto& ud = user_data_[user_id];
	for (auto const& [date, flag] : ud.selection) {
		date_cache_[date][user_id] = flag;
		::std::cout << "=> " << date.To<::std::string>() << " " << date_cache_.size() << ::std::endl;
	}
	ud.selection.clear();
}

void TelegramBot::LoadSelection(ChatId user_id, Date const& first, Date const& last)
{
	auto& ud = user_data_[user_id];
	auto it = date_cache_.lower_bound(first);
	auto iend = date_cache_.upper_bound(last);
	for (; it != iend; ++it) {
		auto const& [date, users] = *it;
		auto iuser = users.find(user_id);
		if (iuser != users.end()) {
			ud.selection.insert({date, iuser->second});
		}
	}
}

void TelegramBot::DiscardSelection(ChatId user_id)
{
	auto& ud = user_data_[user_id];
	ud.selection.clear();
}

bool TelegramBot::ProcessCallbackQuery(pdy::Var const& cq)
{
	auto query = cq.extract<pj::Object::Ptr>();
	auto cq_id = query->getValue<CallbackQueryId>("id");
	auto from = query->getObject("from");

	auto user_id = from->getValue<ChatId>("id");
	//auto username = from->getValue<::std::string>("username");
	auto first_name = from->getValue<::std::string>("first_name");
	//auto last_name = from->getValue<::std::string>("last_name");
	::std::cout << first_name << " " << ": query" << ::std::endl;

	auto message = query->getObject("message");
	auto message_id = message->getValue<MessageId>("message_id");
	auto data_str = query->getValue<::std::string>("data");

	auto& ud = user_data_[user_id];

	CallbackData data {};
	bool data_ok = data.Parse(data_str);

	{
		pj::Object::Ptr jreq{new Poco::JSON::Object};
		jreq->set("callback_query_id", cq_id);
		jreq->set("cache_time", 0);
		jreq->set("show_alert", true);
		if (data_ok) {
			if (data.kb.GetMode() == Keyboard::Mode::VIEW &&
					data.key.type == Key::Type::DAY) {
				::std::size_t n_users = 0;
				if (auto idate = date_cache_.find(data.key.data.date); idate != date_cache_.end()) {
					for (auto const& [user_id, remove] : idate->second) {
						if (!remove) {
							++n_users;
						}
					}
				}
				if (!n_users) {
					jreq->set("text", "Присутствий нет.");
				} else {
					auto text = ::std::string("В этот день будут:\n\n");
					auto idate = date_cache_.find(data.key.data.date);
					for (auto const& [user_id, flag] : idate->second) {
						if (flag) {
							continue;
						}
						auto user = GetUserCaching(user_id);
						auto suser = ::std::string{};
						if (user.first_name.size()) {
							suser.append(user.first_name);
						}
						if (user.last_name.size()) {
							if (suser.size()) {
								suser.append(" ");
							}
							suser.append(user.last_name);
						}
						if (!suser.size()) {
							suser.append("id");
							suser.append(::std::to_string(user_id));
						}
						text.append(suser);
						text.append("\n");
					}
					jreq->set("text", text);
				}
			}
		}
		Send("answerCallbackQuery", jreq);
		auto jresp = Receive();
		//pj::Stringifier::condense(jresp, ::std::cout);
		//::std::cout << ::std::endl;
	}

	if (!data_ok) {
		return false;
	}

	if (data.key.type == Key::Type::CLOSE) {
		pj::Object::Ptr jreq{new Poco::JSON::Object};
		pj::Object::Ptr markup{new Poco::JSON::Object};
		jreq->set("chat_id", user_id);
		jreq->set("message_id", message_id);
		jreq->set("text", "Каледнарь присутствий обновлен.");
		//jreq->set("text", "Каледнарь присутствий оставлен без изменений.");
		//jreq->set("text", "В каледнарь присутствий добавлены дни:\n\n Отменены дни:\n\n");
		jreq->set("reply_markup", markup);
		Send("editMessageText", jreq);
		auto jresp = Receive();
		//pj::Stringifier::condense(jresp, ::std::cout);
		//::std::cout << ::std::endl;
	}
	else {
		switch (data.key.type) {
		case Key::Type::PREV_M:
			data.kb.MoveMonth(-1);
			ReadDataBase(data.kb.FirstDate(), data.kb.LastDate());
			if (data.kb.GetMode() == Keyboard::Mode::EDIT) {
				LoadSelection(user_id, data.kb.FirstDate(), data.kb.LastDate());
			}
			break;
		case Key::Type::PREV_W:
			data.kb.MoveWeek(-1);
			ReadDataBase(data.kb.FirstDate(), data.kb.LastDate());
			if (data.kb.GetMode() == Keyboard::Mode::EDIT) {
				LoadSelection(user_id, data.kb.FirstDate(), data.kb.LastDate());
			}
			break;
		case Key::Type::NEXT_W:
			data.kb.MoveWeek(+1);
			ReadDataBase(data.kb.FirstDate(), data.kb.LastDate());
			if (data.kb.GetMode() == Keyboard::Mode::EDIT) {
				LoadSelection(user_id, data.kb.FirstDate(), data.kb.LastDate());
			}
			break;
		case Key::Type::NEXT_M:
			data.kb.MoveMonth(+1);
			ReadDataBase(data.kb.FirstDate(), data.kb.LastDate());
			if (data.kb.GetMode() == Keyboard::Mode::EDIT) {
				LoadSelection(user_id, data.kb.FirstDate(), data.kb.LastDate());
			}
			break;
		case Key::Type::TODAY:
			data.kb.SetCenter(Date::From(Today()));
			ReadDataBase(data.kb.FirstDate(), data.kb.LastDate());
			if (data.kb.GetMode() == Keyboard::Mode::EDIT) {
				LoadSelection(user_id, data.kb.FirstDate(), data.kb.LastDate());
			}
			break;
		case Key::Type::EDIT:
			data.kb.SetMode(Keyboard::Mode::EDIT);
			LoadSelection(user_id, data.kb.FirstDate(), data.kb.LastDate());
			break;
		case Key::Type::CANCEL:
			data.kb.SetMode(Keyboard::Mode::VIEW);
			DiscardSelection(user_id);
			break;
		case Key::Type::SAVE:
			data.kb.SetMode(Keyboard::Mode::VIEW);
			StoreSelection(user_id);
			UpdateDataBase();
			break;
		case Key::Type::DAY:
			if (data.kb.GetMode() == Keyboard::Mode::EDIT) {
				auto const& date = data.key.data.date;
				auto idate = ud.selection.find(date);
				if (idate != ud.selection.end()) {
					idate->second = !idate->second;
				} else {
					ud.selection.insert({date, false});
				}
			}
			break;
		}
		auto jkb = GenerateKeyboard(data.kb, user_id);
		pj::Object::Ptr jmarkup{new Poco::JSON::Object};
		jmarkup->set("inline_keyboard", jkb);
		pj::Object::Ptr jreq{new Poco::JSON::Object};
		jreq->set("chat_id", user_id);
		jreq->set("message_id", message_id);
		jreq->set("reply_markup", jmarkup);
		Send("editMessageReplyMarkup", jreq);
		auto jresp = Receive();
		//pj::Stringifier::condense(jresp, ::std::cout);
		//::std::cout << ::std::endl;
	}

	return true;
}

bool TelegramBot::ProcessMessage(pdy::Var const& message_dv)
{
	auto message = message_dv.extract<pj::Object::Ptr>();
	auto from = message->getObject("from");
	auto user_id = from->getValue<::std::size_t>("id");
	//auto username = from->getValue<::std::string>("username");
	auto first_name = from->getValue<::std::string>("first_name");
	//auto last_name = from->getValue<::std::string>("last_name");
	//auto text = message->getValue<::std::string>("text");
	::std::cout << first_name << " " << ": msg" << ::std::endl;

	//if (!users_.count(user_id)) {
	//	::std::cout << "user not authorized to use the bot: "
	//		<< first_name << " " << last_name <<
	//		" (@" << username << ", " << user_id << ")" << ::std::endl;
	//	return true;
	//}

	{
		Keyboard kb {Date::From(Today())};
		ReadDataBase(kb.FirstDate(), kb.LastDate());
		auto jkb = GenerateKeyboard(kb, user_id);
		pj::Object::Ptr jmsg{new Poco::JSON::Object};
		pj::Object::Ptr jmarkup{new Poco::JSON::Object};
		jmarkup->set("inline_keyboard", jkb);
		jmsg->set("reply_markup", jmarkup);
		jmsg->set("chat_id", user_id);
		jmsg->set("text", "Календарь присутствий");
		Send("sendMessage", jmsg);
		Receive();
	}
	return true;
}

auto TelegramBot::RecacheUser(ChatId user_id) -> decltype(user_cache_)::iterator
{
	User user{};
	pj::Object::Ptr jreq{new Poco::JSON::Object};
	jreq->set("chat_id", user_id);
	Send("getChat", jreq);
	auto resp_dv = Receive();
	auto jresp = resp_dv.extract<pj::Object::Ptr>();
	auto jresult = jresp->getObject("result");
	if (auto jfirst_name = jresult->get("first_name"); !jfirst_name.isEmpty()) {
		user.first_name = jfirst_name.extract<::std::string>();
	}
	if (auto jlast_name = jresult->get("last_name"); !jlast_name.isEmpty()) {
		user.last_name = jlast_name.extract<::std::string>();
	}
	if (auto jusername = jresult->get("username"); !jusername.isEmpty()) {
		user.username = jusername.extract<::std::string>();
	}
	return user_cache_.insert_or_assign(user_id, ::std::move(user)).first;
}

TelegramBot::User const& TelegramBot::GetUserCaching(ChatId user_id)
{
	auto iuser = user_cache_.find(user_id);
	if (iuser == user_cache_.end()) {
		iuser = RecacheUser(user_id);
	}
	return iuser->second;
}

bool TelegramBot::ProcessUpdate(pj::Object::Ptr update)
{
	//pj::Stringifier::condense(update, ::std::cout);
	//::std::cout << ::std::endl;
	auto update_id = update->getValue<::std::size_t>("update_id");
	//::std::cout << update_id << ::std::endl;
	if (update_id > last_update_id_) {
		last_update_id_ = update_id;
	}
	auto cq = update->get("callback_query");
	if (!cq.isEmpty()) {
		return ProcessCallbackQuery(cq);
	}
	auto msg = update->get("message");
	if (!msg.isEmpty()) {
		return ProcessMessage(msg);
	}
	return true;
}

bool TelegramBot::HandleUpdates()
{
	pj::Object::Ptr jreq{new Poco::JSON::Object};
	jreq->set("offset", last_update_id_ + 1);
	jreq->set("timeout", 2);
	Send("getUpdates", jreq);
	auto jresp = Receive();

	pj::Stringifier::stringify(jresp, ::std::cout, 1, 4);
	::std::cout << ::std::endl;

	auto root = jresp.extract<pj::Object::Ptr>();
	auto result = root->getArray("result");
	for (::std::size_t i = 0; i < result->size(); ++i) {
		ProcessUpdate(result->getObject(i));
	}
	return true;
}

void TelegramBot::Send(::std::string_view method, pdy::Var const& json)
{
	::std::stringstream json_stm{};
	pj::Stringifier::condense(json, json_stm);
	pn::HTTPRequest request(
			pn::HTTPRequest::HTTP_POST,
			GenerateMethodPath(base_path_, method),
			Poco::Net::HTTPMessage::HTTP_1_1);
	request.setContentType("application/json; charset=utf-8");
	request.setContentLength(json_stm.tellp());
	auto& request_stm = api_session_->sendRequest(request);
	request_stm << json_stm.rdbuf();
}

pdy::Var TelegramBot::Receive()
{
	pn::HTTPResponse resp{};
	auto& resp_stm = api_session_->receiveResponse(resp);
	auto resp_dv = pj::Parser{}.parse(resp_stm);
	return resp_dv;
}

pdy::Var TelegramBot::GenerateKeyboard(Keyboard const& kb, ChatId user_id)
{
	using Array = pj::Array;
	using Object = pj::Object;
	using ArrayPtr = pj::Array::Ptr;
	using ObjectPtr = pj::Object::Ptr;
	auto& ud = user_data_[user_id];
	auto ks = CallbackData::Serialize(kb);
	auto grid = kb.Grid();
	auto today = Date::From(Today());
	ArrayPtr jkb{new Array};

	{
		ArrayPtr jrow{new Array};
		jkb->add(jrow);
		{
			ObjectPtr jbutton{new Object};
			jrow->add(jbutton);
			auto const& [first_date, first_is_gap] = grid.front();
			auto const& [last_date, last_is_gap] = grid.back();
			auto smonth = ::std::string{};
			if (first_is_gap) {
				smonth.append(MONTH_NAMES[last_date.month - 1]);
				smonth.append(" ");
				smonth.append(::std::to_string(last_date.year));
			} else if (last_is_gap || first_date.month == last_date.month) {
				smonth.append(MONTH_NAMES[first_date.month - 1]);
				smonth.append(" ");
				smonth.append(::std::to_string(first_date.year));
			} else {
				smonth.append(MONTH_NAMES[first_date.month - 1]);
				smonth.append(" ");
				smonth.append(::std::to_string(first_date.year));
				smonth.append(" — ");
				smonth.append(MONTH_NAMES[last_date.month - 1]);
				smonth.append(" ");
				smonth.append(::std::to_string(last_date.year));
			}
			jbutton->set("text", smonth);
			jbutton->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::MONTH}));
		}
	}

	for (int row = 0; row < DAYS_PER_WEEK; ++row) {
		ArrayPtr jrow{new Array};
		jkb->add(jrow);
		for (int col = 0; col < (1 + kb.n_cols); ++col) {
			ObjectPtr jbutton{new Object};
			jrow->add(jbutton);
			if (col == 0) {
				jbutton->set("text", DAY_NAMES[row]);
				jbutton->set("callback_data",
						CallbackData::Serialize(ks, {Key::Type::EMPTY}));
			} else {
				auto const& [date, is_gap] = grid[row + ((col - 1) * DAYS_PER_WEEK)];
				if (is_gap) {
					jbutton->set("text", " ");
					jbutton->set("callback_data",
							CallbackData::Serialize(ks, {Key::Type::EMPTY}));
				} else {
					auto sday = ::std::to_string(date.day);
					if (sday.size() == 1) {
						sday = ::std::string{" "} + sday;
					}
					if (date == today) {
						sday = UnderlineUtf8String(sday);
					}
					::std::size_t n_users = 0;
					if (auto idate = date_cache_.find(date); idate != date_cache_.end()) {
						for (auto const& [uid, remove] : idate->second) {
							if (kb.mode == Keyboard::Mode::EDIT && user_id == uid) {
								continue;
							}
							if (remove) {
								continue;
							}
							++n_users;
						}
					}
					auto text = ::std::string{};
					if (kb.mode == Keyboard::Mode::EDIT) {
						auto idate = ud.selection.find(date);
						if (idate != ud.selection.end() && !idate->second) {
							text.append("✅ ");
						} else if (n_users) {
							text.append(EMOJI_NUMBERS[n_users]);
							text.append(" ");
						} else {
							text.append("  ·   ");
						}
					} else {
						if (n_users) {
							text.append(EMOJI_NUMBERS[n_users]);
							text.append(" ");
						} else {
							text.append("      ");
						}
					}
					text.append(sday);
					jbutton->set("text", text);
					jbutton->set("callback_data",
							CallbackData::Serialize(ks, {Key::Type::DAY, date}));
				}
			}
		}
	}

	{
		ArrayPtr jrow{new Array};
		jkb->add(jrow);
		{
			ObjectPtr jb{new Object};
			jrow->add(jb);
			jb->set("text", "<<");
			jb->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::PREV_M}));
		} {
			ObjectPtr jb{new Object};
			jrow->add(jb);
			jb->set("text", "<");
			jb->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::PREV_W}));
		} {
			ObjectPtr jb{new Object};
			jrow->add(jb);
			jb->set("text", "•");
			jb->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::TODAY}));
		} {
			ObjectPtr jb{new Object};
			jrow->add(jb);
			jb->set("text", ">");
			jb->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::NEXT_W}));
		} {
			ObjectPtr jb{new Object};
			jrow->add(jb);
			jb->set("text", ">>");
			jb->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::NEXT_M}));
		}
	} {
		ArrayPtr jrow{new Array};
		ObjectPtr jleft{new Object};
		ObjectPtr jright{new Object};
		jkb->add(jrow);
		jrow->add(jleft);
		jrow->add(jright);
		if (kb.mode == Keyboard::Mode::VIEW) {
			jleft->set("text", "добавить");
			jleft->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::EDIT}));
			jright->set("text", "закрыть");
			jright->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::CLOSE}));
		} else if (kb.mode == Keyboard::Mode::EDIT) {
			jleft->set("text", "отмена");
			jleft->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::CANCEL}));
			jright->set("text", "сохранить");
			jright->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::SAVE}));
		}
	}
	return jkb;
}

TelegramBot::Date TelegramBot::Keyboard::LastDate() const
{
	auto tm = first_date.first.To<::std::tm>();
	tm.tm_mday += (DAYS_PER_WEEK * n_cols) - 1;
	::std::mktime(&tm);
	return Date::From(tm);
}

void TelegramBot::Keyboard::SetCenter(Date const& d)
{
	first_date = {d, false};
	MoveWeek(-(n_cols - 1) / 2);
}

void TelegramBot::Keyboard::MoveWeek(int shift)
{
	bool back = false;
	if (shift < 0) {
		back = true;
		shift = -shift;
	}
	for (; shift; --shift) {
		Advance(back);
	}
}

void TelegramBot::Keyboard::MoveMonth(int shift)
{
	MoveWeek(shift * 4);
}

TelegramBot::Keyboard::Keyboard(Date const& date)
{
	SetCenter(date);
}

void TelegramBot::Keyboard::Advance(bool back)
{
	auto tm = first_date.first.To<::std::tm>();
	::std::mktime(&tm);
	tm.tm_mday -= (tm.tm_wday + DAYS_PER_WEEK - 1) % DAYS_PER_WEEK;
	::std::mktime(&tm);
	auto cur = tm;
	if (back) {
		tm.tm_mday -= DAYS_PER_WEEK;
	} else {
		tm.tm_mday += DAYS_PER_WEEK;
	}
	::std::mktime(&tm);
	if (first_date.second) {
		first_date.second = false;
		if (back) {
			tm = cur;
		}
	} else if (tm.tm_mon != cur.tm_mon) {
		first_date.second = true;
		if (!back) {
			tm = cur;
		}
	}
	first_date.first = Date::From(tm);
}

::std::vector<::std::pair<TelegramBot::Date, bool>> TelegramBot::Keyboard::Grid() const
{
	::std::vector<::std::pair<Date, bool>> grid(DAYS_PER_WEEK * n_cols);
	if (!grid.size()) {
		return grid;
	}
	int skip = 0;
	auto tm = first_date.first.To<::std::tm>();
	::std::mktime(&tm);
	tm.tm_mday += DAYS_PER_WEEK -
		(tm.tm_wday + DAYS_PER_WEEK - 1) % DAYS_PER_WEEK;
	::std::mktime(&tm);
	tm.tm_mday -= DAYS_PER_WEEK;
	int mon = tm.tm_mon;
	::std::mktime(&tm);
	if (mon != tm.tm_mon && first_date.second) {
		tm.tm_mon += 1;
		tm.tm_mday = 1;
		::std::mktime(&tm);
		skip = (tm.tm_wday + DAYS_PER_WEEK - 2) % DAYS_PER_WEEK + 1;
	}
	for (int i = 0;;) {
		grid[i] = {Date::From(tm), skip};
		if (++i >= grid.size()) {
			break;
		}
		if (skip) {
			--skip;
		} else {
			tm.tm_mday += 1;
			mon = tm.tm_mon;
			::std::mktime(&tm);
			if (mon != tm.tm_mon) {
				skip = 7;
			}
		}
	}
	return grid;
}


// vim: set ts=4 sw=4 noet :
