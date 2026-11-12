#pragma once

#include <memory>
#include <string>
#include <vector>

namespace vision {

// 领域事件：已经发生的、业务关心的事情。用过去时命名，不可变。
struct DomainEvent {
    virtual ~DomainEvent() = default;
    virtual const char* name() const = 0;
};

using EventPtr = std::shared_ptr<const DomainEvent>;

// 聚合根继承它来记录事件；应用层在保存聚合后取出并发布。
class EventRecorder {
public:
    std::vector<EventPtr> pullEvents() {
        std::vector<EventPtr> out;
        out.swap(events_);
        return out;
    }

protected:
    void record(EventPtr event) { events_.push_back(std::move(event)); }

private:
    std::vector<EventPtr> events_;
};

// 事件发布端口。实现见 adapters/eventbus。
class EventPublisher {
public:
    virtual ~EventPublisher() = default;
    virtual void publish(const EventPtr& event) = 0;

    void publishAll(const std::vector<EventPtr>& events) {
        for (const auto& e : events) publish(e);
    }
};

}  // namespace vision
