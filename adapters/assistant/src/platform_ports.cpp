#include "vision/assist_adapters/platform_ports.h"

#include <functional>
#include <opencv2/core.hpp>

#include "vision/imaging/cv_bridge.h"

namespace vision::assist_adapters {

Result<Image> perturb(const Image& image, const assistant::Perturbation& p, unsigned seed) {
    const cv::Mat src = imaging::view(image);
    cv::Mat out;
    if (p.kind == "brightness") {
        if (p.level <= 0) return Result<Image>::failure("assistant.bad_perturbation", "亮度倍数必须大于 0");
        src.convertTo(out, -1, p.level, 0);  // 每个像素乘以倍数，超过 255 的截断
    } else if (p.kind == "noise") {
        if (p.level < 0) return Result<Image>::failure("assistant.bad_perturbation", "噪声的标准差不能是负数");
        cv::Mat noise(src.size(), CV_16SC(src.channels()));
        cv::RNG rng(seed);  // 固定种子：同一幅图每次得到同样的噪声，评估才能重现
        rng.fill(noise, cv::RNG::NORMAL, 0.0, p.level);
        cv::Mat wide;
        src.convertTo(wide, CV_16S);
        wide += noise;
        wide.convertTo(out, CV_8U);
    } else {
        return Result<Image>::failure("assistant.bad_perturbation", "不认识的扰动：" + p.kind);
    }
    return imaging::fromMat(out);
}

Result<Image> EngineTrialRunner::imageFor(const std::string& imageRef) {
    if (auto it = images_.find(imageRef); it != images_.end()) return it->second;
    auto image = imaging::loadImage(imageRef);
    if (!image) return image.error();
    images_[imageRef] = image.value();
    return image;
}

Result<assistant::TrialResult> EngineTrialRunner::run(const std::string& recipeId, const std::string& imageRef,
                                                      const std::optional<assistant::Perturbation>& perturbation) {
    auto doc = authoring_.getDraftDocument(recipeId);
    if (!doc) return doc.error();
    auto image = imageFor(imageRef);
    if (!image) return image.error();
    if (perturbation) {
        image = perturb(image.value(), *perturbation, static_cast<unsigned>(std::hash<std::string>{}(imageRef)));
        if (!image) return image.error();
    }
    // 调试运行：每次都按草稿当前的内容重新编译，不会用到缓存里的旧参数
    auto run = execution_.debugRun(doc.value(), image.value());
    if (!run) return run.error();

    assistant::TrialResult t;
    t.verdict = run.value().report.verdict;
    t.durationMs = run.value().report.totalMs;
    for (const auto& r : run.value().report.reasons) t.reasons.push_back(r.text);
    for (const auto& n : run.value().report.nodes) {
        if (n.status == "failed") t.reasons.push_back(n.operatorId + " 运行失败：" + n.message);
        for (const auto& [port, text] : n.outputs) t.outputs[n.nodeId + "." + port] = text;
    }
    return t;
}

Result<std::vector<assistant::CaseInfo>> ReleaseGoldenCases::cases(const std::string& goldenSetId) {
    auto set = golden_.getGoldenSet(goldenSetId);
    if (!set) return set.error();
    std::vector<assistant::CaseInfo> out;
    for (const auto& c : set.value().cases) out.push_back({c.id, c.imageRef, c.truthNg});
    return out;
}

Result<assistant::EvaluationData> ReleaseGoldenCases::evaluate(const std::string& goldenSetId,
                                                               const std::vector<assistant::CaseOutcome>& outcomes) {
    std::vector<release::CaseResultView> results;
    for (const auto& o : outcomes) results.push_back({o.caseId, o.verdict, o.durationMs});
    auto e = golden_.evaluate(goldenSetId, results);
    if (!e) return e.error();
    assistant::EvaluationData d;
    d.truthOk = e.value().truthOk;
    d.truthNg = e.value().truthNg;
    d.misses = e.value().misses;
    d.falseAlarms = e.value().falseAlarms;
    d.errors = e.value().errors;
    d.meanMs = e.value().meanMs;
    d.maxMs = e.value().maxMs;
    d.meetsCriteria = e.value().meetsCriteria;
    d.wrongCaseIds = e.value().wrongCaseIds;
    return d;
}

bool ReleaseGoldenCases::betterThan(const assistant::EvaluationData& a, const assistant::EvaluationData& b) {
    const auto view = [](const assistant::EvaluationData& d) {
        release::EvaluationView v;
        v.errors = d.errors;
        v.misses = d.misses;
        v.falseAlarms = d.falseAlarms;
        return v;
    };
    return release::GoldenSetService::betterThan(view(a), view(b));
}

}  // namespace vision::assist_adapters
