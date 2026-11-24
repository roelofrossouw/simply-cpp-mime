#include <sc/mime/trace.h>

#include <algorithm>
#include <cctype>

namespace sc::mime {
    namespace {
        std::string lower(std::string_view text) {
            std::string out{text};
            std::ranges::transform(out, out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return out;
        }

        bool space(const char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

        std::string trim(std::string_view text) {
            while (!text.empty() && space(text.front())) text.remove_prefix(1);
            while (!text.empty() && space(text.back())) text.remove_suffix(1);
            return std::string{text};
        }

        // The text with its (nested) comments as one space each, quoted strings kept as they are
        std::string without_comments(const std::string_view text) {
            std::string out;
            int depth = 0;
            bool quoted = false;
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (depth == 0 && quoted) {
                    out += c;
                    if (c == '\\' && i + 1 < text.size()) out += text[++i];
                    else if (c == '"') quoted = false;
                    continue;
                }
                if (depth > 0) {
                    if (c == '\\') ++i;
                    else if (c == '(') ++depth;
                    else if (c == ')' && --depth == 0) out += ' ';
                    continue;
                }
                if (c == '(') ++depth;
                else {
                    if (c == '"') quoted = true;
                    out += c;
                }
            }
            return out;
        }

        // Parts split at sep outside quoted strings
        std::vector<std::string> split(const std::string_view text, const char sep) {
            std::vector<std::string> parts{""};
            bool quoted = false;
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (quoted && c == '\\' && i + 1 < text.size()) {
                    parts.back() += c;
                    parts.back() += text[++i];
                    continue;
                }
                if (c == '"') quoted = !quoted;
                if (!quoted && c == sep) parts.emplace_back();
                else parts.back() += c;
            }
            return parts;
        }

        // key=value words: whitespace outside quotes separates them, and space around '=' is dropped
        std::vector<std::string> words(const std::string_view text) {
            std::vector<std::string> out;
            std::string word;
            bool quoted = false;
            for (std::size_t i = 0; i < text.size(); ++i) {
                const char c = text[i];
                if (quoted) {
                    word += c;
                    if (c == '\\' && i + 1 < text.size()) word += text[++i];
                    else if (c == '"') quoted = false;
                    continue;
                }
                if (space(c)) {
                    // Space before or after '=' belongs to the word
                    std::size_t next = i;
                    while (next < text.size() && space(text[next])) ++next;
                    if ((!word.empty() && word.back() == '=') || (next < text.size() && text[next] == '=')) {
                        i = next - 1;
                        continue;
                    }
                    if (!word.empty()) out.push_back(std::move(word));
                    word.clear();
                    continue;
                }
                if (c == '"') quoted = true;
                word += c;
            }
            if (!word.empty()) out.push_back(std::move(word));
            return out;
        }

        std::string unquoted(const std::string_view text) {
            if (text.size() < 2 || text.front() != '"' || text.back() != '"') return std::string{text};
            std::string out;
            for (std::size_t i = 1; i + 1 < text.size(); ++i) {
                if (text[i] == '\\' && i + 2 < text.size()) ++i;
                out += text[i];
            }
            return out;
        }

        bool loopback(const std::string &ip) { return ip.starts_with("127.") || ip == "::1"; }
    }

    std::optional<std::string> auth_result::property(const std::string_view name) const {
        const auto wanted = lower(name);
        for (const auto &[key, value]: properties)
            if (key == wanted) return value;
        return std::nullopt;
    }

    std::optional<authentication_results> parse_authentication_results(const std::string_view value) {
        const auto parts = split(without_comments(value), ';');
        const auto head = words(parts.front());
        if (head.empty() || head.front().find('=') != std::string::npos) return std::nullopt;
        authentication_results out;
        out.authserv_id = lower(head.front());
        for (std::size_t i = 1; i < parts.size(); ++i) {
            const auto said = words(parts[i]);
            if (said.empty() || (said.size() == 1 && lower(said.front()) == "none")) continue;
            auth_result one;
            for (const auto &word: said) {
                const auto equals = word.find('=');
                if (equals == std::string::npos || equals == 0) continue;
                const auto key = lower(word.substr(0, equals));
                const auto val = unquoted(word.substr(equals + 1));
                if (one.method.empty()) {
                    one.method = key.substr(0, key.find('/'));
                    one.result = lower(val);
                } else if (key == "reason") {
                    one.reason = val;
                } else if (key.find('.') != std::string::npos) {
                    one.properties.emplace_back(key, val);
                }
            }
            if (!one.method.empty()) out.results.push_back(std::move(one));
        }
        return out;
    }

    bool received::unknown_host() const { return lower(host) == "unknown"; }

    std::optional<received> parse_received(const std::string_view value) {
        const auto text = trim(value);
        if (lower(text.substr(0, 5)) != "from ") return std::nullopt;
        received out;
        std::size_t at = 5;
        while (at < text.size() && space(text[at])) ++at;
        const auto helo_end = std::min(text.size(), text.find_first_of(" \t(", at));
        out.helo = text.substr(at, helo_end - at);
        at = helo_end;
        while (at < text.size() && space(text[at])) ++at;
        if (at < text.size() && text[at] == '(') {
            // The comment: "host.example [192.0.2.1]", "unknown [192.0.2.1]", "[192.0.2.1]"; Exim's "port=1 helo=x" says none
            int depth = 0;
            std::size_t end = at;
            for (; end < text.size(); ++end) {
                if (text[end] == '(') ++depth;
                else if (text[end] == ')' && --depth == 0) break;
            }
            const auto comment = text.substr(at + 1, std::min(end, text.size()) - at - 1);
            const auto open = comment.find('[');
            const auto close = comment.find(']', open == std::string::npos ? 0 : open);
            if (open != std::string::npos && close != std::string::npos) {
                const auto before = trim(comment.substr(0, open));
                if (before.find('=') == std::string::npos) {
                    out.ip = comment.substr(open + 1, close - open - 1);
                    out.host = before;
                }
            }
            at = end + 1;
        }
        if (out.ip.empty() && out.helo.size() > 2 && out.helo.front() == '[' && out.helo.back() == ']')
            out.ip = out.helo.substr(1, out.helo.size() - 2); // Exim's: the address given as the name
        if (lower(out.ip).starts_with("ipv6:")) out.ip = out.ip.substr(5);
        // " by host"
        const auto lowered = lower(text);
        for (std::size_t by = lowered.find("by", at); by != std::string::npos; by = lowered.find("by", by + 2)) {
            if ((by > 0 && !space(text[by - 1])) || by + 2 >= text.size() || !space(text[by + 2])) continue;
            std::size_t start = by + 2;
            while (start < text.size() && space(text[start])) ++start;
            const auto end = std::min(text.size(), text.find_first_of(" \t(;", start));
            out.by = text.substr(start, end - start);
            break;
        }
        return out;
    }

    const auth_result *trace::result(const std::string_view method) const {
        const auto found = results(method);
        return found.empty() ? nullptr : found.front();
    }

    std::vector<const auth_result *> trace::results(const std::string_view method) const {
        const auto wanted = lower(method);
        std::vector<const auth_result *> found;
        for (const auto &header: authentication)
            for (const auto &one: header.results)
                if (one.method == wanted) found.push_back(&one);
        return found;
    }

    std::string trace::spf() const {
        if (const auto *said = result("spf")) return said->result;
        for (const auto &value: received_spf) {
            const auto first = words(trim(value));
            if (!first.empty()) return lower(first.front());
        }
        return "";
    }

    trace trace_of(const std::vector<header> &headers, const std::vector<std::string> &authserv_ids) {
        std::vector<std::string> ids;
        for (const auto &id: authserv_ids) ids.push_back(lower(trim(id)));
        trace out;
        for (const auto &[name, value]: headers) {
            const auto key = lower(name);
            if (key == "received") {
                const auto line = parse_received(value);
                if (line && !line->ip.empty() && !loopback(line->ip)) {
                    out.client = *line;
                    break;
                }
            } else if (key == "authentication-results") {
                const auto results = parse_authentication_results(value);
                if (results && std::ranges::find(ids, results->authserv_id) != ids.end()) out.authentication.push_back(*results);
            } else if (key == "received-spf") {
                out.received_spf.push_back(value);
            }
        }
        return out;
    }

    trace trace_of(const message &mail, const std::vector<std::string> &authserv_ids) {
        return trace_of(mail.headers(), authserv_ids);
    }
}
