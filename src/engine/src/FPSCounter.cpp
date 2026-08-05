#include "PCH.h"
#include "FPSCounter.h"

using hr_clock = std::chrono::high_resolution_clock;
using float_seconds = std::chrono::duration<float>;

AvgRateCounter::AvgRateCounter(const std::string& name, int sampleCount)
    : name(name)
    , sampleCount(sampleCount)
    , remaining(sampleCount)
    , firstCallTime{}
    , initialized(false)
{
}

void AvgRateCounter::start()
{
}

void AvgRateCounter::end()
{
    auto now = hr_clock::now();

    if (!initialized)
    {
        firstCallTime = hr_clock::now();;
        initialized = true;
        remaining = sampleCount;
        return;
    }

    remaining--;

    if (remaining <= 0)
    {
        float totalTime = float_seconds(now - firstCallTime).count();

        if (totalTime > 1e-6f)
        {
            float avgFPS = static_cast<float>(sampleCount) / totalTime;

            std::cout << name << ": "
                << std::fixed << std::setprecision(1) << avgFPS
                << " (over " << sampleCount << " intervals, "
                << std::setprecision(3) << totalTime / sampleCount << " s frame time)"
                << std::endl;
        }
        else
        {
            std::cout << name << ": too fast / zero time ("
                << sampleCount << " intervals)" << std::endl;
        }

        firstCallTime = now;
        remaining = sampleCount;
    }
}
