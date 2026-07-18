#include "ccs.hpp"

#include <cstdio>
#include <iostream>
#include <memory>

// -------------------- best-effort session detection ------------------------

SessionInfo detectClaudeSessions() {
  // Only the native `claude` executable is detectable by name. Detection
  // failure must never block a switch.
  std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen("ps -A -o comm= 2>/dev/null", "r"), pclose);
  if (!pipe) return {false, 0};
  int count = 0;
  char line[512];
  while (std::fgets(line, sizeof(line), pipe.get())) {
    std::string s = jx::trim(line);
    size_t slash = s.find_last_of('/');
    std::string name = slash == std::string::npos ? s : s.substr(slash + 1);
    if (name == "claude") ++count;
  }
  return {true, count};
}

// ------------------------------- actions -----------------------------------

static std::string emailOf(const json& entry) {
  const json* m = jx::member(entry, "metadata");
  std::string email = m ? jx::str(*m, "emailAddress") : "";
  return email.empty() ? "(no email)" : email;
}

static void runUsageAction(json& store, const json& config, const Options& options) {
  std::cout << "Fetching usage from Claude API..." << std::endl;
  std::string currentKey = getAccountKey(config.at("oauthAccount"));
  SnapshotRefresh refresh = refreshStoredUsageSnapshots(store, currentKey);
  if (refresh.changed) writeStore(store, options);
  if (refresh.hasCurrent) {
    for (const auto& line : formatUsageInfo(refresh.currentUsage)) std::cout << line << std::endl;
  } else {
    std::cout << "No usage data available for the current account." << std::endl;
  }
}

static void runListAction(const SyncResult& synced, json& store, const json& config,
                          const json& credentials, const Options& options) {
  if (synced.changed) {
    writeStore(store, options);
    std::cout << "Saved the current account snapshot into " << baseName(options.storePath)
              << " before showing the account list." << std::endl;
  }

  const json* creds = jx::member(credentials, "claudeAiOauth");
  std::string accessToken = creds ? jx::str(*creds, "accessToken") : "";
  if (!accessToken.empty()) {
    SnapshotRefresh refresh = refreshStoredUsageSnapshots(store, getAccountKey(config.at("oauthAccount")));
    if (refresh.changed) writeStore(store, options);
    if (options.showUsage && refresh.hasCurrent) {
      std::cout << "--- Usage ---" << std::endl;
      for (const auto& line : formatUsageInfo(refresh.currentUsage)) std::cout << line << std::endl;
      std::cout << std::endl;
    }
  }

  std::cout << msg::availableAccountsHeading() << std::endl;
  for (const auto& line : formatAccountSummary(getDisplayAccounts(store, config.at("oauthAccount"))))
    std::cout << line << std::endl;
  std::cout << std::endl;
  for (const auto& line : msg::listGuidance(options.usageCommand)) std::cout << line << std::endl;
}

static int runSwitchAction(const json& selected, json& store, const json& config,
                           const Options& options) {
  bool switched = false, refreshed = false;
  std::string abortCode, abortReason;

  SessionInfo sessions = detectClaudeSessions();
  if (sessions.detected && sessions.count > 0)
    std::cout << msg::runningSessionsWarning(sessions.count) << std::endl;

  const json* selCreds = jx::member(selected, "credentials");
  const json* selOauth = selCreds ? jx::member(*selCreds, "claudeAiOauth") : nullptr;
  Assessment assessment = assessCredentials(selOauth ? *selOauth : json(), nowMillis());

  json credentialsForLive = selCreds ? *selCreds : json::object();
  int selectedIndex = selected.contains("index") ? selected["index"].get<int>() : 0;

  if (assessment.verdict == "refresh-expired") {
    abortCode = "refresh-expired";
    abortReason = assessment.reason;
  } else {
    if (assessment.verdict == "need-refresh") {
      std::cout << msg::refreshProgress(selectedIndex) << std::endl;
      RefreshResult r = refreshTokens(credentialsForLive.value("claudeAiOauth", json()));
      if (!r.ok) {
        abortCode = r.code;
        abortReason = r.message;
      } else {
        credentialsForLive["claudeAiOauth"] = r.claudeAiOauth;
        refreshed = true;
        std::cout << msg::refreshSuccess() << std::endl;
      }
    }

    if (abortCode.empty()) {
      std::string nowIso = isoNow();
      std::string selKey = jx::str(selected, "key");
      for (auto& entry : store["accounts"]) {
        if (jx::str(entry, "key") == selKey) {
          entry["lastUsedAt"] = nowIso;
          if (refreshed) {
            entry["credentials"] = credentialsForLive;
            entry["lastSyncedAt"] = nowIso;
          }
          break;
        }
      }
      // Rotated refresh tokens are single-use: they must reach the store before
      // the live swap, or a failed live write would lose the only working copy.
      writeStore(store, options);

      json nextConfig = config;
      nextConfig["oauthAccount"] = selected.at("metadata");
      writeLiveState(nextConfig, credentialsForLive, options);
      switched = true;
    }
  }

  if (!switched) {
    for (const auto& line :
         msg::switchAbortedLines(abortCode, abortReason, getEntryLabel(selected), options.usageCommand))
      std::cout << line << std::endl;
    return 1;
  }

  std::vector<json> currentAccounts = getDisplayAccounts(store, selected.at("metadata"));
  std::string currentPlan = inferPlanType(selected);
  std::cout << "Switched active account to [" << selectedIndex << "] " << getEntryLabel(selected)
            << " <" << jx::str(selected.at("metadata"), "emailAddress") << "> (" << currentPlan
            << ")." << std::endl;
  std::cout << std::endl;
  std::cout << msg::restartNotice() << std::endl;
  std::cout << std::endl;
  std::cout << msg::storedAccountsHeading() << std::endl;
  for (const auto& line : formatAccountSummary(currentAccounts)) std::cout << line << std::endl;
  return 0;
}

static void runSyncAction(const json& existingStore, const json& config, const json& credentials,
                          const Options& options) {
  SyncResult result = syncStoreFromLive(existingStore, config, credentials, STORE_VERSION);
  if (result.skipped) {
    std::cout << "Warning: " << result.warning << std::endl;
    std::cout << "Sync skipped; the store was not updated." << std::endl;
    return;
  }
  if (result.changed) {
    writeStore(result.store, options);
    std::cout << "Synced current account into " << baseName(options.storePath) << "." << std::endl;
  } else {
    std::cout << baseName(options.storePath) << " already matches the current account snapshot."
              << std::endl;
  }
}

static void runRemoveAction(json& existingStore, const Options& options) {
  json removed = removeStoredAccount(existingStore, options.removeIndex);
  writeStore(existingStore, options);
  std::cout << "Removed account: [" << options.removeIndex << "] " << getEntryLabel(removed) << " <"
            << emailOf(removed) << ">" << std::endl;
  std::cout << std::endl;
  std::cout << msg::remainingAccountsHeading() << std::endl;
  for (const auto& entry : existingStore["accounts"]) {
    int index = entry.contains("index") ? entry["index"].get<int>() : 0;
    std::cout << "  [" << index << "] " << getEntryLabel(entry) << " <" << emailOf(entry) << ">"
              << std::endl;
  }
}

static void runRenameAction(json& existingStore, const json& config, const Options& options) {
  int count = (existingStore.contains("accounts") && existingStore["accounts"].is_array())
                  ? (int)existingStore["accounts"].size()
                  : 0;
  if (options.renameIndex < 0 || options.renameIndex >= count) {
    throw std::runtime_error("Invalid account index. Use an index between 0 and " +
                             std::to_string(count - 1) + ".");
  }
  std::string alias;
  for (size_t i = 0; i < options.renameAliasParts.size(); ++i) {
    if (i) alias += " ";
    alias += options.renameAliasParts[i];
  }
  std::string trimmed = jx::trim(alias);
  json& entry = existingStore["accounts"][options.renameIndex];
  if (!trimmed.empty()) {
    entry["alias"] = trimmed;
  } else if (entry.contains("alias")) {
    entry.erase("alias");
  }
  writeStore(existingStore, options);

  std::string email = emailOf(entry);
  if (!trimmed.empty()) {
    std::cout << "Renamed account: [" << options.renameIndex << "] " << trimmed << " <" << email
              << ">" << std::endl;
  } else {
    std::cout << "Cleared alias: [" << options.renameIndex << "] " << getEntryLabel(entry) << " <"
              << email << ">" << std::endl;
  }
  std::cout << std::endl;
  std::cout << msg::storedAccountsHeading() << std::endl;
  json currentMetadata = jx::has(config, "oauthAccount") ? config.at("oauthAccount") : json();
  for (const auto& line : formatAccountSummary(getDisplayAccounts(existingStore, currentMetadata)))
    std::cout << line << std::endl;
}

// ------------------------------- dispatch ----------------------------------

int runMainFlow(Options& options) {
  if (options.handled) return 0;
  try {
    json config = readJson(options.configPath);
    json credentials = readJson(options.credentialsPath);
    json existingStore =
        normalizeStore(readJsonIfExists(options.storePath, json{{"version", STORE_VERSION},
                                                                {"accounts", json::array()}}),
                       STORE_VERSION);

    if (options.usageOnly) {
      SyncResult syncedForUsage =
          syncStoreFromLive(existingStore, config, credentials, STORE_VERSION);
      if (!syncedForUsage.warning.empty())
        std::cout << msg::syncSkippedWarning(syncedForUsage.warning) << std::endl;
      runUsageAction(syncedForUsage.store, config, options);
      return 0;
    }

    if (options.syncOnly) {
      runSyncAction(existingStore, config, credentials, options);
      return 0;
    }

    if (options.touchCurrentOnly) {
      SyncResult synced = syncStoreFromLive(existingStore, config, credentials, STORE_VERSION);
      json store = synced.store;
      std::string currentKey = getAccountKey(config.at("oauthAccount"));
      for (auto& entry : store["accounts"]) {
        if (jx::str(entry, "key") == currentKey) {
          entry["lastUsedAt"] = isoNow();
          break;
        }
      }
      writeStore(store, options);
      return 0;
    }

    if (options.removeOnly) {
      runRemoveAction(existingStore, options);
      return 0;
    }

    if (options.renameOnly) {
      runRenameAction(existingStore, config, options);
      return 0;
    }

    SyncResult synced = syncStoreFromLive(existingStore, config, credentials, STORE_VERSION);
    if (!synced.warning.empty())
      std::cout << msg::syncSkippedWarning(synced.warning) << std::endl;
    json store = synced.store;
    std::vector<json> accounts = getDisplayAccounts(store, config.at("oauthAccount"));

    if (options.selector.empty()) {
      runListAction(synced, store, config, credentials, options);
      return 0;
    }

    json selected = findSelection(accounts, options.selector);
    return runSwitchAction(selected, store, config, options);
  } catch (const std::exception& e) {
    std::cout << "Switch failed: " << e.what() << std::endl;
    return 1;
  }
}
