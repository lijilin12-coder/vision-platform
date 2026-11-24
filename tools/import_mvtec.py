#!/usr/bin/env python3
"""把 MVTec AD 的一个类别导入平台，并冻结成一个数据集版本（可以直接拿去训练）。

MVTec AD 是异常检测最常用的公开数据集（需要自己到官网申请下载，仅限非商业用途）。
解压后每个类别一个目录，结构是：

  bottle/
    train/good/*.png            合格的，用来训练
    test/good/*.png             合格的
    test/<缺陷名>/*.png          不合格的（每种缺陷一个目录）
    ground_truth/...            缺陷的位置（这里用不到）

用法：
  python tools/import_mvtec.py /数据集/mvtec/bottle --cli ./build/apps/vision-cli --data ./vision-data

它只是替你敲这几条命令：sample import（合格的、不合格的各一次）→ dataset new → dataset add → dataset freeze。
之后：vision-cli train submit <数据集版本ID> --name bottle-anomaly --image-size 256 --wait

在真实的 MVTec AD 上验证过（2026-11-24，bottle、leather、screw 三个类别）。

注意：平台会自己把这些图像重新划分成训练集、验证集、测试集（分层、可重现），不沿用 MVTec 自带的 train / test 目录。
所以这里得到的指标不能和论文里的数字直接比较。
"""

import argparse
import subprocess
import sys
from pathlib import Path


def run(cli: list[str], *args: str) -> str:
    result = subprocess.run([*cli, *args], capture_output=True, text=True, encoding="utf-8")
    if result.returncode != 0:
        sys.exit(f"命令失败：{' '.join(args)}\n{result.stdout}{result.stderr}")
    return result.stdout


def main() -> None:
    parser = argparse.ArgumentParser(description="把 MVTec AD 的一个类别导入平台")
    parser.add_argument("category", help="类别的目录，例如 /数据集/mvtec/bottle")
    parser.add_argument("--cli", default="./build/apps/vision-cli", help="vision-cli 的位置")
    parser.add_argument("--data", default="./vision-data", help="平台的数据目录")
    args = parser.parse_args()

    category = Path(args.category)
    good = [d for d in (category / "train" / "good", category / "test" / "good") if d.is_dir()]
    bad = sorted(d for d in (category / "test").glob("*") if d.is_dir() and d.name != "good") if (category / "test").is_dir() else []
    if not good:
        sys.exit(f"{category} 不像是 MVTec AD 的类别目录：找不到 train/good")

    cli = [args.cli, "--data", args.data]
    ok = sum(len(run(cli, "sample", "import", str(d), "--label", "ok").splitlines()) for d in good)
    ng = sum(len(run(cli, "sample", "import", str(d), "--label", "ng").splitlines()) for d in bad)
    print(f"导入了 {ok} 幅合格的、{ng} 幅不合格的图像（{len(bad)} 种缺陷）")

    dataset = run(cli, "dataset", "new", f"mvtec-{category.name}").split()[-1]
    run(cli, "dataset", "add", dataset, "--all-unassigned")
    version = run(cli, "dataset", "freeze", dataset).split()[-1]
    print(f"数据集版本：{version}")
    print(f"下一步：{args.cli} --data {args.data} train submit {version} --name {category.name}-anomaly --image-size 256 --wait")


if __name__ == "__main__":
    main()
