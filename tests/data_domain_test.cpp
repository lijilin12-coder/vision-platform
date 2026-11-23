// BC-9 数据领域层的单元测试（POC-13 第 1 步）。只链接领域库：没有 OpenCV，没有文件。
// 重点是规则 N1：一个样本要么留作基准，要么用于训练，不能两者都是（docs/ddd/08-platforms-and-training.md §3）。
#include <gtest/gtest.h>

#include "vision/data/domain/dataset.h"
#include "vision/data/domain/sample.h"

using namespace vision;
using namespace vision::data;

namespace {

Sample sample(const std::string& id = "abc123") {
    auto s = Sample::import(id, 1000, "a.png");
    EXPECT_TRUE(s.ok());
    return std::move(s).value();
}

Dataset datasetOf(const std::vector<std::string>& ids) {
    Dataset d = Dataset::create("ds-1", "训练用").value();
    for (const auto& id : ids) EXPECT_TRUE(d.add(id).ok());
    return d;
}

}  // namespace

TEST(Sample, ImportRecordsEventAndStartsUnassigned) {
    Sample s = sample();
    EXPECT_EQ(s.use(), SampleUse::Unassigned);
    EXPECT_EQ(s.label(), Label::Unlabeled);
    const auto events = s.pullEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_STREQ(events[0]->name(), "SampleImported");
    EXPECT_FALSE(Sample::import("", 10, "a.png").ok());
    EXPECT_FALSE(Sample::import("abc", 0, "a.png").ok());
}

// 同样的内容换了文件名再导入：还是这个样本，只是多记一个名字
TEST(Sample, SameContentUnderAnotherNameIsTheSameSample) {
    Sample s = sample();
    s.seenAs("b.png");
    s.seenAs("b.png");
    s.seenAs("a.png");
    EXPECT_EQ(s.names(), (std::vector<std::string>{"a.png", "b.png"}));
}

// N1：用途定成一种之后，不能再改成另一种，也不能改回“未分配”
TEST(Sample, N1_UseCannotChangeOnceReserved) {
    Sample benchmark = sample();
    ASSERT_TRUE(benchmark.reserveFor(SampleUse::Benchmark).ok());
    EXPECT_TRUE(benchmark.reserveFor(SampleUse::Benchmark).ok());  // 重复一次没有问题
    EXPECT_EQ(benchmark.reserveFor(SampleUse::Training).error().code, "data.n1");
    EXPECT_FALSE(benchmark.reserveFor(SampleUse::Unassigned).ok());
    EXPECT_EQ(benchmark.use(), SampleUse::Benchmark);

    Sample training = sample();
    ASSERT_TRUE(training.reserveFor(SampleUse::Training).ok());
    EXPECT_EQ(training.reserveFor(SampleUse::Benchmark).error().code, "data.n1");
    EXPECT_EQ(training.use(), SampleUse::Training);
}

// 用途确定之后不能改标签：冻结的图集和数据集版本记录的是冻结时的标签
TEST(Sample, LabelIsLockedOnceTheSampleIsInUse) {
    Sample s = sample();
    ASSERT_TRUE(s.setLabel(Label::Ok).ok());
    ASSERT_TRUE(s.setLabel(Label::Ng).ok());  // 还没有用途：可以改
    ASSERT_TRUE(s.reserveFor(SampleUse::Training).ok());
    EXPECT_EQ(s.setLabel(Label::Ok).error().code, "data.sample_in_use");
    EXPECT_TRUE(s.setLabel(Label::Ng).ok());  // 设成它现在的标签不算修改
    EXPECT_EQ(s.label(), Label::Ng);
}

TEST(Dataset, AddIsIdempotentAndRemoveWorks) {
    Dataset d = datasetOf({"a", "b"});
    EXPECT_TRUE(d.add("a").ok());
    EXPECT_EQ(d.samples().size(), 2u);
    EXPECT_TRUE(d.remove("a").ok());
    EXPECT_FALSE(d.remove("a").ok());
    EXPECT_FALSE(d.add("").ok());
    EXPECT_FALSE(Dataset::create("", "x").ok());
    EXPECT_FALSE(Dataset::create("id", "").ok());
}

TEST(Dataset, FreezeProducesAnImmutableVersionWithLabels) {
    Dataset d = datasetOf({"a", "b", "c"});
    auto v = d.freeze({{"a", Label::Ok, SampleUse::Unassigned}, {"b", Label::Ng, SampleUse::Unassigned}, {"c", Label::Ok, SampleUse::Training}});
    ASSERT_TRUE(v.ok()) << v.error().message;
    EXPECT_EQ(v.value().id(), "ds-1@v1");
    EXPECT_EQ(v.value().entries().size(), 3u);
    EXPECT_EQ(v.value().count(Label::Ok), 2);
    EXPECT_EQ(v.value().count(Label::Ng), 1);
    const auto events = d.pullEvents();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_STREQ(events[0]->name(), "DatasetVersionFrozen");

    // 数据集本身还可以接着改、再冻结：得到的是另一个版本，前一个不变
    ASSERT_TRUE(d.add("d").ok());
    auto v2 = d.freeze({{"a", Label::Ok, SampleUse::Training}, {"b", Label::Ng, SampleUse::Training}, {"c", Label::Ok, SampleUse::Training},
                        {"d", Label::Ok, SampleUse::Unassigned}});
    ASSERT_TRUE(v2.ok());
    EXPECT_EQ(v2.value().id(), "ds-1@v2");
    EXPECT_EQ(v.value().entries().size(), 3u);
}

// N1：数据集里有留作基准的样本 → 拒绝冻结，并说出是哪些
TEST(Dataset, N1_FreezeRejectsBenchmarkSamples) {
    Dataset d = datasetOf({"aaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbb"});
    auto v = d.freeze({{"aaaaaaaaaaaaaaaa", Label::Ok, SampleUse::Unassigned}, {"bbbbbbbbbbbbbbbb", Label::Ok, SampleUse::Benchmark}});
    ASSERT_FALSE(v.ok());
    EXPECT_EQ(v.error().code, "data.n1");
    EXPECT_NE(v.error().message.find("bbbbbbbbbbbb"), std::string::npos);
    EXPECT_EQ(d.nextVersion(), 1);  // 没有生成版本
    EXPECT_TRUE(d.pullEvents().empty());
}

TEST(Dataset, FreezeRejectsEmptyUnlabeledOrUnknownSamples) {
    Dataset empty = datasetOf({});
    EXPECT_EQ(empty.freeze({}).error().code, "data.dataset_empty");
    Dataset d = datasetOf({"a", "b"});
    EXPECT_EQ(d.freeze({{"a", Label::Ok, SampleUse::Unassigned}, {"b", Label::Unlabeled, SampleUse::Unassigned}}).error().code, "data.unlabeled");
    EXPECT_EQ(d.freeze({{"a", Label::Ok, SampleUse::Unassigned}}).error().code, "data.sample_missing");
}
