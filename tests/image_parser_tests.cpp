#include <gtest/gtest.h>

import rstd.cppstd;
import wescene.pkg.parse;
import wescene.types;

namespace
{

class TrackingImageParser : public owe::WPTexImageParser {
public:
    TrackingImageParser(): WPTexImageParser(nullptr) {}

    std::shared_ptr<owe::Image> Parse(const std::string& name) override {
        auto current  = m_active.fetch_add(1) + 1;
        auto observed = m_peak.load();
        while (current > observed && ! m_peak.compare_exchange_weak(observed, current)) {
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto image = std::make_shared<owe::Image>();
        image->key = name;
        m_active.fetch_sub(1);
        return image;
    }

    owe::ImageHeader ParseHeader(const std::string&) override { return {}; }

    int peak() const { return m_peak.load(); }

private:
    std::atomic<int> m_active { 0 };
    std::atomic<int> m_peak { 0 };
};

TEST(ImageParser, BatchPreservesOrderAndBoundsConcurrency) {
    TrackingImageParser      parser;
    std::vector<std::string> names { "0", "1", "2", "3", "4", "5", "6", "7" };

    auto images = parser.ParseMany(names);

    ASSERT_EQ(images.size(), names.size());
    for (std::size_t index = 0; index < names.size(); ++index) {
        ASSERT_NE(images[index], nullptr);
        EXPECT_EQ(images[index]->key, names[index]);
    }
    EXPECT_GT(parser.peak(), 1);
    EXPECT_LE(parser.peak(), 4);
}

} // namespace
