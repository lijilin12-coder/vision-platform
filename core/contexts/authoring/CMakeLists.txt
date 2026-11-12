# BC-2 方案编排（核心域）
vision_domain_library(authoring SOURCES
  domain/src/recipe.cpp
  domain/src/recipe_version.cpp)

vision_application_library(authoring
  SOURCES application/src/authoring_service.cpp
  DEPENDS catalog)
