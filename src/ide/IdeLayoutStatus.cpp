// IdeLayout: the status bar's chips other than Problems and Conflicts.

#include "IdeLayout.hpp"

#include "IdeLayoutInternal.hpp"
#include "McpServer.hpp"
#include "runner/ProfilerOverlay.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>

namespace ide {

namespace {

// Sets a label's text only when it changed, so an unchanged chip does not lay out again.
void SetText(jadefx::Label* label, const std::string& text) {
    if (label != nullptr && label->getText() != text) {
        label->setText(text);
    }
}

void SetTip(const std::shared_ptr<jadefx::Tooltip>& tip, const std::string& text) {
    if (tip && tip->getText() != text) {
        tip->setText(text);
    }
}

// "just now", "40 s ago", "3 min ago", "2 h ago".
std::string Ago(std::chrono::steady_clock::duration elapsed) {
    const long long seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    if (seconds < 5) {
        return "just now";
    }
    if (seconds < 60) {
        return std::to_string(seconds) + " s ago";
    }
    if (seconds < 3600) {
        return std::to_string(seconds / 60) + " min ago";
    }
    return std::to_string(seconds / 3600) + " h ago";
}

// The code editor that has the keyboard: the focused node or one it is inside.
const jadefx::CodeArea* FocusedCodeArea(jadefx::Scene* scene) {
    if (scene == nullptr) {
        return nullptr;
    }
    for (const jadefx::Node* node = scene->focusedNode(); node != nullptr; node = node->getParent()) {
        if (const auto* area = dynamic_cast<const jadefx::CodeArea*>(node)) {
            return area;
        }
    }
    return nullptr;
}

}  // namespace

void IdeLayout::show_play_state() {
    if (play_chip_ == nullptr) {
        return;
    }
    switch (play_) {
    case PlayState::Stopped:
        play_chip_->set_icon(0, "Editing.png");
        SetText(play_state_text_, "Editing");
        break;
    case PlayState::Running:
        play_chip_->set_icon(0, "Play.png");
        SetText(play_state_text_, "Playing");
        break;
    case PlayState::Paused:
        play_chip_->set_icon(0, "Pause.png");
        SetText(play_state_text_, "Paused");
        break;
    }
}

void IdeLayout::show_save_state(bool unsaved) {
    if (save_chip_ == nullptr || static_cast<int>(unsaved) == shown_unsaved_) {
        return;
    }
    shown_unsaved_ = unsaved;
    save_chip_->set_icon(0, unsaved ? "Unsaved.png" : "Save.png");
    SetText(save_state_text_, unsaved ? "Unsaved" : "Saved");
    if (unsaved) {
        SetTip(save_tip_, "Unsaved changes. Click to save");
    } else if (project_) {
        SetTip(save_tip_, "Everything is saved in " + project_->name());
    } else {
        SetTip(save_tip_, "No changes to save");
    }
}

void IdeLayout::show_cursor_position() {
    if (cursor_chip_ == nullptr) {
        return;
    }
    const jadefx::CodeArea* area = FocusedCodeArea(scene_);
    if (area == nullptr) {
        if (cursor_chip_->isVisible()) {
            cursor_chip_->setVisible(false);
        }
        return;
    }
    std::string text = "Ln " + std::to_string(area->currentParagraph() + 1) + ", Col " +
                       std::to_string(area->caretColumn() + 1);
    const jadefx::IndexRange selected = area->selection();
    if (selected.end > selected.start) {
        text += " (" + std::to_string(selected.end - selected.start) + " selected)";
    }
    SetText(cursor_text_, text);
    if (!cursor_chip_->isVisible()) {
        cursor_chip_->setVisible(true);
    }
}

void IdeLayout::show_frame_time() {
    if (frame_view_ == nullptr || frame_text_ == nullptr) {
        return;
    }
    // Four times a second at most: a number that changes every frame cannot be read.
    const double now = scene_ != nullptr ? scene_->timeSeconds() : 0;
    if (frame_shown_at_ >= 0 && now - frame_shown_at_ < 0.25) {
        return;
    }
    frame_shown_at_ = now;
    const char* click = runner::ProfilerUi::get().shown() ? "Click to hide the profiler" : "Click to show the profiler";
    if (!frame_view_->frameTimeCurrent()) {
        SetText(frame_text_, "-- ms");
        SetTip(frame_tip_, std::string("The Scene View is not drawing. ") + click);
        return;
    }
    char text[32];
    std::snprintf(text, sizeof text, "%.1f ms", frame_view_->frameMilliseconds());
    SetText(frame_text_, text);
    SetTip(frame_tip_, std::to_string(frame_view_->framesPerSecond()) +
                           " frames a second in the Scene View. " + click);
}

void IdeLayout::show_zoom() {
    const double zoom = jadefx::Stage::getZoom();
    if (zoom_text_ == nullptr || zoom == shown_zoom_) {
        return;
    }
    shown_zoom_ = zoom;
    SetText(zoom_text_, std::to_string(static_cast<int>(std::lround(zoom * 100.0))) + "%");
}

void IdeLayout::show_ai_client() {
    if (ai_chip_ == nullptr) {
        return;
    }
    SetStyleClass(*ai_chip_, "off", !mcp_);
    if (!mcp_) {
        SetText(ai_text_, "AI off");
        SetTip(ai_tip_, "MCP server: " + mcp_status() + ". Click to set up AI clients");
        return;
    }
    const McpActivity activity = mcp_->activity();
    const auto now = std::chrono::steady_clock::now();
    const std::string client = activity.client.empty() ? std::string("AI") : activity.client;
    // A tool call shows for a moment, so a change made by a client is seen as it lands.
    constexpr auto kCallShown = std::chrono::seconds(3);
    if (activity.calls > 0 && now - activity.last_call < kCallShown) {
        SetText(ai_text_, client + ": " + activity.last_tool);
    } else if (!activity.client.empty()) {
        SetText(ai_text_, activity.client);
    } else {
        SetText(ai_text_, "AI ready");
    }
    std::string tip = "MCP server: " + mcp_status();
    if (activity.last_request == std::chrono::steady_clock::time_point{}) {
        tip += "\nNo client has connected yet";
    } else {
        tip += "\n" + client + ", last request " + Ago(now - activity.last_request);
        if (activity.calls > 0) {
            tip += "\n" + Counted(activity.calls, "tool call", "tool calls") + ", latest " + activity.last_tool + " " +
                   Ago(now - activity.last_call);
        }
    }
    SetTip(ai_tip_, tip);
}

}  // namespace ide
