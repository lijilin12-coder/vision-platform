#include "vision/assist_adapters/search_advisor.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace vision::assist_adapters {

using assistant::AdvisorView;
using assistant::Proposal;
using assistant::ProposedChange;

namespace {

double roundTo(double v) { return std::round(v * 100.0) / 100.0; }  // 保留两位小数：记录里好看，也避免 181.99999

std::string text(double v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

}  // namespace

SearchAdvisor::SearchAdvisor(int pointsPerTunable, int maxPasses)
    : points_(std::max(3, pointsPerTunable)), maxPasses_(std::max(1, maxPasses)) {}

std::string SearchAdvisor::name() const {
    return "search:grid" + std::to_string(points_) + "x" + std::to_string(maxPasses_);
}

// 为当前可调项准备这一趟要试的取值
void SearchAdvisor::startTunable(const AdvisorView& view) {
    const auto& state = view.tunables[tunable_];
    const auto& t = state.tunable;
    Range& r = ranges_[tunable_];
    const auto toScale = [&r](double v) { return r.logScale ? std::log(v) : v; };
    const auto fromScale = [&r](double v) { return r.logScale ? std::exp(v) : v; };

    if (pass_ > 0) {
        // 第二趟起：只在目前最好的值左右各一格的范围里取点
        const double step = (r.hi - r.lo) / (points_ - 1);
        const double best = toScale(state.best);
        r.lo = std::max(toScale(t.min), best - step);
        r.hi = std::min(toScale(t.max), best + step);
    }
    pending_.clear();
    for (int i = 0; i < points_; ++i) {
        double v = roundTo(fromScale(r.lo + (r.hi - r.lo) * i / (points_ - 1)));
        v = std::min(t.max, std::max(t.min, v));
        const bool isBest = std::abs(v - state.best) < 1e-9;  // 目前最好的值已经评估过了，不用再试
        if (!isBest && std::find(pending_.begin(), pending_.end(), v) == pending_.end()) pending_.push_back(v);
    }
}

Result<Proposal> SearchAdvisor::propose(const AdvisorView& view) {
    if (view.tunables.empty()) return Result<Proposal>::failure("advisor.no_tunables", "没有可调项");

    if (!started_) {
        started_ = true;
        for (const auto& s : view.tunables) {
            Range r;
            r.logScale = s.tunable.min > 0 && s.tunable.max / s.tunable.min >= 100;
            r.lo = r.logScale ? std::log(s.tunable.min) : s.tunable.min;
            r.hi = r.logScale ? std::log(s.tunable.max) : s.tunable.max;
            ranges_.push_back(r);
        }
        startTunable(view);
    }
    // 上一轮的提议有没有被保留？从助手给的历史里看
    for (; historySeen_ < view.history.size(); ++historySeen_) {
        if (view.history[historySeen_].decision == "kept") improvedThisPass_ = true;
    }

    while (pending_.empty()) {
        if (++tunable_ >= view.tunables.size()) {  // 一趟结束
            tunable_ = 0;
            ++pass_;
            if (!improvedThisPass_ || pass_ >= maxPasses_) {
                Proposal done;
                done.finish = true;
                done.rationale = improvedThisPass_ ? "搜索的趟数已经用完" : "一整趟搜索下来没有任何改进";
                return done;
            }
            improvedThisPass_ = false;
        }
        startTunable(view);
    }

    const auto& t = view.tunables[tunable_].tunable;
    const double value = pending_.front();
    pending_.pop_front();
    Proposal p;
    p.changes.push_back({ProposedChange::Kind::SetParam, t.nodeKey, t.param, value});
    p.rationale = "搜索第 " + std::to_string(pass_ + 1) + " 趟：试 " + t.name() + " = " + text(value);
    return p;
}

}  // namespace vision::assist_adapters
