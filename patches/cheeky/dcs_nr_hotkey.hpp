#pragma once
// DCS VR Control: in-game DLSS-NR toggle for the standalone host. The key is chosen in
// DCS VR Control and passed as DCSVR_NR_HOTKEY="virtual-key:modifiers" (Ctrl 1, Alt 2,
// Shift 4); "0:0" or a missing variable disables it. The toggle goes through the same runtime
// command channel as the F8 overlay with "set_session": applied at once and never saved,
// so each launch starts from the applied profile.
#include <cstdint>
#include <string>
#include <string_view>

namespace cheeky::standalone {
struct DcsNrHotkeySpec { unsigned virtual_key{}; unsigned modifiers{}; };

// Parses "virtual-key:modifiers" in decimal. Anything else leaves the hotkey disabled.
inline bool dcs_nr_parse_hotkey(std::string_view text, DcsNrHotkeySpec& spec) noexcept {
    const auto colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= text.size()) return false;
    unsigned values[2]{};
    std::string_view parts[2]{text.substr(0, colon), text.substr(colon + 1)};
    for (int i = 0; i < 2; ++i) {
        if (parts[i].size() > 4) return false;
        for (const char c : parts[i]) {
            if (c < '0' || c > '9') return false;
            values[i] = values[i] * 10U + unsigned(c - '0');
        }
    }
    if (values[0] == 0 || values[0] > 0xFE || values[1] > 7) return false;
    spec = {values[0], values[1]};
    return true;
}

// The exact combination: the key plus exactly the chosen modifiers (either side), so Ctrl+Shift+F12
// does not fire on Ctrl+Alt+Shift+F12. key_down(virtual_key) reports the physical state.
template<class KeyDown>
bool dcs_nr_hotkey_down(const DcsNrHotkeySpec& spec, KeyDown key_down) {
    if (!spec.virtual_key || !key_down(spec.virtual_key)) return false;
    constexpr unsigned modifier_keys[3]{0x11U /*VK_CONTROL*/, 0x12U /*VK_MENU*/, 0x10U /*VK_SHIFT*/};
    for (unsigned bit = 0; bit < 3; ++bit)
        if (key_down(modifier_keys[bit]) != (((spec.modifiers >> bit) & 1U) != 0)) return false;
    return true;
}

// Reads "NrEnabled" from the runtime snapshot's flat "settings" object.
inline bool dcs_nr_enabled_from_snapshot(std::string_view json, bool& enabled) noexcept {
    constexpr std::string_view settings_key = "\"settings\":{";
    constexpr std::string_view nr_key = "\"NrEnabled\":";
    const auto settings = json.find(settings_key);
    if (settings == std::string_view::npos) return false;
    const auto end = json.find('}', settings + settings_key.size());
    const auto key = json.find(nr_key, settings + settings_key.size());
    if (end == std::string_view::npos || key == std::string_view::npos || key > end) return false;
    auto value = json.substr(key + nr_key.size(), end - key - nr_key.size());
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) value.remove_prefix(1);
    const auto token = value.substr(0, value.find_first_of(",} \t\r\n"));
    if (token == "true" || token == "1") { enabled = true; return true; }
    if (token == "false" || token == "0") { enabled = false; return true; }
    return false;
}

inline std::string dcs_nr_toggle_command(std::uint64_t request, bool enable) {
    return "1\n" + std::to_string(request) + "\nset_session\nNrEnabled=" + (enable ? "1" : "0") + "\n";
}

struct DcsNrHotkey {
    bool was_down{};
    std::uint64_t request{0x5C000000ULL}; // Distinct from overlay request ids.
    enum class Result { idle, toggled, snapshot_failed, command_failed };
    // down: key currently held; foreground: the game owns the foreground window.
    // Snapshot(std::string&) -> bool, Command(const char*) -> bool, Log(const char*).
    template<class Snapshot, class Command, class Log>
    Result poll(bool down, bool foreground, Snapshot snapshot, Command command, Log log) {
        const bool pressed = down && !was_down;
        was_down = down;
        if (!pressed || !foreground) return Result::idle;
        std::string json;
        bool enabled{};
        if (!snapshot(json) || !dcs_nr_enabled_from_snapshot(json, enabled)) {
            log("DLSS-NR hotkey ignored: runtime settings unavailable");
            return Result::snapshot_failed;
        }
        const auto text = dcs_nr_toggle_command(++request, !enabled);
        if (!command(text.c_str())) {
            log("DLSS-NR hotkey rejected by the runtime");
            return Result::command_failed;
        }
        log(!enabled ? "DLSS-NR hotkey toggled enabled=yes" : "DLSS-NR hotkey toggled enabled=no");
        return Result::toggled;
    }
};
} // namespace cheeky::standalone
