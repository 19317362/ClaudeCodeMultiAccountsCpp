#include "ccs.hpp"

#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#ifndef CCS_VERSION
#define CCS_VERSION "0.3.10"
#endif

static bool isAllDigits(const std::string& s) {
  if (s.empty()) return false;
  for (char c : s) if (c < '0' || c > '9') return false;
  return true;
}

static void printHelp() {
  std::cout << "Claude Code Multi-Account Switcher (C++)\n\n"
            << "Usage:\n"
            << "  ccs                 List stored accounts (and switch with an index)\n"
            << "  ccs <index>         Switch the active Claude account\n"
            << "  ccs sync            Capture the current live account into the store\n"
            << "  ccs usage           Fetch usage from the Claude API\n"
            << "  ccs --remove <i>    Remove a stored account\n"
            << "  ccs --rename <i> [name]  Set or clear an account alias\n"
            << "  ccs --show-usage | --hide-usage\n"
            << "  ccs install         Install the binary, hooks, and slash commands\n"
            << "  ccs uninstall       Remove installed binaries, hooks, and commands\n"
            << "  ccs --version       Print the version\n";
}

// Mirrors parseArgs() from the Node cc-switch.cjs.
static Options parseArgs(const std::vector<std::string>& argv) {
  Options options;
  options.configPath = getDefaultConfigPath();
  options.credentialsPath = getDefaultCredentialsPath();
  options.storePath = getDefaultStorePath();
  options.backupDir = getDefaultBackupDir();

  json settings = readSettings();
  options.showUsage = !(settings.contains("showUsage") && settings["showUsage"].is_boolean() &&
                        settings["showUsage"].get<bool>() == false);

  size_t i = 0;
  auto next = [&](const std::string& fallback) -> std::string {
    if (i < argv.size()) return argv[i++];
    return fallback;
  };

  while (i < argv.size()) {
    std::string cur = argv[i++];
    if (cur == "--usage-command") { options.usageCommand = next(options.usageCommand); continue; }
    if (cur == "--config") { options.configPath = next(options.configPath); continue; }
    if (cur == "--credentials") {
      // Only a flag that actually named a path selects the file backend: a
      // trailing "--credentials" consumes nothing, and forcing the file backend
      // onto the default path would fail on macOS, where that file never exists.
      bool hasValue = i < argv.size();
      options.credentialsPath = next(options.credentialsPath);
      if (hasValue) options.credentialsPathExplicit = true;
      continue;
    }
    if (cur == "--store") { options.storePath = next(options.storePath); continue; }
    if (cur == "--backup-dir") { options.backupDir = next(options.backupDir); continue; }
    if (cur == "--sync" || cur == "sync") { options.syncOnly = true; continue; }
    if (cur == "--touch-current") { options.touchCurrentOnly = true; continue; }
    if (cur == "--usage" || cur == "usage") { options.usageOnly = true; continue; }
    if (cur == "--remove" || cur == "remove") { options.removeOnly = true; continue; }
    if (cur == "--rename" || cur == "rename") { options.renameOnly = true; continue; }
    if (cur == "--show-usage") {
      settings["showUsage"] = true;
      writeSettings(settings);
      std::cout << "Usage display enabled." << std::endl;
      options.showUsage = true;
      break;
    }
    if (cur == "--hide-usage") {
      settings["showUsage"] = false;
      writeSettings(settings);
      std::cout << "Usage display disabled." << std::endl;
      options.showUsage = false;
      break;
    }
    if (options.removeOnly && options.removeIndex < 0 && isAllDigits(cur)) {
      options.removeIndex = std::stoi(cur);
      continue;
    }
    if (options.renameOnly) {
      if (options.renameIndex < 0 && isAllDigits(cur)) {
        options.renameIndex = std::stoi(cur);
        continue;
      }
      options.renameAliasParts.push_back(cur);
      continue;
    }
    if (options.selector.empty()) {
      options.selector = cur;
      continue;
    }
  }
  return options;
}

int main(int argc, char** argv) {
  std::string invoked = baseName(argv[0]);
  std::vector<std::string> args;
  for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

  // Top-level subcommands handled outside the switch flow.
  // --help / -h / help and --version / -v / version anywhere on the command
  // line print their output and exit.
  for (const auto& a : args) {
    if (a == "help" || a == "--help" || a == "-h") { printHelp(); return 0; }
    if (a == "version" || a == "--version" || a == "-v") {
      std::cout << "ccs " << CCS_VERSION << std::endl;
      return 0;
    }
  }

  if (!args.empty()) {
    const std::string& c = args[0];
    if (c == "install") return runInstall();
    if (c == "uninstall") return runUninstall();
    if (c == "session-start") return runSessionStart();
    if (c == "statusline") return runStatusline();
  }

  // Name-based defaults so a single binary behaves per its invoked name.
  std::string defaultUsageCommand;
  bool forceSync = false;
  if (invoked == "ccs") defaultUsageCommand = "ccs";
  else if (invoked == "cc-switch") defaultUsageCommand = "cc-switch";
  else if (invoked == "cc-sync-oauth" || invoked == "ccso") forceSync = true;

  Options options = parseArgs(args);
  if (forceSync) options.syncOnly = true;
  if (!defaultUsageCommand.empty() && options.usageCommand == "/switch")
    options.usageCommand = defaultUsageCommand;

  return runMainFlow(options);
}
