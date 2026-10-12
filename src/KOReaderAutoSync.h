#pragma once
#include <Epub.h>

#include <optional>
#include <string>

#include "CrossPointPosition.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"

// Background progress upload for Auto Sync. Used when a book is closed or the
// device goes to sleep, where no sync screen is shown and nothing may block
// for long. Opening a book uses KOReaderSyncActivity instead, because it has
// to apply a remote position.
namespace koreaderautosync {

// Everything the upload needs, captured while the Epub is still loaded so the
// Epub can be released before the TLS handshake.
struct PendingPush {
  std::string epubPath;
  CrossPointPosition position{};
  SavedProgressPosition koPosition{};
  std::optional<KOReaderMetadata> metadata;
};

// Cleared at boot when the firmware reopens a book on its own (silent reboot
// after a WiFi session, or wake from sleep) so that open does not sync again;
// the reader sets it back after that one open.
inline bool openSyncAllowed = true;

// Auto Sync is on, sync credentials and a saved WiFi network exist, the battery
// can afford a WiFi join, and a recent attempt did not just find no network.
bool available();

// Starts a short pause in which available() is false. Called when a join or a
// request fails, so being away from home does not cost a wait on every book.
void noteNetworkFailure();

PendingPush capture(Epub& epub, std::string epubPath, const CrossPointPosition& position,
                    SavedProgressPosition koPosition);

// Joins the last used saved network, uploads only if the local position is
// ahead of the server copy, then switches the radio off. Time is bounded by
// per-stage timeouts, not a hard limit (typically a few seconds). Never shows UI and never reports failure: the next sync
// retries. Returns true when WiFi and TLS were used, so the caller should
// restart to clear heap fragmentation (unless the device is going to sleep).
bool push(PendingPush&& pending);

}  // namespace koreaderautosync
