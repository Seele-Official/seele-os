#include <cstddef>
#include <vector>

#include "list.h"
#include <gtest/gtest.h>

namespace {

struct item {
    item* next = nullptr;
    item* prev = nullptr;
    int id = 0;
};

TEST(ListTest, IntrusiveOperationsAndIterators) {
    IntrusiveList<item> list;
    item a{.id = 1};
    item b{.id = 2};
    item c{.id = 3};
    item d{.id = 4};

    EXPECT_TRUE(list.empty());
    EXPECT_EQ(list.size(), size_t{0});

    list.push_back(&b);
    list.push_front(&a);
    list.insert_after(&b, &d);
    list.insert_before(&d, &c);
    EXPECT_EQ(list.size(), size_t{4});

    std::vector<int> forward;
    for(const item& elem: list) {
        forward.push_back(elem.id);
    }
    ASSERT_EQ(forward.size(), size_t{4});
    EXPECT_EQ(forward[0], 1);
    EXPECT_EQ(forward[1], 2);
    EXPECT_EQ(forward[2], 3);
    EXPECT_EQ(forward[3], 4);

    std::vector<int> backward;
    for(auto it = list.cend(); it != list.cbegin();) {
        --it;
        backward.push_back(it->id);
    }
    ASSERT_EQ(backward.size(), size_t{4});
    EXPECT_EQ(backward[0], 4);
    EXPECT_EQ(backward[3], 1);

    EXPECT_DEATH(list.push_back(&b), "node is already linked");

    list.erase(&a);
    EXPECT_EQ(list.front(), &b);
    EXPECT_EQ(a.next, nullptr);
    EXPECT_EQ(a.prev, nullptr);

    EXPECT_EQ(list.pop_back(), &d);
    list.clear();
    EXPECT_TRUE(list.empty());
    EXPECT_EQ(b.next, nullptr);
    EXPECT_EQ(c.next, nullptr);
}

TEST(ListTest, HeapOwningListUnderAsanLsan) {
    {
        List<int> values;
        for(int i = 0; i < 32; ++i) {
            values.push_back(i * 10);
        }
        values.push_front(-10);
        EXPECT_EQ(values.size(), size_t{33});
        EXPECT_EQ(values.front(), -10);
        EXPECT_EQ(values.back(), 310);

        values.pop_front();
        values.pop_back();
        EXPECT_EQ(values.size(), size_t{31});
        EXPECT_EQ(values.front(), 0);
        EXPECT_EQ(values.back(), 300);
    }

    List<int> empty_list;
    EXPECT_DEATH(empty_list.pop_front(), "List::pop_front on an empty list");
}

}  // namespace
