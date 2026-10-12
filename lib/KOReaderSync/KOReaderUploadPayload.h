#pragma once
#include <Epub.h>

#include <optional>
#include <string>

#include "CrossPointPosition.h"
#include "KOReaderSyncClient.h"
#include "ProgressMapper.h"

// Pieces of a progress upload shared by the manual sync screen and Auto Sync,
// so both send the same payload for a given server profile.
namespace koreaderupload {

// CrossPoint's rich position extension. Callers send it only when
// KOREADER_STORE.supportsRichProgress().
KOReaderRichPosition richPosition(const CrossPointPosition& position, const SavedProgressPosition& koPosition);

// Document metadata for a server that takes it. `epub` may be null, in which
// case only the filename and plugin sidecar are included.
KOReaderMetadata metadata(Epub* epub, const std::string& epubPath);

}  // namespace koreaderupload
