#pragma once

// The PURE, host-testable reconcile core of the sync engine. Mirrors how
// src/calibre/CalibreClient.h splits networking (the CalibreClient class) from
// pure logic (namespace calibreparser) so the pure half can be unit tested
// off-device with the test/support Arduino String shim -- no WiFi, no SD, no
// clock.
//
// The on-device orchestrator lives in src/sync/CalibreSyncManager.{h,cpp}
// (the analog of src/sync/CompanionSyncManager.{h,cpp}); it calls
// calibresync::computeSyncPlan() to decide what to download and what to delete,
// then performs the I/O.
//
// IMPORTANT: this header must compile on the host. It includes only <Arduino.h>
// (satisfied by the real core on-device, and by test/support/Arduino.h on the
// host) plus <vector>. Do NOT add SD_MMC / WiFi / HTTPClient includes here.

#include <Arduino.h>

#include <vector>

namespace calibresync {

// The deletion policy shared with the CalibreSettings struct. We avoid taking a
// hard compile dependency on CalibreSettings.h here (so this header stays
// trivially host-buildable and CalibreSettings can evolve independently); the
// on-device CalibreSyncManager maps CalibreSettings::DeletionPolicy onto this
// enum. The enumerator order/names match the CalibreSettings contract:
//   enum DeletionPolicy { Mirror, Keep }
enum class DeletionPolicy : uint8_t {
  Mirror = 0,  // remote is authoritative: ids dropped from search are deleted
  Keep = 1,    // never delete local files even when they leave search scope
};

// One book as it currently exists on the Calibre server (post resolveRsvp()).
// key is the opaque change-key compared as a RAW string -- the device has no
// reliable clock, so we never parse mtime into an epoch; we just compare the
// concatenation of (size + mtime) byte-for-byte.
struct RemoteEntry {
  int id = 0;
  String key;    // e.g. "123456|2024-05-06T07:08:09.528479+00:00"
  String url;    // absolute (or base-relative) download URL for the .rsvp
  String title;  // best-effort title used for the on-SD filename
  String path;   // desired absolute SD path, decided by the caller from the
                 // book's tags (see CalibreSyncManager::destinationPath). Left
                 // EMPTY by callers that do not route -- the core then ignores
                 // the path axis entirely and diffs on the change-key alone.
};

// One book as recorded in the on-SD manifest (/books/.calibre-sync.json).
struct ManifestEntry {
  int id = 0;
  String key;   // the change-key captured when this file was last downloaded
  String path;  // absolute SD path of the downloaded .rsvp file
};

// A book that must be (re)downloaded: either new (no manifest entry) or its
// change-key differs from what we have on SD.
struct DownloadAction {
  int id = 0;
  String key;
  String url;
  String title;
  String path;          // where the download must land (may differ from the
                        // manifest path when the book was retagged)
  String previousPath;  // where it currently sits, "" when the book is new;
                        // the caller removes this file once the new one lands
};

// A book that must be removed: present in the manifest but absent from the
// latest search, under DeletionPolicy::Mirror.
struct DeleteAction {
  int id = 0;
  String path;
};

// A book whose bytes are unchanged but which belongs in a different folder than
// where it currently sits -- the retag case: adding "article" in Calibre routes
// the book from /library/books to /library/articles without touching the .rsvp,
// so the change-key is identical and a download would be pure waste. The file
// (and its sidecars) is renamed instead.
struct MoveAction {
  int id = 0;
  String key;  // unchanged; carried so the manifest rewrite keeps it
  String from;
  String to;
  String title;
};

// The reconcile result. unchanged is reported for observability/logging.
struct SyncPlan {
  std::vector<DownloadAction> toDownload;
  std::vector<MoveAction> toMove;
  std::vector<DeleteAction> toDelete;
  std::vector<int> unchanged;  // ids whose key matched the manifest
};

// Finds a manifest entry by id. Returns nullptr when absent.
inline const ManifestEntry *findManifestEntry(
    const std::vector<ManifestEntry> &manifest, int id) {
  for (const ManifestEntry &entry : manifest) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

// Returns true when id appears anywhere in remote.
inline bool remoteHasId(const std::vector<RemoteEntry> &remote, int id) {
  for (const RemoteEntry &entry : remote) {
    if (entry.id == id) {
      return true;
    }
  }
  return false;
}

// The pure reconcile: diff the remote view against the on-SD manifest.
//
//   * id in remote, not in manifest                     -> toDownload (new)
//   * id in both, keys differ (raw string compare)      -> toDownload (changed)
//   * id in both, keys equal, paths differ              -> toMove (retagged)
//   * id in both, keys equal, same path                 -> unchanged
//   * id in manifest, not in remote, policy == Mirror   -> toDelete
//   * id in manifest, not in remote, policy == Keep     -> (left alone)
//
// The path axis is only consulted when the caller populated RemoteEntry::path.
// An empty path means "caller does no folder routing", and the diff collapses
// to the original key-only behaviour.
//
// No I/O, no clock, no allocation beyond the output vectors.
inline SyncPlan computeSyncPlan(const std::vector<RemoteEntry> &remote,
                                const std::vector<ManifestEntry> &manifest,
                                DeletionPolicy policy) {
  SyncPlan plan;

  // Pass 1: walk the remote view, deciding download vs move vs unchanged.
  for (const RemoteEntry &r : remote) {
    const ManifestEntry *existing = findManifestEntry(manifest, r.id);
    // A routed path that matches nothing on SD only matters while the bytes are
    // current; a changed key re-downloads to the new location anyway.
    const bool routed = !r.path.isEmpty();
    const bool relocated = routed && existing != nullptr && existing->path != r.path;

    if (existing != nullptr && existing->key == r.key) {
      if (!relocated) {
        plan.unchanged.push_back(r.id);
        continue;
      }
      MoveAction move;
      move.id = r.id;
      move.key = r.key;
      move.from = existing->path;
      move.to = r.path;
      move.title = r.title;
      plan.toMove.push_back(move);
      continue;
    }

    DownloadAction action;
    action.id = r.id;
    action.key = r.key;
    action.url = r.url;
    action.title = r.title;
    action.path = r.path;
    // Only worth reporting when it actually differs -- otherwise the caller
    // would delete the file it just wrote.
    if (relocated) {
      action.previousPath = existing->path;
    }
    plan.toDownload.push_back(action);
  }

  // Pass 2: reconcile deletions (only under Mirror).
  if (policy == DeletionPolicy::Mirror) {
    for (const ManifestEntry &m : manifest) {
      if (!remoteHasId(remote, m.id)) {
        DeleteAction action;
        action.id = m.id;
        action.path = m.path;
        plan.toDelete.push_back(action);
      }
    }
  }

  return plan;
}

}  // namespace calibresync
