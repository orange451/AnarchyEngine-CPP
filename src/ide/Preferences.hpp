#pragma once

#include "PropertyBag.hpp"

#include <filesystem>
#include <string>

namespace ide {

// The studio's settings for this user, kept in preferences.json in the config
// folder. Keys it does not know are kept when it writes, so a newer studio's
// settings survive an older one.
class Preferences {
public:
    // file: where they are kept. Empty: nothing is read or written.
    explicit Preferences(std::filesystem::path file);

    const std::filesystem::path& file() const { return file_; }
    // Why the file could not be read, or is not a JSON object. The settings
    // are then the defaults, and writing replaces the file.
    const std::string& load_error() const { return load_error_; }

    // The id of the theme the studio draws with. "light" when none was chosen.
    std::string theme() const;
    void set_theme(const std::string& id);

    // The most frames a second the studio draws. kUncappedFrameRate draws as
    // fast as the machine allows. Other values are kept from kMinFrameRate to
    // kMaxFrameRate, so a typo cannot leave the studio too slow to use.
    static constexpr int kUncappedFrameRate = -1;
    static constexpr int kDefaultFrameRate = 120;
    static constexpr int kMinFrameRate = 15;
    static constexpr int kMaxFrameRate = 1000;
    int frame_rate() const;
    void set_frame_rate(int fps);
    // What Stage::setMaxFrameRate takes for fps. Uncapped is 0 there.
    static double stage_frame_rate(int fps);

    // The Assets pane's view: "icons", "list", or "columns". "icons" when none
    // was chosen or the file holds another value.
    std::string assets_view() const;
    void set_assets_view(const std::string& view);

    static constexpr double kMinZoom = 0.5;
    static constexpr double kMaxZoom = 3.0;
    double zoom() const;
    void set_zoom(double zoom);

    std::string editor_font() const;
    void set_editor_font(const std::string& family);

    // Whether the studio runs its MCP server, so AI clients can drive it. Off
    // unless turned on; ANARCHY_MCP and its kin can still decide (see decide_mcp).
    bool mcp_enabled() const;
    void set_mcp_enabled(bool enabled);

    // Whether the Scene Views draw the floor grid while no test runs. On unless turned off.
    bool scene_grid() const;
    void set_scene_grid(bool shown);

    // Writes the file. True, doing nothing, when there is no file.
    bool save(std::string& error) const;

private:
    std::filesystem::path file_;
    engine_core::JsonValue root_ = engine_core::JsonValue::object();
    std::string load_error_;
};

}  // namespace ide
