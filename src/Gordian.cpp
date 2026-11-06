#include "Gordian.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

#include "EasyBMP.h"
#include "EasyBMP_Geometry.h"

namespace fs = std::filesystem;

namespace gordian {

namespace {

// Safety net against a cut search that never reaches its stop condition.
constexpr long kMaxCutSteps = 10000000;
// Safety net against partitioning that never converges.
constexpr int kMaxIterations = 1000;

std::string trim(const std::string &s) {
    const char *ws = " \t\r\n";
    const auto b = s.find_first_not_of(ws);
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(ws) - b + 1);
}

[[noreturn]] void parseError(const std::string &path, int lineNo,
                             const std::string &msg) {
    std::ostringstream os;
    os << path << ":" << lineNo << ": " << msg;
    throw std::runtime_error(os.str());
}

}  // namespace

Placer::Placer(Options options) : options_(std::move(options)) {}

// ---------------------------------------------------------------------------
// Netlist parsing
//
//   Num_Cells=<n>  Num_Mcells=<n>  Num_Fcells=<n>  Num_Nets=<n>  W=<w>  H=<h>
//   N(<net id>)
//   C(<cell id>)(<w>,<h>)(<pin dx>,<pin dy>)M          movable cell
//   C(<cell id>)(<w>,<h>)(<pin dx>,<pin dy>)F(<x>,<y>) fixed cell
// ---------------------------------------------------------------------------
void Placer::loadNetlist(const std::string &path) {
    std::ifstream in(path);
    if (!in) throw std::runtime_error("cannot open netlist file: " + path);

    int numCells = -1, numMcells = -1, numFcells = -1, numNets = -1;
    std::vector<bool> referenced;
    std::string raw;
    int lineNo = 0;

    while (std::getline(in, raw)) {
        ++lineNo;
        const std::string line = trim(raw);
        if (line.empty()) continue;

        const bool isNet = line.rfind("N(", 0) == 0;
        const bool isCell = line.rfind("C(", 0) == 0;

        if (!isNet && !isCell) {  // header: Key=Value
            if (!cells_.empty()) parseError(path, lineNo, "header after body");
            const auto eq = line.find('=');
            if (eq == std::string::npos) parseError(path, lineNo, "unrecognised line");
            const std::string key = trim(line.substr(0, eq));
            int value;
            try {
                value = std::stoi(line.substr(eq + 1));
            } catch (const std::exception &) {
                parseError(path, lineNo, "bad number for " + key);
            }
            if (key == "Num_Cells") numCells = value;
            else if (key == "Num_Mcells") numMcells = value;
            else if (key == "Num_Fcells") numFcells = value;
            else if (key == "Num_Nets") numNets = value;
            else if (key == "W") width_ = value;
            else if (key == "H") height_ = value;
            else parseError(path, lineNo, "unknown header key " + key);
            continue;
        }

        if (cells_.empty()) {  // first body line: header must be complete
            if (numCells <= 0 || width_ <= 0 || height_ <= 0)
                parseError(path, lineNo, "missing Num_Cells, W or H in header");
            cells_.resize(numCells);
            referenced.assign(numCells, false);
            for (Cell &c : cells_) {
                c.pos = {float(height_ / 2), float(width_ / 2)};
            }
        }

        if (isNet) {
            nets_.emplace_back();
            continue;
        }

        if (nets_.empty()) parseError(path, lineNo, "cell listed before any net");
        int id, w, h, dx, dy, fx = 0, fy = 0;
        char kind = 0;
        const int n = std::sscanf(line.c_str(), "C(%d)(%d,%d)(%d,%d)%c(%d,%d)", &id,
                                  &w, &h, &dx, &dy, &kind, &fx, &fy);
        if (n < 6 || (kind != 'M' && kind != 'F') || (kind == 'F' && n != 8))
            parseError(path, lineNo, "malformed cell line");
        if (id < 0 || id >= numCells)
            parseError(path, lineNo, "cell id out of range");

        Cell &cell = cells_[id];
        cell.size = {float(w), float(h)};
        cell.area = float(w) * float(h);
        if (kind == 'F') {
            cell.fixed = true;
            cell.pos = {float(fx), float(fy)};
        }
        referenced[id] = true;
        nets_.back().pins.push_back({id, {float(dx), float(dy)}});
    }

    if (cells_.empty()) throw std::runtime_error(path + ": netlist has no body");

    int fixedCount = 0;
    for (Cell &c : cells_) {
        if (c.fixed) {
            ++fixedCount;
        } else {
            c.solveIndex = numMovable_++;
        }
    }
    int unreferenced = 0;
    for (bool r : referenced) unreferenced += !r;

    auto warnMismatch = [&](const char *what, int header, int actual) {
        if (header >= 0 && header != actual)
            std::cerr << "warning: header says " << what << "=" << header
                      << " but netlist has " << actual << "\n";
    };
    warnMismatch("Num_Nets", numNets, int(nets_.size()));
    warnMismatch("Num_Mcells", numMcells, numMovable_);
    warnMismatch("Num_Fcells", numFcells, fixedCount);
    if (unreferenced)
        std::cerr << "warning: " << unreferenced
                  << " cells are not connected to any net\n";
    if (numMovable_ == 0) throw std::runtime_error(path + ": no movable cells");
    if (const int floating = markFloatingCells())
        std::cerr << "warning: " << floating << " movable cells have no path to a "
                     "fixed cell; anchoring them weakly to their region center\n";
}

// Union-find over nets; a component is anchored if any of its nets touches a
// fixed cell. Returns the number of movable cells in unanchored components.
int Placer::markFloatingCells() {
    std::vector<int> parent(cells_.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = int(i);
    auto find = [&](int x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    std::vector<int> netRoot;
    for (const Net &net : nets_) {
        int root = -1;
        for (const Pin &p : net.pins) {
            if (cells_[p.cell].fixed) continue;
            if (root < 0) root = find(p.cell);
            else parent[find(p.cell)] = root;
        }
        netRoot.push_back(root);
    }
    std::vector<bool> anchored(cells_.size(), false);
    for (size_t n = 0; n < nets_.size(); ++n) {
        if (netRoot[n] < 0) continue;
        for (const Pin &p : nets_[n].pins)
            if (cells_[p.cell].fixed) anchored[find(netRoot[n])] = true;
    }
    int floating = 0;
    for (size_t i = 0; i < cells_.size(); ++i) {
        Cell &c = cells_[i];
        c.floating = !c.fixed && !anchored[find(int(i))];
        floating += c.floating;
    }
    return floating;
}

// ---------------------------------------------------------------------------
// Quadratic objective: sum over nets of squared pin-to-pin distances
// (clique model, weight 2/|net|) -> x^T C x + 2 d^T x, separately per axis.
// ---------------------------------------------------------------------------
void Placer::buildConnectivity() {
    C_.zeros(numMovable_, numMovable_);
    for (auto &d : d_) d.assign(numMovable_, 0.0f);

    for (const Net &net : nets_) {
        const int n = int(net.pins.size());
        const float e = 2.0 / float(n);
        for (const Pin &pj : net.pins) {
            const Cell &u = cells_[pj.cell];
            if (u.fixed) continue;
            const int c = u.solveIndex;
            C_(c, c) += e * (n - 1);
            for (int a = 0; a < 2; ++a) d_[a][c] += pj.offset[a] * e * (n - 1);

            for (const Pin &pl : net.pins) {
                if (pl.cell == pj.cell) continue;
                const Cell &v = cells_[pl.cell];
                if (!v.fixed) {
                    C_(c, v.solveIndex) -= e;
                    for (int a = 0; a < 2; ++a) d_[a][c] -= pl.offset[a] * e;
                } else {
                    for (int a = 0; a < 2; ++a) d_[a][c] -= e * (v.pos[a] + pl.offset[a]);
                }
            }
        }
    }
}

// Movable cells grouped by region. The first cell of each region is its
// "pivot" (eliminated variable in the constraint matrix).
Placer::RegionMembers Placer::collectRegionMembers() const {
    RegionMembers members(regions_.size());
    for (int i = 0; i < int(cells_.size()); ++i) {
        if (cells_[i].fixed) continue;
        auto &m = members[cells_[i].region];
        if (!m.empty() && cells_[i].area > cells_[m.front()].area)
            m.insert(m.begin(), i);
        else
            m.push_back(i);
    }
    return members;
}

// ---------------------------------------------------------------------------
// Minimise the quadratic objective subject to one linear constraint per
// region: the area-weighted center of mass of its cells equals the region
// center (A x = u). Variables are permuted so A = [D | B] with D diagonal
// (one pivot per region); then x = Z y + x0 with Z = [-D^-1 B; I] spans the
// null space of A and the unconstrained problem in y is solved directly.
// ---------------------------------------------------------------------------
void Placer::solve(const RegionMembers &members) {
    using arma::mat;
    const int R = int(regions_.size());
    const int M = numMovable_;

    // order[p] = cell id at permuted position p: pivots first, then the
    // remaining cells region by region.
    std::vector<int> order;
    order.reserve(M);
    for (const auto &m : members) {
        if (m.empty()) throw std::logic_error("solve(): empty region");
        order.push_back(m.front());
    }
    for (const auto &m : members) order.insert(order.end(), m.begin() + 1, m.end());

    mat A(R, M, arma::fill::zeros);
    for (int r = 0, p = R; r < R; ++r) {
        const auto &m = members[r];
        A(r, r) = cells_[m.front()].area;
        for (size_t k = 1; k < m.size(); ++k) A(r, p++) = cells_[m[k]].area;
    }
    const mat rowSum = arma::sum(A, 1);
    for (int r = 0; r < R; ++r) A.row(r) = (1 / rowSum(r, 0)) * A.row(r);

    mat Cp(M, M);
    for (int j = 0; j < M; ++j) {
        const int cj = cells_[order[j]].solveIndex;
        for (int i = 0; i < M; ++i) Cp(i, j) = C_(cells_[order[i]].solveIndex, cj);
        if (cells_[order[j]].floating) Cp(j, j) += kFloatingAnchorWeight;
    }

    const mat D = A.submat(0, 0, R - 1, R - 1);
    const mat B = A.submat(0, R, R - 1, M - 1);
    const mat invD = arma::inv(D);
    const mat Z = arma::join_cols(-invD * B, arma::eye(M - R, M - R));
    const mat Zt = arma::trans(Z);
    const mat H = Zt * Cp * Z;

    mat invH;
    if (!arma::inv(invH, H) || !invH.is_finite()) {
        std::cerr << "warning: singular system (cells not anchored to fixed "
                     "cells?), using pseudo-inverse\n";
        invH = arma::pinv(H);
    }

    const mat zeros(M - R, 1, arma::fill::zeros);
    for (int a = 0; a < 2; ++a) {
        mat u(R, 1), d(M, 1);
        for (int r = 0; r < R; ++r) u(r, 0) = regions_[r].center[a];
        for (int p = 0; p < M; ++p) {
            const Cell &cell = cells_[order[p]];
            d(p, 0) = d_[a][cell.solveIndex];
            if (cell.floating)
                d(p, 0) -= kFloatingAnchorWeight * regions_[cell.region].center[a];
        }

        const mat x0 = arma::join_cols(invD * u, zeros);
        const mat c = arma::trans(arma::trans(Cp * x0 + d) * Z);
        const mat y = -invH * c;
        const mat x = Z * y + x0;
        for (int p = 0; p < M; ++p) cells_[order[p]].pos[a] = float(x(p, 0));
    }
}

// ---------------------------------------------------------------------------
// Partitioning
// ---------------------------------------------------------------------------

// Assign the region's cells to the old/new side of the current cut line.
Placer::SplitStats Placer::assignToSides(const std::vector<int> &cells, int reg,
                                         int newReg, int axis) {
    Region &oldR = regions_[reg];
    Region &newR = regions_[newReg];
    const float cut = oldR.center[axis];
    SplitStats s;
    oldR.cellCount = newR.cellCount = 0;
    for (int id : cells) {
        Cell &cell = cells_[id];
        if (cell.pos[axis] > cut) {
            cell.region = newReg;
            ++newR.cellCount;
            s.areaNew += cell.area;
        } else {
            cell.region = reg;
            ++oldR.cellCount;
            s.areaOld += cell.area;
        }
    }
    s.widthOld = cut - oldR.lo[axis];
    s.widthNew = oldR.hi[axis] - cut;
    return s;
}

// Sweep the cut line from the region center toward the smaller side and keep
// the position whose (width ratio) / (area ratio) is closest to 1.
void Placer::findBalancedCut(const std::vector<int> &cells, int reg, int newReg,
                             int axis) {
    float &cut = regions_[reg].center[axis];  // regions_ is not resized here
    float best = cut;
    float bestRatio = 0;
    float sign = 0;

    cut -= 0.5f;
    for (long step = 0;; ++step) {
        const SplitStats s = assignToSides(cells, reg, newReg, axis);
        if (sign == 0) {
            if (s.areaOld < s.areaNew) sign = 1;
            else if (s.areaOld > s.areaNew) sign = -1;
            else return;  // already balanced
        }
        const bool atLimit = s.widthOld <= 2 || s.widthNew <= 2 ||
                             s.areaNew < 4 || s.areaOld < 4;
        if (atLimit || step >= kMaxCutSteps) {
            cut = best;
            assignToSides(cells, reg, newReg, axis);
            return;
        }
        const float ratio = (s.widthOld * s.areaNew) / (s.areaOld * s.widthNew);
        if (std::abs(ratio - 1) < std::abs(bestRatio - 1)) {
            bestRatio = ratio;
            best = cut;
        }
        cut += sign / regions_[reg].cellCount;
    }
}

// Cut region `reg` in two along its longer side. Returns false (and leaves
// the region untouched) if no cut separates its cells.
bool Placer::splitRegion(const std::vector<int> &cells, int reg) {
    const Region saved = regions_[reg];
    const int newReg = int(regions_.size());
    regions_.push_back(Region{});

    const int axis = (saved.hi[0] - saved.lo[0] >= saved.hi[1] - saved.lo[1]) ? 0 : 1;
    findBalancedCut(cells, reg, newReg, axis);

    if (regions_[reg].cellCount == 0 || regions_[newReg].cellCount == 0) {
        for (int id : cells) cells_[id].region = reg;
        regions_.pop_back();
        regions_[reg] = saved;
        return false;
    }

    Region &oldR = regions_[reg];
    Region &newR = regions_[newReg];
    const float cut = oldR.center[axis];
    newR.lo = oldR.lo;
    newR.hi = oldR.hi;
    newR.center = oldR.center;
    newR.lo[axis] = cut;
    newR.center[axis] = (newR.hi[axis] + cut) / 2;
    oldR.hi[axis] = cut;
    oldR.center[axis] = (cut + oldR.lo[axis]) / 2;
    return true;
}

// Split every region holding more than kMaxCellsPerRegion cells.
// Returns the number of regions that were split.
int Placer::partition(const RegionMembers &members) {
    const int R = int(regions_.size());
    int splits = 0;
    for (int reg = 0; reg < R; ++reg) {
        if (regions_[reg].cellCount <= kMaxCellsPerRegion) continue;
        splits += splitRegion(members[reg], reg);
    }
    return splits;
}

// ---------------------------------------------------------------------------
// Metrics and output
// ---------------------------------------------------------------------------

// Star-model wirelength: Euclidean distance of every pin to its net's centroid.
float Placer::wirelength() const {
    float total = 0;
    for (const Net &net : nets_) {
        if (net.pins.empty()) continue;
        Vec2 center{0, 0};
        for (const Pin &p : net.pins)
            for (int a = 0; a < 2; ++a) center[a] += cells_[p.cell].pos[a] + p.offset[a];
        for (float &v : center) v /= net.pins.size();
        for (const Pin &p : net.pins) {
            const float dx = cells_[p.cell].pos[0] + p.offset[0] - center[0];
            const float dy = cells_[p.cell].pos[1] + p.offset[1] - center[1];
            total += std::sqrt(dx * dx + dy * dy);
        }
    }
    return total;
}

void Placer::writeSnapshot(const std::string &path, int frame) const {
    std::ofstream out(path);
    if (!out) throw std::runtime_error("cannot write " + path);
    out.precision(9);
    out << "# Gordian placement snapshot (axis 0 spans H, axis 1 spans W)\n";
    out << "frame " << frame << "\n";
    out << "chip " << height_ << " " << width_ << "\n";
    out << "wirelength " << wirelength() << "\n";
    out << "regions " << regions_.size() << "\n";
    for (const Region &r : regions_)
        out << r.lo[0] << " " << r.lo[1] << " " << r.hi[0] << " " << r.hi[1] << "\n";
    out << "cells " << cells_.size() << "\n";
    for (const Cell &c : cells_)
        out << c.pos[0] << " " << c.pos[1] << " " << c.size[0] << " " << c.size[1]
            << " " << int(c.fixed) << " " << c.region << "\n";
}

void Placer::writeBmp(const std::string &path) const {
    constexpr int kScale = 10, kMargin = 200;
    auto px = [&](float v) { return int(kScale * v) + kMargin; };
    auto rect = [](BMP &img, int x0, int y0, int x1, int y1, RGBApixel color) {
        DrawFastLine(img, x0, y0, x1, y0, color);
        DrawFastLine(img, x0, y0, x0, y1, color);
        DrawFastLine(img, x1, y1, x0, y1, color);
        DrawFastLine(img, x1, y1, x1, y0, color);
    };

    BMP image;
    image.SetSize(width_ * kScale + 2 * kMargin, height_ * kScale + 2 * kMargin);
    RGBApixel red{0, 0, 255, 0};  // EasyBMP order: Blue, Green, Red, Alpha
    RGBApixel black{0, 0, 0, 0};
    for (const Cell &c : cells_)
        rect(image, px(c.pos[1] - c.size[1] / 2), px(c.pos[0] - c.size[0] / 2),
             px(c.pos[1] + c.size[1] / 2), px(c.pos[0] + c.size[0] / 2), red);
    for (const Region &r : regions_)
        rect(image, px(r.lo[1]), px(r.lo[0]), px(r.hi[1]), px(r.hi[0]), black);
    image.WriteToFile(path.c_str());
}

void Placer::writeFrame(int frame) const {
    char name[32];
    if (options_.writeSnapshots) {
        std::snprintf(name, sizeof name, "frame_%03d.txt", frame);
        writeSnapshot((fs::path(options_.outputDir) / "snapshots" / name).string(), frame);
    }
    if (options_.writeBmp) {
        std::snprintf(name, sizeof name, "frame_%03d.bmp", frame);
        writeBmp((fs::path(options_.outputDir) / "bmp" / name).string());
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------
Result Placer::run() {
    const fs::path outDir(options_.outputDir);
    fs::create_directories(outDir);
    if (options_.writeSnapshots) fs::create_directories(outDir / "snapshots");
    if (options_.writeBmp) fs::create_directories(outDir / "bmp");

    const auto start = std::chrono::steady_clock::now();
    buildConnectivity();

    Region chip;
    chip.center = {float(height_ / 2), float(width_ / 2)};
    chip.lo = {0, 0};
    chip.hi = {float(height_), float(width_)};
    chip.cellCount = numMovable_;
    regions_.assign(1, chip);
    for (Cell &c : cells_) c.region = 0;

    writeFrame(0);  // initial placement
    Result result;
    while (true) {
        const RegionMembers members = collectRegionMembers();
        solve(members);
        ++result.iterations;
        const int splits = partition(members);
        writeFrame(result.iterations);  // this solution + the cuts derived from it
        std::printf("iteration %3d: %4zu regions, wirelength %.3f\n", result.iterations,
                    regions_.size(), wirelength());
        if (splits == 0) break;
        if (result.iterations >= kMaxIterations) {
            std::cerr << "warning: stopped after " << kMaxIterations << " iterations\n";
            break;
        }
    }
    result.seconds = std::chrono::duration<double>(
                         std::chrono::steady_clock::now() - start).count();
    result.regions = int(regions_.size());
    result.wirelength = wirelength();

    std::ofstream placement(outDir / "placement.txt");
    placement << "# cell_id pos0 pos1 fixed\n";
    for (size_t i = 0; i < cells_.size(); ++i)
        placement << i << " " << cells_[i].pos[0] << " " << cells_[i].pos[1] << " "
                  << int(cells_[i].fixed) << "\n";
    return result;
}

}  // namespace gordian
