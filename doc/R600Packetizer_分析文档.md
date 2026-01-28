# R600Packetizer.cpp 代码分析文档

## 一、代码功能概述

### 1.1 文件作用

`R600Packetizer.cpp` 是 LLVM 编译器中用于 AMD R600 系列 GPU 架构的 VLIW（Very Long Instruction Word）指令打包器实现。该文件实现了 R600 架构特定的指令打包逻辑，将多个 ALU 指令打包成 VLIW 指令包，以提高指令级并行性。

### 1.2 核心功能

1. **指令打包（Packetization）**: 将多个 ALU 指令打包成 VLIW 指令包
2. **PreviousVector (PV) 寄存器替换**: 将源寄存器替换为 PV 寄存器以利用前一个包的结果
3. **Bank Swizzle 分配**: 为打包的指令分配 Bank Swizzle 值以满足读取端口限制
4. **资源约束检查**: 检查常量读取限制和读取端口限制
5. **Trans Slot 处理**: 处理 VLIW5 架构中的 Trans 时隙

### 1.3 R600 架构特点

- **VLIW 架构**: R600 使用 VLIW 指令格式，一个指令包可以包含多个操作
- **VLIW5 vs VLIW4**: 
  - VLIW5: 5 个时隙（4 个 ALU + 1 个 Trans）
  - VLIW4 (Cayman): 4 个 ALU 时隙，无 Trans 时隙
- **PreviousVector (PV) 寄存器**: 特殊寄存器（PV_X, PV_Y, PV_Z, PV_W），用于访问前一个指令包的结果
- **Bank Swizzle**: 用于解决寄存器文件读取端口限制的机制

## 二、代码结构分析

### 2.1 核心类结构

#### 2.1.1 R600Packetizer 类
- **继承关系**: 继承自 `MachineFunctionPass`
- **作用**: 作为 LLVM Pass，负责对整个机器函数进行打包处理
- **主要方法**:
  - `runOnMachineFunction()`: 主要的执行入口，遍历所有基本块进行打包
  - `getAnalysisUsage()`: 声明所需的分析依赖（支配树、循环信息）

#### 2.1.2 R600PacketizerList 类
- **继承关系**: 继承自 `VLIWPacketizerList`（定义在 `DFAPacketizer.h` 中）
- **作用**: 实现 R600 特定的打包逻辑
- **核心成员变量**:
  - `TII`: R600 指令信息
  - `TRI`: R600 寄存器信息
  - `VLIW5`: 是否为 VLIW5 架构（非 Cayman）
  - `ConsideredInstUsesAlreadyWrittenVectorElement`: 标记是否使用了已写入的向量元素

### 2.2 主要工作流程

```
1. 初始化阶段
   ├── 获取 R600InstrInfo、R600RegisterInfo
   ├── 创建 R600PacketizerList 实例
   ├── 判断架构类型（VLIW5 或 VLIW4）
   └── 初始化 DFA 状态表

2. 预处理阶段
   ├── 移除 KILL 伪指令
   ├── 移除 IMPLICIT_DEF 指令
   └── 移除空的 CF_ALU 指令

3. 打包阶段（对每个基本块）
   ├── 识别调度区域（scheduling regions）
   ├── 对每个区域调用 PacketizeMIs()
   │   ├── 检查资源可用性（DFA）
   │   ├── 检查指令依赖关系
   │   ├── 检查时隙冲突
   │   ├── 检查常量读取限制
   │   ├── 检查读取端口限制（Bank Swizzle）
   │   ├── 替换 PV 寄存器
   │   └── 设置 isLast 位
   └── 结束当前包并创建 BUNDLE 指令

4. 后处理阶段
   └── 完成打包，生成最终的指令包
```

## 三、关键功能实现详解

### 3.1 PreviousVector (PV) 寄存器处理

#### 3.1.1 getPreviousVector() 方法
**功能**: 获取前一个指令包中产生的寄存器到 PV 寄存器的映射关系

**实现逻辑**:
```cpp
DenseMap<unsigned, unsigned> getPreviousVector(MachineBasicBlock::iterator I)
```

1. **查找前一个指令包**: 向前查找前一个 ALU 指令或 BUNDLE
2. **遍历包中的指令**: 处理包中的所有指令（通过 `isBundledWithPred()` 判断）
3. **计算 PV 映射**:
   - **Trans 指令**: 映射到 `R600::PS`（Previous Scalar）
   - **DOT4 指令**: 映射到 `R600::PV_X`
   - **普通指令**: 根据目标寄存器的通道（X/Y/Z/W）映射到对应的 PV 寄存器
     - 通道 0 → `R600::PV_X`
     - 通道 1 → `R600::PV_Y`
     - 通道 2 → `R600::PV_Z`
     - 通道 3 → `R600::PV_W`
4. **过滤条件**:
   - 跳过谓词指令（如果 write 位为 0）
   - 跳过 OQAP 寄存器

#### 3.1.2 substitutePV() 方法
**功能**: 将指令的源操作数替换为 PV 寄存器

**实现逻辑**:
1. 遍历指令的源操作数（src0, src1, src2）
2. 如果源寄存器在 PV 映射中，则替换为对应的 PV 寄存器
3. 这样可以避免寄存器依赖，利用前一个包的结果

**示例**:
```
前一个包: R0.x = ADD ...
当前包:   R1.x = ADD R0.x, ...  →  R1.x = ADD PV_X, ...
```

### 3.2 时隙（Slot）管理

#### 3.2.1 getSlot() 方法
**功能**: 获取指令的目标寄存器所在的硬件通道（时隙）

**实现**: 
```cpp
unsigned getSlot(const MachineInstr &MI) const {
    return TRI.getHWRegChan(MI.getOperand(0).getReg());
}
```

- R600 架构中，寄存器按通道（X/Y/Z/W）组织
- 每个通道对应一个 ALU 时隙
- 时隙必须按顺序使用（0, 1, 2, 3）

#### 3.2.2 时隙冲突检测
在 `isBundlableWithCurrentPMI()` 中：
- 检查新指令的时隙是否大于等于包中最后一个指令的时隙
- 如果违反顺序，在 VLIW5 架构下可以转换为 Trans 时隙
- 如果 `ConsideredInstUsesAlreadyWrittenVectorElement` 为真，允许使用 Trans 时隙

### 3.3 Trans 时隙处理

**Trans 时隙特点**:
- 仅在 VLIW5 架构中存在（Cayman/VLIW4 没有）
- 用于处理时隙顺序冲突的情况
- 不能读取 LDS 源寄存器
- 使用 `R600::PS`（Previous Scalar）而不是 PV_X/Y/Z/W

**处理逻辑**:
1. 检测是否为 Trans 指令：`TII->isTransOnly(MI)`
2. 检测时隙冲突时自动转换为 Trans：如果时隙顺序违反且满足条件
3. Trans 时隙的指令会立即结束当前包

### 3.4 Bank Swizzle 分配

**Bank Swizzle 的作用**:
- R600 寄存器文件有读取端口限制
- 多个指令同时读取同一 Bank 的寄存器会导致冲突
- Bank Swizzle 通过重新排列读取顺序来避免冲突

**实现**:
1. 调用 `TII->fitsReadPortLimitations()` 检查读取端口限制
2. 如果满足限制，返回一个 Bank Swizzle 向量
3. 为包中的每个指令设置对应的 Bank Swizzle 值

### 3.5 常量读取限制检查

**实现**:
```cpp
if (!TII->fitsConstReadLimitations(CurrentPacketMIs)) {
    // 不能打包，因为违反了常量读取限制
    return false;
}
```

- R600 架构对每个包中能读取的常量数量有限制
- 需要检查当前包中的所有指令是否满足限制

### 3.6 指令打包决策

#### 3.6.1 isLegalToPacketizeTogether() 方法
**功能**: 判断两个指令是否可以打包在一起

**检查项**:
1. **时隙冲突**: 如果两个指令使用相同的时隙，标记 `ConsideredInstUsesAlreadyWrittenVectorElement`
2. **谓词一致性**: 两个指令必须使用相同的谓词寄存器（pred_sel）
3. **依赖关系**:
   - 忽略反依赖（Anti Dependence）
   - 输出依赖：如果目标寄存器不同，可以忽略
   - 其他依赖：不能打包
4. **地址寄存器冲突**: 
   - 如果两个指令都定义地址寄存器，不能打包
   - 如果两个指令都使用地址寄存器，不能打包
   - 一个定义、一个使用：可以打包

#### 3.6.2 isBundlableWithCurrentPMI() 方法
**功能**: 检查指令是否可以与当前包中的指令打包

**检查流程**:
1. **时隙检查**: 检查目标寄存器时隙顺序
2. **常量限制**: 检查常量读取限制
3. **读取端口限制**: 检查 Bank Swizzle 是否满足读取端口限制
4. **Trans 时隙限制**: Trans 时隙不能读取 LDS 源寄存器

### 3.7 isLast 位设置

**功能**: 设置指令的 `isLast` 位，标记包中的最后一个指令

**实现**:
```cpp
void setIsLastBit(MachineInstr *MI, unsigned Bit) const {
    unsigned LastOp = TII->getOperandIdx(MI->getOpcode(), R600::OpName::last);
    MI->getOperand(LastOp).setImm(Bit);
}
```

- 包中最后一个指令的 `isLast` 位为 1
- 其他指令的 `isLast` 位为 0

### 3.8 单独指令（Solo Instructions）处理

**isSoloInstruction() 方法**:
返回 `true` 的指令类型：
1. **向量指令**: `TII->isVector(MI)`
2. **非 ALU 指令**: `!TII->isALUInstr(MI.getOpcode())`
3. **GROUP_BARRIER**: 组屏障指令
4. **LDS 指令**: 本地数据共享指令（由于限制检查不完善，暂时作为单独指令）

## 四、使用的其他代码文件及其作用

### 4.1 R600 目标特定文件

#### 4.1.1 MCTargetDesc/R600MCTargetDesc.h
- **作用**: R600 机器码目标描述
- **提供**: 目标相关的常量和定义，包括 PV 寄存器定义（PV_X, PV_Y, PV_Z, PV_W, PS）

#### 4.1.2 R600.h
- **作用**: R600 目标的主要头文件
- **提供**: 目标相关的常量和操作码定义

#### 4.1.3 R600Subtarget.h / R600Subtarget.cpp
- **作用**: R600 子目标特性类
- **提供功能**:
  - 架构特性查询（`hasCaymanISA()` 用于判断是否为 VLIW4）
  - 指令信息获取（`getInstrInfo()`）

#### 4.1.4 R600InstrInfo.h / R600InstrInfo.cpp
- **作用**: R600 指令信息类
- **提供功能**:
  - 指令属性查询（`isALUInstr()`, `isVector()`, `isTransOnly()` 等）
  - 操作数索引查询（`getOperandIdx()`）
  - 寄存器通道查询（通过 `getRegisterInfo()` 访问）
  - 限制检查（`fitsConstReadLimitations()`, `fitsReadPortLimitations()`）
  - 地址寄存器查询（`definesAddressRegister()`, `usesAddressRegister()`）
- **关键方法**:
  - `isALUInstr()`, `isVector()`, `isVectorOnly()`, `isTransOnly()`
  - `isPredicated()`, `isLDSInstr()`
  - `fitsConstReadLimitations()`, `fitsReadPortLimitations()`
  - `definesAddressRegister()`, `usesAddressRegister()`
  - `readsLDSSrcReg()`

#### 4.1.5 R600RegisterInfo.h / R600RegisterInfo.cpp
- **作用**: R600 寄存器信息类
- **提供功能**:
  - 硬件寄存器通道查询（`getHWRegChan()`）
  - 寄存器类信息

### 4.2 LLVM 核心代码生成文件

#### 4.2.1 llvm/CodeGen/DFAPacketizer.h
- **作用**: DFA 打包器基类定义
- **提供**: 
  - `VLIWPacketizerList` 基类，实现通用的 DFA 资源跟踪和打包逻辑
  - `DFAPacketizer` 类：核心 DFA 引擎
- **关键功能**:
  - DFA 状态管理
  - 资源预留和释放
  - 指令包创建和管理

#### 4.2.2 llvm/CodeGen/ScheduleDAG.h
- **作用**: 调度依赖图（Scheduling DAG）定义
- **提供**:
  - `SUnit`（调度单元）类
  - `SDep`（调度依赖）类，包含依赖类型（Data、Anti、Output、Order）
  - 依赖关系遍历接口

#### 4.2.3 llvm/CodeGen/MachineDominators.h
- **作用**: 机器代码支配树
- **提供**: 支配关系查询（在 Pass 依赖中声明，但在此文件中未直接使用）

#### 4.2.4 llvm/CodeGen/MachineLoopInfo.h
- **作用**: 机器代码循环信息
- **提供**: 循环结构查询（传递给 VLIWPacketizerList 基类）

### 4.3 其他依赖

#### 4.3.1 llvm/ADT/DenseMap.h
- **作用**: 密集映射容器
- **使用**: 存储 PV 寄存器映射关系

#### 4.3.2 llvm/Support/Debug.h
- **作用**: 调试输出支持
- **提供**: `LLVM_DEBUG` 宏，用于条件编译的调试信息

## 五、关键算法和数据结构

### 5.1 PV 寄存器映射算法

```
getPreviousVector(I):
  1. I-- (向前查找)
  2. if (不是 ALU 指令 && 不是 BUNDLE) return empty
  3. if (是 BUNDLE) BI++ (跳过 BUNDLE 头)
  4. do:
     a. 计算时隙 BISlot = getSlot(*BI)
     b. 检查是否为 Trans (LastDstChan >= BISlot)
     c. 跳过谓词指令（write=0）
     d. 获取目标寄存器 Dst
     e. 根据指令类型和通道计算 PV 寄存器
     f. Result[Dst] = PVReg
  5. while (BI->isBundledWithPred())
  6. return Result
```

### 5.2 打包决策算法

```
isBundlableWithCurrentPMI(MI):
  1. 检查时隙顺序
     - if (getSlot(MI) <= getSlot(last)) 
       - if (VLIW5 && 满足条件) → 转换为 Trans
       - else return false
  2. CurrentPacketMIs.push_back(&MI)
  3. 检查常量限制
     - if (!fitsConstReadLimitations) return false
  4. 检查读取端口限制
     - if (!fitsReadPortLimitations) return false
  5. 检查 Trans 时隙限制
     - if (isTransSlot && readsLDSSrcReg) return false
  6. return true
```

### 5.3 数据结构

#### 5.3.1 DenseMap<unsigned, unsigned>
- **用途**: PV 寄存器映射
- **键**: 源寄存器编号
- **值**: 对应的 PV 寄存器编号（PV_X/Y/Z/W 或 PS）

#### 5.3.2 std::vector<R600InstrInfo::BankSwizzle>
- **用途**: 存储每个指令的 Bank Swizzle 值
- **索引**: 对应 CurrentPacketMIs 中的指令索引

#### 5.3.3 CurrentPacketMIs
- **类型**: `std::vector<MachineInstr *>`
- **用途**: 存储当前正在构建的包中的指令

## 六、特殊处理和优化

### 6.1 VLIW5 vs VLIW4 处理

- **VLIW5**: 
  - 有 Trans 时隙
  - 可以处理时隙顺序冲突
  - 使用 `R600::PS` 作为 Trans 时隙的 PV
  
- **VLIW4 (Cayman)**:
  - 无 Trans 时隙
  - 严格的时隙顺序要求
  - 更简单的打包逻辑

### 6.2 时隙顺序优化

- **正常情况**: 时隙必须按 0→1→2→3 的顺序使用
- **冲突处理**: VLIW5 中可以使用 Trans 时隙处理冲突
- **向量元素重用**: 如果检测到使用已写入的向量元素，允许使用 Trans 时隙

### 6.3 依赖关系优化

- **反依赖忽略**: 大多数反依赖可以忽略，因为操作数在写入前已读取
- **输出依赖**: 如果目标寄存器不同，可以忽略
- **地址寄存器**: 一个定义、一个使用的情况可以打包（避免同时定义或同时使用）

## 七、代码执行流程示例

### 7.1 典型打包流程

```
输入指令序列:
  R0.x = ADD ...
  R1.y = MUL R0.x, ...
  R2.z = SUB R1.y, ...

处理过程:
1. 第一个包: [R0.x = ADD ...]
   - isLast = 1
   - PV 映射: {}

2. 第二个包: [R1.y = MUL PV_X, ...]
   - isLast = 1
   - PV 映射: {R0.x → PV_X}
   - 替换: R0.x → PV_X

3. 第三个包: [R2.z = SUB PV_Y, ...]
   - isLast = 1
   - PV 映射: {R1.y → PV_Y}
   - 替换: R1.y → PV_Y
```

### 7.2 Trans 时隙使用示例

```
输入指令序列:
  R0.x = ADD ...
  R1.x = MUL ...  // 时隙冲突！

处理过程:
1. 第一个包: [R0.x = ADD ...]
   - isLast = 1

2. 第二个包: [R1.x = MUL ...] (Trans 时隙)
   - 检测到时隙冲突
   - 转换为 Trans 时隙
   - 使用 PS 而不是 PV_X
   - 立即结束包
```

## 八、总结

`R600Packetizer.cpp` 是一个针对 AMD R600 GPU 架构的专用指令打包器，主要特点：

1. **PV 寄存器优化**: 通过 PreviousVector 寄存器替换，利用前一个包的结果，减少寄存器依赖
2. **时隙管理**: 严格的时隙顺序管理，VLIW5 支持 Trans 时隙处理冲突
3. **资源约束**: 检查常量读取限制和读取端口限制，确保打包的合法性
4. **Bank Swizzle**: 通过 Bank Swizzle 分配解决寄存器文件读取端口冲突
5. **架构适配**: 支持 VLIW5 和 VLIW4 两种架构模式

该实现充分利用了 R600 架构的 VLIW 特性，通过智能的指令打包和 PV 寄存器优化，提高了指令级并行性和执行效率。
