// Outlook .msg files ([MS-OXMSG]): a compound file ([MS-CFB]) of MAPI properties, turned into a MIME
// message with draft, so message::parse() reads them as it reads .eml.

#include "sc_mime.h"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sc::mime {
    namespace {
        constexpr std::array<unsigned char, 8> cfb_magic{0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1};
        constexpr std::uint32_t end_of_chain = 0xFFFFFFFE, free_sector = 0xFFFFFFFF, no_stream = 0xFFFFFFFF;

        std::uint16_t u16(const std::string_view bytes, const std::size_t at) {
            if (at + 2 > bytes.size()) throw std::runtime_error("sc::mime: the .msg file is cut short");
            return static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[at]) | static_cast<unsigned char>(bytes[at + 1]) << 8);
        }

        std::uint32_t u32(const std::string_view bytes, const std::size_t at) {
            if (at + 4 > bytes.size()) throw std::runtime_error("sc::mime: the .msg file is cut short");
            std::uint32_t value = 0;
            for (int i = 3; i >= 0; --i) value = value << 8 | static_cast<unsigned char>(bytes[at + i]);
            return value;
        }

        std::uint64_t u64(const std::string_view bytes, const std::size_t at) {
            return static_cast<std::uint64_t>(u32(bytes, at)) | static_cast<std::uint64_t>(u32(bytes, at + 4)) << 32;
        }

        // UTF-16LE as UTF-8 (a lone surrogate as U+FFFD), stopping at a NUL.
        std::string utf8_of_utf16(const std::string_view bytes) {
            std::string out;
            const auto put = [&](const std::uint32_t c) {
                if (c < 0x80) {
                    out += static_cast<char>(c);
                } else if (c < 0x800) {
                    out += static_cast<char>(0xC0 | c >> 6);
                    out += static_cast<char>(0x80 | (c & 0x3F));
                } else if (c < 0x10000) {
                    out += static_cast<char>(0xE0 | c >> 12);
                    out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                } else {
                    out += static_cast<char>(0xF0 | c >> 18);
                    out += static_cast<char>(0x80 | (c >> 12 & 0x3F));
                    out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
            };
            for (std::size_t i = 0; i + 1 < bytes.size(); i += 2) {
                std::uint32_t c = u16(bytes, i);
                if (c == 0) break;
                if (c >= 0xD800 && c < 0xDC00 && i + 3 < bytes.size()) {
                    const std::uint32_t low = u16(bytes, i + 2);
                    if (low >= 0xDC00 && low < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
                        i += 2;
                    } else {
                        c = 0xFFFD;
                    }
                } else if (c >= 0xD800 && c < 0xE000) {
                    c = 0xFFFD;
                }
                put(c);
            }
            return out;
        }

        // A compound file: its storages and streams by path ("__attach_version1.0_#00000000/__substg1.0_3701000D/...").
        class compound_file {
        public:
            explicit compound_file(const std::string_view bytes) : bytes_(bytes) {
                if (bytes.size() < 512 || !std::equal(cfb_magic.begin(), cfb_magic.end(), reinterpret_cast<const unsigned char *>(bytes.data())))
                    throw std::runtime_error("sc::mime: not an Outlook .msg file");
                const auto shift = u16(bytes, 0x1E), mini_shift = u16(bytes, 0x20);
                if (shift != 9 && shift != 12) throw std::runtime_error("sc::mime: the .msg file's sector size isn't 512 or 4096");
                sector_size_ = std::size_t{1} << shift;
                mini_size_ = std::size_t{1} << mini_shift;
                mini_cutoff_ = u32(bytes, 0x38);

                // The FAT's sectors: 109 in the header, the rest in the DIFAT chain.
                std::vector<std::uint32_t> fat_sectors;
                for (std::size_t i = 0; i < 109; ++i) {
                    const auto sector = u32(bytes, 0x4C + i * 4);
                    if (sector != free_sector) fat_sectors.push_back(sector);
                }
                auto difat = u32(bytes, 0x44);
                for (std::size_t guard = 0; difat != end_of_chain && difat != free_sector && guard < 100000; ++guard) {
                    const auto at = offset(difat);
                    const auto per = sector_size_ / 4 - 1;
                    for (std::size_t i = 0; i < per; ++i) {
                        const auto sector = u32(bytes, at + i * 4);
                        if (sector != free_sector) fat_sectors.push_back(sector);
                    }
                    difat = u32(bytes, at + per * 4);
                }
                for (const auto sector: fat_sectors) {
                    const auto at = offset(sector);
                    for (std::size_t i = 0; i < sector_size_ / 4; ++i) fat_.push_back(u32(bytes, at + i * 4));
                }

                const auto directory = chain(u32(bytes, 0x30));
                for (std::size_t at = 0; at + 128 <= directory.size(); at += 128) {
                    entry each;
                    const auto name_bytes = std::min<std::size_t>(u16(directory, at + 0x40), 64);
                    each.name = utf8_of_utf16(std::string_view{directory}.substr(at, name_bytes));
                    each.type = static_cast<unsigned char>(directory[at + 0x42]);
                    each.left = u32(directory, at + 0x44);
                    each.right = u32(directory, at + 0x48);
                    each.child = u32(directory, at + 0x4C);
                    each.start = u32(directory, at + 0x74);
                    each.size = u64(directory, at + 0x78);
                    if (sector_size_ == 512) each.size &= 0xFFFFFFFF; // version 3: the high half may be junk
                    entries_.push_back(std::move(each));
                }
                if (entries_.empty() || entries_[0].type != 5) throw std::runtime_error("sc::mime: the .msg file has no root storage");
                mini_stream_ = chain(entries_[0].start, entries_[0].size);
                const auto mini_fat_bytes = chain(u32(bytes, 0x3C));
                for (std::size_t at = 0; at + 4 <= mini_fat_bytes.size(); at += 4) mini_fat_.push_back(u32(mini_fat_bytes, at));
            }

            // The children of the storage at index (0: the root), by name.
            [[nodiscard]] std::map<std::string, std::uint32_t> children(const std::uint32_t index) const {
                std::map<std::string, std::uint32_t> out;
                std::vector<std::uint32_t> pending{entries_.at(index).child};
                for (std::size_t guard = 0; !pending.empty() && guard < entries_.size() * 2 + 16; ++guard) {
                    const auto at = pending.back();
                    pending.pop_back();
                    if (at == no_stream || at >= entries_.size()) continue;
                    out.emplace(entries_[at].name, at);
                    pending.push_back(entries_[at].left);
                    pending.push_back(entries_[at].right);
                }
                return out;
            }

            [[nodiscard]] bool is_storage(const std::uint32_t index) const { return entries_.at(index).type == 1; }

            // A stream's bytes.
            [[nodiscard]] std::string stream(const std::uint32_t index) const {
                const auto &each = entries_.at(index);
                if (each.size < mini_cutoff_) {
                    std::string out;
                    auto sector = each.start;
                    for (std::size_t guard = 0; out.size() < each.size && sector != end_of_chain && guard < mini_fat_.size() + 1; ++guard) {
                        const auto at = static_cast<std::size_t>(sector) * mini_size_;
                        if (at + mini_size_ > mini_stream_.size()) throw std::runtime_error("sc::mime: the .msg file's mini stream is cut short");
                        out.append(mini_stream_, at, mini_size_);
                        sector = sector < mini_fat_.size() ? mini_fat_[sector] : end_of_chain;
                    }
                    out.resize(std::min<std::size_t>(out.size(), each.size));
                    return out;
                }
                return chain(each.start, each.size);
            }

        private:
            struct entry {
                std::string name;
                unsigned type = 0;
                std::uint32_t left = no_stream, right = no_stream, child = no_stream, start = end_of_chain;
                std::uint64_t size = 0;
            };

            std::size_t offset(const std::uint32_t sector) const {
                const auto at = (static_cast<std::size_t>(sector) + 1) * sector_size_;
                if (at + sector_size_ > bytes_.size()) throw std::runtime_error("sc::mime: the .msg file is cut short");
                return at;
            }

            // A FAT chain's bytes, cut to size when it's given.
            std::string chain(std::uint32_t sector, const std::optional<std::uint64_t> size = std::nullopt) const {
                std::string out;
                for (std::size_t guard = 0; sector != end_of_chain && sector != free_sector && guard < fat_.size() + 1; ++guard) {
                    if (size && out.size() >= *size) break;
                    out.append(bytes_.substr(offset(sector), sector_size_));
                    sector = sector < fat_.size() ? fat_[sector] : end_of_chain;
                }
                if (size) out.resize(std::min<std::size_t>(out.size(), *size));
                return out;
            }

            std::string_view bytes_;
            std::size_t sector_size_ = 512, mini_size_ = 64;
            std::uint64_t mini_cutoff_ = 4096;
            std::vector<std::uint32_t> fat_, mini_fat_;
            std::vector<entry> entries_;
            std::string mini_stream_;
        };

        // MAPI property types and ids ([MS-OXPROPS]).
        constexpr std::uint16_t pt_long = 0x0003, pt_boolean = 0x000B, pt_object = 0x000D, pt_string8 = 0x001E, pt_unicode = 0x001F,
                                pt_systime = 0x0040, pt_binary = 0x0102;
        enum : std::uint16_t {
            subject = 0x0037,
            client_submit_time = 0x0039,
            sent_representing_name = 0x0042,
            sent_representing_email = 0x0065,
            transport_headers = 0x007D,
            sender_name = 0x0C1A,
            sender_email = 0x0C1F,
            recipient_type = 0x0C15,
            delivery_time = 0x0E06,
            body = 0x1000,
            rtf_compressed = 0x1009,
            html = 0x1013,
            internet_message_id = 0x1035,
            display_name = 0x3001,
            address_type = 0x3002,
            email_address = 0x3003,
            attach_data = 0x3701,
            attach_filename = 0x3704,
            attach_method = 0x3705,
            attach_long_filename = 0x3707,
            attach_mime_tag = 0x370E,
            attach_content_id = 0x3712,
            attach_flags = 0x3714,
            smtp_address = 0x39FE,
            internet_cpid = 0x3FDE,
            sender_smtp = 0x5D01,
            sent_representing_smtp = 0x5D02,
            attachment_hidden = 0x7FFE,
        };

        // One storage's properties: variable ones from their streams, fixed ones from __properties_version1.0.
        class properties {
        public:
            properties(const compound_file &file, const std::uint32_t storage, const std::size_t header_size) : file_(file) {
                for (const auto &[name, index]: file.children(storage)) {
                    // An embedded message is a storage named like a property (__substg1.0_3701000D).
                    if (file.is_storage(index)) {
                        children_[name] = index;
                    } else if (name.starts_with("__substg1.0_") && name.size() == 20) {
                        const auto id = static_cast<std::uint16_t>(std::stoul(name.substr(12, 4), nullptr, 16));
                        const auto type = static_cast<std::uint16_t>(std::stoul(name.substr(16, 4), nullptr, 16));
                        streams_[id] = {type, index};
                    } else if (name == "__properties_version1.0" && !file.is_storage(index)) {
                        const auto bytes = file.stream(index);
                        for (std::size_t at = header_size; at + 16 <= bytes.size(); at += 16) {
                            const auto tag = u32(bytes, at);
                            fixed_[static_cast<std::uint16_t>(tag >> 16)] = {static_cast<std::uint16_t>(tag & 0xFFFF), u64(bytes, at + 8)};
                        }
                    }
                }
            }

            void codepage(const int page) { codepage_ = page; }

            // A string property in UTF-8: PT_UNICODE, or PT_STRING8 in the message's codepage.
            [[nodiscard]] std::optional<std::string> text(const std::uint16_t id) const {
                const auto found = streams_.find(id);
                if (found == streams_.end()) return std::nullopt;
                const auto bytes = file_.stream(found->second.second);
                if (found->second.first == pt_unicode) return utf8_of_utf16(bytes);
                if (found->second.first == pt_string8 || found->second.first == pt_binary) return from_codepage(cut_at_nul(bytes), codepage_);
                return std::nullopt;
            }

            [[nodiscard]] std::optional<std::string> binary(const std::uint16_t id) const {
                const auto found = streams_.find(id);
                if (found == streams_.end()) return std::nullopt;
                if (found->second.first == pt_unicode) return utf8_of_utf16(file_.stream(found->second.second));
                return file_.stream(found->second.second);
            }

            [[nodiscard]] std::optional<std::int64_t> number(const std::uint16_t id) const {
                const auto found = fixed_.find(id);
                if (found == fixed_.end() || (found->second.first != pt_long && found->second.first != pt_boolean)) return std::nullopt;
                return static_cast<std::int32_t>(found->second.second & 0xFFFFFFFF);
            }

            // A PT_SYSTIME as seconds since 1970.
            [[nodiscard]] std::optional<long long> time(const std::uint16_t id) const {
                const auto found = fixed_.find(id);
                if (found == fixed_.end() || found->second.first != pt_systime || found->second.second == 0) return std::nullopt;
                return static_cast<long long>(found->second.second / 10000000ULL) - 11644473600LL;
            }

            // An embedded object's storage (an attached message's).
            [[nodiscard]] std::optional<std::uint32_t> object(const std::uint16_t id) const {
                char name[32];
                std::snprintf(name, sizeof name, "__substg1.0_%04X%04X", id, pt_object);
                const auto found = children_.find(name);
                if (found == children_.end()) return std::nullopt;
                return found->second;
            }

            [[nodiscard]] std::vector<std::uint32_t> storages(const std::string_view prefix) const {
                std::vector<std::uint32_t> out;
                for (const auto &[name, index]: children_)
                    if (name.starts_with(prefix)) out.push_back(index);
                return out;
            }

            static std::string from_codepage(const std::string &bytes, const int codepage) {
                if (codepage == 65001 || std::all_of(bytes.begin(), bytes.end(), [](const unsigned char c) { return c < 0x80; })) return bytes;
                const std::string charset = codepage == 20127 ? "ASCII" : codepage == 28591 ? "ISO-8859-1" : "CP" + std::to_string(codepage);
                gsize written = 0;
                gchar *converted = g_convert_with_fallback(bytes.data(), static_cast<gssize>(bytes.size()), "UTF-8", charset.c_str(), "?", nullptr, &written, nullptr);
                if (!converted) converted = g_convert_with_fallback(bytes.data(), static_cast<gssize>(bytes.size()), "UTF-8", "CP1252", "?", nullptr, &written, nullptr);
                if (!converted) return bytes;
                std::string out{converted, written};
                g_free(converted);
                return out;
            }

        private:
            static std::string cut_at_nul(std::string bytes) {
                if (const auto nul = bytes.find('\0'); nul != std::string::npos) bytes.resize(nul);
                return bytes;
            }

            const compound_file &file_;
            int codepage_ = 1252;
            std::map<std::uint16_t, std::pair<std::uint16_t, std::uint32_t>> streams_; // id -> type, entry
            std::map<std::uint16_t, std::pair<std::uint16_t, std::uint64_t>> fixed_;   // id -> type, value
            std::map<std::string, std::uint32_t> children_;
        };

        std::string trimmed(std::string text) {
            while (!text.empty() && (text.back() == '\0' || std::isspace(static_cast<unsigned char>(text.back())))) text.pop_back();
            text.erase(0, text.find_first_not_of(" \t\r\n"));
            return text;
        }

        // A mailbox from a name and an address that may be an Exchange one ("/O=ORG/OU=.../CN=ann"):
        // the SMTP address when there is one, else what's there.
        mailbox mailbox_of(const std::optional<std::string> &name, const std::optional<std::string> &smtp, const std::optional<std::string> &address) {
            mailbox out;
            out.name = trimmed(name.value_or(""));
            out.address = trimmed(smtp && !smtp->empty() ? *smtp : address.value_or(""));
            return out;
        }

        // The headers Outlook kept from the message as it arrived (PR_TRANSPORT_MESSAGE_HEADERS):
        // those that say who and when, unfolded, by lower-cased name.
        std::map<std::string, std::string> transport_headers_of(const std::string &text) {
            std::map<std::string, std::string> out;
            std::string name, value;
            const auto flush = [&] {
                if (name.empty()) return;
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
                if (!out.contains(lower)) out[lower] = trimmed(value);
                name.clear();
                value.clear();
            };
            std::size_t start = 0;
            while (start < text.size()) {
                auto end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                std::string line = text.substr(start, end - start);
                start = end + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) break;
                if (line.front() == ' ' || line.front() == '\t') {
                    value += " " + trimmed(line);
                } else if (const auto colon = line.find(':'); colon != std::string::npos) {
                    flush();
                    name = line.substr(0, colon);
                    value = line.substr(colon + 1);
                }
            }
            flush();
            return out;
        }

        std::string message_from(const compound_file &file, std::uint32_t storage, bool top, int depth);

        // An attachment's filename, type and bytes (an attached message as message/rfc822).
        void add_attachment(draft &out, const compound_file &file, const std::uint32_t storage, const int codepage, const int depth) {
            properties attachment{file, storage, 8};
            attachment.codepage(codepage);
            const auto method = attachment.number(attach_method).value_or(1);
            std::string filename = trimmed(attachment.text(attach_long_filename).value_or(attachment.text(attach_filename).value_or(attachment.text(display_name).value_or(""))));
            const auto content_id = trimmed(attachment.text(attach_content_id).value_or(""));
            std::string type = trimmed(attachment.text(attach_mime_tag).value_or(""));
            std::string bytes;
            if (const auto embedded = attachment.object(attach_data); embedded && method == 5) {
                bytes = message_from(file, *embedded, false, depth + 1);
                type = "message/rfc822";
                if (filename.empty()) filename = "message.eml";
            } else if (const auto data = attachment.binary(attach_data)) {
                bytes = *data;
            } else {
                return; // an OLE object or a link: nothing to carry
            }
            if (type.empty()) type = "application/octet-stream";
            // Inline pictures: shown by the HTML through their Content-ID.
            const bool inline_ = !content_id.empty() && (attachment.number(attachment_hidden).value_or(0) != 0 || (attachment.number(attach_flags).value_or(0) & 4) != 0 ||
                                                         type.starts_with("image/"));
            out.attach(filename, type, std::move(bytes), inline_, content_id);
        }

        // A message storage (the root, or an attached one) as MIME text.
        std::string message_from(const compound_file &file, const std::uint32_t storage, const bool top, const int depth) {
            if (depth > 10) throw std::runtime_error("sc::mime: the .msg file nests messages too deeply");
            properties message{file, storage, std::size_t{top ? 32u : 24u}};
            const auto cpid = message.number(internet_cpid);
            const int codepage = cpid && *cpid > 0 ? static_cast<int>(*cpid) : 1252;
            message.codepage(codepage);

            draft out;
            const auto headers = transport_headers_of(message.text(transport_headers).value_or(""));
            const auto header = [&](const char *name) -> std::optional<std::string> {
                const auto found = headers.find(name);
                return found == headers.end() ? std::nullopt : std::optional<std::string>{found->second};
            };

            // Who: the SMTP addresses MAPI has, else the transport headers' (GMime parses them).
            const auto from = mailbox_of(message.text(sent_representing_name).has_value() ? message.text(sent_representing_name) : message.text(sender_name),
                                         message.text(sent_representing_smtp).has_value() ? message.text(sent_representing_smtp) : message.text(sender_smtp),
                                         message.text(sent_representing_email).has_value() ? message.text(sent_representing_email) : message.text(sender_email));
            bool listed = false;
            for (const auto recipient: message.storages("__recip_version1.0_")) {
                properties who{file, recipient, 8};
                who.codepage(codepage);
                const auto box = mailbox_of(who.text(display_name), who.text(smtp_address), who.text(email_address));
                if (box.address.empty() && box.name.empty()) continue;
                listed = true;
                switch (who.number(recipient_type).value_or(1)) {
                    case 2: out.cc(box); break;
                    case 3: out.bcc(box); break;
                    default: out.to(box);
                }
            }
            if (!from.address.empty() || !from.name.empty()) out.from(from);
            else if (const auto value = header("from")) out.header("From", *value);
            if (!listed) {
                if (const auto value = header("to")) out.header("To", *value);
                if (const auto value = header("cc")) out.header("Cc", *value);
            }
            if (const auto value = header("reply-to")) out.header("Reply-To", *value);
            out.subject(trimmed(message.text(subject).value_or(header("subject").value_or(""))));
            const auto id = trimmed(message.text(internet_message_id).value_or(header("message-id").value_or("")));
            if (!id.empty()) out.header("Message-ID", id);
            if (const auto when = message.time(client_submit_time).has_value() ? message.time(client_submit_time) : message.time(delivery_time))
                out.date(sc::datetime::from_unix(*when));
            else if (const auto value = header("date"))
                out.header("Date", *value);

            // The body: the text, and HTML from PR_HTML or from the RTF (Outlook's wrapped HTML, or RTF itself).
            if (const auto text = message.text(body)) out.text(trimmed(*text) + "\n");
            std::optional<std::string> html_body;
            if (const auto bytes = message.binary(html)) html_body = properties::from_codepage(*bytes, codepage);
            if (!html_body) {
                if (const auto compressed = message.binary(rtf_compressed)) {
                    try {
                        html_body = rtf_to_html(decompress_rtf(*compressed));
                    } catch (const std::exception &) {
                        // An RTF that can't be read: the text body stands.
                    }
                }
            }
            if (html_body) {
                if (const auto nul = html_body->find('\0'); nul != std::string::npos) html_body->resize(nul);
                out.html(*html_body);
            }
            for (const auto attachment: message.storages("__attach_version1.0_")) add_attachment(out, file, attachment, codepage, depth);
            return out.str();
        }
    }

    bool is_outlook(const std::string_view bytes) {
        return bytes.size() >= cfb_magic.size() && std::equal(cfb_magic.begin(), cfb_magic.end(), reinterpret_cast<const unsigned char *>(bytes.data()));
    }

    std::string outlook_to_mime(const std::string_view msg) {
        const compound_file file{msg};
        return message_from(file, 0, true, 0);
    }
}
