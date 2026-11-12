#include "vision/eventbus/in_memory_event_bus.h"

namespace vision::eventbus {

void InMemoryEventBus::subscribe(const std::string& name, Handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_[name].push_back(std::move(handler));
}

void InMemoryEventBus::publish(const EventPtr& event) {
    std::vector<Handler> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& key : {std::string(event->name()), std::string("*")}) {
            auto it = handlers_.find(key);
            if (it != handlers_.end()) targets.insert(targets.end(), it->second.begin(), it->second.end());
        }
    }
    for (auto& h : targets) h(event);  // 在锁外调用，处理器里可以再发布事件
}

}  // namespace vision::eventbus
