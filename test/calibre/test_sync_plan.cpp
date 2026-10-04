// Host unit test for the PURE reconcile core in src/calibre/CalibreSyncPlan.h
// (calibresync::computeSyncPlan). Mirrors test/calibre/test_calibre_parse.cpp
// in spirit and harness: built standalone by run_host_test.sh with plain
// g++ -std=c++17 against the test/support Arduino String shim. No networking,
// no SD, no clock -- just the diff.
//
// Coverage: new id, changed key, unchanged, deleted-with-Mirror (planned for
// delete), deleted-with-Keep (NOT deleted), empty remote, empty manifest, and
// the folder-routing axis: retag-only moves, retag+edit downloads that report
// the old path, and the unrouted caller that must keep the key-only behaviour.
// Also the open-book guard: any action that would touch the book open in the
// reader is deferred, never planned. And filename collisions: two books that
// route to the same file get distinct paths, and a file shared by an earlier
// collision is re-downloaded rather than trusted, moved or deleted.

#include <cstdio>
#include <string>
#include <vector>

#include "calibre/CalibreSyncPlan.h"

using calibresync::computeSyncPlan;
using calibresync::DeletionPolicy;
using calibresync::ManifestEntry;
using calibresync::RemoteEntry;
using calibresync::resolvePathCollisions;
using calibresync::SyncPlan;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                 \
  do {                                                              \
    ++g_checks;                                                     \
    if (!(cond)) {                                                  \
      ++g_failures;                                                 \
      std::printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                               \
  } while (0)

// Same shape as the macro in test_calibre_parse.cpp; the two host tests are
// standalone binaries with no shared header.
#define CHECK_STR_EQ(expected, actual)                                  \
  do {                                                                  \
    ++g_checks;                                                         \
    const std::string e = (expected);                                   \
    const std::string a = (actual);                                     \
    if (e != a) {                                                       \
      ++g_failures;                                                     \
      std::printf("  FAIL %s:%d: expected \"%s\" got \"%s\"\n", __FILE__, \
                  __LINE__, e.c_str(), a.c_str());                      \
    }                                                                   \
  } while (0)

namespace {

RemoteEntry remote(int id, const char *key, const char *title = "T",
                   const char *url = "/get/rsvp/x") {
  RemoteEntry r;
  r.id = id;
  r.key = key;
  r.title = title;
  r.url = url;
  return r;
}

// The default path is unique per id, like a real manifest: entries sharing a
// file are the collision residue that computeSyncPlan treats specially.
ManifestEntry manifest(int id, const char *key, const char *path = nullptr) {
  ManifestEntry m;
  m.id = id;
  m.key = key;
  m.path = path != nullptr ? String(path)
                           : String("/books/books/") + String(id) + ".rsvp";
  return m;
}

bool downloadHas(const SyncPlan &plan, int id) {
  for (const auto &a : plan.toDownload) {
    if (a.id == id) {
      return true;
    }
  }
  return false;
}

RemoteEntry routed(int id, const char *key, const char *path,
                   const char *title = "T") {
  RemoteEntry r = remote(id, key, title);
  r.path = path;
  return r;
}

const calibresync::MoveAction *moveFor(const SyncPlan &plan, int id) {
  for (const auto &m : plan.toMove) {
    if (m.id == id) {
      return &m;
    }
  }
  return nullptr;
}

const calibresync::DownloadAction *downloadFor(const SyncPlan &plan, int id) {
  for (const auto &a : plan.toDownload) {
    if (a.id == id) {
      return &a;
    }
  }
  return nullptr;
}

bool deleteHas(const SyncPlan &plan, int id) {
  for (const auto &a : plan.toDelete) {
    if (a.id == id) {
      return true;
    }
  }
  return false;
}

bool unchangedHas(const SyncPlan &plan, int id) {
  for (const int u : plan.unchanged) {
    if (u == id) {
      return true;
    }
  }
  return false;
}

// New id (in remote, not in manifest) -> download.
void test_new_id() {
  std::printf("test_new_id\n");
  std::vector<RemoteEntry> r = {remote(1, "10|m1")};
  std::vector<ManifestEntry> m;  // empty
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.toDownload.size() == 1);
  CHECK(downloadHas(plan, 1));
  CHECK(plan.toDelete.empty());
  CHECK(plan.unchanged.empty());
  // The download action carries url/title/key forward.
  if (!plan.toDownload.empty()) {
    CHECK(plan.toDownload[0].key == String("10|m1"));
  }
}

// Changed key (same id, different raw key) -> re-download, not unchanged.
void test_changed_key() {
  std::printf("test_changed_key\n");
  std::vector<RemoteEntry> r = {remote(42, "999|new-mtime")};
  std::vector<ManifestEntry> m = {manifest(42, "999|old-mtime")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(downloadHas(plan, 42));
  CHECK(!unchangedHas(plan, 42));
  CHECK(plan.toDelete.empty());
}

// Same id, identical raw key -> unchanged (no download, no delete).
void test_unchanged() {
  std::printf("test_unchanged\n");
  std::vector<RemoteEntry> r = {remote(7, "55|2024-01-01")};
  std::vector<ManifestEntry> m = {manifest(7, "55|2024-01-01")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.toDownload.empty());
  CHECK(plan.toDelete.empty());
  CHECK(unchangedHas(plan, 7));
}

// In manifest, absent from remote, policy == Mirror -> planned for delete.
void test_deleted_mirror() {
  std::printf("test_deleted_mirror\n");
  std::vector<RemoteEntry> r = {remote(1, "10|m1")};
  std::vector<ManifestEntry> m = {manifest(1, "10|m1"),
                                  manifest(99, "20|gone", "/books/books/gone.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(deleteHas(plan, 99));
  CHECK(unchangedHas(plan, 1));
  CHECK(plan.toDownload.empty());
  // The delete action carries the on-SD path so the caller can unlink it.
  if (!plan.toDelete.empty()) {
    CHECK(plan.toDelete[0].path == String("/books/books/gone.rsvp"));
  }
}

// In manifest, absent from remote, policy == Keep -> NOT deleted.
void test_deleted_keep() {
  std::printf("test_deleted_keep\n");
  std::vector<RemoteEntry> r = {remote(1, "10|m1")};
  std::vector<ManifestEntry> m = {manifest(1, "10|m1"), manifest(99, "20|gone")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Keep);
  CHECK(plan.toDelete.empty());
  CHECK(!deleteHas(plan, 99));
  CHECK(unchangedHas(plan, 1));
}

// Empty remote: under Mirror every manifest entry is deleted; under Keep none.
void test_empty_remote() {
  std::printf("test_empty_remote\n");
  std::vector<RemoteEntry> r;  // empty
  std::vector<ManifestEntry> m = {manifest(1, "a"), manifest(2, "b")};

  const SyncPlan mirror = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(mirror.toDownload.empty());
  CHECK(mirror.toDelete.size() == 2);
  CHECK(deleteHas(mirror, 1));
  CHECK(deleteHas(mirror, 2));

  const SyncPlan keep = computeSyncPlan(r, m, DeletionPolicy::Keep);
  CHECK(keep.toDownload.empty());
  CHECK(keep.toDelete.empty());
}

// Empty manifest: every remote entry is a fresh download; nothing to delete.
void test_empty_manifest() {
  std::printf("test_empty_manifest\n");
  std::vector<RemoteEntry> r = {remote(1, "a"), remote(2, "b"), remote(3, "c")};
  std::vector<ManifestEntry> m;  // empty
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.toDownload.size() == 3);
  CHECK(downloadHas(plan, 1));
  CHECK(downloadHas(plan, 2));
  CHECK(downloadHas(plan, 3));
  CHECK(plan.toDelete.empty());
  CHECK(plan.unchanged.empty());
}

// Both empty: a no-op plan.
void test_both_empty() {
  std::printf("test_both_empty\n");
  std::vector<RemoteEntry> r;
  std::vector<ManifestEntry> m;
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.toDownload.empty());
  CHECK(plan.toDelete.empty());
  CHECK(plan.unchanged.empty());
}

// Tagging a book "article" in Calibre does not touch the .rsvp, so the
// change-key is byte-identical. Only the destination folder moves -- and a
// download here would re-fetch bytes the device already has.
void test_retag_moves_without_download() {
  std::printf("test_retag_moves_without_download\n");
  std::vector<RemoteEntry> r{
      routed(1, "100|t", "/library/articles/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/A.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);

  CHECK(plan.toDownload.empty());
  CHECK(plan.unchanged.empty());
  CHECK(plan.toDelete.empty());
  CHECK(plan.toMove.size() == 1);
  const auto *move = moveFor(plan, 1);
  CHECK(move != nullptr);
  if (move != nullptr) {
    CHECK_STR_EQ("/library/books/A.rsvp", move->from.c_str());
    CHECK_STR_EQ("/library/articles/A.rsvp", move->to.c_str());
    CHECK_STR_EQ("100|t", move->key.c_str());
  }
}

void test_same_path_and_key_is_unchanged() {
  std::printf("test_same_path_and_key_is_unchanged\n");
  std::vector<RemoteEntry> r{routed(1, "100|t", "/library/books/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/A.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.unchanged.size() == 1);
  CHECK(plan.toMove.empty());
  CHECK(plan.toDownload.empty());
}

// Retagged AND edited: the bytes changed, so it must be a download -- but to
// the NEW folder, with the old copy reported for cleanup so the shelf does not
// end up showing the book twice.
void test_retag_with_changed_key_downloads_and_reports_old_path() {
  std::printf("test_retag_with_changed_key_downloads_and_reports_old_path\n");
  std::vector<RemoteEntry> r{
      routed(1, "200|t2", "/library/articles/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t1", "/library/books/A.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);

  CHECK(plan.toMove.empty());
  CHECK(plan.toDownload.size() == 1);
  const auto *action = downloadFor(plan, 1);
  CHECK(action != nullptr);
  if (action != nullptr) {
    CHECK_STR_EQ("/library/articles/A.rsvp", action->path.c_str());
    CHECK_STR_EQ("/library/books/A.rsvp", action->previousPath.c_str());
  }
}

// Same folder, changed bytes: previousPath must stay empty, or the caller would
// delete the file it just downloaded.
void test_changed_key_same_path_has_no_previous_path() {
  std::printf("test_changed_key_same_path_has_no_previous_path\n");
  std::vector<RemoteEntry> r{routed(1, "200|t2", "/library/books/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t1", "/library/books/A.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  const auto *action = downloadFor(plan, 1);
  CHECK(action != nullptr);
  if (action != nullptr) {
    CHECK(action->previousPath.isEmpty());
    CHECK_STR_EQ("/library/books/A.rsvp", action->path.c_str());
  }
}

void test_new_book_carries_path_and_no_previous() {
  std::printf("test_new_book_carries_path_and_no_previous\n");
  std::vector<RemoteEntry> r{
      routed(7, "100|t", "/library/articles/New.rsvp", "New")};
  std::vector<ManifestEntry> m;
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  const auto *action = downloadFor(plan, 7);
  CHECK(action != nullptr);
  if (action != nullptr) {
    CHECK_STR_EQ("/library/articles/New.rsvp", action->path.c_str());
    CHECK(action->previousPath.isEmpty());
  }
  CHECK(plan.toMove.empty());
}

// A caller that does not route (RemoteEntry::path left empty) must get exactly
// the pre-move behaviour, whatever the manifest says the path is.
void test_unrouted_caller_ignores_path_axis() {
  std::printf("test_unrouted_caller_ignores_path_axis\n");
  std::vector<RemoteEntry> r{remote(1, "100|t")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/Any.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.unchanged.size() == 1);
  CHECK(plan.toMove.empty());
  CHECK(plan.toDownload.empty());
}

// Untagging is the same operation in reverse -- articles must be able to go
// back to being books.
void test_move_back_from_articles_to_books() {
  std::printf("test_move_back_from_articles_to_books\n");
  std::vector<RemoteEntry> r{routed(1, "100|t", "/library/books/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/articles/A.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  const auto *move = moveFor(plan, 1);
  CHECK(move != nullptr);
  if (move != nullptr) {
    CHECK_STR_EQ("/library/articles/A.rsvp", move->from.c_str());
    CHECK_STR_EQ("/library/books/A.rsvp", move->to.c_str());
  }
}

// A move and a delete in the same run must not interfere: both ids are keyed
// separately and the mover must not resurrect the deleted one.
void test_move_and_delete_coexist() {
  std::printf("test_move_and_delete_coexist\n");
  std::vector<RemoteEntry> r{routed(1, "100|t", "/library/articles/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/A.rsvp"),
                               manifest(2, "200|t", "/library/books/B.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.toMove.size() == 1);
  CHECK(moveFor(plan, 1) != nullptr);
  CHECK(plan.toDelete.size() == 1);
  CHECK(deleteHas(plan, 2));
  CHECK(!deleteHas(plan, 1));
}

bool deferredHas(const SyncPlan &plan, int id) {
  for (const int d : plan.deferred) {
    if (d == id) {
      return true;
    }
  }
  return false;
}

void test_open_book_delete_is_deferred() {
  std::printf("test_open_book_delete_is_deferred\n");
  std::vector<RemoteEntry> r;
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/A.rsvp"),
                               manifest(2, "200|t", "/library/books/B.rsvp")};
  const SyncPlan plan =
      computeSyncPlan(r, m, DeletionPolicy::Mirror, "/library/books/A.rsvp");
  CHECK(!deleteHas(plan, 1));
  CHECK(deferredHas(plan, 1));
  CHECK(deleteHas(plan, 2));
  CHECK(plan.deferred.size() == 1);
}

void test_open_book_move_is_deferred() {
  std::printf("test_open_book_move_is_deferred\n");
  std::vector<RemoteEntry> r{routed(1, "100|t", "/library/articles/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/A.rsvp")};
  const SyncPlan plan =
      computeSyncPlan(r, m, DeletionPolicy::Mirror, "/library/books/A.rsvp");
  CHECK(plan.toMove.empty());
  CHECK(deferredHas(plan, 1));
  CHECK(plan.unchanged.empty());
}

void test_open_book_redownload_is_deferred() {
  std::printf("test_open_book_redownload_is_deferred\n");
  std::vector<RemoteEntry> r{routed(1, "999|t", "/library/books/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/A.rsvp")};
  const SyncPlan plan =
      computeSyncPlan(r, m, DeletionPolicy::Mirror, "/library/books/A.rsvp");
  CHECK(!downloadHas(plan, 1));
  CHECK(deferredHas(plan, 1));
}

void test_download_onto_open_path_is_deferred() {
  std::printf("test_download_onto_open_path_is_deferred\n");
  // A new book whose routed filename collides with the open one must not
  // overwrite it while it is being read.
  std::vector<RemoteEntry> r{routed(7, "100|t", "/library/books/A.rsvp", "A")};
  std::vector<ManifestEntry> m;
  const SyncPlan plan =
      computeSyncPlan(r, m, DeletionPolicy::Mirror, "/library/books/A.rsvp");
  CHECK(plan.toDownload.empty());
  CHECK(deferredHas(plan, 7));
}

void test_move_onto_open_path_is_deferred() {
  std::printf("test_move_onto_open_path_is_deferred\n");
  // Untagging an article routes it back to books/, where a book of the same
  // name is open: renaming over it would replace the file being read.
  std::vector<RemoteEntry> r{routed(1, "100|t", "/library/books/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/articles/A.rsvp")};
  const SyncPlan plan =
      computeSyncPlan(r, m, DeletionPolicy::Mirror, "/library/books/A.rsvp");
  CHECK(plan.toMove.empty());
  CHECK(deferredHas(plan, 1));
}

void test_open_book_retag_and_edit_is_deferred() {
  std::printf("test_open_book_retag_and_edit_is_deferred\n");
  // Download lands elsewhere, but its previousPath cleanup would remove the
  // open file.
  std::vector<RemoteEntry> r{routed(1, "999|t", "/library/articles/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/A.rsvp")};
  const SyncPlan plan =
      computeSyncPlan(r, m, DeletionPolicy::Mirror, "/library/books/A.rsvp");
  CHECK(!downloadHas(plan, 1));
  CHECK(deferredHas(plan, 1));
}

void test_unchanged_open_book_is_not_deferred() {
  std::printf("test_unchanged_open_book_is_not_deferred\n");
  std::vector<RemoteEntry> r{routed(1, "100|t", "/library/books/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/A.rsvp")};
  const SyncPlan plan =
      computeSyncPlan(r, m, DeletionPolicy::Mirror, "/library/books/A.rsvp");
  CHECK(plan.deferred.empty());
  CHECK(plan.unchanged.size() == 1);
}

void test_no_open_book_defers_nothing() {
  std::printf("test_no_open_book_defers_nothing\n");
  std::vector<RemoteEntry> r{routed(1, "100|t", "/library/articles/A.rsvp", "A")};
  std::vector<ManifestEntry> m{manifest(1, "100|t", "/library/books/A.rsvp"),
                               manifest(2, "200|t", "/library/books/B.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.deferred.empty());
  CHECK(plan.toMove.size() == 1);
  CHECK(plan.toDelete.size() == 1);
}

const RemoteEntry *remoteFor(const std::vector<RemoteEntry> &r, int id) {
  for (const auto &e : r) {
    if (e.id == id) {
      return &e;
    }
  }
  return nullptr;
}

const auto kNothingOnCard = [](const String &) { return false; };

void test_with_id_suffix() {
  std::printf("test_with_id_suffix\n");
  CHECK_STR_EQ("/library/books/Poems (12).rsvp",
               calibresync::withIdSuffix("/library/books/Poems.rsvp", 12).c_str());
  CHECK_STR_EQ("/library/books/Poems (12)",
               calibresync::withIdSuffix("/library/books/Poems", 12).c_str());
}

void test_new_books_with_same_title_get_distinct_paths() {
  std::printf("test_new_books_with_same_title_get_distinct_paths\n");
  std::vector<RemoteEntry> r{routed(9, "1|t", "/library/books/Poems.rsvp"),
                             routed(4, "2|t", "/library/books/Poems.rsvp")};
  std::vector<ManifestEntry> m;
  resolvePathCollisions(r, m, kNothingOnCard);
  // Lowest id keeps the clean name, independent of search order.
  CHECK_STR_EQ("/library/books/Poems.rsvp", remoteFor(r, 4)->path.c_str());
  CHECK_STR_EQ("/library/books/Poems (9).rsvp", remoteFor(r, 9)->path.c_str());
}

void test_collision_ignores_case() {
  std::printf("test_collision_ignores_case\n");
  // FAT is case-insensitive: these are the same file on the card.
  std::vector<RemoteEntry> r{routed(1, "1|t", "/library/books/Poems.rsvp"),
                             routed(2, "2|t", "/library/books/POEMS.rsvp")};
  std::vector<ManifestEntry> m;
  resolvePathCollisions(r, m, kNothingOnCard);
  CHECK_STR_EQ("/library/books/Poems.rsvp", remoteFor(r, 1)->path.c_str());
  CHECK_STR_EQ("/library/books/POEMS (2).rsvp", remoteFor(r, 2)->path.c_str());
}

void test_book_already_on_card_keeps_clean_name() {
  std::printf("test_book_already_on_card_keeps_clean_name\n");
  // id 7 has the file; a newer, lower-id book with the same title must not
  // take the name and force a rename of the book already there.
  std::vector<RemoteEntry> r{routed(7, "1|t", "/library/books/Poems.rsvp"),
                             routed(3, "2|t", "/library/books/Poems.rsvp")};
  std::vector<ManifestEntry> m{manifest(7, "1|t", "/library/books/Poems.rsvp")};
  resolvePathCollisions(r, m, kNothingOnCard);
  CHECK_STR_EQ("/library/books/Poems.rsvp", remoteFor(r, 7)->path.c_str());
  CHECK_STR_EQ("/library/books/Poems (3).rsvp", remoteFor(r, 3)->path.c_str());
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.unchanged.size() == 1);
  CHECK(downloadFor(plan, 3) != nullptr);
}

void test_path_held_by_departing_book_is_not_reused() {
  std::printf("test_path_held_by_departing_book_is_not_reused\n");
  // id 1 left Calibre (Mirror deletes it after downloads run); a new book with
  // the same title must not land on the file that is about to be deleted.
  std::vector<RemoteEntry> r{routed(2, "2|t", "/library/books/Poems.rsvp")};
  std::vector<ManifestEntry> m{manifest(1, "1|t", "/library/books/Poems.rsvp")};
  resolvePathCollisions(r, m, kNothingOnCard);
  CHECK_STR_EQ("/library/books/Poems (2).rsvp", remoteFor(r, 2)->path.c_str());
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(deleteHas(plan, 1));
  CHECK_STR_EQ("/library/books/Poems (2).rsvp", downloadFor(plan, 2)->path.c_str());
}

void test_foreign_file_is_not_overwritten() {
  std::printf("test_foreign_file_is_not_overwritten\n");
  // A companion upload or RSS article already sits at the clean path.
  std::vector<RemoteEntry> r{routed(5, "1|t", "/library/books/Poems.rsvp")};
  std::vector<ManifestEntry> m;
  resolvePathCollisions(r, m, [](const String &path) {
    return path == "/library/books/Poems.rsvp";
  });
  CHECK_STR_EQ("/library/books/Poems (5).rsvp", remoteFor(r, 5)->path.c_str());
}

void test_own_file_is_not_foreign() {
  std::printf("test_own_file_is_not_foreign\n");
  std::vector<RemoteEntry> r{routed(5, "1|t", "/library/books/Poems.rsvp")};
  std::vector<ManifestEntry> m{manifest(5, "1|t", "/library/books/Poems.rsvp")};
  resolvePathCollisions(r, m, [](const String &) { return true; });
  CHECK_STR_EQ("/library/books/Poems.rsvp", remoteFor(r, 5)->path.c_str());
}

void test_unrouted_entries_are_left_alone() {
  std::printf("test_unrouted_entries_are_left_alone\n");
  std::vector<RemoteEntry> r{remote(1, "1|t"), remote(2, "2|t")};
  std::vector<ManifestEntry> m;
  resolvePathCollisions(r, m, kNothingOnCard);
  CHECK(r[0].path.isEmpty());
  CHECK(r[1].path.isEmpty());
}

void test_retag_away_does_not_hand_old_path_to_new_book() {
  std::printf("test_retag_away_does_not_hand_old_path_to_new_book\n");
  // Downloads run before moves: if the new book took /books/A.rsvp it would
  // overwrite id 1's file before id 1 is moved out of it.
  std::vector<RemoteEntry> r{routed(1, "1|t", "/library/articles/A.rsvp"),
                             routed(2, "2|t", "/library/books/A.rsvp")};
  std::vector<ManifestEntry> m{manifest(1, "1|t", "/library/books/A.rsvp")};
  resolvePathCollisions(r, m, kNothingOnCard);
  CHECK_STR_EQ("/library/books/A (2).rsvp", remoteFor(r, 2)->path.c_str());
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(moveFor(plan, 1) != nullptr);
  CHECK_STR_EQ("/library/books/A (2).rsvp", downloadFor(plan, 2)->path.c_str());
}

void test_title_swap_converges_without_overwriting() {
  std::printf("test_title_swap_converges_without_overwriting\n");
  // Two books swap titles in Calibre. Each clean name is still held by the
  // other book, so neither move may target it; both take a suffixed name.
  std::vector<RemoteEntry> r{routed(1, "1|t", "/library/books/B.rsvp"),
                             routed(2, "2|t", "/library/books/A.rsvp")};
  std::vector<ManifestEntry> m{manifest(1, "1|t", "/library/books/A.rsvp"),
                               manifest(2, "2|t", "/library/books/B.rsvp")};
  resolvePathCollisions(r, m, kNothingOnCard);
  const SyncPlan first = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(first.toMove.size() == 2);
  CHECK_STR_EQ("/library/books/B (1).rsvp", moveFor(first, 1)->to.c_str());
  CHECK_STR_EQ("/library/books/A (2).rsvp", moveFor(first, 2)->to.c_str());

  // Next sync: the clean names are free, so each book moves onto its own.
  std::vector<RemoteEntry> r2{routed(1, "1|t", "/library/books/B.rsvp"),
                              routed(2, "2|t", "/library/books/A.rsvp")};
  std::vector<ManifestEntry> m2{manifest(1, "1|t", "/library/books/B (1).rsvp"),
                                manifest(2, "2|t", "/library/books/A (2).rsvp")};
  resolvePathCollisions(r2, m2, kNothingOnCard);
  const SyncPlan second = computeSyncPlan(r2, m2, DeletionPolicy::Mirror);
  CHECK_STR_EQ("/library/books/B.rsvp", moveFor(second, 1)->to.c_str());
  CHECK_STR_EQ("/library/books/A.rsvp", moveFor(second, 2)->to.c_str());

  // And then it is stable.
  std::vector<RemoteEntry> r3{routed(1, "1|t", "/library/books/B.rsvp"),
                              routed(2, "2|t", "/library/books/A.rsvp")};
  std::vector<ManifestEntry> m3{manifest(1, "1|t", "/library/books/B.rsvp"),
                                manifest(2, "2|t", "/library/books/A.rsvp")};
  resolvePathCollisions(r3, m3, kNothingOnCard);
  const SyncPlan third = computeSyncPlan(r3, m3, DeletionPolicy::Mirror);
  CHECK(third.toMove.empty());
  CHECK(third.toDownload.empty());
  CHECK(third.unchanged.size() == 2);
}

void test_shared_file_from_earlier_collision_is_redownloaded() {
  std::printf("test_shared_file_from_earlier_collision_is_redownloaded\n");
  // Before collision handling both books were written to the same file, so it
  // holds whichever downloaded last. Neither copy can be trusted: the owner
  // re-downloads in place, the other re-downloads to its suffixed path, and
  // the shared file is not removed as anyone's previousPath.
  std::vector<RemoteEntry> r{routed(1, "1|t", "/library/books/Poems.rsvp"),
                             routed(2, "2|t", "/library/books/Poems.rsvp")};
  std::vector<ManifestEntry> m{manifest(1, "1|t", "/library/books/Poems.rsvp"),
                               manifest(2, "2|t", "/library/books/Poems.rsvp")};
  resolvePathCollisions(r, m, kNothingOnCard);
  CHECK_STR_EQ("/library/books/Poems.rsvp", remoteFor(r, 1)->path.c_str());
  CHECK_STR_EQ("/library/books/Poems (2).rsvp", remoteFor(r, 2)->path.c_str());
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.toMove.empty());
  CHECK(plan.unchanged.empty());
  CHECK(downloadFor(plan, 1) != nullptr);
  CHECK(downloadFor(plan, 2) != nullptr);
  CHECK(downloadFor(plan, 1)->previousPath.isEmpty());
  CHECK(downloadFor(plan, 2)->previousPath.isEmpty());
}

void test_shared_file_survives_delete_of_one_holder() {
  std::printf("test_shared_file_survives_delete_of_one_holder\n");
  std::vector<RemoteEntry> r{routed(2, "2|t", "/library/books/Poems.rsvp")};
  std::vector<ManifestEntry> m{manifest(1, "1|t", "/library/books/Poems.rsvp"),
                               manifest(2, "2|t", "/library/books/Poems.rsvp")};
  resolvePathCollisions(r, m, kNothingOnCard);
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(deleteHas(plan, 1));
  for (const auto &d : plan.toDelete) {
    if (d.id == 1) {
      CHECK(d.keepFile);
    }
  }
  CHECK(downloadFor(plan, 2) != nullptr);
}

void test_shared_file_removed_when_all_holders_leave() {
  std::printf("test_shared_file_removed_when_all_holders_leave\n");
  std::vector<RemoteEntry> r;
  std::vector<ManifestEntry> m{manifest(1, "1|t", "/library/books/Poems.rsvp"),
                               manifest(2, "2|t", "/library/books/Poems.rsvp")};
  const SyncPlan plan = computeSyncPlan(r, m, DeletionPolicy::Mirror);
  CHECK(plan.toDelete.size() == 2);
  for (const auto &d : plan.toDelete) {
    CHECK(!d.keepFile);
  }
}

}  // namespace

int main() {
  test_new_id();
  test_changed_key();
  test_unchanged();
  test_deleted_mirror();
  test_deleted_keep();
  test_empty_remote();
  test_empty_manifest();
  test_both_empty();
  test_retag_moves_without_download();
  test_same_path_and_key_is_unchanged();
  test_retag_with_changed_key_downloads_and_reports_old_path();
  test_changed_key_same_path_has_no_previous_path();
  test_new_book_carries_path_and_no_previous();
  test_unrouted_caller_ignores_path_axis();
  test_move_back_from_articles_to_books();
  test_move_and_delete_coexist();
  test_open_book_delete_is_deferred();
  test_open_book_move_is_deferred();
  test_open_book_redownload_is_deferred();
  test_download_onto_open_path_is_deferred();
  test_move_onto_open_path_is_deferred();
  test_open_book_retag_and_edit_is_deferred();
  test_unchanged_open_book_is_not_deferred();
  test_no_open_book_defers_nothing();
  test_with_id_suffix();
  test_new_books_with_same_title_get_distinct_paths();
  test_collision_ignores_case();
  test_book_already_on_card_keeps_clean_name();
  test_path_held_by_departing_book_is_not_reused();
  test_foreign_file_is_not_overwritten();
  test_own_file_is_not_foreign();
  test_unrouted_entries_are_left_alone();
  test_retag_away_does_not_hand_old_path_to_new_book();
  test_title_swap_converges_without_overwriting();
  test_shared_file_from_earlier_collision_is_redownloaded();
  test_shared_file_survives_delete_of_one_holder();
  test_shared_file_removed_when_all_holders_leave();

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures == 0) {
    std::printf("ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("TESTS FAILED\n");
  return 1;
}
