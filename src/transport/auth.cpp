// src/transport/auth.cpp

#include "transport/auth.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace mru {
namespace {

[[nodiscard]] std::string env_or_empty(const char* name) {
  const char* v = std::getenv(name);
  return v != nullptr ? std::string(v) : std::string();
}

[[nodiscard]] std::string trim(std::string s) {
  const auto not_space = [](unsigned char c) {
    return c != ' ' && c != '\t' && c != '\r' && c != '\n';
  };
  std::size_t b = 0;
  while (b < s.size() && !not_space(static_cast<unsigned char>(s[b]))) ++b;
  std::size_t e = s.size();
  while (e > b && !not_space(static_cast<unsigned char>(s[e - 1]))) --e;
  return s.substr(b, e - b);
}

// Returns file contents, or empty on any failure. Reading in binary mode keeps
// a PEM byte-identical on Windows, where text mode would eat the CRs.
[[nodiscard]] std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

std::vector<std::string> ca_candidate_paths() {
  std::vector<std::string> out;

  const std::string override_path = env_or_empty("MRU_MINKNOW_CA");
  if (!override_path.empty()) {
    out.push_back(override_path);
    return out;  // an explicit override is authoritative: do not fall back
  }

#if defined(_WIN32)
  const std::string pd = env_or_empty("ProgramData");
  const std::string base = pd.empty() ? "C:/ProgramData" : pd;
  out.push_back(base + "/OxfordNanopore/MinKNOW/conf/rpc-certs/minknow/ca.crt");
  out.push_back(base + "/OxfordNanopore/MinKNOW/conf/rpc-certs/ca.crt");
  out.push_back("C:/Program Files/OxfordNanopore/MinKNOW/conf/rpc-certs/minknow/ca.crt");
#elif defined(__APPLE__)
  out.push_back(
      "/Applications/MinKNOW.app/Contents/Resources/conf/rpc-certs/minknow/ca.crt");
  out.push_back("/Library/Application Support/MinKNOW/conf/rpc-certs/minknow/ca.crt");
#else
  out.push_back("/opt/ont/minknow/conf/rpc-certs/minknow/ca.crt");
  out.push_back("/opt/ont/minknow/conf/rpc-certs/ca.crt");
  out.push_back("/etc/minknow/conf/rpc-certs/minknow/ca.crt");
#endif
  return out;
}

std::vector<std::string> token_candidate_paths() {
  std::vector<std::string> out;

  const std::string override_path = env_or_empty("MRU_MINKNOW_TOKEN");
  if (!override_path.empty()) {
    out.push_back(override_path);
    return out;
  }

#if defined(_WIN32)
  const std::string pd = env_or_empty("ProgramData");
  const std::string base = pd.empty() ? "C:/ProgramData" : pd;
  out.push_back(base + "/OxfordNanopore/MinKNOW/local_auth_token.json");
  out.push_back(base + "/OxfordNanopore/MinKNOW/conf/local_auth_token.json");
#elif defined(__APPLE__)
  out.push_back("/Library/Application Support/MinKNOW/local_auth_token.json");
#else
  out.push_back("/opt/ont/minknow/conf/local_auth_token.json");
  out.push_back("/var/lib/minknow/local_auth_token.json");
#endif

  const std::string home = env_or_empty("HOME");
  if (!home.empty()) {
    out.push_back(home + "/.local/share/ONT/MinKNOW/local_auth_token.json");
  }
  return out;
}

Credentials discover_credentials() {
  Credentials c;

  for (const auto& p : ca_candidate_paths()) {
    c.tried.push_back(p);
    std::string contents = read_file(p);
    if (!contents.empty()) {
      c.ca_cert_pem = std::move(contents);
      c.ca_path = p;
      break;
    }
  }

  for (const auto& p : token_candidate_paths()) {
    c.tried.push_back(p);
    std::string contents = trim(read_file(p));
    if (!contents.empty()) {
      c.auth_token = std::move(contents);
      c.token_path = p;
      break;
    }
  }

  return c;
}

Credentials credentials_from_paths(const std::string& ca_path,
                                   const std::string& token_path) {
  Credentials c;
  if (!ca_path.empty()) {
    c.tried.push_back(ca_path);
    c.ca_cert_pem = read_file(ca_path);
    if (!c.ca_cert_pem.empty()) c.ca_path = ca_path;
  }
  if (!token_path.empty()) {
    c.tried.push_back(token_path);
    c.auth_token = trim(read_file(token_path));
    if (!c.auth_token.empty()) c.token_path = token_path;
  }
  return c;
}

std::string Credentials::describe() const {
  std::ostringstream ss;
  ss << "MinKNOW credentials:\n";

  if (have_ca()) {
    ss << "  CA certificate : " << ca_path << " (" << ca_cert_pem.size() << " bytes)\n";
  } else {
    ss << "  CA certificate : NOT FOUND\n";
  }

  if (have_token()) {
    // Never log the token itself. Length alone is enough to tell "present" from
    // "empty file", and a token in a log file is a credential leak.
    ss << "  Auth token     : " << token_path << " (" << auth_token.size()
       << " chars, not shown)\n";
  } else {
    ss << "  Auth token     : NOT FOUND\n";
  }

  if (!have_ca() || !have_token()) {
    ss << "  Paths examined, in order:\n";
    for (const auto& p : tried) ss << "    " << p << '\n';
    ss << "  Set MRU_MINKNOW_CA and/or MRU_MINKNOW_TOKEN to point at the correct\n"
          "  files. These locations are installation- and version-dependent, so the\n"
          "  list above is a best-effort search rather than an authoritative set.\n";
  }
  return ss.str();
}

}  // namespace mru
