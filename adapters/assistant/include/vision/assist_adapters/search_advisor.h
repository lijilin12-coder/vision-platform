#pragma once

#include <deque>
#include <string>
#include <vector>

#include "vision/assistant/domain/ports.h"

namespace vision::assist_adapters {

// 搜索顾问：不用大模型，也不看图像，只靠“试几个值，看评估有没有变好”来调参。
//
// 做法是“逐个可调项的网格搜索”（坐标搜索）：
//   1. 轮流处理每个可调项：在它的范围里均匀取几个值，一个一个试。
//      变好了助手会保留，没变好助手会回退——顾问不用自己记“目前最好的是多少”，每轮都能从助手给的内容里看到。
//   2. 所有可调项都试过一遍算“一趟”。下一趟在目前最好的值附近取更密的点。
//   3. 一整趟下来没有任何改进，或者趟数用完了，就建议结束。
//
// 范围跨度很大的参数（最大值是最小值的 100 倍以上，例如面积 1～2000）按对数均匀取点：
// 1、3、9、26……比 1、286、572……有用得多。
//
// 它有两个用处：证明“自动调参”不靠大模型也能走通；给大模型顾问一个比较的基准。
class SearchAdvisor final : public assistant::RecipeAdvisor {
public:
    explicit SearchAdvisor(int pointsPerTunable = 8, int maxPasses = 3);

    std::string name() const override;
    Result<assistant::Proposal> propose(const assistant::AdvisorView& view) override;

private:
    struct Range {
        double lo = 0, hi = 0;  // 这一趟的搜索范围（对数刻度时存的是对数）
        bool logScale = false;
    };
    void startTunable(const assistant::AdvisorView& view);

    int points_;
    int maxPasses_;
    int pass_ = 0;
    std::size_t tunable_ = 0;
    bool started_ = false;
    bool improvedThisPass_ = false;
    std::size_t historySeen_ = 0;
    std::vector<Range> ranges_;
    std::deque<double> pending_;  // 当前可调项还没有试的取值
};

}  // namespace vision::assist_adapters
