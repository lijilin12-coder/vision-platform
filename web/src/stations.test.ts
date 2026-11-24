import { describe, expect, it } from "vitest";
import { FakePlatformClient } from "./platform/FakePlatformClient";
import type { InspectionRecord } from "./platform/types";
import { createStations } from "./stations";

const record = (sequence: number, verdict: "OK" | "NG" = "OK", swapped = false): InspectionRecord => ({
  sequence, frameName: `${sequence}.png`, verdict, releaseId: "rel-1", swapped, totalMs: 1, reasons: [],
  result: { defects: [], measurements: [], poses: [] },
});

async function setup() {
  const client = new FakePlatformClient();
  await client.createRelease("亮斑@v1");
  const stations = createStations(client);
  await stations.refresh();
  await stations.create("一号");
  return { client, stations };
}

describe("工位页面", () => {
  it("新建的工位被选中，出现在列表里", async () => {
    const { stations } = await setup();
    expect(stations.state.stations.map((s) => s.name)).toEqual(["一号"]);
    expect(stations.state.current?.name).toBe("一号");
    expect(stations.state.releases.length).toBe(1);
  });

  it("后端拒绝的操作：显示原因，状态不变", async () => {
    const { stations } = await setup();
    expect(await stations.start()).toBe(false); // 还没有部署
    expect(stations.state.error).toContain("请先部署");
    expect(stations.state.current?.runner.running).toBe(false);
    expect(await stations.deploy("nope")).toBe(false);
    expect(stations.state.error).toContain("找不到上线包");
    // 成功的操作清掉出错信息
    expect(await stations.deploy("rel-1")).toBe(true);
    expect(stations.state.error).toBeNull();
    expect(stations.state.current?.activeRelease).toBe("rel-1");
  });

  it("轮询只收下新的检测记录，新的排在前面，不重复", async () => {
    const { client, stations } = await setup();
    await stations.deploy("rel-1");
    await stations.start();
    client.produced.push(record(1), record(2, "NG"));
    await stations.poll();
    expect(stations.state.records.map((r) => r.sequence)).toEqual([2, 1]);
    expect(stations.state.current?.runner).toMatchObject({ ok: 1, ng: 1 });
    await stations.poll(); // 没有新的
    expect(stations.state.records.length).toBe(2);
    client.produced.push(record(3, "OK", true));
    await stations.poll();
    expect(stations.state.records.map((r) => r.sequence)).toEqual([3, 2, 1]);
    expect(stations.state.records[0].swapped).toBe(true);
  });

  it("记录只留最近的一批；换一个工位时清空", async () => {
    const { client, stations } = await setup();
    await stations.deploy("rel-1");
    for (let i = 1; i <= 80; i++) client.produced.push(record(i));
    await stations.poll();
    expect(stations.state.records.length).toBe(60);
    expect(stations.state.records[0].sequence).toBe(80);
    await stations.create("二号");
    expect(stations.state.current?.name).toBe("二号");
    expect(stations.state.records).toEqual([]);
  });

  it("上线包显示成“方案名 版本（编号的开头）”：同一个方案的几个版本分得清", async () => {
    const { client, stations } = await setup();
    client.releases = [
      { id: "aaaaaaaa-1111", recipeVersionId: "r1@v1", recipeName: "亮斑检测", withdrawn: false },
      { id: "bbbbbbbb-2222", recipeVersionId: "r1@v2", recipeName: "亮斑检测", withdrawn: false },
    ];
    await stations.refresh();
    expect(stations.releaseName("aaaaaaaa-1111")).toBe("亮斑检测 v1（aaaaaaaa）");
    expect(stations.releaseName("bbbbbbbb-2222")).toBe("亮斑检测 v2（bbbbbbbb）");
    expect(stations.releaseName("")).toBe("（无）");
    expect(stations.releaseName("cccccccc-3333")).toBe("cccccccc"); // 列表里没有的：只显示编号
  });
});
