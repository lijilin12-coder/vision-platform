#pragma once

// vision-cli 各个源文件共用的部分：组装好的全部服务，以及几个小工具。

#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "vision/assist_adapters/platform_ports.h"
#include "vision/assist_adapters/storage.h"
#include "vision/assistant/api.h"
#include "vision/authoring/api.h"
#include "vision/catalog/api.h"
#include "vision/common/log.h"
#include "vision/content/content_store.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/execution/api.h"
#include "vision/persistence/json_repositories.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/release/api.h"
#include "vision/station/api.h"

namespace vision::cli {

// 把“取方案版本文档”这个端口接到方案编排上。
// 现在两个平台还在同一个程序里；分开之后，产线运行平台会换成从上线包文件里读。
class AuthoringRecipeSource final : public release::RecipeVersionSource {
public:
    explicit AuthoringRecipeSource(const authoring::AuthoringService& authoring) : authoring_(authoring) {}
    Result<authoring::RecipeVersionDocument> find(const std::string& recipeVersionId) const override {
        return authoring_.getVersionDocument(recipeVersionId);
    }

private:
    const authoring::AuthoringService& authoring_;
};

// 组装根：在这里把适配器和各个上下文的应用服务连接起来。
// 成员的声明顺序就是构造顺序，析构时反过来：工位服务最先析构，它会等后台加载线程结束。
struct App {
    eventbus::InMemoryEventBus bus;
    persistence::JsonRecipeRepository recipes;
    persistence::JsonRecipeVersionRepository versions;
    persistence::JsonReleaseRepository releaseRepository;
    persistence::JsonStationRepository stationRepository;
    content::FileContentStore contentStore;  // 按内容哈希存放的二进制内容（现在只有匹配模板）
    content::TemplateContent templates;
    authoring::AuthoringService authoring;
    execution::ExecutionService execution;
    AuthoringRecipeSource recipeSource;
    release::ReleaseService releases;
    station::StationService stations;

    // ---- 以下只属于方案构建平台（ADR-28）：基准图集、工作流、方案助手 ----
    // 现在两个平台还在同一个程序里；分开之后，产线运行平台的组装根里没有这一段。
    persistence::JsonGoldenSetRepository goldenSetRepository;
    release::GoldenSetService goldenSets;
    assist_adapters::JsonWorkflowRepository workflows;
    assist_adapters::JsonAssistSessionRepository assistSessions;
    assist_adapters::AuthoringCandidateEditor candidateEditor;
    assist_adapters::EngineTrialRunner trialRunner;
    assist_adapters::ReleaseGoldenCases goldenCases;

    bool verboseEvents = false;

    explicit App(const std::filesystem::path& data)
        : recipes(data),
          versions(data),
          releaseRepository(data),
          stationRepository(data),
          contentStore(data),
          templates(contentStore),
          authoring(recipes, versions, catalog::OperatorCatalog::global(), bus),
          execution(catalog::OperatorCatalog::global(), bus),
          recipeSource(authoring),
          releases(releaseRepository, recipeSource, bus),
          stations(stationRepository, releases, execution, bus),
          goldenSetRepository(data),
          goldenSets(goldenSetRepository, bus),
          workflows(data),
          assistSessions(data),
          candidateEditor(authoring),
          trialRunner(authoring, execution),
          goldenCases(goldenSets) {
        // 匹配模板：方案编排往内容仓库里存，检测引擎从内容仓库里取
        authoring.useTemplateStore(templates);
        execution.useTemplateSource(templates);
        bus.subscribe("*", [this](const EventPtr& e) {
            // 每个领域事件都记入日志：事后可以还原“发生过什么”。
            // 每次检测都有一个“检测已完成”，数量很大，记为 debug，平时不输出。
            if (std::string(e->name()) == "InspectionCompleted") {
                log::debug("event", e->name());
            } else {
                log::info("event", e->name());
            }
            if (verboseEvents) std::cerr << "  [事件] " << e->name() << "\n";
        });
    }
};

// 报告命令失败：写日志、在控制台显示，返回退出码 1。
int fail(const Error& e);

// 把命令行里给出的图像文件和目录展开成图像文件的列表（目录里的按文件名排序）。
std::vector<std::filesystem::path> expandImages(const std::vector<std::string>& args);

// release ... 和 station ... 两组命令。args[0] 是 "release" 或 "station"。
int runReleaseCommand(App& app, const std::vector<std::string>& args);
int runStationCommand(App& app, const std::vector<std::string>& args);

// golden ...、workflow ...、assist ... 三组命令（方案助手，见 assist_commands.cpp）。
int runGoldenCommand(App& app, const std::vector<std::string>& args);
int runWorkflowCommand(App& app, const std::vector<std::string>& args);
int runAssistCommand(App& app, const std::vector<std::string>& args);

}  // namespace vision::cli
