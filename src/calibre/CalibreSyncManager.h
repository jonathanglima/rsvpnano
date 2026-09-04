#pragma once

// The on-device reconciling sync engine. Mirrors src/sync/CompanionSyncManager.
// {h,cpp} for structure, logging style ("[calibre-sync] ...") and hand-rolled
// String JSON (this repo does NOT use ArduinoJson -- see the note in
// src/calibre/CalibreClient.h).
//
// Flow (see runSync()):
//   read CalibreSettings
//     -> CalibreClient.search(query)
//     -> CalibreClient.resolveRsvp(id, ref)
//     -> calibresync::computeSyncPlan(...)          (pure core, host-tested)
//     -> net::get(url, sink) streaming to SD, or a rename for a retag-only move
//     -> rewrite /books/.calibre-sync.json manifest
//     -> StorageManager::refreshBooks() reindex
//
// The pure reconcile logic lives in src/calibre/CalibreSyncPlan.h so it can be
// unit tested on the host (test/calibre/test_sync_plan.cpp).

#include <Arduino.h>

#include <functional>
#include <vector>

#include "calibre/CalibreClient.h"
#include "calibre/CalibreSettings.h"
#include "network/HttpFetch.h"
#include "calibre/CalibreSyncPlan.h"

class StorageManager;

class CalibreSyncManager {
 public:
  // Progress callback, modelled on StorageManager::StatusCallback and the
  // status lines CompanionSyncManager surfaces. phase is a short tag
  // ("search", "download", "delete", "done", "error"); current/total drive a
  // percentage; detail is a human-readable line (e.g. a book title).
  using ProgressCallback = std::function<void(const String &phase, int current,
                                              int total, const String &detail)>;

  struct Result {
    bool ok = false;
    int downloaded = 0;
    int moved = 0;      // retagged books relocated between books/ and articles/
    int deleted = 0;
    int unchanged = 0;
    int failed = 0;     // downloads and moves that errored (counted, sync
                        // continues; a failed move keeps the old path so the
                        // next run retries it)
    String error;       // set when ok == false (a fatal, abort-the-run error)
  };

  // storage may be null (the engine still syncs files; it just skips the
  // reindex). When non-null, refreshBooks() is called once after a successful
  // run so the device library picks up the new/removed files.
  explicit CalibreSyncManager(StorageManager *storage = nullptr)
      : storage_(storage) {}

  void setProgressCallback(ProgressCallback callback) {
    progress_ = std::move(callback);
  }

  // Brings up station WiFi (net::connectStation, using settings.* and the
  // wifi creds), runs one full reconcile against the configured library, then
  // tears the radio down. Returns a Result; never throws. When
  // settings.enabled is false this is a no-op success.
  Result runSync(const CalibreSettings &settings, const String &wifiSsid,
                 const String &wifiPassword);

  // Lower-level entry point: assumes WiFi is already up and does not touch the
  // radio. Useful for callers that manage their own connection (and for the
  // future menu). Performs search -> diff -> download -> delete ->
  // manifest rewrite -> reindex.
  Result reconcile(const CalibreSettings &settings);

  // Manifest location on SD.
  static const char *manifestPath();

  // Parse a manifest JSON body into entries (cleared first). Hand-rolled with
  // the same indexOf/substring idiom as the rest of the repo. Exposed for
  // testing and reuse. Returns true when the "books" object was found (even if
  // empty).
  static bool parseManifest(const String &json,
                            std::vector<calibresync::ManifestEntry> &out);

  // Serialise a manifest. titles is parallel to entries (same index) so the
  // human-friendly title survives a round-trip even though the reconcile core
  // does not need it. Simple String concat, matching settingsJson() etc.
  static String serializeManifest(
      const std::vector<calibresync::ManifestEntry> &entries,
      const std::vector<String> &titles);

 private:
  // Builds the change-key compared as a raw string: size + "|" + mtime. No
  // epoch parsing -- the device has no reliable clock.
  static String changeKey(const CalibreClient::RsvpRef &ref);

  // <routed folder>/<sanitized title or id>.rsvp. Takes the resolved RsvpRef
  // rather than a RemoteEntry because routing reads the book's Calibre tags,
  // which only the ref carries; id is the filename fallback for a book with an
  // empty title.
  static String destinationPath(const CalibreClient::RsvpRef &ref, int id);

  // Folder routing: StoragePaths::kArticleFilesPath for a book tagged
  // kArticleTag, StoragePaths::kBookFilesPath otherwise. The device draws the
  // two differently (BookLibrary::isArticle() keys purely off the
  // /library/articles/ prefix), so the folder IS the article/book distinction.
  static const char *targetDirectoryFor(const CalibreClient::RsvpRef &ref);

  // Renames a book's .rsvp together with its per-path sidecars. Reading
  // progress (.rstate.toml) and the prebuilt index (.ridx/.rdat) are keyed by
  // the document path, so moving only the .rsvp would silently reset the
  // reader's position and force a reindex. Missing sidecars are not an error.
  static bool moveBookFiles(const String &from, const String &to);

  // Deletes a book's .rsvp and the same sidecar set. Used by both the delete
  // phase and the relocate-after-download cleanup.
  static void removeBookFiles(const String &path);

  // sanitize a title/id into a filesystem-safe base name (no extension),
  // mirroring CompanionSyncManager::sanitizeFilename().
  static String sanitizeBaseName(const String &name);

  // Streams url to path via net::get, writing through a .tmp then renaming.
  // Returns true on success.
  bool downloadTo(const String &url, const String &path,
                  const net::HttpAuth &auth);

  void report(const String &phase, int current, int total,
              const String &detail);

  StorageManager *storage_ = nullptr;
  ProgressCallback progress_;
};
