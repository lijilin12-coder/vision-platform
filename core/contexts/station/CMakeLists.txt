# BC-5 工位运行（核心域）。POC-9：启动、部署、热切换、回滚，驱动检测。
vision_domain_library(station SOURCES
  domain/src/station.cpp)

# 工位只认上线包（BC-4 的发布语言），用检测引擎（BC-3）执行检测。它不依赖方案编排的服务。
vision_application_library(station
  SOURCES application/src/station_service.cpp
  DEPENDS release execution)
