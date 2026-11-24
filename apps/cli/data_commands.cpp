// vision-cli 的 sample / dataset 两组命令（BC-9 数据，训练闭环需要的最小部分）。

#include <chrono>
#include <opencv2/imgproc.hpp>
#include <thread>

#include "cli_app.h"
#include "vision/common/path.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::cli {

namespace {

std::string shortId(const std::string& id) { return id.size() > 12 ? id.substr(0, 12) + "…" : id; }

// 样本 ID 是 64 个字符的哈希。命令行里允许只写开头的几位，只要不和别的样本混淆
Result<std::string> resolveSample(App& app, const std::string& prefix) {
    std::string found;
    for (const auto& id : app.data.listSamples()) {
        if (id.rfind(prefix, 0) != 0) continue;
        if (!found.empty()) return Result<std::string>::failure("cli.ambiguous", "有不止一个样本以 " + prefix + " 开头，请多写几位");
        found = id;
    }
    if (found.empty()) return Result<std::string>::failure("data.sample_not_found", "找不到样本 " + prefix);
    return found;
}

// 练习训练用的合成图像：灰色背景上一块深色零件，零件表面从左到右略有明暗变化。
// 合格的图上可能有几个 2×2 的灰尘亮点、整体亮度也有些浮动——这些都是“正常的变化”，模型要学会容忍。
// defect：0 没有缺陷；1 亮斑；2 深色划痕；3 污渍（一块比周围略暗的区域）。
cv::Mat drawSurfaceSample(int defect, unsigned seed) {
    cv::RNG rng(seed);  // OpenCV 自己的随机数发生器：在任何平台上结果都一样
    cv::Mat m(480, 640, CV_8UC1, cv::Scalar(150));
    const cv::Rect part(120, 90, 400, 300);
    for (int x = part.x; x < part.x + part.width; ++x) {
        const double g = 70 + 12.0 * (x - part.x) / part.width;
        cv::line(m, {x, part.y}, {x, part.y + part.height - 1}, cv::Scalar(g));
    }
    if (rng.uniform(0, 2) == 1) {
        for (int i = 0; i < 3; ++i) cv::rectangle(m, cv::Rect(rng.uniform(140, 500), rng.uniform(110, 370), 2, 2), cv::Scalar(230), cv::FILLED);
    }
    const cv::Point center(rng.uniform(170, 470), rng.uniform(130, 350));
    if (defect == 1) {
        cv::circle(m, center, rng.uniform(7, 13), cv::Scalar(225), cv::FILLED);
    } else if (defect == 2) {
        const double angle = rng.uniform(0.0, CV_PI);
        const int half = rng.uniform(25, 45);
        const cv::Point d(static_cast<int>(half * std::cos(angle)), static_cast<int>(half * std::sin(angle)));
        cv::line(m, center - d, center + d, cv::Scalar(15), 4);
    } else if (defect == 3) {
        cv::ellipse(m, center, {rng.uniform(16, 26), rng.uniform(10, 18)}, rng.uniform(0, 180), 0, 360, cv::Scalar(30), cv::FILLED);
    }
    m.convertTo(m, -1, rng.uniform(0.92, 1.08), 0);
    cv::Mat noise(m.size(), CV_16SC1), wide;
    rng.fill(noise, cv::RNG::NORMAL, 0.0, 4.0);
    m.convertTo(wide, CV_16S);
    wide += noise;
    wide.convertTo(m, CV_8U);
    return m;
}

int generateSamples(const std::string& dir, int okCount, int ngCount, unsigned seed) {
    namespace fs = std::filesystem;
    static const char* kDefects[] = {"", "spot", "scratch", "stain"};
    std::error_code ec;
    for (const char* sub : {"ok", "ng"}) {
        fs::create_directories(pathFromUtf8(dir) / sub, ec);
        if (ec) {
            std::cerr << "无法创建目录 " << dir << "\n";
            return 1;
        }
    }
    for (int i = 0; i < okCount + ngCount; ++i) {
        const bool ng = i >= okCount;
        const int defect = ng ? 1 + (i - okCount) % 3 : 0;
        char name[64];
        if (ng) std::snprintf(name, sizeof name, "ng_%03d_%s.png", i - okCount + 1, kDefects[defect]);
        else std::snprintf(name, sizeof name, "ok_%03d.png", i + 1);
        const cv::Mat drawn = drawSurfaceSample(defect, seed * 100003u + static_cast<unsigned>(i));
        auto image = imaging::fromMat(drawn);
        if (!image) return fail(image.error());
        const auto file = pathFromUtf8(dir) / (ng ? "ng" : "ok") / name;
        if (auto st = imaging::saveImage(pathToUtf8(file), image.value()); !st) return fail(st.error());
    }
    std::cout << "已生成 " << okCount << " 幅合格的、" << ngCount << " 幅不合格的图像：" << dir << "\n";
    return 0;
}

}  // namespace

int runSampleCommand(App& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
    if (sub == "generate" && args.size() >= 3) {
        int ok = 60, ng = 12;
        unsigned seed = 1;
        for (std::size_t i = 3; i + 1 < args.size(); i += 2) {
            try {
                if (args[i] == "--ok") ok = std::stoi(args[i + 1]);
                else if (args[i] == "--ng") ng = std::stoi(args[i + 1]);
                else if (args[i] == "--seed") seed = static_cast<unsigned>(std::stoul(args[i + 1]));
            } catch (const std::exception&) {
                std::cerr << args[i] << " 后面必须是数字\n";
                return 1;
            }
        }
        if (ok < 0 || ng < 0 || ok + ng == 0 || ok + ng > 5000) {
            std::cerr << "图像的数量必须在 1～5000 之间\n";
            return 1;
        }
        return generateSamples(args[2], ok, ng, seed);
    }
    if (sub == "import" && args.size() >= 3) {
        // sample import <图像或目录...> [--label ok|ng]
        std::string label;
        std::vector<std::string> inputs;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--label" && i + 1 < args.size()) label = args[++i];
            else inputs.push_back(args[i]);
        }
        int count = 0;
        for (const auto& path : expandImages(inputs)) {
            auto id = app.data.importSample(pathToUtf8(path), label);
            if (!id) return fail(id.error());
            std::cout << id.value() << "  " << pathToUtf8(path.filename()) << "\n";
            ++count;
        }
        if (count == 0) {
            std::cerr << "没有找到图像文件\n";
            return 1;
        }
        return 0;
    }
    if (sub == "list") {
        for (const auto& id : app.data.listSamples()) {
            const auto s = app.data.getSample(id).value();
            std::cout << shortId(s.id) << "  " << s.label << "  " << s.use << "  " << (s.names.empty() ? "" : s.names.front())
                      << (s.names.size() > 1 ? "（还有 " + std::to_string(s.names.size() - 1) + " 个名字）" : "") << "\n";
        }
        return 0;
    }
    if (sub == "label" && args.size() >= 4) {
        auto id = resolveSample(app, args[2]);
        if (!id) return fail(id.error());
        const auto st = app.data.labelSample(id.value(), args[3]);
        return st ? 0 : fail(st.error());
    }
    std::cerr << "未知的 sample 命令\n";
    return 1;
}

int runDatasetCommand(App& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
    if (sub == "new" && args.size() >= 3) {
        auto id = app.data.createDataset(args[2]);
        if (!id) return fail(id.error());
        std::cout << id.value() << "\n";
        return 0;
    }
    if (sub == "add" && args.size() >= 4) {
        // dataset add <数据集ID> <样本ID...>；样本 ID 可以只写开头几位，也可以写 --all-unassigned
        std::vector<std::string> ids;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--all-unassigned") {
                for (const auto& id : app.data.listSamples()) {
                    if (app.data.getSample(id).value().use == "unassigned") ids.push_back(id);
                }
                continue;
            }
            auto id = resolveSample(app, args[i]);
            if (!id) return fail(id.error());
            ids.push_back(id.value());
        }
        const auto st = app.data.addToDataset(args[2], ids);
        if (!st) return fail(st.error());
        std::cout << "加入了 " << ids.size() << " 个样本\n";
        return 0;
    }
    if (sub == "freeze" && args.size() >= 3) {
        auto version = app.data.freezeDataset(args[2]);
        if (!version) return fail(version.error());
        std::cout << version.value() << "\n";
        return 0;
    }
    if (sub == "show" && args.size() >= 3) {
        auto v = app.data.getDatasetVersion(args[2]);
        if (!v) return fail(v.error());
        std::cout << v.value().id << "  " << v.value().name << "：" << v.value().entries.size() << " 个样本（合格 " << v.value().okCount
                  << "，不合格 " << v.value().ngCount << "）\n";
        for (const auto& e : v.value().entries) std::cout << "  " << e.label << "  " << shortId(e.sampleId) << "\n";
        return 0;
    }
    std::cerr << "未知的 dataset 命令\n";
    return 1;
}

namespace {

void printJob(const training::JobView& v);

// 每秒向训练后端问一次，直到任务结束
int waitForJob(App& app, const std::string& jobId) {
    int lastPercent = -1, unreachable = 0;
    for (;;) {
        auto view = app.training.refresh(jobId);
        if (!view) {
            // 偶尔一次查询失败不算什么；连续失败就不等了（任务本身没有变，之后可以再查）
            if (++unreachable >= 5) return fail(view.error());
        } else {
            unreachable = 0;
            const auto& v = view.value();
            if (v.status != "running" && v.status != "submitted") {
                printJob(v);
                return v.status == "completed" ? 0 : 1;
            }
            const int percent = static_cast<int>(v.progress * 100);
            if (percent / 10 != lastPercent / 10) std::cout << "  训练中 " << percent << "%" << std::endl;
            lastPercent = percent;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

void printJob(const training::JobView& v) {
    std::cout << v.id << "  " << v.status;
    if (v.status == "running") std::cout << "  " << static_cast<int>(v.progress * 100) << "%";
    std::cout << "\n  数据集版本 " << v.datasetVersionId << "，划分 " << v.split << "，模型 " << v.modelName << "（" << v.modelKind
              << "），训练后端 " << v.backend << "\n";
    if (!v.failure.empty()) std::cout << "  失败原因：" << v.failure << "\n";
    for (const auto& [name, value] : v.metrics) std::cout << "  " << name << " = " << value << "\n";
    if (!v.modelVersionId.empty()) std::cout << "  模型版本 " << v.modelVersionId << "\n";
}

}  // namespace

int runTrainCommand(App& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
    if (sub == "submit" && args.size() >= 3) {
        // train submit <数据集版本ID> --name <模型名> [--epochs N] [--image-size N] [--seed N] [--wait]
        training::SubmitRequest request;
        request.datasetVersionId = args[2];
        bool wait = false;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] == "--wait") {
                wait = true;
                continue;
            }
            if (i + 1 >= args.size()) break;
            ++i;
            try {
                if (args[i - 1] == "--name") request.modelName = args[i];
                else if (args[i - 1] == "--image-size") request.settings["imageSize"] = std::stod(args[i]);
                else if (args[i - 1] == "--epochs") request.settings["epochs"] = std::stod(args[i]);
                else if (args[i - 1] == "--seed") request.seed = static_cast<unsigned>(std::stoul(args[i]));
            } catch (const std::exception&) {
                std::cerr << args[i - 1] << " 后面必须是数字\n";
                return 1;
            }
        }
        auto job = app.training.submit(request);
        if (!job) return fail(job.error());
        const auto view = app.training.getJob(job.value()).value();
        if (wait && view.status == "running") {
            std::cout << view.id << "  已提交，训练后端 " << view.backend << "\n";
            return waitForJob(app, job.value());
        }
        printJob(view);
        return view.status == "failed" ? 1 : 0;
    }
    if (sub == "wait" && args.size() >= 3) return waitForJob(app, args[2]);
    if (sub == "status" && args.size() >= 3) {
        auto view = app.training.refresh(args[2]);  // 顺便向训练后端问一次进度
        if (!view) return fail(view.error());
        printJob(view.value());
        return 0;
    }
    if (sub == "cancel" && args.size() >= 3) {
        const auto st = app.training.cancel(args[2]);
        return st ? 0 : fail(st.error());
    }
    if (sub == "list") {
        for (const auto& id : app.training.listJobs()) {
            const auto v = app.training.getJob(id).value();
            std::cout << v.id << "  " << v.status << "  " << v.modelName << "  " << v.modelVersionId << "\n";
        }
        return 0;
    }
    std::cerr << "未知的 train 命令\n";
    return 1;
}

int runModelCommand(App& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
    if (sub == "list") {
        for (const auto& id : app.models.listModelVersions()) {
            const auto v = app.models.getModelVersion(id).value();
            std::cout << v.id << "  " << v.status << "  " << v.contract.kind << "  " << v.provenance.kind << "\n";
        }
        return 0;
    }
    if (sub == "show" && args.size() >= 3) {
        auto view = app.models.getModelVersion(args[2]);
        if (!view) return fail(view.error());
        const auto& v = view.value();
        std::cout << v.id << "  " << v.status << "  " << v.bytes << " 字节  " << shortId(v.sha256) << "\n";
        std::cout << "  契约：" << v.contract.kind << "，输入 " << v.contract.inputWidth << "×" << v.contract.inputHeight << "×"
                  << v.contract.inputChannels << "，输出 " << v.contract.output << "\n";
        for (const auto& [name, value] : v.contract.numbers) std::cout << "    " << name << " = " << value << "\n";
        if (v.provenance.kind == "trained") {
            std::cout << "  来源：训练任务 " << v.provenance.trainingJobId << "，数据集版本 " << v.provenance.datasetVersionId << "，划分 "
                      << v.provenance.split << "，训练后端 " << v.provenance.backend << "\n";
        } else {
            std::cout << "  来源：手工导入\n";
        }
        return 0;
    }
    if (sub == "publish" && args.size() >= 3) {
        const auto st = app.models.publish(args[2]);
        return st ? 0 : fail(st.error());
    }
    std::cerr << "未知的 model 命令\n";
    return 1;
}

}  // namespace vision::cli
