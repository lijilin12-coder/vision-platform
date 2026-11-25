import { describe, expect, it } from "vitest";
import { FakePlatformClient } from "./platform/FakePlatformClient";
import type { InspectionRecord, SavedRecord } from "./platform/types";
import { createStations, imageNote, reviewLabel } from "./stations";

const record = (sequence: number, verdict: "OK" | "NG" = "OK", swapped = false): InspectionRecord => ({
  sequence, frameName: `${sequence}.png`, verdict, releaseId: "rel-1", swapped, totalMs: 1, reasons: [],
  result: { defects: [], measurements: [], poses: [] },
});

async function setup() {
  const client = new FakePlatformClient();
  client.allowUnaccepted = true; // 这里测的是工位页面，不是验收
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
      { id: "aaaaaaaa-1111", recipeVersionId: "r1@v1", recipeName: "亮斑检测", withdrawn: false, acceptanceState: "passed", acceptance: null },
      { id: "bbbbbbbb-2222", recipeVersionId: "r1@v2", recipeName: "亮斑检测", withdrawn: false, acceptanceState: "none", acceptance: null },
    ];
    await stations.refresh();
    expect(stations.releaseName("aaaaaaaa-1111")).toBe("亮斑检测 v1（aaaaaaaa）");
    expect(stations.releaseName("bbbbbbbb-2222")).toBe("亮斑检测 v2（bbbbbbbb） · 未验收"); // 没验收的标出来
    expect(stations.releaseName("")).toBe("（无）");
    expect(stations.releaseName("cccccccc-3333")).toBe("cccccccc"); // 列表里没有的：只显示编号
  });

  // ---- 历史记录 ----
  const saved = (n: number, verdict: "OK" | "NG" = "OK"): SavedRecord => ({
    ...record(n, verdict), id: String(1000 + n), time: "2026-11-24 12:00:00", recipeVersionId: "亮斑@v1", imageState: verdict === "NG" ? "kept" : "cached", review: null,
    result: { defects: verdict === "NG" ? [{ nodeId: "n5", category: "亮斑", centerX: 1, centerY: 1, left: 0, top: 0, width: 2, height: 2, area: 4 }] : [],
      measurements: [], poses: [] },
  });

  it("历史记录：从新到旧，一次一页，可以往前翻", async () => {
    const { client, stations } = await setup();
    for (let n = 1; n <= 70; n++) client.saved.push(saved(n, n % 10 === 0 ? "NG" : "OK"));
    expect(await stations.loadHistory()).toBe(true);
    const h = () => stations.state.history;
    expect(h().records.length).toBe(30);
    expect(h().records[0].sequence).toBe(70);
    expect(h().hasMore).toBe(true);
    expect(h().summary?.total).toBe(70);
    expect(stations.yieldText(h().summary)).toBe("90.0%");
    await stations.loadMoreHistory();
    await stations.loadMoreHistory();
    expect(h().records.length).toBe(70);
    expect(h().records[69].sequence).toBe(1);
    expect(h().hasMore).toBe(false); // 最后一页不满：到头了
    expect(new Set(h().records.map((r) => r.id)).size).toBe(70); // 翻页没有重复
  });

  it("历史记录：只看不合格的；点开一条看当时的图像", async () => {
    const { client, stations } = await setup();
    for (let n = 1; n <= 20; n++) client.saved.push(saved(n, n % 5 === 0 ? "NG" : "OK"));
    await stations.loadHistory();
    stations.pickRecord(stations.state.history.records[0]);
    await stations.setOnlyNg(true);
    const h = stations.state.history;
    expect(h.records.map((r) => r.sequence)).toEqual([20, 15, 10, 5]);
    expect(h.selected).toBeNull(); // 换了筛选条件：原来点开的那条不一定还在列表里
    expect(h.summary?.total).toBe(20); // 汇总是全部的，不跟着筛选变
    expect(h.summary?.defectsByCategory).toEqual({ 亮斑: 4 });
    stations.pickRecord(h.records[1]);
    expect(stations.recordImageUrl(h.selected!)).toBe("/api/stations/s1/records/1015/image");
    expect(stations.recordImageUrl(saved(1))).toContain("/records/1001/image"); // 合格的在短期缓存里：现在看得到
    expect(stations.recordImageUrl({ ...saved(1), imageState: "expired" })).toBe(""); // 过期清掉了
    expect(stations.recordImageUrl({ ...saved(1), imageState: "none" })).toBe("");
    expect(imageNote(saved(1))).toContain("短期缓存");
    expect(imageNote({ ...saved(1), imageState: "expired" })).toContain("过了保留期");
    expect(imageNote({ ...saved(1), imageState: "none" })).toContain("没有保留");
    expect(imageNote(saved(2, "NG"))).toBe("");
    expect(stations.yieldText(null)).toBe("—");
    expect(stations.yieldText({ total: 1, ok: 0, ng: 0, error: 1, yield: null, defectsByCategory: {}, reviewed: 0, reviewedCorrect: 0,
      falseAlarms: 0, misses: 0, reviewedErrors: 0 })).toBe("—");
  });

  it("历史记录：换一个工位，原来那个工位的记录不留在页面上", async () => {
    const { client, stations } = await setup();
    client.saved.push(saved(1, "NG"));
    await stations.setOnlyNg(true);
    expect(stations.state.history.records.length).toBe(1);
    await stations.create("二号");
    expect(stations.state.history.records).toEqual([]);
    expect(stations.state.history.summary).toBeNull();
    expect(stations.state.history.onlyNg).toBe(true); // 筛选条件是人选的，留着
  });

  // ---- 复判 ----
  it("复判：误报和漏检分得清；列表不跳，汇总跟着变", async () => {
    const { client, stations } = await setup();
    for (let n = 1; n <= 6; n++) client.saved.push(saved(n, n % 2 === 0 ? "NG" : "OK")); // 2、4、6 判 NG
    await stations.loadHistory();
    const h = stations.state.history;
    expect(await stations.review("OK")).toBe(false); // 还没点开任何一条
    stations.pickRecord(h.records[0]); // #6，判 NG
    expect(await stations.review("OK", "  是反光 ")).toBe(true); // 人说其实合格
    expect(h.selected?.review?.kind).toBe("false_alarm");
    expect(h.selected?.review?.note).toBe("是反光");
    expect(reviewLabel(h.records[0].review)).toBe("误报");
    expect(h.records.map((r) => r.sequence)).toEqual([6, 5, 4, 3, 2, 1]); // 顺序没变
    stations.pickNext(); // #5，判 OK
    expect(h.selected?.sequence).toBe(5);
    await stations.review("NG");
    expect(reviewLabel(h.selected!.review)).toBe("漏检");
    stations.pickNext(); // #4，判 NG
    await stations.review("NG");
    expect(reviewLabel(h.selected!.review)).toBe("判对了");
    expect(h.summary).toMatchObject({ reviewed: 3, falseAlarms: 1, misses: 1, reviewedCorrect: 1 });
    // 改主意：#4 其实是合格的
    await stations.review("OK");
    expect(reviewLabel(h.selected!.review)).toBe("误报");
    expect(h.summary).toMatchObject({ reviewed: 3, falseAlarms: 2, reviewedCorrect: 0 });
    expect(reviewLabel(null)).toBe("");
    expect(h.records.find((r) => r.sequence === 5)?.imageState).toBe("kept"); // 判 OK 的那条复判之后，图像转成了长期保留
    // 最后一条之后没有下一条：留在原地
    stations.pickRecord(h.records[5]);
    stations.pickNext();
    expect(h.selected?.sequence).toBe(1);
  });

  it("按复判筛选：还没复判的、确认判错了的", async () => {
    const { client, stations } = await setup();
    for (let n = 1; n <= 5; n++) client.saved.push(saved(n, "NG"));
    await stations.loadHistory();
    stations.pickRecord(stations.state.history.records[0]); // #5
    await stations.review("OK"); // 误报
    stations.pickNext(); // #4
    await stations.review("NG"); // 判对了
    await stations.setReviewFilter("misjudged");
    expect(stations.state.history.records.map((r) => r.sequence)).toEqual([5]);
    await stations.setReviewFilter("unreviewed");
    expect(stations.state.history.records.map((r) => r.sequence)).toEqual([3, 2, 1]);
    await stations.setReviewFilter("");
    expect(stations.state.history.records.length).toBe(5);
  });

  it("导出复判过的图像：说清导出了多少、有多少没有图像", async () => {
    const { client, stations } = await setup();
    client.saved.push({ ...saved(1, "OK"), imageState: "expired" }, saved(2, "NG"));
    await stations.loadHistory();
    stations.pickRecord(stations.state.history.records[0]); // #2，NG，有图像
    await stations.review("OK");
    stations.pickNext(); // #1，OK，图像已经过期清掉了
    await stations.review("NG"); // 漏检，但没有图像可导
    expect(await stations.exportReviewed(" ")).toBe(false);
    expect(await stations.exportReviewed(" /out ")).toBe(true);
    expect(client.exportedReviews).toEqual(["/out"]);
    expect(stations.state.history.notice).toBe("已导出到 /out：ok 1 幅，ng 0 幅；另有 1 条复判过的记录没有图像（当时没留，或者已经过期）");
    stations.dismiss();
    expect(stations.state.history.notice).toBeNull();
  });

  it("复判被后端拒绝：显示原因", async () => {
    const { client, stations } = await setup();
    client.saved.push(saved(1, "NG"));
    await stations.loadHistory();
    stations.pickRecord({ ...stations.state.history.records[0], id: "不存在" });
    expect(await stations.review("OK")).toBe(false);
    expect(stations.state.error).toContain("找不到这条检测记录");
  });
});
