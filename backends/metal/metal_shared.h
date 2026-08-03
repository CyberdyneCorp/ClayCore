#pragma once

// Uniform structs shared verbatim between the Metal host (metal_backend.cpp)
// and the MSL kernels (clay_kernels.metal). Plain scalars only — identical
// layout on both sides.

typedef struct {
    unsigned int instr_count;
    unsigned int point_count;
    unsigned int has_colors;
    unsigned int pad;
} ClayEvalUniforms;

typedef struct {
    float origin[3];
    float spacing;
    unsigned int instr_count;
    unsigned int nx, ny, nz;
    unsigned int has_colors;
    unsigned int pad0, pad1, pad2;
} ClayGridUniforms;

typedef struct {
    unsigned int instr_count;
    unsigned int ray_count;
    unsigned int max_steps;
    unsigned int has_bounds;
    float tmin, tmax, eps, step_scale;
    float bounds_min[3];
    float pad0;
    float bounds_max[3];
    float pad1;
} ClayRayUniforms;

// Must match clay::eval::RayHit exactly (int32 + 7 floats).
typedef struct {
    int hit;
    float t;
    float pos[3];
    float normal[3];
} ClayRayHitGpu;
