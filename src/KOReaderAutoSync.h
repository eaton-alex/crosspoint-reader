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

// The open-time sync and the leave/sleep upload pause independently, so a failed
// join on open does not silence the upload when the book is closed.
enum class Channel : uint8_t { Open, Leave };

// Auto Sync is on, sync credentials and a saved WiFi network exist, the battery
// can afford a WiFi join, and this channel did not just fail to reach a network.
bool available(Channel channel);

// Starts a short pause in which available(channel) is false. Called when a join
// or a request fails, so being away from home does not cost a wait on every book.
void noteNetworkFailure(Channel channel);

// False for a book that has not been read yet; there is nothing to upload.
bool worthPushing(float percentage);

PendingPush capture(Epub& epub, std::string epubPath, const CrossPointPosition& position,
                    SavedProgressPosition koPosition);

// Joins the last used saved network, uploads only if the local position is
// ahead of the server copy, then switches the radio off. Time is bounded by
// per-stage timeouts, not a hard limit (typically a few seconds). Never reports
// failure: the next sync retries. Returns true when WiFi and TLS were used, so the caller should
// restart to clear heap fragmentation (unless the device is going to sleep).
bool push(PendingPush&& pending);

}  // namespace koreaderautosync
