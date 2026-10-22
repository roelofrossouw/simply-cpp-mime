#include "sc_mime.h"

#include <gmime/gmime.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace sc::mime {
    namespace detail {
        // The parsed message; every part and attached message of it points into this one tree.
        struct document {
            GMimeMessage *message;

            explicit document(GMimeMessage *parsed) : message(parsed) {}
            document(const document &) = delete;
            document &operator=(const document &) = delete;
            ~document() { g_object_unref(message); }
        };
    }

    namespace {
        void start_gmime() {
            static std::once_flag once;
            std::call_once(once, [] { g_mime_init(); });
        }

        std::string text_of(const char *text) { return text ? text : ""; }

        std::string lowercase(std::string text) {
            std::ranges::transform(text, text.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        bool same_name(const std::string_view a, const std::string_view b) {
            return std::ranges::equal(a, b, [](const unsigned char x, const unsigned char y) { return std::tolower(x) == std::tolower(y); });
        }

        // Owns a GObject reference for the length of a scope.
        template<typename T>
        struct owned {
            T *object;
            explicit owned(T *created) : object(created) {}
            owned(const owned &) = delete;
            owned &operator=(const owned &) = delete;
            ~owned() { if (object) g_object_unref(object); }
        };

        std::string bytes_of(GMimeStream *stream) {
            const GByteArray *bytes = g_mime_stream_mem_get_byte_array(GMIME_STREAM_MEM(stream));
            return {reinterpret_cast<const char *>(bytes->data), bytes->len};
        }

        // An object as it would be sent, headers and all.
        std::string written(GMimeObject *object) {
            const owned<GMimeStream> stream{g_mime_stream_mem_new()};
            g_mime_object_write_to_stream(object, nullptr, stream.object);
            return bytes_of(stream.object);
        }

        std::string without_carriage_returns(std::string text) {
            std::erase(text, '\r');
            return text;
        }

        std::vector<mailbox> mailboxes(InternetAddressList *list) {
            std::vector<mailbox> out;
            if (!list) return out;
            for (int i = 0; i < internet_address_list_length(list); ++i) {
                InternetAddress *address = internet_address_list_get_address(list, i);
                if (INTERNET_ADDRESS_IS_GROUP(address)) {
                    auto members = mailboxes(internet_address_group_get_members(INTERNET_ADDRESS_GROUP(address)));
                    out.insert(out.end(), std::make_move_iterator(members.begin()), std::make_move_iterator(members.end()));
                } else if (INTERNET_ADDRESS_IS_MAILBOX(address)) {
                    out.push_back({text_of(internet_address_get_name(address)),
                                   text_of(internet_address_mailbox_get_addr(INTERNET_ADDRESS_MAILBOX(address)))});
                }
            }
            return out;
        }

        std::optional<std::string> header_in(GMimeObject *object, const std::string_view name) {
            GMimeHeaderList *list = g_mime_object_get_header_list(object);
            if (!list) return std::nullopt;
            GMimeHeader *found = g_mime_header_list_get_header(list, std::string{name}.c_str());
            if (!found) return std::nullopt;
            return text_of(g_mime_header_get_value(found));
        }

        std::vector<header> headers_in(GMimeObject *object) {
            std::vector<header> out;
            GMimeHeaderList *list = g_mime_object_get_header_list(object);
            if (!list) return out;
            const int count = g_mime_header_list_get_count(list);
            out.reserve(static_cast<std::size_t>(count));
            for (int i = 0; i < count; ++i) {
                GMimeHeader *each = g_mime_header_list_get_header_at(list, i);
                out.push_back({text_of(g_mime_header_get_name(each)), text_of(g_mime_header_get_value(each))});
            }
            return out;
        }
    }

    std::string content_type::name() const { return type + '/' + subtype; }

    bool content_type::is(const std::string_view media) const {
        const auto slash = media.find('/');
        if (slash == std::string_view::npos) return same_name(type, media);
        if (!same_name(type, media.substr(0, slash))) return false;
        const auto sub = media.substr(slash + 1);
        return sub == "*" || same_name(subtype, sub);
    }

    std::optional<std::string> content_type::parameter(const std::string_view name) const {
        for (const auto &[key, value]: parameters) {
            if (same_name(key, name)) return value;
        }
        return std::nullopt;
    }

    std::string content_type::str() const {
        std::string out = name();
        for (const auto &[key, value]: parameters) {
            const bool plain = !value.empty() && std::ranges::all_of(value, [](const unsigned char c) {
                return std::isalnum(c) || std::string_view{"!#$&+-.^_`|~"}.find(static_cast<char>(c)) != std::string_view::npos;
            });
            out += "; " + key + '=';
            if (plain) {
                out += value;
            } else {
                out += '"';
                for (const char c: value) {
                    if (c == '"' || c == '\\') out += '\\';
                    out += c;
                }
                out += '"';
            }
        }
        return out;
    }

    std::string mailbox::str() const {
        if (name.empty()) return address;
        return name + " <" + address + '>';
    }

    // ---- part ----

    part::part(std::shared_ptr<const detail::document> document, _GMimeObject *object)
        : document_(std::move(document)), object_(object) {}

    content_type part::type() const {
        content_type out;
        GMimeContentType *gmime = g_mime_object_get_content_type(object_);
        if (!gmime) return out;
        out.type = lowercase(text_of(g_mime_content_type_get_media_type(gmime)));
        out.subtype = lowercase(text_of(g_mime_content_type_get_media_subtype(gmime)));
        if (GMimeParamList *list = g_mime_content_type_get_parameters(gmime)) {
            for (int i = 0; i < g_mime_param_list_length(list); ++i) {
                GMimeParam *param = g_mime_param_list_get_parameter_at(list, i);
                out.parameters.emplace_back(lowercase(text_of(g_mime_param_get_name(param))), text_of(g_mime_param_get_value(param)));
            }
        }
        return out;
    }

    mime::disposition part::disposition() const {
        GMimeContentDisposition *gmime = g_mime_object_get_content_disposition(object_);
        if (!gmime) return mime::disposition::none;
        const std::string kind = text_of(g_mime_content_disposition_get_disposition(gmime));
        if (same_name(kind, "attachment")) return mime::disposition::attachment;
        if (same_name(kind, "inline")) return mime::disposition::inline_;
        return mime::disposition::attachment; // RFC 2183: an unknown kind is treated as an attachment
    }

    std::optional<std::string> part::filename() const {
        const char *name = GMIME_IS_PART(object_)
                               ? g_mime_part_get_filename(GMIME_PART(object_))
                               : g_mime_object_get_content_disposition_parameter(object_, "filename");
        if (!name && !GMIME_IS_PART(object_)) name = g_mime_object_get_content_type_parameter(object_, "name");
        if (!name || !*name) return std::nullopt;
        return std::string{name};
    }

    std::string part::content_id() const { return text_of(g_mime_object_get_content_id(object_)); }

    mime::transfer_encoding part::transfer_encoding() const {
        if (!GMIME_IS_PART(object_)) return mime::transfer_encoding::none;
        switch (g_mime_part_get_content_encoding(GMIME_PART(object_))) {
            case GMIME_CONTENT_ENCODING_7BIT: return mime::transfer_encoding::seven_bit;
            case GMIME_CONTENT_ENCODING_8BIT: return mime::transfer_encoding::eight_bit;
            case GMIME_CONTENT_ENCODING_BINARY: return mime::transfer_encoding::binary;
            case GMIME_CONTENT_ENCODING_BASE64: return mime::transfer_encoding::base64;
            case GMIME_CONTENT_ENCODING_QUOTEDPRINTABLE: return mime::transfer_encoding::quoted_printable;
            case GMIME_CONTENT_ENCODING_UUENCODE: return mime::transfer_encoding::uuencode;
            default: return mime::transfer_encoding::none;
        }
    }

    std::optional<std::string> part::header(const std::string_view name) const { return header_in(object_, name); }

    std::vector<header> part::headers() const { return headers_in(object_); }

    std::string part::data() const {
        if (GMIME_IS_MESSAGE_PART(object_)) {
            GMimeMessage *inner = g_mime_message_part_get_message(GMIME_MESSAGE_PART(object_));
            return inner ? written(GMIME_OBJECT(inner)) : std::string{};
        }
        if (!GMIME_IS_PART(object_)) return written(object_);
        GMimeDataWrapper *content = g_mime_part_get_content(GMIME_PART(object_));
        if (!content) return {};
        const owned<GMimeStream> stream{g_mime_stream_mem_new()};
        g_mime_data_wrapper_write_to_stream(content, stream.object);
        return bytes_of(stream.object);
    }

    std::string part::text() const {
        if (!GMIME_IS_TEXT_PART(object_)) throw std::logic_error("sc::mime: " + type().name() + " isn't text");
        char *text = g_mime_text_part_get_text(GMIME_TEXT_PART(object_));
        std::string out = text_of(text);
        g_free(text);
        return without_carriage_returns(std::move(out));
    }

    bool part::is_multipart() const { return GMIME_IS_MULTIPART(object_); }

    std::vector<part> part::parts() const {
        std::vector<part> out;
        if (!GMIME_IS_MULTIPART(object_)) return out;
        GMimeMultipart *multipart = GMIME_MULTIPART(object_);
        const int count = g_mime_multipart_get_count(multipart);
        out.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) out.push_back(part{document_, g_mime_multipart_get_part(multipart, i)});
        return out;
    }

    bool part::is_message() const { return GMIME_IS_MESSAGE_PART(object_); }

    std::optional<mime::message> part::message() const {
        if (!GMIME_IS_MESSAGE_PART(object_)) return std::nullopt;
        GMimeMessage *inner = g_mime_message_part_get_message(GMIME_MESSAGE_PART(object_));
        if (!inner) return std::nullopt;
        return mime::message{document_, inner};
    }

    bool part::is_attachment() const {
        if (is_multipart()) return false;
        if (disposition() == mime::disposition::attachment || filename()) return true;
        const auto kind = type();
        return !kind.is("text/plain") && !kind.is("text/html");
    }

    // ---- message ----

    message::message(std::shared_ptr<const detail::document> document, _GMimeMessage *gmime)
        : document_(std::move(document)), message_(gmime) {}

    message message::parse(const std::string_view text) {
        // GMime before 3.2.13 (jammy's 3.2.7) makes an empty message of nothing.
        if (text.find_first_not_of(" \t\r\n") == std::string_view::npos) throw std::runtime_error("sc::mime: not a mail message");
        start_gmime();
        const owned<GMimeStream> stream{g_mime_stream_mem_new_with_buffer(text.data(), text.size())};
        const owned<GMimeParser> parser{g_mime_parser_new_with_stream(stream.object)};
        GMimeMessage *parsed = g_mime_parser_construct_message(parser.object, nullptr);
        if (!parsed) throw std::runtime_error("sc::mime: not a mail message");
        auto document = std::make_shared<const detail::document>(parsed);
        return {document, parsed};
    }

    message message::load(const std::filesystem::path &file) {
        std::ifstream in{file, std::ios::binary};
        if (!in) throw std::runtime_error("sc::mime: can't read " + file.string());
        std::ostringstream text;
        text << in.rdbuf();
        if (in.bad()) throw std::runtime_error("sc::mime: can't read " + file.string());
        return parse(text.str());
    }

    std::string message::subject() const { return text_of(g_mime_message_get_subject(message_)); }

    std::vector<mailbox> message::from() const { return mailboxes(g_mime_message_get_from(message_)); }

    std::vector<mailbox> message::sender() const { return mailboxes(g_mime_message_get_sender(message_)); }

    std::vector<mailbox> message::reply_to() const { return mailboxes(g_mime_message_get_reply_to(message_)); }

    std::vector<mailbox> message::to() const { return mailboxes(g_mime_message_get_to(message_)); }

    std::vector<mailbox> message::cc() const { return mailboxes(g_mime_message_get_cc(message_)); }

    std::vector<mailbox> message::bcc() const { return mailboxes(g_mime_message_get_bcc(message_)); }

    std::optional<sc::datetime> message::date() const {
        GDateTime *date = g_mime_message_get_date(message_);
        if (!date) return std::nullopt;
        return sc::datetime::from_unix(g_date_time_to_unix(date));
    }

    std::string message::message_id() const { return text_of(g_mime_message_get_message_id(message_)); }

    std::optional<std::string> message::header(const std::string_view name) const { return header_in(GMIME_OBJECT(message_), name); }

    std::vector<header> message::headers() const { return headers_in(GMIME_OBJECT(message_)); }

    part message::body() const {
        GMimeObject *top = g_mime_message_get_mime_part(message_);
        if (!top) throw std::runtime_error("sc::mime: the message has no body");
        return part{document_, top};
    }

    namespace {
        // The body text of one kind (text/plain or text/html) under a part: the first alternative
        // that has it, or every piece of it in a mixed multipart, joined. Attachments and attached
        // messages don't count.
        std::optional<std::string> body_text(const part &where, const std::string_view kind) {
            if (where.is_multipart()) {
                const bool alternatives = where.is("multipart/alternative");
                std::optional<std::string> joined;
                for (const auto &each: where.parts()) {
                    auto found = body_text(each, kind);
                    if (!found) continue;
                    if (alternatives) return found;
                    if (!joined) joined = std::move(found);
                    else *joined += (joined->ends_with('\n') ? "" : "\n") + *found;
                }
                return joined;
            }
            if (where.is_attachment() || !where.is(kind)) return std::nullopt;
            return where.text();
        }

        void collect_leaves(const part &where, std::vector<part> &out) {
            if (!where.is_multipart()) {
                out.push_back(where);
                return;
            }
            for (const auto &each: where.parts()) collect_leaves(each, out);
        }
    }

    std::optional<std::string> message::text() const { return body_text(body(), "text/plain"); }

    std::optional<std::string> message::html() const { return body_text(body(), "text/html"); }

    std::vector<part> message::leaves() const {
        std::vector<part> out;
        collect_leaves(body(), out);
        return out;
    }

    std::vector<part> message::attachments() const {
        auto out = leaves();
        std::erase_if(out, [](const part &each) { return !each.is_attachment(); });
        return out;
    }
}
