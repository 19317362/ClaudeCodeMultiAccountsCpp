#include "ccs.hpp"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>

long long nowMillis() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string isoNow() {
  long long ms = nowMillis();
  time_t secs = (time_t)(ms / 1000);
  int millis = (int)(ms % 1000);
  struct tm tm{};
  gmtime_r(&secs, &tm);
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, millis);
  return buf;
}

std::string backupTimestamp() {
  long long ms = nowMillis();
  time_t secs = (time_t)(ms / 1000);
  struct tm tm{};
  gmtime_r(&secs, &tm);
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
  return buf;
}

long long parseIsoMillis(const std::string& s) {
  if (s.empty()) return -1;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0, consumed = 0;
  int fields = std::sscanf(s.c_str(), "%d-%d-%dT%d:%d:%d%n",
                           &y, &mo, &d, &h, &mi, &sec, &consumed);
  if (fields < 6) {
    // Try a space separator or a bare date.
    fields = std::sscanf(s.c_str(), "%d-%d-%d %d:%d:%d%n",
                         &y, &mo, &d, &h, &mi, &sec, &consumed);
    if (fields < 6) {
      h = mi = sec = 0;
      fields = std::sscanf(s.c_str(), "%d-%d-%d%n", &y, &mo, &d, &consumed);
      if (fields < 3) return -1;
    }
  }
  struct tm tm{};
  tm.tm_year = y - 1900;
  tm.tm_mon = mo - 1;
  tm.tm_mday = d;
  tm.tm_hour = h;
  tm.tm_min = mi;
  tm.tm_sec = sec;
  time_t t = timegm(&tm);
  long long ms = (long long)t * 1000;
  const char* p = s.c_str() + consumed;
  if (*p == '.') {
    ++p;
    int mult = 100, frac = 0;
    while (*p >= '0' && *p <= '9') {
      if (mult >= 1) { frac += (*p - '0') * mult; mult /= 10; }
      ++p;
    }
    ms += frac;
  }
  if (*p == '+' || *p == '-') {
    int sign = (*p == '+') ? 1 : -1;
    ++p;
    int oh = 0, om = 0;
    std::sscanf(p, "%d:%d", &oh, &om);
    ms -= (long long)sign * (oh * 3600 + om * 60) * 1000;
  }
  return ms;
}

static std::string formatLocal(long long millis, bool timeOnly) {
  time_t secs = (time_t)(millis / 1000);
  struct tm tm{};
  localtime_r(&secs, &tm);
  char buf[48];
  if (timeOnly) {
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min, tm.tm_sec);
  } else {
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
  }
  return buf;
}

std::string formatLocalDateTime(long long millis) { return formatLocal(millis, false); }
std::string formatLocalTime(long long millis) { return formatLocal(millis, true); }
