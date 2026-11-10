// SPDX-License-Identifier: GPL-3.0-or-later
// ModKitNX — читаемые имена функций sdk для экрана и деление пробелов sdk на «онлайн-службы» и остальные.
// Чистые функции без borealis и платформы; хост-тест — tests/host/symbol_names_tests.cpp.
#pragma once

#include <cctype>
#include <string>
#include <vector>

namespace mk::app {

// Простой деманглер Itanium ТОЛЬКО для показа: квалифицированное имя без параметров.
//   _ZN2nn7friends21StartFriendInvitationERKNS_7account3UidE…  ->  nn::friends::StartFriendInvitation
//   _Z3foov                                                    ->  foo
// Всё, что сложнее вложенного имени из простых частей (St, подстановки S_, шаблоны I…E, конструкторы C1/D1,
// операторы), и любое нестандартное или кривое имя — возвращается как есть. Полное имя .dynsym показывается
// отдельно («Подробности»), здесь ничего не теряется.
inline std::string readableSymbol(const std::string& sym) {
    if (sym.size() < 3 || sym.compare(0, 2, "_Z") != 0) return sym;
    size_t p = 2;
    const bool nested = sym[p] == 'N';
    if (nested) {
        ++p;
        while (p < sym.size() && (sym[p] == 'r' || sym[p] == 'V' || sym[p] == 'K' || sym[p] == 'R' || sym[p] == 'O')) ++p;
    }
    std::vector<std::string> parts;
    while (p < sym.size() && std::isdigit(static_cast<unsigned char>(sym[p]))) {
        if (sym[p] == '0') return sym;   // длина с ведущим нулём — не Itanium
        size_t len = 0;
        while (p < sym.size() && std::isdigit(static_cast<unsigned char>(sym[p]))) {
            len = len * 10 + static_cast<size_t>(sym[p] - '0');
            if (len > sym.size()) return sym;
            ++p;
        }
        if (len == 0 || p + len > sym.size()) return sym;
        std::string id = sym.substr(p, len);
        for (char c : id)
            if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return sym;
        parts.push_back(std::move(id));
        p += len;
        if (!nested) break;   // _Z<имя><параметры>: одно имя
    }
    if (parts.empty()) return sym;
    if (nested) {
        if (p >= sym.size() || sym[p] != 'E') return sym;   // внутри вложенного имени — не простая часть
    }
    std::string out;
    for (const auto& s : parts) out += (out.empty() ? "" : "::") + s;
    return out;
}

// Функции онлайн-служб Nintendo: игра вызывает их только когда работает в сети (друзья, аккаунт в сети, облачные
// сохранения, магазин…). Если такой функции нет в старой sdk, игра обычно запускается и работает без онлайна, поэтому
// пробел показывается нейтрально, а не жёлтой тревогой. Список — одно место; почему каждое пространство здесь:
//   nn::friends  — список друзей, приглашения, присутствие в сети (сервер друзей Nintendo);
//   nn::account  — только сетевая часть: NetworkServiceAccount… и NintendoAccount… (вход в сетевой аккаунт, токены);
//                  остальной nn::account (выбор пользователя, Uid, профиль) нужен игре при запуске — не онлайн;
//   nn::olsc     — облачные сохранения (Nintendo Switch Online);
//   nn::npns     — push-уведомления сервера Nintendo;
//   nn::nex      — библиотека игровых серверов Nintendo (онлайн-матчи, рейтинги);
//   nn::pia::inet — интернет-часть сетевой библиотеки pia; остальной pia — и локальная игра (LDN), не онлайн;
//   nn::ec       — покупки в eShop (дополнения, расходуемые предметы);
//   nn::news     — новости Nintendo.
// Не включены (вызываются и без сети, в т. ч. при запуске): nn::nifm (проверка подключения), nn::bcat (кэш доставки
// монтируется при запуске), nn::prepo (отчёты копятся локально), nn::ldn (локальная игра), nn::socket, nn::ssl.
// Возвращает, к какой службе относится функция (ключ строки «fixes/svc_<служба>»), или пустую строку — не онлайн.
inline std::string onlineServiceOf(const std::string& readable) {
    auto starts = [&readable](const char* p) { return readable.rfind(p, 0) == 0; };
    static const struct {
        const char* prefix;
        const char* service;
    } kSpaces[] = {{"nn::friends::", "friends"},     {"nn::olsc::", "olsc"},          {"nn::npns::", "npns"},
                   {"nn::nex::", "online_play"},    {"nn::pia::inet::", "online_play"}, {"nn::ec::", "ec"},
                   {"nn::news::", "news"}};
    for (const auto& s : kSpaces)
        if (starts(s.prefix)) return s.service;
    if (starts("nn::account::") && (readable.find("NetworkServiceAccount") != std::string::npos ||
                                    readable.find("NintendoAccount") != std::string::npos))
        return "account";
    return {};
}

// По имени из .dynsym; нераспознанное имя — не онлайн (осторожно: остаётся жёлтым).
inline std::string onlineServiceOfSymbol(const std::string& sym) {
    const std::string r = readableSymbol(sym);
    return r == sym ? std::string() : onlineServiceOf(r);
}

}  // namespace mk::app
