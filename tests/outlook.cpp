// Outlook .msg files read as messages (a file resource/make_msg.py writes: recipients, an RTF body
// wrapping HTML, attachments in the mini stream and in sectors of their own, an inline picture, an
// attached message), RTF as HTML, compressed RTF, and a draft's own date.
#include "sc_mime.h"
#include "sc_test.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <sstream>
#include <string>

int main() {
    SECTION("RTF as HTML: Outlook's wrapped HTML, and RTF itself");
    {
        const auto wrapped = sc::mime::rtf_to_html(
            "{\\rtf1\\ansi\\ansicpg1252\\fromhtml1 {\\fonttbl{\\f0 Arial;}}{\\*\\htmltag19 <html>}\\htmlrtf {\\htmlrtf0 "
            "Caf\\'e9 \\{x\\} \\u8364?\\htmlrtf\\par\\htmlrtf0 {\\*\\htmltag27 </html>}\\htmlrtf }\\htmlrtf0 }");
        CHECK_EQ(wrapped, std::string{"<html>Café {x} €</html>"});
        const auto plain = sc::mime::rtf_to_html("{\\rtf1\\ansi{\\fonttbl{\\f0 Arial;}}{\\colortbl;\\red255\\green0\\blue0;}"
                                                 "\\pard First <line>\\par Second\\line third\\tab end}");
        CHECK(plain.find("<p>First &lt;line&gt;</p>") != std::string::npos);
        CHECK(plain.find("<p>Second<br>\nthird\tend</p>") != std::string::npos);
        CHECK(plain.find("Arial") == std::string::npos); // the font table isn't text
        CHECK_THROWS_AS((void) sc::mime::rtf_to_html("not rtf"), std::runtime_error);
    }

    SECTION("Compressed RTF: stored as it is (MELA)");
    {
        const std::string rtf = "{\\rtf1 x}";
        std::string mela(16, '\0');
        const auto put = [&](const std::size_t at, const std::uint32_t value) {
            for (int i = 0; i < 4; ++i) mela[at + i] = static_cast<char>(value >> (8 * i) & 0xFF);
        };
        put(0, static_cast<std::uint32_t>(rtf.size() + 12));
        put(4, static_cast<std::uint32_t>(rtf.size()));
        put(8, 0x414C454D);
        CHECK_EQ(sc::mime::decompress_rtf(mela + rtf), rtf);
        CHECK_THROWS_AS((void) sc::mime::decompress_rtf("short"), std::runtime_error);
    }

    SECTION("A draft's own date");
    {
        const auto text = sc::mime::draft{}.from({"A", "a@example.com"}).date(sc::datetime::from_unix(1704067200)).text("x").str();
        CHECK_EQ(sc::mime::message::parse(text).date()->as_unix(), 1704067200LL);
    }

    SECTION("An Outlook .msg file");
    if (std::system("python3 --version > /dev/null 2>&1") != 0) {
        std::cout << "   (no python3: the .msg file wasn't made)" << std::endl;
    } else {
        const auto file = std::filesystem::temp_directory_path() / ("sc-mime-" + std::to_string(std::random_device{}()) + ".msg");
        CHECK_EQ(std::system(("python3 " SC_MIME_TEST_RESOURCE "/make_msg.py " + file.string()).c_str()), 0);
        std::ifstream in{file, std::ios::binary};
        std::stringstream bytes;
        bytes << in.rdbuf();
        CHECK(sc::mime::is_outlook(bytes.str()));
        CHECK(!sc::mime::is_outlook("From: a@example.com\n\nx"));

        const auto mail = sc::mime::message::load(file);
        CHECK_EQ(mail.subject(), std::string{"Résumé review"});
        const auto from = mail.from(), to = mail.to(), cc = mail.cc();
        CHECK_EQ(from.size(), std::size_t{1});
        if (!from.empty()) {
            CHECK_EQ(from[0].name, std::string{"Ann Smith"});
            CHECK_EQ(from[0].address, std::string{"ann@example.com"}); // the SMTP one, not the Exchange one
        }
        CHECK(to.size() == 1 && to[0].address == "carol@example.com");
        CHECK(cc.size() == 1 && cc[0].address == "dave@example.com");
        CHECK(mail.date() && mail.date()->as_unix() == 1704067200LL);
        CHECK_EQ(mail.message_id(), std::string{"m1@example.com"});
        CHECK_EQ(mail.text().value_or(""), std::string{"Hello é world, as text.\n"});
        CHECK_EQ(mail.html().value_or(""), std::string{"<html><body><p>Hello é world, hello world, hello world</p></body></html>"});

        const auto files = mail.attachments();
        CHECK_EQ(files.size(), std::size_t{4});
        bool notes = false, logo = false, big = false, inner = false;
        for (const auto &each: files) {
            const auto name = each.filename().value_or("");
            if (name == "notes.txt") notes = each.type().name() == "text/plain" && each.data() == "Some notes.\n";
            if (name == "logo.png") logo = each.content_id() == "logo@example.com" && each.disposition() == sc::mime::disposition::inline_ && each.data().starts_with("\x89PNG");
            if (name == "big.bin") {
                const auto data = each.data();
                big = data.size() == 5000 && static_cast<unsigned char>(data[4999]) == 4999 % 251;
            }
            if (const auto earlier = each.message()) inner = earlier->subject() == "Inner message" && earlier->text().value_or("") == "The forwarded text.\n";
        }
        CHECK(notes);
        CHECK(logo);
        CHECK(big);
        CHECK(inner);

        // Damaged: refused, not read past the end.
        CHECK_THROWS_AS((void) sc::mime::outlook_to_mime(bytes.str().substr(0, 1500)), std::runtime_error);
        std::filesystem::remove(file);
    }
    TEST_SUMMARY();
}
