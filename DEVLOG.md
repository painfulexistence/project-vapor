# Devlog

- 2024/04/01 - Created the repo 🎉!
- 2026/07/16 - ReSTIR denoising for the stochastic RT shadows: per-pixel light-sample reservoirs with temporal + spatial reuse (RHI renderer, Metal RT)
- 2026/07/22 - Adaptive GPU tessellation on CBT/LEB (Dupuy 2020): compute-driven subdivision (classify -> reduce -> indirect args), crack-free by construction, rendered via mesh/task shaders where supported with an instanced indirect fallback; CPU-verified core (cbt.hpp + test_cbt)
