#include "ccs.hpp"

#include <cstdlib>

#include <unistd.h>

// ------------------------------- color -------------------------------------

bool supportsColor() {
  static int cached = -1;
  if (cached < 0) {
    const char* noColor = std::getenv("NO_COLOR");
    cached = (isatty(fileno(stdout)) && !(noColor && *noColor)) ? 1 : 0;
  }
  return cached == 1;
}

std::string colorize(const std::string& text, const char* code) {
  if (!supportsColor()) return text;
  return std::string("\x1b[") + code + "m" + text + "\x1b[0m";
}

// --------------------------- display names ---------------------------------

static bool isSuspiciousDisplayName(const std::string& value) {
  // U+FFFD replacement char (UTF-8: EF BF BD) or two-plus '?' marks.
  if (value.find("\xEF\xBF\xBD") != std::string::npos) return true;
  int q = 0;
  for (char c : value) if (c == '?') ++q;
  return q >= 2;
}

std::string getPreferredDisplayName(const json& metadata) {
  std::string displayName = jx::str(metadata, "displayName");
  if (!displayName.empty() && !isSuspiciousDisplayName(displayName)) return displayName;

  std::string email = jx::str(metadata, "emailAddress");
  if (!email.empty()) {
    size_t at = email.find('@');
    if (at != std::string::npos && at > 0) return email.substr(0, at);
    return email;
  }
  return "(no display name)";
}

std::string getEntryLabel(const json& entry) {
  std::string alias = jx::str(entry, "alias");
  if (!alias.empty()) return alias;
  const json* metadata = jx::member(entry, "metadata");
  return getPreferredDisplayName(metadata ? *metadata : json::object());
}

// ---------------------------- plan inference -------------------------------

std::string inferPlanType(const json& entry) {
  const json* metadataPtr = jx::member(entry, "metadata");
  json metadata = metadataPtr ? *metadataPtr : json::object();
  const json* creds = jx::member(entry, "credentials");
  const json* oauthPtr = creds ? jx::member(*creds, "claudeAiOauth") : nullptr;
  json oauth = oauthPtr ? *oauthPtr : json::object();

  std::string subscriptionType = jx::str(oauth, "subscriptionType");
  std::string rateLimitTier = jx::str(oauth, "rateLimitTier");

  if (subscriptionType == "team")
    return rateLimitTier == "default_claude_max_5x" ? "Team Premium" : "Team Standard";
  if (subscriptionType == "enterprise") return "Enterprise";
  if (subscriptionType == "pro") return "Pro";
  if (subscriptionType == "max") return "Max";

  bool hasOrgScope = !jx::str(metadata, "organizationRole").empty() ||
                     !jx::str(metadata, "workspaceRole").empty();
  std::string billingType = jx::str(metadata, "billingType");
  if (hasOrgScope) return billingType == "stripe_subscription" ? "Teams" : "Enterprise";
  if (metadata.contains("hasExtraUsageEnabled") && metadata["hasExtraUsageEnabled"].is_boolean() &&
      metadata["hasExtraUsageEnabled"].get<bool>())
    return "Max";
  if (billingType == "stripe_subscription") return "Pro";
  return "Unknown";
}

std::string getCompactPlanLabel(const json& entry) {
  std::string plan = inferPlanType(entry);
  if (plan == "Team Premium") return "Team Prem";
  if (plan == "Team Standard") return "Team Std";
  if (plan == "Enterprise") return "Ent";
  if (plan == "Unknown") return "Unk";
  return plan;
}

// --------------------------- relative time ---------------------------------

std::string formatRelativeTime(const std::string& iso) {
  if (iso.empty()) return "never";
  long long ms = parseIsoMillis(iso);
  if (ms < 0) return "never";
  long long diff = nowMillis() - ms;
  if (diff < 0) return "just now";
  long long minutes = diff / 60000;
  if (minutes < 1) return "just now";
  if (minutes < 60) return std::to_string(minutes) + "m ago";
  long long hours = minutes / 60;
  if (hours < 24) return std::to_string(hours) + "h ago";
  return std::to_string(hours / 24) + "d ago";
}

// ---------------------------- account summary ------------------------------

// Truncate to maxLen Unicode code points, appending an ellipsis when cut.
static std::string truncateCodepoints(const std::string& text, size_t maxLen) {
  size_t count = 0, i = 0;
  std::vector<size_t> starts;
  while (i < text.size()) {
    starts.push_back(i);
    unsigned char c = (unsigned char)text[i];
    if (c < 0x80) i += 1;
    else if ((c >> 5) == 0x6) i += 2;
    else if ((c >> 4) == 0xE) i += 3;
    else if ((c >> 3) == 0x1E) i += 4;
    else i += 1;
    ++count;
  }
  if (count <= maxLen) return text;
  size_t cut = starts[maxLen - 1];
  return text.substr(0, cut) + "\xE2\x80\xA6";  // U+2026 …
}

std::vector<std::string> formatAccountSummary(const std::vector<json>& accounts) {
  std::vector<std::string> lines;
  for (const auto& entry : accounts) {
    bool current = entry.contains("current") && entry["current"].is_boolean() &&
                   entry["current"].get<bool>();
    std::string marker = current ? "*" : " ";
    std::string displayName = truncateCodepoints(getEntryLabel(entry), 18);
    std::string plan = getCompactPlanLabel(entry);
    std::string lastUsed = formatRelativeTime(jx::str(entry, "lastUsedAt"));
    std::string usageColumns = getUsageColumns(entry);
    int index = entry.contains("index") ? entry["index"].get<int>() : 0;
    lines.push_back(marker + " [" + std::to_string(index) + "] " + displayName + " | " + plan +
                    " | " + usageColumns + " | used:" + lastUsed);
  }
  return lines;
}

// ------------------------------- messages ----------------------------------

namespace msg {

std::vector<std::string> listGuidance(const std::string& usageCommand) {
  return {
      "Run " + usageCommand + " <index> to make one of these stored entries the active Claude account.",
      "Run " + usageCommand + " --remove <index> to remove a stored account.",
  };
}

std::string restartNotice() { return "Note: Restart Claude Code to apply the account change."; }
std::string availableAccountsHeading() { return "Available Claude accounts:"; }
std::string storedAccountsHeading() { return "Stored account list:"; }
std::string remainingAccountsHeading() { return "Remaining accounts:"; }

std::string runningSessionsWarning(int count) {
  return "Warning: " + std::to_string(count) +
         " running Claude Code process(es) detected. They may rewrite credentials after the "
         "switch; close them and restart Claude Code once the switch completes.";
}

std::string refreshProgress(int index) {
  return "Stored access token for [" + std::to_string(index) +
         "] is expired or expiring soon - refreshing...";
}

std::string refreshSuccess() { return "Token refreshed."; }

std::vector<std::string> switchAbortedLines(const std::string& code, const std::string& reason,
                                            const std::string& label,
                                            const std::string& usageCommand) {
  std::vector<std::string> lines;
  if (code == "refresh-expired" || code == "revoked") {
    lines.push_back("Switch aborted: the stored credentials for " + label +
                    " are no longer usable (" + reason + ").");
    lines.push_back("Your current live login was left untouched.");
    lines.push_back("Recover that account by logging into it in Claude Code (/login), then run '" +
                    usageCommand + " sync' to re-capture it.");
  } else if (code == "rate-limited") {
    lines.push_back("Switch aborted: " + reason + ".");
    lines.push_back("Your current live login was left untouched. Try again in a few minutes.");
  } else {
    lines.push_back("Switch aborted: " + reason + ".");
    lines.push_back("Your current live login was left untouched.");
  }
  return lines;
}

std::string syncSkippedWarning(const std::string& reason) { return "Warning: " + reason; }

}  // namespace msg
