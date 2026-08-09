#include "cloakframe/ReleaseNotes.hpp"

#include <QStringList>

namespace cloakframe
{
    namespace
    {
        // The document folds the non-default languages into <details> blocks so the release
        // page stays readable. Those wrappers are markup, not content, and the update dialog
        // renders plain text.
        bool isMarkupOnlyLine(const QString &line)
        {
            const QString trimmed = line.trimmed();
            return trimmed.startsWith(QLatin1String("<details"))
                   || trimmed == QLatin1String("</details>")
                   || (trimmed.startsWith(QLatin1String("<summary"))
                       && trimmed.endsWith(QLatin1String("</summary>")));
        }

        QString sectionFor(const QString &notes, const QString &languageCode)
        {
            const QString marker = QString::fromLatin1(kReleaseNotesLanguageMarker) + languageCode;
            const qsizetype markerStart = notes.indexOf(marker);
            if (markerStart < 0)
            {
                return {};
            }
            const qsizetype lineEnd = notes.indexOf(QLatin1Char('\n'), markerStart);
            if (lineEnd < 0)
            {
                return {};
            }
            const qsizetype nextMarker =
                notes.indexOf(QString::fromLatin1(kReleaseNotesLanguageMarker), lineEnd);
            const QString body = nextMarker < 0 ? notes.mid(lineEnd + 1)
                                                : notes.mid(lineEnd + 1, nextMarker - lineEnd - 1);

            QStringList kept;
            for (const auto &line : body.split(QLatin1Char('\n')))
            {
                if (!isMarkupOnlyLine(line))
                {
                    kept.push_back(line);
                }
            }
            return kept.join(QLatin1Char('\n')).trimmed();
        }
    }

    QString releaseNotesForLanguage(const QString &notes, const QString &languageCode)
    {
        QString requested = sectionFor(notes, languageCode);
        if (!requested.isEmpty())
        {
            return requested;
        }
        QString english = sectionFor(notes, QStringLiteral("en"));
        if (!english.isEmpty())
        {
            return english;
        }
        return notes.trimmed();
    }
}
