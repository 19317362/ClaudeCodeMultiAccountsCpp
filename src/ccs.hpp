// Claude Code Multi-Account Switcher — C++ port
// Single-binary implementation. All declarations live here; each area has a .cpp.
#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <map>
#include <optional>

// ordered_json preserves key order so rewriting ~/.claude.json (a large file
// Claude owns) does not needlessly reshuffle every key on each switch.
using json = nlohmann::ordered_json;

// ---------------------------------------------------------------------------
// Small JSON access helpers (mirror JS optional-chaining semantics).
// ---------------------------------------------------------------------------
namespace jx {
// Returns the trimmed string value at key, or "" when absent / not a string.
std::string str(const json& obj, const char* key);
// Trims leading/trailing ASCII whitespace.
std::string trim(const std::string& s);
std::string toLower(const std::string& s);
// Pointer to a nested object member, or nullptr when absent / not an object path.
const json* member(const json& obj, const char* key);
// True when obj is an object holding key with a non-null value.
bool has(const json& obj, const char* key);
// Numeric value at key (accepts JSON numbers), or def when missing/non-numeric.
double num(const json& obj, const char* key, double def);
bool isFiniteNum(const json& obj, const char* key);
}  // namespace jx

// ---------------------------------------------------------------------------
// paths.cpp — locations and filesystem helpers.
// ---------------------------------------------------------------------------
std::string homeDir();
std::string getDefaultConfigPath();       // ~/.claude.json
std::string getDefaultCredentialsPath();   // ~/.claude/.credentials.json
std::string getDefaultStorePath();         // ~/.ClaudeCodeMultiAccounts.json
std::string getDefaultBackupDir();         // ~/.claude/backups/multi-account-switch
std::string getToolConfigDir();            // ~/.claude/multi-account-switch
std::string getToolSettingsPath();         // .../settings.json
std::string pathJoin(const std::string& a, const std::string& b);
std::string baseName(const std::string& p);
std::string dirName(const std::string& p);
void ensureDir(const std::string& dir);
bool pathExists(const std::string& p);

// ---------------------------------------------------------------------------
// timefmt.cpp — time formatting/parsing (JS Date parity where it matters).
// ---------------------------------------------------------------------------
long long nowMillis();
std::string isoNow();                       // e.g. 2026-07-18T09:00:00.000Z
long long parseIsoMillis(const std::string& s);  // -1 when unparseable
std::string backupTimestamp();              // YYYYMMDD-HHMMSS (UTC)
std::string formatLocalDateTime(long long millis);
std::string formatLocalTime(long long millis);

// ---------------------------------------------------------------------------
// store.cpp — JSON IO, atomic writes, backups, live/store persistence, settings.
// ---------------------------------------------------------------------------
struct Options {
  std::string usageCommand = "/switch";
  std::string configPath;
  std::string credentialsPath;
  std::string storePath;
  std::string backupDir;
  bool syncOnly = false;
  bool touchCurrentOnly = false;
  bool usageOnly = false;
  bool removeOnly = false;
  int removeIndex = -1;
  bool renameOnly = false;
  int renameIndex = -1;
  std::vector<std::string> renameAliasParts;
  bool showUsage = true;
  std::string selector;
  bool handled = false;  // set when a flag already produced terminal output
};

json readJson(const std::string& path);                     // throws on missing/invalid
json readJsonIfExists(const std::string& path, json fallback);
void writeJsonAtomic(const std::string& path, const json& value, int mode /*0=default*/);
void backupFile(const std::string& path, const std::string& backupDir);
void writeLiveState(const json& config, const json& credentials, const Options& o);
void writeStore(const json& store, const Options& o);

json readSettings();
void writeSettings(const json& settings);
// Rate-limit reset cache; returns -1 when none/past.
long long getRateLimitResetAt();
void setRateLimitResetAt(long long retryAfterSecs);
void setRateLimitResetAtFromIso(const std::string& iso);

// ---------------------------------------------------------------------------
// accounts.cpp — account identity, store sync, selection, credential guards.
// ---------------------------------------------------------------------------
std::string getAccountKey(const json& account);             // throws when unidentifiable
json normalizeStore(json store, const std::string& version);
// Returns copies of stored accounts annotated with "index" and "current".
std::vector<json> getDisplayAccounts(const json& store, const json& currentMetadata);

struct SyncResult {
  bool changed = false;
  json store;
  std::string key;
  bool skipped = false;
  std::string warning;
};
SyncResult syncStoreFromLive(const json& store, const json& config,
                             const json& credentials, const std::string& version);
json findSelection(const std::vector<json>& accounts, const std::string& selector);  // throws
json removeStoredAccount(json& store, int removeIndex);     // returns removed entry

struct Assessment { std::string verdict; std::string reason; };  // ok|need-refresh|refresh-expired
Assessment assessCredentials(const json& claudeAiOauth, long long now);
struct IdentityCheck { bool ok; std::string reason; };
IdentityCheck verifyLiveIdentity(const json& config, const json& credentials, const json& store);

// ---------------------------------------------------------------------------
// http.cpp — libcurl wrappers.
// ---------------------------------------------------------------------------
struct HttpResponse {
  bool networkError = false;
  std::string errorMessage;
  long status = 0;
  std::string body;
  std::map<std::string, std::string> headers;  // lowercased names
};
HttpResponse httpPostJson(const std::string& url, const json& body,
                          const std::vector<std::string>& extraHeaders);
HttpResponse httpGet(const std::string& url, const std::vector<std::string>& headers);

// ---------------------------------------------------------------------------
// auth.cpp — OAuth token refresh.
// ---------------------------------------------------------------------------
struct RefreshResult { bool ok; std::string code; std::string message; json claudeAiOauth; };
RefreshResult refreshTokens(const json& claudeAiOauth);

// ---------------------------------------------------------------------------
// usage.cpp — Claude usage API + snapshot bookkeeping + column formatting.
// ---------------------------------------------------------------------------
struct UsageResult { bool ok = false; bool rateLimited = false; long retryAfter = -1; json data; std::string error; };
UsageResult fetchUsage(const std::string& accessToken);
std::vector<std::string> formatUsageInfo(const UsageResult& usage);
// Fetches usage with existing access tokens only; OAuth refresh is switch-only.
// Updates usageSnapshot and returns current-account usage when available.
struct SnapshotRefresh { UsageResult currentUsage; bool hasCurrent = false; bool changed = false; };
SnapshotRefresh refreshStoredUsageSnapshots(json& store, const std::string& currentKey);
std::string getUsageColumns(const json& entry);

// ---------------------------------------------------------------------------
// output.cpp — display names, plan inference, summaries, messages, color.
// ---------------------------------------------------------------------------
bool supportsColor();
std::string colorize(const std::string& text, const char* code);
std::string getPreferredDisplayName(const json& metadata);
std::string getEntryLabel(const json& entry);
std::string inferPlanType(const json& entry);
std::string getCompactPlanLabel(const json& entry);
std::string formatRelativeTime(const std::string& iso);
std::vector<std::string> formatAccountSummary(const std::vector<json>& accounts);

namespace msg {
std::vector<std::string> listGuidance(const std::string& usageCommand);
std::string restartNotice();
std::string availableAccountsHeading();
std::string storedAccountsHeading();
std::string remainingAccountsHeading();
std::string runningSessionsWarning(int count);
std::string refreshProgress(int index);
std::string refreshSuccess();
std::vector<std::string> switchAbortedLines(const std::string& code, const std::string& reason,
                                            const std::string& label, const std::string& usageCommand);
std::string syncSkippedWarning(const std::string& reason);
}  // namespace msg

// ---------------------------------------------------------------------------
// proc — best-effort running-session detection (implemented in actions.cpp).
// ---------------------------------------------------------------------------
struct SessionInfo { bool detected; int count; };
SessionInfo detectClaudeSessions();

// ---------------------------------------------------------------------------
// actions.cpp — top-level command flows.
// ---------------------------------------------------------------------------
constexpr const char* STORE_VERSION = "0.2.9";
constexpr int RESET_WINDOW_DAYS = 7;

int runMainFlow(Options& options);  // list / switch / sync / usage / remove / rename

// ---------------------------------------------------------------------------
// install.cpp — simple installer for the single binary + Claude hooks.
// ---------------------------------------------------------------------------
int runInstall();
int runUninstall();
int runSessionStart();
int runStatusline();
