#pragma once

// vision-cli 各个源文件共用的部分：组装好的全部服务，以及几个小工具。

#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "vision/assist_adapters/platform_ports.h"
#include "vision/assist_adapters/storage.h"
#include "vision/assistant/api.h"
#include "vision/authoring/api.h"
#include "vision/catalog/api.h"
#include "vision/common/log.h"
#include "vision/data/api.h"
#include "vision/content/content_store.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/execution/api.h"
#ifdef VISION_WITH_ONNXRUNTIME
#include "vision/inference_onnx/onnx_model_source.h"
#endif
#include "vision/models/api.h"
#include "vision/persistence/json_repositories.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/release/api.h"
#include "vision/station/api.h"
#include "vision/training/api.h"
#include "vision/training_http/http_training_backend.h"

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

// 把“这个方案版本能不能加载”这个端口接到检测引擎上：让它真的加载一次（结果会留在缓存里，不白做）。
class ExecutionLoadCheck final : public release::LoadCheck {
public:
    explicit ExecutionLoadCheck(execution::ExecutionService& execution) : execution_(execution) {}
    Status canLoad(const authoring::RecipeVersionDocument& recipe) override { return execution_.prepare(recipe); }

private:
    execution::ExecutionService& execution_;
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
    ExecutionLoadCheck loadCheck;
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

    // 数据（BC-9）：样本、数据集。基准图集的用例通过 sampleRegistry 变成对样本的引用（规则 N1 靠它检查）
    persistence::JsonSampleRepository sampleRepository;
    persistence::JsonDatasetRepository datasetRepository;
    content::SampleFiles sampleFiles;
    data::DataService data;
    content::DataSampleRegistry sampleRegistry;

    // 模型仓库（BC-7）和模型训练（BC-12）。
    // 训练后端：配置了训练服务的地址就通过 HTTP 调它（services/training）；没有配置时是占位的“没有配置”——
    // 提交的训练任务会以“已失败”记录下来，其余功能照常可用。
    persistence::JsonModelVersionRepository modelVersionRepository;
    persistence::JsonTrainingJobRepository trainingJobRepository;
    content::ModelFileStore modelFiles;
    models::ModelService models;
    content::DataDatasetSource datasetSource;
    content::RepositoryModelImporter modelImporter;
#ifdef VISION_WITH_ONNXRUNTIME
    // 推理引擎：检测引擎通过它运行模型仓库里的模型。编译时关掉了推理功能的话，用到模型的方案会报“没有配置推理引擎”
    inference_onnx::OnnxModelSource modelSource;
#endif
    http::CurlCommandHttpClient trainingHttp;
    std::unique_ptr<training::TrainingBackend> trainingBackend;
    training::TrainingService training;

    bool verboseEvents = false;

    explicit App(const std::filesystem::path& dataDir, const std::string& trainingUrl = {})
        : recipes(dataDir),
          versions(dataDir),
          releaseRepository(dataDir),
          stationRepository(dataDir),
          contentStore(dataDir),
          templates(contentStore),
          authoring(recipes, versions, catalog::OperatorCatalog::global(), bus),
          execution(catalog::OperatorCatalog::global(), bus),
          recipeSource(authoring),
          releases(releaseRepository, recipeSource, bus),
          loadCheck(execution),
          stations(stationRepository, releases, execution, bus),
          goldenSetRepository(dataDir),
          goldenSets(goldenSetRepository, bus),
          workflows(dataDir),
          assistSessions(dataDir),
          candidateEditor(authoring),
          trialRunner(authoring, execution),
          goldenCases(goldenSets),
          sampleRepository(dataDir),
          datasetRepository(dataDir),
          sampleFiles(contentStore),
          data(sampleRepository, datasetRepository, sampleFiles, bus),
          sampleRegistry(data),
          modelVersionRepository(dataDir),
          trainingJobRepository(dataDir),
          modelFiles(contentStore),
          models(modelVersionRepository, modelFiles, bus),
          datasetSource(data),
          modelImporter(models),
#ifdef VISION_WITH_ONNXRUNTIME
          modelSource(models),
#endif
          trainingBackend(trainingUrl.empty()
                              ? std::unique_ptr<training::TrainingBackend>(std::make_unique<content::NoTrainingBackend>())
                              : std::make_unique<training_http::HttpTrainingBackend>(trainingHttp, trainingUrl)),
          training(trainingJobRepository, *trainingBackend, datasetSource, modelImporter, bus) {
        goldenSets.useSampleRegistry(sampleRegistry);
        // 匹配模板：方案编排往内容仓库里存，检测引擎从内容仓库里取
        authoring.useTemplateStore(templates);
        execution.useTemplateSource(templates);
        releases.useLoadCheck(loadCheck);
#ifdef VISION_WITH_ONNXRUNTIME
        execution.useModelSource(modelSource);
#endif
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

// sample ...、dataset ... 两组命令（数据，见 data_commands.cpp）。
int runSampleCommand(App& app, const std::vector<std::string>& args);
int runDatasetCommand(App& app, const std::vector<std::string>& args);
// train ...、model ... 两组命令（训练任务和模型仓库，见 data_commands.cpp）。
int runTrainCommand(App& app, const std::vector<std::string>& args);
int runModelCommand(App& app, const std::vector<std::string>& args);

}  // namespace vision::cli
