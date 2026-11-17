// A message's headers: encoded words (RFC 2047) in the subject and names, address lists with a
// group, the date, Message-ID, repeated headers, a quoted-printable Latin-1 body read as UTF-8, CRLF
// line ends, and what happens with files that aren't there.
#include <sc/mime/sc_mime.h>
#include "sc_test.h"

#include <filesystem>
#include <fstream>
#include <string>

namespace {
    // Mail goes over the wire with CRLF line ends.
    std::string crlf(const std::string &text) {
        std::string out;
        for (const char c: text) {
            if (c == '\n') out += '\r';
            out += c;
        }
        return out;
    }

    const std::string headers_and_text = crlf(R"(From: =?UTF-8?Q?Ren=C3=A9_Botha?= <rene@example.com>
To: Ann <ann@example.com>, bob@example.com
Cc: Team: carol@example.com, Dave <dave@example.com>;
Reply-To: help@example.com
Subject: =?ISO-8859-1?Q?Caf=E9?= and
 =?UTF-8?B?4pyT?=
Date: Wed, 22 Oct 2026 14:30:00 +0200
Message-ID: <abc.123@example.com>
X-Custom: one
X-Custom: two
MIME-Version: 1.0
Content-Type: text/plain; charset=iso-8859-1
Content-Transfer-Encoding: quoted-printable

Caf=E9 au lait,=
 soft break.
Second line.
)");
}

int main() {
    using std::string;

    SECTION("Subject, names and addresses decoded; groups come as their members");
    {
        const auto mail = sc::mime::message::parse(headers_and_text);
        CHECK_EQ(mail.subject(), string{"Café and ✓"});
        const auto from = mail.from();
        CHECK_EQ(from.size(), std::size_t{1});
        if (!from.empty()) {
            CHECK_EQ(from[0].name, string{"René Botha"});
            CHECK_EQ(from[0].address, string{"rene@example.com"});
            CHECK_EQ(from[0].str(), string{"René Botha <rene@example.com>"});
        }
        const auto to = mail.to();
        CHECK_EQ(to.size(), std::size_t{2});
        if (to.size() == 2) {
            CHECK((to[0] == sc::mime::mailbox{"Ann", "ann@example.com"}));
            CHECK((to[1] == sc::mime::mailbox{"", "bob@example.com"}));
            CHECK_EQ(to[1].str(), string{"bob@example.com"});
        }
        const auto cc = mail.cc();
        CHECK_EQ(cc.size(), std::size_t{2});
        if (cc.size() == 2) {
            CHECK_EQ(cc[0].address, string{"carol@example.com"});
            CHECK_EQ(cc[1].name, string{"Dave"});
        }
        const auto reply_to = mail.reply_to();
        CHECK_EQ(reply_to.size(), std::size_t{1});
        CHECK(mail.bcc().empty());
        CHECK(mail.sender().empty());
    }

    SECTION("Date, Message-ID, headers by name and in order");
    {
        const auto mail = sc::mime::message::parse(headers_and_text);
        const auto date = mail.date();
        CHECK(date.has_value());
        if (date) {
            const long long when = date->as_unix(); // a copy: CHECK_EQ keeps a reference
            CHECK_EQ(when, 1792672200LL);
        }
        CHECK_EQ(mail.message_id(), string{"abc.123@example.com"});
        CHECK_EQ(mail.header("x-custom").value_or(""), string{"one"});
        CHECK(!mail.header("X-Missing").has_value());
        const auto all = mail.headers();
        CHECK_EQ(all.size(), std::size_t{10}); // the Content- headers are the body's
        CHECK(!mail.header("Content-Type").has_value());
        CHECK_EQ(mail.body().header("content-type").value_or(""), string{"text/plain; charset=iso-8859-1"});
        int custom = 0;
        for (const auto &each: all) custom += each.name == "X-Custom";
        CHECK_EQ(custom, 2);
        CHECK_EQ(mail.header("Subject").value_or(""), string{"Café and ✓"});
    }

    SECTION("A one-part message: its text in UTF-8 with \\n line ends, no attachments");
    {
        const auto mail = sc::mime::message::parse(headers_and_text);
        CHECK_EQ(mail.text().value_or("(none)"), string{"Café au lait, soft break.\nSecond line.\n"});
        CHECK(!mail.html().has_value());
        CHECK(mail.attachments().empty());
        const auto body = mail.body();
        CHECK(body.is("text/plain"));
        CHECK(body.transfer_encoding() == sc::mime::transfer_encoding::quoted_printable);
        CHECK_EQ(body.type().parameter("CHARSET").value_or(""), string{"iso-8859-1"});
        CHECK_EQ(body.data(), string{"Caf\xe9 au lait, soft break.\r\nSecond line.\r\n"}); // as sent, encoding undone
        CHECK_EQ(mail.leaves().size(), std::size_t{1});
    }

    SECTION("No Date, no Message-ID, no Content-Type: text/plain in US-ASCII");
    {
        const auto mail = sc::mime::message::parse("Subject: plain\n\nHello\n");
        CHECK(!mail.date().has_value());
        CHECK(mail.message_id().empty());
        CHECK(mail.from().empty());
        CHECK_EQ(mail.text().value_or(""), string{"Hello\n"});
        CHECK_EQ(mail.body().type().name(), string{"text/plain"});
    }

    SECTION("load(): a file, and one that isn't there");
    {
        const auto file = std::filesystem::temp_directory_path() / "sc-mime-test.eml";
        {
            std::ofstream out{file, std::ios::binary};
            out << headers_and_text;
        }
        const auto mail = sc::mime::message::load(file);
        CHECK_EQ(mail.message_id(), string{"abc.123@example.com"});
        std::filesystem::remove(file);
        CHECK_THROWS_AS(sc::mime::message::load(file), std::runtime_error);
    }

    SECTION("Not a message: nothing, or no headers");
    {
        CHECK_THROWS_AS(sc::mime::message::parse(""), std::runtime_error);
        CHECK_THROWS_AS(sc::mime::message::parse("just some text\nwithout headers\n"), std::runtime_error);
    }

    SECTION("raw_header(): as written, folds joined; in_reply_to() and references() without <>");
    {
        const auto mail = sc::mime::message::parse(headers_and_text);
        CHECK_EQ(mail.raw_header("subject").value_or("(none)"), string{"=?ISO-8859-1?Q?Caf=E9?= and =?UTF-8?B?4pyT?="});
        CHECK_EQ(mail.raw_header("From").value_or("(none)"), string{"=?UTF-8?Q?Ren=C3=A9_Botha?= <rene@example.com>"});
        CHECK(!mail.raw_header("Thread-Index").has_value());
        CHECK(mail.in_reply_to().empty());
        CHECK(mail.references().empty());
        const auto reply = sc::mime::message::parse(crlf("From: a@example.com\nTo: \"Smith, J\" <j@example.com>,\n\tk@example.com\n"
                                                         "In-Reply-To: <omq1f2e.3@example.com> <other@example.com>\n"
                                                         "References: <first@example.com>\n <omq1f2e.3@example.com>\n"
                                                         "Subject: =?utf-8?Q?Caf=C3=A9?=\n\t=?utf-8?Q?_bond?=\n"
                                                         "Message-ID: <r@example.com>\n\nThanks\n"));
        CHECK_EQ(reply.raw_header("To").value_or("(none)"), string{"\"Smith, J\" <j@example.com>, k@example.com"}); // a fold is one space
        CHECK_EQ(reply.raw_header("Subject").value_or("(none)"), string{"=?utf-8?Q?Caf=C3=A9?= =?utf-8?Q?_bond?="});
        CHECK_EQ(reply.raw_header("References").value_or("(none)"), string{"<first@example.com> <omq1f2e.3@example.com>"});
        CHECK_EQ(reply.in_reply_to(), string{"omq1f2e.3@example.com"});
        const auto references = reply.references();
        CHECK_EQ(references.size(), std::size_t{2});
        if (references.size() == 2) {
            CHECK_EQ(references[0], string{"first@example.com"});
            CHECK_EQ(references[1], string{"omq1f2e.3@example.com"});
        }
    }

    SECTION("text() is always valid UTF-8: Windows-1252 under a Latin-1 label, an unknown charset or none");
    {
        const string body = "Caf\xe9 \x93quoted\x94 \x80 10\n";
        const string expected = "Café “quoted” € 10\n";
        for (const string type: {"text/plain; charset=iso-8859-1", "text/plain; charset=windows-1252", "text/plain; charset=x-unknown", "text/plain"}) {
            const auto mail = sc::mime::message::parse("From: a@example.com\nMIME-Version: 1.0\nContent-Type: " + type +
                                                       "\nContent-Transfer-Encoding: 8bit\n\n" + body);
            CHECK_EQ(mail.text().value_or("(none)"), expected);
        }
        // UTF-8 stays as it is, with a stray byte read as Windows-1252
        const auto mixed = sc::mime::message::parse("From: a@example.com\nMIME-Version: 1.0\nContent-Type: text/plain; charset=utf-8\n"
                                                    "Content-Transfer-Encoding: 8bit\n\n\xe2\x9c\x93 ok \x96 dash\n");
        CHECK_EQ(mixed.text().value_or("(none)"), string{"✓ ok – dash\n"});
    }

    SECTION("content_type: is(), parameter(), str() quoting what needs it");
    {
        const sc::mime::content_type type{"text", "plain", {{"charset", "utf-8"}, {"name", "my notes.txt"}}};
        CHECK(type.is("text"));
        CHECK(type.is("TEXT/*"));
        CHECK(type.is("text/Plain"));
        CHECK(!type.is("text/html"));
        CHECK(!type.is("image"));
        CHECK_EQ(type.name(), string{"text/plain"});
        CHECK_EQ(type.str(), string{"text/plain; charset=utf-8; name=\"my notes.txt\""});
        CHECK(!type.parameter("format").has_value());
    }

    TEST_SUMMARY();
}
