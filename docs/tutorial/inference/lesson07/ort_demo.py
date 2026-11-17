"""用 ONNX Runtime 看三件事：图优化把模型改成了什么样、用的是哪个 execution provider、第一次推理有多慢。

这个脚本不在自动构建里。运行前先安装：

    pip install onnx onnxruntime numpy

然后：

    python ort_demo.py

它自己构造一个很小的模型（卷积 → 批归一化 → ReLU → 全局平均池化 → 全连接），不需要下载任何文件。
生成的 .onnx 文件放在系统的临时目录里。
"""
import os
import tempfile
import time

import numpy as np
import onnx
import onnxruntime as ort
from onnx import TensorProto, helper, numpy_helper


def build_model(path):
    rng = np.random.default_rng(0)

    def const(name, array):
        return numpy_helper.from_array(array.astype(np.float32), name)

    initializers = [
        const("conv_w", rng.standard_normal((16, 3, 3, 3))),
        const("bn_scale", np.ones(16)),
        const("bn_bias", np.zeros(16)),
        const("bn_mean", rng.standard_normal(16) * 0.1),
        const("bn_var", np.ones(16)),
        const("fc_w", rng.standard_normal((10, 16))),
        const("fc_b", np.zeros(10)),
    ]
    nodes = [
        helper.make_node("Conv", ["image", "conv_w"], ["conv"], pads=[1, 1, 1, 1]),
        helper.make_node("BatchNormalization", ["conv", "bn_scale", "bn_bias", "bn_mean", "bn_var"], ["bn"]),
        helper.make_node("Relu", ["bn"], ["relu"]),
        helper.make_node("GlobalAveragePool", ["relu"], ["pool"]),
        helper.make_node("Flatten", ["pool"], ["flat"]),
        helper.make_node("Gemm", ["flat", "fc_w", "fc_b"], ["scores"], transB=1),
    ]
    graph = helper.make_graph(
        nodes,
        "tiny",
        [helper.make_tensor_value_info("image", TensorProto.FLOAT, [1, 3, 224, 224])],
        [helper.make_tensor_value_info("scores", TensorProto.FLOAT, [1, 10])],
        initializers,
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    model.ir_version = 9
    onnx.checker.check_model(model)
    onnx.save(model, path)


def main():
    folder = tempfile.mkdtemp(prefix="ort_demo_")
    model_path = os.path.join(folder, "tiny.onnx")
    build_model(model_path)
    image = np.random.default_rng(1).standard_normal((1, 3, 224, 224)).astype(np.float32)

    print("onnxruntime", ort.__version__)
    print("这台机器上可用的 execution provider：", ort.get_available_providers())
    print()

    # 1. 不同的图优化级别，把模型改成了什么样
    levels = [
        ("不优化", ort.GraphOptimizationLevel.ORT_DISABLE_ALL),
        ("基本", ort.GraphOptimizationLevel.ORT_ENABLE_BASIC),
        ("扩展", ort.GraphOptimizationLevel.ORT_ENABLE_EXTENDED),
    ]
    reference = None
    for name, level in levels:
        options = ort.SessionOptions()
        options.graph_optimization_level = level
        options.optimized_model_filepath = os.path.join(folder, "optimized.onnx")  # 把优化后的模型存下来看
        session = ort.InferenceSession(model_path, options, providers=["CPUExecutionProvider"])
        scores = session.run(None, {"image": image})[0]
        if reference is None:
            reference = scores
        optimized = onnx.load(options.optimized_model_filepath)
        print(f"{name}：", " → ".join(node.op_type for node in optimized.graph.node))
        print(f"      和不优化时的输出相比，最大误差 {np.abs(scores - reference).max():.1e}")
    print()

    # 2. 第一次推理和之后的推理
    session = ort.InferenceSession(model_path, providers=["CPUExecutionProvider"])
    times = []
    for _ in range(5):
        start = time.perf_counter()
        session.run(None, {"image": image})
        times.append((time.perf_counter() - start) * 1000)
    print("连续推理 5 次的耗时（毫秒）：", " ".join(f"{t:.2f}" for t in times))


if __name__ == "__main__":
    main()
