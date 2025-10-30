#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <dftracer/utils/utilities/composites/dft/event_hasher_utility.h>
#include <doctest/doctest.h>

using namespace dftracer::utils::utilities::composites::dft;

TEST_SUITE("EventHasher") {
    TEST_CASE("EventHasher - Hash single event") {
        // Create test events
        std::vector<EventId> events;
        events.push_back(EventId(1, 1000, 2000));

        auto input = EventHashInput::from_events(events);

        EventHasher hasher;
        auto hash = hasher.process(input);

        // Should get a non-zero hash
        CHECK(hash != 0);
    }

    TEST_CASE("EventHasher - Hash multiple events") {
        std::vector<EventId> events;
        events.push_back(EventId(1, 1000, 2000));
        events.push_back(EventId(2, 1001, 2001));
        events.push_back(EventId(3, 1002, 2002));

        auto input = EventHashInput::from_events(events);

        EventHasher hasher;
        auto hash = hasher.process(input);

        CHECK(hash != 0);
    }

    TEST_CASE("EventHasher - Deterministic hashing") {
        // Same events should produce same hash
        std::vector<EventId> events1;
        events1.push_back(EventId(10, 2000, 3000));
        events1.push_back(EventId(20, 2001, 3001));

        std::vector<EventId> events2;
        events2.push_back(EventId(10, 2000, 3000));
        events2.push_back(EventId(20, 2001, 3001));

        EventHasher hasher;
        auto hash1 = hasher.process(EventHashInput::from_events(events1));
        auto hash2 = hasher.process(EventHashInput::from_events(events2));

        CHECK(hash1 == hash2);
    }

    TEST_CASE("EventHasher - Order sensitivity") {
        // Different order should produce different hash
        std::vector<EventId> events1;
        events1.push_back(EventId(1, 1000, 2000));
        events1.push_back(EventId(2, 1001, 2001));

        std::vector<EventId> events2;
        events2.push_back(EventId(2, 1001, 2001));
        events2.push_back(EventId(1, 1000, 2000));

        EventHasher hasher;
        auto hash1 = hasher.process(EventHashInput::from_events(events1));
        auto hash2 = hasher.process(EventHashInput::from_events(events2));

        CHECK(hash1 != hash2);
    }

    TEST_CASE("EventHasher - Empty events") {
        std::vector<EventId> events;  // Empty

        auto input = EventHashInput::from_events(events);

        EventHasher hasher;
        auto hash = hasher.process(input);

        // Should still produce a hash (likely 0 or seed value)
        CHECK(hash >= 0);
    }
}
