// vision-cli 的 sample / dataset 两组命令（BC-9 数据，训练闭环需要的最小部分）。

#include "cli_app.h"
#include "vision/common/path.h"

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

}  // namespace

int runSampleCommand(App& app, const std::vector<std::string>& args) {
    const std::string& sub = args[1];
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

}  // namespace vision::cli
