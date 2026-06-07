# Tech Stack and Considerations

## Window
Using [GLFW3](https://github.com/glfw/glfw) or [SDL3](https://github.com/libsdl-org/SDL)

## Graphics Backends
Using Metal, Vulkan, DX12 (not a priority) to support realtime raytracing and potentially mesh shading, allowing me to experiment with modern rendering techniques. Web platform is not gonna be supported.

## ImGui
Using [Nuklear](https://github.com/Immediate-Mode-UI/Nuklear)

## GUI Framework
Using [RmlUi](https://github.com/mikke89/RmlUi)

## Audio Engine
Using [OpenAL Soft](https://github.com/kcat/openal-soft)?

## Physics Engine
Using [Jolt](https://github.com/jrouwe/JoltPhysics) for better multi-threading support

## Scene Format
Using [OpenUSD](https://github.com/PixarAnimationStudios/OpenUSD) for DCC interoperability

## Data Serialization
Using [FlatBuffers](https://github.com/google/flatbuffers) (rather than [Protobuf](https://github.com/protocolbuffers/protobuf) because network functionalities are not needed)

## Scripting
Using [Terra](https://github.com/terralang/terra) (or [Ravi](https://github.com/dibyendumajumdar/ravi)) for even faster speed than Luau; anyways, there are no JIT restrictions on native platforms

## Logging
Using [spdlog](https://github.com/gabime/spdlog)

## Profiling
Maybe forking [Optick](https://github.com/bombomby/optick) or using [Tracy](https://github.com/wolfpld/tracy) or [microprofile](https://github.com/jonasmr/microprofile)

## Editor Framework
Using [Avalonia UI](https://github.com/AvaloniaUI/Avalonia) or Desktop-Web technologies