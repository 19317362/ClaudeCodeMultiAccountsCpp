#include "ccs.hpp"

#include <cmath>
#include <cstdio>

// ------------------------------- fetch -------------------------------------

UsageResult fetchUsage(const std::string& accessToken) {
  std::vector<std::string> headers = {
      "Authorization: Bearer " + accessToken,
      "anthropic-beta: oauth-2025-04-20",
      "anthropic-version: 2023-06-01",
  };
  HttpResponse res = httpGet("https://api.anthropic.com/api/oauth/usage", headers);

  UsageResult out;
  if (res.networkError) {
    out.error = "Usage API unreachable: " + res.errorMessage;
    return out;
  }
  if (res.status == 429) {
    out.rateLimited = true;
    out.ok = true;
    auto it = res.headers.find("retry-after");
    if (it != res.headers.end()) {
      try {
        out.retryAfter = std::stol(it->second);
        if (out.retryAfter > 0) setRateLimitResetAt(out.retryAfter);
      } catch (...) {
        out.retryAfter = -1;
      }
    }
    return out;
  }
  if (res.status != 200) {
    out.error = "Usage API returned " + std::to_string(res.status);
    return out;
  }
  try {
    out.data = json::parse(res.body);
  } catch (...) {
    out.error = "Failed to parse usage response.";
    return out;
  }
  const json* seven = jx::member(out.data, "seven_day");
  if (seven) {
    std::string resetsAt = jx::str(*seven, "resets_at");
    if (!resetsAt.empty()) setRateLimitResetAtFromIso(resetsAt);
  }
  out.ok = true;
  return out;
}

// ---------------------------- top usage block ------------------------------

// utilization is the used fraction; show it directly (used, not remaining).
static std::string usedPct(const json& window) {
  if (jx::isFiniteNum(window, "utilization")) {
    double util = jx::num(window, "utilization", 0);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f", util);
    return buf;
  }
  return "N/A";
}

std::vector<std::string> formatUsageInfo(const UsageResult& usage) {
  std::vector<std::string> lines;
  if (usage.rateLimited) {
    long retrySecs = usage.retryAfter;
    if (retrySecs > 0) {
      long long resetAt = nowMillis() + retrySecs * 1000;
      long h = retrySecs / 3600;
      long m = (retrySecs % 3600) / 60;
      long s = retrySecs % 60;
      std::string countdown;
      if (h > 0) countdown = "~" + std::to_string(h) + "h " + std::to_string(m) + "m";
      else if (m > 0) countdown = "~" + std::to_string(m) + "m " + std::to_string(s) + "s";
      else countdown = "~" + std::to_string(s) + "s";
      lines.push_back("Usage API is rate limited. Resets in " + countdown + " (at " +
                      formatLocalTime(resetAt) + ").");
    } else {
      lines.push_back("Usage API is rate limited. Try again in a few seconds.");
    }
    return lines;
  }
  const json* five = jx::member(usage.data, "five_hour");
  if (five) {
    std::string resetsAt = "unknown";
    std::string iso = jx::str(*five, "resets_at");
    if (!iso.empty()) {
      long long ms = parseIsoMillis(iso);
      if (ms > 0) resetsAt = formatLocalDateTime(ms);
    }
    lines.push_back("5h used/reset: " + usedPct(*five) + "% / " + resetsAt);
  }
  const json* seven = jx::member(usage.data, "seven_day");
  if (seven) {
    std::string resetsAt = "unknown";
    std::string iso = jx::str(*seven, "resets_at");
    if (!iso.empty()) {
      long long ms = parseIsoMillis(iso);
      if (ms > 0) resetsAt = formatLocalDateTime(ms);
    }
    lines.push_back("7d used/reset: " + usedPct(*seven) + "% / " + resetsAt);
  }
  if (lines.empty()) lines.push_back("No usage data available for this account.");
  return lines;
}

// --------------------------- snapshots -------------------------------------

static json toUsageSnapshot(const UsageResult& usage) {
  if (!usage.ok || usage.rateLimited || usage.data.is_null()) return json();
  const json* five = jx::member(usage.data, "five_hour");
  const json* seven = jx::member(usage.data, "seven_day");
  bool hasFive = five && (jx::isFiniteNum(*five, "utilization") || jx::has(*five, "resets_at"));
  bool hasSeven = seven && (jx::isFiniteNum(*seven, "utilization") || jx::has(*seven, "resets_at"));
  if (!hasFive && !hasSeven) return json();

  json snap = json::object();
  if (five) {
    json w = json::object();
    if (jx::has(*five, "utilization")) w["utilization"] = (*five)["utilization"];
    if (jx::has(*five, "resets_at")) w["resets_at"] = (*five)["resets_at"];
    snap["five_hour"] = w;
  }
  if (seven) {
    json w = json::object();
    if (jx::has(*seven, "utilization")) w["utilization"] = (*seven)["utilization"];
    if (jx::has(*seven, "resets_at")) w["resets_at"] = (*seven)["resets_at"];
    snap["seven_day"] = w;
  }
  snap["fetchedAt"] = isoNow();
  return snap;
}

// Heuristic for seatless/non-usable accounts: the API often reports 100% used
// with an immediate reset while the cached snapshot still has a sensible value.
static bool shouldKeepExistingSnapshot(const json& existing, const json& next, bool isCurrent) {
  if (isCurrent || existing.is_null() || next.is_null()) return false;
  const json* five = jx::member(next, "five_hour");
  if (!five) return false;
  double util = jx::num(*five, "utilization", -1);
  std::string resetIso = jx::str(*five, "resets_at");
  long long reset = resetIso.empty() ? -1 : parseIsoMillis(resetIso);
  long long now = nowMillis();
  if (util == 100 && reset > 0 && reset <= now + 60 * 1000) return true;
  return false;
}

SnapshotRefresh refreshStoredUsageSnapshots(json& store, const std::string& currentKey) {
  SnapshotRefresh result;
  if (!store.contains("accounts") || !store["accounts"].is_array()) return result;

  for (auto& entry : store["accounts"]) {
    if (!entry.contains("credentials") || !entry["credentials"].is_object()) continue;
    if (!entry["credentials"].contains("claudeAiOauth") ||
        !entry["credentials"]["claudeAiOauth"].is_object())
      continue;
    json oauth = entry["credentials"]["claudeAiOauth"];  // work on a copy
    std::string accessToken = jx::str(oauth, "accessToken");
    if (accessToken.empty()) continue;

    // Refresh an expired/expiring access token first, so every account reports
    // fresh usage + reset times — not only the currently-active one. Rotated
    // tokens are persisted back into the store (the caller writes it out).
    if (assessCredentials(oauth, nowMillis()).verdict == "need-refresh") {
      RefreshResult refreshed = refreshTokens(oauth);
      if (refreshed.ok) {
        entry["credentials"]["claudeAiOauth"] = refreshed.claudeAiOauth;
        oauth = refreshed.claudeAiOauth;
        accessToken = jx::str(oauth, "accessToken");
        result.changed = true;
      }
    }

    UsageResult usage = fetchUsage(accessToken);
    std::string key = jx::str(entry, "key");
    bool isCurrent = (key == currentKey);
    if (isCurrent && usage.ok) {
      // Matches the Node behavior: a fetch failure leaves currentUsage unset so
      // the caller falls back to cached values instead of printing an error.
      result.currentUsage = usage;
      result.hasCurrent = true;
    }
    if (!usage.ok) continue;  // network/parse failure: keep previous snapshot
    json nextSnapshot = toUsageSnapshot(usage);
    if (nextSnapshot.is_null()) continue;

    json existing = jx::has(entry, "usageSnapshot") ? entry["usageSnapshot"] : json();
    if (shouldKeepExistingSnapshot(existing, nextSnapshot, isCurrent)) continue;
    if (existing.is_null() ? true : existing.dump() != nextSnapshot.dump()) {
      if (existing.dump() != nextSnapshot.dump()) {
        entry["usageSnapshot"] = nextSnapshot;
        result.changed = true;
      }
    }
  }
  return result;
}

// ---------------------------- column formatting ----------------------------

// Show the used percentage (utilization) directly. Colors escalate with usage:
// red once heavily used, yellow past the halfway mark.
static std::string formatUsagePercentPtr(const json* window) {
  if (!window || !jx::isFiniteNum(*window, "utilization")) return "?";
  double value = jx::num(*window, "utilization", 0);
  long used = (long)std::lround(value);
  if (used < 0) used = 0;
  if (used > 100) used = 100;
  std::string text = std::to_string(used) + "%";
  if (used >= 90) return colorize(text, "31");
  if (used >= 50) return colorize(text, "33");
  return text;
}

static std::string formatDurationUntil(long long millis) {
  if (millis < 0) return "?";
  long long diff = millis - nowMillis();
  if (diff <= 0) return "now";
  long long totalHours = diff / 3600000;
  if (totalHours >= 24) {
    long long days = totalHours / 24;
    long long hours = totalHours % 24;
    return std::to_string(days) + "D " + std::to_string(hours) + "h";
  }
  long long minutes = (diff % 3600000) / 60000;
  return "~" + std::to_string(totalHours) + "h " + std::to_string(minutes) + "min";
}

std::string getUsageColumns(const json& entry) {
  json usage = jx::has(entry, "usageSnapshot") ? entry["usageSnapshot"] : json::object();
  bool current = entry.contains("current") && entry["current"].is_boolean() && entry["current"].get<bool>();
  long long rateLimitReset = current ? getRateLimitResetAt() : -1;

  const json* five = jx::member(usage, "five_hour");
  const json* seven = jx::member(usage, "seven_day");

  std::string fiveHourPct = formatUsagePercentPtr(five);
  long long fiveResetMs = -1;
  if (five) {
    std::string iso = jx::str(*five, "resets_at");
    if (!iso.empty()) fiveResetMs = parseIsoMillis(iso);
  }
  std::string fiveHourReset = formatDurationUntil(fiveResetMs);

  std::string sevenDayPct = formatUsagePercentPtr(seven);
  long long sevenResetMs = rateLimitReset;
  if (sevenResetMs < 0 && seven) {
    std::string iso = jx::str(*seven, "resets_at");
    if (!iso.empty()) sevenResetMs = parseIsoMillis(iso);
  }
  std::string sevenDayReset = formatDurationUntil(sevenResetMs);

  return "5H:" + fiveHourPct + " (" + fiveHourReset + ") | 7D:" + sevenDayPct + " (" +
         sevenDayReset + ")";
}
