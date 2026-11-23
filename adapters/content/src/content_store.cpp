#include "vision/content/content_store.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <opencv2/imgcodecs.hpp>

#include "vision/common/id.h"
#include "vision/common/path.h"
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

}  // namespace vision::content
