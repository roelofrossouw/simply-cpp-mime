// A message with every kind of part: alternatives (plain and HTML), a picture the HTML shows by
// Content-ID, a PDF with an RFC 2231 file name, an attached message and a text file; which of them
// are body text and which are files, their bytes, and parts outliving the message they came from.
#include "sc_mime.h"
#include "sc_test.h"

#include <string>

namespace {
    const std::string everything = R"(From: ann@example.com
To: bob@example.com
Subject: Everything
MIME-Version: 1.0
Content-Type: multipart/mixed; boundary="outer"

This is a multi-part message in MIME format.

--outer
Content-Type: multipart/alternative; boundary="alt"

--alt
Content-Type: text/plain; charset=utf-8
Content-Transfer-Encoding: quoted-printable

Hello Bob,=0A=0Athe r=C3=A9sum=C3=A9 is attached.
--alt
Content-Type: multipart/related; boundary="rel"

--rel
Content-Type: text/html; charset=utf-8

<p>Hello Bob,</p><img src="cid:logo@example.com">
--rel
Content-Type: image/png; name="logo.png"
Content-Transfer-Encoding: base64
Content-ID: <logo@example.com>
Content-Disposition: inline

iVBORw0KGgoAAf8=
--rel--
--alt--

--outer
Content-Type: application/pdf
Content-Transfer-Encoding: base64
Content-Disposition: attachment; filename*=UTF-8''r%C3%A9sum%C3%A9.pdf

JVBERi0xLjQKJeLjz9MKZmFrZQo=
--outer
Content-Type: message/rfc822

From: carol@example.com
Subject: The earlier one

Forwarded text.

--outer
Content-Type: text/plain; charset=us-ascii
Content-Disposition: attachment; filename="notes.txt"

Not body text.
--outer--
)";

    // Apple Mail's way: text, a picture shown in it, more text, all in a mixed multipart.
    const std::string pieces = R"(From: ann@example.com
Content-Type: multipart/mixed; boundary="b"

--b
Content-Type: text/plain

Before the picture.
--b
Content-Type: image/jpeg
Content-Disposition: inline
Content-Transfer-Encoding: base64

/9j/
--b
Content-Type: text/plain

After it.
--b--
)";
}

int main() {
    using std::string;

    SECTION("Body text: the plain alternative, the HTML one from inside multipart/related");
    {
        const auto mail = sc::mime::message::parse(everything);
        CHECK_EQ(mail.text().value_or(""), string{"Hello Bob,\n\nthe résumé is attached."});
        const auto html = mail.html().value_or("");
        CHECK(html.find("<img src=\"cid:logo@example.com\">") != string::npos);
        CHECK(html.find("Not body text") == string::npos);
    }

    SECTION("Attachments, in order: the picture, the PDF, the attached message, the text file");
    {
        const auto mail = sc::mime::message::parse(everything);
        const auto files = mail.attachments();
        CHECK_EQ(files.size(), std::size_t{4});
        if (files.size() == 4) {
            const auto &logo = files[0];
            CHECK(logo.is("image/png"));
            CHECK(logo.disposition() == sc::mime::disposition::inline_);
            CHECK_EQ(logo.content_id(), string{"logo@example.com"});
            CHECK_EQ(logo.filename().value_or(""), string{"logo.png"});
            CHECK_EQ(logo.data(), string("\x89PNG\r\n\x1a\n\x00\x01\xff", 11));
            CHECK_THROWS_AS(logo.text(), std::logic_error);

            const auto &pdf = files[1];
            CHECK(pdf.is("application/pdf"));
            CHECK(pdf.disposition() == sc::mime::disposition::attachment);
            CHECK_EQ(pdf.filename().value_or(""), string{"résumé.pdf"});
            CHECK(pdf.transfer_encoding() == sc::mime::transfer_encoding::base64);
            CHECK_EQ(pdf.data(), string{"%PDF-1.4\n%\xe2\xe3\xcf\xd3\nfake\n"});

            const auto &earlier = files[2];
            CHECK(earlier.is_message());
            CHECK(!earlier.filename().has_value());
            const auto inner = earlier.message();
            CHECK(inner.has_value());
            if (inner) {
                CHECK_EQ(inner->subject(), string{"The earlier one"});
                const auto from = inner->from(); // a copy: CHECK_EQ keeps a reference
                CHECK_EQ(from.size(), std::size_t{1});
                if (!from.empty()) CHECK_EQ(from[0].address, string{"carol@example.com"});
                CHECK_EQ(inner->text().value_or(""), string{"Forwarded text.\n"});
            }
            CHECK(earlier.data().find("Subject: The earlier one") != string::npos);

            const auto &notes = files[3];
            CHECK_EQ(notes.filename().value_or(""), string{"notes.txt"});
            CHECK_EQ(notes.text(), string{"Not body text."} /* the line end before a boundary is the boundary's */);
        }
    }

    SECTION("The tree: multiparts hold parts, leaves skip them");
    {
        const auto mail = sc::mime::message::parse(everything);
        const auto top = mail.body();
        CHECK(top.is_multipart());
        CHECK(top.is("multipart/mixed"));
        CHECK(!top.is_attachment());
        CHECK_EQ(top.type().parameter("boundary").value_or(""), string{"outer"});
        const auto parts = top.parts();
        CHECK_EQ(parts.size(), std::size_t{4});
        if (!parts.empty()) {
            CHECK(parts[0].is("multipart/alternative"));
            CHECK_EQ(parts[0].parts().size(), std::size_t{2});
        }
        CHECK_EQ(mail.leaves().size(), std::size_t{6});
        const auto first = mail.leaves().front();
        CHECK(first.parts().empty());
        CHECK(!first.message().has_value());
    }

    SECTION("Parts keep their message alive");
    {
        const auto files = sc::mime::message::parse(everything).attachments();
        CHECK_EQ(files.size(), std::size_t{4});
        if (files.size() == 4) {
            CHECK_EQ(files[1].filename().value_or(""), string{"résumé.pdf"});
            CHECK_EQ(files[2].message()->subject(), string{"The earlier one"});
        }
    }

    SECTION("Several text pieces in a mixed multipart are joined; the picture between them is a file");
    {
        const auto mail = sc::mime::message::parse(pieces);
        CHECK_EQ(mail.text().value_or(""), string{"Before the picture.\nAfter it."});
        const auto files = mail.attachments();
        CHECK_EQ(files.size(), std::size_t{1});
        if (!files.empty()) {
            CHECK(files[0].is("image/jpeg"));
            CHECK_EQ(files[0].data(), string{"\xff\xd8\xff"});
        }
    }

    TEST_SUMMARY();
}
