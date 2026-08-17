// Live credential storage backend.
//
// Claude Code stores the OAuth blob differently per platform:
//   Linux  — ~/.claude/.credentials.json, a plain 0600 JSON file
//   macOS  — a generic password in the login Keychain, service
//            "Claude Code-credentials", whose value is the very same JSON
//            document ({"claudeAiOauth": {...}})
//
// Because the payload is identical, everything above this file keeps working on
// the parsed JSON and only the read/write pair changes.
#include "ccs.hpp"

#include <stdexcept>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <unistd.h>
#endif

// The service name Claude Code registers the item under.
static constexpr const char* kKeychainService = "Claude Code-credentials";

bool credentialsUseKeychain(const Options& o) {
#ifdef __APPLE__
  // An explicit --credentials path means the caller wants that file, not the
  // Keychain (used by tests and by anyone inspecting a copied credentials file).
  return !o.credentialsPathExplicit;
#else
  (void)o;
  return false;
#endif
}

#ifdef __APPLE__

namespace {

// RAII for the CoreFoundation objects below: every Create/Copy result must be
// released exactly once, including on the throw paths.
template <typename T>
class CFRef {
 public:
  explicit CFRef(T ref = nullptr) : ref_(ref) {}
  ~CFRef() { if (ref_) CFRelease(ref_); }
  CFRef(const CFRef&) = delete;
  CFRef& operator=(const CFRef&) = delete;
  T get() const { return ref_; }
  T* slot() { return &ref_; }
  explicit operator bool() const { return ref_ != nullptr; }

 private:
  T ref_;
};

CFStringRef cfString(const std::string& s) {
  return CFStringCreateWithBytes(nullptr, (const UInt8*)s.data(), (CFIndex)s.size(),
                                 kCFStringEncodingUTF8, false);
}

std::string fromCFString(CFStringRef s) {
  if (!s) return "";
  CFIndex len = CFStringGetLength(s);
  CFIndex max = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingUTF8) + 1;
  std::string out((size_t)max, '\0');
  if (!CFStringGetCString(s, out.data(), max, kCFStringEncodingUTF8)) return "";
  out.resize(std::strlen(out.c_str()));
  return out;
}

std::string statusMessage(OSStatus status) {
  CFRef<CFStringRef> msg(SecCopyErrorMessageString(status, nullptr));
  std::string text = fromCFString(msg.get());
  if (text.empty()) return "OSStatus " + std::to_string((long)status);
  return text + " (OSStatus " + std::to_string((long)status) + ")";
}

// Claude Code files the item under the login account name.
std::string loginAccountName() {
  const char* user = std::getenv("USER");
  if (user && *user) return user;
  struct passwd* pw = getpwuid(getuid());
  if (pw && pw->pw_name) return pw->pw_name;
  return "";
}

// Base query matching the item: service always, account only when known. A
// service-only query is the fallback for items filed under another account name
// (for example when $USER differs from the name Claude Code used).
CFMutableDictionaryRef baseQuery(const std::string& account) {
  CFMutableDictionaryRef q = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                       &kCFTypeDictionaryValueCallBacks);
  CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
  CFRef<CFStringRef> service(cfString(kKeychainService));
  CFDictionarySetValue(q, kSecAttrService, service.get());
  if (!account.empty()) {
    CFRef<CFStringRef> acct(cfString(account));
    CFDictionarySetValue(q, kSecAttrAccount, acct.get());
  }
  return q;
}

struct KeychainRead {
  OSStatus status = errSecItemNotFound;
  std::string payload;
  std::string account;  // the account attribute the match was filed under
};

KeychainRead keychainRead(const std::string& account) {
  KeychainRead result;
  CFRef<CFMutableDictionaryRef> query(baseQuery(account));
  CFDictionarySetValue(query.get(), kSecReturnData, kCFBooleanTrue);
  CFDictionarySetValue(query.get(), kSecReturnAttributes, kCFBooleanTrue);
  CFDictionarySetValue(query.get(), kSecMatchLimit, kSecMatchLimitOne);

  CFRef<CFTypeRef> found;
  result.status = SecItemCopyMatching(query.get(), found.slot());
  if (result.status != errSecSuccess || !found) return result;
  if (CFGetTypeID(found.get()) != CFDictionaryGetTypeID()) {
    result.status = errSecInvalidData;
    return result;
  }

  CFDictionaryRef attrs = (CFDictionaryRef)found.get();
  CFDataRef data = (CFDataRef)CFDictionaryGetValue(attrs, kSecValueData);
  if (!data) {
    result.status = errSecInvalidData;
    return result;
  }
  result.payload.assign((const char*)CFDataGetBytePtr(data), (size_t)CFDataGetLength(data));

  CFStringRef acct = (CFStringRef)CFDictionaryGetValue(attrs, kSecAttrAccount);
  result.account = acct ? fromCFString(acct) : account;
  return result;
}

// Reads the item, trying the login account name first and then service-only.
KeychainRead keychainReadAny() {
  std::string login = loginAccountName();
  if (!login.empty()) {
    KeychainRead byAccount = keychainRead(login);
    if (byAccount.status == errSecSuccess) return byAccount;
  }
  return keychainRead("");
}

void keychainWrite(const std::string& account, const std::string& payload) {
  CFRef<CFDataRef> value(CFDataCreate(nullptr, (const UInt8*)payload.data(),
                                      (CFIndex)payload.size()));
  if (!value) throw std::runtime_error("out of memory while encoding credentials");

  {
    CFRef<CFMutableDictionaryRef> query(baseQuery(account));
    CFRef<CFMutableDictionaryRef> update(CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    CFDictionarySetValue(update.get(), kSecValueData, value.get());
    OSStatus status = SecItemUpdate(query.get(), update.get());
    if (status == errSecSuccess) return;
    if (status != errSecItemNotFound) {
      throw std::runtime_error("cannot update the Keychain item for " +
                               std::string(kKeychainService) + ": " + statusMessage(status));
    }
  }

  // No item yet. Unreachable through the switch flow (readLiveCredentials
  // already refused with "log in first"), but kept so the write is complete on
  // its own: the item can disappear between the read and the write.
  CFRef<CFMutableDictionaryRef> add(baseQuery(account.empty() ? loginAccountName() : account));
  CFDictionarySetValue(add.get(), kSecValueData, value.get());
  // Claude Code labels its item with the service name; matching that keeps a
  // self-created item indistinguishable in Keychain Access and to label lookups.
  CFRef<CFStringRef> label(cfString(kKeychainService));
  CFDictionarySetValue(add.get(), kSecAttrLabel, label.get());
  OSStatus status = SecItemAdd(add.get(), nullptr);
  if (status != errSecSuccess) {
    throw std::runtime_error("cannot create the Keychain item for " +
                             std::string(kKeychainService) + ": " + statusMessage(status));
  }
}

json parseCredentials(const std::string& payload) {
  try {
    return json::parse(payload);
  } catch (...) {
    // Never echo the payload: it holds token bytes.
    throw std::runtime_error("Failed to parse the Keychain credentials: invalid JSON.");
  }
}

}  // namespace

#endif  // __APPLE__

// --------------------------------- reads -----------------------------------

json readLiveCredentials(const Options& o) {
  if (!credentialsUseKeychain(o)) return readJson(o.credentialsPath);
#ifdef __APPLE__
  KeychainRead read = keychainReadAny();
  if (read.status == errSecItemNotFound) {
    throw std::runtime_error("no Claude Code credentials in the macOS Keychain (service \"" +
                             std::string(kKeychainService) +
                             "\"). Log in with /login in Claude Code first.");
  }
  if (read.status != errSecSuccess) {
    throw std::runtime_error("cannot read the Keychain item for " + std::string(kKeychainService) +
                             ": " + statusMessage(read.status));
  }
  return parseCredentials(read.payload);
#else
  return readJson(o.credentialsPath);  // unreachable
#endif
}

// --------------------------------- writes ----------------------------------

// Only claudeAiOauth belongs to this tool; sibling keys Claude Code may add to
// the credentials document must survive a switch.
static json mergeCredentials(const json& existing, const json& credentials) {
  if (!existing.is_object()) return credentials;
  json next = existing;
  next["claudeAiOauth"] =
      credentials.contains("claudeAiOauth") ? credentials.at("claudeAiOauth") : json();
  return next;
}

void writeLiveCredentials(const json& credentials, const Options& o) {
  if (!credentialsUseKeychain(o)) {
    backupFile(o.credentialsPath, o.backupDir);
    json existing;
    try {
      existing = readJsonIfExists(o.credentialsPath, json());
    } catch (...) {
      existing = json();
    }
    writeJsonAtomic(o.credentialsPath, mergeCredentials(existing, credentials), 0600);
    return;
  }

#ifdef __APPLE__
  KeychainRead read = keychainReadAny();

  // Anything other than success or a missing item (a denied prompt,
  // errSecUserCanceled, unreadable data) must abort before the item is touched.
  // Treating it as "nothing there" would drop the sibling keys the merge is for
  // and overwrite the live tokens with no snapshot taken.
  if (read.status != errSecSuccess && read.status != errSecItemNotFound) {
    throw std::runtime_error("cannot read the Keychain item for " +
                             std::string(kKeychainService) +
                             " before writing it: " + statusMessage(read.status));
  }

  if (read.status == errSecSuccess && !o.backupDir.empty()) {
    // Snapshot the raw bytes, not the parsed document: an unparseable value is
    // exactly when a recoverable copy matters most, and parsing it would throw.
    const std::string base = ".credentials.keychain.json";
    ensureDir(o.backupDir);
    std::string dest = pathJoin(o.backupDir, base + "." + backupTimestamp() + ".bak");
    try {
      writeTextAtomic(dest, read.payload, 0600);
      pruneBackups(o.backupDir, base);
    } catch (...) {
      // A failed backup must not block the switch; the store already holds this
      // account's credentials.
    }
  }

  json existing;
  if (read.status == errSecSuccess) {
    try {
      existing = parseCredentials(read.payload);
    } catch (...) {
      // Unparseable: treat it as absent and let the merge write a clean
      // document. The raw original is in the backup taken above.
      existing = json();
    }
  }

  // Compact, no trailing newline: that is byte-for-byte the shape Claude Code
  // itself stores, and it keeps `security find-generic-password -w` printing the
  // value as text rather than falling back to a hex dump.
  json next = mergeCredentials(existing, credentials);
  keychainWrite(read.status == errSecSuccess ? read.account : loginAccountName(), next.dump());
#endif
}
