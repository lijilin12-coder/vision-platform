#include "vision/content/content_store.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <opencv2/imgcodecs.hpp>

#include "vision/common/id.h"
#include "vision/common/path.h"
#include "vision/common/time.h"
#include "vision/imaging/cv_bridge.h"

namespace vision::content {

namespace fs = std::filesystem;

// ---------------- SHA-256（FIPS 180-4）----------------
// 自己实现，是为了不给三个平台再加一个加密库的依赖。只用来识别内容，不用于任何安全用途。
// 测试里用标准文档给出的几组结果核对过。

namespace {

const std::uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void compress(std::uint32_t state[8], const unsigned char block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (std::uint32_t(block[i * 4]) << 24) | (std::uint32_t(block[i * 4 + 1]) << 16) |
               (std::uint32_t(block[i * 4 + 2]) << 8) | std::uint32_t(block[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        const std::uint32_t s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const std::uint32_t ch = (e & f) ^ (~e & g);
        const std::uint32_t t1 = h + s1 + ch + kRound[i] + w[i];
        const std::uint32_t s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const std::uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e; state[5] += f; state[6] += g; state[7] += h;
}

bool looksLikeSha256(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

}  // namespace

std::string sha256Hex(const unsigned char* data, std::size_t size) {
    std::uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::size_t offset = 0;
    for (; offset + 64 <= size; offset += 64) compress(state, data + offset);

    // 最后一块：剩下的字节 + 一个 0x80 + 若干个 0 + 8 个字节的总长度（按位计）
    unsigned char tail[128] = {0};
    const std::size_t rest = size - offset;
    if (rest > 0) std::memcpy(tail, data + offset, rest);
    tail[rest] = 0x80;
    const std::size_t tailSize = rest + 9 <= 64 ? 64 : 128;
    const std::uint64_t bits = static_cast<std::uint64_t>(size) * 8;
    for (int i = 0; i < 8; ++i) tail[tailSize - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
    compress(state, tail);
    if (tailSize == 128) compress(state, tail + 64);

    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint32_t word : state) {
        for (int shift = 28; shift >= 0; shift -= 4) out += kHex[(word >> shift) & 0xf];
    }
    return out;
}

// ---------------- 文件内容仓库 ----------------

FileContentStore::FileContentStore(fs::path root) : dir_(std::move(root) / "content" / "sha256") {}

fs::path FileContentStore::pathOf(const std::string& sha256) const { return dir_ / sha256.substr(0, 2) / sha256.substr(2); }

bool FileContentStore::contains(const std::string& sha256) const {
    std::error_code ec;
    return looksLikeSha256(sha256) && fs::exists(pathOf(sha256), ec);
}

Result<std::string> FileContentStore::put(const std::vector<unsigned char>& bytes) {
    using R = Result<std::string>;
    if (bytes.empty()) return R::failure("content.empty", "不能保存空的内容");
    const std::string hash = sha256Hex(bytes);
    const fs::path target = pathOf(hash);
    std::error_code ec;
    if (fs::exists(target, ec)) return hash;  // 同样的内容已经在了：什么都不用做

    fs::create_directories(target.parent_path(), ec);
    // 先写临时文件再改名：写到一半时程序崩溃，不会留下一个内容不全、名字却是完整哈希的文件。
    // 临时文件名带随机数：两个进程同时保存同一份内容也不会互相覆盖到一半。
    fs::path temp = target;
    temp += "." + newUuid() + ".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return R::failure("content.write", "无法写入 " + pathToUtf8(temp));
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) return R::failure("content.write", "写入失败 " + pathToUtf8(temp));
    }
    fs::rename(temp, target, ec);
    if (ec) {
        fs::remove(temp, ec);
        if (!fs::exists(target, ec)) return R::failure("content.write", "无法保存内容 " + hash);
    }
    return hash;
}

Result<std::vector<unsigned char>> FileContentStore::get(const std::string& sha256) const {
    using R = Result<std::vector<unsigned char>>;
    if (!looksLikeSha256(sha256)) return R::failure("content.bad_hash", "不是合法的内容哈希：" + sha256);
    std::ifstream in(pathOf(sha256), std::ios::binary);
    if (!in) return R::failure("content.not_found", "内容仓库里没有 " + sha256.substr(0, 12) + "…");
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // 校验：文件名说它的哈希是这个，实际算出来必须也是这个
    if (sha256Hex(bytes) != sha256) {
        return R::failure("content.corrupt", "内容 " + sha256.substr(0, 12) + "… 已经损坏或被改动（哈希对不上）");
    }
    return bytes;
}

// ---------------- 检测时的图像 ----------------

namespace {

// 以前存在内容仓库里的检测图像：编号是 64 位十六进制的内容哈希
bool isLegacyImageId(const std::string& id) { return id.size() == 64 && id.find_first_not_of("0123456789abcdef") == std::string::npos; }

// 记录的编号 "1795521600000-000042" → 它的时间（毫秒）。不是这种样子的返回 -1
std::int64_t timeOfRecordId(const std::string& id) {
    if (id.size() < 14 || id[13] != '-' || id.find_first_not_of("0123456789") != 13) return -1;
    if (id.find_first_not_of("0123456789", 14) != std::string::npos) return -1;
    return std::stoll(id.substr(0, 13));
}

// 工位 ID 要拿来当目录名：不能带路径分隔符，不能是 . 或 ..
bool safeName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:") == std::string::npos;
}

int removeFile(const fs::path& file) {
    std::error_code ec;
    return fs::remove(file, ec) ? 1 : 0;
}

// 删掉一个目录，返回里面有多少个文件
int removeDirectory(const fs::path& dir) {
    std::error_code ec;
    int files = 0;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) ++files;
    fs::remove_all(dir, ec);
    return files;
}

}  // namespace

InspectionImageFiles::InspectionImageFiles(const fs::path& root, FileContentStore& legacy)
    : dir_(root / "inspection-images"), legacy_(legacy) {}

fs::path InspectionImageFiles::keptFile(const std::string& stationId, const std::string& imageId) const {
    return dir_ / pathFromUtf8(stationId) / "kept" / (imageId + ".png");
}

fs::path InspectionImageFiles::cachedFile(const std::string& stationId, const std::string& imageId) const {
    return dir_ / pathFromUtf8(stationId) / "cache" / utcDay(timeOfRecordId(imageId)) / (imageId + ".png");
}

Result<std::string> InspectionImageFiles::store(const std::string& stationId, const std::string& recordId, const Image& image,
                                                bool longTerm) {
    using R = Result<std::string>;
    if (!safeName(stationId) || timeOfRecordId(recordId) < 0) return R::failure("content.bad_id", "工位或记录的编号不能用作文件名");
    std::vector<unsigned char> png;
    try {
        if (!cv::imencode(".png", imaging::view(image), png)) return R::failure("content.encode", "图像编码失败");
    } catch (const cv::Exception& e) {
        return R::failure("content.encode", std::string("图像编码失败：") + e.what());
    }
    const fs::path file = longTerm ? keptFile(stationId, recordId) : cachedFile(stationId, recordId);
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    out.flush();
    if (!out) {
        out.close();
        fs::remove(file, ec);  // 写了一半的不留：留着会被当成一幅完整的图像
        return R::failure("content.write", "检测图像写不进去（磁盘满了？）");
    }
    return recordId;  // 图像的编号就是记录的编号
}

std::string InspectionImageFiles::state(const std::string& stationId, const std::string& imageId) {
    std::error_code ec;
    if (isLegacyImageId(imageId)) return legacy_.get(imageId) ? "kept" : "expired";
    if (!safeName(stationId) || timeOfRecordId(imageId) < 0) return "expired";
    if (fs::exists(keptFile(stationId, imageId), ec)) return "kept";
    if (fs::exists(cachedFile(stationId, imageId), ec)) return "cached";
    return "expired";
}

Result<std::vector<unsigned char>> InspectionImageFiles::load(const std::string& stationId, const std::string& imageId) {
    using R = Result<std::vector<unsigned char>>;
    if (isLegacyImageId(imageId)) return legacy_.get(imageId);
    if (!safeName(stationId) || timeOfRecordId(imageId) < 0) return R::failure("content.not_found", "找不到这幅检测图像");
    for (const auto& file : {keptFile(stationId, imageId), cachedFile(stationId, imageId)}) {
        std::ifstream in(file, std::ios::binary);
        if (!in) continue;
        std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (!bytes.empty()) return bytes;
    }
    return R::failure("content.not_found", "找不到这幅检测图像（可能已经过了保留期）");
}

Status InspectionImageFiles::keep(const std::string& stationId, const std::string& imageId) {
    if (isLegacyImageId(imageId) || !safeName(stationId) || timeOfRecordId(imageId) < 0) return Status::success();
    const fs::path from = cachedFile(stationId, imageId), to = keptFile(stationId, imageId);
    std::error_code ec;
    if (!fs::exists(from, ec)) return Status::success();  // 已经是长期的，或者已经过期：没什么可做的
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);  // 同一个磁盘上的改名：要么成了，要么没动，不会出现半个文件
    if (ec) return Status::failure("content.write", "图像转成长期保留时失败：" + ec.message());
    return Status::success();
}

quality::ImageCleanup InspectionImageFiles::cleanup(std::int64_t cutoffMs, std::uint64_t minFreeBytes) {
    quality::ImageCleanup done;
    std::error_code ec;
    // 所有工位的缓存目录：（日期，目录）
    std::vector<std::pair<std::string, fs::path>> days;
    for (fs::directory_iterator station(dir_, ec), end; !ec && station != end; station.increment(ec)) {
        std::error_code inner;
        for (fs::directory_iterator day(station->path() / "cache", inner), dayEnd; !inner && day != dayEnd; day.increment(inner)) {
            if (day->is_directory(inner)) days.emplace_back(pathToUtf8(day->path().filename()), day->path());
        }
    }
    std::sort(days.begin(), days.end());  // 日期是 "2026-11-25" 这样的：按文字排就是按时间排

    // 第一步：过期的。整天都过期的目录一起删；跨着界线的那一天逐个文件看
    const std::string cutoffDay = utcDay(cutoffMs);
    std::vector<std::pair<std::string, fs::path>> remaining;
    for (const auto& [day, path] : days) {
        if (day < cutoffDay) {
            done.expired += removeDirectory(path);
        } else if (day == cutoffDay) {
            int left = 0;
            std::error_code inner;
            for (fs::directory_iterator file(path, inner), fileEnd; !inner && file != fileEnd; file.increment(inner)) {
                const std::int64_t time = timeOfRecordId(pathToUtf8(file->path().stem()));
                if (time >= 0 && time < cutoffMs) {
                    done.expired += removeFile(file->path());
                } else {
                    ++left;
                }
            }
            if (left == 0) {
                fs::remove(path, inner);
            } else {
                remaining.emplace_back(day, path);
            }
        } else {
            remaining.emplace_back(day, path);
        }
    }

    // 第二步：磁盘快满了。从最旧的一天开始再删，直到剩余空间够了，或者缓存清空了。
    // 检测比“事后能看图”要紧：不能因为留图把磁盘写满、让检测记录都写不进去
    if (minFreeBytes == 0) return done;
    const auto low = [&] {
        const auto space = fs::space(dir_, ec);
        return !ec && space.available < minFreeBytes;
    };
    fs::create_directories(dir_, ec);  // 目录还不存在时 space 会失败
    for (const auto& [day, path] : remaining) {
        if (!low()) return done;
        done.forSpace += removeDirectory(path);
    }
    done.spaceLow = low();
    return done;
}

// ---------------- 匹配模板 ----------------

Result<authoring::StoredContent> TemplateContent::storeCrop(const Image& source, int x, int y, int width, int height) {
    using R = Result<authoring::StoredContent>;
    const cv::Mat full = imaging::view(source);
    const cv::Rect box(x, y, width, height);
    if (width <= 0 || height <= 0 || (box & cv::Rect(0, 0, full.cols, full.rows)) != box) {
        return R::failure("content.template_box", "框选的范围不在样图之内");
    }
    std::vector<unsigned char> png;
    if (!cv::imencode(".png", full(box), png)) return R::failure("content.encode", "模板图像编码失败");
    auto hash = store_.put(png);
    if (!hash) return hash.error();
    return authoring::StoredContent{hash.value(), static_cast<long long>(png.size())};
}

Result<Image> TemplateContent::load(const authoring::RecipeDocumentTemplate& ref) {
    auto bytes = store_.get(ref.sha256);
    if (!bytes) return bytes.error();
    const cv::Mat decoded = cv::imdecode(bytes.value(), cv::IMREAD_UNCHANGED);
    if (decoded.empty()) return Result<Image>::failure("content.decode", "模板图像无法解码");
    return imaging::fromMat(decoded);
}

// ---------------- 样本 ----------------

Result<data::StoredFile> SampleFiles::importFile(const std::string& path) {
    using R = Result<data::StoredFile>;
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) return R::failure("content.read", "无法读取 " + path);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    // 确认它真的是一幅能解码的图像，免得把别的文件当成样本存进来
    if (bytes.empty() || cv::imdecode(bytes, cv::IMREAD_UNCHANGED).empty()) return R::failure("content.not_image", path + " 不是能识别的图像文件");
    auto hash = store_.put(bytes);
    if (!hash) return hash.error();
    return data::StoredFile{hash.value(), static_cast<long long>(bytes.size())};
}

std::string SampleFiles::pathOf(const std::string& sha256) const { return pathToUtf8(store_.pathOf(sha256)); }

// ---------------- 训练闭环 ----------------

Result<models::StoredModelFile> ModelFileStore::importFile(const std::string& path) {
    using R = Result<models::StoredModelFile>;
    std::ifstream in(pathFromUtf8(path), std::ios::binary);
    if (!in) return R::failure("content.read", "无法读取模型文件 " + path);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return R::failure("content.empty", "模型文件是空的：" + path);
    auto hash = store_.put(bytes);
    if (!hash) return hash.error();
    return models::StoredModelFile{hash.value(), static_cast<long long>(bytes.size())};
}

Result<models::StoredModelFile> ModelFileStore::importBytes(const std::vector<unsigned char>& bytes) {
    using R = Result<models::StoredModelFile>;
    if (bytes.empty()) return R::failure("content.empty", "模型文件是空的");
    auto hash = store_.put(bytes);
    if (!hash) return hash.error();
    return models::StoredModelFile{hash.value(), static_cast<long long>(bytes.size())};
}

std::string ModelFileStore::pathOf(const std::string& sha256) const { return pathToUtf8(store_.pathOf(sha256)); }

Result<training::DatasetInfo> DataDatasetSource::load(const std::string& datasetVersionId) {
    auto version = data_.getDatasetVersion(datasetVersionId);
    if (!version) return version.error();
    std::vector<std::string> ids;
    for (const auto& e : version.value().entries) ids.push_back(e.sampleId);
    // T-2：提交时再问一次，这些样本里现在有没有留作基准的
    const auto benchmark = data_.benchmarkSamplesAmong(ids);
    training::DatasetInfo info;
    info.datasetVersionId = version.value().id;
    info.frozen = true;  // 数据上下文里，有版本号的就是已经冻结的；没有冻结的数据集根本取不到版本
    for (const auto& e : version.value().entries) {
        info.samples.push_back({e.sampleId, e.label == "ng", std::find(benchmark.begin(), benchmark.end(), e.sampleId) != benchmark.end(),
                                e.contentPath});
    }
    return info;
}

Result<std::string> RepositoryModelImporter::importTrained(const training::TrainedModel& m) {
    return models_.importModel(m.modelName, m.modelFile, {m.contractKind, m.inputWidth, m.inputHeight, m.inputChannels, m.outputMeaning, m.numbers},
                               {"trained", m.trainingJobId, m.datasetVersionId, m.split, m.backend});
}

}  // namespace vision::content
