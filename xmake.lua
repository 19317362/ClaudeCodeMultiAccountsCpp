-- Claude Code Multi-Account Switcher — C++ single-binary build.
-- Third-party dependencies are fetched by xrepo.
set_project("ccs")
set_version("0.3.10")

set_languages("c++17")
set_warnings("all")
add_rules("mode.debug", "mode.release")

-- Default to release when the user does not pass a mode.
if not is_mode("debug") then
    set_symbols("hidden")
    set_optimize("fastest")
    set_strip("all")
end

-- nlohmann_json: header-only JSON. libcurl(+openssl): HTTPS for token refresh
-- and the usage API.
--
-- system = false forces xrepo to build these from source and link them
-- statically, so the produced binary does not depend on whichever libcurl /
-- TLS flavor happens to be installed on the host — the goal is a single,
-- self-contained executable.
-- libcurl is pinned: 8.21.0+ requires OpenSSL 3.x (OPENSSL_VERSION_STRING),
-- which does not build against the 1.1.1 series resolved above.
add_requires("nlohmann_json")
add_requires("openssl", {system = false})
add_requires("libcurl 8.11.0", {system = false, configs = {openssl = true}})

target("ccs")
    set_kind("binary")
    add_defines("CCS_VERSION=\"0.3.10\"")
    add_files("src/*.cpp")
    add_packages("nlohmann_json", "libcurl", "openssl")
    if is_plat("linux") then
        add_syslinks("pthread", "dl")
    end
    if is_plat("macosx") then
        -- Security/CoreFoundation: reading and writing the Claude Code OAuth blob
        -- in the login Keychain, which is where macOS keeps it instead of
        -- ~/.claude/.credentials.json (see src/creds.cpp).
        add_frameworks("Security", "CoreFoundation")
    end
target_end()
