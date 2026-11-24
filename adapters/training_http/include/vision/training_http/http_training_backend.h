#pragma once

#include <string>

#include "vision/http/http_client.h"
#include "vision/training/domain/training_job.h"

namespace vision::training_http {

// 训练后端的一个实现：通过 HTTP 调我们自己的训练服务（services/training，Python + PyTorch）。
//
//   start  → POST   <地址>/jobs            请求体：训练配置 + 每个样本的图像文件路径、标签、划到哪一部分
//   poll   → GET    <地址>/jobs/<编号>
//   cancel → DELETE <地址>/jobs/<编号>
//
// 图像和训练出来的模型都以【文件路径】交接，不经过 HTTP 传输内容：
// 平台和训练服务必须看得到同一份文件（同一台机器；或者 Docker 把数据目录挂载到相同的路径）。
//
// 训练服务的回复是外部输入：字段缺失、类型不对都按“失败”处理，不会让平台崩溃。
class HttpTrainingBackend final : public training::TrainingBackend {
public:
    // baseUrl 例如 "http://127.0.0.1:8765"
    HttpTrainingBackend(http::HttpClient& http, std::string baseUrl);

    std::string name() const override { return "training-service " + baseUrl_; }
    Result<std::string> start(const training::TrainingRequest& request) override;
    Result<training::BackendProgress> poll(const std::string& ticket) override;
    Status cancel(const std::string& ticket) override;

private:
    http::HttpClient& http_;
    std::string baseUrl_;
};

}  // namespace vision::training_http
