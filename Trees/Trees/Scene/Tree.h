#pragma once


#include "glm\glm.hpp"
#include "glm/gtx/norm.hpp"

#include "Globals.h"
#include "AttractorPointCloud.h"
#include "..\CUDA\kernels.h"

#include <vector>
#include <chrono>
#include <ctime>

/// User-defined Parameters for the growth simulation

// For Space Colonization
#define INITIAL_NUM_ITERATIONS 25
#define INTERNODE_SCALE 0.04f
#define INITIAL_BRANCH_RADIUS 0.1f
#define INITIAL_BUD_INTERNODE_RADIUS INTERNODE_SCALE
#define COS_THETA 0.70710678118f // cos(pi/4)
#define COS_THETA_SMALL 0.86602540378f // cos(pi6)

// For BH Model
#define ALPHA 1.0f // proportionality constant for resource flow computation
#define LAMBDA 0.51f

// For Addition of new shoots
#define OPTIMAL_GROWTH_DIR_WEIGHT 0.4f
#define TROPISM_DIR_WEIGHT 0.0f
#define TROPISM_VECTOR glm::vec3(0.0f, -1.0f, 0.0f)

// For branch radius computation
#define MINIMUM_BRANCH_RADIUS 0.1f // Radius of outermost branches
#define PIPE_EXPONENT 2.8f // somewhere between 2 and 3 usually according to the paper
#define MAXIMUM_BRANCH_RADIUS 0.05f

/// Definition of structures

struct TreeParameters {
    float initialBranchRadius;
    float initialBudInternodeRadius;
    float perceptionCosTheta;
    float perceptionCosThetaSmall;
    float BHAlpha;
    float BHLambda;
    float optimalGrowthDirWeight;
    float tropismDirWeight;
    glm::vec3 tropismVector;
    float minimumBranchRadius;
    float pipeModelExponent;
    float maximumBranchRadius;
    int numSpaceColonizationIterations;

    TreeParameters() :
        initialBranchRadius(INITIAL_BRANCH_RADIUS), initialBudInternodeRadius(INITIAL_BUD_INTERNODE_RADIUS), perceptionCosTheta(COS_THETA), perceptionCosThetaSmall(COS_THETA_SMALL),
        BHAlpha(ALPHA), BHLambda(LAMBDA), optimalGrowthDirWeight(OPTIMAL_GROWTH_DIR_WEIGHT), tropismDirWeight(TROPISM_DIR_WEIGHT), tropismVector(TROPISM_DIR_WEIGHT),
        minimumBranchRadius(MINIMUM_BRANCH_RADIUS), pipeModelExponent(PIPE_EXPONENT), numSpaceColonizationIterations(INITIAL_NUM_ITERATIONS) {}
};

enum BUD_FATE {
    DORMANT,
    FORMED_BRANCH,
    FORMED_FLOWER,
    ABORT // TODO: when does this happen?
};

enum BUD_TYPE {
    TERMINAL, // at the end of a branch
    AXILLARY // along the sides of a branch
};

// Store any information relevant to a particular bud
struct Bud {
    glm::vec3 point;
    glm::vec3 naturalGrowthDir; // Growth direction of this bud. Use Golden Angle (137.5 degrees) for axillary buds.
    glm::vec3 optimalGrowthDir; // optimal growth direction computing during space colonization
    float environmentQuality; // In space colonization, this is a binary 0 or 1
    float accumEnvironmentQuality; // Using Borchert-Honda Model, indicates the accumulated amount of resources reaching this bud
    float resourceBH; // amount of available resource reaching this Bud using the BH Model
    int formedBranchIndex; // If this bud's fate is FORMED_BRANCH, this is the index in the Tree's list of branches of that formed branch. -1 o.w.
    float internodeLength;
    float branchRadius;
    int numNearbyAttrPts;
    BUD_TYPE type;
    BUD_FATE fate;

    // Constructor: to allow use with emplace_back() for std::vectors
    Bud(const glm::vec3& p, const glm::vec3& nd, const glm::vec3& d, float q, float aq, float re,
        int i, float l, float br, int n, BUD_TYPE t, BUD_FATE f) :
        point(p), naturalGrowthDir(nd), optimalGrowthDir(d), environmentQuality(q), accumEnvironmentQuality(aq), resourceBH(re),
        formedBranchIndex(i), internodeLength(l), branchRadius(br), numNearbyAttrPts(n), type(t), fate(f) {}
    Bud() : point(glm::vec3(0.0f)), naturalGrowthDir(glm::vec3(0.0f)), optimalGrowthDir(glm::vec3(0.0f)), environmentQuality(0.0f), accumEnvironmentQuality(0.0f), resourceBH(0.0f),
        formedBranchIndex(-1), internodeLength(0.0f), branchRadius(0.0f), numNearbyAttrPts(0), type(TERMINAL), fate(ABORT) {}
};

// Wraps up necessary information regarding a tree branch.
class TreeBranch {
    friend class Tree;
private:
    std::vector<Bud> buds; // List of buds. Last bud is always the terminal bud.
    glm::vec3 growthDirection; // World space direction in which this branch is oriented
    float radius; // Branch radius. Computed using pipe model
    unsigned int axisOrder; // Order n (0, 1, ..., n) of this axis. Original trunk of a tree is 0, each branch supported by this branch has order 1, etc
    int prevBranchIndex; // Index of the branch supporting this one in the 

public:
    TreeBranch(const glm::vec3& p, const glm::vec3& d, int ao, int bi) :
        growthDirection(d), radius(INITIAL_BRANCH_RADIUS), axisOrder(ao), prevBranchIndex(bi) {
        buds = std::vector<Bud>();
        buds.reserve(8); // Reserve memory beforehand so we are less likely to have to resize the array later on. Performance test this.
        buds.emplace_back(p, glm::vec3(growthDirection), glm::vec3(0.0f), 0.0f, 0.0f, 0.0f, -1, INITIAL_BUD_INTERNODE_RADIUS, 0.0f, 0, TERMINAL, DORMANT); // add the terminal bud for this branch. Applies a prelim internode length (tweak, TODO)
    }
    const std::vector<Bud>& GetBuds() const { return buds; }
    int GetAxisOrder() const { return axisOrder; }
    // Adds a certain number of axillary buds to the list of buds, starting at the index just before the terminal bud
    void AddAxillaryBuds(const Bud& sourceBud, const int numBuds, const float internodeLength);
};

// Wrap up branches into one Tree class. This class also organizes the simulation functions
class Tree {
private:
    std::vector<TreeBranch> branches; // all branches in the tree
    bool didUpdate; // flag indicating whether or not the tree changed form (aka gained a bud) during the most recent iteration of growth
    void InitializeTree(const glm::vec3& p) { branches.emplace_back(TreeBranch(p, glm::vec3(0.0f, 1.0f, 0.0f), 0, -1)); } // Initialize a tree to be a single branch
public:
    Tree() : Tree(glm::vec3(0.0f)) {}
    Tree(const glm::vec3& p) : didUpdate(false) {
        branches.reserve(65536); // Reserve a lot so we don't have to resize often. This vector will definitely expand a lot. Also, the code will crash without this due to some contiguous memory issue, probably. TODO fix this?
        InitializeTree(p);
    }
    const std::vector<TreeBranch>& GetBranches() const { return branches; }
    void IterateGrowth(std::vector<AttractorPoint>& attractorPoints, const TreeParameters& treeParams, bool useGPU = false);
    void PerformSpaceColonization(std::vector<AttractorPoint>& attractorPoints, bool useGPU);
    void PerformSpaceColonizationCPU(std::vector<AttractorPoint>& attractorPoints);
    void PerformSpaceColonizationGPU(std::vector<AttractorPoint>& attractorPoints);
    void RemoveAttractorPoints(std::vector<AttractorPoint>& attractorPoints);
    float ComputeQAccumRecursive(TreeBranch & branch);
    void ComputeBHModelBasipetalPass();
    void ComputeResourceFlowRecursive(TreeBranch & branch, float resource);
    void ComputeBHModelAcropetalPass();
    void AppendNewShoots(int n);
    float ComputeBranchRadiiRecursive(TreeBranch & branch, const TreeParameters& treeParams);
    void ComputeBranchRadii(const TreeParameters& treeParams);
    void ResetState(std::vector<AttractorPoint>& attractorPoints);
};
