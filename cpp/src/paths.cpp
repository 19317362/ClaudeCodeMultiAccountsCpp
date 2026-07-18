#include "ccs.hpp"

#include <cstdlib>
#include <filesystem>

#include <pwd.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace jx {

std::string trim(const std::string& s) {
  size_t a = 0, b = s.size();
  while (a < b && (unsigned char)s[a] <= ' ') a++;
  while (b > a && (unsigned char)s[b - 1] <= ' ') b--;
  return s.substr(a, b - a);
}

std::string toLower(const std::string& s) {
  std::string out = s;
  for (char& c : out) c = (char)std::tolower((unsigned char)c);
  return out;
}

std::string str(const json& obj, const char* key) {
  if (!obj.is_object()) return "";
  auto it = obj.find(key);
  if (it == obj.end() || !it->is_string()) return "";
  return trim(it->get<std::string>());
}

const json* member(const json& obj, const char* key) {
  if (!obj.is_object()) return nullptr;
  auto it = obj.find(key);
  if (it == obj.end() || it->is_null()) return nullptr;
  return &(*it);
}

bool has(const json& obj, const char* key) {
  if (!obj.is_object()) return false;
  auto it = obj.find(key);
  return it != obj.end() && !it->is_null();
}

double num(const json& obj, const char* key, double def) {
  if (!obj.is_object()) return def;
  auto it = obj.find(key);
  if (it == obj.end() || !it->is_number()) return def;
  return it->get<double>();
}

bool isFiniteNum(const json& obj, const char* key) {
  if (!obj.is_object()) return false;
  auto it = obj.find(key);
  return it != obj.end() && it->is_number();
}

}  // namespace jx

std::string homeDir() {
  const char* h = std::getenv("HOME");
  if (h && *h) return h;
  struct passwd* pw = getpwuid(getuid());
  if (pw && pw->pw_dir) return pw->pw_dir;
  return ".";
}

std::string pathJoin(const std::string& a, const std::string& b) {
  return (fs::path(a) / b).string();
}

std::string baseName(const std::string& p) { return fs::path(p).filename().string(); }
std::string dirName(const std::string& p) { return fs::path(p).parent_path().string(); }

std::string getDefaultConfigPath() { return pathJoin(homeDir(), ".claude.json"); }
std::string getDefaultCredentialsPath() {
  return pathJoin(pathJoin(homeDir(), ".claude"), ".credentials.json");
}
std::string getDefaultStorePath() { return pathJoin(homeDir(), ".ClaudeCodeMultiAccounts.json"); }
std::string getDefaultBackupDir() {
  return pathJoin(pathJoin(homeDir(), ".claude"), pathJoin("backups", "multi-account-switch"));
}
std::string getToolConfigDir() {
  return pathJoin(pathJoin(homeDir(), ".claude"), "multi-account-switch");
}
std::string getToolSettingsPath() { return pathJoin(getToolConfigDir(), "settings.json"); }

void ensureDir(const std::string& dir) {
  if (dir.empty()) return;
  std::error_code ec;
  fs::create_directories(dir, ec);
}

bool pathExists(const std::string& p) {
  std::error_code ec;
  return fs::exists(p, ec);
}
