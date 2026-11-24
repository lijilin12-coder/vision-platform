#pragma once

// 方案构建平台的组装：在产线运行平台（line_platform.h）之上，再加方案编排、基准图集、方案助手、数据集、训练。
// vision-cli 和 vision-runtime 用的是这一份；vision-line 只用 line_platform.h。

#include "line_platform.h"
#include "vision/assist_adapters/platform_ports.h"
#include "vision/assist_adapters/storage.h"
#include "vision/assistant/api.h"
#include "vision/authoring/api.h"
#include "vision/data/api.h"
#include "vision/persistence/json_repositories.h"
#include "vision/training/api.h"
#include "vision/training_http/http_training_backend.h"

namespace vision::platform {

// 本机方案编排发布的方案版本
class AuthoringRecipeSource final : public release::RecipeVersionSource {
public:
    explicit AuthoringRecipeSource(const authoring::AuthoringService& authoring) : authoring_(authoring) {}
    Result<authoring::RecipeVersionDocument> find(const std::string& recipeVersionId) const override {
        return authoring_.getVersionDocument(recipeVersionId);
    }

private:
    const authoring::AuthoringService& authoring_;
};

struct Platform : LinePlatform {
    // 方案编排（BC-2）
    persistence::JsonRecipeRepository recipes;
    persistence::JsonRecipeVersionRepository versions;
    authoring::AuthoringService authoring;
    AuthoringRecipeSource authoringSource;

    // 基准图集、工作流、方案助手（BC-4 的一部分、BC-11）
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

    // 模型训练（BC-12）。
    // 训练后端：配置了训练服务的地址就通过 HTTP 调它（services/training）；没有配置时是占位的“没有配置”——
    // 提交的训练任务会以“已失败”记录下来，其余功能照常可用。
    persistence::JsonTrainingJobRepository trainingJobRepository;
    content::DataDatasetSource datasetSource;
    content::RepositoryModelImporter modelImporter;
    http::CurlCommandHttpClient trainingHttp;
    std::unique_ptr<training::TrainingBackend> trainingBackend;
    training::TrainingService training;

    // 这里的成员会比产线运行平台那一层的先销毁，而工位的后台加载会通过 recipeSource 用到方案编排。
    // 所以同样要先等后台加载结束。
    ~Platform() { stations.finishBackgroundWork(); }

    explicit Platform(const std::filesystem::path& dataDir, const std::string& trainingUrl = {})
        : LinePlatform(dataDir),
          recipes(dataDir),
          versions(dataDir),
          authoring(recipes, versions, catalog::OperatorCatalog::global(), bus),
          authoringSource(authoring),
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
          trainingJobRepository(dataDir),
          datasetSource(data),
          modelImporter(models),
          trainingBackend(trainingUrl.empty()
                              ? std::unique_ptr<training::TrainingBackend>(std::make_unique<content::NoTrainingBackend>())
                              : std::make_unique<training_http::HttpTrainingBackend>(trainingHttp, trainingUrl)),
          training(trainingJobRepository, *trainingBackend, datasetSource, modelImporter, bus) {
        recipeSource.setPrimary(authoringSource);  // 方案版本：先找本机方案编排发布的
        goldenSets.useSampleRegistry(sampleRegistry);
        authoring.useTemplateStore(templates);  // 匹配模板：方案编排往内容仓库里存，检测引擎从内容仓库里取
    }
};

}  // namespace vision::platform
