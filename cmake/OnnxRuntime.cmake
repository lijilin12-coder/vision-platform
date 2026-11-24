# ONNX Runtime：在 Mac、Windows、Linux 上运行模型的推理引擎（鸿蒙上会换成 MindSpore Lite）。
#
# 用微软官方发布的预编译库，不自己编译（自己编译要一个多小时）。
# 来源是 NuGet 上的 Microsoft.ML.OnnxRuntime 包：它其实是一个 zip，里面同时有头文件和
# Windows、macOS、Linux 三个平台的库，所以三个平台用同一个下载地址、同一个校验值。
#
# 第一次配置时下载（约 120 MB），之后用缓存。
#   - 不想要推理功能：   cmake -DVISION_WITH_ONNXRUNTIME=OFF ...
#   - 下载不了：手工下载下面的 URL，改名为 .zip 解压，然后
#                        cmake -DFETCHCONTENT_SOURCE_DIR_ONNXRUNTIME_NUGET=<解压出来的目录> ...
#
# 提供：
#   目标 onnxruntime::onnxruntime
#   函数 vision_copy_onnxruntime(<可执行目标>)：Windows 上把 DLL 复制到可执行文件旁边（别的平台什么也不做）

set(VISION_ONNXRUNTIME_VERSION 1.22.0)
set(VISION_ONNXRUNTIME_SHA256 d571e63a2329baacb713f441e65ad75284de354db6e1ac435fe4bebbb417986a)

include(FetchContent)
FetchContent_Declare(onnxruntime_nuget
  URL https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime/${VISION_ONNXRUNTIME_VERSION}/microsoft.ml.onnxruntime.${VISION_ONNXRUNTIME_VERSION}.nupkg
  URL_HASH SHA256=${VISION_ONNXRUNTIME_SHA256}
  DOWNLOAD_NAME onnxruntime-${VISION_ONNXRUNTIME_VERSION}.zip  # 扩展名是 .zip，CMake 才知道要解压
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
message(STATUS "ONNX Runtime ${VISION_ONNXRUNTIME_VERSION}：第一次配置时需要下载约 120 MB")
FetchContent_MakeAvailable(onnxruntime_nuget)

# 这台机器对应包里的哪个目录
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
  set(_ort_arch arm64)
else()
  set(_ort_arch x64)
endif()
if(WIN32)
  set(_ort_platform win-${_ort_arch})
elseif(APPLE)
  set(_ort_platform osx-${_ort_arch})
else()
  set(_ort_platform linux-${_ort_arch})
endif()
set(_ort_native ${onnxruntime_nuget_SOURCE_DIR}/runtimes/${_ort_platform}/native)
if(NOT EXISTS ${_ort_native})
  message(FATAL_ERROR "ONNX Runtime 的预编译包里没有 ${_ort_platform} 的库。可以用 -DVISION_WITH_ONNXRUNTIME=OFF 关掉推理功能")
endif()

add_library(onnxruntime::onnxruntime SHARED IMPORTED GLOBAL)
target_include_directories(onnxruntime::onnxruntime INTERFACE ${onnxruntime_nuget_SOURCE_DIR}/build/native/include)

# 包里的库文件名不带版本号，但库自己记录的名字带（程序运行时按后者去找）。所以复制一份，改成它要的名字。
set(_ort_lib_dir ${CMAKE_BINARY_DIR}/onnxruntime/lib)
file(MAKE_DIRECTORY ${_ort_lib_dir})
if(WIN32)
  set(VISION_ONNXRUNTIME_DLL ${_ort_native}/onnxruntime.dll CACHE INTERNAL "")
  set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION ${_ort_native}/onnxruntime.dll
    IMPORTED_IMPLIB ${_ort_native}/onnxruntime.lib)
elseif(APPLE)
  set(_ort_file ${_ort_lib_dir}/libonnxruntime.${VISION_ONNXRUNTIME_VERSION}.dylib)
  configure_file(${_ort_native}/libonnxruntime.dylib ${_ort_file} COPYONLY)
  set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION ${_ort_file}
    IMPORTED_SONAME "@rpath/libonnxruntime.${VISION_ONNXRUNTIME_VERSION}.dylib")
else()
  set(_ort_file ${_ort_lib_dir}/libonnxruntime.so.1)
  configure_file(${_ort_native}/libonnxruntime.so ${_ort_file} COPYONLY)
  set_target_properties(onnxruntime::onnxruntime PROPERTIES
    IMPORTED_LOCATION ${_ort_file}
    IMPORTED_SONAME libonnxruntime.so.1)
endif()

function(vision_copy_onnxruntime target)
  if(WIN32)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different ${VISION_ONNXRUNTIME_DLL} $<TARGET_FILE_DIR:${target}>
      VERBATIM)
  endif()
endfunction()
