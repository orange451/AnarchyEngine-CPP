#include "DataModelLock.hpp"

#include "DataModel.hpp"

namespace engine_core {

DataModelLock::DataModelLock(DataModel& model, Kind) : model_(&model) {
    owns_ = model_->lock_write_blocking();
}

DataModelLock::DataModelLock(DataModel& model, Kind, std::chrono::milliseconds timeout) : model_(&model) {
    owns_ = model_->lock_write_for(timeout);
}

DataModelLock::~DataModelLock() {
    if (owns_ && model_ != nullptr) {
        model_->unlock_write();
    }
}

}  // namespace engine_core
