#pragma once

// 产线运行平台的组装：检测执行、上线管理、工位运行、模型仓库（只接收）、方案包导入。
// 没有方案编排、方案助手、数据集、训练——那些只属于方案构建平台（apps/common/platform.h，它在这一份之上再加）。
//
// 两个平台的划分见 docs/ddd/08-platforms-and-training.md 和 docs/ddd/13-two-programs.md。

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <mutex>
#include <string>

#include "vision/catalog/api.h"
#include "vision/common/log.h"
#include "vision/common/path.h"
#include "vision/content/content_store.h"
#include "vision/eventbus/in_memory_event_bus.h"
#include "vision/exchange/package_file.h"
#include "vision/execution/api.h"
#include "vision/image_sources/folder_source.h"
#ifdef VISION_WITH_ONNXRUNTIME
#include "vision/inference_onnx/onnx_model_source.h"
#endif
#include "vision/models/api.h"
#include "vision/persistence/runtime_repositories.h"
#include "vision/quality/api.h"
#include "vision/release/api.h"
#include "vision/station/api.h"

namespace vision::platform {

// 方案版本从哪里来。
//   产线运行平台：只有从方案包导入的。
//   方案构建平台：还有本机方案编排发布的（通过 setPrimary 接上），先找它。
class RecipeSources final : public release::RecipeVersionSource {
public:
    explicit RecipeSources(const release::ImportedRecipes& imported) : imported_(imported) {}
    void setPrimary(const release::RecipeVersionSource& primary) { primary_ = &primary; }
    Result<authoring::RecipeVersionDocument> find(const std::string& recipeVersionId) const override {
        if (primary_ != nullptr) {
            if (auto own = primary_->find(recipeVersionId)) return own;
        }
        if (auto doc = imported_.find(recipeVersionId)) return *doc;
        return Result<authoring::RecipeVersionDocument>::failure("release.recipe_version_not_found",
                                                                 "找不到方案版本 " + recipeVersionId + "（它是不是还没有导入？）");
    }

private:
    const release::ImportedRecipes& imported_;
    const release::RecipeVersionSource* primary_ = nullptr;
};

// 把“这个方案版本能不能加载”这个端口接到检测引擎上：让它真的加载一次（结果会留在缓存里，不白做）。
class ExecutionLoadCheck final : public release::LoadCheck {
public:
    explicit ExecutionLoadCheck(execution::ExecutionService& execution) : execution_(execution) {}
    Status canLoad(const authoring::RecipeVersionDocument& recipe) override { return execution_.prepare(recipe); }

private:
    execution::ExecutionService& execution_;
};

// 系统时钟
class SystemClock final : public quality::Clock {
public:
    std::int64_t nowMs() override {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
};

// 工位每完成一次检测，交给质量分析记下来。
// 两个上下文互不认识：工位只知道“有人想听”，质量分析只知道“有一次检测要记”，翻译在这里做。
class RecordInspections final : public station::InspectionObserver {
public:
    explicit RecordInspections(quality::QualityService& quality) : quality_(quality) {}
    void onInspected(const std::string& stationId, const station::StationInspection& inspection) override {
        quality::InspectionToRecord in;
        in.stationId = stationId;
        in.sequence = inspection.sequence;
        in.releaseId = inspection.releaseId;
        in.recipeVersionId = inspection.report.recipeVersionId;
        in.frameName = inspection.frameName;
        in.verdict = inspection.report.verdict;
        for (const auto& r : inspection.report.reasons) in.reasons.push_back({r.nodeId, r.pass, r.text});
        in.result = inspection.report.result;
        in.totalMs = inspection.report.totalMs;
        in.swapped = inspection.swapped;
        in.frame = inspection.frame;
        // 记录写不下来不能让检测失败：产线照常跑，只在日志里留一条
        if (auto r = quality_.record(in); !r) {
            log::warn("quality", "检测记录没有写下来", {{"station", stationId}, {"message", r.error().message}});
        }
    }

private:
    quality::QualityService& quality_;
};

// 定时清理过期的检测图像：启动时清一次，之后每隔一段时间清一次。
// 只有长时间运行的程序（vision-runtime、vision-line serve）需要它；跑完一条命令就退出的，启动时清一次就够了。
class ImageJanitor {
public:
    explicit ImageJanitor(quality::QualityService& quality) : quality_(quality) {}
    ~ImageJanitor() { stop(); }
    void start(std::chrono::seconds interval = std::chrono::hours(1)) {
        if (thread_.joinable()) return;
        thread_ = std::thread([this, interval] {
            std::unique_lock<std::mutex> lock(mutex_);
            for (;;) {
                lock.unlock();
                (void)quality_.cleanExpiredImages();
                lock.lock();
                if (wake_.wait_for(lock, interval, [this] { return stopping_; })) return;
            }
        });
    }
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }

private:
    quality::QualityService& quality_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool stopping_ = false;
    std::thread thread_;
};

// 成员的声明顺序就是构造顺序，析构时反过来。
struct LinePlatform {
    eventbus::InMemoryEventBus bus;
    content::FileContentStore contentStore;  // 按内容哈希存放的二进制内容：匹配模板、模型文件、样本图像
    content::TemplateContent templates;
    execution::ExecutionService execution;

    // 模型仓库（BC-7）。产线运行平台上只接收方案包带来的模型，不训练
    persistence::JsonModelVersionRepository modelVersionRepository;
    content::ModelFileStore modelFiles;
    models::ModelService models;
#ifdef VISION_WITH_ONNXRUNTIME
    // 推理引擎。编译时关掉了推理功能的话，用到模型的方案会报“没有配置推理引擎”
    inference_onnx::OnnxModelSource modelSource;
#endif

    // 上线管理（BC-4）
    persistence::JsonImportedRecipes importedRecipes;  // 从方案包导入的方案版本
    RecipeSources recipeSource;
    persistence::JsonReleaseRepository releaseRepository;
    release::ReleaseService releases;
    ExecutionLoadCheck loadCheck;

    // 方案包（REL-02）
    exchange::TarPackageExchange packageExchange;
    exchange::CatalogOperatorInventory operatorInventory;
    exchange::RepositoryModelArchive modelArchive;
    exchange::StoreContentArchive contentArchive;
    release::PackageService packages;

    // 工位运行（BC-5）
    image_sources::ImageSources imageSources;  // 工位的图像从哪里来（现在只有“目录”这一种）
    persistence::JsonStationRepository stationRepository;
    station::StationService stations;

    // 质量分析（BC-10）：每次产线检测记一条。不合格的和出错的连图像一起留下
    persistence::JsonlInspectionRecordRepository inspectionRecords;
    persistence::JsonlReviewRepository reviews;  // 复判
    SystemClock clock;
    content::InspectionImageFiles inspectionImages;  // 检测图像：短期缓存 + 长期保留
    quality::QualityService quality;
    RecordInspections recordInspections;
    ImageJanitor imageJanitor;  // 没有 start 就什么都不做

    bool verboseEvents = false;

    // 工位的后台加载线程会用到检测引擎、模型、方案版本的来源。在任何成员开始销毁之前，先等它们全部结束。
    // （2026-11-24 发现：部署了一个用到模型的上线包之后马上退出程序，会崩溃。）
    ~LinePlatform() { stations.finishBackgroundWork(); }
    LinePlatform(const LinePlatform&) = delete;
    LinePlatform& operator=(const LinePlatform&) = delete;

    explicit LinePlatform(const std::filesystem::path& dataDir)
        : contentStore(dataDir),
          templates(contentStore),
          execution(catalog::OperatorCatalog::global(), bus),
          modelVersionRepository(dataDir),
          modelFiles(contentStore),
          models(modelVersionRepository, modelFiles, bus),
#ifdef VISION_WITH_ONNXRUNTIME
          modelSource(models),
#endif
          importedRecipes(dataDir),
          recipeSource(importedRecipes),
          releaseRepository(dataDir),
          releases(releaseRepository, recipeSource, bus),
          loadCheck(execution),
          operatorInventory(catalog::OperatorCatalog::global()),
          modelArchive(models),
          contentArchive(contentStore),
          packages(recipeSource, importedRecipes, packageExchange, operatorInventory, modelArchive, contentArchive, bus),
          stationRepository(dataDir),
          stations(stationRepository, releases, execution, bus),
          inspectionRecords(dataDir),
          reviews(dataDir),
          inspectionImages(dataDir, contentStore),
          quality(inspectionRecords, clock, inspectionImages),
          recordInspections(quality),
          imageJanitor(quality) {
        execution.useTemplateSource(templates);
#ifdef VISION_WITH_ONNXRUNTIME
        execution.useModelSource(modelSource);
#endif
        releases.useLoadCheck(loadCheck);
        packages.useReleases(releases);
        quality.useReviews(reviews, bus);
        stations.useImageSources(imageSources);
        stations.useInspectionObserver(recordInspections);
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

// 程序的图像选项（--keep-images、--image-retention-hours）。出错时返回原因
inline Status applyImageOptions(LinePlatform& app, const std::string& keepImages, const std::string& retentionHours) {
    if (!keepImages.empty()) {
        if (keepImages != "all" && keepImages != "ng" && keepImages != "none") {
            return Status::failure("cli.bad_option", "--keep-images 只能是 all、ng 或 none");
        }
        app.quality.setImagePolicy(keepImages == "all" ? quality::ImagePolicy::All
                                   : keepImages == "none" ? quality::ImagePolicy::None : quality::ImagePolicy::NgAndError);
    }
    if (!retentionHours.empty()) {
        double hours = 0;
        try {
            hours = std::stod(retentionHours);
        } catch (const std::exception&) {
            return Status::failure("cli.bad_option", "--image-retention-hours 后面要跟一个数字");
        }
        if (auto st = app.quality.setImageRetentionHours(hours); !st) return st;
    }
    return Status::success();
}

// 把复判过的检测的图像导出成两个目录：<目录>/ok（人确认是合格的）和 <目录>/ng（人确认是不合格的）。
// 这正是建基准图集、导入样本要的样子——复判的结果由此回到方案构建那一侧（数据闭环，规则 P3）。
// 只有当时留下了图像的记录才导得出来。misjudgedOnly：只要复判确认判错了的（误报、漏检）。
struct ReviewedExport {
    int ok = 0, ng = 0;
    int withoutImage = 0;  // 复判过、但没有图像的（当时没留，或者过期清掉了）
};
inline Result<ReviewedExport> exportReviewedImages(LinePlatform& app, const std::string& stationId, const std::string& directory,
                                                   bool misjudgedOnly) {
    namespace fs = std::filesystem;
    using R = Result<ReviewedExport>;
    if (auto v = app.stations.getStation(stationId); !v) return v.error();
    const fs::path root = pathFromUtf8(directory);
    std::error_code ec;
    fs::create_directories(root / "ok", ec);
    fs::create_directories(root / "ng", ec);
    if (ec) return R::failure("quality.export_failed", "建不了目录 " + directory);
    ReviewedExport report;
    quality::RecordQuery query;
    query.limit = 500;
    for (;;) {
        const auto page = app.quality.query(stationId, query);
        for (const auto& r : page) {
            if (!r.review || (misjudgedOnly && r.review->kind != "false_alarm" && r.review->kind != "miss")) continue;
            if (r.imageState != "kept" && r.imageState != "cached") {  // 当时没留，或者已经过期清掉了
                ++report.withoutImage;
                continue;
            }
            auto bytes = app.quality.recordImage(stationId, r.id);
            if (!bytes) return R::failure(bytes.error().code, "记录 " + r.id + " 的图像读不出来：" + bytes.error().message);
            // 文件名用记录的编号：不会重名，也能从文件找回是哪一次检测
            const fs::path file = root / (r.review->truthNg ? "ng" : "ok") / (r.id + ".png");
            std::ofstream out(file, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(bytes.value().data()), static_cast<std::streamsize>(bytes.value().size()));
            if (!out) return R::failure("quality.export_failed", "写不进 " + pathToUtf8(file));
            ++(r.review->truthNg ? report.ng : report.ok);
        }
        if (static_cast<int>(page.size()) < query.limit) return report;
        query.beforeId = page.back().id;
    }
}

}  // namespace vision::platform
