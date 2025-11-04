# 3D Clustering and Event Classification Architecture

**Author:** Kazimierz Gofron  
**Institution:** Oak Ridge National Laboratory  
**Created:** November 2, 2025  
**Modified:** November 4, 2025

## Overview

This document outlines the architecture for future implementation of 3D spatial-temporal clustering and event classification for Timepix3 detector data.

## Phase 1: Core Parser (Completed)

✅ TCP server listening on port 8085  
✅ Complete packet decoding (pixel, TDC, control, time extension)  
✅ Timestamp extension using experimental extra packets  
✅ Hit buffering and statistics tracking  

## Phase 2: Time Alignment (Future)

### Requirement
Sort hits by timestamp before clustering to ensure temporal consistency.

### Implementation Approach

```cpp
// Pseudocode for time sorting
void TimeAligner::sortByTimestamp(std::vector<PixelHit>& hits) {
    std::sort(hits.begin(), hits.end(), 
        [](const PixelHit& a, const PixelHit& b) {
            return a.toa_ns < b.toa_ns;
        });
}
```

### Time Window Processing
- Process hits in configurable time windows (e.g., 1ms, 100ms)
- Overlap windows to handle edge cases
- Maintain sorted order throughout processing

## Phase 3: 3D Spatial-Temporal Clustering (Future)

### Clustering Strategy

Define a 3D distance metric combining spatial and temporal dimensions:

```
Distance = sqrt(alpha * dx² + beta * dy² + gamma * dt²)
```

Where:
- dx, dy: spatial pixel separation
- dt: temporal separation in nanoseconds
- alpha, beta, gamma: weighting factors

### Implementation Options

#### Option A: Connected Component Labeling (OpenCV)
```cpp
#include <opencv2/imgproc.hpp>

class ClusteringProcessor {
    cv::Mat create_labeled_image(const std::vector<PixelHit>& hits, 
                                  uint64_t time_window);
    std::vector<Cluster> extract_clusters(const cv::Mat& labels);
};
```

Pros:
- Mature, well-tested library
- Good for 2D spatial clustering
- Requires time slicing for 3D approach

Cons:
- May need custom extensions for 3D temporal dimension
- Additional dependency

#### Option B: Intel Performance Primitives (IPP)
```cpp
#include <ipp.h>

class ClusteringProcessor {
    void compute_distances(const float* points, int num_points, 
                          float* distances);
    void cluster_by_distance(float threshold);
};
```

Pros:
- Hardware-accelerated vector operations
- Very fast for distance calculations
- Good for large datasets

Cons:
- Requires Intel hardware or optimized build
- More complex setup

#### Option C: Custom DBSCAN Implementation
```cpp
class DBSCANClusterer {
    struct Point3D { float x, y, t; };
    
    std::vector<std::vector<size_t>> cluster(
        const std::vector<Point3D>& points, 
        float eps, 
        size_t min_samples);
};
```

Pros:
- Full control over algorithm
- No external dependencies
- Can optimize for specific use case

Cons:
- Needs careful implementation
- May be slower than optimized libraries

### Recommended Approach

**Hybrid: Custom DBSCAN with IPP acceleration**

1. Use DBSCAN algorithm for flexible 3D clustering
2. Use IPP for distance matrix computation (if available)
3. Fall back to OpenCV or pure C++ if IPP unavailable

## Phase 4: Centroid Extraction (Future)

### Method

For each cluster, compute weighted centroid:

```cpp
struct ClusterCentroid {
    float x, y;           // Spatial centroid
    uint64_t toa_ns;      // Temporal centroid
    float tot_sum;        // Total energy
    size_t pixel_count;   // Cluster size
    float spread;         // Spatial spread
};

ClusterCentroid compute_centroid(const std::vector<PixelHit>& cluster) {
    ClusterCentroid c = {0, 0, 0, 0, 0, 0};
    
    // ToT-weighted centroid
    double tot_sum = 0;
    for (const auto& hit : cluster) {
        c.x += hit.x * hit.tot_ns;
        c.y += hit.y * hit.tot_ns;
        c.toa_ns += hit.toa_ns;
        c.tot_sum += hit.tot_ns;
        tot_sum += hit.tot_ns;
        c.pixel_count++;
    }
    
    c.x /= tot_sum;
    c.y /= tot_sum;
    c.toa_ns /= cluster.size();
    
    // Compute spread
    for (const auto& hit : cluster) {
        c.spread += std::sqrt(
            std::pow(hit.x - c.x, 2) + 
            std::pow(hit.y - c.y, 2)
        );
    }
    c.spread /= cluster.size();
    
    return c;
}
```

## Phase 5: Event Classification (Future)

### Classification Features

1. **Cluster Size**: Number of pixels
2. **Total Energy**: Sum of ToT values
3. **Cluster Spread**: Spatial extent (RMS)
4. **Energy Density**: TOT / pixel_count
5. **Temporal Width**: Max - min ToA within cluster
6. **Shape Descriptors**: Aspect ratio, compactness

### Particle Type Characteristics

#### Neutrons
- Large cluster size (50-500 pixels)
- Low energy density
- Broad spatial distribution
- Often with secondary interactions

#### Electrons
- Small to medium clusters (5-50 pixels)
- Low to medium energy density
- Compact, circular shape
- High temporal correlation

#### X-rays
- Very small clusters (1-5 pixels)
- Medium to high energy density
- Single or few pixels
- Minimal spread

#### Ions
- Medium clusters (10-100 pixels)
- High energy density
- Elongated track shape
- Clear directional pattern

### Classification Approaches

#### Option A: Rule-Based Classifier
```cpp
enum ParticleType { NEUTRON, ELECTRON, XRAY, ION, UNKNOWN };

ParticleType classify(const ClusterCentroid& centroid) {
    if (centroid.pixel_count <= 5 && centroid.tot_sum < 1000) {
        return XRAY;
    }
    // ... more rules
}
```

#### Option B: Machine Learning Classifier
```cpp
#include <mlpack/core.hpp>
#include <mlpack/methods/random_forest.hpp>

class MLClassifier {
    mlpack::tree::RandomForest<mlpack::tree::GiniGain> model;
    
public:
    ParticleType classify(const ClusterCentroid& centroid);
    void train(const std::vector<LabeledCluster>& training_data);
};
```

### Recommendation

Start with rule-based classifier for early validation, then move to ML if accuracy needs improvement.

## Interface Design

### Abstract Clustering Interface

```cpp
class ClusterProcessor {
public:
    virtual ~ClusterProcessor() = default;
    
    // Add hit to buffer
    virtual void addHit(const PixelHit& hit) = 0;
    
    // Process hits in current time window
    virtual void processTimeWindow(uint64_t window_ns) = 0;
    
    // Extract cluster centroids
    virtual std::vector<ClusterCentroid> extractCentroids() = 0;
    
    // Clear buffers
    virtual void clear() = 0;
};
```

### Event Classifier Interface

```cpp
class EventClassifier {
public:
    virtual ~EventClassifier() = default;
    
    // Classify a single cluster
    virtual ParticleType classify(const ClusterCentroid& centroid) = 0;
    
    // Batch classify
    virtual std::vector<ParticleType> classifyBatch(
        const std::vector<ClusterCentroid>& centroids) = 0;
};
```

## Integration into Main Parser

```cpp
int main(int argc, char* argv[]) {
    // ... existing setup ...
    
    // Add clustering processor
    std::unique_ptr<ClusterProcessor> clusterer;
    clusterer = std::make_unique<DBSCANClusterer>(eps=5.0, min_samples=3);
    
    // Add event classifier
    std::unique_ptr<EventClassifier> classifier;
    classifier = std::make_unique<RuleBasedClassifier>();
    
    server.run([&](const uint8_t* data, size_t size) {
        process_raw_data(data, size, processor, chunk_meta);
        
        // Process hits through clustering
        for (const auto& hit : processor.getHits()) {
            clusterer->addHit(hit);
        }
        
        // Process time window every 1ms
        clusterer->processTimeWindow(1'000'000ULL);
        
        // Extract and classify clusters
        auto centroids = clusterer->extractCentroids();
        auto classifications = classifier->classifyBatch(centroids);
        
        // Publish results (future EPICS integration)
        // publish_to_epics(centroids, classifications);
    });
}
```

## Performance Considerations

### Expected Data Rates
- Max hit rate: ~1M hits/second
- Typical cluster: 5-50 pixels
- Time window: 1-100ms

### Optimization Strategies
1. **Pre-allocate buffers**: Avoid dynamic allocation in hot paths
2. **SIMD acceleration**: Use IPP or manual SIMD for distance calculations
3. **Parallel processing**: Process multiple time windows in parallel
4. **Memory pools**: Reuse cluster and centroid structures

## Testing Strategy

1. **Unit tests**: Individual clustering and classification functions
2. **Integration tests**: Full pipeline with recorded data
3. **Performance tests**: Benchmark against Python reference
4. **Validation**: Compare results with known particle types

## Future Enhancements

1. **GPU acceleration**: CUDA/OpenCL for clustering on GPU
2. **Online learning**: Adaptive classification with feedback
3. **Multi-chip support**: Coordinate clustering across detector chips
4. **Event reconstruction**: Track particle trajectories
5. **EPICS integration**: Real-time PV publishing

## References

- DBSCAN: Ester et al., "A density-based algorithm for discovering clusters"
- OpenCV: Connected Components
- Intel IPP: Vector Mathematical Functions
- Timepix3 clustering: Various neutron imaging papers

