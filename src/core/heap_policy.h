#pragma once

#include <cstddef>
#include <cstdint>

namespace HeapPolicy {
    // TLS gating thresholds
    static constexpr size_t kMinHeapForTls = 35000;
    static constexpr size_t kMinContigForTls = 35000;
    static constexpr size_t kProactiveTlsConditioning = 45000;

    // General allocation safety thresholds
    static constexpr size_t kMinHeapForOinkNetworkAdd = 30000;
    static constexpr size_t kMinHeapForHandshakeAdd = 60000;
    static constexpr size_t kMinHeapForReconGrowth = 20000;
    static constexpr size_t kMinHeapForDnhGrowth = 40000;
    static constexpr size_t kMinHeapForSpectrumGrowth = 20000;

    // Heap stabilization / recovery thresholds
    static constexpr size_t kHeapStableThreshold = 50000;
    static constexpr size_t kFileServerRecoveryThreshold = 50000;
    static constexpr size_t kFileServerMinHeap = 40000;
    static constexpr size_t kFileServerMinLargest = 30000;

    // Allocation slack (allocator overhead / fragmentation cushion)
    static constexpr size_t kReserveSlackSmall = 256;
    static constexpr size_t kReserveSlackLarge = 1024;
    static constexpr size_t kPmkidAllocSlack = 256;
    static constexpr size_t kHandshakeAllocSlack = 1024;

    // Mode-specific thresholds
    static constexpr size_t kWarhogHeapWarning = 40000;
    static constexpr size_t kWarhogHeapCritical = 25000;
    static constexpr size_t kDnhInjectMinHeap = 80000;
    static constexpr size_t kPigSyncMinContig = 26000;

    // Heap health sampling/tuning
    static constexpr uint32_t kHealthSampleIntervalMs = 1000;
    static constexpr uint32_t kHealthToastDurationMs = 5000;
    static constexpr uint8_t kHealthToastMinDelta = 5;
    static constexpr uint8_t kHealthConditionTriggerPct = 65;
    static constexpr uint8_t kHealthConditionClearPct = 75;
    static constexpr uint32_t kHealthConditionCooldownMs = 30000;
    static constexpr float kHealthFragPenaltyScale = 0.60f;

    // Growth gating (fragmentation-aware)
    static constexpr float kMinFragRatioForGrowth = 0.40f;

    // Stress test guardrail
    static constexpr size_t kStressMinHeap = 70000;

    // Boot conditioning (allocator training)
    static constexpr int kBootFragBlocks = 50;
    static constexpr size_t kBootFragBlockSize = 1024;
    static constexpr int kBootStructBlocks = 20;
    static constexpr size_t kBootStructBlockSize = 3072;
    static constexpr size_t kBootTlsTestSizes[3] = {26624, 32768, 40960};

    // Heap conditioning dwell times
    static constexpr uint32_t kConditioningDwellMs = 3000;
    static constexpr uint32_t kConditioningStepMs = 100;
    static constexpr uint32_t kConditioningWarmupMs = 1000;
    static constexpr uint32_t kConditioningLogIntervalMs = 1000;
    static constexpr uint32_t kBrewDefaultDwellMs = 1000;
    static constexpr uint32_t kBrewAutoDwellMs = 1200;
    static constexpr uint32_t kBrewFileServerDwellMs = 2000;

    // Boot conditioning delays
    static constexpr int kBootFragYieldEvery = 10;
    static constexpr uint32_t kBootFragYieldDelayMs = 2;
    static constexpr uint32_t kBootStructAllocDelayMs = 1;
    static constexpr uint32_t kBootFreeDelayMs = 1;
    static constexpr uint32_t kBootTlsTestDelayMs = 1;
    static constexpr uint32_t kBootFinalDelayMs = 200;
}
