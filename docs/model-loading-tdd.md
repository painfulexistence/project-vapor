# Model Asset Loading — Technical Design Document

## 1. Background

### 1.1 DCC 工具的格式哲學

DCC 工具（Blender、Maya、Houdini）的核心設計原則是**非破壞性編輯**：每一個操作都是可撤銷的、可重新參數化的。因此它們的原生格式儲存的是「意圖」，而非「結果」：

- Subdivision surface（細分曲面）而非三角面
- Modifier stack、Construction history
- NURBS 曲線、volumes、particles
- 任意 metadata 與自訂屬性
- 多個 take / variant / layer

這類格式（.blend、.ma、.hip）根本不適合 runtime 使用，因為要得到最終幾何就必須執行完整的 modifier pipeline，而且格式是工具私有的。

因此業界發展出兩類**交換格式**，各自解決不同層次的問題：

---

### 1.2 GLTF — 為 Runtime 設計的交換格式

GLTF 2.0（GL Transmission Format）的目標是「3D 的 JPEG」——一個低解析代價、接近 GPU-ready 的傳輸格式。

**格式結構**：

```
.gltf (JSON) / .glb (binary container)
├── scenes[]          → 場景列表（含 defaultScene）
├── nodes[]           → 變換節點（TRS 或 matrix，含 children[]）
├── meshes[]          → 幾何組（每個含多個 primitives[]）
│   └── primitives[]  → 一個 draw call 的資料
│       ├── attributes → { "POSITION": 0, "NORMAL": 1, ... }（accessor 索引）
│       ├── indices    → accessor 索引
│       └── material   → material 索引
├── accessors[]       → 型別化資料描述（componentType、count、bufferView）
├── bufferViews[]     → buffer 的切片（byteOffset、byteLength、byteStride）
├── buffers[]         → 原始二進位 blob（base64 或外部 .bin）
├── materials[]       → PBR metallic-roughness 材質
├── textures[]        → 圖片 + sampler 的組合
└── images[]          → 圖片來源（URI 或 bufferView）
```

**關鍵設計決策**：

| 特性 | 說明 |
|------|------|
| Accessor 抽象 | 資料語義（POSITION）與儲存格式（FLOAT、UNSIGNED_SHORT…）分離 |
| byteStride | BufferView 可以是 interleaved（多個 attribute 共用一個 bufferView），stride ≠ 0 時必須遵守 |
| TANGENT w | vec4，`w = ±1` 表示 bitangent 的手性（`bitangent = cross(N, T.xyz) * T.w`） |
| TEXCOORD 型別 | 可以是 FLOAT、UNSIGNED_BYTE normalized、UNSIGNED_SHORT normalized |
| 場景實例化 | 多個 node 可以 ref 同一個 mesh index，共享頂點資料 |
| 兩種變體 | .gltf（JSON + 外部 .bin）適合開發；.glb（單一二進位容器）適合發布 |
| Extensions | KHR_draco_mesh_compression、KHR_texture_basisu、KHR_lights_punctual 等 |

**GLTF 不是編輯格式**：所有幾何已三角化、resolved，沒有 modifier stack，沒有程序式描述。

---

### 1.3 USD — 為大型製作流水線設計的場景描述語言

USD（Universal Scene Description，Pixar）解決的問題比 GLTF 大得多：多人協作、大型場景、非破壞性的場景合成。

**核心概念**：

```
Stage（舞台）
└── Layer stack（圖層堆疊）
    ├── session layer
    ├── root layer (.usda / .usdc)
    │   └── Prim（場景物件，有 schema）
    │       ├── UsdGeomXform    → 變換節點
    │       ├── UsdGeomMesh     → 幾何資料
    │       ├── UsdShadeMaterial → 材質綁定
    │       └── UsdGeomPointInstancer → GPU instancing
    └── sublayers（可疊加的圖層）
```

**Composition Arcs**（強度由高到低）：

| Arc | 用途 |
|-----|------|
| Local opinions | 在當前 layer 直接寫的值 |
| References | 引用另一個 USD 檔案的 prim |
| Payloads | 像 Reference 但可以延遲載入（streaming 的基礎） |
| Inherits | 類繼承語義，用於 class prim |
| Specializes | 比 inherit 更強的特化 |
| Variant Sets | 在同一個 prim 上定義多個變體（LOD0/LOD1、材質 A/B） |

**UsdGeomMesh 的幾何描述**（與 GLTF 的主要差異）：

```
points[]              → 頂點位置（不一定 indexed）
faceVertexCounts[]    → 每個面有幾個頂點（可以是 quad、ngon）
faceVertexIndices[]   → 面的頂點索引（polygon soup，非三角化）
primvars:st[]         → UV（interpolation: faceVarying / vertex）
primvars:normals[]    → 法線
```

USD 的幾何是 **polygon soup**，引擎使用前必須三角化。Primvar 的 interpolation 模式（`vertex`、`faceVarying`、`uniform`、`constant`）決定如何展開成 per-vertex flat array。

**Payload 與 streaming**：Payload arc 是 USD 天然的串流邊界。未載入的 payload prim 佔很少記憶體；只有在需要時才「activate」並組合其內容。這是大型開放世界場景分塊的基礎。

**tinyusd 方案**：tinyusd 是輕量級的 USD 實作，適合嵌入引擎。它需要處理：stage 組合（解析所有 composition arcs）、三角化（polygon soup → triangles）、primvar 展開（faceVarying → flat vertex array）、material binding 解析。

---

### 1.4 引擎希望拿到的格式

引擎的 runtime 不需要「場景描述語言」，它需要的是可以直接送進 GPU 的資料：

```
Mesh（runtime）
├── vertex buffer    → interleaved, GPU-ready layout（position, normal, tangent, uv, ...）
├── index buffer     → uint16 or uint32
├── vertexOffset     → 在全局 vertex buffer 中的起點
├── indexOffset      → 在全局 index buffer 中的起點
├── vertexCount
├── indexCount
├── AABB             → culling 用
└── material handle  → 指向已上傳 GPU texture 的 material

Scene（runtime）
├── vertices[]       → flat global vertex buffer
├── indices[]        → flat global index buffer
├── stagedMeshes[]   → mesh handle（含 offset）列表
└── stagedMeshTransforms[] → per-draw world transform
```

**引擎格式的設計原則**：

1. 頂點資料 interleaved、對齊，可以直接 `vkCmdCopyBuffer` 到 GPU
2. 一個 scene = 一個 VBO + 一個 IBO，減少 bind 次數
3. Mesh 是 handle（offset + count），不 inline 資料，允許多 node 共享
4. Material = GPU texture handle，不持有 CPU 圖片
5. 不需要 modifier stack、不需要 variant 語義（已在 import 時 resolve）

---

## 2. 問題點

### 2.1 byteStride 未處理（GLTF 特有）

GLTF 允許 interleaved buffer：POSITION、NORMAL、TEXCOORD_0 可以共用同一個 bufferView，以 byteStride 分隔每個 vertex。

```
bufferView.byteStride = 32  （12 pos + 12 normal + 8 uv）
```

舊版讀法：

```cpp
// 假設 tightly-packed，stride = sizeof(float)*3 = 12
const float* data = reinterpret_cast<const float*>(&buffer.data[byteOffset]);
position = glm::vec3(data[i*3], data[i*3+1], data[i*3+2]);
```

當 stride = 32 時，`data[1*3]` 讀到的是第 12 byte，但 vertex 1 的 position 在第 32 byte。結果每個頂點都讀到錯誤的位置，Beautiful Game 等使用 interleaved buffer 的模型完全無法正確載入。

**正確作法**：

```cpp
const size_t stride = bufferView.byteStride ? bufferView.byteStride : sizeof(float) * 3;
const uint8_t* base = buffer.data.data() + bufferView.byteOffset + accessor.byteOffset;
position = *reinterpret_cast<const glm::vec3*>(base + i * stride);
```

### 2.2 TEXCOORD component type 未分派

GLTF spec 允許 TEXCOORD 使用三種型別：

| componentType | 解讀 |
|---------------|------|
| FLOAT | 直接使用 |
| UNSIGNED_BYTE | 除以 255.0，normalized |
| UNSIGNED_SHORT | 除以 65535.0，normalized |

舊版一律當 float 讀，碰到 normalized integer UV 會得到錯誤的貼圖座標。

### 2.3 圖片雙重複製

```cpp
// tinygltf 已經 decode → img.image（std::vector<unsigned char>）
// 又 copy 一份進 Image::byteArray
.byteArray = std::vector<Uint8>(img.image.begin(), img.image.end())
```

同一份 pixel data 在記憶體中存了兩份。tinygltf 的 `img.image` 在 `model` 生命週期內不會釋放。

### 2.4 loadGLTFOptimized 做了兩遍工

舊版流程：

```
loadGLTF()
  └── 解析 GLTF → 建 node tree（每個 Mesh 有自己的 vertices/indices vector）
        ↓ 所有頂點都 inline 在 node tree 裡
loadGLTFOptimized()
  └── 呼叫 loadGLTF() → 遍歷 node tree → 把所有 vertices/indices concat 進 flat buffer
```

問題：
- 第一遍建 node tree 時每個 Mesh 都分配了自己的 vector
- 第二遍又把它們全部 copy 進 flat buffer
- 峰值記憶體 = node tree 所有頂點 + flat buffer 所有頂點 = **兩倍**

### 2.5 相同 GLTF mesh 的資料被複製多次

GLTF 允許多個 node ref 同一個 mesh index（instancing 語義）。舊版遍歷每個 node 都把頂點 append 一次，同樣的幾何資料在 flat buffer 裡出現多份。

### 2.6 USD 三角化與 primvar 展開（待解）

`UsdGeomMesh` 的 `faceVertexCounts` 可以包含 quad 或 ngon。primvar 的 interpolation 可以是 `faceVarying`（每個面的每個頂點有獨立值，常用於 UV），這與 GPU 需要的 per-vertex flat array 不同。

展開規則：

```
faceVarying → 每個三角面的每個頂點獨立一份 UV
             → 三角化後 vertex 數 = faceVertexIndices 的總數
             → 需要 index buffer 重建
vertex      → 每個 point 一份，展開方式與 GLTF accessor 相同
```

---

## 3. 解決方案

### 3.1 Stride-aware accessor 讀取

對所有 attribute accessor 統一使用 byte pointer 模式：

```cpp
const auto readVec3 = [&](const tinygltf::Accessor& acc, size_t i) -> glm::vec3 {
    const auto& bv = model.bufferViews[acc.bufferView];
    const size_t stride = bv.byteStride ? bv.byteStride : sizeof(float) * 3;
    const float* f = reinterpret_cast<const float*>(
        model.buffers[bv.buffer].data.data() + bv.byteOffset + acc.byteOffset + i * stride);
    return {f[0], f[1], f[2]};
};
```

TEXCOORD 加 componentType dispatch：

```cpp
switch (acc.componentType) {
case TINYGLTF_COMPONENT_TYPE_FLOAT:          /* 直接讀 */ break;
case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:  /* / 255.0f */ break;
case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT: /* / 65535.0f */ break;
}
```

### 3.2 Move image buffer，不 copy

```cpp
for (auto& img : model.images) {   // 非 const ref
    images.push_back(std::make_shared<Image>(Image{
        .byteArray = std::move(img.image),   // move，不 copy
        ...
    }));
}
```

`std::move` 後 `img.image` 變空，ownership 轉移到 `Image::byteArray`，記憶體只有一份。

### 3.3 Single-pass flat buffer + mesh cache

```
loadGLTFOptimized（新版）
  ├── 解析 GLTF（一次）
  ├── 建 meshCache: gltf_mesh_index → vector<shared_ptr<Mesh>>
  └── processNode（遞迴遍歷）
      ├── 若 node.mesh 已在 cache → 直接取 handle，不重寫頂點
      ├── 若未 cache → 讀 accessor → append 到 scene->vertices/indices → 存入 cache
      └── 把 (mesh_handle, worldTransform) push 進 stagedMeshes / stagedMeshTransforms
```

效果：
- 每個 unique GLTF mesh 的頂點資料只寫一次
- 多個 node ref 同一 mesh → 共享 vertexOffset/indexOffset，只新增一個 transform entry
- 無中間 node tree，記憶體峰值 = flat buffer 本身

---

## 4. 進一步改進

### 4.1 Disk Cache 版本化

目前 `.vscene` / `.vscene_optimized` 沒有版本資訊。當 source 檔案更新或序列化格式改版時，stale cache 會被讀入導致錯誤或 crash。

**方案 A：Source mtime 比對**

```cpp
struct CacheHeader {
    uint32_t magic;          // 'VSCO'
    uint32_t formatVersion;  // 遞增整數，格式改版時 bump
    uint64_t sourceMtime;    // source GLTF/USD 的 last-write-time
    uint64_t sourceSize;     // 額外確認
};
```

載入時：若 `formatVersion` 不符或 source mtime 變了，刪除 cache 重新 import。

**方案 B：Content hash**

對 source 檔案算 xxHash64 或 CRC32，存進 cache header。比 mtime 更可靠（mtime 在某些 VCS 操作後會被重置）。

**方案 C：Cache manifest**

獨立的 `.cache_manifest.json`，記錄每個 source → cache 的對應關係、hash、版本，集中管理。

### 4.2 Draco 幾何壓縮（KHR_draco_mesh_compression）

Draco 是 Google 開發的網格壓縮演算法，對頂點資料使用 delta coding + entropy coding，可壓縮 10×。GLTF 透過 extension 支援：

```json
{
  "primitives": [{
    "extensions": {
      "KHR_draco_mesh_compression": {
        "bufferView": 0,
        "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 }
      }
    }
  }]
}
```

**整合步驟**：

1. 加入 `draco` 依賴（google/draco，vcpkg 有）
2. 在 accessor 讀取前，先檢查 `primitive.extensions` 是否有 `KHR_draco_mesh_compression`
3. 若有：從 bufferView 取出 Draco bitstream → `draco::Decoder::DecodeBufferToGeometry` → 得到 decoded point cloud
4. 從 decoded geometry 讀 attribute arrays（與一般 accessor 流程相同）

```cpp
if (prim.extensions.count("KHR_draco_mesh_compression")) {
    const auto& dracoExt = prim.extensions.at("KHR_draco_mesh_compression");
    const auto& bv = model.bufferViews[dracoExt.Get("bufferView").GetNumberAsInt()];
    draco::DecoderBuffer buf;
    buf.Init(reinterpret_cast<const char*>(
        model.buffers[bv.buffer].data.data() + bv.byteOffset), bv.byteLength);
    draco::Decoder decoder;
    auto result = decoder.DecodeMeshFromBuffer(&buf);
    // 從 result->mesh() 讀 POSITION、NORMAL、TEXCOORD_0 attribute
}
```

**注意**：Draco 解壓縮是 CPU 操作，建議在 import（離線）時解壓縮並存進 disk cache，runtime 直接讀解壓縮後的格式，不在每次載入時解壓。

### 4.3 紋理壓縮

原始 PNG/JPG 圖片不適合直接上傳到 GPU 作為紋理——GPU 不能隨機存取壓縮的 JPEG，它需要支援隨機 texel 存取的格式（block compression）。

**GPU Block Compression 格式**：

| 格式 | 平台 | Block size | 用途 |
|------|------|-----------|------|
| BC1 / DXT1 | PC (DX11+) | 4x4 → 8B | 無 alpha 的 diffuse |
| BC3 / DXT5 | PC | 4x4 → 16B | 有 alpha |
| BC5 | PC | 4x4 → 16B | Normal map（RG 雙通道） |
| BC7 | PC (DX11.1+) | 4x4 → 16B | 高品質 RGBA |
| ASTC | Mobile / Apple | 4x4 ~ 12x12 | 可變壓縮比 |
| ETC2 | Android | 4x4 → 8B | 廣泛支援 |

**Basis Universal / KHR_texture_basisu**

Basis Universal 是一種「超壓縮」方案：source → .basis / .ktx2，在載入時轉碼（transcode）到目標平台的 native format（BC7 on PC、ASTC on iOS、ETC2 on Android）。GLTF 透過 `KHR_texture_basisu` extension 支援。

```
[離線] PNG → toktx → .ktx2（含 UASTC supercompressed data）
[Runtime] .ktx2 → libktx transcode → BC7 / ASTC buffer → GPU upload
```

整合時只需要 `libktx`（KTX-Software，vcpkg 有）。載入 texture 時：

```cpp
ktxTexture2* texture;
ktxTexture2_CreateFromMemory(data, size, KTX_TEXTURE_CREATE_NO_FLAGS, &texture);
if (ktxTexture2_NeedsTranscoding(texture)) {
    ktxTexture2_TranscodeBasis(texture, KTX_TTF_BC7_RGBA, 0);  // 依平台選
}
// 用 texture->pData 上傳到 GPU
```

---

## 5. 遊戲打包時模型的存放方式

### 5.1 開發期（Import Pipeline）

```
DCC 輸出
  │
  ▼
GLTF / USD（交換格式）
  │
  ├── 引擎 Editor Import
  │     ├── 解析格式
  │     ├── 三角化（USD）
  │     ├── 壓縮紋理（offline BCn/ASTC encoding）
  │     ├── 計算 tangent / AABB / LOD
  │     └── 序列化成 runtime binary（.mesh、.tex、.mat）
  │
  └── 存入 Asset Database（editor 端的 cache）
```

每個 source 資源對應一個或多個 runtime asset 檔案，引擎 editor 管理它們的對應關係（通常用 GUID 或 content hash）。

### 5.2 打包期（Cook & Pack）

打包是把 asset database 中的 runtime binary 按照目標平台重新排列、壓縮：

```
Asset Database
  │
  ├── 選取此平台需要的 assets
  ├── 轉碼紋理到目標格式（PC→BC7，Switch→ASTC，PS5→BCn+Kraken）
  ├── 計算載入順序（按 level / streaming chunk 分組）
  └── 打包進 PAK 檔案
```

**PAK 檔案結構**（概念）：

```
[Header]
  magic
  version
  entry_count

[Entry Table]（entry_count 個）
  asset_id : uint64   (路徑的 hash 或 GUID)
  offset   : uint64   (在 PAK 內的 byte 起點)
  size     : uint64   (未壓縮)
  flags    : uint32   (壓縮方式、資源類型)

[Data Blob]
  ... asset 資料依序排列 ...
```

實際引擎的做法各有不同：
- **Unreal Engine**：`.pak`，內含 index + per-asset zlib/Oodle 壓縮
- **Unity**：AssetBundle / Addressables，`.bundle` 檔案
- **id Tech / DOOM**：`.resources`，Oodle Kraken 壓縮
- **Godot**：`.pck`

### 5.3 Runtime 載入

```
請求 asset "models/car.mesh"
  │
  ├── Asset Manager 查 PAK index → 找到 offset + size
  ├── 非同步 IO 讀取 blob（async read / memory-mapped file）
  ├── 若壓縮 → 解壓縮（Oodle、zstd、LZ4）
  ├── 反序列化（通常是 zero-copy：blob 本身就是 runtime layout）
  └── GPU Upload（VkBuffer / vertex buffer）
```

**Zero-copy 的理想情況**：runtime binary 的 layout 和 GPU upload 的格式完全相同，讀入記憶體後可以直接 `vkCmdCopyBuffer` 到 device local，不需要 CPU 端的任何 transform。這要求 import 時就把頂點排列成 interleaved、對齊到 GPU 需要的格式。

### 5.4 Instancing 的存放

如果場景中有大量同一個 mesh 的實例（樹木、石頭、道具），打包時：

- Mesh 資料（VBO + IBO）只存一份
- Instance 的 transform 存在另一個 buffer（通常是 `mat4x3`，節省空間）
- 渲染時用 `vkCmdDrawIndexedIndirect` 或 GPU-driven rendering，一個 draw call 畫所有 instance

### 5.5 USD Payload 作為 Streaming 邊界

USD 的 payload arc 天然對應 runtime streaming：

```
world.usda
  └── /World/City (Xform)
      ├── /World/City/DistrictA (payload: districtA.usdc)  ← 預設不載入
      ├── /World/City/DistrictB (payload: districtB.usdc)  ← 同上
      └── /World/City/DistrictC (payload: districtC.usdc)
```

引擎可以把每個 payload 對應到一個 PAK chunk，玩家靠近時再 activate payload 並 stream in 對應的 PAK chunk。這和 Unreal 的 World Partition 概念等價，只是用 USD 的語法表達。

---

## 6. 格式選擇建議

| 用途 | 建議格式 |
|------|---------|
| DCC → 引擎交換（開發期） | GLTF 2.0（單 mesh）；USD（大型場景、多人協作） |
| Runtime（遊戲執行中） | 引擎私有 binary（flat VBO + IBO + 壓縮紋理） |
| 發布打包 | PAK 檔案，內含 platform-specific cooked assets |
| 紋理 | BC7（PC）、ASTC（Mobile/Apple）、KTX2（跨平台 transcode） |
| 幾何壓縮（傳輸用） | Draco（GLTF extension），import 時解壓，不在 runtime 解 |
