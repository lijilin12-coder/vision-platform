// “逻辑泄漏”：内存一直在涨，但每一块都还有人指着，任何泄漏检查工具都不会报告。
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string>
#include <vector>

struct InspectionRecord {
    int id;
    std::string verdict;
    std::vector<unsigned char> thumbnail = std::vector<unsigned char>(64 * 64, 0);  // 约 4 KB
};

// 有问题的版本：每次检测都把记录追加进去，从不清理
class UnboundedHistory {
public:
    void add(InspectionRecord record) { records_.push_back(std::move(record)); }
    std::size_t size() const { return records_.size(); }

private:
    std::vector<InspectionRecord> records_;
};

// 修复：只保留最近 N 条
class RecentHistory {
public:
    explicit RecentHistory(std::size_t capacity) : capacity_(capacity) {}
    void add(InspectionRecord record) {
        records_.push_back(std::move(record));
        if (records_.size() > capacity_) records_.pop_front();
    }
    std::size_t size() const { return records_.size(); }
    int oldestId() const { return records_.front().id; }

private:
    std::size_t capacity_;
    std::deque<InspectionRecord> records_;
};

int main() {
    UnboundedHistory unbounded;
    RecentHistory recent(100);

    const int inspections = 5000;  // 每秒 20 次检测的话，这只是 4 分钟的量
    for (int i = 0; i < inspections; ++i) {
        unbounded.add({i, "OK"});
        recent.add({i, "OK"});
    }

    std::cout << "不清理：保留了 " << unbounded.size() << " 条，约 " << unbounded.size() * 4 / 1024 << " MB\n";
    std::cout << "只保留最近 100 条：保留了 " << recent.size() << " 条，最旧的是第 " << recent.oldestId() << " 条\n";

    const bool ok = unbounded.size() == 5000 && recent.size() == 100 && recent.oldestId() == 4900;
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
