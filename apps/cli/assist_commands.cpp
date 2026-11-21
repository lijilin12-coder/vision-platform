// vision-cli 的 golden / workflow / assist 三组命令（POC-11：方案助手的最小闭环）。
// 这三组命令属于方案构建平台；产线运行平台的程序里不会有它们。

#include <cstdlib>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>
#include <opencv2/imgproc.hpp>
#include <sstream>

#include "cli_app.h"
#include "vision/assist_adapters/scripted_advisor.h"
#include "vision/assist_adapters/search_advisor.h"
#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"
#include "vision/llm/llm_advisor.h"

namespace fs = std::filesystem;
using nlohmann::json;

namespace vision::cli {

namespace {

Result<std::string> readText(const std::string& file) {
    std::ifstream in(pathFromUtf8(file), std::ios::binary);
    if (!in) return Result<std::string>::failure("cli.read", "无法读取 " + file);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// ---------- 示例基准图集 ----------
//
// 画面：浅灰背景（约 150）上有一块深色零件（约 75）；缺陷是零件上的白色亮斑（约 235）。
// 一部分合格的图上有 2×2 像素的灰尘亮点：它们不算缺陷，方案必须能忽略。
// lighting 是整体亮度的倍数：0.6 模拟“换了一个更暗的光源”。
// glare 为真时零件左侧有一条大面积的反光（约 3000 像素，和亮斑一样亮）：它不算缺陷。
cv::Mat drawSample(int spots, bool dust, double lighting, unsigned seed, bool glare = false) {
    cv::RNG rng(seed);  // OpenCV 自己的随机数发生器：在任何平台上结果都一样
    cv::Mat m(480, 640, CV_8UC3, cv::Scalar(150, 150, 150));
    cv::rectangle(m, cv::Rect(120, 90, 400, 300), cv::Scalar(70, 75, 80), cv::FILLED);
    for (int i = 0; i < spots; ++i) {
        const cv::Point center(rng.uniform(glare ? 180 : 150, 490), rng.uniform(120, 360));
        cv::circle(m, center, rng.uniform(6, 13), cv::Scalar(235, 235, 235), cv::FILLED);
    }
    if (glare) cv::rectangle(m, cv::Rect(132, 150, 20, 150), cv::Scalar(235, 235, 235), cv::FILLED);
    if (dust) {
        for (int i = 0; i < 3; ++i) {
            cv::rectangle(m, cv::Rect(rng.uniform(140, 500), rng.uniform(110, 370), 2, 2), cv::Scalar(240, 240, 240), cv::FILLED);
        }
    }
    m.convertTo(m, -1, lighting, 0);
    cv::Mat noise(m.size(), CV_16SC3);
    rng.fill(noise, cv::RNG::NORMAL, 0.0, 4.0);
    cv::Mat wide;
    m.convertTo(wide, CV_16S);
    wide += noise;
    wide.convertTo(m, CV_8U);
    return m;
}

int cmdGoldenSample(App& app, const std::string& dir, const std::string& blindLighting, const std::string& scenario) {
    if (scenario != "basic" && scenario != "glare") {
        std::cerr << "--scenario 只能是 basic 或 glare\n";
        return 1;
    }
    const bool withGlare = scenario == "glare";
    if (blindLighting != "dim" && blindLighting != "same") {
        std::cerr << "--blind-lighting 只能是 dim 或 same\n";
        return 1;
    }
    const fs::path outDir = fs::absolute(pathFromUtf8(dir));
    fs::create_directories(outDir);

    release::GoldenSetDefinition def;
    def.name = blindLighting == "dim" ? "亮斑示例（盲测集的光照更暗）" : "亮斑示例";
    def.criteria = {0, 0.0, 0};  // 不允许漏检、误报、出错
    std::vector<std::string> blind;
    unsigned seed = 100;
    int index = 0;
    const auto add = [&](const std::string& id, int spots, bool dust, double lighting) -> Status {
        // 反光场景：大约三分之二的图上有反光，合格和不合格的图上都有
        const bool glare = withGlare && (index++ % 3 != 2);
        auto image = imaging::fromMat(drawSample(spots, dust, lighting, ++seed, glare));
        if (!image) return image.error();
        const std::string file = pathToUtf8(outDir / (id + ".png"));
        if (auto st = imaging::saveImage(file, image.value()); !st) return st;
        def.cases.push_back({id, file, spots > 0});
        return Status::success();
    };
    char name[32];
    for (int i = 1; i <= 8; ++i) {  // 调参用：正常光照
        std::snprintf(name, sizeof name, "ok_%02d", i);
        if (auto st = add(name, 0, i % 2 == 0, 1.0); !st) return fail(st.error());
        std::snprintf(name, sizeof name, "ng_%02d", i);
        if (auto st = add(name, 1 + i % 3, i % 4 == 0, 1.0); !st) return fail(st.error());
    }
    const double lighting = blindLighting == "dim" ? 0.6 : 1.0;
    for (int i = 1; i <= 4; ++i) {  // 盲测用
        std::snprintf(name, sizeof name, "blind_ok_%02d", i);
        if (auto st = add(name, 0, i % 2 == 0, lighting); !st) return fail(st.error());
        blind.emplace_back(name);
        std::snprintf(name, sizeof name, "blind_ng_%02d", i);
        if (auto st = add(name, 1 + i % 3, false, lighting); !st) return fail(st.error());
        blind.emplace_back(name);
    }

    auto id = app.goldenSets.createGoldenSet(def);
    if (!id) return fail(id.error());

    // 顺手写一份任务书，方便直接试
    const json brief = {{"goal", withGlare ? "检出零件表面的亮斑，一个都不允许；灰尘和大面积的反光不算缺陷"
                                           : "检出零件表面的亮斑，一个都不允许；灰尘不算缺陷"},
                        {"goldenSet", id.value()},
                        {"workflow", withGlare ? "bright-spot-glare" : "bright-spot"},
                        {"blindCases", blind},
                        {"dataSharing", "no-images"},
                        {"maxRounds", 30},
                        {"hardeningRounds", 8},
                        {"maxSeconds", 300}};
    const fs::path briefFile = outDir / "brief.json";
    std::ofstream(briefFile, std::ios::binary) << brief.dump(2) << "\n";

    std::cout << "基准图集 " << id.value() << "：" << def.cases.size() << " 个用例（其中 " << blind.size() << " 个留作盲测）\n";
    std::cout << "任务书 " << pathToUtf8(briefFile) << "\n";
    return 0;
}

int cmdGoldenShow(App& app, const std::string& id) {
    auto set = app.goldenSets.getGoldenSet(id);
    if (!set) return fail(set.error());
    const auto& s = set.value();
    std::cout << s.id << "  " << s.name << "\n";
    std::cout << "验收标准：漏检 ≤ " << s.criteria.maxMisses << "，误报率 ≤ " << s.criteria.maxFalseAlarmRate << "，出错 ≤ "
              << s.criteria.maxErrors << "\n";
    for (const auto& c : s.cases) std::cout << "  " << (c.truthNg ? "NG" : "OK") << "  " << c.id << "  " << c.imageRef << "\n";
    return 0;
}

// ---------- 助手 ----------

Result<assistant::AssistRequest> parseBrief(const std::string& text) try {
    const json j = json::parse(text);
    assistant::AssistRequest r;
    r.goal = j.value("goal", "");
    r.goldenSetId = j.at("goldenSet").get<std::string>();
    r.workflowId = j.at("workflow").get<std::string>();
    r.blindCaseIds = j.value("blindCases", std::vector<std::string>{});
    r.blindFraction = j.value("blindFraction", 0.3);
    r.seed = j.value("seed", 1u);
    r.dataSharing = j.value("dataSharing", "no-images");
    r.maxRounds = j.value("maxRounds", 20);
    r.maxSeconds = j.value("maxSeconds", 300.0);
    r.hardeningRounds = j.value("hardeningRounds", 8);
    return r;
} catch (const std::exception& e) {
    return Result<assistant::AssistRequest>::failure("cli.bad_brief", std::string("任务书格式不对：") + e.what());
}

void printSummary(const assistant::SessionView& v) {
    std::cout << "会话 " << v.sessionId << "\n";
    std::cout << "候选方案 " << v.candidateRecipeId << "\n";
    std::cout << "状态 " << v.status << "，结束原因 " << v.endReason << "\n";
}

int cmdAssistRun(App& app, const std::vector<std::string>& args) {
    std::string briefFile, scriptFile, reportFile, advisorKind, llmFile;
    bool withStatistics = true;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--script" && i + 1 < args.size()) scriptFile = args[++i];
        else if (args[i] == "--advisor" && i + 1 < args.size()) advisorKind = args[++i];
        else if (args[i] == "--llm" && i + 1 < args.size()) llmFile = args[++i];
        else if (args[i] == "--no-statistics") withStatistics = false;
        else if (args[i] == "--report" && i + 1 < args.size()) reportFile = args[++i];
        else briefFile = args[i];
    }
    if (advisorKind.empty() && !scriptFile.empty()) advisorKind = "script";
    if (advisorKind.empty() && !llmFile.empty()) advisorKind = "llm";
    const bool known = advisorKind == "search" || advisorKind == "script" || advisorKind == "llm";
    if (briefFile.empty() || !known || (advisorKind == "script" && scriptFile.empty()) || (advisorKind == "llm" && llmFile.empty())) {
        std::cerr << "用法：assist run <任务书.json> (--advisor search | --script <顾问脚本.json> | --llm <大模型配置.json>)"
                     " [--no-statistics] [--report <报告.md>]\n";
        return 1;
    }
    app.trialRunner.setWithStatistics(withStatistics);
    auto briefText = readText(briefFile);
    if (!briefText) return fail(briefText.error());
    auto request = parseBrief(briefText.value());
    if (!request) return fail(request.error());

    // 选一个顾问。它们都是 RecipeAdvisor 的实现，助手的其余部分不知道也不关心用的是哪一个；
    // 大模型顾问（POC-12）会是这里的第三个分支。
    std::unique_ptr<assistant::RecipeAdvisor> advisor;
    llm::CurlCommandHttpClient http;
    llm::LlmAdvisor* llmAdvisor = nullptr;
    if (advisorKind == "llm") {
        auto configText = readText(llmFile);
        if (!configText) return fail(configText.error());
        auto config = llm::parseLlmConfig(configText.value());
        if (!config) return fail(config.error());
        // 密钥只从环境变量读：不写进配置文件，不出现在命令行、日志和会话记录里
        const char* key = std::getenv(config.value().apiKeyEnv.c_str());
        auto protocol = llm::makeProtocol(config.value(), key == nullptr ? "" : key);
        if (!protocol) return fail(protocol.error());
        auto created = std::make_unique<llm::LlmAdvisor>(config.value(), std::move(protocol).value(), http, llm::fileImageLoader());
        llmAdvisor = created.get();
        advisor = std::move(created);
    } else if (advisorKind == "search") {
        advisor = std::make_unique<assist_adapters::SearchAdvisor>();
    } else {
        auto scriptText = readText(scriptFile);
        if (!scriptText) return fail(scriptText.error());
        auto scripted = assist_adapters::ScriptedAdvisor::fromJson(scriptText.value());
        if (!scripted) return fail(scripted.error());
        advisor = std::make_unique<assist_adapters::ScriptedAdvisor>(std::move(scripted).value());
    }

    assistant::AssistService service(app.workflows, app.assistSessions, *advisor, app.candidateEditor, app.trialRunner,
                                     app.goldenCases, app.bus);
    auto view = service.run(request.value());
    if (!view) return fail(view.error());

    std::cout << view.value().reportMarkdown << "\n";
    printSummary(view.value());
    std::error_code ec;
    const auto recordFile = app.assistSessions.fileOf(view.value().sessionId);
    std::cout << "会话记录 " << pathToUtf8(recordFile) << "（" << fs::file_size(recordFile, ec) << " 字节）\n";
    if (!reportFile.empty()) {
        std::ofstream(pathFromUtf8(reportFile), std::ios::binary) << view.value().reportMarkdown;
        std::cout << "报告 " << reportFile << "\n";
    }
    if (llmAdvisor != nullptr) {
        // 和大模型的全部往来另存一个文件：事后可以核对到底发出去了什么（没有密钥：密钥在请求头里，不在这里）
        json calls = json::array();
        double seconds = 0;
        int inputTokens = 0, outputTokens = 0;
        for (const auto& c : llmAdvisor->calls()) {
            calls.push_back({{"request", json::parse(c.requestBody, nullptr, false)}, {"reply", c.replyText},
                             {"httpStatus", c.httpStatus}, {"attempts", c.attempts}, {"seconds", c.seconds},
                             {"inputTokens", c.inputTokens}, {"outputTokens", c.outputTokens}, {"error", c.error}});
            seconds += c.seconds;
            inputTokens += c.inputTokens;
            outputTokens += c.outputTokens;
        }
        auto transcript = recordFile;
        transcript.replace_extension(".llm.json");
        std::ofstream(transcript, std::ios::binary) << calls.dump(2) << "\n";
        std::cout << "大模型调用 " << calls.size() << " 次，共 " << seconds << " 秒，输入 " << inputTokens << " / 输出 "
                  << outputTokens << " 个 token；往来记录 " << pathToUtf8(transcript) << "\n";
    }
    std::cout << "\n候选方案还没有生效。看过报告之后，用 assist adopt 或 assist abandon 做决定。\n";
    return 0;
}

assistant::AssistService decisionService(App& app, assistant::RecipeAdvisor& nobody) {
    return assistant::AssistService(app.workflows, app.assistSessions, nobody, app.candidateEditor, app.trialRunner,
                                    app.goldenCases, app.bus);
}

}  // namespace

int runGoldenCommand(App& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
    if (sub == "sample" && args.size() >= 3) {
        std::string lighting = "dim", scenario = "basic";
        for (std::size_t i = 3; i + 1 < args.size(); ++i) {
            if (args[i] == "--blind-lighting") lighting = args[i + 1];
            if (args[i] == "--scenario") scenario = args[i + 1];
        }
        return cmdGoldenSample(app, args[2], lighting, scenario);
    }
    if (sub == "list") {
        for (const auto& id : app.goldenSets.listGoldenSets()) std::cout << id << "\n";
        return 0;
    }
    if (sub == "show" && args.size() >= 3) return cmdGoldenShow(app, args[2]);
    std::cerr << "未知的 golden 命令\n";
    return 1;
}

int runWorkflowCommand(App& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
    if (sub == "import" && args.size() >= 3) {
        auto id = app.workflows.importFile(pathFromUtf8(args[2]));
        if (!id) return fail(id.error());
        std::cout << id.value() << "\n";
        return 0;
    }
    if (sub == "list") {
        for (const auto& id : app.workflows.list()) std::cout << id << "\n";
        return 0;
    }
    std::cerr << "未知的 workflow 命令\n";
    return 1;
}

int runAssistCommand(App& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
    if (sub == "run") return cmdAssistRun(app, args);

    // 下面几个命令不需要顾问：查看，以及由人做出的决定
    assist_adapters::ScriptedAdvisor nobody("none", {});
    auto service = decisionService(app, nobody);
    if (sub == "list") {
        for (const auto& id : service.listSessions()) std::cout << id << "\n";
        return 0;
    }
    if (sub == "show" && args.size() >= 3) {
        auto view = service.getSession(args[2]);
        if (!view) return fail(view.error());
        std::cout << view.value().reportMarkdown << "\n";
        return 0;
    }
    if ((sub == "adopt" || sub == "abandon") && args.size() >= 3) {
        const auto st = sub == "adopt" ? service.adopt(args[2]) : service.abandon(args[2]);
        if (!st) return fail(st.error());
        auto view = service.getSession(args[2]);
        if (!view) return fail(view.error());
        if (sub == "adopt") {
            std::cout << "已采纳。候选方案 " << view.value().candidateRecipeId
                      << " 现在是你的一份普通草稿；要用于生产，仍然需要你自己发布（recipe publish）。\n";
        } else {
            std::cout << "已放弃。\n";
        }
        return 0;
    }
    std::cerr << "未知的 assist 命令\n";
    return 1;
}

}  // namespace vision::cli
