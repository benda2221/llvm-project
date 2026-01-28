# HexagonVLIWPacketizer.cpp 代码分析文档

## 一、代码基本框架

### 1.1 概述

`HexagonVLIWPacketizer.cpp` 是 LLVM 编译器中用于 Hexagon 架构的 VLIW（Very Long Instruction Word）指令打包器（Packetizer）实现。该文件实现了一个基于 DFA（确定性有限自动机）的简单 VLIW 打包器，用于将机器指令打包成 VLIW 指令包（packet），以提高指令级并行性。

### 1.2 核心类结构

#### 1.2.1 HexagonPacketizer 类
- **继承关系**: 继承自 `MachineFunctionPass`
- **作用**: 作为 LLVM Pass，负责对整个机器函数进行打包处理
- **主要方法**:
  - `runOnMachineFunction()`: 主要的执行入口，遍历所有基本块进行打包
  - `getAnalysisUsage()`: 声明所需的分析依赖（别名分析、分支概率、支配树、循环信息等）

#### 1.2.2 HexagonPacketizerList 类
- **继承关系**: 继承自 `VLIWPacketizerList`（定义在 `DFAPacketizer.h` 中）
- **作用**: 实现 Hexagon 特定的打包逻辑
- **核心功能**:
  - 指令依赖关系检查
  - 指令优化（.new、.cur 指令转换）
  - 资源分配和冲突检测
  - 打包约束验证

### 1.3 主要工作流程

```
1. 初始化阶段
   ├── 获取 HexagonInstrInfo、HexagonRegisterInfo
   ├── 创建 HexagonPacketizerList 实例
   └── 初始化 DFA 状态表

2. 预处理阶段
   ├── 移除 KILL 伪指令（避免依赖分析混淆）
   └── TinyCore 模式下的指令转换

3. 打包阶段（对每个基本块）
   ├── 识别调度区域（scheduling regions）
   ├── 对每个区域调用 PacketizeMIs()
   │   ├── 检查资源可用性（DFA）
   │   ├── 检查指令依赖关系
   │   ├── 尝试指令优化（.new、.cur 转换）
   │   └── 将指令添加到当前包
   └── 结束当前包并创建 BUNDLE 指令

4. 后处理阶段
   ├── TinyCore 模式下的指令还原
   └── 解包单独指令（solo instructions）
```

### 1.4 关键算法和数据结构

#### 1.4.1 DFA 资源跟踪
- 使用 `ResourceTracker` 跟踪机器资源（功能单元、时隙等）
- 通过 DFA 状态表检查指令是否可以加入当前包

#### 1.4.2 依赖关系处理
- **数据依赖（Data Dependence）**: 通过 `.new` 指令转换消除
- **反依赖（Anti Dependence）**: 大多数情况下可以忽略
- **输出依赖（Output Dependence）**: 通常阻止打包
- **顺序依赖（Order Dependence）**: 内存操作的特殊处理

#### 1.4.3 指令优化技术
- **.new 指令**: 使用前一个包中产生的新值
- **.cur 指令**: 使用当前包中产生的值（HVX 向量指令）
- **新值存储（New Value Store）**: 使用刚产生的值进行存储
- **新值跳转（New Value Jump）**: 使用刚产生的值进行条件跳转

## 二、使用的其他代码文件及其作用

### 2.1 Hexagon 目标特定文件

#### 2.1.1 HexagonVLIWPacketizer.h
- **作用**: 定义 `HexagonPacketizerList` 类的接口
- **提供**: 类声明、方法签名、成员变量定义

#### 2.1.2 Hexagon.h
- **作用**: Hexagon 目标的主要头文件
- **提供**: 目标相关的常量和定义

#### 2.1.3 HexagonInstrInfo.h / HexagonInstrInfo.cpp
- **作用**: Hexagon 指令信息类
- **提供功能**:
  - 指令属性查询（是否为谓词指令、是否为 .new 指令等）
  - 指令转换（获取 .new、.cur、.old 操作码）
  - 指令类型判断（HVX、内存操作、控制流等）
  - 指令约束检查（时隙限制、资源需求等）
- **关键方法**:
  - `isPredicated()`, `isDotNewInst()`, `isDotCurInst()`
  - `getDotNewOp()`, `getDotCurOp()`, `getDotOldOp()`
  - `mayBeNewStore()`, `isNewValueStore()`, `isNewValueJump()`
  - `isSolo()`, `isSchedulingBoundary()`

#### 2.1.4 HexagonRegisterInfo.h / HexagonRegisterInfo.cpp
- **作用**: Hexagon 寄存器信息类
- **提供功能**:
  - 寄存器类查询（`getMinimalPhysRegClass()`）
  - 寄存器关系（`isSuperRegister()`）
  - 特殊寄存器（返回地址寄存器、栈指针寄存器等）
- **关键方法**:
  - `getRARegister()`, `getFrameRegister()`, `getStackRegister()`

#### 2.1.5 HexagonSubtarget.h / HexagonSubtarget.cpp
- **作用**: Hexagon 子目标特性类
- **提供功能**:
  - 架构特性查询（`hasV60OpsOnly()`, `hasV65Ops()` 等）
  - 打包器配置（`usePackets()`）
  - TinyCore 模式支持（`isTinyCoreWithDuplex()`）
  - 调度器变异（Mutations）: UsrOverflowMutation、HVXMemLatencyMutation、BankConflictMutation

### 2.2 LLVM 核心代码生成文件

#### 2.2.1 llvm/CodeGen/DFAPacketizer.h
- **作用**: DFA 打包器基类定义
- **提供**: 
  - `DFAPacketizer` 类：核心 DFA 引擎，管理自动机状态和资源跟踪
  - `VLIWPacketizerList` 基类：实现通用的 DFA 资源跟踪和打包逻辑
  - `DefaultVLIWScheduler` 类：默认的 VLIW 调度器，构建依赖图
- **关键功能**:
  - DFA 状态管理（通过 `Automaton` 类）
  - 资源预留和释放（`canReserveResources()`, `reserveResources()`）
  - 指令包创建和管理
  - 调度依赖图构建（继承自 `ScheduleDAGInstrs`）
- **DFA 工作原理**:
  - DFA 从目标的 `Schedule.td` 文件自动生成
  - 状态表示功能单元消耗的所有可能组合
  - 输入是指令类集合
  - 转换表示将指令添加到包的操作
  - 有效转换表示指令可以加入当前包

#### 2.2.2 llvm/CodeGen/ScheduleDAG.h
- **作用**: 调度依赖图（Scheduling DAG）定义
- **提供**:
  - `SUnit`（调度单元）类
  - `SDep`（调度依赖）类，包含依赖类型（Data、Anti、Output、Order）
  - 依赖关系遍历接口

#### 2.2.3 llvm/CodeGen/MachineFunction.h / MachineFunction.cpp
- **作用**: 机器函数表示
- **提供**: 机器函数的基本操作，包含所有基本块

#### 2.2.4 llvm/CodeGen/MachineBasicBlock.h / MachineBasicBlock.cpp
- **作用**: 机器基本块表示
- **提供**: 基本块中的指令迭代、插入、删除操作

#### 2.2.5 llvm/CodeGen/MachineInstr.h / MachineInstr.cpp
- **作用**: 机器指令表示
- **提供功能**:
  - 指令操作数访问
  - 指令属性查询（`mayLoad()`, `mayStore()`, `isCall()`, `isBranch()` 等）
  - 指令打包标记（`isBundledWithPred()`, `isBundledWithSucc()`）
  - 寄存器使用/定义查询

#### 2.2.6 llvm/CodeGen/MachineInstrBundle.h
- **作用**: 指令包（Bundle）操作
- **提供**: `finalizeBundle()` 函数，将多个指令打包成 BUNDLE

#### 2.2.7 llvm/CodeGen/MachineOperand.h
- **作用**: 机器操作数表示
- **提供**: 操作数类型判断（寄存器、立即数、内存引用等）

#### 2.2.8 llvm/CodeGen/MachineFunctionPass.h
- **作用**: 机器函数 Pass 基类
- **提供**: Pass 框架接口，用于集成到 LLVM Pass 管道

#### 2.2.9 llvm/CodeGen/MachineFrameInfo.h
- **作用**: 栈帧信息
- **提供**: 栈大小查询，用于 ALLOCFRAME 相关优化

#### 2.2.10 llvm/CodeGen/TargetRegisterInfo.h
- **作用**: 目标寄存器信息基类
- **提供**: 寄存器相关的通用接口

### 2.3 LLVM 分析和优化文件

#### 2.3.1 llvm/Analysis/AliasAnalysis.h
- **作用**: 别名分析
- **提供**: 内存别名查询，用于判断内存操作是否可以重排序
- **使用场景**: 在 `isLegalToPacketizeTogether()` 中检查存储-加载依赖

#### 2.3.2 llvm/CodeGen/MachineBranchProbabilityInfo.h
- **作用**: 分支概率信息
- **提供**: 分支概率查询，用于 .new 谓词指令的选择优化

#### 2.3.3 llvm/CodeGen/MachineDominators.h
- **作用**: 机器代码支配树
- **提供**: 支配关系查询（虽然在此文件中未直接使用，但在 Pass 依赖中声明）

#### 2.3.4 llvm/CodeGen/MachineLoopInfo.h
- **作用**: 机器代码循环信息
- **提供**: 循环结构查询，用于计算停顿周期（stall cycles）

### 2.4 LLVM 支持库文件

#### 2.4.1 llvm/ADT/BitVector.h
- **作用**: 位向量容器
- **使用**: 在 `hasDeadDependence()` 中跟踪死定义寄存器

#### 2.4.2 llvm/ADT/DenseSet.h
- **作用**: 密集集合容器
- **使用**: 在调试代码中查找重复的寄存器定义

#### 2.4.3 llvm/MC/MCInstrDesc.h
- **作用**: 机器码指令描述符
- **提供**: 指令的元数据（操作数数量、类型、调度类等）

#### 2.4.4 llvm/Support/CommandLine.h
- **作用**: 命令行选项支持
- **提供**: 编译时选项定义（`-disable-packetizer`, `-hexagon-packetize-volatiles` 等）

#### 2.4.5 llvm/Support/Debug.h
- **作用**: 调试输出支持
- **提供**: `LLVM_DEBUG` 宏，用于条件编译的调试信息

#### 2.4.6 llvm/Support/raw_ostream.h
- **作用**: 原始输出流
- **提供**: 调试和错误信息的输出接口

### 2.5 其他依赖

#### 2.5.1 llvm/InitializePasses.h
- **作用**: Pass 初始化
- **提供**: `INITIALIZE_PASS_BEGIN/END` 宏，用于注册 Pass

#### 2.5.2 llvm/IR/DebugLoc.h
- **作用**: 调试位置信息
- **提供**: 调试信息位置标记

## 三、关键功能模块详解

### 3.1 指令打包决策流程

```
isLegalToPacketizeTogether(SUI, SUJ)
├── 检查是否可以共存（cannotCoexist）
├── 检查死依赖（hasDeadDependence）
├── 检查控制依赖（hasControlDependence）
├── 检查寄存器掩码依赖（hasRegMaskDependence）
├── 检查双存储依赖（hasDualStoreDependence）
├── 检查新值跳转依赖
└── 检查调度依赖图中的依赖
    ├── 尝试 .cur 转换
    ├── 尝试 .new 转换
    ├── 检查谓词互补性
    └── 处理各种依赖类型
```

### 3.2 指令优化转换

#### 3.2.1 .new 指令转换
- **条件**: 存在数据依赖，且源指令可以产生新值
- **效果**: 消除数据依赖，允许指令打包
- **限制**: 需要满足谓词一致性、寄存器类限制等

#### 3.2.2 .cur 指令转换
- **条件**: HVX 向量指令，且在当前包中有数据产生者
- **效果**: 使用当前包中产生的值
- **限制**: 仅适用于 HVX 向量加载指令

#### 3.2.3 新值存储转换
- **条件**: 存储指令的值来自同一包中的指令
- **效果**: 使用新值存储指令，减少延迟
- **限制**: 严格的约束条件（无其他存储、无后增量、谓词一致性等）

### 3.3 资源管理

- **DFA 状态跟踪**: 通过 `ResourceTracker` 管理功能单元和时隙
- **常量扩展器资源**: 4 字节的额外资源需求
- **停顿检测**: 计算指令与前一包的依赖导致的停顿周期

### 3.4 特殊指令处理

- **单独指令（Solo Instructions）**: 不能与其他指令打包的指令
- **调度边界（Scheduling Boundaries）**: 定义打包区域的边界
- **内联汇编**: 特殊处理，可能从包中移出
- **KILL 伪指令**: 在打包前移除，避免依赖分析混淆

## 四、总结

`HexagonVLIWPacketizer.cpp` 是一个复杂的指令打包器实现，它：

1. **利用 DFA 进行资源管理**: 通过确定性有限自动机跟踪和分配机器资源
2. **智能依赖处理**: 通过指令转换（.new、.cur）消除或绕过依赖关系
3. **架构特定优化**: 充分利用 Hexagon 架构的特性（新值存储、新值跳转、谓词指令等）
4. **集成 LLVM 框架**: 充分利用 LLVM 的分析基础设施（别名分析、循环信息、分支概率等）

该实现展示了如何在一个成熟的编译器框架中实现目标特定的优化，同时保持与通用框架的良好集成。
