// Authentication-Results, Received and Received-SPF, and which of them this host wrote.
#include <sc/mime/trace.h>

#include "sc_test.h"

#include <string>

int main() {
    SECTION("Authentication-Results");
    {
        const auto dmarc = sc::mime::parse_authentication_results("mail.example.com; dmarc=none (p=none dis=none) header.from=sender.example");
        CHECK(dmarc.has_value());
        if (dmarc) {
            CHECK_EQ(dmarc->authserv_id, std::string{"mail.example.com"});
            CHECK_EQ(dmarc->results.size(), std::size_t{1});
            const auto from = dmarc->results.at(0).property("Header.From").value_or("");
            CHECK_EQ(from, std::string{"sender.example"});
            const auto result = dmarc->results.at(0).result;
            CHECK_EQ(result, std::string{"none"});
        }
        const auto dkim = sc::mime::parse_authentication_results(
            "Mail.Example.com;\n\tdkim=fail reason=\"signature verification failed\" (2048-bit key; unprotected) header.d=sender.example "
            "header.i=@sender.example header.a=rsa-sha256 header.s=default header.b=Yb6nuIVT;\n\tdkim-atps=neutral");
        CHECK(dkim.has_value());
        if (dkim) {
            CHECK_EQ(dkim->authserv_id, std::string{"mail.example.com"});
            CHECK_EQ(dkim->results.size(), std::size_t{2});
            if (dkim->results.size() == 2) {
                const auto &first = dkim->results[0];
                CHECK_EQ(first.method, std::string{"dkim"});
                CHECK_EQ(first.result, std::string{"fail"});
                CHECK_EQ(first.reason, std::string{"signature verification failed"});
                CHECK_EQ(first.property("header.d").value_or(""), std::string{"sender.example"});
                CHECK_EQ(first.property("header.b").value_or(""), std::string{"Yb6nuIVT"});
                CHECK_EQ(dkim->results[1].method, std::string{"dkim-atps"});
            }
        }
        // Versions, spaces around '=', a quoted value with ';', none
        const auto spaced = sc::mime::parse_authentication_results("mx.example 1; spf/1 = Pass smtp.mailfrom = \"a;b@x.example\"");
        CHECK(spaced.has_value());
        if (spaced && spaced->results.size() == 1) {
            CHECK_EQ(spaced->results[0].method, std::string{"spf"});
            CHECK_EQ(spaced->results[0].result, std::string{"pass"});
            CHECK_EQ(spaced->results[0].property("smtp.mailfrom").value_or(""), std::string{"a;b@x.example"});
        }
        const auto none = sc::mime::parse_authentication_results("mx.example; none");
        CHECK(none.has_value() && none->results.empty());
        CHECK(!sc::mime::parse_authentication_results(""));
        CHECK(!sc::mime::parse_authentication_results("dkim=pass"));
    }

    SECTION("Received");
    {
        const auto known = sc::mime::parse_received("from spe13.ucebox.example (spe13.ucebox.example [192.0.2.28]) "
                                                    "by mail2.v.example.com (Postfix) with ESMTPS id 3FACE19F4A for <a@example.com>; Tue, 24 Nov 2026");
        CHECK(known.has_value());
        if (known) {
            CHECK_EQ(known->helo, std::string{"spe13.ucebox.example"});
            CHECK_EQ(known->host, std::string{"spe13.ucebox.example"});
            CHECK_EQ(known->ip, std::string{"192.0.2.28"});
            CHECK_EQ(known->by, std::string{"mail2.v.example.com"});
            CHECK(!known->unknown_host());
        }
        const auto unknown = sc::mime::parse_received("from bank-statements.example (unknown [198.51.100.7]) by mx.example.com (Postfix) with ESMTP id X");
        CHECK(unknown && unknown->unknown_host());
        if (unknown) CHECK_EQ(unknown->ip, std::string{"198.51.100.7"});
        const auto v6 = sc::mime::parse_received("from x.example (x.example [IPv6:2001:db8::5]) by mx.example.com");
        if (v6) CHECK_EQ(v6->ip, std::string{"2001:db8::5"});
        // Exim's: the address as the name, helo= in the comment
        const auto exim = sc::mime::parse_received("from [192.0.2.2] (port=58132 helo=[192.168.8.108]) by relay.example with esmtpsa");
        if (exim) {
            CHECK_EQ(exim->ip, std::string{"192.0.2.2"});
            CHECK_EQ(exim->host, std::string{""});
            CHECK_EQ(exim->by, std::string{"relay.example"});
        }
        CHECK(!sc::mime::parse_received("by mx.example.com (Postfix, from userid 0) id 1"));
    }

    SECTION("This host's trace");
    {
        const auto mail = sc::mime::message::parse(
            "Return-Path: <x@sender.example>\n"
            "Authentication-Results: mail.example.com; dmarc=fail (p=reject dis=none) header.from=bank.example\n"
            "Authentication-Results: mail.example.com; spf=softfail smtp.mailfrom=sender.example\n"
            "Authentication-Results: elsewhere.example; dkim=pass header.d=bank.example\n"
            "Received-SPF: Softfail (mailfrom) identity=mailfrom; client-ip=198.51.100.7\n"
            "Received: from localhost (localhost [127.0.0.1]) by mx.example.com (Postfix) id A\n"
            "Received: from statements.example (unknown [198.51.100.7]) by mx.example.com (Postfix) id B\n"
            "Authentication-Results: mail.example.com; dmarc=pass header.from=bank.example\n"
            "Received: from somewhere (somewhere [203.0.113.1]) by statements.example\n"
            "From: Bank <alerts@bank.example>\n"
            "Subject: Statement\n"
            "Message-ID: <1@x>\n"
            "\n"
            "Hello\n");
        const auto trace = sc::mime::trace_of(mail, {"Mail.Example.COM"});
        CHECK_EQ(trace.authentication.size(), std::size_t{2}); // not elsewhere's, nor the one below our Received
        const auto *dmarc = trace.result("DMARC");
        CHECK(dmarc != nullptr);
        if (dmarc) CHECK_EQ(dmarc->result, std::string{"fail"});
        CHECK_EQ(trace.spf(), std::string{"softfail"});
        CHECK(trace.client.has_value());
        if (trace.client) {
            CHECK_EQ(trace.client->ip, std::string{"198.51.100.7"});
            CHECK(trace.client->unknown_host());
        }
        CHECK(trace.result("dkim") == nullptr);
        CHECK_EQ(trace.received_spf.size(), std::size_t{1});

        // Received-SPF alone gives the SPF result; no authserv-id takes no Authentication-Results
        const auto bare = sc::mime::trace_of(std::vector<sc::mime::header>{
                                                 {"Received-SPF", "Pass (mailfrom) identity=mailfrom"},
                                                 {"Authentication-Results", "mail.example.com; spf=fail"},
                                                 {"Received", "from a.example (a.example [192.0.2.9]) by mx.example.com"}},
                                             {});
        CHECK_EQ(bare.spf(), std::string{"pass"});
        CHECK(bare.authentication.empty());
    }

    TEST_SUMMARY();
}
