#pragma once

// HTTP 客户端搬到了 adapters/http（训练服务也要用它）。这里保留原来的名字，大模型顾问的代码不用改。

#include "vision/http/http_client.h"

namespace vision::llm {

using http::CurlCommandHttpClient;
using http::HttpClient;
using http::HttpRequest;
using http::HttpResponse;

}  // namespace vision::llm
