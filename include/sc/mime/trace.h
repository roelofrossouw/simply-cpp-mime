#ifndef SC_MIME_TRACE_H
#define SC_MIME_TRACE_H
// A mail's trace headers as the receiving host wrote them: Authentication-Results (RFC 8601:
// DKIM, SPF, DMARC as opendkim, opendmarc and others report them), Received-SPF (RFC 7208) and
// the Received line saying which client handed it over (as Postfix writes it).
//
// A sender can write any of these headers itself. What this host's mail server added is above
// its own Received line, since milters and policy services put theirs on top: trace_of() reads
// only those, and only Authentication-Results under this host's authserv-ids.
//
//     const auto mail = sc::mime::message::parse(text);
//     const auto trace = sc::mime::trace_of(mail, {"mail.example.com"});
//     if (const auto *dmarc = trace.result("dmarc"); dmarc && dmarc->result == "pass")
//         ... dmarc->property("header.from") ...                 // "example.com"
//     if (trace.client && trace.client->host == "unknown") ...  // no reverse DNS for trace.client->ip
#include <sc/mime/mime.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sc::mime {
    // One method's result in an Authentication-Results header: "dmarc=fail (p=reject)
    // header.from=example.com" is method "dmarc", result "fail" and property header.from.
    struct auth_result {
        std::string method; // lowercased, without a version ("dkim", "spf", "dmarc", "dkim-atps", "auth")
        std::string result; // lowercased ("pass", "fail", "softfail", "none", "neutral", "temperror", "permerror")
        std::string reason; // reason=, unquoted; empty when not given
        // ptype.property=value, in order ("header.from", "smtp.mailfrom", "header.d"): names lowercased, values
        // as written, unquoted. Comments are left out.
        std::vector<std::pair<std::string, std::string>> properties;

        // A property's value by name, ignoring case; nullopt when absent.
        [[nodiscard]] std::optional<std::string> property(std::string_view name) const;
    };

    // One Authentication-Results header.
    struct authentication_results {
        std::string authserv_id; // lowercased
        std::vector<auth_result> results; // empty for "none"
    };

    // An Authentication-Results header's value read; nullopt when it has no authserv-id.
    [[nodiscard]] std::optional<authentication_results> parse_authentication_results(std::string_view value);

    // A Received header's "from" and "by" clauses.
    struct received {
        std::string helo; // the name the client gave (EHLO/HELO), as written
        std::string host; // the client's name as the receiver found it by reverse DNS, as written: Postfix
                          // writes "unknown" when it found none; empty when the line doesn't say
        std::string ip;   // the client's address, without [] or "IPv6:"; empty when the line doesn't say
        std::string by;   // the receiving host, as written

        // Whether the receiver found no reverse DNS name for the client (Postfix's "unknown").
        [[nodiscard]] bool unknown_host() const;
    };

    // A Received header's value read: "from helo.example (host.example [192.0.2.1]) by mx.example
    // (Postfix) with ESMTPS id ...". nullopt when it has no "from" clause.
    [[nodiscard]] std::optional<received> parse_received(std::string_view value);

    // What this host's mail server added to a mail, from the headers above its first Received
    // line from another host (a loopback client is a local relay, so its line is passed over).
    struct trace {
        std::vector<authentication_results> authentication; // only those under the authserv-ids asked for, top first
        std::vector<std::string> received_spf;               // Received-SPF values, top first
        std::optional<mime::received> client;                // that Received line: who handed the mail over

        // The first result of a method ("dmarc", "spf", "dkim") in authentication, lowercased name; nullptr when none.
        [[nodiscard]] const auth_result *result(std::string_view method) const;

        // Every result of a method, top first (DKIM has one per signature).
        [[nodiscard]] std::vector<const auth_result *> results(std::string_view method) const;

        // The SPF result: Authentication-Results' spf= when there's one, else the first Received-SPF's
        // ("pass", "fail", "softfail", "neutral", "none", "temperror", "permerror"); empty when neither says.
        [[nodiscard]] std::string spf() const;
    };

    // A mail's trace as this host wrote it (see trace). authserv_ids are this host's (any case);
    // none takes no Authentication-Results.
    [[nodiscard]] trace trace_of(const message &mail, const std::vector<std::string> &authserv_ids);

    // The same from headers in order, top first (message::headers()).
    [[nodiscard]] trace trace_of(const std::vector<header> &headers, const std::vector<std::string> &authserv_ids);
}

#endif
