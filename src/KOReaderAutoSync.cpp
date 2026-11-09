#include "KOReaderAutoSync.h"

#include <HalPowerManager.h>
#include <Logging.h>
#include <WiFi.h>

#include <cmath>

#include "KOReaderCredentialStore.h"
#include "KOReaderDocumentId.h"
#include "KOReaderUploadPayload.h"
#include "WifiCredentialStore.h"
#include "network/WifiPowerSaveGuard.h"

namespace koreaderautosync {
namespace {
constexpr unsigned long JOIN_TIMEOUT_MS = 8000;
// Per-stage timeout for each request (connect, headers, body), reset on every
// byte, so this is not a hard cap: a slow server can take well over 15s.
// TOTAL_BUDGET_MS only skips the upload when the join and fetch used it up.
constexpr uint32_t REQUEST_TIMEOUT_MS = 5000;
constexpr unsigned long TOTAL_BUDGET_MS = 15000;
constexpr unsigned long FAILURE_PAUSE_MS = 5UL * 60UL * 1000UL;
constexpr uint16_t MIN_BATTERY_PERCENT = 20;
// Same tolerance as ProgressComparison: closer than this counts as in sync.
constexpr float SAME_PROGRESS_EPSILON = 0.001f;

struct Pause {
  bool active = false;
  unsigned long until = 0;
};
Pause pauses[2];  // indexed by Channel; RAM only, so any restart clears them

Pause& pauseFor(const Channel channel) { return pauses[static_cast<size_t>(channel)]; }

bool joinSavedNetwork() {
  if (WiFi.status() == WL_CONNECTED) return true;
  const auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) return false;

  // Credentials live in WifiCredentialStore; keep the SDK from also writing
  // them to NVS on every join (same as WifiSelectionActivity).
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  // Poll like the sleep-time join in main.cpp. The driver can still report the
  // previous attempt's WL_NO_SSID_AVAIL right after begin(), so only believe
  // "network absent" once the scan has had time to run.
  constexpr unsigned long NO_SSID_GRACE_MS = 3000;
  const unsigned long startMs = millis();
  const unsigned long deadline = startMs + JOIN_TIMEOUT_MS;
  wl_status_t status = WiFi.status();
  while (status != WL_CONNECTED && millis() < deadline) {
    if (status == WL_NO_SSID_AVAIL && millis() - startMs > NO_SSID_GRACE_MS) break;
    delay(100);
    status = WiFi.status();
  }
  LOG_INF("KOSync", "Auto push: join status=%d after %lu ms", static_cast<int>(status), millis() - startMs);
  return status == WL_CONNECTED;
}

void radioOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void logHeap(const char* where) {
  (void)where;  // unused when LOG_DBG compiles out
  LOG_DBG("KOSync", "Auto push heap (%s): free=%u max_block=%u", where, (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
}
}  // namespace

void loadNetworkList() {
  // No SD read for people who do not use Auto Sync.
  if (!KOREADER_STORE.getAutoSync() || !KOREADER_STORE.hasCredentials()) return;
  if (WIFI_STORE.getCredentialCount() == 0) WIFI_STORE.loadFromFile();
}

bool available(const Channel channel) {
  const Pause& pause = pauseFor(channel);
  if (pause.active && static_cast<long>(millis() - pause.until) < 0) return false;
  return KOREADER_STORE.getAutoSync() && KOREADER_STORE.hasCredentials() && WIFI_STORE.getCredentialCount() != 0 &&
         powerManager.getBatteryPercentage() >= MIN_BATTERY_PERCENT;
}

void noteNetworkFailure(const Channel channel) {
  Pause& pause = pauseFor(channel);
  pause.active = true;
  pause.until = millis() + FAILURE_PAUSE_MS;
}

bool worthPushing(const float percentage) { return percentage > SAME_PROGRESS_EPSILON; }

PendingPush capture(Epub& epub, std::string epubPath, const CrossPointPosition& position,
                    SavedProgressPosition koPosition) {
  PendingPush pending;
  pending.position = position;
  pending.koPosition = std::move(koPosition);
  if (KOREADER_STORE.getSendMetadata()) {
    pending.metadata = koreaderupload::metadata(&epub, epubPath);
  }
  pending.epubPath = std::move(epubPath);
  return pending;
}

bool push(PendingPush&& pending) {
  if (!available(Channel::Leave)) {
    LOG_INF("KOSync", "Auto push: not available (setting/credentials/network/battery/pause)");
    return false;
  }
  // A book that was only opened has nothing worth uploading.
  if (!worthPushing(pending.koPosition.percentage)) {
    LOG_INF("KOSync", "Auto push: skipped, nothing read yet");
    return false;
  }

  const std::string documentHash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                                       ? KOReaderDocumentId::calculateFromFilename(pending.epubPath)
                                       : KOReaderDocumentId::calculate(pending.epubPath);
  if (documentHash.empty()) {
    LOG_ERR("KOSync", "Auto push: could not hash %s", pending.epubPath.c_str());
    return false;
  }

  const unsigned long start = millis();
  LOG_INF("KOSync", "Auto push: starting, local=%.6f", pending.koPosition.percentage);
  logHeap("before join");
  if (!joinSavedNetwork()) {
    LOG_INF("KOSync", "Auto push: no saved network joined, pausing auto sync");
    radioOff();
    noteNetworkFailure(Channel::Leave);
    return false;
  }
  logHeap("after join");

  {
    WifiPowerSaveGuard psGuard;
    KOReaderProgress remote;
    const auto got = KOReaderSyncClient::getProgress(documentHash, remote, REQUEST_TIMEOUT_MS);

    bool upload = false;
    if (got == KOReaderSyncClient::NOT_FOUND) {
      upload = true;
    } else if (got == KOReaderSyncClient::OK) {
      // Never overwrite a server position that is ahead of ours.
      upload = std::isfinite(remote.percentage) &&
               pending.koPosition.percentage - remote.percentage > SAME_PROGRESS_EPSILON;
    } else {
      LOG_DBG("KOSync", "Auto push: fetch failed (%d), skipping", static_cast<int>(got));
      if (got == KOReaderSyncClient::NETWORK_ERROR) noteNetworkFailure(Channel::Leave);
    }

    if (upload && millis() - start > TOTAL_BUDGET_MS) {
      LOG_DBG("KOSync", "Auto push: time budget spent, skipping upload");
      upload = false;
    }

    LOG_INF("KOSync", "Auto push: local=%.6f fetch=%d upload=%d", pending.koPosition.percentage,
            static_cast<int>(got), upload ? 1 : 0);
    if (upload) {
      KOReaderProgress progress;
      progress.document = documentHash;
      progress.progress = pending.koPosition.xpath;
      progress.percentage = pending.koPosition.percentage;
      // Same rule as the manual upload: rich position only for server
      // profiles that support the CrossPoint extension.
      if (KOREADER_STORE.supportsRichProgress()) {
        progress.position = koreaderupload::richPosition(pending.position, pending.koPosition);
      }
      progress.metadata = std::move(pending.metadata);
      const auto result = KOReaderSyncClient::updateProgress(progress, REQUEST_TIMEOUT_MS);
      LOG_INF("KOSync", "Auto push: upload result=%d", static_cast<int>(result));
      if (result == KOReaderSyncClient::NETWORK_ERROR) noteNetworkFailure(Channel::Leave);
    }
  }

  radioOff();
  logHeap("after radio off");
  LOG_INF("KOSync", "Auto push took %lu ms", millis() - start);
  return true;
}

}  // namespace koreaderautosync
