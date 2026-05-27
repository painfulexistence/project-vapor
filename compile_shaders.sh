#!/bin/bash

# SPIR-V Shader Compilation Script for Project Vapor
# This script compiles all GLSL shaders to SPIR-V for Vulkan backend

set -e  # Exit on error

SHADER_DIR="Vapor/assets/shaders"
OUTPUT_DIR="$SHADER_DIR"

echo "=== Compiling GLSL Shaders to SPIR-V ==="
echo "Shader directory: $SHADER_DIR"
echo "Output directory: $OUTPUT_DIR"
echo ""

# Check if glslangValidator is available
if ! command -v glslangValidator &> /dev/null; then
    echo "ERROR: glslangValidator not found!"
    echo ""
    echo "Please install Vulkan SDK:"
    echo "  - macOS: brew install vulkan-sdk"
    echo "  - Linux: sudo apt install glslang-tools"
    echo "  - Or download from: https://vulkan.lunarg.com/sdk/home"
    echo ""
    exit 1
fi

echo "Using glslangValidator: $(which glslangValidator)"
echo ""

# Function to compile a shader
compile_shader() {
    local input=$1
    local output=$2

    echo "Compiling: $input -> $output"
    glslangValidator -V "$input" -o "$output"

    if [ $? -eq 0 ]; then
        echo "  ✓ Success"
    else
        echo "  ✗ Failed"
        return 1
    fi
}

# Core rendering shaders
echo "--- Core Rendering Shaders ---"
compile_shader "$SHADER_DIR/TBN.vert" "$OUTPUT_DIR/TBN.vert.spv"
compile_shader "$SHADER_DIR/PBRNormalMapped.frag" "$OUTPUT_DIR/PBRNormalMapped.frag.spv"
compile_shader "$SHADER_DIR/PrePass.vert" "$OUTPUT_DIR/PrePass.vert.spv"
compile_shader "$SHADER_DIR/PrePass.frag" "$OUTPUT_DIR/PrePass.frag.spv"
compile_shader "$SHADER_DIR/FullScreen.vert" "$OUTPUT_DIR/FullScreen.vert.spv"
compile_shader "$SHADER_DIR/PostProcess.frag" "$OUTPUT_DIR/PostProcess.frag.spv"
echo ""

# Compute shaders
echo "--- Compute Shaders ---"
compile_shader "$SHADER_DIR/TileLightCull.comp" "$OUTPUT_DIR/TileLightCull.comp.spv"
echo ""

# Particle shaders
echo "--- Particle Shaders ---"
compile_shader "$SHADER_DIR/ParticleForce.comp" "$OUTPUT_DIR/ParticleForce.comp.spv"
compile_shader "$SHADER_DIR/ParticleIntegrate.comp" "$OUTPUT_DIR/ParticleIntegrate.comp.spv"
compile_shader "$SHADER_DIR/Particle.vert" "$OUTPUT_DIR/Particle.vert.spv"
compile_shader "$SHADER_DIR/Particle.frag" "$OUTPUT_DIR/Particle.frag.spv"
echo ""

# UI shaders
echo "--- UI Shaders ---"
compile_shader "$SHADER_DIR/RmlUi.vert" "$OUTPUT_DIR/RmlUi.vert.spv"
compile_shader "$SHADER_DIR/RmlUi.frag" "$OUTPUT_DIR/RmlUi.frag.spv"
echo ""

# 2D Batch rendering shaders
echo "--- 2D Batch Shaders ---"
compile_shader "$SHADER_DIR/Batch2D.vert" "$OUTPUT_DIR/Batch2D.vert.spv"
compile_shader "$SHADER_DIR/Batch2D.frag" "$OUTPUT_DIR/Batch2D.frag.spv"
echo ""

echo "=== Compilation Complete ==="
echo ""
echo "Summary:"
echo "  Core rendering: 6 shaders"
echo "  Compute: 1 shader"
echo "  Particles: 4 shaders"
echo "  UI: 2 shaders"
echo "  2D Batch: 2 shaders"
echo "  Total: 15 shaders compiled"
echo ""
echo "✓ All shaders compiled successfully!"
echo ""
echo "Next steps:"
echo "  1. Build the project: cmake --build build"
echo "  2. Run with Vulkan: ./build/Vapor --vulkan"
