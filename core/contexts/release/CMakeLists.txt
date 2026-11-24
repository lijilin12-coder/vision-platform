# BC-4 上线管理（核心域）。目前是最小实现：
#   上线包的创建、撤回、内容查询（POC-9）；基准图集和评估规则（POC-11）。
vision_domain_library(release SOURCES
  domain/src/release.cpp
  domain/src/golden_set.cpp
  domain/src/recipe_package.cpp)

# 依赖 authoring 只为了发布语言 RecipeVersionDocument
vision_application_library(release
  SOURCES application/src/release_service.cpp application/src/golden_set_service.cpp application/src/package_service.cpp
  DEPENDS authoring)
