// test/auth_test.cpp
//
// Credential discovery is dependency-free, so unlike the rest of the transport
// layer it can be verified without a gRPC toolchain.
//
// The assertion that matters most is the last one: describe() must never emit the
// token's contents. A credential in a CI log is a credential leak, and this is
// the kind of thing that is easy to regress while adding a "helpful" debug line.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "transport/auth.hpp"

namespace {

int g_failures = 0;

void fail(const char* file, int line, const char* what, const char* expr) {
  std::printf("  FAIL  %s:%d  %s  [%s]\n", file, line, what, expr);
  ++g_failures;
}

#define CHECK(cond, what)                                 \
  do {                                                    \
    if (!(cond)) fail(__FILE__, __LINE__, (what), #cond); \
  } while (0)

void banner(const char* name) { std::printf("[ RUN ] %s\n", name); }

void set_env(const char* k, const char* v) {
#if defined(_WIN32)
  _putenv_s(k, v);
#else
  if (v == nullptr || *v == '\0') {
    unsetenv(k);
  } else {
    setenv(k, v, 1);
  }
#endif
}

// Deliberately not std::filesystem: on this project's Windows dev machine, Git
// Bash's mingw64/bin precedes the compiler's own bin on PATH and ships an older
// libstdc++-6.dll that lacks GCC 13's std::filesystem symbols, so any binary
// touching them fails to load with STATUS_ENTRYPOINT_NOT_FOUND. A test does not
// need std::filesystem, so it does not use it.
void write_file(const std::string& p, const std::string& contents) {
  std::ofstream f(p, std::ios::binary);
  f << contents;
}

void test_candidate_search_order() {
  banner("candidate_search_order");
  set_env("MRU_MINKNOW_CA", "");
  set_env("MRU_MINKNOW_TOKEN", "");

  const auto ca = mru::ca_candidate_paths();
  const auto tok = mru::token_candidate_paths();
  CHECK(!ca.empty(), "there is at least one CA candidate for this platform");
  CHECK(!tok.empty(), "there is at least one token candidate for this platform");

  std::printf("        %zu CA candidates, first: %s\n", ca.size(),
              ca.empty() ? "(none)" : ca.front().c_str());
  std::printf("        %zu token candidates, first: %s\n", tok.size(),
              tok.empty() ? "(none)" : tok.front().c_str());
}

void test_env_override_is_authoritative() {
  banner("env_override_is_authoritative");
  set_env("MRU_MINKNOW_CA", "/custom/ca.pem");
  set_env("MRU_MINKNOW_TOKEN", "/custom/token.json");

  const auto ca = mru::ca_candidate_paths();
  const auto tok = mru::token_candidate_paths();

  // An explicit override must NOT fall back to the built-in guesses: silently
  // using a different certificate than the operator asked for would be worse
  // than failing.
  CHECK(ca.size() == 1 && ca.front() == "/custom/ca.pem",
        "MRU_MINKNOW_CA replaces the search list entirely");
  CHECK(tok.size() == 1 && tok.front() == "/custom/token.json",
        "MRU_MINKNOW_TOKEN replaces the search list entirely");

  set_env("MRU_MINKNOW_CA", "");
  set_env("MRU_MINKNOW_TOKEN", "");
}

void test_reads_and_trims() {
  banner("reads_and_trims");
  // Relative to the test's working directory, which ctest makes the build dir.
  const std::string ca_path = "mru_auth_test_ca.pem";
  const std::string tok_path = "mru_auth_test_token.json";

  const std::string pem =
      "-----BEGIN CERTIFICATE-----\nZmFrZQ==\n-----END CERTIFICATE-----\n";
  const std::string token_raw = "  abc123-token-value\r\n";
  const std::string token_clean = "abc123-token-value";

  write_file(ca_path, pem);
  write_file(tok_path, token_raw);

  const auto c = mru::credentials_from_paths(ca_path, tok_path);
  CHECK(c.have_ca(), "CA found");
  CHECK(c.have_token(), "token found");
  CHECK(c.ca_cert_pem == pem, "PEM read byte-for-byte, no newline translation");
  CHECK(c.auth_token == token_clean, "token whitespace-trimmed");
  CHECK(c.ca_path == ca_path, "CA path recorded");

  // --- the one that guards against a credential leak ---
  const std::string desc = c.describe();
  CHECK(desc.find(token_clean) == std::string::npos,
        "describe() must NOT contain the token value");
  CHECK(desc.find(tok_path) != std::string::npos,
        "describe() does name the token's path");
  CHECK(desc.find("not shown") != std::string::npos,
        "describe() says the token was withheld");

  std::remove(ca_path.c_str());
  std::remove(tok_path.c_str());
}

void test_missing_files_are_reported_not_fatal() {
  banner("missing_files_are_reported_not_fatal");
  const auto c = mru::credentials_from_paths("/definitely/not/here/ca.pem",
                                             "/definitely/not/here/token.json");
  CHECK(!c.have_ca(), "absent CA reported as absent");
  CHECK(!c.have_token(), "absent token reported as absent");
  CHECK(c.tried.size() == 2, "both paths recorded as tried");

  const std::string desc = c.describe();
  CHECK(desc.find("NOT FOUND") != std::string::npos, "describe() says NOT FOUND");
  CHECK(desc.find("/definitely/not/here/ca.pem") != std::string::npos,
        "describe() lists every path examined so the operator can see what was checked");
  CHECK(desc.find("MRU_MINKNOW_CA") != std::string::npos,
        "describe() tells the operator how to override");
}

}  // namespace

int main() {
  std::printf("credential discovery tests\n\n");

  test_candidate_search_order();
  test_env_override_is_authoritative();
  test_reads_and_trims();
  test_missing_files_are_reported_not_fatal();

  std::printf("\n%s  (%d failure%s)\n", g_failures == 0 ? "PASSED" : "FAILED",
              g_failures, g_failures == 1 ? "" : "s");
  return g_failures == 0 ? 0 : 1;
}
