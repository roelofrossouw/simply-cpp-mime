#ifndef SC_MIME_H
#define SC_MIME_H
// MIME mail messages (RFC 5322, 2045-2049, 2183, 2231): read one, from a file or from memory,
// and get its headers, addresses, date, body text and attachments, every part of it decoded; or
// write one (draft): addresses, subject, headers, a text and an HTML body, attachments.
// GMime does the parsing and the writing; nothing of GMime or GLib shows here.
#include <datetime.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct _GMimeObject;
struct _GMimeMessage;

namespace sc::mime {
    namespace detail {
        struct document;
    }

    class message;

    // A media type, "text/plain; charset=utf-8": its type, subtype and parameters, names
    // lowercased (values as they came).
    struct content_type {
        std::string type = "text";
        std::string subtype = "plain";
        std::vector<std::pair<std::string, std::string>> parameters;

        // "text/plain"
        [[nodiscard]] std::string name() const;

        // Whether it's "text/plain", any "text" (or "text/*"), ignoring case.
        [[nodiscard]] bool is(std::string_view media) const;

        // A parameter by name, ignoring case ("charset", "boundary", "name").
        [[nodiscard]] std::optional<std::string> parameter(std::string_view name) const;

        // "text/plain; charset=utf-8"
        [[nodiscard]] std::string str() const;
    };

    // How a part wants to be shown (Content-Disposition): in the text or as a file to save; none
    // when the part doesn't say.
    enum class disposition { none, inline_, attachment };

    // How the part's bytes were encoded for the trip (Content-Transfer-Encoding).
    enum class transfer_encoding { none, seven_bit, eight_bit, binary, base64, quoted_printable, uuencode };

    // One address: "Ann Smith <ann@example.com>". Groups ("Team: a@x, b@y;") come as their members.
    struct mailbox {
        std::string name;    // decoded, UTF-8; empty when there's none
        std::string address; // "ann@example.com"

        // "Ann Smith <ann@example.com>", or the address alone.
        [[nodiscard]] std::string str() const;

        friend bool operator==(const mailbox &, const mailbox &) = default;
    };

    // A header as it reads: the name as written, the value unfolded and decoded to UTF-8 (RFC 2047).
    struct header {
        std::string name;
        std::string value;
    };

    // A part of a message: a leaf (text, a picture, a PDF), a multipart holding parts, or an
    // attached message. It keeps its message alive, so it can outlive the message object it came
    // from.
    class part {
    public:
        [[nodiscard]] content_type type() const;

        // Whether its type is media (see content_type::is).
        [[nodiscard]] bool is(std::string_view media) const { return type().is(media); }

        [[nodiscard]] mime::disposition disposition() const;

        // The file name it was sent with (Content-Disposition's filename, else Content-Type's name),
        // decoded (RFC 2231, 2047); nullopt when it has none.
        [[nodiscard]] std::optional<std::string> filename() const;

        // Content-ID without its <>, as an HTML body's "cid:" links name it; empty when there's none.
        [[nodiscard]] std::string content_id() const;

        [[nodiscard]] mime::transfer_encoding transfer_encoding() const;

        // A header by name, ignoring case; nullopt when absent.
        [[nodiscard]] std::optional<std::string> header(std::string_view name) const;

        [[nodiscard]] std::vector<mime::header> headers() const;

        // The content, its transfer encoding undone: a picture's or a PDF's bytes, text in its own
        // charset. An attached message comes as the whole message's text; a multipart as it was
        // sent.
        [[nodiscard]] std::string data() const;

        // A text part's content in UTF-8, converted from its charset, with line ends as "\n".
        // Throws std::logic_error for a part that isn't text.
        [[nodiscard]] std::string text() const;

        [[nodiscard]] bool is_multipart() const;

        // A multipart's parts, in order; empty for anything else.
        [[nodiscard]] std::vector<part> parts() const;

        // Whether it's an attached message (message/rfc822), and that message.
        [[nodiscard]] bool is_message() const;

        [[nodiscard]] std::optional<mime::message> message() const;

        // Whether it's a file rather than body text: an attachment, a part with a file name, or
        // anything that isn't text/plain or text/html (pictures, signatures, attached messages).
        // A multipart is neither.
        [[nodiscard]] bool is_attachment() const;

    private:
        friend class mime::message;

        part(std::shared_ptr<const detail::document> document, _GMimeObject *object);

        std::shared_ptr<const detail::document> document_;
        _GMimeObject *object_;
    };

    // A mail message. Copies share it; it's read only, and one thread at a time should use it.
    class message {
    public:
        // Reads a message from its text (an .eml file's content). GMime takes most of what
        // turns up in mailboxes; throws std::runtime_error when there's no message to find.
        static message parse(std::string_view text);

        // Reads an .eml file; throws std::runtime_error when it can't.
        static message load(const std::filesystem::path &file);

        // Decoded, UTF-8.
        [[nodiscard]] std::string subject() const;

        [[nodiscard]] std::vector<mailbox> from() const;

        [[nodiscard]] std::vector<mailbox> sender() const;

        [[nodiscard]] std::vector<mailbox> reply_to() const;

        [[nodiscard]] std::vector<mailbox> to() const;

        [[nodiscard]] std::vector<mailbox> cc() const;

        [[nodiscard]] std::vector<mailbox> bcc() const;

        // When it was written (Date:); nullopt when missing or unreadable.
        [[nodiscard]] std::optional<sc::datetime> date() const;

        // Message-ID without its <>; empty when there's none.
        [[nodiscard]] std::string message_id() const;

        // A header by name, ignoring case; nullopt when absent. The Content- headers (type,
        // transfer encoding, disposition) are body()'s.
        [[nodiscard]] std::optional<std::string> header(std::string_view name) const;

        // Every header but the Content- ones, in order, repeats included.
        [[nodiscard]] std::vector<mime::header> headers() const;

        // The top part: a single text, or the multipart holding the rest.
        [[nodiscard]] part body() const;

        // The plain text body (UTF-8, "\n" line ends): the text/plain alternative, and every
        // text/plain shown in the text joined when there are several; nullopt when there's none.
        [[nodiscard]] std::optional<std::string> text() const;

        // The HTML body, chosen the same way; nullopt when there's none.
        [[nodiscard]] std::optional<std::string> html() const;

        // The files, in order (see part::is_attachment): those attached, and pictures shown in the
        // HTML (their content_id() is what it links to). Attached messages are one part each;
        // their own parts are in message().
        [[nodiscard]] std::vector<part> attachments() const;

        // Every part but the multiparts, in order, depth first; attached messages aren't opened.
        [[nodiscard]] std::vector<part> leaves() const;

    private:
        friend class part;

        message(std::shared_ptr<const detail::document> document, _GMimeMessage *gmime);

        std::shared_ptr<const detail::document> document_;
        _GMimeMessage *message_;
    };

    // A message to send, built up and written as text: for "sendmail -t", an SMTP client or a file.
    // Names and the subject in UTF-8 are encoded as they must be (RFC 2047); text goes as UTF-8,
    // quoted-printable when it needs to; attachments go base64. With both text() and html() the
    // body is multipart/alternative (mail programs show the HTML, or the text when they can't);
    // with attachments, multipart/mixed around it. Date is set when it's written.
    //
    //     const auto mail = sc::mime::draft{}
    //         .from({"IDs", "id@example.com"})
    //         .to({"Ann Smith", "ann@example.com"})
    //         .subject("Re: Our IDs")
    //         .header("In-Reply-To", "<m1@example.com>")
    //         .text("Thanks, 2 identities were loaded.")
    //         .html("<p>Thanks, <b>2</b> identities were loaded.</p>")
    //         .str();
    class draft {
    public:
        draft &from(const mailbox &who);
        // Each call adds one.
        draft &to(const mailbox &who);
        draft &cc(const mailbox &who);
        draft &bcc(const mailbox &who);
        draft &reply_to(const mailbox &who);
        draft &subject(std::string_view text);

        // Any other header, as given (In-Reply-To, References, Auto-Submitted, X-...): a value in
        // UTF-8 that isn't plain ASCII is encoded. Each call adds one; the same name may repeat.
        draft &header(std::string_view name, std::string_view value);

        // The body: plain text, HTML, or both (multipart/alternative). UTF-8, "\n" line ends.
        draft &text(std::string_view utf8);
        draft &html(std::string_view utf8);

        // A file, after the body (multipart/mixed). type: "application/pdf"; inline_: shown in the
        // HTML rather than offered to save, content_id the HTML's "cid:" link names.
        draft &attach(std::string_view filename, std::string_view type, std::string bytes, bool inline_ = false,
                      std::string_view content_id = {});

        // The message as it's sent: headers, a blank line, the body. Line ends "\n" (what sendmail
        // reads), or "\r\n" as SMTP sends them.
        [[nodiscard]] std::string str(bool crlf = false) const;

    private:
        struct file {
            std::string filename, type, bytes, content_id;
            bool inline_ = false;
        };
        std::vector<std::pair<std::string, mailbox>> addresses_; // "from", "to", "cc", "bcc", "reply-to"
        std::optional<std::string> subject_;
        std::vector<mime::header> headers_;
        std::optional<std::string> text_, html_;
        std::vector<file> files_;
    };
}

#endif
