#include "KOReaderUploadPayload.h"

#include <HalStorage.h>
#include <Logging.h>

#include "KOReaderCredentialStore.h"

namespace koreaderupload {

KOReaderRichPosition richPosition(const CrossPointPosition& position, const SavedProgressPosition& koPosition) {
  KOReaderRichPosition pos;
  const float pct = koPosition.percentage < 0.0f ? 0.0f : koPosition.percentage > 1.0f ? 1.0f : koPosition.percentage;
  pos.pctQ = static_cast<uint32_t>(pct * 1000000.0f + 0.5f);
  pos.spineIndex = static_cast<uint16_t>(position.spineIndex);
  pos.pageNumber = static_cast<uint16_t>(position.pageNumber);
  pos.totalPages = static_cast<uint16_t>(position.totalPages > 0 ? position.totalPages : 1);
  if (position.hasParagraphIndex) {
    pos.paragraphIndex = position.paragraphIndex;
  }
  pos.xpath = koPosition.xpath;
  return pos;
}

KOReaderMetadata metadata(Epub* epub, const std::string& epubPath) {
  KOReaderMetadata meta;
  const auto lastSlash = epubPath.rfind('/');
  meta.filename = (lastSlash != std::string::npos) ? epubPath.substr(lastSlash + 1) : epubPath;
  if (epub) {
    meta.title = epub->getTitle();
    meta.authors = epub->getAuthor();
    if (KOREADER_STORE.supportsExtendedMetadata()) {
      Epub::SyncMetadata syncMetadata;
      if (epub->loadSyncMetadata(syncMetadata)) {
        meta.isbn = std::move(syncMetadata.isbn);
        meta.asin = std::move(syncMetadata.asin);
        meta.series = std::move(syncMetadata.series);
        meta.seriesIndex = syncMetadata.seriesIndex;
      } else {
        LOG_DBG("KOSync", "Could not read extended EPUB metadata; sending core metadata only");
      }
    }
  } else {
    LOG_ERR("KOSync", "Epub unavailable for metadata; sending filename only");
  }
  // Plugin sidecar fields ("<book>.meta.json", written at download time via
  // the catalog sidecar mechanism or /api/plugin-fs) ride along so a custom
  // sync server can route progress by a service book id.
  // A missing sidecar (the common case) leaves extraJson empty.
  Storage.readFileToString("KOSync", epubPath + ".meta.json", 2 * 1024, meta.extraJson);
  return meta;
}

}  // namespace koreaderupload
