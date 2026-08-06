#include "cloakframe/SelfUpdater.hpp"

#include <QMetaObject>
#include <QThread>

#include <Velopack.hpp>
#include <exception>
#include <memory>
#include <optional>

namespace cloakframe
{
    namespace
    {
        constexpr auto kRepoUrl = "https://github.com/nyattic/CloakFrame";

        class VelopackWorker final : public QObject
        {
            Q_OBJECT

        public slots:
            void check()
            {
                try
                {
                    if (!manager_)
                    {
                        manager_ = std::make_unique<Velopack::UpdateManager>(
                            std::make_unique<Velopack::GithubSource>(kRepoUrl));
                    }
                    update_ = manager_->CheckForUpdates();
                }
                catch (const std::exception &error)
                {
                    emit checkFailed(QString::fromUtf8(error.what()));
                    return;
                }
                if (!update_)
                {
                    return;
                }
                emit updateAvailable(QString::fromStdString(update_->TargetFullRelease.Version),
                    QString::fromStdString(update_->TargetFullRelease.NotesMarkdown));
            }

            void download()
            {
                if (!manager_ || !update_)
                {
                    emit downloadFailed(QStringLiteral("no update is pending"));
                    return;
                }
                try
                {
                    manager_->DownloadUpdates(*update_, &VelopackWorker::forwardProgress, this);
                }
                catch (const std::exception &error)
                {
                    emit downloadFailed(QString::fromUtf8(error.what()));
                    return;
                }
                emit downloadFinished();
            }

            void apply()
            {
                if (!manager_ || !update_)
                {
                    return;
                }
                try
                {
                    manager_->WaitExitThenApplyUpdates(*update_);
                }
                catch (const std::exception &error)
                {
                    emit downloadFailed(QString::fromUtf8(error.what()));
                    return;
                }
                emit applied();
            }

        signals:
            void updateAvailable(const QString &version, const QString &releaseNotes);

            void checkFailed(const QString &error);

            void progress(int percent);

            void downloadFinished();

            void downloadFailed(const QString &error);

            void applied();

        private:
            static void forwardProgress(void *userData, std::size_t value)
            {
                auto *worker = static_cast<VelopackWorker *>(userData);
                emit worker->progress(static_cast<int>(value));
            }

            std::unique_ptr<Velopack::UpdateManager> manager_;
            std::optional<Velopack::UpdateInfo> update_;
        };

        class VelopackSelfUpdater final : public SelfUpdater
        {
            Q_OBJECT

        public:
            explicit VelopackSelfUpdater(QObject *parent)
                : SelfUpdater(parent)
                , thread_(new QThread)
                , worker_(new VelopackWorker)
            {
                worker_->moveToThread(thread_);
                connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
                connect(thread_, &QThread::finished, thread_, &QObject::deleteLater);
                connect(
                    worker_, &VelopackWorker::updateAvailable, this, &SelfUpdater::updateAvailable);
                connect(worker_, &VelopackWorker::checkFailed, this, &SelfUpdater::checkFailed);
                connect(worker_, &VelopackWorker::progress, this, &SelfUpdater::downloadProgress);
                connect(worker_,
                    &VelopackWorker::downloadFinished,
                    this,
                    &SelfUpdater::downloadFinished);
                connect(
                    worker_, &VelopackWorker::downloadFailed, this, &SelfUpdater::downloadFailed);
                connect(worker_, &VelopackWorker::applied, this, &SelfUpdater::readyToQuit);
                thread_->start();
            }

            ~VelopackSelfUpdater() override
            {
                thread_->quit();
            }

            Mode mode() const override
            {
                return Mode::InApp;
            }

            void checkForUpdates() override
            {
                QMetaObject::invokeMethod(worker_, &VelopackWorker::check, Qt::QueuedConnection);
            }

            void downloadUpdate() override
            {
                QMetaObject::invokeMethod(worker_, &VelopackWorker::download, Qt::QueuedConnection);
            }

            void restartToApply() override
            {
                QMetaObject::invokeMethod(worker_, &VelopackWorker::apply, Qt::QueuedConnection);
            }

        private:
            QThread *thread_;
            VelopackWorker *worker_;
        };
    }

    void SelfUpdater::runStartupHooks()
    {
        Velopack::VelopackApp::Build().Run();
    }

    SelfUpdater *SelfUpdater::create(QObject *parent)
    {
        return new VelopackSelfUpdater(parent);
    }
}

#include "SelfUpdaterVelopack.moc"
