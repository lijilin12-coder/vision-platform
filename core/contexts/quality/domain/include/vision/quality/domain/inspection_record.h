#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "vision/common/result.h"
#include "vision/kernel/result_items.h"

namespace vision::quality {

// 判定依据在记录里的样子
struct RecordedReason {
    std::string nodeId;
    bool pass = true;
    std::string text;
};

// 检测记录：一次产线检测留下的、事后可以查的东西（术语表：检测记录；NFR-06 可追溯）。
//
// 它回答三个问题：
//   什么时候、哪个工位、哪一幅图像      time、stationId、frameName、imageId
//   用的是什么                          releaseId、recipeVersionId —— 由此可以追到方案的每个参数、用的模型
//   结果是什么                          verdict、reasons、result（缺陷、测量值、位姿）
//
// 不变量：
//   Q1 记录一旦写下就不再改（只追加）。这个类没有任何修改自己的方法
//   Q2 必须说得清是谁、什么时候、用什么检测的：工位、时间、上线包、方案版本都不能为空
//   Q3 判定只能是 OK / NG / ERROR；判 NG 的记录至少有一条不合格的判定依据（和检测执行的 I4 一致）
class InspectionRecord {
public:
    struct Fields {
        std::string id;  // 记录的编号：按时间排序，后写的更大（见 makeRecordId）
        std::string stationId;
        std::int64_t timeMs = 0;  // 检测完成的时刻：1970-01-01 UTC 以来的毫秒数
        std::uint64_t sequence = 0;  // 工位的第几次检测（工位重启后可能重新计数，所以不能当编号用）
        std::string releaseId, recipeVersionId;
        std::string frameName;
        std::string imageId;  // 图像的编号（交给图像存档去找）；没有保存图像时为空。图像可能后来过期被清掉，这里不改（Q1）
        std::string verdict;
        std::vector<RecordedReason> reasons;
        ResultItems result;
        double totalMs = 0;
        bool swapped = false;  // 这次检测开始前发生了热切换
    };

    static Result<InspectionRecord> create(Fields fields);  // 检查 Q2、Q3
    const Fields& fields() const { return f_; }
    const Fields* operator->() const { return &f_; }

private:
    explicit InspectionRecord(Fields f) : f_(std::move(f)) {}
    Fields f_;
};

// 记录的编号：13 位的毫秒时间 + 6 位的序号，例如 "1795512345678-000042"。
// 按字符串比较大小就是按时间先后；同一毫秒里的几条靠序号分先后。
std::string makeRecordId(std::int64_t timeMs, std::uint64_t counter);

// 查询条件
struct RecordFilter {
    std::string verdict;   // 只要这种判定；空表示都要
    std::string beforeId;  // 只要比这个编号更早的（翻页用）；空表示从最新的开始
    int limit = 50;
};

// 汇总（QUA-02 的一部分）
struct Summary {
    long total = 0, ok = 0, ng = 0, error = 0;
    std::map<std::string, long> defectsByCategory;  // 每种缺陷出现了多少个
    // 良率：合格的 ÷（合格的 + 不合格的）。ERROR 不算进去——它说明的是系统出了问题，不是产品好坏。没有可算的时候返回空
    std::optional<double> yield() const;
    void add(const class InspectionRecord& record);  // 把一条记录算进来
};

Summary summarize(const std::vector<InspectionRecord>& records);

// 仓储：只追加。query 返回的记录从新到旧
class InspectionRecordRepository {
public:
    virtual ~InspectionRecordRepository() = default;
    virtual Status append(const InspectionRecord& record) = 0;
    virtual std::vector<InspectionRecord> query(const std::string& stationId, const RecordFilter& filter) = 0;
    virtual std::optional<InspectionRecord> find(const std::string& stationId, const std::string& recordId) = 0;
    virtual std::vector<InspectionRecord> all(const std::string& stationId) = 0;  // 汇总用
};

}  // namespace vision::quality
