// Shows a mail message (.eml): who it's from and to, the subject and date, the start of its text,
// and its attachments; --extract saves the attachments into a folder. Without a file, it reads a
// small message of its own.
//
//     sc-mime-demo invoice.eml
//     sc-mime-demo invoice.eml --extract out

#include <sc_mime.h>
#include <params.h>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
    const char *sample = "From: =?UTF-8?Q?Ren=C3=A9?= <rene@example.com>\r\n"
                         "To: Ann <ann@example.com>, bob@example.com\r\n"
                         "Subject: =?UTF-8?Q?The_r=C3=A9sum=C3=A9?=\r\n"
                         "Date: Wed, 22 Oct 2026 14:30:00 +0200\r\n"
                         "MIME-Version: 1.0\r\n"
                         "Content-Type: multipart/mixed; boundary=\"b\"\r\n"
                         "\r\n"
                         "--b\r\n"
                         "Content-Type: text/plain; charset=utf-8\r\n"
                         "\r\n"
                         "Hello Ann,\r\n\r\nhere it is.\r\n"
                         "--b\r\n"
                         "Content-Type: text/plain\r\n"
                         "Content-Disposition: attachment; filename*=UTF-8''r%C3%A9sum%C3%A9.txt\r\n"
                         "Content-Transfer-Encoding: base64\r\n"
                         "\r\n"
                         "SGlyZSBtZSE=\r\n"
                         "--b--\r\n";

    std::string list(const std::vector<sc::mime::mailbox> &mailboxes) {
        std::string out;
        for (const auto &each: mailboxes) out += (out.empty() ? "" : ", ") + each.str();
        return out;
    }
}

int main(int argc, char **argv) {
    const sc::params args{argc, argv, {
        {"name", "sc-mime-demo"},
        {"description", "Shows a mail message (.eml): sender, recipients, subject, date, the start of its text and "
                        "its attachments, or saves the attachments with --extract. Without a file, it reads a small "
                        "message of its own."},
        {"options", {
            {"extract", {{"short", "x"}, {"value", "FOLDER"}, {"default", ""}, {"help", "save the attachments into FOLDER"}}}}},
        {"positional", {
            {"file", {{"default", ""}, {"help", "the message (.eml)"}}}}}}};
    try {
        // [readme]
        const auto file = args.as<std::string>("file");
        const auto mail = file.empty() ? sc::mime::message::parse(sample) : sc::mime::message::load(file);
        std::cout << "From:    " << list(mail.from()) << '\n'
                  << "To:      " << list(mail.to()) << '\n'
                  << "Subject: " << mail.subject() << '\n';
        if (const auto date = mail.date()) std::cout << "Date:    " << date->format() << '\n';
        if (const auto text = mail.text()) {
            std::cout << '\n' << text->substr(0, 500) << (text->size() > 500 ? "...\n" : "\n");
        } else if (mail.html()) {
            std::cout << "\n(HTML only)\n";
        }
        const auto folder = std::filesystem::path{args.as<std::string>("extract")};
        int count = 0;
        for (const auto &each: mail.attachments()) {
            ++count;
            auto name = each.filename().value_or("");
            if (name.empty() && each.is_message()) name = each.message()->subject() + ".eml";
            if (name.empty() || name == ".eml") name = "attachment-" + std::to_string(count) + ".bin";
            const auto bytes = each.data();
            std::cout << "Attachment: " << name << " (" << each.type().name() << ", " << bytes.size() << " bytes)\n";
            if (!folder.empty()) {
                std::filesystem::create_directories(folder);
                // Only the name's last part: a sender's "../../x" stays in the folder.
                std::ofstream{folder / std::filesystem::path{name}.filename(), std::ios::binary} << bytes;
            }
        }
        // [/readme]
    } catch (const std::exception &error) {
        std::cerr << "sc-mime-demo: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
