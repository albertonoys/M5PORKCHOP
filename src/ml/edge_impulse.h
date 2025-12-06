// Edge Impulse SDK Integration Scaffold
// =====================================
// This file provides the interface for integrating a trained Edge Impulse model.
//
// SETUP INSTRUCTIONS:
// 1. Train model at https://studio.edgeimpulse.com
// 2. Export as "C++ Library" for ESP32
// 3. Copy the edge-impulse-sdk folder to lib/
// 4. Copy model-parameters folder to lib/
// 5. Uncomment the #define EDGE_IMPULSE_ENABLED below
// 6. Rebuild project
//
// The model should be trained on WiFi feature vectors with labels:
//   0 = NORMAL, 1 = ROGUE_AP, 2 = EVIL_TWIN, 3 = DEAUTH_TARGET, 4 = VULNERABLE

#pragma once

#include <Arduino.h>
#include "features.h"

// Uncomment when Edge Impulse SDK is installed
// #define EDGE_IMPULSE_ENABLED

#ifdef EDGE_IMPULSE_ENABLED

// Include Edge Impulse generated headers
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"
#include "edge-impulse-sdk/dsp/numpy.hpp"

#endif

// Model metadata
#define EI_MODEL_NAME "porkchop_wifi_classifier"
#define EI_MODEL_VERSION "1.0.0"
#define EI_INPUT_SIZE FEATURE_VECTOR_SIZE
#define EI_OUTPUT_SIZE 5

// Labels match MLLabel enum
static const char* EI_LABELS[] = {
    "normal",
    "rogue_ap", 
    "evil_twin",
    "deauth_target",
    "vulnerable"
};

struct EIResult {
    float predictions[EI_OUTPUT_SIZE];
    int32_t predictedClass;
    float confidence;
    int32_t timingDsp;
    int32_t timingClassification;
    int32_t timingAnomaly;
    bool success;
};

class EdgeImpulse {
public:
    // Initialize Edge Impulse runtime
    static bool init() {
#ifdef EDGE_IMPULSE_ENABLED
        Serial.println("[EI] Edge Impulse SDK initialized");
        Serial.printf("[EI] Model: %s v%s\n", EI_MODEL_NAME, EI_MODEL_VERSION);
        Serial.printf("[EI] Input size: %d, Output size: %d\n", EI_IMPULSE_DSP_INPUT_FRAME_SIZE, EI_CLASSIFIER_LABEL_COUNT);
        return true;
#else
        Serial.println("[EI] Edge Impulse SDK not enabled - using heuristic classifier");
        return false;
#endif
    }
    
    // Run inference on feature vector
    static EIResult classify(const float* features, size_t size) {
        EIResult result = {0};
        result.success = false;
        
#ifdef EDGE_IMPULSE_ENABLED
        if (size != EI_IMPULSE_DSP_INPUT_FRAME_SIZE) {
            Serial.printf("[EI] Feature size mismatch: %d != %d\n", size, EI_IMPULSE_DSP_INPUT_FRAME_SIZE);
            return result;
        }
        
        // Create signal from features
        signal_t signal;
        signal.total_length = size;
        signal.get_data = [](size_t offset, size_t length, float *out_ptr) -> int {
            // This callback would need access to the features - use static/global
            return EIDSP_OK;
        };
        
        // Run classifier
        ei_impulse_result_t ei_result = {0};
        EI_IMPULSE_ERROR err = run_classifier(&signal, &ei_result, false);
        
        if (err != EI_IMPULSE_OK) {
            Serial.printf("[EI] Classifier error: %d\n", err);
            return result;
        }
        
        // Extract results
        result.timingDsp = ei_result.timing.dsp;
        result.timingClassification = ei_result.timing.classification;
        result.timingAnomaly = ei_result.timing.anomaly;
        
        float maxScore = 0.0f;
        for (int i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
            result.predictions[i] = ei_result.classification[i].value;
            if (result.predictions[i] > maxScore) {
                maxScore = result.predictions[i];
                result.predictedClass = i;
            }
        }
        
        result.confidence = maxScore;
        result.success = true;
#else
        // Stub - returns empty result when SDK not available
        Serial.println("[EI] SDK not enabled");
#endif
        
        return result;
    }
    
    // Classify WiFiFeatures directly
    static EIResult classifyNetwork(const WiFiFeatures& network) {
        float features[FEATURE_VECTOR_SIZE];
        FeatureExtractor::toFeatureVector(network, features);
        return classify(features, FEATURE_VECTOR_SIZE);
    }
    
    // Get label string for class index
    static const char* getLabel(int classIdx) {
        if (classIdx >= 0 && classIdx < EI_OUTPUT_SIZE) {
            return EI_LABELS[classIdx];
        }
        return "unknown";
    }
    
    // Check if SDK is available
    static bool isEnabled() {
#ifdef EDGE_IMPULSE_ENABLED
        return true;
#else
        return false;
#endif
    }
    
    // Print model info
    static void printInfo() {
        Serial.println("=== Edge Impulse Model Info ===");
        Serial.printf("Name: %s\n", EI_MODEL_NAME);
        Serial.printf("Version: %s\n", EI_MODEL_VERSION);
        Serial.printf("Input features: %d\n", EI_INPUT_SIZE);
        Serial.printf("Output classes: %d\n", EI_OUTPUT_SIZE);
        Serial.println("Labels:");
        for (int i = 0; i < EI_OUTPUT_SIZE; i++) {
            Serial.printf("  %d: %s\n", i, EI_LABELS[i]);
        }
#ifdef EDGE_IMPULSE_ENABLED
        Serial.println("Status: ENABLED");
#else
        Serial.println("Status: DISABLED (using heuristics)");
#endif
    }
};

// =====================================
// DATA COLLECTION HELPER
// =====================================
// Use this to collect training data for Edge Impulse

class EIDataCollector {
public:
    // Format features as Edge Impulse data forwarder format
    static void printForDataForwarder(const WiFiFeatures& network, int label) {
        float features[FEATURE_VECTOR_SIZE];
        FeatureExtractor::toFeatureVector(network, features);
        
        // Edge Impulse data forwarder expects comma-separated values
        for (int i = 0; i < FEATURE_VECTOR_SIZE; i++) {
            Serial.printf("%.4f", features[i]);
            if (i < FEATURE_VECTOR_SIZE - 1) Serial.print(",");
        }
        Serial.printf(",%d\n", label);  // Label at end
    }
    
    // Print CSV header for manual data collection
    static void printCSVHeader() {
        Serial.print("rssi,noise,snr,channel,secondary_ch,beacon_interval,");
        Serial.print("capability_lo,capability_hi,has_wps,has_wpa,has_wpa2,has_wpa3,");
        Serial.print("is_hidden,response_time,beacon_count,beacon_jitter,");
        Serial.print("responds_probe,probe_response_time,vendor_ie_count,");
        Serial.print("supported_rates,ht_cap,vht_cap,anomaly_score,");
        Serial.println("f23,f24,f25,f26,f27,f28,f29,f30,f31,label");
    }
};
