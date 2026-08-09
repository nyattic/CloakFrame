#pragma once

#include <QString>

namespace cloakframe
{
    // Marker that opens a language section in a combined release-notes document. It is an HTML
    // comment so the same text renders as an ordinary release page on GitHub while the
    // updater can still pick one language out of it.
    inline constexpr auto kReleaseNotesLanguageMarker = "<!-- notes:";

    // Returns the section for `languageCode` (an ISO 639 code such as "ko"), falling back to
    // English and then to the whole document, so notes that carry no markers at all still
    // reach the user unchanged.
    [[nodiscard]] QString releaseNotesForLanguage(
        const QString &notes, const QString &languageCode);
}
