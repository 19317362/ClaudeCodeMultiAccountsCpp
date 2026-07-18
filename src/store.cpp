#include "ccs.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

static std::string readFileToString(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

json readJson(const std::string& path) {
  std::string raw = readFileToString(path);
  try {
    return json::parse(raw);
  } catch (...) {
    // Never echo the source text: the credentials file would leak token bytes.
    throw std::runtime_error("Failed to parse " + baseName(path) + ": invalid JSON.");
  }
}

json readJsonIfExists(const std::string& path, json fallback) {
  if (!pathExists(path)) return fallback;
  return readJson(path);
}

void writeJsonAtomic(const std::string& path, const json& value, int mode) {
  ensureDir(dirName(path));
  std::string payload = value.dump(2) + "\n";

  // A rename transfers the temp file's mode onto the target, so the temp file
  // must inherit the target's permissions (0600 on POSIX credentials files) or
  // they would be silently widened to the umask default.
  int effectiveMode = mode;
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    effectiveMode = st.st_mode & 0777;
  }

  std::string tempPath = path + "." + std::to_string((long)getpid()) + "." +
                         std::to_string(nowMillis()) + ".tmp";
  try {
    {
      std::ofstream out(tempPath, std::ios::binary | std::ios::trunc);
      if (!out) throw std::runtime_error("cannot write " + tempPath);
      out << payload;
      if (!out) throw std::runtime_error("write failed " + tempPath);
    }
    if (effectiveMode > 0) ::chmod(tempPath.c_str(), effectiveMode);
    std::error_code ec;
    fs::rename(tempPath, path, ec);
    if (ec) throw std::runtime_error("rename failed");
  } catch (...) {
    // Fall back to a direct overwrite; the temp file must never linger because
    // it may contain tokens.
    std::error_code ec;
    fs::remove(tempPath, ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << payload;
    out.close();
    if (effectiveMode > 0) ::chmod(path.c_str(), effectiveMode);
  }
}

void backupFile(const std::string& path, const std::string& backupDir) {
  if (!pathExists(path)) return;
  ensureDir(backupDir);
  std::string base = baseName(path);
  std::string dest = pathJoin(backupDir, base + "." + backupTimestamp() + ".bak");
  std::error_code ec;
  fs::copy_file(path, dest, fs::copy_options::overwrite_existing, ec);

  // Retention is per source file: the backup dir is shared, so a global keep-3
  // would let one file's backups evict another's within one switch.
  std::vector<std::string> backups;
  for (auto& e : fs::directory_iterator(backupDir, ec)) {
    std::string name = e.path().filename().string();
    if (name.rfind(base + ".", 0) == 0 && name.size() >= 4 &&
        name.substr(name.size() - 4) == ".bak") {
      backups.push_back(name);
    }
  }
  std::sort(backups.begin(), backups.end());
  std::reverse(backups.begin(), backups.end());
  for (size_t i = 3; i < backups.size(); ++i) {
    fs::remove(pathJoin(backupDir, backups[i]), ec);
  }
}

// Only claudeAiOauth belongs to this tool; sibling keys Claude Code may add to
// .credentials.json must survive a switch.
static void mergeCredentialsWrite(const std::string& credentialsPath, const json& credentials) {
  json existing;
  bool haveExisting = false;
  try {
    existing = readJsonIfExists(credentialsPath, json());
    haveExisting = existing.is_object();
  } catch (...) {
    haveExisting = false;
  }
  json next;
  if (haveExisting) {
    next = existing;
    next["claudeAiOauth"] = credentials.contains("claudeAiOauth") ? credentials["claudeAiOauth"] : json();
  } else {
    next = credentials;
  }
  writeJsonAtomic(credentialsPath, next, 0600);
}

void writeLiveState(const json& config, const json& credentials, const Options& o) {
  backupFile(o.configPath, o.backupDir);
  backupFile(o.credentialsPath, o.backupDir);
  writeJsonAtomic(o.configPath, config, 0);
  mergeCredentialsWrite(o.credentialsPath, credentials);
}

void writeStore(const json& store, const Options& o) {
  backupFile(o.storePath, o.backupDir);
  writeJsonAtomic(o.storePath, store, 0);
}

// --------------------------- tool settings ---------------------------------

json readSettings() {
  std::string p = getToolSettingsPath();
  if (!pathExists(p)) return json{{"showUsage", true}};
  try {
    return readJson(p);
  } catch (...) {
    return json{{"showUsage", true}};
  }
}

void writeSettings(const json& settings) {
  std::string p = getToolSettingsPath();
  ensureDir(dirName(p));
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out << settings.dump(2) << "\n";
}

long long getRateLimitResetAt() {
  json settings = readSettings();
  if (settings.contains("rateLimitResetAt") && settings["rateLimitResetAt"].is_string()) {
    long long resetTime = parseIsoMillis(settings["rateLimitResetAt"].get<std::string>());
    if (resetTime > nowMillis()) return resetTime;
  }
  return -1;
}

void setRateLimitResetAt(long long retryAfterSecs) {
  json settings = readSettings();
  long long resetMs = nowMillis() + retryAfterSecs * 1000;
  // Store as ISO for parity with the Node settings file.
  time_t secs = (time_t)(resetMs / 1000);
  int millis = (int)(resetMs % 1000);
  struct tm tm{};
  gmtime_r(&secs, &tm);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
  settings["rateLimitResetAt"] = std::string(buf);
  writeSettings(settings);
}

void setRateLimitResetAtFromIso(const std::string& iso) {
  if (iso.empty()) return;
  long long resetTime = parseIsoMillis(iso);
  if (resetTime > 0 && resetTime > nowMillis()) {
    json settings = readSettings();
    settings["rateLimitResetAt"] = iso;
    writeSettings(settings);
  }
}
