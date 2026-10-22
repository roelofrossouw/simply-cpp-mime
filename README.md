# simply-cpp-mime

`sc-mime` reads mail messages (MIME: RFC 5322, 2045-2049, 2183, 2231) and hands
you what's in them, decoded: the sender and recipients, the subject and date,
the body text in UTF-8, and the attachments' bytes; and writes them (`draft`: addresses,
subject, headers, text and HTML, attachments). [GMime](https://github.com/jstedfast/gmime)
does the parsing and writing, and none of GMime or GLib shows in the API.

**Documentation:** the [simply-cpp wiki](https://github.com/roelofrossouw/simply-cpp/wiki) has a [Mail guide](https://github.com/roelofrossouw/simply-cpp/wiki/Mail) and the [sc-mime reference](https://github.com/roelofrossouw/simply-cpp/wiki/Reference-sc-mime).

```cpp
#include <sc_mime.h>

const auto mail = sc::mime::message::load("invoice.eml");   // or message::parse(text)
mail.subject();                       // "The résumé", encoded words decoded
for (const auto &who: mail.from()) who.name, who.address;   // also sender, reply_to, to, cc, bcc
mail.date();                          // std::optional<sc::datetime>
mail.message_id();                    // without <>
mail.header("X-Mailer");              // std::optional<std::string>, any case

const std::optional<std::string> text = mail.text();  // UTF-8, "\n" line ends
const std::optional<std::string> html = mail.html();

for (const auto &file: mail.attachments()) {
    file.filename();                  // "résumé.pdf" (RFC 2231 decoded), or nullopt
    file.type().name();               // "application/pdf"
    file.is("image");                 // also "image/png", "image/*"
    file.content_id();                // what the HTML's "cid:" links name
    const std::string bytes = file.data();          // base64 or quoted-printable undone
    if (const auto earlier = file.message()) earlier->subject();  // a forwarded message
}

const auto top = mail.body();         // the part tree
for (const auto &each: top.parts()) each.is_multipart(), each.disposition(), each.headers();
```

| Type | | |
|---|---|---|
| `sc::mime::message` | `parse()`, `load()`; `subject()`, `from()` ... `bcc()`, `date()`, `message_id()`, `header()`, `headers()`; `body()`, `text()`, `html()`, `attachments()`, `leaves()` | a message |
| `sc::mime::part` | `type()`, `is()`, `disposition()`, `filename()`, `content_id()`, `transfer_encoding()`, `header()`, `headers()`; `data()`, `text()`; `parts()`, `message()`, `is_attachment()` | one part |
| `sc::mime::content_type` | `type`, `subtype`, `parameters`; `name()`, `is()`, `parameter()`, `str()` | "text/plain; charset=utf-8" |
| `sc::mime::mailbox` | `name`, `address`; `str()` | "Ann <ann@example.com>" |
| `sc::mime::header` | `name`, `value` | unfolded, decoded |
| `sc::mime::disposition` | `none`, `inline_`, `attachment` | Content-Disposition |
| `sc::mime::draft` | `from()`, `to()`, `cc()`, `bcc()`, `reply_to()`, `subject()`, `header()`, `text()`, `html()`, `attach()`; `str()` | a message to write |
| `sc::mime::transfer_encoding` | `none`, `seven_bit`, `eight_bit`, `binary`, `base64`, `quoted_printable`, `uuencode` | Content-Transfer-Encoding |

- **Body text:** `text()` is the text/plain alternative of a multipart/alternative;
  when a mixed multipart holds several text pieces with pictures between them
  (Apple Mail does this), they're joined. `html()` is found the same way, inside
  multipart/related too. Text attachments don't count. Text comes in UTF-8,
  converted from its charset, with `\n` line ends.
- **Attachments** are every part that's a file rather than body text: those
  marked attachment, any part with a file name, and whatever isn't text/plain
  or text/html (pictures shown in the HTML, signatures, attached messages).
  `data()` is the bytes to save; an attached message is one part whose
  `message()` opens it.
- **Headers:** values come unfolded and decoded to UTF-8 (RFC 2047). The
  Content- headers belong to the part they describe: `body().header("Content-Type")`.
- Parts keep their message alive: `message::load(f).attachments()` is fine.
- `parse()` and `load()` throw `std::runtime_error` when there's no message to
  read (no headers); GMime takes most of what's found in real mailboxes.
- A message and its parts are read only; use each from one thread at a time.
- **Writing:** `sc::mime::draft{}.from(...).to(...).subject(...).text(...).html(...).str()`
  makes a message for `sendmail -t` (or `str(true)` for SMTP's CRLF): text and HTML as
  multipart/alternative, attachments base64 in multipart/mixed, UTF-8 subjects and names
  encoded, Date set. `sc-mime-demo --reply` writes a reply this way.
- Not yet: mbox files, S/MIME and PGP (GMime can do all three).

## Building

GMime 3 comes from the system: `libgmime-3.0-dev` on Ubuntu (jammy, noble,
resolute), `gmime` in Homebrew; CMake installs it when it's missing.
`scripts/install.sh` builds, tests and installs the module; `sc-mime-demo`
shows a message, or saves its attachments with `--extract FOLDER`.

## License

Apache License 2.0 (see `LICENSE` and `NOTICE`). GMime and GLib are
LGPL-2.1-or-later and linked dynamically from the system, so nothing of theirs is
in this module.
