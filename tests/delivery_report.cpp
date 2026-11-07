// delivery_report against Perl's MIME-tools 5.509 / Mail::Header 2.21 running correspondence.read_dsn's code
// on the bounces in resource/dsn (made up; example.com addresses), recorded 2026-11-07.
#include "sc_mime.h"

#include "sc_test.h"

#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {
    std::map<std::string, std::string> report(const std::string &file) {
        std::ifstream in(std::string{SC_MIME_TEST_RESOURCE} + "/dsn/" + file, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return sc::mime::delivery_report(sc::mime::message::parse(text.str()));
    }
}

int main() {
    SECTION("b64_nestedmulti.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Action", "failed"},
            {"Final-Recipient", "rfc822; a@example.org"},
            {"Message-ID", "<g@example.com>"},
            {"Reporting-Mta", "dns; inner.example.org"},
            {"Status", "4.4.1"},
            {"To", "a@example.org"},
        };
        CHECK((report("b64_nestedmulti.eml") == expected));
    }
    SECTION("consonant_words.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Abc--Def", "5"},
            {"DSN-Gateway", "3"},
            {"WWW-Thing", "4"},
            {"X-MS-Exchange-ID", "2"},
            {"X-SMTP-RCPT", "1"},
        };
        CHECK((report("consonant_words.eml") == expected));
    }
    SECTION("dupes.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Action", "delayed"},
            {"Message-ID", "<u@example.com>"},
            {"Received", "one"},
            {"Status", "4.0.0"},
            {"To", "x@example.com"},
            {"To-Do", "nope"},
            {"Topic", "maybe"},
        };
        CHECK((report("dupes.eml") == expected));
    }
    SECTION("empty_status.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Message-ID", "<m@example.com>"},
            {"Received-SPF", "pass"},
            {"To", "y@example.com"},
        };
        CHECK((report("empty_status.eml") == expected));
    }
    SECTION("global_nested.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Action", "failed"},
            {"Diagnostic-Code", "smtp; 552 Mailbox voll — over quota"},
            {"Final-Recipient", "utf-8; jörg@example.net"},
            {"Reporting-Mta", "dns; mx.example.net"},
            {"Status", "5.2.2"},
        };
        CHECK((report("global_nested.eml") == expected));
    }
    SECTION("no_ct_part.eml");
    {
        const std::map<std::string, std::string> expected{
        };
        CHECK((report("no_ct_part.eml") == expected));
    }
    SECTION("odd_lines.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Action", "failed"},
            {"Diagnostic-Code", "smtp; 550 folded after empty"},
            {"From ", "someone"},
            {"Last-Attempt-Date", "Fri"},
            {"Reporting-Mta", "dns; a"},
            {"X-Spam-Level", "***"},
        };
        CHECK((report("odd_lines.eml") == expected));
    }
    SECTION("plain.eml");
    {
        const std::map<std::string, std::string> expected{
        };
        CHECK((report("plain.eml") == expected));
    }
    SECTION("postfix_crlf.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Arrival-Date", "Fri,  6 Nov 2026 09:59:58 +0200 (SAST)"},
            {"Message-ID", "<abc123@app1.example.com>"},
            {"Received", "from app1.example.com (app1.example.com [192.0.2.10])\n\tby mx.example.com (Postfix) with ESMTP id 1A2B3C\n\tfor <nobody@example.org>; Fri,  6 Nov 2026 09:59:58 +0200 (SAST)"},
            {"Reporting-Mta", "dns; mx.example.com"},
            {"To", "nobody@example.org"},
            {"X-Postfix-Queue-ID", "1A2B3C"},
            {"X-Postfix-Sender", "rfc822; sender@example.com"},
        };
        CHECK((report("postfix_crlf.eml") == expected));
    }
    SECTION("postfix_lf.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Action", "failed"},
            {"Arrival-Date", "Fri,  6 Nov 2026 09:59:58 +0200 (SAST)"},
            {"Diagnostic-Code", "smtp; 550 5.1.1 <nobody@example.org>: Recipient address\n    rejected: User unknown in virtual mailbox table"},
            {"Final-Recipient", "rfc822; nobody@example.org"},
            {"Message-ID", "<abc123@app1.example.com>"},
            {"Original-Recipient", "rfc822;nobody@example.org"},
            {"Received", "from app1.example.com (app1.example.com [192.0.2.10])\n\tby mx.example.com (Postfix) with ESMTP id 1A2B3C\n\tfor <nobody@example.org>; Fri,  6 Nov 2026 09:59:58 +0200 (SAST)"},
            {"Remote-Mta", "dns; mx.example.org"},
            {"Reporting-Mta", "dns; mx.example.com"},
            {"Status", "5.1.1"},
            {"To", "nobody@example.org"},
            {"X-Postfix-Queue-ID", "1A2B3C"},
            {"X-Postfix-Sender", "rfc822; sender@example.com"},
        };
        CHECK((report("postfix_lf.eml") == expected));
    }
    SECTION("qp.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Action", "failed"},
            {"Diagnostic-Code", "smtp; 550 very long line that was soft wrapped= here"},
        };
        CHECK((report("qp.eml") == expected));
    }
    SECTION("single.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Action", "Failed"},
            {"Reporting-Mta", "dns; single.example.com"},
            {"Status", "5.0.0"},
        };
        CHECK((report("single.eml") == expected));
    }
    SECTION("triple_blank.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Reporting-Mta", "dns; a"},
        };
        CHECK((report("triple_blank.eml") == expected));
    }
    SECTION("upper_ct.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Message-ID", "<g@example.com>"},
            {"Reporting-Mta", "dns; up"},
        };
        CHECK((report("upper_ct.eml") == expected));
    }
    SECTION("ws_line.eml");
    {
        const std::map<std::string, std::string> expected{
            {"Reporting-Mta", "dns; a\n   "},
            {"Status", "5.0.0"},
        };
        CHECK((report("ws_line.eml") == expected));
    }
    TEST_SUMMARY();
}
