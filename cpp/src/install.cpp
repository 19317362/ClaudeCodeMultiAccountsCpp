#include "ccs.hpp"

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>

#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

static std::string selfExePath() {
  std::error_code ec;
  fs::path p = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) return p.string();
  return "";
}

static std::string installRootDir() { return getToolConfigDir(); }
static std::string installedBinPath() { return pathJoin(pathJoin(installRootDir(), "bin"), "ccs"); }
static std::string userBinDir() { return pathJoin(homeDir(), pathJoin(".local", "bin")); }
static std::string claudeSettingsPath() {
  return pathJoin(pathJoin(homeDir(), ".claude"), "settings.json");
}
static std::string commandsDir() { return pathJoin(pathJoin(homeDir(), ".claude"), "commands"); }
static std::string statuslineTargetPath() {
  return pathJoin(installRootDir(), "statusline-target.json");
}

static void copyExecutable(const std::string& from, const std::string& to) {
  std::error_code ec;
  ensureDir(dirName(to));
  fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
  if (ec) throw std::runtime_error("failed to copy binary to " + to + ": " + ec.message());
  ::chmod(to.c_str(), 0755);
}

static std::string quote(const std::string& s) { return "'" + s + "'"; }

// -------------------------------- install ----------------------------------

int runInstall() {
  std::string self = selfExePath();
  if (self.empty()) {
    std::cerr << "Install failed: cannot resolve the running binary path." << std::endl;
    return 1;
  }

  const std::string bin = installedBinPath();
  copyExecutable(self, bin);

  // Install name-dispatched copies so cc-switch/ccs list and cc-sync-oauth/ccso sync.
  const std::string ub = userBinDir();
  for (const char* name : {"cc-switch", "ccs", "cc-sync-oauth", "ccso"}) {
    copyExecutable(self, pathJoin(ub, name));
  }

  const std::string syncCmd = quote(bin) + " sync";
  const std::string sessionStartCmd = quote(bin) + " session-start";
  const std::string statuslineCmd = quote(bin) + " statusline";

  // ---- Claude settings.json: hooks + statusline ----
  const std::string settingsPath = claudeSettingsPath();
  const std::string backupDir =
      pathJoin(pathJoin(homeDir(), ".claude"), pathJoin("backups", "multi-account-switch-installer"));
  backupFile(settingsPath, backupDir);

  json settings = readJsonIfExists(settingsPath, json::object());
  if (!settings.is_object()) settings = json::object();
  if (!settings.contains("$schema"))
    settings["$schema"] = "https://json.schemastore.org/claude-code-settings.json";
  if (!settings.contains("hooks") || !settings["hooks"].is_object()) settings["hooks"] = json::object();
  json& hooks = settings["hooks"];
  if (!hooks.contains("Notification") || !hooks["Notification"].is_array())
    hooks["Notification"] = json::array();
  if (!hooks.contains("SessionStart") || !hooks["SessionStart"].is_array())
    hooks["SessionStart"] = json::array();

  // Preserve a pre-existing statusline command as our downstream target.
  const json* existingStatusLine = jx::member(settings, "statusLine");
  if (existingStatusLine && jx::str(*existingStatusLine, "type") == "command") {
    std::string cmd = jx::str(*existingStatusLine, "command");
    if (!cmd.empty() && cmd != statuslineCmd) {
      ensureDir(installRootDir());
      std::ofstream out(statuslineTargetPath(), std::ios::binary | std::ios::trunc);
      out << existingStatusLine->dump(2) << "\n";
    }
  }
  settings["statusLine"] = json{{"type", "command"}, {"command", statuslineCmd}};

  auto ensureMatcherHook = [](json& arr, const std::string& matcherName, const std::string& command) {
    json* matcher = nullptr;
    for (auto& m : arr) {
      if (m.is_object() && jx::str(m, "matcher") == matcherName) { matcher = &m; break; }
    }
    if (!matcher) {
      arr.push_back(json{{"matcher", matcherName}, {"hooks", json::array()}});
      matcher = &arr.back();
    }
    if (!(*matcher).contains("hooks") || !(*matcher)["hooks"].is_array())
      (*matcher)["hooks"] = json::array();
    for (const auto& h : (*matcher)["hooks"]) {
      if (h.is_object() && jx::str(h, "command") == command) return;  // already present
    }
    (*matcher)["hooks"].push_back(json{{"type", "command"}, {"shell", "bash"}, {"command", command}});
  };

  ensureMatcherHook(hooks["Notification"], "auth_success", syncCmd);
  ensureMatcherHook(hooks["SessionStart"], "startup", sessionStartCmd);

  writeJsonAtomic(settingsPath, settings, 0);

  // ---- Slash commands ----
  const std::string cdir = commandsDir();
  ensureDir(cdir);
  auto writeCommand = [&](const std::string& file, const std::string& desc,
                          const std::string& argHint, const std::string& body) {
    std::ofstream out(pathJoin(cdir, file), std::ios::binary | std::ios::trunc);
    out << "---\n";
    out << "description: " << desc << "\n";
    if (!argHint.empty()) out << "argument-hint: " << argHint << "\n";
    out << "allowed-tools: [\"Bash(" << bin << ":*)\"]\n";
    out << "disable-model-invocation: true\n";
    out << "---\n\n";
    out << "Run the installed local command and use its output as the command result.\n\n";
    out << "```!\n" << body << "\n```\n";
  };
  writeCommand("cc-switch.md",
               "Show saved Claude OAuth accounts and switch oauthAccount by index.",
               "[index]", quote(bin) + " --usage-command \"/cc-switch\" $ARGUMENTS");
  writeCommand("cc-sync-oauth.md", "Sync the current Claude oauthAccount into the store.", "",
               syncCmd);

  std::cout << "Installed multi-account switcher binary to " << bin << std::endl;
  std::cout << "Installed commands into " << ub << ": cc-switch, ccs, cc-sync-oauth, ccso"
            << std::endl;
  std::cout << "Configured Claude hooks and statusline in " << settingsPath << std::endl;
  std::cout << "Make sure " << ub << " is on your PATH." << std::endl;
  return 0;
}

// ------------------------------- uninstall ---------------------------------

int runUninstall() {
  std::error_code ec;
  const std::string bin = installedBinPath();
  fs::remove(bin, ec);
  const std::string ub = userBinDir();
  for (const char* name : {"cc-switch", "ccs", "cc-sync-oauth", "ccso"}) {
    fs::remove(pathJoin(ub, name), ec);
  }
  fs::remove(pathJoin(commandsDir(), "cc-switch.md"), ec);
  fs::remove(pathJoin(commandsDir(), "cc-sync-oauth.md"), ec);

  const std::string settingsPath = claudeSettingsPath();
  if (pathExists(settingsPath)) {
    try {
      json settings = readJson(settingsPath);
      if (settings.contains("hooks") && settings["hooks"].is_object()) {
        json& hooks = settings["hooks"];
        auto stripOurHooks = [&](const char* section) {
          if (!hooks.contains(section) || !hooks[section].is_array()) return;
          for (auto& matcher : hooks[section]) {
            if (!matcher.is_object() || !matcher.contains("hooks") || !matcher["hooks"].is_array())
              continue;
            json kept = json::array();
            for (auto& h : matcher["hooks"]) {
              std::string cmd = jx::str(h, "command");
              if (cmd.find(installedBinPath()) == std::string::npos) kept.push_back(h);
            }
            matcher["hooks"] = kept;
          }
        };
        stripOurHooks("Notification");
        stripOurHooks("SessionStart");
      }
      const json* sl = jx::member(settings, "statusLine");
      if (sl && jx::str(*sl, "command").find(installedBinPath()) != std::string::npos) {
        settings.erase("statusLine");
      }
      writeJsonAtomic(settingsPath, settings, 0);
    } catch (const std::exception& e) {
      std::cerr << "Warning: could not clean settings.json: " << e.what() << std::endl;
    }
  }

  std::cout << "Removed multi-account switcher binaries, commands, and hook entries." << std::endl;
  std::cout << "Stored accounts in " << getDefaultStorePath() << " were left untouched." << std::endl;
  return 0;
}

// ----------------------------- session-start -------------------------------

int runSessionStart() {
  // Touch the current account marker (best-effort), then print the reminder.
  try {
    Options options;
    options.configPath = getDefaultConfigPath();
    options.credentialsPath = getDefaultCredentialsPath();
    options.storePath = getDefaultStorePath();
    options.backupDir = getDefaultBackupDir();
    options.touchCurrentOnly = true;
    // Swallow all output/errors from the touch so startup stays quiet.
    std::ios::sync_with_stdio(true);
    runMainFlow(options);
  } catch (...) {
  }
  std::cout << "Claude Code Multi-Account Switcher is available." << std::endl;
  std::cout << "Use !cc-switch or !ccs to list/switch accounts." << std::endl;
  std::cout << "Use !cc-sync-oauth or !ccso to sync the active account into oauthList." << std::endl;
  return 0;
}

// ------------------------------- statusline --------------------------------

int runStatusline() {
  // Read (and ignore) stdin JSON that Claude feeds the statusline command.
  std::string input((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
  std::string label = "use !ccs";

  std::string downstream;
  json target = readJsonIfExists(statuslineTargetPath(), json());
  std::string cmd = jx::str(target, "command");
  if (!cmd.empty()) {
    std::string full = cmd + " 2>/dev/null";
    std::array<char, 4096> buf{};
    if (FILE* pipe = popen(full.c_str(), "r")) {
      // Feed nothing to the downstream; just capture what it prints.
      std::string out;
      while (std::fgets(buf.data(), (int)buf.size(), pipe)) out += buf.data();
      pclose(pipe);
      while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
      if (out.find("Plugin not installed") == std::string::npos &&
          out.find("[OMC HUD]") == std::string::npos)
        downstream = out;
    }
  }

  if (downstream.empty()) {
    std::cout << label << std::endl;
  } else {
    size_t nl = downstream.find('\n');
    std::string firstLine = downstream.substr(0, nl);
    std::string rest = (nl == std::string::npos) ? "" : downstream.substr(nl);
    std::cout << label << " \xE2\x94\x82 " << firstLine << rest << std::endl;
  }
  return 0;
}
