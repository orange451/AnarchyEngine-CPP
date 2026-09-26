#include "DataModelLock.hpp"

#include "DataModel.hpp"

namespace engine_core {

DataModelLock::DataModelLock(DataModel& game, Kind) : game_(&game) {
    owns_ = game_->lock_write_blocking();
}

DataModelLock::DataModelLock(DataModel& game, Kind, std::chrono::milliseconds timeout) : game_(&game) {
    owns_ = game_->lock_write_for(timeout);
}

DataModelLock::~DataModelLock() {
    if (owns_ && game_ != nullptr) {
        game_->unlock_write();
    }
}

}  // namespace engine_core
