#pragma once

// 方案构建平台的组装：在产线运行平台（line_platform.h）之上，再加方案编排、基准图集、方案助手、数据集、训练。
// vision-cli 和 vision-runtime 用的是这一份；vision-line 只用 line_platform.h。

#include <algorithm>
#include <cctype>

#include "line_platform.h"
#include "vision/assist_adapters/platform_ports.h"
#include "vision/assist_adapters/storage.h"
#include "vision/assistant/api.h"
#include "vision/authoring/api.h"
#include "vision/common/path.h"
#include "vision/data/api.h"
#include "vision/imaging/cv_bridge.h"
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

// 验收检查里“跑一个用例”：读图像，用检测引擎按“检查”的用途检测一次
class EngineCaseRunner final : public release::CaseRunner {
public:
    explicit EngineCaseRunner(execution::ExecutionService& execution) : execution_(execution) {}
    Result<release::CaseRun> run(const authoring::RecipeVersionDocument& recipe, const std::string& imageRef) override {
        auto image = imaging::loadImage(imageRef);
        if (!image) return image.error();
        auto report = execution_.inspect(recipe, image.value(), execution::RunPurpose::Check);
        if (!report) return report.error();
        return release::CaseRun{report.value().verdict, report.value().totalMs};
    }

private:
    execution::ExecutionService& execution_;
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
    EngineCaseRunner caseRunner;  // 验收检查用

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
          caseRunner(execution),
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
        releases.useAcceptance(goldenSetRepository, caseRunner);
        authoring.useTemplateStore(templates);  // 匹配模板：方案编排往内容仓库里存，检测引擎从内容仓库里取
    }
};

// 用两个目录做一个基准图集：okDirectory 里的图像真实判定是合格，ngDirectory 里的是不合格。
// “这幅图像在哪个目录里”就是人对它的确认。只看目录的第一层，按文件名排序。
inline Result<std::string> createGoldenSetFromFolders(Platform& app, const std::string& name, const std::string& okDirectory,
                                                      const std::string& ngDirectory, const release::CriteriaView& criteria) {
    using R = Result<std::string>;
    namespace fs = std::filesystem;
    release::GoldenSetDefinition definition;
    definition.name = name;
    definition.criteria = criteria;
    const auto add = [&](const std::string& directory, bool truthNg) -> Status {
        if (directory.empty()) return Status::success();  // 可以只有一类（例如只有合格的），另一类不给
        const fs::path dir = pathFromUtf8(directory);
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return Status::failure("golden.folder_not_found", "找不到目录 " + directory);
        std::vector<fs::path> files;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            std::string ext = pathToUtf8(it->path().extension());
            for (auto& ch : ext) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            if (it->is_regular_file(ec) && (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".tif" || ext == ".tiff")) {
                files.push_back(it->path());
            }
        }
        std::sort(files.begin(), files.end());
        for (const auto& file : files) {
            definition.cases.push_back({std::string(truthNg ? "ng/" : "ok/") + pathToUtf8(file.filename()),
                                        pathToUtf8(fs::absolute(file, ec)), truthNg, {}});
        }
        return Status::success();
    };
    if (auto st = add(okDirectory, false); !st) return st.error();
    if (auto st = add(ngDirectory, true); !st) return st.error();
    if (definition.cases.empty()) return R::failure("golden.no_images", "两个目录里都没有找到图像");
    return app.goldenSets.createGoldenSet(definition);
}

}  // namespace vision::platform
