#include "ClicknetServer.h"
#include <atomic>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace
{
std::atomic<bool> shutdownRequested{false};

void SignalHandler(int)
{
    shutdownRequested.store(true, std::memory_order_relaxed);
}

bool ParseFloat(const char* text, float& value)
{
    char* end = nullptr;
    value = std::strtof(text, &end);
    return end != text && *end == '\0' && std::isfinite(value);
}

std::filesystem::path FindDefaultMap(const std::filesystem::path& executable)
{
    // Explorer and Visual Studio choose different working directories. Search
    // both the working directory and executable ancestors in this checkout.
    for (auto directory : {std::filesystem::current_path(), executable.parent_path()})
    {
        while (!directory.empty())
        {
            const auto candidate = directory / "Saved/Box3DTest.box3d";
            std::error_code error;
            if (std::filesystem::is_regular_file(candidate, error)) return candidate;
            const auto parent = directory.parent_path();
            if (parent == directory) break;
            directory = parent;
        }
    }
    return {};
}
}

int main(int argc, char** argv)
{
    std::string mapPath;
    std::filesystem::path statsPath;
    b3Pos spawn{0.0f, 0.0f, 2.0f};
    float interestRadius = 40.0f;
    const unsigned processors = std::thread::hardware_concurrency();
    unsigned replicationWorkers = processors > 1 ? (std::min)(3u, processors - 1) : 0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        if (arg == "--map" && i + 1 < argc)
        {
            mapPath = argv[++i];
        }
        else if (arg == "--stats" && i + 1 < argc)
        {
            statsPath = argv[++i];
        }
        else if (arg == "--spawn" && i + 3 < argc
            && ParseFloat(argv[i + 1], spawn.x)
            && ParseFloat(argv[i + 2], spawn.y)
            && ParseFloat(argv[i + 3], spawn.z))
        {
            i += 3;
        }
        else if (arg == "--replication-workers" && i + 1 < argc)
        {
            const std::string value = argv[++i];
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
            if (value.empty() || value[0] == '-' || *end != '\0' || parsed > 16)
            {
                std::cerr << "--replication-workers requires an integer from 0 to 16\n";
                return 2;
            }
            replicationWorkers = static_cast<unsigned>(parsed);
        }
        else if (arg == "--interest-radius" && i + 1 < argc
            && ParseFloat(argv[i + 1], interestRadius) && interestRadius >= 5.0f && interestRadius <= 500.0f)
        {
            ++i;
        }
        else
        {
            std::cerr << "Usage: clicknetserver [--map path-to-level.box3d] [--spawn x y z] [--stats path-to-live.json] [--interest-radius meters] [--replication-workers 0..16]\n"
                      << "Spawn coordinates are Box3D meters. Default: 0 0 2.\n";
            return 2;
        }
    }

    if (statsPath.empty())
    {
        std::filesystem::path directory = std::filesystem::absolute(argv[0]).parent_path();
        if ((directory.filename() == "Release" || directory.filename() == "Debug")
            && directory.parent_path().filename() == "x64")
        {
            directory = directory.parent_path().parent_path();
        }
        statsPath = directory / "bandwidth-live.json";
    }
    statsPath = std::filesystem::absolute(statsPath);
    if (mapPath.empty())
    {
        mapPath = FindDefaultMap(std::filesystem::absolute(argv[0])).string();
        if (mapPath.empty())
        {
            std::cerr << "No default map found; pass --map path-to-level.box3d\n";
            return 2;
        }
    }

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    ClicknetServer server;
    if (!server.Init(mapPath, spawn, statsPath, interestRadius, replicationWorkers))
    {
        server.Shutdown();
        return 1;
    }
    std::cout << "ClicknetServer listening on UDP 27015 (60 Hz). Press Ctrl+C to stop.\n";
    std::cout << "Live bandwidth file: " << statsPath.string() << '\n';
    server.Run(shutdownRequested);
    server.Shutdown();
    return 0;
}
