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
// host) plus standard headers. Do NOT add SD_MMC / WiFi / HTTPClient includes
// here; card access is injected (see resolvePathCollisions).

#include <Arduino.h>

#include <cctype>
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
  bool keepFile = false;  // the file is shared with a book that stays (left by
                          // an earlier filename collision): forget the entry,
                          // keep the bytes
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
  // ids whose action would touch the book open in the reader. Nothing is done
  // for them this run; their manifest entries survive untouched, so the next
  // sync recomputes the same action once the book is closed.
  std::vector<int> deferred;
};

// FAT compares names case-insensitively, so paths that differ only in case
// name the same file on the card.
inline bool samePath(const String &a, const String &b) {
  const char *x = a.c_str();
  const char *y = b.c_str();
  for (; *x != '\0' && *y != '\0'; ++x, ++y) {
    if (std::tolower(static_cast<unsigned char>(*x)) !=
        std::tolower(static_cast<unsigned char>(*y))) {
      return false;
    }
  }
  return *x == *y;
}

// "/library/books/Poems.rsvp" + 12 -> "/library/books/Poems (12).rsvp". Calibre
// ids are unique, so suffixed names cannot collide with each other, and
// sanitizeBaseName turns parentheses into '-', so no clean title-derived name
// has this shape either (a book titled "Poems (12)" is "Poems -12-.rsvp").
inline String withIdSuffix(const String &path, int id) {
  const String suffix = String(" (") + String(id) + ")";
  const int slash = path.lastIndexOf('/');
  const int dot = path.lastIndexOf('.');
  if (dot <= slash) {
    return path + suffix;
  }
  return path.substring(0, dot) + suffix + path.substring(dot);
}

// True when another manifest entry records the same file -- the residue of a
// filename collision from before resolvePathCollisions existed. Whatever bytes
// are there belong to whichever book downloaded last.
inline bool sharesFile(const std::vector<ManifestEntry> &manifest,
                       const ManifestEntry &entry) {
  for (const ManifestEntry &other : manifest) {
    if (other.id != entry.id && samePath(other.path, entry.path)) {
      return true;
    }
  }
  return false;
}

// True when some remote book will live at path after this sync.
inline bool targetedByRemote(const std::vector<RemoteEntry> &remote,
                             const String &path) {
  for (const RemoteEntry &r : remote) {
    if (!r.path.isEmpty() && samePath(r.path, path)) {
      return true;
    }
  }
  return false;
}

// Gives every routed book a file of its own. destinationPath() names files by
// sanitized title alone, so two books can route to one file: equal titles,
// titles that differ only in accents or case, or titles equal up to the length
// cap. One book keeps the clean name; the others get withIdSuffix().
//
// The clean name goes to, in order:
//   1. a book the manifest already records there (lowest id if several), so
//      adding a same-titled book never renames the one already on the card;
//   2. nobody, when the manifest records it for a book that is not claiming
//      it (deleted, retagged elsewhere, or kept by the Keep policy) or a file
//      the sync did not write exists there (companion upload, RSS article);
//   3. otherwise the lowest claiming id -- independent of search order, so the
//      choice is stable from one sync to the next.
//
// existsOnCard(path) reports whether a file exists; injected so this stays
// host-testable. Entries with an empty path (unrouted callers) are untouched.
template <typename ExistsOnCard>
void resolvePathCollisions(std::vector<RemoteEntry> &remote,
                           const std::vector<ManifestEntry> &manifest,
                           ExistsOnCard existsOnCard) {
  const auto claims = [&remote](int id, const String &path) {
    for (const RemoteEntry &r : remote) {
      if (r.id == id && samePath(r.path, path)) {
        return true;
      }
    }
    return false;
  };

  // Decide every owner from the clean paths before rewriting any of them.
  std::vector<bool> keepsCleanName(remote.size(), false);
  for (size_t i = 0; i < remote.size(); ++i) {
    const String &path = remote[i].path;
    if (path.isEmpty()) {
      continue;
    }
    bool hasOwner = false;
    int owner = 0;
    bool held = false;
    for (const ManifestEntry &m : manifest) {
      if (!samePath(m.path, path)) {
        continue;
      }
      held = true;
      if (claims(m.id, path) && (!hasOwner || m.id < owner)) {
        hasOwner = true;
        owner = m.id;
      }
    }
    if (!held && !existsOnCard(path)) {
      for (const RemoteEntry &r : remote) {
        if (samePath(r.path, path) && (!hasOwner || r.id < owner)) {
          hasOwner = true;
          owner = r.id;
        }
      }
    }
    keepsCleanName[i] = hasOwner && owner == remote[i].id;
  }

  for (size_t i = 0; i < remote.size(); ++i) {
    if (!remote[i].path.isEmpty() && !keepsCleanName[i]) {
      remote[i].path = withIdSuffix(remote[i].path, remote[i].id);
    }
  }
}

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
//   * any of the above touching openPath                -> deferred
//   * manifest entry sharing its file with another      -> toDownload (the
//     bytes may be the other book's); never moved, and the shared file is only
//     removed once no remote book lives there
//
// Paths are compared case-insensitively (samePath), like FAT does. Callers
// that route should run resolvePathCollisions() on remote first.
//
// openPath is the book currently open in the reader (empty when none). Its
// .rsvp and index sidecars are held open, so deleting, moving or overwriting
// it mid-read is deferred to a later sync -- the same "in use" rule the
// companion API applies before it removes an open book.
//
// The path axis is only consulted when the caller populated RemoteEntry::path.
// An empty path means "caller does no folder routing", and the diff collapses
// to the original key-only behaviour.
//
// No I/O, no clock, no allocation beyond the output vectors.
inline SyncPlan computeSyncPlan(const std::vector<RemoteEntry> &remote,
                                const std::vector<ManifestEntry> &manifest,
                                DeletionPolicy policy,
                                const String &openPath = String()) {
  SyncPlan plan;
  const auto isOpen = [&openPath](const String &path) {
    return !openPath.isEmpty() && samePath(path, openPath);
  };

  // Pass 1: walk the remote view, deciding download vs move vs unchanged.
  for (const RemoteEntry &r : remote) {
    const ManifestEntry *existing = findManifestEntry(manifest, r.id);
    // A routed path that matches nothing on SD only matters while the bytes are
    // current; a changed key re-downloads to the new location anyway.
    const bool routed = !r.path.isEmpty();
    const bool relocated =
        routed && existing != nullptr && !samePath(existing->path, r.path);
    const bool shared = existing != nullptr && sharesFile(manifest, *existing);

    if (existing != nullptr && existing->key == r.key && !relocated && !shared) {
      plan.unchanged.push_back(r.id);
      continue;
    }
    // Every remaining case writes r.path and/or vacates existing->path.
    if (isOpen(r.path) || (existing != nullptr && isOpen(existing->path))) {
      plan.deferred.push_back(r.id);
      continue;
    }

    if (existing != nullptr && existing->key == r.key && !shared) {
      // Same bytes, new path: a retag, or a rename in Calibre.
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
    // would delete the file it just wrote -- and when no other book will live
    // there (a shared file stays with the book that keeps the name).
    if (relocated && !targetedByRemote(remote, existing->path)) {
      action.previousPath = existing->path;
    }
    plan.toDownload.push_back(action);
  }

  // Pass 2: reconcile deletions (only under Mirror).
  if (policy == DeletionPolicy::Mirror) {
    for (const ManifestEntry &m : manifest) {
      if (!remoteHasId(remote, m.id)) {
        if (isOpen(m.path)) {
          plan.deferred.push_back(m.id);
          continue;
        }
        DeleteAction action;
        action.id = m.id;
        action.path = m.path;
        action.keepFile = targetedByRemote(remote, m.path);
        plan.toDelete.push_back(action);
      }
    }
  }

  return plan;
}

}  // namespace calibresync
