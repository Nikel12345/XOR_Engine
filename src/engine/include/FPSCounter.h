


#include <string>
#include <chrono>

class AvgRateCounter
{
public:
    using hr_clock = std::chrono::high_resolution_clock;
    using time_point = hr_clock::time_point;
    using float_seconds = std::chrono::duration<float>;

    AvgRateCounter(const std::string& name, int sampleCount);

    void start();
    void end();

    float getLastExecTime() const;

private:
    std::string     name;
    int             sampleCount;
    int             remaining;

    time_point      startTime; 
    time_point      firstCallTime;

    bool            initialized;

    float           lastExecTime;
};

