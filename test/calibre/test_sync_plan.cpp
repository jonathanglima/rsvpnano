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

#include <cstdio>
#include <string>
#include <vector>

#include "calibre/CalibreSyncPlan.h"

using calibresync::computeSyncPlan;
using calibresync::DeletionPolicy;
using calibresync::ManifestEntry;
using calibresync::RemoteEntry;
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

ManifestEntry manifest(int id, const char *key, const char *path = "/books/books/x.rsvp") {
  ManifestEntry m;
  m.id = id;
  m.key = key;
  m.path = path;
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

  std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
  if (g_failures == 0) {
    std::printf("ALL TESTS PASSED\n");
    return 0;
  }
  std::printf("TESTS FAILED\n");
  return 1;
}
