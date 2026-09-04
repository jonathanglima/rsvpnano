#pragma once

// Sync configuration as the Calibre engine consumes it.
//
// This is deliberately a plain Arduino-String struct with no dependency on
// settings/SettingsModel.h: the host tests in test/calibre/ compile it against
// the test/support/Arduino.h shim, and pulling in the firmware settings stack
// (glaze, std::expected, ESP headers) would make that impossible.
//
// Persistence lives in the normal firmware settings store as
// settings::CalibreSettings (plus DeviceSecrets::calibrePassword). App.cpp
// converts from that representation into this one before each sync; the
// companion HTTP API reads and writes the stored form directly. There is no
// separate NVS namespace for Calibre any more.

#include <Arduino.h>

struct CalibreSettings {
  bool enabled = false;
  String baseUrl;     // e.g. http://192.168.0.120:8080  (no trailing slash)
  String libraryId;   // empty => use server default_library
  String searchQuery; // saved search / tag, e.g. "tag:rsvp"
  String username;    // optional HTTP Basic
  String password;    // optional HTTP Basic
  // NOTE: the password is held in the encrypted-secrets half of the settings
  // store and is never written to SD. HTTP Basic over plain HTTP is cleartext
  // on the LAN -- acceptable per design because calibre-server is typically a
  // local, trusted network service.
  enum DeletionPolicy { Mirror, Keep };
  DeletionPolicy deletionPolicy = Mirror;
};

// Strip any trailing slashes from a Calibre base URL so stored values and
// constructed URLs are consistent regardless of what the user typed.
// Returns the normalised URL (may be the same string if already clean).
inline String normalizeCalibreBaseUrl(const String &url) {
  String result = url;
  while (result.length() > 0 && result[result.length() - 1] == '/') {
    result.remove(result.length() - 1);
  }
  return result;
}
