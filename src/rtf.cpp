// RTF for mail: Outlook's compressed RTF ([MS-OXRTFCP], PR_RTF_COMPRESSED) and RTF turned into HTML,
// either the HTML Outlook wrapped in RTF ([MS-OXRTFEX], \fromhtml1) or, for RTF written as RTF, simple
// HTML of its text and paragraphs.

#include "sc_mime.h"

#include <glib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sc::mime {
    namespace {
        std::uint32_t u32(const std::string_view bytes, const std::size_t at) {
            std::uint32_t value = 0;
            for (int i = 3; i >= 0; --i) value = value << 8 | static_cast<unsigned char>(bytes[at + i]);
            return value;
        }

        // What the LZFu dictionary starts with ([MS-OXRTFCP] 2.1.3.1.1).
        constexpr std::string_view prebuffer = "{\\rtf1\\ansi\\mac\\deff0\\deftab720{\\fonttbl;}{\\f0\\fnil \\froman \\fswiss \\fmodern \\fscript "
                                               "\\fdecor MS Sans SerifSymbolArialTimes New RomanCourier{\\colortbl\\red0\\green0\\blue0\r\n\\par "
                                               "\\pard\\plain\\f0\\fs20\\b\\i\\u\\tab\\tx";
    }

    std::string decompress_rtf(const std::string_view compressed) {
        if (compressed.size() < 16) throw std::runtime_error("sc::mime: compressed RTF is cut short");
        const auto size = u32(compressed, 0), raw_size = u32(compressed, 4), type = u32(compressed, 8);
        const auto data = compressed.substr(16, std::min<std::size_t>(compressed.size() - 16, size >= 12 ? size - 12 : 0));
        if (type == 0x414C454D) return std::string{data.substr(0, std::min<std::size_t>(data.size(), raw_size))}; // "MELA": not compressed
        if (type != 0x75465A4C) throw std::runtime_error("sc::mime: not compressed RTF");                         // "LZFu"
        std::array<char, 4096> dictionary{};
        std::memcpy(dictionary.data(), prebuffer.data(), prebuffer.size());
        std::size_t write = prebuffer.size();
        std::string out;
        out.reserve(raw_size);
        std::size_t at = 0;
        while (at < data.size()) {
            const auto control = static_cast<unsigned char>(data[at++]);
            for (int bit = 0; bit < 8 && at < data.size(); ++bit) {
                if (!(control & 1 << bit)) {
                    const char c = data[at++];
                    out += c;
                    dictionary[write] = c;
                    write = (write + 1) % dictionary.size();
                    continue;
                }
                if (at + 1 >= data.size()) return out;
                const auto token = static_cast<unsigned>(static_cast<unsigned char>(data[at]) << 8 | static_cast<unsigned char>(data[at + 1]));
                at += 2;
                const auto offset = token >> 4;
                const auto length = (token & 15) + 2;
                if (offset == write) return out; // the end
                for (unsigned i = 0; i < length; ++i) {
                    const char c = dictionary[(offset + i) % dictionary.size()];
                    out += c;
                    dictionary[write] = c;
                    write = (write + 1) % dictionary.size();
                }
            }
        }
        return out;
    }

    namespace {
        // RTF read as tokens: groups, control words with their numbers, control symbols and text.
        class rtf_reader {
        public:
            explicit rtf_reader(const std::string_view rtf) : rtf_(rtf) {}

            std::string html() {
                const bool from_html = rtf_.find("\\fromhtml1") != std::string_view::npos;
                codepage_ = codepage_of();
                std::vector<state> stack{state{}};
                std::string out;
                std::string pending; // bytes from \'hh, turned into UTF-8 together (a codepage may need two)
                const auto flush = [&] {
                    if (pending.empty()) return;
                    out += from_html ? convert(pending) : escape_utf8(convert(pending));
                    pending.clear();
                };
                const auto emit = [&](const std::string_view text) {
                    flush();
                    const auto &now = stack.back();
                    if (now.skip) return;
                    if (from_html) {
                        if (now.html_tag || !now.html_rtf) out += text;
                    } else {
                        for (const char c: text) out += escape(c);
                    }
                };
                const auto visible = [&] {
                    const auto &now = stack.back();
                    return !now.skip && (from_html ? (now.html_tag || !now.html_rtf) : true);
                };

                std::size_t i = 0;
                while (i < rtf_.size()) {
                    const char c = rtf_[i];
                    if (c == '{') {
                        flush();
                        auto inner = stack.back();
                        inner.destination_pending = false;
                        stack.push_back(inner);
                        ++i;
                        continue;
                    }
                    if (c == '}') {
                        flush();
                        if (stack.size() > 1) stack.pop_back();
                        ++i;
                        continue;
                    }
                    if (c == '\r' || c == '\n') {
                        ++i;
                        continue;
                    }
                    if (c != '\\') {
                        std::size_t end = i;
                        while (end < rtf_.size() && rtf_[end] != '\\' && rtf_[end] != '{' && rtf_[end] != '}' && rtf_[end] != '\r' && rtf_[end] != '\n') ++end;
                        if (stack.back().skip_chars > 0) {
                            const auto skipped = std::min<std::size_t>(stack.back().skip_chars, end - i);
                            stack.back().skip_chars -= static_cast<int>(skipped);
                            i += skipped;
                            continue;
                        }
                        emit(rtf_.substr(i, end - i));
                        i = end;
                        continue;
                    }
                    // A control symbol or word.
                    if (i + 1 >= rtf_.size()) break;
                    const char next = rtf_[i + 1];
                    if (next == '\'' && i + 3 < rtf_.size()) {
                        const char hex[3] = {rtf_[i + 2], rtf_[i + 3], 0};
                        i += 4;
                        if (stack.back().skip_chars > 0) {
                            --stack.back().skip_chars;
                            continue;
                        }
                        if (visible()) pending += static_cast<char>(std::strtol(hex, nullptr, 16));
                        continue;
                    }
                    if (!std::isalpha(static_cast<unsigned char>(next))) {
                        i += 2;
                        if (next == '*') {
                            stack.back().destination_pending = true;
                            continue;
                        }
                        if (next == '\\' || next == '{' || next == '}') emit(std::string_view{&next, 1});
                        else if (next == '~') emit(from_html ? "&nbsp;" : " ");
                        else if (next == '_') emit("-");
                        continue;
                    }
                    std::size_t end = i + 1;
                    while (end < rtf_.size() && std::isalpha(static_cast<unsigned char>(rtf_[end]))) ++end;
                    const std::string word{rtf_.substr(i + 1, end - i - 1)};
                    bool has_number = false;
                    long number = 0;
                    if (end < rtf_.size() && (rtf_[end] == '-' || std::isdigit(static_cast<unsigned char>(rtf_[end])))) {
                        std::size_t digits = end + (rtf_[end] == '-' ? 1 : 0);
                        while (digits < rtf_.size() && std::isdigit(static_cast<unsigned char>(rtf_[digits]))) ++digits;
                        number = std::strtol(std::string{rtf_.substr(end, digits - end)}.c_str(), nullptr, 10);
                        has_number = true;
                        end = digits;
                    }
                    if (end < rtf_.size() && rtf_[end] == ' ') ++end; // the delimiter belongs to the word
                    i = end;
                    word_(word, has_number, number, stack, out, emit, flush, from_html);
                }
                flush();
                if (from_html) return out;
                return "<!DOCTYPE html>\n<html><head><meta charset=\"utf-8\"></head><body><p>" + out + "</p></body></html>\n";
            }

        private:
            struct state {
                bool skip = false;               // a destination whose text isn't shown (fonttbl, pict, ...)
                bool html_tag = false;           // inside \*\htmltag: its text is the HTML
                bool html_rtf = false;           // \htmlrtf: RTF-only text, not the HTML's
                bool destination_pending = false; // after \*: the next word names a destination
                int unicode_skip = 1;            // \ucN
                int skip_chars = 0;              // what's left to skip after a \u
            };

            template<typename Emit, typename Flush>
            void word_(const std::string &word, const bool has_number, const long number, std::vector<state> &stack, std::string &out, Emit &emit,
                       Flush &flush, const bool from_html) {
                auto &now = stack.back();
                if (now.destination_pending) {
                    now.destination_pending = false;
                    if (word == "htmltag") {
                        now.html_tag = true;
                        now.skip = false;
                    } else {
                        now.skip = true; // an optional destination this doesn't know
                    }
                    return;
                }
                static const std::vector<std::string> skipped{"fonttbl", "colortbl", "stylesheet", "info", "pict", "header", "footer", "headerl", "headerr",
                                                              "footerl", "footerr", "object", "listtable", "listoverridetable", "rsidtbl", "generator",
                                                              "xmlnstbl", "themedata", "colorschememapping", "latentstyles", "datastore", "mmathPr"};
                if (std::find(skipped.begin(), skipped.end(), word) != skipped.end()) {
                    now.skip = true;
                    return;
                }
                if (word == "htmlrtf") {
                    now.html_rtf = !has_number || number != 0;
                    return;
                }
                if (word == "uc" && has_number) {
                    now.unicode_skip = static_cast<int>(number);
                    return;
                }
                if (word == "u" && has_number) {
                    if (!now.skip && (from_html ? (now.html_tag || !now.html_rtf) : true)) {
                        flush();
                        const auto code = static_cast<std::uint32_t>(number < 0 ? number + 65536 : number);
                        if (from_html) out += utf8(code);
                        else out += escape_utf8(utf8(code));
                    }
                    now.skip_chars = now.unicode_skip;
                    return;
                }
                if (word == "par" || word == "line") emit(from_html ? "\r\n" : "");
                if (!from_html && (word == "par")) {
                    flush();
                    if (!now.skip) out += "</p>\n<p>";
                } else if (!from_html && word == "line") {
                    flush();
                    if (!now.skip) out += "<br>\n";
                }
                if (word == "tab") emit("\t");
                if (word == "lquote") emit("\xE2\x80\x98");
                if (word == "rquote") emit("\xE2\x80\x99");
                if (word == "ldblquote") emit("\xE2\x80\x9C");
                if (word == "rdblquote") emit("\xE2\x80\x9D");
                if (word == "bullet") emit("\xE2\x80\xA2");
                if (word == "endash") emit("\xE2\x80\x93");
                if (word == "emdash") emit("\xE2\x80\x94");
            }

            static std::string utf8(const std::uint32_t c) {
                std::string out;
                if (c < 0x80) {
                    out += static_cast<char>(c);
                } else if (c < 0x800) {
                    out += static_cast<char>(0xC0 | c >> 6);
                    out += static_cast<char>(0x80 | (c & 0x3F));
                } else {
                    out += static_cast<char>(0xE0 | c >> 12);
                    out += static_cast<char>(0x80 | (c >> 6 & 0x3F));
                    out += static_cast<char>(0x80 | (c & 0x3F));
                }
                return out;
            }

            static std::string escape(const char c) {
                switch (c) {
                    case '<': return "&lt;";
                    case '>': return "&gt;";
                    case '&': return "&amp;";
                    case '"': return "&quot;";
                    default: return std::string(1, c);
                }
            }

            static std::string escape_utf8(const std::string &text) {
                std::string out;
                for (const char c: text) out += escape(c);
                return out;
            }

            // \ansicpgN, else Windows-1252.
            int codepage_of() const {
                const auto at = rtf_.find("\\ansicpg");
                if (at == std::string_view::npos) return 1252;
                return std::atoi(std::string{rtf_.substr(at + 8, 6)}.c_str());
            }

            std::string convert(const std::string &bytes) const {
                const std::string charset = codepage_ == 65001 ? "UTF-8" : "CP" + std::to_string(codepage_ > 0 ? codepage_ : 1252);
                gsize written = 0;
                gchar *converted = g_convert_with_fallback(bytes.data(), static_cast<gssize>(bytes.size()), "UTF-8", charset.c_str(), "?", nullptr, &written, nullptr);
                if (!converted) return std::string(bytes.size(), '?');
                std::string out{converted, written};
                g_free(converted);
                return out;
            }

            std::string_view rtf_;
            int codepage_ = 1252;
        };
    }

    std::string rtf_to_html(const std::string_view rtf) {
        if (!rtf.starts_with("{\\rtf")) throw std::runtime_error("sc::mime: not RTF");
        return rtf_reader{rtf}.html();
    }
}
