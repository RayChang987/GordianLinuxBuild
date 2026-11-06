// GORDIAN global placement: quadratic placement + recursive bipartitioning.
// Originally written by Kangli Chu (CSE788, Assignment 3); refactored for Linux.
#ifndef GORDIAN_H_
#define GORDIAN_H_

#include <array>
#include <string>
#include <vector>

#include <armadillo>

namespace gordian {

// Regions holding at most this many movable cells are not partitioned further.
constexpr int kMaxCellsPerRegion = 5;
// Weak spring pulling floating cells (see Cell::floating) to their region
// center; without it their position is undetermined (singular system).
constexpr float kFloatingAnchorWeight = 0.01f;

struct Options {
    std::string outputDir = "output";
    bool writeSnapshots = true;  // text snapshots for scripts/visualize.py
    bool writeBmp = false;       // legacy EasyBMP frames
};

// Axis 0 spans the chip height H, axis 1 spans the chip width W
// (kept from the original implementation).
using Vec2 = std::array<float, 2>;

struct Cell {
    Vec2 size{0, 0};
    float area = 0;
    Vec2 pos{0, 0};  // center
    bool fixed = false;
    bool floating = false;  // movable and not connected (via nets) to any fixed cell
    int solveIndex = -1;  // row/column in the connectivity matrix (movable cells only)
    int region = 0;
};

struct Pin {
    int cell;
    Vec2 offset;  // relative to the cell center
};

struct Net {
    std::vector<Pin> pins;
};

struct Region {
    Vec2 center;  // also the running cut position while the region is split
    Vec2 lo;
    Vec2 hi;
    int cellCount = 0;  // movable cells assigned to this region
};

struct Result {
    int iterations = 0;
    int regions = 0;
    float wirelength = 0;
    double seconds = 0;
};

class Placer {
   public:
    explicit Placer(Options options);

    void loadNetlist(const std::string &path);  // throws std::runtime_error
    Result run();

   private:
    using RegionMembers = std::vector<std::vector<int>>;  // region -> cell ids

    void buildConnectivity();
    int markFloatingCells();
    RegionMembers collectRegionMembers() const;
    void solve(const RegionMembers &members);
    int partition(const RegionMembers &members);
    bool splitRegion(const std::vector<int> &cells, int reg);
    void findBalancedCut(const std::vector<int> &cells, int reg, int newReg,
                         int axis);

    struct SplitStats {
        float areaOld = 0, areaNew = 0;
        float widthOld = 0, widthNew = 0;
    };
    SplitStats assignToSides(const std::vector<int> &cells, int reg, int newReg,
                             int axis);

    float wirelength() const;
    void writeFrame(int frame) const;
    void writeSnapshot(const std::string &path, int frame) const;
    void writeBmp(const std::string &path) const;

    Options options_;
    int width_ = 0;   // W
    int height_ = 0;  // H
    int numMovable_ = 0;
    std::vector<Cell> cells_;
    std::vector<Net> nets_;
    std::vector<Region> regions_;

    arma::mat C_;                 // quadratic wirelength matrix (movable x movable)
    std::array<std::vector<float>, 2> d_;  // linear terms per axis
};

}  // namespace gordian

#endif  // GORDIAN_H_
