# 为 RISCV 增加 Itinerary/DFA 支持并提供有效 ResourceTracker 的实现指南

本文档说明如何为 RISCV 后端增加 **itinerary + DFA packetizer** 支持，使 `CreateTargetScheduleState` 返回有效的 `DFAPacketizer`（ResourceTracker），从而在 packetizer 中做基于功能单元的合法 packet 判定。

---

## 一、背景与约束

1. **DFA Packetizer 依赖 itinerary 模型**  
   LLVM 的 DFA packetizer 由 TableGen 的 `-gen-dfa-packetizer` 根据 **ProcessorItineraries**（旧 itinerary 模型）生成。  
   它需要：
   - **FuncUnit**：功能单元（如 ALU、Load、Store、Branch 等）
   - **InstrItinClass**：指令 itinerary 类
   - **ProcessorItineraries**：FU 列表 + InstrItinData（类 → Stages → Units）
   - **Processor**（旧式）：使用上述 ProcessorItineraries 的处理器定义

2. **RISCV 当前状态**  
   - RISCV 使用 **ProcessorModel + SchedModel**（SchedWrite/SchedRead/ProcResource 等），**没有**使用 ProcessorItineraries。
   - 因此 `CreateTargetScheduleState` 默认返回 `nullptr`，packetizer 只能做“无 DFA”的依赖驱动 packetization。
   - 若要 ResourceTracker 有效，必须**额外**为 packetizer 引入一套 itinerary 定义，并让 TableGen 生成 DFA 与 `createDFAPacketizer`。

3. **参考实现**  
   - **Hexagon**：`HexagonSchedule.td` + 各 `HexagonScheduleV*.td`，`HexagonGenDFAPacketizer.inc`，`HexagonInstrInfo::CreateTargetScheduleState`。  
   - **R600 (AMDGPU)**：`R600Schedule.td`（FuncUnit、ProcessorItineraries）、`R600Processors.td`（Processor），`R600GenDFAPacketizer.inc`，`R600InstrInfo::CreateTargetScheduleState`。

---

## 二、实现步骤概览

1. 在 TableGen 中为 RISCV 增加 **itinerary 定义**（FuncUnit、InstrItinClass、ProcessorItineraries、Processor）。  
2. 在 RISCV 的 TableGen 构建中增加 **-gen-dfa-packetizer**，生成 `RISCVGenDFAPacketizer.inc`。  
3. 在 **RISCVInstrInfo** 中实现 **CreateTargetScheduleState**，调用生成的 `createDFAPacketizer`。  
4. 保证 **指令的 SchedClass** 与 itinerary 类对应（见下文“指令与 itinerary 的对应”）。

以下按步骤展开。

---

## 三、Step 1：定义 Itinerary（.td）

### 3.1 新建或扩展现有 Schedule .td

在 `llvm/lib/Target/RISCV/` 下新建例如 **RISCVPacketizerItinerary.td**（或合并进现有 `RISCVSchedule.td`），并确保被 `RISCV.td` include。

### 3.2 定义功能单元（FuncUnit）

根据你对“一个 packet 里能同时发射哪些指令”的约束来设计。例如简单单发、双发：

```tablegen
// 示例：单发流水，所有指令占同一个单元
def RISCV_ALU : FuncUnit;

// 或：双发，两条 ALU 可并行
// def RISCV_ALU0 : FuncUnit;
// def RISCV_ALU1 : FuncUnit;
```

R600 示例：`R600Schedule.td` 中 `ALU_X, ALU_Y, ALU_Z, ALU_W, TRANS, ALU_NULL`。

### 3.3 定义 InstrItinClass 与 ProcessorItineraries

```tablegen
// InstrItinClass：与指令 SchedClass 对应（见 Step 4）
def RISCV_ItinALU  : InstrItinClass;
def RISCV_ItinLoad : InstrItinClass;
def RISCV_ItinStore: InstrItinClass;
def RISCV_ItinBranch : InstrItinClass;
// ... 其他类

def RISCV_NullUnit : FuncUnit;  // 无资源/伪指令用

def RISCV_Packet_Itin : ProcessorItineraries <
  [RISCV_ALU, RISCV_NullUnit],   // FU 列表
  [],                            // Bypass（可留空）
  [
    InstrItinData<RISCV_ItinALU,   [InstrStage<1, [RISCV_ALU]>]>,
    InstrItinData<RISCV_ItinLoad,  [InstrStage<1, [RISCV_ALU]>]>,
    InstrItinData<RISCV_ItinStore, [InstrStage<1, [RISCV_ALU]>]>,
    InstrItinData<RISCV_ItinBranch,[InstrStage<1, [RISCV_ALU]>]>,
    InstrItinData<NoItinerary,     [InstrStage<1, [RISCV_NullUnit]>]>
  ],
  PacketizerNamespace = "RISCV"   // 生成 createRISCVDFAPacketizer
>;
```

要点：

- **PacketizerNamespace** 非空时，会生成 `create<Namespace>DFAPacketizer`（如 `createRISCVDFAPacketizer`）；若为 `""`，则生成 `createDFAPacketizer`。
- **IID 列表顺序** 会变成 SchedClass 的索引，指令的 itinerary 类必须与这些 InstrItinData 对应。

### 3.4 定义 Processor（旧式）

让该处理器使用上面的 ProcessorItineraries，这样 `CodeGenProcModel` 才会 `hasItineraries()`，`-gen-dfa-packetizer` 才会为 RISCV 生成代码：

```tablegen
// 在 RISCVProcessors.td 或本文件中
def : Processor<"generic-packet", RISCV_Packet_Itin, []>;
```

若希望与现有 `generic` 共用，需要理清当前 RISCV 是只用 ProcessorModel 还是也混用 Processor；若只共用名字，可先单独用 `"generic-packet"` 做 DFA 测试。

### 3.5 在 RISCV.td 中 include

在 `RISCV.td` 中增加：

```tablegen
include "RISCVPacketizerItinerary.td"
```

（若合并进 `RISCVSchedule.td` 则无需单独 include 新文件。）

---

## 四、Step 2：加入 -gen-dfa-packetizer 并包含生成文件

### 4.1 CMakeLists.txt

在 `llvm/lib/Target/RISCV/CMakeLists.txt` 的 tablegen 段增加：

```cmake
tablegen(LLVM RISCVGenDFAPacketizer.inc -gen-dfa-packetizer)
```

### 4.2 生成内容说明

- 会生成 `RISCVGenDFAPacketizer.inc`，其中包含：
  - DFA 状态与转移表（根据 FU / itinerary 推导）
  - `RISCVGenSubtargetInfo::createRISCVDFAPacketizer(const InstrItineraryData *IID) const`（若 PacketizerNamespace = "RISCV"）
- **仅当**存在带 itinerary 的 Processor（即 `hasItineraries()` 为 true）时，才会为该 target 生成上述实现。

### 4.3 Subtarget 头文件

`RISCVGenSubtargetInfo.inc` 中已有 `createDFAPacketizer` 的**声明**（由 SubtargetEmitter 统一生成）。  
实现则在 **RISCVGenDFAPacketizer.inc** 里；因此需要在某处 **include 该 .inc**，通常在与 Subtarget/InstrInfo 相关的 .cpp 里（见 Step 3）。

---

## 五、Step 3：RISCVInstrInfo 中实现 CreateTargetScheduleState

### 5.1 声明（RISCVInstrInfo.h）

在 `RISCVInstrInfo` 中重写：

```cpp
DFAPacketizer *CreateTargetScheduleState(const TargetSubtargetInfo &STI) const override;
```

### 5.2 实现（RISCVInstrInfo.cpp）

- 包含生成的 .inc（在包含其它 RISCV Gen*.inc 的合适位置）：

```cpp
#include "RISCVGenDFAPacketizer.inc"
```

- 实现：

```cpp
DFAPacketizer *RISCVInstrInfo::CreateTargetScheduleState(
    const TargetSubtargetInfo &STI) const {
  const InstrItineraryData *II = STI.getInstrItineraryData();
  if (!II)
    return nullptr;
  return static_cast<const RISCVSubtarget &>(STI).createRISCVDFAPacketizer(II);
}
```

若 PacketizerNamespace 为 `""`，则方法名为 `createDFAPacketizer`。

- **getInstrItineraryData()** 只有在当前选中的 Processor 是“带 itinerary 的 Processor”时非 null。因此要么：
  - 使用 `"generic-packet"` 等 CPU，并保证该 CPU 对应上面定义的 Processor；要么  
  - 在 Subtarget 里根据 CPU 选择性地返回某份 InstrItineraryData（需要和现有 ProcessorModel 的兼容方式，此处不展开）。

---

## 六、Step 4：指令与 Itinerary 的对应（关键）

DFAPacketizer 通过 **SchedClass** 查 ItinActions：

- `canReserveResources(MI)` 使用 `MID->getSchedClass()` 得到 SchedClass 索引。
- 该索引必须对应到你在 ProcessorItineraries 的 **IID** 里列出的 InstrItinData 顺序。

因此有两种做法：

### 6.1 做法 A：在指令定义中指定 Itinerary（旧模型）

在 RISCV 的 InstrInfo .td 里，给每条指令（或通过 multiclass/class）指定 **Itinerary**，且该 Itinerary 的类要在 `RISCV_Packet_Itin` 的 IID 里出现，例如：

```tablegen
def : Inst<..., Itinerary<RISCV_ItinALU>, ...>;
```

这样 TableGen 会为该指令分配对应的 SchedClass，与 IID 顺序一致。  
注意：RISCV 目前大量使用 **Sched<...>**（新模型），与 Itinerary 是两套体系；若只为了 packetizer 引入 itinerary，需要**只**在“用于 packetizer 的 Processor”下让 SchedClass 与 IID 对齐，避免破坏现有调度模型。

### 6.2 做法 B：保持 SchedModel，在 Subtarget 中做 SchedClass → Itinerary 映射

若不想改大量指令的 Itinerary，可以：

- 保持现有 SchedWrite/SchedRead 定义；
- 在 Subtarget 或 InstrInfo 中实现 **resolveSchedClass**（或等价逻辑），使“使用某 SchedClass 的指令”在查询 itinerary 时映射到你为 packetizer 定义的那套 IID 索引。

这样 DFA 看到的仍是“类索引 → 资源”，但索引来源于你自定义的映射而非直接来自 TableGen 的 Itinerary 字段。  
具体需要对照 `DFAPacketizer::canReserveResources` 与生成代码里 `ItinActions` / `ResourceIndices` 的用法，保证传入的 SchedClass 落在正确范围内并与 IID 一致。

---

## 七、可选：Transcription（getUsedResources）

若希望 `getUsedResources(InstIdx)` 可用，Automaton 在构造时需提供 **TranscriptionTable**。  
当前 `-gen-dfa-packetizer` 生成的 Automaton 是否带 Transcription，取决于 TableGen 后端的实现；若默认未带，则 `setTrackResources(true)` 可能触发 assert（你之前遇到的崩溃）。  
我们已在基类中做了 `ResourceTracker == nullptr` 时的防护；若你实现了非 null 的 ResourceTracker，需确认生成代码里 Automaton 的构造是否支持 enableTranscription；若不支持，可暂时不调用 `setTrackResources(true)`，或在本地补丁中为 RISCV 的 DFA 打开 Transcription 生成（需改 TableGen）。

---

## 八、验证

1. **编译**  
   - `ninja -C build-debug LLVMRISCVCodeGen llc`
2. **确认 ResourceTracker 非 null**  
   - 在 `RISCVPacketizer::runOnMachineFunction` 里临时 `assert(Packetizer.getResourceTracker())` 或打日志，用 `-mcpu=generic-packet`（或你定义的 CPU）跑 `llc -run-pass=riscv-packetizer`。
3. **行为**  
   - 若 DFA 配置正确，`canReserveResources` / `reserveResources` 会按 FU 限制拒绝/接受指令入包；可配合 `-debug-only=packets` 观察。

---

## 九、小结

| 项目 | 说明 |
|------|------|
| 新增 .td | FuncUnit、InstrItinClass、ProcessorItineraries、Processor，并设 PacketizerNamespace |
| CMake | 增加 `tablegen(LLVM RISCVGenDFAPacketizer.inc -gen-dfa-packetizer)` |
| RISCVInstrInfo | 实现 `CreateTargetScheduleState`，调用 `createRISCVDFAPacketizer(II)`，并 include `RISCVGenDFAPacketizer.inc` |
| 指令 ↔ DFA | 通过 Itinerary 或 resolveSchedClass 使 SchedClass 与 IID 顺序一致 |
| Subtarget | 使用“带 itinerary 的 Processor”时 `getInstrItineraryData()` 才非 null |

按上述步骤即可为 RISCV 增加 itinerary/DFA 支持并提供有效的 ResourceTracker；最易出错的是 **SchedClass 与 IID 的对应** 以及 **当前 CPU 是否选到了带 itinerary 的 Processor**，需要结合 `llc -mcpu=...` 与调试输出逐项确认。
