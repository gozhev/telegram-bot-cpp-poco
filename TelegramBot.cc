#include "TelegramBot.h"

#include <fstream>
#include <regex>
#include <sstream>

#include <Poco/Base64Encoder.h>
#include <Poco/Data/MySQL/MySQLException.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Exception.h>
#include <Poco/Random.h>
#include <Poco/TextEncoding.h>
#include <Poco/TextIterator.h>
#include <Poco/Util/PropertyFileConfiguration.h>

namespace p = ::Poco;
namespace p_json = ::Poco::JSON;
namespace p_net = ::Poco::Net;
namespace p_dyn = ::Poco::Dynamic;
namespace p_util = ::Poco::Util;
namespace p_data = ::Poco::Data;
namespace p_kw = ::Poco::Data::Keywords;

TelegramBot::TelegramBot(Error& error) noexcept
try {
	auto conf = p_util::AbstractConfiguration::Ptr{
		new p_util::PropertyFileConfiguration{"telegram-bot.conf"}};
	api_token_ = conf->getString("api.token");
	db_host_ = conf->getString("db.host");
	db_port_ = conf->getString("db.port");
	db_database_ = conf->getString("db.database");
	db_user_ = conf->getString("db.user");
	db_password_ = conf->getString("db.password");

	base_path_ = GenerateBasePath(api_token_);

	p_net::HTTPSStreamFactory::registerFactory();
	p_net::initializeSSL();

	context_ = p_net::Context::Ptr{new p_net::Context(p_net::Context::CLIENT_USE, "")};

	cert_handler_ =
		p_net::SSLManager::InvalidCertificateHandlerPtr{new p_net::AcceptCertificateHandler(false)};
	p_net::SSLManager::instance().initializeClient(0, cert_handler_, context_);

	const auto uri = p::URI{API_URL};
	api_session_ = ::std::make_unique<p_net::HTTPSClientSession>(uri.getHost(), uri.getPort(), context_);
	api_session_->setKeepAlive(true);

	p_data::MySQL::Connector::registerConnector();
	::std::stringstream conn_sstm {};
	conn_sstm <<
		"host=" << db_host_ << ";" <<
		"port=" << db_port_ << ";" <<
		"db=" << db_database_ << ";" <<
		"user=" << db_user_ << ";" <<
		"password=" << db_password_ << ";" <<
		"compress=true;" <<
		"auto-reconnect=true";
	db_session_ = ::std::make_unique<p_data::Session>("MySQL", conn_sstm.str());
	*db_session_ << "CREATE TABLE IF NOT EXISTS RegisteredUsers ("
		"UserId BIGINT PRIMARY KEY);", p_kw::now;
	*db_session_ << "CREATE TABLE IF NOT EXISTS Attendances ("
		"Date DATE, "
		"UserId BIGINT, "
		"PRIMARY KEY (Date, UserId))", p_kw::now;
	*db_session_ << "CREATE TABLE IF NOT EXISTS Invites ("
		"Invite VARCHAR(64) PRIMARY KEY, "
		"InvitedBy BIGINT)", p_kw::now;
}
catch (p::Exception const& e) {
	error = Error{true};
	::std::cerr << e.displayText() << ::std::endl;
}
catch (::std::exception const& e) {
	error = Error{true};
	::std::cerr << e.what() << ::std::endl;
}
catch (...) {
	error = Error{true};
	::std::cerr << "unknown non-stantard exception" << ::std::endl;
}

bool TelegramBot::PopInvite(::std::string const& invite_token, ChatId& user_id) const
{
	p_data::Statement select(*db_session_);
	select << "SELECT * FROM Invites WHERE Invite=?",
		p_kw::bind(invite_token),
		p_kw::now;
	p_data::RecordSet rs(select);
	if (!rs.extractedRowCount()) {
		return false;
	}
	*db_session_ << "DELETE FROM Invites WHERE Invite=?",
		p_kw::bind(invite_token),
		p_kw::now;
	rs.row(0).get(1).convert(user_id);
	return true;
}

void TelegramBot::PushInvite(::std::string const& invite_token, ChatId user_id) const
{
	*db_session_ << "INSERT INTO Invites VALUES(?, ?)",
		p_kw::bind(invite_token),
		p_kw::bind(user_id),
		p_kw::now;
}

void TelegramBot::RegisterUser(ChatId user_id) const
{
	*db_session_ << "INSERT INTO RegisteredUsers VALUES(?) ON DUPLICATE KEY UPDATE UserId=UserId",
		p_kw::bind(user_id),
		p_kw::now;
}

void TelegramBot::UpdateDataBase()
{
	for (auto& [date, users] : date_cache_) {
		auto db_date = date.To<p_data::Date>();
		for (auto iuser = users.begin(); iuser != users.end();) {
			auto& [user_id, remove] = *iuser;
			if (remove) {
				*db_session_ << "DELETE FROM Attendances WHERE Date=? AND UserId=?",
					p_kw::bind(db_date),
					p_kw::bind(user_id),
					p_kw::now;
				iuser = users.erase(iuser);
			} else {
				*db_session_ << "INSERT INTO Attendances VALUES(?, ?) ON DUPLICATE KEY UPDATE Date=Date",
					p_kw::bind(db_date),
					p_kw::bind(user_id),
					p_kw::now;
				++iuser;
			}
		}
	}
}

void TelegramBot::ReadDataBase(Date const& first_date, Date const& last_date)
{
	auto db_first = first_date.To<p_data::Date>();
	auto db_last = last_date.To<p_data::Date>();
	p_data::Statement select(*db_session_);
	select << "SELECT * FROM Attendances WHERE ?<=Date AND Date<=?",
		p_kw::bind(db_first),
		p_kw::bind(db_last),
		p_kw::now;
	p_data::RecordSet rs(select);
	date_cache_.clear();
	for (auto& row : rs) {
		auto db_date = row.get(0).extract<p_data::Date>();
		auto date = Date::From(db_date);
		ChatId user_id {};
		row.get(1).convert(user_id);
		date_cache_[date].insert({user_id, false});
	}
}

void TelegramBot::StoreSelection(ChatId user_id)
{
	auto& ud = user_data_[user_id];
	for (auto const& [date, flag] : ud.selection) {
		date_cache_[date][user_id] = flag;
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

auto TelegramBot::RecacheUser(ChatId user_id) -> decltype(user_cache_)::iterator
{
	User user{};
	auto req_jo = p_json::Object::Ptr{new Poco::JSON::Object};
	req_jo->set("chat_id", user_id);
	auto res_dv = SendMessage("getChat", req_jo);
	auto res_jo = res_dv.extract<p_json::Object::Ptr>();
	if (auto dv = res_jo->get("first_name"); !dv.isEmpty()) {
		user.first_name = dv.extract<::std::string>();
	}
	if (auto dv = res_jo->get("last_name"); !dv.isEmpty()) {
		user.last_name = dv.extract<::std::string>();
	}
	if (auto dv = res_jo->get("username"); !dv.isEmpty()) {
		user.username = dv.extract<::std::string>();
	}
	user.user_id = user_id;
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

void TelegramBot::ProcessCallbackQuery(p_dyn::Var const& cq)
{
	auto cq_jo = cq.extract<p_json::Object::Ptr>();
	auto cq_id = cq_jo->getValue<CallbackQueryId>("id");
	auto from_jo = cq_jo->getObject("from");
	auto user_id = from_jo->getValue<ChatId>("id");
	auto first_name = from_jo->getValue<::std::string>("first_name");
	auto msg_jo = cq_jo->getObject("message");
	auto msg_id = msg_jo->getValue<MessageId>("message_id");

	//auto username = from->getValue<::std::string>("username");
	//auto last_name = from->getValue<::std::string>("last_name");

	auto& ud = user_data_[user_id];

	CallbackData data {};
	auto data_str = cq_jo->getValue<::std::string>("data");
	if (!data.Parse(data_str)) {
		auto req_jo = p_json::Object::Ptr{new p_json::Object};
		req_jo->set("callback_query_id", cq_id);
		req_jo->set("cache_time", 0);
		req_jo->set("text", "Некорректные или устаревшие данные.");
		SendMessage("answerCallbackQuery", req_jo);
		return;
	}

	if (data.kb.GetMode() == Keyboard::Mode::VIEW && data.key.type == Key::Type::DAY) {
		auto req_jo = p_json::Object::Ptr{new p_json::Object};
		req_jo->set("callback_query_id", cq_id);
		req_jo->set("cache_time", 0);
		req_jo->set("show_alert", true);

		::std::size_t n_users = 0;
		if (auto idate = date_cache_.find(data.key.data.date); idate != date_cache_.end()) {
			for (auto const& [user_id, remove] : idate->second) {
				if (!remove) {
					++n_users;
				}
			}
		}
		if (!n_users) {
			req_jo->set("text", "Присутствий нет.");
		} else {
			auto text = ::std::string("В этот день будут:\n\n");
			auto idate = date_cache_.find(data.key.data.date);
			for (auto const& [user_id, flag] : idate->second) {
				if (flag) {
					continue;
				}
				auto user = GetUserCaching(user_id);
				auto user_str = ::std::string{};
				if (user.first_name.size()) {
					user_str.append(user.first_name);
				}
				if (user.last_name.size()) {
					if (user_str.size()) {
						user_str.append(" ");
					}
					user_str.append(user.last_name);
				}
				if (!user_str.size()) {
					user_str.append("id");
					user_str.append(::std::to_string(user_id));
				}
				text.append(user_str);
				text.append("\n");
			}
			req_jo->set("text", text);
		}
		SendMessage("answerCallbackQuery", req_jo);
		return;
	}

	{
		auto req_jo = p_json::Object::Ptr{new p_json::Object};
		req_jo->set("callback_query_id", cq_id);
		req_jo->set("cache_time", 0);
		SendMessage("answerCallbackQuery", req_jo);
	}

	if (data.key.type == Key::Type::CLOSE) {
		auto req_jo = p_json::Object::Ptr{new Poco::JSON::Object};
		auto mk_jo = p_json::Object::Ptr{new Poco::JSON::Object};
		req_jo->set("chat_id", user_id);
		req_jo->set("message_id", msg_id);
		req_jo->set("text", "Каледнарь присутствий обновлен.");
		//jreq->set("text", "Каледнарь присутствий оставлен без изменений.");
		//jreq->set("text", "В каледнарь присутствий добавлены дни:\n\n Отменены дни:\n\n");
		req_jo->set("reply_markup", mk_jo); // sic! empty markup
		SendMessage("editMessageText", req_jo);
		return;
	}

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
	case Key::Type::MONTH:
	case Key::Type::EMPTY:
	case Key::Type::CLOSE:
		break;
	}

	{
		auto req_jo = p_json::Object::Ptr{new Poco::JSON::Object};
		req_jo->set("chat_id", user_id);
		req_jo->set("message_id", msg_id);
		auto kb_dv = GenerateKeyboard(data.kb, user_id);
		auto mk_jo = p_json::Object::Ptr{new Poco::JSON::Object};
		mk_jo->set("inline_keyboard", kb_dv);
		req_jo->set("reply_markup", mk_jo);
		SendMessage("editMessageReplyMarkup", req_jo);
	}
}

void TelegramBot::ProcessMessage(p_dyn::Var const& msg_dv)
{
	auto msg_jo = msg_dv.extract<p_json::Object::Ptr>();
	auto from_jo = msg_jo->getObject("from");
	auto user_id = from_jo->getValue<::std::size_t>("id");

	auto registered_user = IsUserRegistered(user_id);

	//auto first_name = from->getValue<::std::string>("first_name");
	//auto last_name = from->getValue<::std::string>("last_name");
	//auto username = from->getValue<::std::string>("username");
	//auto text = message->getValue<::std::string>("text");

	auto text_dv = msg_jo->get("text");
	if (text_dv.isEmpty()) {
		if (registered_user) {
			auto req_jo = p_json::Object::Ptr{new p_json::Object};
			req_jo->set("chat_id", user_id);
			req_jo->set("text", "Неправильный формат команды.");
			SendMessage("sendMessage", req_jo);
			req_jo->set("text", GetListOfCommads());
			SendMessage("sendMessage", req_jo);
		}
		return;
	}
	auto text = text_dv.extract<::std::string>();

	::std::regex const re{"/([A-Za-z0-9_-]+)(?: (.*))?"};
	::std::smatch match{};
	if (!::std::regex_match(text, match, re)) {
		if (registered_user) {
			auto req_jo = p_json::Object::Ptr{new p_json::Object};
			req_jo->set("chat_id", user_id);
			req_jo->set("text", "Неправильный формат команды.");
			SendMessage("sendMessage", req_jo);
			req_jo->set("text", GetListOfCommads());
			SendMessage("sendMessage", req_jo);
		}
		return;
	}
	auto command = match[1].str();

	if (command == "start") {
		if (!match[2].length()) {
			if (registered_user) {
				auto req_jo = p_json::Object::Ptr{new p_json::Object};
				req_jo->set("chat_id", user_id);
				req_jo->set("text", GetListOfCommads());
				SendMessage("sendMessage", req_jo);
			}
			return;
		} else {
			auto payload = match[2].str();
			ChatId invited_by{};
			if (!PopInvite(payload, invited_by)) {
				if (registered_user) {
					auto req_jo = p_json::Object::Ptr{new p_json::Object};
					req_jo->set("chat_id", user_id);
					req_jo->set("text", "Ключ не найден.");
					SendMessage("sendMessage", req_jo);
				}
				return;
			}
			RegisterUser(user_id);
			auto req_jo = p_json::Object::Ptr{new p_json::Object};
			req_jo->set("chat_id", user_id);
			req_jo->set("text", "Регистрация прошла успешно.");
			SendMessage("sendMessage", req_jo);
			req_jo->set("text", GetListOfCommads());
			SendMessage("sendMessage", req_jo);
			return;
		}
	}

	if (!registered_user) {
		return;
	}

	if (command == "calendar") {
		Keyboard kb {Date::From(Today())};
		ReadDataBase(kb.FirstDate(), kb.LastDate());
		auto kb_dv = GenerateKeyboard(kb, user_id);
		auto mk_jo = p_json::Object::Ptr{new Poco::JSON::Object};
		mk_jo->set("inline_keyboard", kb_dv);
		auto req_jo = p_json::Object::Ptr{new p_json::Object};
		req_jo->set("chat_id", user_id);
		req_jo->set("reply_markup", mk_jo);
		req_jo->set("text", "Календарь присутствий");
		SendMessage("sendMessage", req_jo);
	} else if (command == "invite") {
		auto invite_token = GenerateInviteToken();
		PushInvite(invite_token, user_id);
		auto invite_link = ::std::string{"https://t.me/HomeGozhevRuBot?start="};
		invite_link.append(invite_token);
		auto text = ::std::string{
			"Передайте эту ссылку пользователю, которого хотите добавить:\n"};
		text.append(invite_link);
		auto req_jo = p_json::Object::Ptr{new p_json::Object};
		req_jo->set("chat_id", user_id);
		req_jo->set("text", text);
		SendMessage("sendMessage", req_jo);
	} else if (command == "users") {
		HandleCommandUsers(user_id);
	} else if (command == "sensor") {
		HandleCommandSensor(user_id);
	} else {
		auto req_jo = p_json::Object::Ptr{new p_json::Object};
		req_jo->set("chat_id", user_id);
		req_jo->set("text", "Неизвестная команда.");
		SendMessage("sendMessage", req_jo);
		req_jo->set("text", GetListOfCommads());
		SendMessage("sendMessage", req_jo);
	}
}

void TelegramBot::HandleCommandSensor(ChatId user_id) {
	auto res_jo = p_json::Object::Ptr{};
	auto text = ::std::string{};
	try {
		static const p::URI uri("http://info.bvo.home.gozhev.ru");
		auto path = uri.getPathAndQuery();
		if (path.empty()) {
			path = "/";
		}
		p_net::HTTPClientSession sess(uri.getHost(), uri.getPort());
		p_net::HTTPRequest req(p_net::HTTPRequest::HTTP_GET, path, p_net::HTTPMessage::HTTP_1_1);
		p_net::HTTPResponse res{};
		sess.sendRequest(req);
		auto& res_stm = sess.receiveResponse(res);
		auto res_dv = p_json::Parser{}.parse(res_stm);
		res_jo = res_dv.extract<p_json::Object::Ptr>();
	}
	catch (p::Exception const& e) {
		::std::cerr << "error: handle sensor: " << e.displayText() << ::std::endl;
	}
	if (res_jo.isNull()) {
		text = "Невозможно получить данные";
	} else {
		::std::ostringstream sstm{};
		if (auto dv = res_jo->get("temperature"); !dv.isEmpty()) {
			double value{};
			dv.convert(value);
			sstm << "Температура:  " << value << " ℃\n";
		}
		if (auto dv = res_jo->get("humidity"); !dv.isEmpty()) {
			double value{};
			dv.convert(value);
			sstm << "Влажность:  " << value << " %\n";
		}
		if (auto dv = res_jo->get("pressure"); !dv.isEmpty()) {
			double value{};
			dv.convert(value);
			sstm << "Давление:  " << value << " hPa\n";
		}
		if (!sstm.tellp()) {
			text = "Пустые данные";
		} else {
			if (auto dv = res_jo->get("last_seen"); !dv.isEmpty()) {
				sstm << "\nВремя измерения:  " << dv.extract<::std::string>() << "\n";
			}
			auto str = sstm.str();
			text.append("Показания внутри дома:\n\n");
			text.append(str);
		}
	}

	auto req_jo = p_json::Object::Ptr{new p_json::Object};
	req_jo->set("chat_id", user_id);
	req_jo->set("text", text);
	SendMessage("sendMessage", req_jo);
}

void TelegramBot::HandleCommandUsers(ChatId user_id) {
	auto users = GetRegisteredUsers();
	::std::ostringstream sstm{};
	sstm << "Зарегистрированные пользователи:\n";
	for (auto const& user : users) {
		sstm << "\n";
		if (!user.first_name.empty()) {
			sstm << user.first_name << " ";
		}
		if (!user.last_name.empty()) {
			sstm << user.last_name << " ";
		}
		sstm << "(" << user.user_id << ")";
	}
	auto req_jo = p_json::Object::Ptr{new p_json::Object};
	req_jo->set("chat_id", user_id);
	req_jo->set("text", sstm.str());
	SendMessage("sendMessage", req_jo);
}

::std::vector<TelegramBot::User> TelegramBot::GetRegisteredUsers()
{
	auto select = p_data::Statement{*db_session_};
	select << "SELECT UserId FROM RegisteredUsers",
		p_kw::now;
	auto rs = p_data::RecordSet{select};
	::std::vector<User> users{};
	for (auto& row : rs) {
		ChatId user_id{};
		row.get(0).convert(user_id);
		users.push_back(GetUserCaching(user_id));
	}
	return users;
}

bool TelegramBot::IsUserRegistered(ChatId user_id) const
{
	auto select = p_data::Statement{*db_session_};
	select << "SELECT * FROM RegisteredUsers WHERE UserId=?",
		p_kw::bind(user_id),
		p_kw::now;
	auto rs = p_data::RecordSet{select};
	if (!rs.extractedRowCount()) {
		return false;
	}
	return true;
}

::std::string TelegramBot::GetListOfCommads() const
{
	::std::ostringstream sstm{};
	sstm << "Доступные команды:\n"
		<< "\n" << "/sensor - получить показания датчика"
		<< "\n" << "/calendar - открыть календарь посещений"
		<< "\n" << "/invite - пригласить нового пользователя"
		<< "\n" << "/users - показать зарегистрированных пользователей"
		<< "\n" << "/start - показать доступные команды"
		;
	return sstm.str();
}

::std::string TelegramBot::GenerateInviteToken()
{
	auto prng = p::Random{};
	auto sstm = ::std::stringstream{};
	auto estm = p::Base64Encoder{sstm,
		p::Base64EncodingOptions::BASE64_URL_ENCODING |
		p::Base64EncodingOptions::BASE64_NO_PADDING};
	static constexpr int TOKEN_LENGTH = 32;
	static constexpr int N_BYTES = TOKEN_LENGTH / 4 * 3;
	for (int i = 0; i < N_BYTES; ++i) {
		estm << prng.nextChar();
	}
	estm.close();
	return sstm.str();
}

void TelegramBot::ProcessUpdate(p_json::Object::Ptr update)
{
	if (auto upid = update->getValue<::std::size_t>("update_id"); upid > last_update_id_) {
		last_update_id_ = upid;
	}
	if (auto msg = update->get("message"); !msg.isEmpty()) {
		ProcessMessage(msg);
	}
	// TODO block unregistered users here
	else if (auto cq = update->get("callback_query"); !cq.isEmpty()) {
		ProcessCallbackQuery(cq);
	}
	// TODO handle unknown update
}

void TelegramBot::HandleUpdates(Error& error) noexcept
try {
	auto req_jo = p_json::Object::Ptr{new Poco::JSON::Object};
	req_jo->set("offset", last_update_id_ + 1);
	req_jo->set("timeout", 2);
	auto res_dv = SendMessage("getUpdates", req_jo);
	auto res_ja = res_dv.extract<p_json::Array::Ptr>();
	for (::std::size_t i = 0; i < res_ja->size(); ++i) {
		ProcessUpdate(res_ja->getObject(i));
	}
	OnUpdateSucceed(error);
}
catch (p::Exception const& e) {
	OnUpdateFailed(error);
	::std::cerr << e.displayText() << ::std::endl;
}
catch (::std::exception const& e) {
	OnUpdateFailed(error);
	::std::cerr << e.what() << ::std::endl;
}
catch (...) {
	error = Error{true};
	::std::cerr << "unknown non-stantard exception" << ::std::endl;
}

void TelegramBot::OnUpdateSucceed(Error& error) noexcept {
	(void) error;
	error_seq_count_ = 0;
}

void TelegramBot::OnUpdateFailed(Error& error) noexcept {
	static constexpr int ERROR_SEQ_COUNT_MAX = 5;
	if (++error_seq_count_ > ERROR_SEQ_COUNT_MAX) {
		error = Error{true};
	}
}

p_dyn::Var TelegramBot::SendMessage(::std::string_view method, p_dyn::Var const& req)
{
	Send(method, req);
	auto resp_dv = Receive();

#if 0
	::std::cout << "REQUEST:" << ::std::endl;
	::std::cout << method << " ";
	p_json::Stringifier::stringify(req, ::std::cout);
	::std::cout << ::std::endl;

	::std::cout << "RESPONSE:" << ::std::endl;
	p_json::Stringifier::stringify(resp_dv, ::std::cout, 1, 2);
	::std::cout << ::std::endl;
#endif

	auto resp_jo = resp_dv.extract<p_json::Object::Ptr>();
	if (auto ok = resp_jo->getValue<bool>("ok"); !ok) {
		::std::stringstream sstm{};
		sstm << "bad response: ";
		p_json::Stringifier::condense(resp_dv, sstm);
		throw ::std::runtime_error{sstm.str()};
	}
	return resp_jo->get("result");
}

void TelegramBot::Send(::std::string_view method, p_dyn::Var const& json)
{
	::std::stringstream json_stm{};
	p_json::Stringifier::condense(json, json_stm);
	p_net::HTTPRequest req(
			p_net::HTTPRequest::HTTP_POST,
			GenerateMethodPath(base_path_, method),
			Poco::Net::HTTPMessage::HTTP_1_1);
	req.setContentType("application/json; charset=utf-8");
	req.setContentLength(json_stm.tellp());
	auto& req_stm = api_session_->sendRequest(req);
	req_stm << json_stm.rdbuf();
}

p_dyn::Var TelegramBot::Receive()
{
	p_net::HTTPResponse resp{};
	auto& resp_stm = api_session_->receiveResponse(resp);
	auto resp_dv = p_json::Parser{}.parse(resp_stm);
	return resp_dv;
}

p_dyn::Var TelegramBot::GenerateKeyboard(Keyboard const& kb, ChatId user_id)
{
	using Array = p_json::Array;
	using Object = p_json::Object;

	auto& ud = user_data_[user_id];
	auto ks = CallbackData::Serialize(kb);
	auto grid = kb.GenerateGrid();
	auto today = Date::From(Today());
	auto kb_ja = Array::Ptr{new Array};

	{
		auto row_ja = Array::Ptr{new Array};
		kb_ja->add(row_ja);
		{
			auto bn_jo = Object::Ptr{new Object};
			row_ja->add(bn_jo);
			auto const& [first_date, first_is_gap] = grid.front();
			auto const& [last_date, last_is_gap] = grid.back();
			auto text = ::std::string{};
			if (first_is_gap) {
				text.append(MONTH_NAMES[last_date.month - 1]);
				text.append(" ");
				text.append(::std::to_string(last_date.year));
			} else if (last_is_gap || first_date.month == last_date.month) {
				text.append(MONTH_NAMES[first_date.month - 1]);
				text.append(" ");
				text.append(::std::to_string(first_date.year));
			} else {
				text.append(MONTH_NAMES[first_date.month - 1]);
				text.append(" ");
				text.append(::std::to_string(first_date.year));
				text.append(" — ");
				text.append(MONTH_NAMES[last_date.month - 1]);
				text.append(" ");
				text.append(::std::to_string(last_date.year));
			}
			bn_jo->set("text", text);
			bn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::MONTH}));
		}
	}

	for (int row = 0; row < DAYS_PER_WEEK; ++row) {
		auto row_ja = Array::Ptr{new Array};
		kb_ja->add(row_ja);
		for (int col = 0; col < (1 + kb.n_cols); ++col) {
			auto bn_jo = Object::Ptr{new Object};
			row_ja->add(bn_jo);
			if (col == 0) {
				bn_jo->set("text", DAY_NAMES[row]);
				bn_jo->set("callback_data",
						CallbackData::Serialize(ks, {Key::Type::EMPTY}));
			} else {
				auto const& [date, is_gap] = grid[row + ((col - 1) * DAYS_PER_WEEK)];
				if (is_gap) {
					bn_jo->set("text", " ");
					bn_jo->set("callback_data",
							CallbackData::Serialize(ks, {Key::Type::EMPTY}));
				} else {
					auto day = ::std::to_string(date.day);
					if (day.size() == 1) {
						day = ::std::string{" "} + day;
					}
					if (date == today) {
						day = UnderlineUtf8String(day);
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
					text.append(day);
					bn_jo->set("text", text);
					bn_jo->set("callback_data",
							CallbackData::Serialize(ks, {Key::Type::DAY, date}));
				}
			}
		}
	}

	{
		auto row_ja = Array::Ptr{new Array};
		kb_ja->add(row_ja);
		{
			auto bn_jo = Object::Ptr{new Object};
			row_ja->add(bn_jo);
			bn_jo->set("text", "<<");
			bn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::PREV_M}));
		} {
			auto bn_jo = Object::Ptr{new Object};
			row_ja->add(bn_jo);
			bn_jo->set("text", "<");
			bn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::PREV_W}));
		} {
			auto bn_jo = Object::Ptr{new Object};
			row_ja->add(bn_jo);
			bn_jo->set("text", "•");
			bn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::TODAY}));
		} {
			auto bn_jo = Object::Ptr{new Object};
			row_ja->add(bn_jo);
			bn_jo->set("text", ">");
			bn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::NEXT_W}));
		} {
			auto bn_jo = Object::Ptr{new Object};
			row_ja->add(bn_jo);
			bn_jo->set("text", ">>");
			bn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::NEXT_M}));
		}
	} {
		auto row_ja = Array::Ptr{new Array};
		kb_ja->add(row_ja);
		auto lbn_jo = Object::Ptr{new Object};
		row_ja->add(lbn_jo);
		auto rbn_jo = Object::Ptr{new Object};
		row_ja->add(rbn_jo);
		if (kb.mode == Keyboard::Mode::VIEW) {
			lbn_jo->set("text", "добавить");
			lbn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::EDIT}));
			rbn_jo->set("text", "закрыть");
			rbn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::CLOSE}));
		} else if (kb.mode == Keyboard::Mode::EDIT) {
			lbn_jo->set("text", "отмена");
			lbn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::CANCEL}));
			rbn_jo->set("text", "сохранить");
			rbn_jo->set("callback_data",
					CallbackData::Serialize(ks, {Key::Type::SAVE}));
		}
	}
	return kb_ja;
}

::std::string TelegramBot::UnderlineUtf8String(::std::string const& s)
{
	auto underlined = ::std::string{};
	static constexpr int UTF8_MAX_BYTES = 4;
	::std::array<uint8_t, UTF8_MAX_BYTES> buf{};
	p::UTF8Encoding enc{};
	auto ich = p::TextIterator{s, enc};
	auto iend = p::TextIterator{s};
	for (; ich != iend; ++ich) {
		int n_bytes = enc.convert(*ich, buf.data(), UTF8_MAX_BYTES);
		underlined.append(reinterpret_cast<char*>(buf.data()), n_bytes);
		underlined.append("̲");
	}
	return underlined;
}

TelegramBot::Keyboard::Keyboard(Date const& date)
{
	SetCenter(date);
}

void TelegramBot::Keyboard::SetCenter(Date const& d)
{
	first_date = {d, false};
	ToStartOfWeek();
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
	auto tm = first_date.first.To<::std::tm>();
	tm.tm_mon += shift + (first_date.second ? 1 : 0);
	tm.tm_mday = 1;
	::std::mktime(&tm);
	first_date.first = Date::From(tm);
	ToStartOfWeek();
}

void TelegramBot::Keyboard::ToStartOfWeek()
{
	auto tm = first_date.first.To<::std::tm>();
	::std::mktime(&tm);
	tm.tm_mday -= (tm.tm_wday + DAYS_PER_WEEK - 1) % DAYS_PER_WEEK;
	auto old_mon = tm.tm_mon;
	::std::mktime(&tm);
	first_date.second = (old_mon != tm.tm_mon);
	first_date.first = Date::From(tm);
}

void TelegramBot::Keyboard::Advance(bool back)
{
	auto tm = first_date.first.To<::std::tm>();
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

TelegramBot::Date TelegramBot::Keyboard::LastDate() const
{
	auto tm = first_date.first.To<::std::tm>();
	tm.tm_mday += (DAYS_PER_WEEK * n_cols) - 1;
	::std::mktime(&tm);
	return Date::From(tm);
}

::std::vector<::std::pair<TelegramBot::Date, bool>> TelegramBot::Keyboard::GenerateGrid() const
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
		if (++i >= static_cast<int>(grid.size())) {
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

template<> p_data::Date TelegramBot::Date::To() const
{
	return {year, month, day};
}

TelegramBot::Date TelegramBot::Date::From(p_data::Date const& pd)
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
	::std::regex const re{"([0-9]+).([0-9]+).([0-9]+)"};
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

// vim: set ts=4 sw=4 noet :
