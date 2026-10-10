#pragma once

#include "AssetChoices.hpp"

#include "DataModel.hpp"

#include "jadefx/jadefx.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ide {

class AssetPickerField;

// The popover that picks one asset of a class: a search field, the assets
// that fit, and None while something is picked. Typing filters, Up and Down
// move, Enter or a click picks, and Escape or a click outside closes it. The
// Prefab editor's slots and Properties' asset references both open one.
class AssetPicker : public jadefx::VBox {
public:
    static std::shared_ptr<AssetPicker> create();

    // Makes a choice's detailed icon, size points square: a Texture's image or
    // a Material's ball, filling in once loaded. The Assets pane, which keeps
    // those, sets it; without one, rows show the class's icon.
    using IconSource = std::function<std::shared_ptr<jadefx::Node>(const AssetChoice& choice,
                                                                   const std::string& asset_class, double size)>;
    static void setIconSource(IconSource source);
    // choice's detailed icon from the source, or null without one or for an asset with none.
    static std::shared_ptr<jadefx::Node> detailedIcon(const AssetChoice& choice, const std::string& asset_class,
                                                      double size);

    // Shows the picker under anchor, listing choices, with current checked (0
    // for none). pick runs with the chosen id, or 0 for None, after it closes.
    void open(jadefx::Node& anchor, std::string asset_class, std::vector<AssetChoice> choices,
              engine_core::InstanceId current, std::function<void(engine_core::InstanceId)> pick);
    void dismiss();
    bool showing() const;

    // The search field, and the row for asset, or None's row for 0.
    jadefx::TextField* field() const;
    jadefx::Node* row(engine_core::InstanceId asset) const;

protected:
    AssetPicker();
    void layoutChildren() override;

private:
    void rebuild();
    void add_row(engine_core::InstanceId id, const std::string& name, const std::string& where,
                 std::shared_ptr<jadefx::Node> icon, const char* extra_class);
    void paint();
    void move(int delta);
    void choose(int index);

    std::weak_ptr<AssetPicker> self_;
    std::shared_ptr<AssetPickerField> field_;
    std::shared_ptr<jadefx::Label> clear_;
    std::shared_ptr<jadefx::VBox> list_;
    std::shared_ptr<jadefx::ScrollPane> scroll_;
    std::vector<std::pair<engine_core::InstanceId, std::shared_ptr<jadefx::HBox>>> rows_;
    std::vector<AssetChoice> all_;
    std::string query_;
    // The search the picker opened with, remembered from the last of its class.
    std::string opened_query_;
    std::string asset_class_;
    engine_core::InstanceId current_ = 0;
    int active_ = -1;
    std::function<void(engine_core::InstanceId)> pick_;
};

}  // namespace ide
