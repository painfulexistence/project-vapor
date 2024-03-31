FROM mcr.microsoft.com/devcontainers/cpp:0-ubuntu-22.04

# Install CMake
RUN if ! [command -v cmake &> /dev/null]; then \
        apt-get install cmake; \
    fi \
    && echo "CMake version: $(which cmake)"

# Set Vcpkg variables to workaround https://github.com/msclock/features/issues/15
ENV VCPKG_FORCE_SYSTEM_BINARIES=1

# Install dependencies
RUN apt-get update && apt-get install -y libxmu-dev libxi-dev libgl-dev libxinerama-dev libxcursor-dev xorg-dev libglu1-mesa-dev pkg-config
