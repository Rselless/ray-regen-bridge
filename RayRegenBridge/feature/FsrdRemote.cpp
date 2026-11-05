#include "pch.h"
#include "FsrdRemote.h"
#include "FsrdFrameDump.h"
#include "FsrdMetrics.h"

#include <Config.h>
#include <Util.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <variant>

namespace
{
using Clock = std::chrono::steady_clock;

constexpr auto PollInterval = std::chrono::milliseconds(500);
constexpr float DefaultMessageSeconds = 8.0f;

std::deque<std::string> queue;
Clock::time_point nextPoll {};
Clock::time_point waitUntil {};
bool waitForDump = false;

std::mutex messageMutex;
std::string message;
Clock::time_point messageUntil {};

// Options a test can change, and their value before the first set (restored by reset)
using FloatOption = CustomOptional<float>;
using TunableOption = CustomOptional<float, NoDefault>;
using IntOption = CustomOptional<int>;
using BoolOption = CustomOptional<bool>;
using OptionPtr = std::variant<FloatOption*, TunableOption*, IntOption*, BoolOption*>;
using Saved = std::variant<std::optional<float>, std::optional<int>, std::optional<bool>>;

std::map<std::string, OptionPtr> Options()
{
    auto* c = Config::Instance();
    return {
        { "DiffuseHitDistance", &c->FsrdDiffuseHitDistance },
        { "SplitMode", &c->FsrdSplitMode },
        { "ReflectionReactive", &c->FsrdReflectionReactive },
        { "SpecHitDistanceScale", &c->FsrdSpecHitDistanceScale },
        { "SentinelMode", &c->FsrdSentinelMode },
        { "ParticleLayer", &c->FsrdParticleLayer },
        { "TransparencyLayer", &c->FsrdTransparencyLayer },
        { "FarPassthrough", &c->FsrdFarPassthrough },
        { "ExposureCompensation", &c->FsrdExposureCompensation },
        { "DefaultSharpness", &c->FsrdDefaultSharpness },
        { "BypassDenoiser", &c->FsrdBypassDenoiser },
        { "StabilityBias", &c->FsrdStabilityBias },
        { "RadianceClipStdK", &c->FsrdRadianceClipStdK },
        { "KernelRelaxation", &c->FsrdKernelRelaxation },
        { "MaxRadiance", &c->FsrdMaxRadiance },
    };
}

std::map<std::string, Saved> saved;

std::string Describe(const OptionPtr& option)
{
    return std::visit(
        [](auto* o) -> std::string
        {
            if (!o->has_value())
                return "default";

            return std::format("{}", o->value());
        },
        option);
}

bool SetOption(const std::string& key, const std::string& text)
{
    auto options = Options();
    auto it = options.find(key);

    if (it == options.end())
        return false;

    if (!saved.contains(key))
    {
        std::visit(
            [&](auto* o)
            {
                using T = std::decay_t<decltype(o->value())>;
                saved[key] = o->has_value() ? std::optional<T>(o->value()) : std::optional<T>();
            },
            it->second);
    }

    try
    {
        std::visit(
            [&](auto* o)
            {
                using T = std::decay_t<decltype(o->value())>;

                if constexpr (std::is_same_v<T, bool>)
                    o->set_volatile_value(text == "1" || text == "true" || text == "on");
                else if constexpr (std::is_same_v<T, int>)
                    o->set_volatile_value(std::stoi(text));
                else
                    o->set_volatile_value(std::stof(text));
            },
            it->second);
    }
    catch (...)
    {
        return false;
    }

    return true;
}

bool ResetOption(const std::string& key)
{
    auto options = Options();
    auto it = options.find(key);
    auto value = saved.find(key);

    if (it == options.end() || value == saved.end())
        return false;

    std::visit(
        [&](auto* o)
        {
            using T = std::decay_t<decltype(o->value())>;
            *o = std::get<std::optional<T>>(value->second);
        },
        it->second);

    saved.erase(value);
    return true;
}

void SetMessage(const std::string& text, float seconds)
{
    std::lock_guard lock(messageMutex);
    message = text;
    messageUntil = Clock::now() + std::chrono::milliseconds((int64_t) (seconds * 1000.0f));
}

void Poll()
{
    const auto file = FsrdRemote::Folder() / "command.txt";
    std::error_code ec;

    if (!std::filesystem::exists(file, ec))
        return;

    // Renamed before reading, so a file being written again isn't half read twice
    const auto taken = FsrdRemote::Folder() / "command.running";
    std::filesystem::rename(file, taken, ec);

    if (ec)
        return;

    std::ifstream in(taken);
    std::string line;
    int added = 0;

    while (std::getline(in, line))
    {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();

        if (!line.empty() && line[0] != '#')
        {
            queue.push_back(line);
            added++;
        }
    }

    in.close();
    std::filesystem::remove(taken, ec);
    FsrdRemote::Status(std::format("read {} commands, {} queued", added, queue.size()));
}

// Runs one command, false when it has to wait (it stays at the front of the queue)
bool Execute(const std::string& line, FsrdFrameDump* dump, FsrdMetrics* metrics)
{
    std::istringstream words(line);
    std::string command;
    words >> command;

    auto rest = [&]()
    {
        std::string text;
        std::getline(words >> std::ws, text);
        return text;
    };

    auto startDump = [&](FsrdFrameDump::Request request)
    {
        // The dump runs with the denoiser; in bypass it would never start. The captured input color is the bypass image.
        if (dump == nullptr || Config::Instance()->FsrdBypassDenoiser.value_or_default())
        {
            FsrdRemote::Status("error: " + line + ": no denoiser running");
            return true;
        }

        if (dump->Busy())
            return false;

        dump->Start(request);
        FsrdRemote::Status(std::format("started: {}", line));
        waitForDump = true; // captures run one at a time, the next command waits for this one
        return true;
    };

    if (command == "say")
    {
        SetMessage(rest(), DefaultMessageSeconds);
    }
    else if (command == "sayfor")
    {
        float seconds = DefaultMessageSeconds;
        words >> seconds;
        SetMessage(rest(), seconds);
    }
    else if (command == "wait")
    {
        float seconds = 1.0f;
        words >> seconds;
        waitUntil = Clock::now() + std::chrono::milliseconds((int64_t) (seconds * 1000.0f));
    }
    else if (command == "waitdump")
    {
        waitForDump = true;
    }
    else if (command == "capture")
    {
        FsrdFrameDump::Request request;
        words >> request.label;
        return startDump(request);
    }
    else if (command == "sequence")
    {
        FsrdFrameDump::Request request;
        words >> request.label >> request.count >> request.intervalSeconds;
        return startDump(request);
    }
    else if (command == "burst")
    {
        FsrdFrameDump::Request request;
        request.reduced = true;
        words >> request.label >> request.count;
        return startDump(request);
    }
    else if (command == "set")
    {
        std::string key, value;
        words >> key >> value;

        if (SetOption(key, value))
            FsrdRemote::Status(std::format("set {} = {}", key, Describe(Options()[key])));
        else
            FsrdRemote::Status("error: " + line + ": unknown option or value");
    }
    else if (command == "reset")
    {
        std::string key;
        words >> key;

        if (ResetOption(key))
            FsrdRemote::Status(std::format("reset {} = {}", key, Describe(Options()[key])));
        else
            FsrdRemote::Status("error: " + line + ": not set");
    }
    else if (command == "resetall")
    {
        std::vector<std::string> keys;

        for (const auto& [key, value] : saved)
            keys.push_back(key);

        for (const auto& key : keys)
            ResetOption(key);

        FsrdRemote::Status(std::format("reset {} options", keys.size()));
    }
    else if (command == "metrics")
    {
        std::string state;
        words >> state;

        FsrdMetrics::SetEnabled(state == "on");

        FsrdRemote::Status("metrics " + state);
    }
    else
    {
        FsrdRemote::Status("error: unknown command: " + line);
    }

    return true;
}
} // namespace

namespace FsrdRemote
{
std::filesystem::path Folder()
{
    auto& folder = Config::Instance()->FsrdCaptureFolder;

    if (folder.has_value() && !folder.value().empty() && folder.value() != "auto")
        return std::filesystem::path(folder.value());

    return Util::DllPath().parent_path() / "RRB_Captures";
}

void Tick(FsrdFrameDump* dump, FsrdMetrics* metrics)
{
    const auto now = Clock::now();

    if (now >= nextPoll)
    {
        nextPoll = now + PollInterval;
        Poll();
    }

    while (!queue.empty())
    {
        if (Clock::now() < waitUntil)
            return;

        if (waitForDump)
        {
            if (dump != nullptr && dump->Busy())
                return;

            if (dump != nullptr && !dump->LastFolder().empty())
                Status("written: " + dump->LastFolder());

            waitForDump = false;
            continue;
        }

        if (!Execute(queue.front(), dump, metrics))
            return;

        queue.pop_front();
    }

    // A capture started by the last command: report its folder once written
    if (waitForDump && (dump == nullptr || !dump->Busy()))
    {
        if (dump != nullptr && !dump->LastFolder().empty())
            Status("written: " + dump->LastFolder());

        waitForDump = false;
    }
}

void Status(const std::string& line)
{
    static std::mutex fileMutex;
    std::lock_guard lock(fileMutex);

    char stamp[16];
    const auto now = std::time(nullptr);
    std::tm local {};
    localtime_s(&local, &now);
    std::strftime(stamp, sizeof(stamp), "%H:%M:%S", &local);

    std::error_code ec;
    std::filesystem::create_directories(Folder(), ec);
    std::ofstream(Folder() / "status.txt", std::ios::app) << stamp << " " << line << "\n";
    LOG_INFO("[RRB remote] {}", line);
}

std::string Message()
{
    std::lock_guard lock(messageMutex);
    return Clock::now() < messageUntil ? message : std::string();
}
} // namespace FsrdRemote
