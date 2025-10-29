#ifndef DFTRACER_UTILS_UTILITIES_BEHAVIORS_H
#define DFTRACER_UTILS_UTILITIES_BEHAVIORS_H

/**
 * @file behaviors.h
 * @brief Convenience header including all behavior system components.
 *
 * This header provides the complete behavior system for utilities:
 *
 * Core Components:
 * - UtilityBehavior: Base interface for behaviors
 * - BehaviorChain: Orchestrates multiple behaviors
 * - BehaviorFactory: Creates behaviors from tags
 *
 * Standard Behaviors:
 * - CachingBehavior: LRU cache with TTL
 * - RetryBehavior: Retry with exponential backoff
 * - MonitoringBehavior: Logging and timing
 *
 * Default Behaviors:
 * - register_default_behaviors(): Register standard tag mappings
 * - get_default_behavior_factory(): Get factory with defaults
 *
 * Note: UtilityExecutor is not included here as it's an orchestrator
 * that uses behaviors, not a behavior itself. Include utility_executor.h
 * separately.
 */

#include <dftracer/utils/core/utilities/behaviors/behavior.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_chain.h>
#include <dftracer/utils/core/utilities/behaviors/behavior_factory.h>
#include <dftracer/utils/core/utilities/behaviors/caching_behavior.h>
#include <dftracer/utils/core/utilities/behaviors/default_behaviors.h>
#include <dftracer/utils/core/utilities/behaviors/monitoring_behavior.h>
#include <dftracer/utils/core/utilities/behaviors/retry_behavior.h>

#endif  // DFTRACER_UTILS_UTILITIES_BEHAVIORS_H
