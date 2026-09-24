#include "IdeConsole.hpp"

namespace ide {

IdeConsole::IdeConsole() : IdePane("Console", true) {
    auto log = jadefx::make<jadefx::StyleClassedTextArea>();
    log->setEditable(false);
    log->setWrapText(true);
    Fill(*log);

    auto command = jadefx::make<jadefx::TextField>();
    command->setPromptText("Lua Command Line");
    command->setStyle("width: 100%;");

    // The log takes the page left after the command line, so a splitter drag
    // grows and shrinks the log instead of leaving a gap or pushing the field out.
    auto column = jadefx::make<jadefx::BorderPane>();
    Fill(*column);
    column->setCenter(log);
    column->setBottom(command);
    getChildren().add(column);
}

}  // namespace ide
