#include "ccs.hpp"

static const char* OAUTH_CLIENT_ID = "9d1c250a-e61b-44d9-88ed-5944d1962f5e";

// The refresh response only carries access_token/refresh_token/expires_in;
// refreshTokenExpiresAt, scopes, subscriptionType, rateLimitTier must survive.
static json mergeRefreshedTokens(const json& claudeAiOauth, const json& tokenResponse, long long now) {
  json merged = claudeAiOauth;
  merged["accessToken"] = tokenResponse.value("access_token", std::string());
  if (tokenResponse.contains("refresh_token") && tokenResponse["refresh_token"].is_string()) {
    merged["refreshToken"] = tokenResponse["refresh_token"];
  }
  if (tokenResponse.contains("expires_in") && tokenResponse["expires_in"].is_number()) {
    double expiresIn = tokenResponse["expires_in"].get<double>();
    if (expiresIn > 0) merged["expiresAt"] = (double)now + expiresIn * 1000.0;
  }
  return merged;
}

RefreshResult refreshTokens(const json& claudeAiOauth) {
  const std::vector<std::string> endpoints = {
      "https://platform.claude.com/v1/oauth/token",
      "https://api.anthropic.com/v1/oauth/token",
  };

  RefreshResult fallbackFailure{false, "", "", json()};
  bool haveFallback = false;

  json payload = json::object();
  payload["grant_type"] = "refresh_token";
  payload["refresh_token"] = jx::str(claudeAiOauth, "refreshToken");
  payload["client_id"] = OAUTH_CLIENT_ID;

  for (const auto& endpoint : endpoints) {
    HttpResponse res = httpPostJson(endpoint, payload, {});
    if (res.networkError) {
      fallbackFailure = {false, "network", "Token endpoint unreachable: " + res.errorMessage, json()};
      haveFallback = true;
      continue;
    }
    if (res.status == 200) {
      json parsed;
      try {
        parsed = json::parse(res.body);
      } catch (...) {
        return {false, "protocol", "Token endpoint returned an unparsable response.", json()};
      }
      if (!parsed.contains("access_token") || !parsed["access_token"].is_string() ||
          parsed["access_token"].get<std::string>().empty()) {
        return {false, "protocol", "Token endpoint response is missing access_token.", json()};
      }
      return {true, "", "", mergeRefreshedTokens(claudeAiOauth, parsed, nowMillis())};
    }
    if (res.status == 400 || res.status == 401) {
      return {false, "revoked", "the stored refresh token was rejected (revoked or rotated)", json()};
    }
    if (res.status == 429) {
      return {false, "rate-limited", "the token endpoint is rate limiting requests", json()};
    }
    if (res.status == 404 || res.status == 403 || res.status >= 500) {
      fallbackFailure = {false, "protocol",
                         "token endpoint returned " + std::to_string(res.status), json()};
      haveFallback = true;
      continue;
    }
    return {false, "protocol", "token endpoint returned " + std::to_string(res.status), json()};
  }

  if (haveFallback) return fallbackFailure;
  return {false, "network", "no token endpoint could be reached", json()};
}
