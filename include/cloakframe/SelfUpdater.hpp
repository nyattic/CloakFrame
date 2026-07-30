#pragma once

#include <QObject>
#include <QString>

namespace cloakframe
{
    class SelfUpdater : public QObject
    {
        Q_OBJECT

    public:
        enum class Mode
        {
            InApp,
            OwnUi
        };

        static void runStartupHooks();

        static SelfUpdater *create(QObject *parent = nullptr);

        virtual Mode mode() const = 0;

        virtual void checkForUpdates() = 0;

        virtual void downloadUpdate() = 0;

        virtual void restartToApply() = 0;

    signals:
        void updateAvailable(const QString &version, const QString &releaseNotes);

        void checkFailed(const QString &error);

        void downloadProgress(int percent);

        void downloadFinished();

        void downloadFailed(const QString &error);

        void readyToQuit();

    protected:
        using QObject::QObject;
    };
}
