#ifndef DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_FACTORY_H
#define DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_FACTORY_H

#include <dftracer/utils/core/utilities/behaviors/behavior.h>

#include <any>
#include <functional>
#include <memory>
#include <typeindex>
#include <unordered_map>

namespace dftracer::utils::utilities::behaviors {

/**
 * @brief Factory that creates behaviors based on tags (type-erased mapping).
 *
 * This factory allows registering tag-to-behavior mappings, enabling
 * generic behavior creation without hardcoding specific tag types.
 *
 * Key design principle: The executor/wrapper doesn't know about specific tags.
 * It just asks the factory "give me a behavior for this tag" and the factory
 * handles the type-erased creation.
 *
 * @tparam I Input type
 * @tparam O Output type
 */
template <typename I, typename O>
class BehaviorFactory {
   private:
    // Type-erased function: takes std::any (tag config) -> returns behavior
    using Creator =
        std::function<std::shared_ptr<UtilityBehavior<I, O>>(const std::any&)>;

    // Map: type_index(Tag) -> creator function
    std::unordered_map<std::type_index, Creator> creators_;

   public:
    BehaviorFactory() = default;

    /**
     * @brief Register a behavior creator for a specific tag type.
     *
     * The creator function receives the tag configuration and returns
     * the corresponding behavior instance.
     *
     * @tparam Tag The tag type to register
     * @param creator Function that creates behavior from tag config
     *
     * Usage:
     * @code
     * factory.register_behavior<tags::Cacheable>(
     *     [](const tags::Cacheable& tag) {
     *         return std::make_shared<CachingBehavior<I, O>>(tag);
     *     }
     * );
     * @endcode
     */
    template <typename Tag>
    void register_behavior(
        std::function<std::shared_ptr<UtilityBehavior<I, O>>(const Tag&)>
            creator) {
        // Wrap typed creator in type-erased creator
        creators_[std::type_index(typeid(Tag))] =
            [creator](const std::any& tag_config) {
                return creator(std::any_cast<const Tag&>(tag_config));
            };
    }

    /**
     * @brief Create a behavior for a specific tag.
     *
     * Looks up the registered creator for the tag type and invokes it.
     *
     * @tparam Tag The tag type
     * @param tag The tag configuration instance
     * @return Shared pointer to created behavior, or nullptr if not registered
     *
     * Usage:
     * @code
     * tags::Cacheable cache_tag;
     * cache_tag.max_cache_size = 500;
     * auto behavior = factory.create(cache_tag);
     * if (behavior) {
     *     chain.add_behavior(behavior);
     * }
     * @endcode
     */
    template <typename Tag>
    std::shared_ptr<UtilityBehavior<I, O>> create(const Tag& tag) const {
        auto it = creators_.find(std::type_index(typeid(Tag)));
        if (it == creators_.end()) {
            return nullptr;  // No behavior registered for this tag
        }

        // Invoke type-erased creator with type-erased tag
        return it->second(std::any(tag));
    }

    /**
     * @brief Check if a behavior is registered for a tag type.
     *
     * @tparam Tag The tag type to check
     * @return true if behavior creator is registered
     */
    template <typename Tag>
    bool has() const {
        return creators_.find(std::type_index(typeid(Tag))) != creators_.end();
    }

    /**
     * @brief Get number of registered behaviors.
     * @return Number of tag-to-behavior mappings
     */
    std::size_t size() const { return creators_.size(); }

    /**
     * @brief Clear all registered behaviors.
     */
    void clear() { creators_.clear(); }
};

/**
 * @brief Get the global behavior factory instance for a specific I/O type pair.
 *
 * Each I/O type pair has its own factory instance (stored as static).
 * This allows different utilities with different types to have different
 * behavior registrations.
 *
 * @tparam I Input type
 * @tparam O Output type
 * @return Reference to the global factory for this type pair
 *
 * Usage:
 * @code
 * auto& factory = get_behavior_factory<std::string, int>();
 * factory.register_behavior<tags::Cacheable>(...);
 * @endcode
 */
template <typename I, typename O>
BehaviorFactory<I, O>& get_behavior_factory() {
    static BehaviorFactory<I, O> factory;
    return factory;
}

/**
 * @brief Register default behaviors for a specific I/O type pair.
 *
 * This function should be called once to register the standard behaviors
 * (caching, retry, monitoring) for a given type pair.
 *
 * Note: This is forward-declared here but will be implemented after
 * the specific behavior classes are defined.
 *
 * @tparam I Input type
 * @tparam O Output type
 *
 * Usage:
 * @code
 * // Call once at startup for each type pair you use
 * register_default_behaviors<std::string, FileData>();
 * @endcode
 */
template <typename I, typename O>
void register_default_behaviors();

}  // namespace dftracer::utils::utilities::behaviors

#endif  // DFTRACER_UTILS_CORE_UTILITIES_BEHAVIORS_BEHAVIOR_FACTORY_H
