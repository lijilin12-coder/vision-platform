# BC-4 上线管理（核心域）。目前只有 POC-9 需要的最小部分：上线包的创建、撤回、内容查询。
vision_domain_library(release SOURCES
  domain/src/release.cpp)

# 依赖 authoring 只为了发布语言 RecipeVersionDocument
vision_application_library(release
  SOURCES application/src/release_service.cpp
  DEPENDS authoring)
