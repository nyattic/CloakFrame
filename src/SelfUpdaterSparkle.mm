#include "cloakframe/SelfUpdater.hpp"

#import <Sparkle/Sparkle.h>

namespace cloakframe
{
    namespace
    {
        class SparkleSelfUpdater final : public SelfUpdater
        {
        public:
            explicit SparkleSelfUpdater(QObject *parent)
                : SelfUpdater(parent)
                , controller_([[SPUStandardUpdaterController alloc] initWithStartingUpdater:YES
                                                                            updaterDelegate:nil
                                                                         userDriverDelegate:nil])
            {
                controller_.updater.automaticallyChecksForUpdates = YES;
            }

            Mode mode() const override
            {
                return Mode::OwnUi;
            }

            void checkForUpdates() override
            {
                [controller_.updater checkForUpdatesInBackground];
            }

            void downloadUpdate() override
            {
            }

            void restartToApply() override
            {
            }

        private:
            SPUStandardUpdaterController *controller_;
        };
    }

    void SelfUpdater::runStartupHooks()
    {
    }

    SelfUpdater *SelfUpdater::create(QObject *parent)
    {
        return new SparkleSelfUpdater(parent);
    }
}
