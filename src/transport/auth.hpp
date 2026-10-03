// src/transport/auth.hpp
//
// Locating MinKNOW's TLS root certificate and local authentication token.
//
// A deliberate design note on honesty here
// ----------------------------------------
// The on-disk locations of MinKNOW's CA certificate and local auth token are
// installation- and version-dependent, and ONT has moved them more than once.
// This code therefore does NOT hardcode a single guessed path. It searches an
// ordered list of candidates, records which one it used, and on failure reports
// every path it tried so the operator can see exactly what was checked.
//
// Two environment variables override the search outright, which is also how you
// point the daemon at Icarust or any other simulator:
//
//     MRU_MINKNOW_CA      path to the PEM root certificate
//     MRU_MINKNOW_TOKEN   path to the local authentication token file
//
// If you know the correct path for your installation, set these rather than
// relying on the search order below.
#pragma once

#include <string>
#include <vector>

namespace mru {

struct Credentials {
  std::string ca_cert_pem;      // file CONTENTS, not a path
  std::string auth_token;       // file contents, whitespace-trimmed
  std::string ca_path;          // where it was found, for logging
  std::string token_path;
  std::vector<std::string> tried;  // every candidate examined, in order

  [[nodiscard]] bool have_ca() const noexcept { return !ca_cert_pem.empty(); }
  [[nodiscard]] bool have_token() const noexcept { return !auth_token.empty(); }

  // A human-readable account of what was found and what was not. Print this at
  // startup: a silent credential failure turns into a confusing TLS handshake
  // error several layers down.
  [[nodiscard]] std::string describe() const;
};

// Ordered candidate paths for this platform, after applying the env overrides.
// Exposed so the daemon can log them and so tests can assert the search order.
[[nodiscard]] std::vector<std::string> ca_candidate_paths();
[[nodiscard]] std::vector<std::string> token_candidate_paths();

// Reads the first candidate that exists and is non-empty. Never throws; an
// absent file is reported through the returned struct, not by failing.
[[nodiscard]] Credentials discover_credentials();

// Convenience for tests and for simulators that need no TLS at all.
[[nodiscard]] Credentials credentials_from_paths(const std::string& ca_path,
                                                 const std::string& token_path);

// The metadata key MinKNOW expects the local token under. Version-dependent;
// kept in one place so there is a single thing to change.
inline constexpr const char* kLocalAuthMetadataKey = "local-auth";

}  // namespace mru
