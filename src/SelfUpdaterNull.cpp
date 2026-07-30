#include "cloakframe/SelfUpdater.hpp"

namespace cloakframe
{
    void SelfUpdater::runStartupHooks()
    {
    }

    SelfUpdater *SelfUpdater::create(QObject *)
    {
        return nullptr;
    }
}
