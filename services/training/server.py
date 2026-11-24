"""训练服务：一个很小的 HTTP 接口，把 trainer.py 包起来。

  POST   /jobs           提交一次训练，请求体见 README；返回 {"ticket": "..."}
  GET    /jobs/<ticket>  查询：{"state": "running|completed|failed", "progress": 0.4, ...}
  DELETE /jobs/<ticket>  取消
  GET    /health         {"ok": true}

只用到 Python 标准库的 http.server：这是一个内部服务，一次只有平台一个调用者，不需要 Web 框架。
训练在后台线程里跑；同一时间只训练一个任务，其余的排队。

注意：请求里样本的图像是【文件路径】，训练出来的模型也是以【文件路径】交回去的。
所以平台和训练服务必须看得到同一份文件：在同一台机器上，或者用 Docker 时把数据目录挂载到同样的路径。

  python server.py --port 8765 --work-dir ./work
"""

from __future__ import annotations

import argparse
import json
import threading
import traceback
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import trainer

jobs: dict[str, dict] = {}
jobs_lock = threading.Lock()
train_lock = threading.Lock()  # 一次只训练一个
work_dir = Path("work")


def run_job(ticket: str, request: dict) -> None:
    job = jobs[ticket]
    with train_lock:
        if job["cancel"]:
            job.update(state="failed", failure="已取消")
            return
        try:
            result = trainer.train(
                request, work_dir / ticket,
                progress=lambda f: job.__setitem__("progress", f),
                cancelled=lambda: job["cancel"],
            )
            job.update(state="completed", progress=1.0, **result)
        except trainer.Cancelled:
            job.update(state="failed", failure="已取消")
        except ValueError as e:  # 请求本身有问题：原因是给人看的
            job.update(state="failed", failure=str(e))
        except Exception as e:  # noqa: BLE001  其他意外：服务不能因为一次训练出错就停掉
            traceback.print_exc()
            job.update(state="failed", failure=f"训练出错：{type(e).__name__}: {e}")


class Handler(BaseHTTPRequestHandler):
    def reply(self, status: int, body: dict) -> None:
        data = json.dumps(body, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def ticket(self) -> str | None:
        parts = self.path.strip("/").split("/")
        return parts[1] if len(parts) == 2 and parts[0] == "jobs" else None

    def do_GET(self) -> None:  # noqa: N802
        if self.path == "/health":
            return self.reply(200, {"ok": True, "modelKinds": [trainer.MODEL_KIND]})
        ticket = self.ticket()
        with jobs_lock:
            job = jobs.get(ticket or "")
        if job is None:
            return self.reply(404, {"error": "没有这次训练"})
        self.reply(200, {k: v for k, v in job.items() if k != "cancel"})

    def do_POST(self) -> None:  # noqa: N802
        if self.path.rstrip("/") != "/jobs":
            return self.reply(404, {"error": "没有这个地址"})
        try:
            request = json.loads(self.rfile.read(int(self.headers.get("Content-Length", "0"))).decode("utf-8"))
        except Exception:  # noqa: BLE001
            return self.reply(400, {"error": "请求体不是合法的 JSON"})
        kind = request.get("config", {}).get("modelKind")
        if kind != trainer.MODEL_KIND:
            return self.reply(400, {"error": f"这个训练服务只会训练 {trainer.MODEL_KIND}，不认识 {kind!r}"})
        ticket = uuid.uuid4().hex
        with jobs_lock:
            jobs[ticket] = {"state": "running", "progress": 0.0, "failure": "", "cancel": False}
        threading.Thread(target=run_job, args=(ticket, request), daemon=True).start()
        self.reply(200, {"ticket": ticket})

    def do_DELETE(self) -> None:  # noqa: N802
        ticket = self.ticket()
        with jobs_lock:
            job = jobs.get(ticket or "")
        if job is None:
            return self.reply(404, {"error": "没有这次训练"})
        job["cancel"] = True
        self.reply(200, {"ok": True})

    def log_message(self, fmt: str, *args) -> None:  # 默认的访问日志太吵：查询进度每秒一次
        pass


def main() -> None:
    global work_dir
    parser = argparse.ArgumentParser(description="视觉检测平台的训练服务")
    parser.add_argument("--host", default="127.0.0.1", help="监听的地址。默认只接受本机的连接")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--work-dir", default="work", help="训练出来的模型放在这里")
    args = parser.parse_args()
    work_dir = Path(args.work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    print(f"训练服务已启动：http://{args.host}:{args.port}，模型输出到 {work_dir.resolve()}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
