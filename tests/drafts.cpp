// Writing a message (draft) and reading it back: addresses with UTF-8 names, an encoded subject,
// extra headers kept as given, text and HTML as multipart/alternative, attachments (one inline with
// a Content-ID) in multipart/mixed, the date set, and CRLF line ends on request.
#include "sc_mime.h"
#include "sc_test.h"

#include <string>

int main() {
    SECTION("Text and HTML, attachments, headers: everything reads back");
    {
        const std::string pdf = std::string{"%PDF-1.4\n"} + std::string(300, '\x01') + "\xff\xfe";
        const auto text = sc::mime::draft{}
                              .from({"René Botha", "rene@example.com"})
                              .to({"Ann", "ann@example.com"})
                              .to({"", "bob@example.com"})
                              .cc({"Carol", "carol@example.com"})
                              .reply_to({"", "help@example.com"})
                              .subject("Re: Café ✓")
                              .header("In-Reply-To", "<m1@example.com>")
                              .header("References", "<m0@example.com> <m1@example.com>")
                              .header("Auto-Submitted", "auto-replied")
                              .text("Thanks, 2 identities.\nSecond line, é.\n")
                              .html("<p>Thanks, <b>2</b> identities, é.</p>\n")
                              .attach("scan.pdf", "application/pdf", pdf)
                              .attach("logo.png", "image/png", "PNG", true, "logo@example.com")
                              .str();
        CHECK(text.find('\r') == std::string::npos);
        CHECK(text.find("Subject: Re: =?UTF-8?") != std::string::npos); // encoded, not raw UTF-8
        CHECK(text.find("Content-Transfer-Encoding: quoted-printable") != std::string::npos); // mostly ASCII text
        const auto mail = sc::mime::message::parse(text);
        CHECK_EQ(mail.subject(), std::string{"Re: Café ✓"});
        CHECK(mail.from() == (std::vector<sc::mime::mailbox>{{"René Botha", "rene@example.com"}}));
        CHECK(mail.to() == (std::vector<sc::mime::mailbox>{{"Ann", "ann@example.com"}, {"", "bob@example.com"}}));
        CHECK(mail.cc() == (std::vector<sc::mime::mailbox>{{"Carol", "carol@example.com"}}));
        CHECK(mail.reply_to() == (std::vector<sc::mime::mailbox>{{"", "help@example.com"}}));
        CHECK(mail.header("In-Reply-To") == std::optional<std::string>{"<m1@example.com>"});
        CHECK(mail.header("References") == std::optional<std::string>{"<m0@example.com> <m1@example.com>"});
        CHECK(mail.header("Auto-Submitted") == std::optional<std::string>{"auto-replied"});
        CHECK(mail.date().has_value());
        CHECK(mail.text() == std::optional<std::string>{"Thanks, 2 identities.\nSecond line, é.\n"});
        CHECK(mail.html() == std::optional<std::string>{"<p>Thanks, <b>2</b> identities, é.</p>\n"});
        // mixed { related { alternative { text, html }, logo }, scan.pdf }
        CHECK(mail.body().is("multipart/mixed"));
        CHECK(mail.body().parts().at(0).is("multipart/related"));
        CHECK(mail.body().parts().at(0).parts().at(0).is("multipart/alternative"));
        CHECK(mail.body().parts().at(1).is("application/pdf"));
        const auto files = mail.attachments();
        CHECK_EQ(files.size(), std::size_t{2});
        if (files.size() == 2) {
            CHECK(files[0].disposition() == sc::mime::disposition::inline_);
            CHECK_EQ(files[0].content_id(), std::string{"logo@example.com"});
            CHECK_EQ(files[0].data(), std::string{"PNG"});
            CHECK(files[1].filename() == std::optional<std::string>{"scan.pdf"});
            CHECK(files[1].is("application/pdf"));
            CHECK(files[1].disposition() == sc::mime::disposition::attachment);
            CHECK(files[1].data() == pdf);
            CHECK(files[1].transfer_encoding() == sc::mime::transfer_encoding::base64);
        }
    }

    SECTION("Text only: a single part; ASCII stays 7-bit; CRLF on request");
    {
        const auto text = sc::mime::draft{}.to({"", "a@example.com"}).subject("Plain").text("Hello\nthere\n").str(true);
        CHECK(text.find("\r\n") != std::string::npos);
        CHECK(text.find("Subject: Plain\r\n") != std::string::npos);
        const auto mail = sc::mime::message::parse(text);
        CHECK(mail.body().is("text/plain"));
        CHECK(mail.text() == std::optional<std::string>{"Hello\nthere\n"});
        CHECK(mail.attachments().empty());
        CHECK(mail.body().transfer_encoding() != sc::mime::transfer_encoding::base64);
    }

    SECTION("Only pictures for the HTML: multipart/related, no mixed");
    {
        const auto mail = sc::mime::message::parse(sc::mime::draft{}
                                                       .to({"", "a@example.com"})
                                                       .text("x")
                                                       .html("<img src=\"cid:p1@x\">")
                                                       .attach("p1.jpg", "image/jpeg", "JPEG", true, "p1@x")
                                                       .str());
        CHECK(mail.body().is("multipart/related"));
        CHECK(mail.html() == std::optional<std::string>{"<img src=\"cid:p1@x\">"});
        CHECK(mail.text() == std::optional<std::string>{"x"});
        CHECK_EQ(mail.attachments().size(), std::size_t{1});
    }

    SECTION("HTML only, and an empty draft still makes a message");
    {
        const auto html = sc::mime::message::parse(sc::mime::draft{}.to({"", "a@example.com"}).html("<p>x</p>").str());
        CHECK(html.body().is("text/html"));
        CHECK(!html.text().has_value());
        const auto empty = sc::mime::message::parse(sc::mime::draft{}.to({"", "a@example.com"}).str());
        CHECK(empty.body().is("text/plain"));
    }

    TEST_SUMMARY();
}
