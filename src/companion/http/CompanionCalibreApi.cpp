#include "companion/http/CompanionApi.h"

#include <utility>

#include "settings/SettingsRules.h"
#include "text/AsciiText.h"

namespace {

    namespace api = companion::api;

    // Stored base URLs never keep a trailing slash, so the URLs the sync engine
    // builds are consistent no matter what the user typed.
    [[nodiscard]] std::string stripTrailingSlashes(std::string url) {
        while (!url.empty() && url.back() == '/')
            url.pop_back();
        return url;
    }

    [[nodiscard]] api::Result<> checkLength(const std::optional<std::string>& value, size_t maximum,
                                            std::string message, std::string field) {
        if (value && value->size() > maximum) {
            return std::unexpected(api::httpError(HTTP_CODE_UNPROCESSABLE_ENTITY, "invalid_calibre",
                                                  std::move(message), std::move(field)));
        }
        return {};
    }

    [[nodiscard]] api::Result<api::CalibreUpdate> validateCalibreUpdate(api::CalibreUpdate update) {
        if (update.baseUrl) {
            update.baseUrl = stripTrailingSlashes(std::string{AsciiText::trim(*update.baseUrl)});
            // An empty URL is allowed: it is how the user clears the configuration.
            if (!update.baseUrl->empty() && !update.baseUrl->starts_with("http://") &&
                !update.baseUrl->starts_with("https://")) {
                return std::unexpected(api::httpError(HTTP_CODE_UNPROCESSABLE_ENTITY, "invalid_calibre",
                                                      "Server URL must start with http:// or https://",
                                                      "baseUrl"));
            }
        }
        if (update.searchQuery)
            update.searchQuery = std::string{AsciiText::trim(*update.searchQuery)};
        if (update.username)
            update.username = std::string{AsciiText::trim(*update.username)};
        if (update.libraryId)
            update.libraryId = std::string{AsciiText::trim(*update.libraryId)};

        namespace rules = settings::rules;
        for (auto&& checked: {
                 checkLength(update.baseUrl, rules::kCalibreBaseUrlMaxLength, "Server URL is too long",
                             "baseUrl"),
                 checkLength(update.libraryId, rules::kCalibreLibraryIdMaxLength, "Library ID is too long",
                             "libraryId"),
                 checkLength(update.searchQuery, rules::kCalibreSearchQueryMaxLength,
                             "Search query is too long", "searchQuery"),
                 checkLength(update.username, rules::kCalibreUsernameMaxLength, "Username is too long",
                             "username"),
                 checkLength(update.password, rules::kCalibrePasswordMaxLength, "Password is too long",
                             "password"),
             }) {
            if (!checked)
                return std::unexpected(checked.error());
        }

        return update;
    }

} // namespace

companion::api::Result<companion::api::CalibreView> CompanionApi::getCalibre(httpd_req_t& request) {
    (void) request;
    const settings::CalibreSettings& stored = settingsStore_.settings().calibre;
    // password stays empty on purpose: the stored credential is never echoed back.
    return companion::api::CalibreView{
        .enabled = stored.enabled,
        .baseUrl = stored.baseUrl,
        .libraryId = stored.libraryId,
        .searchQuery = stored.searchQuery,
        .username = stored.username,
        .password = {},
        .deletionPolicy = stored.deletionPolicy,
    };
}

companion::api::Result<> CompanionApi::putCalibre(httpd_req_t& request) {
    return readJson<companion::api::CalibreUpdate>(request, 1024, "Calibre payload exceeds 1 KB")
        .and_then(validateCalibreUpdate)
        .transform([this](companion::api::CalibreUpdate update) {
            settings::CalibreSettings& target = settingsStore_.settings().calibre;
            if (update.enabled)
                target.enabled = *update.enabled;
            if (update.baseUrl)
                target.baseUrl = std::move(*update.baseUrl);
            if (update.libraryId)
                target.libraryId = std::move(*update.libraryId);
            if (update.searchQuery)
                target.searchQuery = std::move(*update.searchQuery);
            if (update.username)
                target.username = std::move(*update.username);
            if (update.deletionPolicy)
                target.deletionPolicy = *update.deletionPolicy;
            settingsStore_.acceptChanges();

            // Empty means "keep what is stored" — see CalibreUpdate's note.
            if (update.password && !update.password->empty()) {
                settingsStore_.secrets().calibrePassword = std::move(*update.password);
                settingsStore_.acceptSecretChanges();
            }
        });
}
