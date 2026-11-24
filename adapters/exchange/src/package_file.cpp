#include "vision/exchange/package_file.h"

#include <cstring>
#include <fstream>

#include <nlohmann/json.hpp>

#include "vision/common/path.h"
#include "vision/persistence/json_repositories.h"

namespace vision::exchange {

using nlohmann::json;
using release::PackageFile;
using release::PackageManifest;

namespace {

// ============ tar（ustar）：只实现我们用到的那一点 ============
// 每个文件：一个 512 字节的头，后面是内容，补零到 512 的整数倍。结尾是两个全零的块。

constexpr std::size_t kBlock = 512;

void writeOctal(char* field, std::size_t width, unsigned long long value) {
    // width - 1 位八进制数字，最后一个字节是 '\0'
    for (std::size_t i = width - 1; i-- > 0;) {
        field[i] = static_cast<char>('0' + (value & 7));
        value >>= 3;
    }
    field[width - 1] = '\0';
}

Status appendEntry(std::ostream& out, const std::string& name, const unsigned char* data, std::size_t size) {
    if (name.size() > 99) return Status::failure("exchange.name_too_long", "包里的文件名太长：" + name);
    char header[kBlock] = {};
    std::memcpy(header, name.data(), name.size());
    writeOctal(header + 100, 8, 0644);        // 权限
    writeOctal(header + 108, 8, 0);           // 用户
    writeOctal(header + 116, 8, 0);           // 组
    writeOctal(header + 124, 12, size);       // 大小（11 位八进制：最大约 8 GB）
    writeOctal(header + 136, 12, 0);          // 修改时间：固定写 0，同样的内容导出的包字节也相同
    header[156] = '0';                        // 普通文件
    std::memcpy(header + 257, "ustar", 6);    // 格式标记
    header[263] = '0';
    header[264] = '0';
    // 校验和：计算时把校验和那 8 个字节当作空格
    std::memset(header + 148, ' ', 8);
    unsigned sum = 0;
    for (const char c : header) sum += static_cast<unsigned char>(c);
    writeOctal(header + 148, 7, sum);
    header[155] = ' ';

    out.write(header, kBlock);
    out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    static const char zeros[kBlock] = {};
    if (const std::size_t rest = size % kBlock; rest != 0) out.write(zeros, static_cast<std::streamsize>(kBlock - rest));
    return out ? Status::success() : Status::failure("exchange.write", "写方案包文件失败（磁盘满了？）");
}

// 读一个 tar 文件：文件名 → 内容。任何不对劲的地方都返回失败
Result<std::map<std::string, std::vector<unsigned char>>> readEntries(std::istream& in) {
    using R = Result<std::map<std::string, std::vector<unsigned char>>>;
    const auto corrupt = [](const std::string& why) { return R::failure("exchange.corrupt", "方案包文件损坏：" + why); };
    std::map<std::string, std::vector<unsigned char>> entries;
    for (;;) {
        char header[kBlock];
        in.read(header, kBlock);
        if (in.gcount() == 0 && in.eof()) return corrupt("文件不完整（没有结尾标记）");
        if (in.gcount() != static_cast<std::streamsize>(kBlock)) return corrupt("文件不完整");
        bool allZero = true;
        for (const char c : header) allZero = allZero && c == 0;
        if (allZero) return entries;  // 结尾标记

        if (std::memcmp(header + 257, "ustar", 5) != 0) return corrupt("不是方案包的格式");
        // 头的校验和
        unsigned expected = 0;
        for (int i = 0; i < 8; ++i) {
            const char c = header[148 + i];
            if (c >= '0' && c <= '7') expected = expected * 8 + static_cast<unsigned>(c - '0');
        }
        unsigned sum = 0;
        for (std::size_t i = 0; i < kBlock; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<unsigned char>(header[i]);
        if (sum != expected) return corrupt("文件头的校验和不对");

        std::string name(header, strnlen(header, 100));
        unsigned long long size = 0;
        for (int i = 0; i < 11; ++i) {
            const char c = header[124 + i];
            if (c < '0' || c > '7') return corrupt("文件头里的大小不合法");
            size = size * 8 + static_cast<unsigned long long>(c - '0');
        }
        // 文件名只允许我们自己会写的那几种：不接受带路径穿越的名字
        const bool known = name == "manifest.json" || name == "recipe-version.json" ||
                           (name.rfind("content/", 0) == 0 && name.find('/', 8) == std::string::npos && name.find("..") == std::string::npos);
        if (header[156] != '0' && header[156] != '\0') return corrupt("包里有不是普通文件的东西");
        if (!known) return corrupt("包里有不认识的文件：" + name);
        if (entries.count(name) != 0) return corrupt("包里有重名的文件：" + name);
        if (size > 4ull * 1024 * 1024 * 1024) return corrupt("包里的文件太大");

        std::vector<unsigned char> data(static_cast<std::size_t>(size));
        in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
        if (static_cast<unsigned long long>(in.gcount()) != size) return corrupt("文件不完整（" + name + " 被截断了）");
        if (const auto rest = size % kBlock; rest != 0) in.ignore(static_cast<std::streamsize>(kBlock - rest));
        entries[name] = std::move(data);
    }
}

// ============ 清单 ↔ JSON ============

json toJson(const PackageManifest& m) {
    json operators = json::array(), templates = json::array(), models = json::array();
    for (const auto& [id, major] : m.operators) operators.push_back({{"id", id}, {"major", major}});
    for (const auto& t : m.templates) templates.push_back({{"id", t.id}, {"name", t.name}, {"sha256", t.sha256}, {"bytes", t.bytes}});
    for (const auto& x : m.models) {
        models.push_back({{"id", x.id}, {"sha256", x.sha256}, {"bytes", x.bytes},
                          {"contract", {{"kind", x.kind}, {"output", x.output}, {"inputWidth", x.inputWidth}, {"inputHeight", x.inputHeight},
                                        {"inputChannels", x.inputChannels}, {"numbers", x.numbers}}},
                          {"provenance", {{"kind", x.provenanceKind}, {"trainingJobId", x.trainingJobId},
                                          {"datasetVersionId", x.datasetVersionId}, {"split", x.split}, {"backend", x.backend}}}});
    }
    return {{"schema", m.schema}, {"packageId", m.packageId},
            {"recipeVersion", {{"id", m.recipeVersionId}, {"name", m.recipeName}, {"sha256", m.recipeSha256}}},
            {"operators", std::move(operators)}, {"templates", std::move(templates)}, {"models", std::move(models)}};
}

PackageManifest manifestFrom(const json& j) {  // 字段缺失、类型不对时抛异常，由调用的地方接住
    PackageManifest m;
    m.schema = j.at("schema").get<std::string>();
    m.packageId = j.at("packageId").get<std::string>();
    m.recipeVersionId = j.at("recipeVersion").at("id").get<std::string>();
    m.recipeName = j.at("recipeVersion").at("name").get<std::string>();
    m.recipeSha256 = j.at("recipeVersion").at("sha256").get<std::string>();
    for (const auto& o : j.at("operators")) m.operators.emplace_back(o.at("id").get<std::string>(), o.at("major").get<int>());
    for (const auto& t : j.at("templates")) {
        m.templates.push_back({t.at("id").get<std::string>(), t.at("name").get<std::string>(), t.at("sha256").get<std::string>(),
                               t.at("bytes").get<long long>()});
    }
    for (const auto& x : j.at("models")) {
        release::PackageModelEntry e;
        e.id = x.at("id").get<std::string>();
        e.sha256 = x.at("sha256").get<std::string>();
        e.bytes = x.at("bytes").get<long long>();
        const auto& c = x.at("contract");
        e.kind = c.at("kind").get<std::string>();
        e.output = c.at("output").get<std::string>();
        e.inputWidth = c.at("inputWidth").get<int>();
        e.inputHeight = c.at("inputHeight").get<int>();
        e.inputChannels = c.at("inputChannels").get<int>();
        e.numbers = c.at("numbers").get<std::map<std::string, double>>();
        const auto& p = x.at("provenance");
        e.provenanceKind = p.at("kind").get<std::string>();
        e.trainingJobId = p.at("trainingJobId").get<std::string>();
        e.datasetVersionId = p.at("datasetVersionId").get<std::string>();
        e.split = p.at("split").get<std::string>();
        e.backend = p.at("backend").get<std::string>();
        m.models.push_back(std::move(e));
    }
    return m;
}

bool isSha256(const std::string& s) {
    return s.size() == 64 && s.find_first_not_of("0123456789abcdef") == std::string::npos;
}

std::string hashOf(const std::string& text) {
    return content::sha256Hex(reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

// 方案文档写成文字。两台机器上对同一份文档必须得到一样的文字（哈希靠它），所以固定不缩进
std::string documentText(const authoring::RecipeVersionDocument& recipe) { return persistence::toJson(recipe, -1); }

}  // namespace

std::string TarPackageExchange::fingerprint(const authoring::RecipeVersionDocument& recipe) const { return hashOf(documentText(recipe)); }

Status TarPackageExchange::write(const std::string& filePath, const PackageFile& package) {
    const std::filesystem::path target = pathFromUtf8(filePath);
    // 先写到旁边的临时文件，写完再改名：不会留下一个写了一半、看起来像方案包的文件
    std::filesystem::path temp = target;
    temp += ".part";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return Status::failure("exchange.write", "无法创建文件 " + filePath);
        const std::string manifest = toJson(package.manifest).dump(2);
        const std::string recipe = documentText(package.recipe);
        if (auto st = appendEntry(out, "manifest.json", reinterpret_cast<const unsigned char*>(manifest.data()), manifest.size()); !st) return st;
        if (auto st = appendEntry(out, "recipe-version.json", reinterpret_cast<const unsigned char*>(recipe.data()), recipe.size()); !st) return st;
        for (const auto& [sha256, bytes] : package.contents) {  // map 是按键排序的：顺序固定
            if (!isSha256(sha256)) return Status::failure("exchange.bad_hash", "内容的哈希不合法");
            if (auto st = appendEntry(out, "content/" + sha256, bytes.data(), bytes.size()); !st) return st;
        }
        const char end[kBlock * 2] = {};
        out.write(end, sizeof end);
        out.flush();
        if (!out) return Status::failure("exchange.write", "写方案包文件失败（磁盘满了？）");
    }
    std::error_code ec;
    std::filesystem::rename(temp, target, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        return Status::failure("exchange.write", "无法写入 " + filePath);
    }
    return Status::success();
}

Result<PackageFile> TarPackageExchange::read(const std::string& filePath) {
    using R = Result<PackageFile>;
    std::ifstream in(pathFromUtf8(filePath), std::ios::binary);
    if (!in) return R::failure("exchange.read", "无法打开方案包文件 " + filePath);
    auto entries = readEntries(in);
    if (!entries) return entries.error();
    auto& files = entries.value();
    if (files.count("manifest.json") == 0 || files.count("recipe-version.json") == 0) {
        return R::failure("exchange.corrupt", "方案包文件损坏：缺少清单或方案版本");
    }

    PackageFile package;
    try {
        const auto& raw = files.at("manifest.json");
        package.manifest = manifestFrom(json::parse(raw.begin(), raw.end()));
    } catch (const std::exception&) {
        return R::failure("exchange.corrupt", "方案包文件损坏：清单看不懂");
    }
    // 方案文档：先核对哈希，再解析
    const std::string recipeText(files.at("recipe-version.json").begin(), files.at("recipe-version.json").end());
    if (hashOf(recipeText) != package.manifest.recipeSha256) {
        return R::failure("exchange.corrupt", "方案包文件损坏：方案版本的内容和清单里记的不一致");
    }
    auto recipe = persistence::recipeDocumentFromJson(recipeText);
    if (!recipe) return R::failure("exchange.corrupt", "方案包文件损坏：方案版本看不懂");
    package.recipe = std::move(recipe).value();

    // 每一份内容：文件名就是它应有的哈希，核对一遍
    for (auto& [name, bytes] : files) {
        if (name.rfind("content/", 0) != 0) continue;
        const std::string sha256 = name.substr(8);
        if (!isSha256(sha256) || content::sha256Hex(bytes) != sha256) {
            return R::failure("exchange.corrupt", "方案包文件损坏：内容 " + sha256.substr(0, 12) + "… 和它的校验和不一致");
        }
        package.contents[sha256] = std::move(bytes);
    }
    return package;
}

// ---------------- 端口的桥接 ----------------

std::optional<release::OperatorFacts> CatalogOperatorInventory::find(const std::string& operatorId) const {
    const auto meta = catalog_.find(operatorId);
    if (!meta) return std::nullopt;
    release::OperatorFacts facts;
    facts.majorVersion = meta->signature.majorVersion;
    for (const auto& p : meta->signature.params) {
        if (p.type == ParamType::Model) facts.modelParams.push_back(p.name);
    }
    return facts;
}

Result<release::ArchivedModel> RepositoryModelArchive::find(const std::string& modelVersionId) {
    auto view = models_.getModelVersion(modelVersionId);
    if (!view) return view.error();
    const auto& v = view.value();
    release::ArchivedModel m;
    m.published = v.status == "published";
    m.entry.id = v.id;
    m.entry.sha256 = v.sha256;
    m.entry.bytes = v.bytes;
    m.entry.kind = v.contract.kind;
    m.entry.output = v.contract.output;
    m.entry.inputWidth = v.contract.inputWidth;
    m.entry.inputHeight = v.contract.inputHeight;
    m.entry.inputChannels = v.contract.inputChannels;
    m.entry.numbers = v.contract.numbers;
    m.entry.provenanceKind = v.provenance.kind;
    m.entry.trainingJobId = v.provenance.trainingJobId;
    m.entry.datasetVersionId = v.provenance.datasetVersionId;
    m.entry.split = v.provenance.split;
    m.entry.backend = v.provenance.backend;
    return m;
}

Status RepositoryModelArchive::add(const release::PackageModelEntry& e, const std::vector<unsigned char>& bytes) {
    models::ContractView contract{e.kind, e.inputWidth, e.inputHeight, e.inputChannels, e.output, e.numbers};
    models::ProvenanceView provenance{e.provenanceKind, e.trainingJobId, e.datasetVersionId, e.split, e.backend};
    return models_.adoptModel(e.id, bytes, contract, provenance);
}

Status StoreContentArchive::put(const std::vector<unsigned char>& bytes, const std::string& expectedSha256) {
    auto stored = store_.put(bytes);
    if (!stored) return stored.error();
    if (stored.value() != expectedSha256) return Status::failure("exchange.corrupt", "内容和它的校验和不一致");
    return Status::success();
}

}  // namespace vision::exchange
