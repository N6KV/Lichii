// SPDX-FileCopyrightText: Copyright 2026 Lichii Chimera frontend contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// lichii-chimera: runs Lichii without a window for the Chimera frontend.
//
// Chimera starts this program as a child process and drives it over stdin/stdout with the same
// protocol as its libretro host (see chimera_protocol.h):
//
//   lichii-chimera --core lichii --rom <game> --save-dir <profile dir> [--system-dir <dir>]
//                  [--option key=value]... [--port-device port=device]...
//
// * Video: Vulkan on a headless surface; every presented frame is composited into a CPU buffer
//   (the renderer's screenshot path, as GPU trace replay uses it) and sent as RGBA8.
// * Audio: the null sink plays in real time; its samples are captured and sent with the frames.
// * Input: each Run command sets Lichii's virtual gamepad (players 1 and 2).
// * Profile: every Lichii directory (keys, NAND with firmware and saves, config, cache) lives
//   below --save-dir, apart from a regular Lichii installation.
//
// Options: lichii_output=<width>x<height> (composited size, default 1280x720),
//          lichii_docked=0|1 (default 1).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#else
#include <unistd.h>
#endif

#include <fmt/format.h>

#include "audio_core/sink/null_sink.h"
#include "chimera_protocol.h"
#include "common/fs/fs_paths.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/scm_rev.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/registered_cache.h"
#include "core/file_sys/vfs/vfs_real.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/acc/profile_manager.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/hle/service/am/applet_manager.h"
#include "core/hle/service/filesystem/filesystem.h"
#include "hid_core/hid_core.h"
#include "core/loader/loader.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "lichii_cmd/sdl_config.h"
#include "network/network.h"
#include "video_core/gpu.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/renderer_base.h"

namespace {

namespace P = ChimeraProtocol;
using VB = InputCommon::VirtualGamepad::VirtualButton;

class HeadlessContext final : public Core::Frontend::GraphicsContext {};

/// A window that never appears: Vulkan presents to VK_EXT_headless_surface.
class HeadlessWindow final : public Core::Frontend::EmuWindow {
public:
    HeadlessWindow(u32 width, u32 height) {
        window_info.type = Core::Frontend::WindowSystemType::Headless;
        UpdateCurrentFramebufferLayout(width, height);
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<HeadlessContext>();
    }
    bool IsShown() const override {
        return true;
    }
};

struct Args {
    std::string rom;
    std::filesystem::path save_dir;
    std::map<std::string, std::string> options;
};

std::optional<Args> ParseArgs(int argc, char** argv, std::string& error) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg{argv[i]};
        const auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (arg == "--rom" || arg == "--save-dir" || arg == "--core" || arg == "--system-dir" ||
            arg == "--option" || arg == "--port-device") {
            const char* v = value();
            if (!v) {
                error = fmt::format("{} needs a value", arg);
                return std::nullopt;
            }
            if (arg == "--rom") {
                args.rom = v;
            } else if (arg == "--save-dir") {
                args.save_dir = v;
            } else if (arg == "--option") {
                const std::string pair{v};
                const auto eq = pair.find('=');
                if (eq == std::string::npos) {
                    error = "--option expects key=value";
                    return std::nullopt;
                }
                args.options[pair.substr(0, eq)] = pair.substr(eq + 1);
            }
            // --core and --system-dir are part of the host protocol; Lichii needs neither.
            // --port-device: players 1 and 2 are always Pro Controllers here.
        } else {
            error = fmt::format("unknown argument {}", arg);
            return std::nullopt;
        }
    }
    if (args.rom.empty()) {
        error = "--rom is required";
        return std::nullopt;
    }
    if (args.save_dir.empty()) {
        error = "--save-dir is required";
        return std::nullopt;
    }
    return args;
}

/// Points every Lichii directory below `root` (same layout as a portable `user/` folder).
void UseProfileDirectory(const std::filesystem::path& root) {
    using Common::FS::EdenPath;
    // SetEdenPath only accepts existing folders.
    const auto SetEdenPath = [](EdenPath which, const std::filesystem::path& path) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        Common::FS::SetEdenPath(which, path);
    };
    SetEdenPath(EdenPath::EdenDir, root);
    SetEdenPath(EdenPath::AmiiboDir, root / AMIIBO_DIR);
    SetEdenPath(EdenPath::CrashDumpsDir, root / CRASH_DUMPS_DIR);
    SetEdenPath(EdenPath::DumpDir, root / DUMP_DIR);
    SetEdenPath(EdenPath::KeysDir, root / KEYS_DIR);
    SetEdenPath(EdenPath::LoadDir, root / LOAD_DIR);
    SetEdenPath(EdenPath::LogDir, root / LOG_DIR);
    SetEdenPath(EdenPath::LosslessDir, root / LOSSLESS_DIR);
    SetEdenPath(EdenPath::NANDDir, root / NAND_DIR);
    SetEdenPath(EdenPath::PlayTimeDir, root / PLAY_TIME_DIR);
    SetEdenPath(EdenPath::PostPresetDir, root / POST_PRESET_DIR);
    SetEdenPath(EdenPath::PostShaderDir, root / POST_SHADER_DIR);
    SetEdenPath(EdenPath::SaveDir, root / NAND_DIR);
    SetEdenPath(EdenPath::ScreenshotsDir, root / SCREENSHOTS_DIR);
    SetEdenPath(EdenPath::SDMCDir, root / SDMC_DIR);
    SetEdenPath(EdenPath::TASDir, root / TAS_DIR);
    SetEdenPath(EdenPath::IconsDir, root / ICONS_DIR);
    SetEdenPath(EdenPath::StreamlineDir, root / STREAMLINE_DIR);
    SetEdenPath(EdenPath::FidelityFxDir, root / FIDELITYFX_DIR);
    SetEdenPath(EdenPath::ConfigDir, root / CONFIG_DIR);
    SetEdenPath(EdenPath::CacheDir, root / CACHE_DIR);
    SetEdenPath(EdenPath::ShaderDir, root / CACHE_DIR / SHADER_DIR);
    Common::FS::CreateEdenPaths();
}

/// Audio captured from the null sink, handed out with each frame.
class AudioQueue {
public:
    void Push(std::span<const s16> samples, u32 channels) {
        std::scoped_lock lock{mutex};
        const std::size_t frames = channels == 0 ? 0 : samples.size() / channels;
        for (std::size_t f = 0; f < frames; ++f) {
            // Stereo out: front left/right of a 2- or 6-channel stream.
            queue.push_back(samples[f * channels]);
            queue.push_back(samples[f * channels + (channels > 1 ? 1 : 0)]);
        }
        // Keep at most 0.5 s (a stalled Chimera must not grow this forever).
        constexpr std::size_t limit = 48000;
        while (queue.size() > limit) {
            queue.pop_front();
        }
    }
    std::vector<s16> Take() {
        std::scoped_lock lock{mutex};
        std::vector<s16> out(queue.begin(), queue.end());
        queue.clear();
        return out;
    }

private:
    std::mutex mutex;
    std::deque<s16> queue;
};

/// Requests the composited picture of the next present and waits for it.
class FrameCapture {
public:
    FrameCapture(Core::System& system, u32 width, u32 height)
        : system{system}, width{width}, height{height}, bgra(std::size_t{width} * height * 4) {}

    /// Starts a capture unless one is pending.
    void Request() {
        std::scoped_lock lock{mutex};
        if (pending) {
            return;
        }
        pending = true;
        ready = false;
        system.GPU().Renderer().RequestScreenshot(
            bgra.data(),
            [this](bool) {
                std::scoped_lock inner{mutex};
                ready = true;
                pending = false;
                cv.notify_all();
            },
            Layout::DefaultFrameLayout(width, height));
    }

    /// RGBA pixels of a finished capture within `timeout`, or empty (repeat the last picture).
    std::span<const u8> Wait(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex};
        if (!cv.wait_for(lock, timeout, [this] { return ready; })) {
            return {};
        }
        ready = false;
        rgba.resize(bgra.size());
        for (std::size_t i = 0; i < bgra.size(); i += 4) {
            rgba[i] = bgra[i + 2];
            rgba[i + 1] = bgra[i + 1];
            rgba[i + 2] = bgra[i];
            rgba[i + 3] = 255;
        }
        return rgba;
    }

private:
    Core::System& system;
    u32 width;
    u32 height;
    std::vector<u8> bgra;
    std::vector<u8> rgba;
    std::mutex mutex;
    std::condition_variable cv;
    bool pending = false;
    bool ready = false;
};

/// Reads commands on a thread of its own, so the main loop can notice when Chimera stops
/// sending them (pause) without blocking on stdin.
class CommandReader {
public:
    CommandReader()
        : thread{[this] {
              while (auto command = P::ReadCommand(stdin)) {
                  std::scoped_lock lock{mutex};
                  queue.push_back(*command);
                  cv.notify_one();
                  if (command->kind == P::CommandKind::Quit) {
                      break;
                  }
              }
              std::scoped_lock lock{mutex};
              ended = true;
              cv.notify_one();
          }} {}

    ~CommandReader() {
        // Blocked in fread until Chimera closes stdin; the process exits right after.
        thread.detach();
    }

    /// The next command within `timeout`, or nullopt (see Ended()).
    std::optional<P::Command> Next(std::chrono::milliseconds timeout) {
        std::unique_lock lock{mutex};
        cv.wait_for(lock, timeout, [this] { return !queue.empty() || ended; });
        if (queue.empty()) {
            return std::nullopt;
        }
        const P::Command command = queue.front();
        queue.pop_front();
        return command;
    }

    bool Ended() {
        std::scoped_lock lock{mutex};
        return ended && queue.empty();
    }

private:
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<P::Command> queue;
    bool ended = false;
    std::thread thread;
};

/// RetroPad state -> Lichii's virtual gamepad. Nintendo layout: the RetroPad's positions match
/// the Switch controller's (A east, B south, X north, Y west).
void ApplyPad(InputCommon::VirtualGamepad& gamepad, std::size_t player, const P::Pad& pad) {
    const auto set = [&](VB button, u16 bit) {
        gamepad.SetButtonState(player, button, (pad.buttons & bit) != 0);
    };
    set(VB::ButtonA, P::Joypad::A);
    set(VB::ButtonB, P::Joypad::B);
    set(VB::ButtonX, P::Joypad::X);
    set(VB::ButtonY, P::Joypad::Y);
    set(VB::TriggerL, P::Joypad::L);
    set(VB::TriggerR, P::Joypad::R);
    set(VB::TriggerZL, P::Joypad::L2);
    set(VB::TriggerZR, P::Joypad::R2);
    set(VB::StickL, P::Joypad::L3);
    set(VB::StickR, P::Joypad::R3);
    set(VB::ButtonPlus, P::Joypad::Start);
    set(VB::ButtonMinus, P::Joypad::Select);
    set(VB::ButtonUp, P::Joypad::Up);
    set(VB::ButtonDown, P::Joypad::Down);
    set(VB::ButtonLeft, P::Joypad::Left);
    set(VB::ButtonRight, P::Joypad::Right);
    // libretro: down positive; Switch sticks: up positive.
    const auto axis = [&](P::Axis a) { return static_cast<float>(pad.axes[a]) / 32767.0f; };
    gamepad.SetStickPosition(player, InputCommon::VirtualGamepad::VirtualStick::Left,
                             axis(P::LeftX), -axis(P::LeftY));
    gamepad.SetStickPosition(player, InputCommon::VirtualGamepad::VirtualStick::Right,
                             axis(P::RightX), -axis(P::RightY));
}

/// Moves the protocol to its own descriptor and sends stdout (Lichii's console log) to stderr,
/// so log lines can never corrupt the protocol.
std::FILE* TakeProtocolOutput() {
    std::fflush(stdout);
#ifdef _WIN32
    const int fd = _dup(_fileno(stdout));
    _dup2(_fileno(stderr), _fileno(stdout));
    _setmode(fd, _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
    return _fdopen(fd, "wb");
#else
    const int fd = dup(STDOUT_FILENO);
    dup2(STDERR_FILENO, STDOUT_FILENO);
    return fdopen(fd, "wb");
#endif
}

std::string LoadErrorText(Core::SystemResultStatus status) {
    switch (status) {
    case Core::SystemResultStatus::ErrorGetLoader:
        return "Lichii cannot read this game file";
    case Core::SystemResultStatus::ErrorMemoryAllocation:
        return "not enough memory to start the game";
    case Core::SystemResultStatus::ErrorNotInitialized:
        return "the CPU core is not initialized";
    case Core::SystemResultStatus::ErrorVideoCore:
        return "the Vulkan renderer could not start";
    default: {
        const u16 loader_id = u16(Core::SystemResultStatus::ErrorLoader);
        const u16 error_id = u16(status) - loader_id;
        return fmt::format("the game could not be loaded: {} (are your keys and firmware "
                           "added?)",
                           Loader::GetResultStatusString(Loader::ResultStatus(error_id)));
    }
    }
}

} // namespace

int main(int argc, char** argv) {
    std::FILE* proto_out = TakeProtocolOutput();
    if (!proto_out) {
        std::fprintf(stderr, "lichii-chimera: cannot open the protocol output\n");
        return 1;
    }
    P::Writer writer{proto_out};
    const auto fail = [&](const std::string& message) {
        std::fprintf(stderr, "lichii-chimera: %s\n", message.c_str());
        writer.Error(message);
        return 1;
    };

    std::string arg_error;
    const auto args = ParseArgs(argc, argv, arg_error);
    if (!args) {
        return fail(arg_error);
    }
    const auto option = [&](const char* key, const char* fallback) {
        const auto it = args->options.find(key);
        return it == args->options.end() ? std::string{fallback} : it->second;
    };
    u32 out_width = 1280;
    u32 out_height = 720;
    if (std::sscanf(option("lichii_output", "1280x720").c_str(), "%ux%u", &out_width,
                    &out_height) != 2 ||
        out_width == 0 || out_height == 0 || out_width > 3840 || out_height > 2160) {
        return fail("lichii_output must be <width>x<height> up to 3840x2160");
    }

    UseProfileDirectory(args->save_dir);
    Common::Log::Initialize();
    Common::Log::Start();

    Core::System system;
    InputCommon::InputSubsystem input_subsystem;
    SdlConfig config{std::nullopt};

    // Settings this frontend relies on (not persisted beyond this run's config file).
    Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
    Settings::values.sink_id.SetValue(Settings::AudioEngine::Null);
    Settings::values.use_docked_mode.SetValue(option("lichii_docked", "1") == "1"
                                                  ? Settings::ConsoleMode::Docked
                                                  : Settings::ConsoleMode::Handheld);
    {
        auto& players = Settings::values.players.GetValue();
        players[0].connected = true;
        players[0].controller_type = Settings::ControllerType::ProController;
    }

    AudioQueue audio;
    AudioCore::Sink::GetNullSinkCapture() = [&audio](std::span<const s16> samples, u32 channels) {
        audio.Push(samples, channels);
    };

    const bool network = Network::Init();
    system.Initialize();
    system.ApplySettings();

    // Registers the input engines, the virtual gamepad among them (lichii-cmd's SDL window does
    // this; there is no window here).
    input_subsystem.Initialize();
    system.HIDCore().ReloadInputDevices();

    HeadlessWindow window{out_width, out_height};
    system.SetContentProvider(std::make_unique<FileSys::ContentProviderUnion>());
    system.SetFilesystem(std::make_shared<FileSys::RealVfsFilesystem>());
    system.GetFileSystemController().CreateFactories(*system.GetFilesystem());
    system.GetUserChannel().clear();
    system.GetProfileManager().ResetUserSaveFile();

    Service::AM::FrontendAppletParameters load_parameters{
        .applet_id = Service::AM::AppletId::Application,
    };
    const auto status = system.Load(window, args->rom, load_parameters);
    if (status != Core::SystemResultStatus::Success) {
        return fail(LoadErrorText(status));
    }

    std::atomic_bool exited{false};
    system.RegisterExitCallback([&exited] { exited = true; });
    system.GPU().Start();
    system.GetCpuManager().OnGpuReady();
    if (Settings::values.use_disk_shader_cache.GetValue()) {
        system.Renderer().ReadRasterizer()->LoadDiskResources(
            system.GetApplicationProcessProgramID(), std::stop_token{},
            [](VideoCore::LoadCallbackStage, size_t, size_t) {});
    }
    void(system.Run());

    const float aspect = static_cast<float>(out_width) / static_cast<float>(out_height);
    if (!writer.Ready(out_width, out_height, aspect, 60.0, 48000.0, "Lichii",
                      Common::g_build_fullname)) {
        return 1;
    }

    FrameCapture capture{system, out_width, out_height};
    auto* gamepad = input_subsystem.GetVirtualGamepad();
    CommandReader commands;
    bool paused = false;
    int result = 0;
    while (true) {
        // Chimera pauses by no longer asking for frames; Lichii runs on its own clock, so it is
        // paused here and resumed with the next command.
        auto command = commands.Next(std::chrono::milliseconds{150});
        if (!command) {
            if (commands.Ended()) {
                break;
            }
            if (!paused && system.IsPoweredOn()) {
                void(system.Pause());
                paused = true;
            }
            continue;
        }
        if (command->kind == P::CommandKind::Quit) {
            break;
        }
        if (paused) {
            void(system.Run());
            paused = false;
        }
        if (command->kind == P::CommandKind::Reset) {
            continue; // Not offered for Lichii.
        }
        if (exited) {
            writer.Error("the game has exited");
            result = 1;
            break;
        }
        for (std::size_t player = 0; player < 2; ++player) {
            ApplyPad(*gamepad, player, command->pads[player]);
        }
        capture.Request();
        // Up to one 60 Hz frame for the next picture; otherwise Chimera keeps the last one.
        const auto pixels = capture.Wait(std::chrono::milliseconds{16});
        const auto samples = audio.Take();
        if (!writer.Frame(out_width, out_height, aspect, pixels, samples)) {
            break; // Chimera went away.
        }
    }

    AudioCore::Sink::GetNullSinkCapture() = nullptr;
    if (system.IsPoweredOn()) {
        void(system.Pause());
        system.ShutdownMainProcess();
    }
    input_subsystem.Shutdown();
    if (network) {
        Network::Shutdown();
    }
    writer.Bye();
    return result;
}

#define VMA_IMPLEMENTATION
#include "video_core/vulkan_common/vma.h"
