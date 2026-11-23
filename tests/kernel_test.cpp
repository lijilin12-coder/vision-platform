#include <gtest/gtest.h>

#include "vision/kernel/geometry.h"
#include "vision/kernel/signature.h"

using namespace vision;

TEST(Roi, RejectsNonPositiveSize) {
    EXPECT_FALSE(Roi::rect(0, 0, 0, 10).ok());
    EXPECT_FALSE(Roi::rect(0, 0, 10, -1).ok());
    EXPECT_TRUE(Roi::rect(5, 5, 10, 10).ok());
}

TEST(Roi, IsValueObject) {
    EXPECT_EQ(Roi::rect(1, 2, 3, 4).value(), Roi::rect(1, 2, 3, 4).value());
    EXPECT_NE(Roi::rect(1, 2, 3, 4).value(), Roi::rect(1, 2, 3, 5).value());
}

TEST(Image, ValidatesPixelCount) {
    EXPECT_FALSE(Image::create(2, 2, Image::Format::Gray8, {1, 2, 3}).ok());
    auto img = Image::create(2, 2, Image::Format::Bgr8, std::vector<std::uint8_t>(12, 0));
    ASSERT_TRUE(img.ok());
    Image copy = img.value();
    EXPECT_EQ(copy.data(), img.value().data()) << "复制图像不应拷贝像素";
}

TEST(Region, RequiresGrayMask) {
    auto bgr = Image::create(1, 1, Image::Format::Bgr8, {0, 0, 0}).value();
    EXPECT_FALSE(Region::fromMask(bgr).ok());
}

TEST(ParamDef, ValidatesTypeAndRange) {
    ParamDef p{"value", ParamType::Number, 1.0, 0.0, 10.0, {}, ""};
    EXPECT_TRUE(p.validate(5.0).ok());
    EXPECT_FALSE(p.validate(11.0).ok());
    EXPECT_FALSE(p.validate(true).ok());
    ParamDef c{"method", ParamType::Choice, std::string("a"), std::nullopt, std::nullopt, {"a", "b"}, ""};
    EXPECT_TRUE(c.validate(std::string("b")).ok());
    EXPECT_FALSE(c.validate(std::string("x")).ok());
}

TEST(PortType, CompatibilityIsStrictForNow) {
    EXPECT_TRUE(isCompatible(PortType::Image, PortType::Image));
    EXPECT_FALSE(isCompatible(PortType::Image, PortType::Region));
}

// 模板参数的值是匹配模板的 ID（文本）；空文本表示还没有设置。ID 是否存在由方案编排检查（R7）
TEST(ParamDef, TemplateTypeAcceptsOnlyText) {
    const ParamDef t{"template", ParamType::Template, std::string(""), std::nullopt, std::nullopt, {}, ""};
    EXPECT_TRUE(t.validate(std::string("t1")).ok());
    EXPECT_TRUE(t.validate(std::string("")).ok());
    EXPECT_FALSE(t.validate(1.0).ok());
    EXPECT_FALSE(t.validate(true).ok());
}
