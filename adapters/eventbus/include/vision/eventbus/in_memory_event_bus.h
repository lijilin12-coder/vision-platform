#pragma once

#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "vision/common/domain_event.h"

namespace vision::eventbus {

// 进程内事件总线（ADR-21）：在聚合保存成功后同步分发。
class InMemoryEventBus final : public EventPublisher {
public:
    using Handler = std::function<void(const EventPtr&)>;

    // 订阅某个事件名；name 为 "*" 时订阅全部事件。
    void subscribe(const std::string& name, Handler handler);
    void publish(const EventPtr& event) override;

private:
    std::mutex mutex_;
    std::map<std::string, std::vector<Handler>> handlers_;
};

}  // namespace vision::eventbus
