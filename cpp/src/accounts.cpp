#include "ccs.hpp"

#include <stdexcept>

static constexpr long long ACCESS_TOKEN_SAFETY_MS = 5LL * 60 * 1000;

std::string getAccountKey(const json& account) {
  std::string uuid = jx::str(account, "accountUuid");
  if (!uuid.empty()) return "uuid:" + jx::toLower(uuid);
  std::string email = jx::str(account, "emailAddress");
  if (!email.empty()) return "email:" + jx::toLower(email);
  throw std::runtime_error("Account entry is missing both accountUuid and emailAddress.");
}

json normalizeStore(json store, const std::string& version) {
  json normalized = store.is_object() ? store : json::object();
  if (!normalized.contains("accounts") || !normalized["accounts"].is_array()) {
    normalized["accounts"] = json::array();
  }
  normalized["version"] = version;
  return normalized;
}

std::vector<json> getDisplayAccounts(const json& store, const json& currentMetadata) {
  std::string currentKey;
  bool haveCurrent = currentMetadata.is_object();
  if (haveCurrent) currentKey = getAccountKey(currentMetadata);

  std::vector<json> out;
  if (!store.contains("accounts") || !store["accounts"].is_array()) return out;
  int index = 0;
  for (const auto& entry : store["accounts"]) {
    json e = entry;
    e["index"] = index;
    bool current = false;
    if (haveCurrent && e.contains("metadata")) {
      try {
        current = getAccountKey(e["metadata"]) == currentKey;
      } catch (...) {
        current = false;
      }
    }
    e["current"] = current;
    out.push_back(std::move(e));
    ++index;
  }
  return out;
}

Assessment assessCredentials(const json& claudeAiOauth, long long now) {
  if (!claudeAiOauth.is_object()) {
    return {"refresh-expired", "stored credentials are missing claudeAiOauth"};
  }
  if (jx::str(claudeAiOauth, "refreshToken").empty()) {
    return {"refresh-expired", "stored credentials have no refresh token"};
  }
  if (jx::isFiniteNum(claudeAiOauth, "refreshTokenExpiresAt")) {
    double refreshExpiresAt = jx::num(claudeAiOauth, "refreshTokenExpiresAt", 0);
    if (refreshExpiresAt > 0 && refreshExpiresAt <= (double)now) {
      return {"refresh-expired", "stored refresh token has expired"};
    }
  }
  bool hasAccess = !jx::str(claudeAiOauth, "accessToken").empty();
  if (!hasAccess || !jx::isFiniteNum(claudeAiOauth, "expiresAt")) {
    return {"need-refresh", "stored access token is expired or expiring soon"};
  }
  double accessExpiresAt = jx::num(claudeAiOauth, "expiresAt", 0);
  if (accessExpiresAt <= (double)now + (double)ACCESS_TOKEN_SAFETY_MS) {
    return {"need-refresh", "stored access token is expired or expiring soon"};
  }
  return {"ok", "stored access token is still valid"};
}

// Local files cannot prove which account a token belongs to; the detectable
// poisoning case is live tokens that verbatim match a different stored slot.
IdentityCheck verifyLiveIdentity(const json& config, const json& credentials, const json& store) {
  if (!config.is_object() || !jx::has(config, "oauthAccount")) {
    return {false, "The Claude config does not contain oauthAccount."};
  }
  const json* oauth = jx::member(credentials, "claudeAiOauth");
  std::string access = oauth ? jx::str(*oauth, "accessToken") : "";
  std::string refresh = oauth ? jx::str(*oauth, "refreshToken") : "";
  if (!oauth || access.empty() || refresh.empty()) {
    return {false, "The Claude credentials file has no usable claudeAiOauth tokens."};
  }
  std::string liveKey = getAccountKey(config.at("oauthAccount"));
  if (store.contains("accounts") && store["accounts"].is_array()) {
    for (const auto& entry : store["accounts"]) {
      std::string entryKey = jx::str(entry, "key");
      if (entryKey.empty() || entryKey == liveKey) continue;
      const json* stored = jx::member(entry, "credentials");
      const json* storedOauth = stored ? jx::member(*stored, "claudeAiOauth") : nullptr;
      if (!storedOauth) continue;
      std::string sRefresh = jx::str(*storedOauth, "refreshToken");
      std::string sAccess = jx::str(*storedOauth, "accessToken");
      if ((!sRefresh.empty() && sRefresh == refresh) || (!sAccess.empty() && sAccess == access)) {
        return {false,
                "Live credentials match the stored tokens of a different account (" + entryKey +
                    "). Skipping sync to avoid corrupting the store."};
      }
    }
  }
  return {true, ""};
}

SyncResult syncStoreFromLive(const json& store, const json& config, const json& credentials,
                             const std::string& version) {
  if (!jx::has(config, "oauthAccount")) {
    throw std::runtime_error("The Claude config does not contain oauthAccount.");
  }
  if (!jx::has(credentials, "claudeAiOauth")) {
    throw std::runtime_error("The Claude credentials file does not contain claudeAiOauth.");
  }

  std::string key = getAccountKey(config.at("oauthAccount"));

  IdentityCheck identity = verifyLiveIdentity(config, credentials, store);
  if (!identity.ok) {
    SyncResult r;
    r.changed = false;
    r.store = normalizeStore(store, version);
    r.key = key;
    r.skipped = true;
    r.warning = identity.reason;
    return r;
  }

  std::string now = isoNow();
  // Carry forward existing bookkeeping for this account.
  const json* existingEntry = nullptr;
  if (store.contains("accounts") && store["accounts"].is_array()) {
    for (const auto& e : store["accounts"]) {
      if (jx::str(e, "key") == key) { existingEntry = &e; break; }
    }
  }

  json snapshot = json::object();
  snapshot["key"] = key;
  snapshot["metadata"] = config.at("oauthAccount");
  snapshot["credentials"] = credentials;
  snapshot["capturedAt"] = now;
  snapshot["lastSyncedAt"] = now;
  if (existingEntry && jx::has(*existingEntry, "lastUsedAt"))
    snapshot["lastUsedAt"] = (*existingEntry)["lastUsedAt"];
  if (existingEntry && jx::has(*existingEntry, "usageSnapshot"))
    snapshot["usageSnapshot"] = (*existingEntry)["usageSnapshot"];
  if (existingEntry && jx::has(*existingEntry, "alias"))
    snapshot["alias"] = (*existingEntry)["alias"];

  json nextStore = normalizeStore(store, version);
  bool replaced = false;
  for (auto& e : nextStore["accounts"]) {
    if (jx::str(e, "key") == key) { e = snapshot; replaced = true; break; }
  }
  if (!replaced) nextStore["accounts"].push_back(snapshot);
  nextStore["updatedAt"] = isoNow();

  SyncResult r;
  r.changed = store.dump() != nextStore.dump();
  r.store = nextStore;
  r.key = key;
  return r;
}

json findSelection(const std::vector<json>& accounts, const std::string& selector) {
  std::string trimmed = jx::trim(selector);
  if (trimmed.empty()) throw std::runtime_error("Selector cannot be empty.");

  // Numeric index only, matching the Node behavior.
  bool numeric = !trimmed.empty();
  for (char c : trimmed) if (c < '0' || c > '9') numeric = false;
  if (numeric) {
    int idx = std::stoi(trimmed);
    for (const auto& e : accounts) {
      if (e.contains("index") && e["index"].get<int>() == idx) return e;
    }
  }
  throw std::runtime_error("No account matched index '" + trimmed + "'. Use a numeric index.");
}

json removeStoredAccount(json& store, int removeIndex) {
  int count = (store.contains("accounts") && store["accounts"].is_array())
                  ? (int)store["accounts"].size()
                  : 0;
  if (removeIndex < 0 || removeIndex >= count) {
    throw std::runtime_error("Invalid account index. Use an index between 0 and " +
                             std::to_string(count - 1) + ".");
  }
  json removed = store["accounts"][removeIndex];
  store["accounts"].erase(store["accounts"].begin() + removeIndex);
  int i = 0;
  for (auto& e : store["accounts"]) e["index"] = i++;
  return removed;
}
