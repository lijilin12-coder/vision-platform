# 训练服务的镜像（只用 CPU）。
#
#   docker build -t vision-training services/training
#   docker run --rm -p 8765:8765 -v /绝对路径/vision-data:/绝对路径/vision-data vision-training \
#       --work-dir /绝对路径/vision-data/training-work
#
# 注意 -v 两边的路径必须相同：平台把图像的【文件路径】交给训练服务，训练服务也把模型的文件路径交回来，
# 两边看到的路径要一样。所以 --work-dir（训练出来的模型放在哪里）也要指到挂载的目录里。
#
# 下载慢的时候可以换镜像源，例如：
#   docker build -t vision-training services/training \
#       --build-arg BASE=<镜像仓库>/library/python:3.12-slim \
#       --build-arg PIP_INDEX=https://pypi.tuna.tsinghua.edu.cn/simple
ARG BASE=python:3.12-slim
FROM ${BASE}

# 只含 CPU 的 PyTorch（比默认的小得多）。想用别的来源时改这个参数；设成和 PIP_INDEX 一样就是从 PyPI 装（很大）。
ARG TORCH_INDEX=https://download.pytorch.org/whl/cpu
ARG PIP_INDEX=https://pypi.org/simple

WORKDIR /app
RUN pip install --no-cache-dir torch --index-url ${TORCH_INDEX} \
 && pip install --no-cache-dir numpy opencv-python-headless onnx --index-url ${PIP_INDEX}
COPY trainer.py server.py ./

EXPOSE 8765
ENTRYPOINT ["python", "server.py", "--host", "0.0.0.0", "--port", "8765"]
CMD ["--work-dir", "/work"]
