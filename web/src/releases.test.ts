import { describe, expect, it } from "vitest";
import { FakePlatformClient } from "./platform/FakePlatformClient";
import type { Acceptance } from "./platform/types";
import { acceptanceLabel, createReleases, releaseTitle, verdictLine } from "./releases";
import { createStations } from "./stations";

const newSet = (maxMisses = 0) => ({ name: "亮斑基准", okDirectory: "/g/ok", ngDirectory: "/g/ng", maxMisses, maxFalseAlarmRate: 0, maxErrors: 0 });

async function setup() {
  const client = new FakePlatformClient();
  const strict = await client.createRelease("亮斑@v1");
  const lenient = await client.createRelease("亮斑@v2");
  client.missesFor[lenient] = 2;
  const releases = createReleases(client);
  await releases.refresh();
  return { client, releases, strict, lenient };
}

describe("上线包页面", () => {
  it("打开时选中第一个上线包；都还没有验收", async () => {
    const { releases, strict } = await setup();
    expect(releases.state.selectedId).toBe(strict);
    expect(releases.state.releases.map(acceptanceLabel)).toEqual(["未验收", "未验收"]);
    expect(releaseTitle(releases.selected()!)).toBe("亮斑@v1 v1（rel-1）");
    expect(await releases.check()).toBe(false); // 还没有基准图集：什么都不做
  });

  it("新建基准图集：建好就选中它；后端拒绝时显示原因", async () => {
    const { releases } = await setup();
    expect(await releases.createGoldenSet({ ...newSet(), okDirectory: "", ngDirectory: "" })).toBe(false);
    expect(releases.state.error).toContain("没有找到图像");
    expect(await releases.createGoldenSet(newSet())).toBe(true);
    expect(releases.state.error).toBeNull();
    expect(releases.state.goldenSets.length).toBe(1);
    expect(releases.state.goldenSetId).toBe("gs-1");
    await releases.createGoldenSet(newSet(2));
    expect(releases.state.goldenSetId).toBe("gs-2");
  });

  it("验收：通过和没通过都记在上线包上，列表跟着变", async () => {
    const { releases, lenient } = await setup();
    await releases.createGoldenSet(newSet());
    expect(await releases.check()).toBe(true);
    expect(releases.selected()?.acceptanceState).toBe("passed");
    releases.select(lenient);
    expect(await releases.check()).toBe(true); // 没通过不是出错：检查做完了
    expect(releases.state.error).toBeNull();
    expect(releases.selected()?.acceptanceState).toBe("failed");
    expect(releases.selected()?.acceptance?.wrongCases.length).toBe(2);
    expect(releases.state.releases.map(acceptanceLabel)).toEqual(["验收通过", "验收没有通过"]);
    expect(releases.state.checking).toBe(false);
    // 换一个允许漏 2 张的基准图集重新验收：通过了。以最近的一次为准
    await releases.createGoldenSet(newSet(2));
    await releases.check();
    expect(releases.selected()?.acceptanceState).toBe("passed");
  });

  it("只有验收通过的上线包才能部署，工位页面显示后端的原因", async () => {
    const { client, releases, strict, lenient } = await setup();
    const stations = createStations(client);
    await stations.refresh();
    await stations.create("一号");
    expect(await stations.deploy(strict)).toBe(false); // 还没验收
    expect(stations.state.error).toContain("还没有做过验收检查");
    expect(stations.releaseName(strict)).toContain("未验收"); // 下拉框里就看得出来
    await releases.createGoldenSet(newSet());
    await releases.check();
    releases.select(lenient);
    await releases.check();
    await stations.refresh();
    expect(await stations.deploy(lenient)).toBe(false);
    expect(stations.state.error).toContain("验收检查没有通过");
    expect(stations.releaseName(lenient)).toContain("验收没有通过");
    expect(await stations.deploy(strict)).toBe(true);
    expect(stations.releaseName(strict)).not.toContain("验收");
  });

  it("导出上线包文件：验收通过的才行；导出到哪里说清楚", async () => {
    const { client, releases, strict } = await setup();
    expect(await releases.exportTo("/tmp/a.vrel")).toBe(false);
    expect(releases.state.error).toContain("请先验收");
    expect(releases.state.notice).toBeNull();
    await releases.createGoldenSet(newSet());
    await releases.check();
    expect(await releases.exportTo("  /tmp/a.vrel ")).toBe(true);
    expect(client.exported).toEqual([{ releaseId: strict, path: "/tmp/a.vrel" }]);
    expect(releases.state.notice).toContain("/tmp/a.vrel");
    expect(await releases.exportTo(" ")).toBe(false); // 没填路径：什么都不做
    expect(client.exported.length).toBe(1);
  });

  it("结论说得清是哪一条没达到", () => {
    const base: Acceptance = { releaseId: "r", goldenSetId: "g", goldenSetName: "基准", checkedAtMs: 1, passed: false,
      criteria: { maxMisses: 0, maxFalseAlarmRate: 0.02, maxErrors: 0 }, total: 100, truthOk: 50, truthNg: 50, misses: 1, falseAlarms: 5,
      errors: 0, falseAlarmRate: 0.1, meanMs: 1, maxMs: 2, wrongCases: [] };
    expect(verdictLine(base)).toBe("没有通过：漏检 1 张（最多允许 0）；误报率 10.0%（最多允许 2.0%）");
    expect(verdictLine({ ...base, misses: 0, falseAlarms: 1, falseAlarmRate: 0.02, errors: 3 })).toBe("没有通过：出错 3 张（最多允许 0）");
    expect(verdictLine({ ...base, passed: true, misses: 0, falseAlarms: 1, falseAlarmRate: 0.02 })).toContain("通过：100 个用例");
    expect(acceptanceLabel({ id: "r", recipeVersionId: "a@v1", recipeName: "a", withdrawn: true, acceptanceState: "passed", acceptance: null })).toBe("已撤回");
  });
});
