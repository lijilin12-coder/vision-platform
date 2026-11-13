// 类的设计：让非法状态“创建不出来”。用公差和测量值做例子（术语表 §6）。
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// ---------- 反面例子：一个什么都不保护的结构体 ----------

struct LooseTolerance {
    double nominal;  // 标称值
    double lower;    // 下偏差（应该 ≤ 0）
    double upper;    // 上偏差（应该 ≥ 0）
};
// 问题：LooseTolerance{5.0, 0.05, -0.05} 可以创建出来，上下偏差写反了，所有零件都会被判为不合格。
// 每个使用它的函数都得自己检查一遍，或者干脆不检查。

// ---------- 值对象：公差 ----------

class Tolerance {
public:
    // 工厂函数：参数不合法时返回空。构造函数是私有的，这是创建 Tolerance 的唯一途径。
    static std::optional<Tolerance> create(double nominal, double lower, double upper) {
        if (!std::isfinite(nominal) || !std::isfinite(lower) || !std::isfinite(upper)) return std::nullopt;
        if (lower > 0 || upper < 0) return std::nullopt;     // 下偏差不能为正，上偏差不能为负
        if (lower == 0 && upper == 0) return std::nullopt;   // 公差带不能为零
        return Tolerance(nominal, lower, upper);
    }

    // 常用的对称公差：5.00 ± 0.05
    static std::optional<Tolerance> symmetric(double nominal, double plusMinus) {
        return create(nominal, -plusMinus, plusMinus);
    }

    double nominal() const { return nominal_; }
    double min() const { return nominal_ + lower_; }
    double max() const { return nominal_ + upper_; }
    double band() const { return upper_ - lower_; }  // 公差带的宽度

    bool contains(double value) const { return value >= min() && value <= max(); }

    // 值对象不提供 setXxx。要“修改”，就返回一个新的对象。
    Tolerance shifted(double delta) const { return Tolerance(nominal_ + delta, lower_, upper_); }

    // 值对象按值比较：所有属性相等就是相等
    friend bool operator==(const Tolerance& a, const Tolerance& b) {
        return a.nominal_ == b.nominal_ && a.lower_ == b.lower_ && a.upper_ == b.upper_;
    }
    friend bool operator!=(const Tolerance& a, const Tolerance& b) { return !(a == b); }

private:
    Tolerance(double nominal, double lower, double upper) : nominal_(nominal), lower_(lower), upper_(upper) {}
    double nominal_;
    double lower_;
    double upper_;
};

// ---------- 值对象：测量值。行为放在数据身边 ----------

class Measurement {
public:
    Measurement(std::string item, double value, Tolerance tolerance)
        : item_(std::move(item)), value_(value), tolerance_(tolerance) {}

    const std::string& item() const { return item_; }
    double value() const { return value_; }
    bool pass() const { return tolerance_.contains(value_); }

    // 超差了多少：合格时为 0，偏大为正，偏小为负
    double deviation() const {
        if (value_ > tolerance_.max()) return value_ - tolerance_.max();
        if (value_ < tolerance_.min()) return value_ - tolerance_.min();
        return 0;
    }

    // 给操作员看的判定依据
    std::string reason() const {
        if (pass()) return item_ + " 合格";
        const double d = deviation();
        return item_ + " 超差 " + (d > 0 ? "+" : "") + std::to_string(d) + " mm";
    }

private:
    std::string item_;
    double value_;
    Tolerance tolerance_;
};

// ---------- 实体：有身份，靠 ID 区分 ----------

class InspectionRecord {
public:
    explicit InspectionRecord(std::string id) : id_(std::move(id)) {}
    const std::string& id() const { return id_; }

    void add(Measurement m) { measurements_.push_back(std::move(m)); }
    bool pass() const {
        for (const auto& m : measurements_) {
            if (!m.pass()) return false;
        }
        return true;
    }

    // 实体按身份比较：ID 相同就是同一条记录，哪怕内容不同
    friend bool operator==(const InspectionRecord& a, const InspectionRecord& b) { return a.id_ == b.id_; }

private:
    std::string id_;
    std::vector<Measurement> measurements_;
};

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) {
        std::cout << "✗ " << what << "\n";
        ++failures;
    }
}

int main() {
    // 非法的公差创建不出来
    check(!Tolerance::create(5.0, 0.05, -0.05).has_value(), "上下偏差写反：创建失败");
    check(!Tolerance::symmetric(5.0, 0.0).has_value(), "公差带为零：创建失败");
    check(!Tolerance::symmetric(5.0, -0.05).has_value(), "± 值为负：创建失败");

    const Tolerance tol = *Tolerance::symmetric(5.0, 0.05);
    check(tol.contains(5.04) && !tol.contains(5.08), "是否在公差内");
    check(std::fabs(tol.band() - 0.10) < 1e-12, "公差带宽度");

    // 值对象：按值比较，“修改”得到新对象，原对象不变
    check(tol == *Tolerance::create(5.0, -0.05, 0.05), "属性相同的两个公差相等");
    const Tolerance moved = tol.shifted(1.0);
    check(moved.nominal() == 6.0 && tol.nominal() == 5.0, "shifted 返回新对象，原对象不变");

    // 行为跟着数据走
    const Measurement ok("孔径 A", 5.03, tol);
    const Measurement ng("孔径 B", 5.08, tol);
    check(ok.pass() && !ng.pass(), "合格与超差");
    check(std::fabs(ng.deviation() - 0.03) < 1e-9, "超差量");
    std::cout << ok.reason() << "\n" << ng.reason() << "\n";

    // 实体：按身份比较
    InspectionRecord r1("r-0001");
    InspectionRecord r1Again("r-0001");
    InspectionRecord r2("r-0002");
    r1.add(ok);
    r1Again.add(ng);
    check(r1 == r1Again, "ID 相同就是同一条记录");
    check(!(r1 == r2), "ID 不同就是不同的记录");
    check(r1.pass() && !r1Again.pass(), "记录的判定");

    std::cout << (failures == 0 ? "全部符合预期\n" : "有不符合预期的结果\n");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
