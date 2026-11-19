// SPDX-FileCopyrightText: Copyright 2026 Lichii Chimera frontend contributors
// SPDX-License-Identifier: GPL-3.0-or-later

// The wire protocol of Chimera's emulator host processes (see chimera-retro-host's lib.rs in the
// Chimera project). Binary, little-endian; Chimera writes commands to stdin, the host writes
// messages to stdout, each message prefixed with the magic bytes C3 48.
//
//   host -> Chimera   1 Ready   width u32, height u32, aspect f32, fps f64, rate f64, name, version
//                     2 Frame   width u32, height u32 (0 = repeat), aspect f32, RGBA8 pixels,
//                               audio_frames u32, stereo i16 samples
//                     3 Error   message
//                     5 Bye
//   Chimera -> host   1 Run     per port (4): RetroPad bits u16, axes i16 x 6
//                     2 Reset
//                     3 Quit
//   string = length u32 + UTF-8 bytes

#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace ChimeraProtocol {

constexpr std::array<std::uint8_t, 2> MAGIC{0xC3, 0x48};
constexpr std::size_t PORTS = 4;
constexpr std::size_t AXES = 6;

/// RetroPad bits (libretro RETRO_DEVICE_ID_JOYPAD_*), positioned like a Nintendo pad.
namespace Joypad {
constexpr std::uint16_t B = 1 << 0;
constexpr std::uint16_t Y = 1 << 1;
constexpr std::uint16_t Select = 1 << 2;
constexpr std::uint16_t Start = 1 << 3;
constexpr std::uint16_t Up = 1 << 4;
constexpr std::uint16_t Down = 1 << 5;
constexpr std::uint16_t Left = 1 << 6;
constexpr std::uint16_t Right = 1 << 7;
constexpr std::uint16_t A = 1 << 8;
constexpr std::uint16_t X = 1 << 9;
constexpr std::uint16_t L = 1 << 10;
constexpr std::uint16_t R = 1 << 11;
constexpr std::uint16_t L2 = 1 << 12;
constexpr std::uint16_t R2 = 1 << 13;
constexpr std::uint16_t L3 = 1 << 14;
constexpr std::uint16_t R3 = 1 << 15;
} // namespace Joypad

/// Axis indices: sticks -32768..32767 (right and down positive), triggers 0..32767.
enum Axis : std::size_t { LeftX, LeftY, RightX, RightY, L2Axis, R2Axis };

struct Pad {
    std::uint16_t buttons{};
    std::array<std::int16_t, AXES> axes{};
};

enum class CommandKind { Run, Reset, Quit };

struct Command {
    CommandKind kind{};
    std::array<Pad, PORTS> pads{};
};

/// Reads exactly `size` bytes; false at end of stream or on error.
inline bool ReadExact(std::FILE* in, void* data, std::size_t size) {
    return std::fread(data, 1, size, in) == size;
}

inline std::uint16_t Le16(const std::uint8_t* b) {
    return static_cast<std::uint16_t>(b[0] | (b[1] << 8));
}

/// Next command, or nullopt at end of input or on a malformed command.
inline std::optional<Command> ReadCommand(std::FILE* in) {
    std::uint8_t tag{};
    if (!ReadExact(in, &tag, 1)) {
        return std::nullopt;
    }
    Command command{};
    switch (tag) {
    case 1: {
        std::array<std::uint8_t, PORTS * (2 + 2 * AXES)> payload{};
        if (!ReadExact(in, payload.data(), payload.size())) {
            return std::nullopt;
        }
        const std::uint8_t* p = payload.data();
        for (auto& pad : command.pads) {
            pad.buttons = Le16(p);
            p += 2;
            for (auto& axis : pad.axes) {
                axis = static_cast<std::int16_t>(Le16(p));
                p += 2;
            }
        }
        command.kind = CommandKind::Run;
        return command;
    }
    case 2:
        command.kind = CommandKind::Reset;
        return command;
    case 3:
        command.kind = CommandKind::Quit;
        return command;
    default:
        return std::nullopt;
    }
}

/// Builds messages in a buffer and writes them in one go.
class Writer {
public:
    explicit Writer(std::FILE* out) : out{out} {}

    bool Ready(std::uint32_t width, std::uint32_t height, float aspect, double fps, double rate,
               std::string_view name, std::string_view version) {
        Begin(1);
        U32(width);
        U32(height);
        F32(aspect);
        F64(fps);
        F64(rate);
        Str(name);
        Str(version);
        return Flush();
    }

    /// `pixels` empty = repeat the previous picture.
    bool Frame(std::uint32_t width, std::uint32_t height, float aspect,
               std::span<const std::uint8_t> pixels, std::span<const std::int16_t> stereo) {
        Begin(2);
        if (pixels.empty()) {
            width = 0;
            height = 0;
        }
        U32(width);
        U32(height);
        F32(aspect);
        buffer.insert(buffer.end(), pixels.begin(), pixels.end());
        U32(static_cast<std::uint32_t>(stereo.size() / 2));
        for (const std::int16_t sample : stereo) {
            U16(static_cast<std::uint16_t>(sample));
        }
        return Flush();
    }

    bool Error(std::string_view message) {
        Begin(3);
        Str(message);
        return Flush();
    }

    bool Bye() {
        Begin(5);
        return Flush();
    }

private:
    void Begin(std::uint8_t tag) {
        buffer.clear();
        buffer.push_back(MAGIC[0]);
        buffer.push_back(MAGIC[1]);
        buffer.push_back(tag);
    }
    void U16(std::uint16_t v) {
        buffer.push_back(static_cast<std::uint8_t>(v));
        buffer.push_back(static_cast<std::uint8_t>(v >> 8));
    }
    void U32(std::uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buffer.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void U64(std::uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buffer.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
        }
    }
    void F32(float v) {
        std::uint32_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        U32(bits);
    }
    void F64(double v) {
        std::uint64_t bits;
        std::memcpy(&bits, &v, sizeof bits);
        U64(bits);
    }
    void Str(std::string_view s) {
        U32(static_cast<std::uint32_t>(s.size()));
        buffer.insert(buffer.end(), s.begin(), s.end());
    }
    bool Flush() {
        const bool ok = std::fwrite(buffer.data(), 1, buffer.size(), out) == buffer.size();
        return std::fflush(out) == 0 && ok;
    }

    std::FILE* out;
    std::vector<std::uint8_t> buffer;
};

} // namespace ChimeraProtocol
