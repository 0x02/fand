#include <iostream>
#include <string>
#include <array>
#include <algorithm>
#include <ctime>
#include <locale>
using namespace std;

#include <unistd.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/resource.h>

const char* const ctl_thermal = "dev.acpi_ibm.0.thermal";
const char* const ctl_fan = "dev.acpi_ibm.0.fan";
const char* const ctl_fanlevel = "dev.acpi_ibm.0.fan_level";

/*
 * 1. CPU
 * 2. Mini PCI Module
 * 3. HDD
 * 4. GPU
 * 5. Built-in battery
 * 6. UltraBay battery
 * 7. Built-in battery
 * 8. UltraBay battery
 */

static string DateTime(const char* fmt = "%Y.%m.%d %T")
{
    // buffer size is limited to 100
    char buf[100];
    std::time_t t = std::time(NULL);
    size_t len = std::strftime(buf, sizeof(buf), fmt, std::localtime(&t));
    return string(buf, len);
}

static bool FetchThermal(array<int, 8>& thermal)
{
    // verify, fetch, normalize
    size_t thermalLen = 0;
    if (sysctlbyname(ctl_thermal, nullptr, &thermalLen, nullptr, 0) != 0)
        return false;
    if (thermalLen != sizeof(thermal))
        return false;
    if (sysctlbyname(ctl_thermal, thermal.data(), &thermalLen, nullptr, 0) != 0)
        return false;
    replace_if(thermal.begin(), thermal.end(),
               [](int t)->bool { return t >= 255; },
               -1);
    return true;
}

enum class EmHandleRet
{
    Ok,
    Err,
    Out
};

struct IHandler
{
    virtual ~IHandler() { }

    virtual EmHandleRet Handle(int t) = 0;
    virtual bool InRange(int t) const = 0;

    static bool TrySwitch(int level)
    {
        // fetch
        int prevLevel = 0;
        size_t levelLen = sizeof(prevLevel);
        if (sysctlbyname(ctl_fanlevel, &prevLevel, &levelLen, nullptr, 0) != 0)
            return false;
        if (prevLevel == level)
            return true;

        // update
        cout << DateTime() << " " << prevLevel << " => " << level;
        if (sysctlbyname(ctl_fanlevel, nullptr, 0, &level, sizeof(level)) == 0) {
            cout << endl;
            return true;
        } else {
            cout <<  " Failed!" << endl;
            return true;
        }
    }

    static int HOLD_TIMES;
    static int MORE;
};
int IHandler::HOLD_TIMES = 0;
int IHandler::MORE = 0;

// (min, max]

struct LV1Handler: public IHandler
{
    virtual EmHandleRet Handle(int t)
    {
        if (t > max+MORE)
            return EmHandleRet::Out;

        return TrySwitch(1) ? EmHandleRet::Ok : EmHandleRet::Err;
    }

    virtual bool InRange(int t) const
    {
        return (t <= max);
    }

    int max;
};

struct LV2Handler: public IHandler
{
    virtual EmHandleRet Handle(int t)
    {
        if (t > max+MORE) {
            escapeTimes = 0;
            return EmHandleRet::Out;
        }

        if (t <= min) {
            if (++escapeTimes >= HOLD_TIMES) {
                escapeTimes = 0;
                return EmHandleRet::Out;
            }
        }

        return TrySwitch(2) ? EmHandleRet::Ok : EmHandleRet::Err;
    }

    virtual bool InRange(int t) const
    {
        return (t > min && t <= max);
    }

    int max;
    int min;

private:
    int escapeTimes = 0;
};

struct LV3Handler: public IHandler
{
    virtual EmHandleRet Handle(int t)
    {
        if (t <= min) {
            if (++escapeTimes >= HOLD_TIMES) {
                escapeTimes = 0;
                return EmHandleRet::Out;
            }
        }

        return TrySwitch(3) ? EmHandleRet::Ok : EmHandleRet::Err;
    }

    virtual bool InRange(int t) const
    {
        return (t > min);
    }

    int min;

private:
    int escapeTimes = 0;
};

int main()
{
    array<int, 8> thermal;
    const int hold = 30*1000;   // ms
    const int tick = 500;       // ms
    const int nice = -10;
    IHandler::MORE = 5;
    IHandler::HOLD_TIMES = hold/tick;

    // renice
    if (setpriority(PRIO_PROCESS, 0, nice) != 0) {
        cout << "renice myself failed!" << endl;
        return 1;
    }

    // prepare
    LV1Handler h1; h1.max = 40;
    LV2Handler h2; h2.min = 40; h2.max = 50;
    LV3Handler h3; h3.min = 50;
    IHandler* const handlers[3] = { &h1, &h2, &h3 };

    auto FnPickHandler = [&handlers](int t)->IHandler* {
        for (IHandler* h: handlers) {
            if (h->InRange(t))
                return h;
        }
        return nullptr;
    };

    // switch to manual mode
    while (true) {
        int err = 0;
        int val = 1;

        err = sysctlbyname(ctl_fanlevel, nullptr, 0, &val, sizeof(val));
        if (err != 0) goto label_failed;

        val = 0;
        err = sysctlbyname(ctl_fan, nullptr, 0, &val, sizeof(val));
        if (err != 0) goto label_failed;

        break;

label_failed:
        cout << "can't switch to manual mode!" << endl;
        return 1;
    }

    // begin to work
    cout << DateTime() << " begin" << endl;
    for (IHandler* h = FnPickHandler(10); h != nullptr; ) {
        if (!FetchThermal(thermal))
            break;

        int cpu = thermal[0];
        EmHandleRet ret = h->Handle(cpu); 
        switch (ret) {
        case EmHandleRet::Out: {
            IHandler* _h = FnPickHandler(cpu);
            // avoid busy Handle()
            if (_h != h) {
                h = _h;
                continue;
            }
        }
            break;

        case EmHandleRet::Ok:
            break;

        case EmHandleRet::Err:
        default: {
            h = nullptr;
            continue;
        }
            break;
        }

        usleep(tick*1000);
    }
    cout << DateTime() << " end" << endl;

    // recover to automatic mode
    {
        int val = 1;
        if (sysctlbyname(ctl_fan, nullptr, 0, &val, sizeof(val)) != 0)
            return 1;
    }
    cout << DateTime() << " recovered" << endl;

    return 0;
}
