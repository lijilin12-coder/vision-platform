"""训练服务自己的测试。需要先装好 requirements.txt 里的依赖：

  python -m unittest test_service -v

（CI 里不跑这个文件：CI 的机器上没有装 PyTorch。平台一侧对训练服务的调用，在 C++ 的测试里用假的 HTTP 回复覆盖。）
"""

import json
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

import numpy as np
from PIL import Image

import server
import trainer


def make_images(folder: Path, ok: int = 16, ng: int = 4) -> list[dict]:
    """合格的：深色底上一点噪声。不合格的：多一个亮方块。"""
    rng = np.random.default_rng(1)
    samples = []
    for i in range(ok + ng):
        is_ng = i >= ok
        image = rng.normal(80, 3, (64, 64))
        if is_ng:
            x, y = rng.integers(8, 44, 2)
            image[y:y + 10, x:x + 10] = 230
        path = folder / f"{i}.png"
        Image.fromarray(image.clip(0, 255).astype(np.uint8)).save(path)
        split = "test" if i % 4 == 3 else "val" if i % 4 == 2 else "train"
        samples.append({"sampleId": str(i), "ng": is_ng, "split": split, "path": str(path)})
    return samples


def request_for(samples, **settings):
    return {"config": {"modelKind": trainer.MODEL_KIND, "modelName": "t", "settings": {"imageSize": 64, **settings}}, "samples": samples}


class TrainerTest(unittest.TestCase):
    def test_trains_and_exports_a_model_that_separates_ok_from_ng(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = trainer.train(request_for(make_images(Path(tmp)), epochs=30), Path(tmp) / "out")
            metrics = result["report"]["metrics"]
            self.assertEqual(metrics["test_auroc"], 1.0)
            self.assertGreater(metrics["ng_score_min"], metrics["ok_score_max"])
            self.assertTrue(Path(result["output"]["modelFile"]).is_file())
            self.assertEqual(result["output"]["inputWidth"], 64)

    def test_exported_model_gives_the_same_scores_in_onnx_runtime(self):
        try:
            import onnxruntime
        except ImportError:
            self.skipTest("没有安装 onnxruntime")
        with tempfile.TemporaryDirectory() as tmp:
            samples = make_images(Path(tmp))
            result = trainer.train(request_for(samples, epochs=5), Path(tmp) / "out")
            session = onnxruntime.InferenceSession(result["output"]["modelFile"])
            batch = np.stack([trainer.load_image(s["path"], 64) for s in samples[:3]])[:, None]
            score, anomaly_map = session.run(None, {"image": batch})
            self.assertEqual(score.shape, (3,))
            self.assertEqual(anomaly_map.shape, (3, 1, 64, 64))
            np.testing.assert_allclose(score, anomaly_map.max(axis=(1, 2, 3)), rtol=1e-5)

    def test_rejects_bad_requests_with_a_readable_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            samples = make_images(Path(tmp))
            with self.assertRaisesRegex(ValueError, "8 的倍数"):
                trainer.train(request_for(samples, imageSize=100), Path(tmp) / "out")
            with self.assertRaisesRegex(ValueError, "太少"):
                trainer.train(request_for(samples[:2]), Path(tmp) / "out")
            with self.assertRaisesRegex(ValueError, "读不了"):
                trainer.train(request_for(samples + [{"sampleId": "x", "ng": False, "split": "train", "path": "/no/such.png"}]), Path(tmp) / "out")
            with self.assertRaisesRegex(ValueError, "只会训练"):
                trainer.train({"config": {"modelKind": "other"}, "samples": samples}, Path(tmp) / "out")

    def test_stops_when_cancelled(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(trainer.Cancelled):
                trainer.train(request_for(make_images(Path(tmp)), epochs=1000), Path(tmp) / "out", cancelled=lambda: True)


class ServerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        server.work_dir = Path(self.tmp.name) / "work"
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
        self.url = f"http://127.0.0.1:{self.httpd.server_address[1]}"
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def tearDown(self):
        self.httpd.shutdown()
        self.httpd.server_close()
        self.tmp.cleanup()

    def call(self, method, path, body=None):
        data = json.dumps(body).encode() if body is not None else None
        request = urllib.request.Request(self.url + path, data=data, method=method)
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))  # 本机的请求不走代理
        try:
            with opener.open(request, timeout=10) as response:
                return response.status, json.loads(response.read())
        except urllib.error.HTTPError as e:
            return e.code, json.loads(e.read())

    def wait(self, ticket):
        for _ in range(600):
            _, job = self.call("GET", f"/jobs/{ticket}")
            if job["state"] != "running":
                return job
            time.sleep(0.1)
        self.fail("训练没有在一分钟内结束")

    def test_submit_poll_complete(self):
        samples = make_images(Path(self.tmp.name))
        status, reply = self.call("POST", "/jobs", request_for(samples, epochs=5))
        self.assertEqual(status, 200)
        job = self.wait(reply["ticket"])
        self.assertEqual(job["state"], "completed")
        self.assertTrue(Path(job["output"]["modelFile"]).is_file())
        self.assertNotIn("cancel", job)

    def test_failure_reason_is_reported(self):
        samples = make_images(Path(self.tmp.name))
        _, reply = self.call("POST", "/jobs", request_for(samples, imageSize=100))
        job = self.wait(reply["ticket"])
        self.assertEqual(job["state"], "failed")
        self.assertIn("8 的倍数", job["failure"])

    def test_cancel(self):
        samples = make_images(Path(self.tmp.name))
        _, reply = self.call("POST", "/jobs", request_for(samples, epochs=100000))
        self.assertEqual(self.call("DELETE", f"/jobs/{reply['ticket']}")[0], 200)
        self.assertEqual(self.wait(reply["ticket"])["failure"], "已取消")

    def test_unknown_things(self):
        self.assertEqual(self.call("GET", "/jobs/nope")[0], 404)
        self.assertEqual(self.call("DELETE", "/jobs/nope")[0], 404)
        self.assertEqual(self.call("POST", "/jobs", {"config": {"modelKind": "other"}})[0], 400)
        self.assertEqual(self.call("GET", "/health")[0], 200)


if __name__ == "__main__":
    unittest.main()
