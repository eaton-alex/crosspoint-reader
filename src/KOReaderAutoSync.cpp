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

bool pauseActive = false;
unsigned long pauseUntil = 0;

bool joinSavedNetwork() {
  if (WiFi.status() == WL_CONNECTED) return true;
  const auto cred = WIFI_STORE.findCredential(WIFI_STORE.getLastConnectedSsid());
  if (!cred) return false;

  // Credentials live in WifiCredentialStore; keep the SDK from also writing
  // them to NVS on every join (same as WifiSelectionActivity).
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.begin(cred->ssid.c_str(), cred->password.c_str());
  // Give up as soon as the driver says the network is absent, so being away
  // from home costs a couple of seconds rather than the whole timeout.
  const unsigned long deadline = millis() + JOIN_TIMEOUT_MS;
  wl_status_t status = WiFi.status();
  while (status != WL_CONNECTED && status != WL_NO_SSID_AVAIL && status != WL_CONNECT_FAILED && millis() < deadline) {
    delay(100);
    status = WiFi.status();
  }
  return status == WL_CONNECTED;
}

void radioOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

void logHeap(const char* where) {
  LOG_DBG("KOSync", "Auto push heap (%s): free=%u max_block=%u", where, (unsigned)ESP.getFreeHeap(),
          (unsigned)ESP.getMaxAllocHeap());
}
}  // namespace

bool available() {
  if (pauseActive && static_cast<long>(millis() - pauseUntil) < 0) return false;
  return KOREADER_STORE.getAutoSync() && KOREADER_STORE.hasCredentials() && WIFI_STORE.getCredentialCount() != 0 &&
         powerManager.getBatteryPercentage() >= MIN_BATTERY_PERCENT;
}

void noteNetworkFailure() {
  pauseActive = true;
  pauseUntil = millis() + FAILURE_PAUSE_MS;
}

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
  if (!available()) return false;
  // A book that was only opened has nothing worth uploading.
  if (pending.koPosition.percentage <= SAME_PROGRESS_EPSILON) return false;

  const std::string documentHash = KOREADER_STORE.getMatchMethod() == DocumentMatchMethod::FILENAME
                                       ? KOReaderDocumentId::calculateFromFilename(pending.epubPath)
                                       : KOReaderDocumentId::calculate(pending.epubPath);
  if (documentHash.empty()) {
    LOG_ERR("KOSync", "Auto push: could not hash %s", pending.epubPath.c_str());
    return false;
  }

  const unsigned long start = millis();
  logHeap("before join");
  if (!joinSavedNetwork()) {
    LOG_DBG("KOSync", "Auto push: no saved network joined, pausing auto sync");
    radioOff();
    noteNetworkFailure();
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
      if (got == KOReaderSyncClient::NETWORK_ERROR) noteNetworkFailure();
    }

    if (upload && millis() - start > TOTAL_BUDGET_MS) {
      LOG_DBG("KOSync", "Auto push: time budget spent, skipping upload");
      upload = false;
    }

    LOG_DBG("KOSync", "Auto push: local=%.6f fetch=%d upload=%d", pending.koPosition.percentage,
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
      LOG_DBG("KOSync", "Auto push: upload result=%d", static_cast<int>(result));
      if (result == KOReaderSyncClient::NETWORK_ERROR) noteNetworkFailure();
    }
  }

  radioOff();
  logHeap("after radio off");
  LOG_DBG("KOSync", "Auto push took %lu ms", millis() - start);
  return true;
}

}  // namespace koreaderautosync
