# HexagonVLIWPacketizer.cpp 函数调用关系分析

## 一、顶层调用流程

```
LLVM Pass 管理器
    ↓
HexagonPacketizer::runOnMachineFunction()  [第198行]
    ├── 初始化分析结果（别名分析、分支概率等）
    ├── 创建 HexagonPacketizerList 实例
    ├── 移除 KILL 伪指令
    ├── TinyCore 模式：转换指令
    ├── 遍历所有基本块
    │   └── 对每个调度区域调用：
    │       └── Packetizer.PacketizeMIs()  [基类方法，第154行]
    ├── TinyCore 模式：还原指令
    └── unpacketizeSoloInstrs()  [第268行]
```

## 二、核心打包流程（PacketizeMIs）

```
VLIWPacketizerList::PacketizeMIs()  [基类，第154行]
    ├── VLIWScheduler->startBlock()
    ├── VLIWScheduler->enterRegion()
    ├── VLIWScheduler->schedule()  [构建依赖图]
    │   ├── buildSchedGraph()  [基类方法]
    │   └── postProcessDAG()  [应用变异]
    ├── 生成 MI -> SU 映射表
    └── 主循环：遍历每个指令
        ├── initPacketizerState()  [第1025行]
        ├── isSoloInstruction()  [第1057行]
        │   └── 如果是单独指令 → endPacket()
        ├── ignorePseudoInstruction()  [第1035行]
        ├── ResourceTracker->canReserveResources()  [DFA资源检查]
        ├── shouldAddToPacket()  [第1826行]
        │   ├── producesStall()  [第1931行]
        │   │   └── calcStall()  [第1870行]
        │   └── 检查 Duplex 配对
        ├── 依赖检查循环（对当前包中的每个指令）
        │   ├── isLegalToPacketizeTogether()  [第1316行] ⭐核心函数
        │   │   ├── cannotCoexist()  [第1152行]
        │   │   ├── hasDeadDependence()  [第1198行]
        │   │   ├── hasControlDependence()  [第1225行]
        │   │   ├── hasRegMaskDependence()  [第1261行]
        │   │   ├── hasDualStoreDependence()  [第1291行]
        │   │   └── 处理调度依赖图中的依赖
        │   │       ├── isCallDependent()  [第294行]
        │   │       ├── canPromoteToDotCur()  [第398行]
        │   │       │   └── promoteToDotCur()  [第367行]
        │   │       ├── canPromoteToDotNew()  [第841行]
        │   │       │   ├── canPromoteToNewValue()  [第811行]
        │   │       │   │   └── canPromoteToNewValueStore()  [第642行]
        │   │       │   └── restrictingDepExistInPacket()  [第911行]
        │   │       └── promoteToDotNew()  [第449行]
        │   │           ├── arePredicatesComplements()  [第958行]
        │   │           └── updateOffset()  [第507行]
        │   └── isLegalToPruneDependencies()  [第1634行]
        │       ├── cannotCoexist()
        │       ├── demoteToDotOld()  [第462行]
        │       ├── cleanUpDotCur()  [第376行]
        │       ├── useCalleesSP()  [第489行]
        │       ├── undoChangedOffset()  [第543行]
        │       └── updateOffset()
        └── addToPacket()  [第1701行]
            ├── producesStall()
            ├── calcStall()
            ├── ResourceTracker->reserveResources()
            ├── tryAllocateResourcesForConstExt()  [第285行]
            │   └── ResourceTracker->canReserveResources()
            └── endPacket()  [如果常量扩展器失败，第1780行]
                ├── foundLSInPacket()  [第1681行]
                └── finalizeBundle()  [创建 BUNDLE 指令]
```

## 三、详细函数调用关系图

### 3.1 主要打包决策函数

```
isLegalToPacketizeTogether()  [第1316行]
    ├── cannotCoexist()  [第1152行]
    │   └── cannotCoexistAsymm()  [第1097行]
    ├── hasDeadDependence()  [第1198行]
    │   └── hasWriteToReadDep()  [静态函数，第140行]
    ├── hasControlDependence()  [第1225行]
    ├── hasRegMaskDependence()  [第1261行]
    ├── hasDualStoreDependence()  [第1291行]
    └── 遍历调度依赖图
        ├── isRegDependence()  [静态函数，第318行]
        ├── isCallDependent()  [第294行]
        ├── canPromoteToDotCur()  [第398行]
        │   └── promoteToDotCur()  [第367行]
        ├── canPromoteToDotNew()  [第841行]
        │   ├── canPromoteToNewValue()  [第811行]
        │   │   └── canPromoteToNewValueStore()  [第642行]
        │   │       ├── getStoreValueOperand()  [静态函数，第601行]
        │   │       ├── isLoadAbsSet()  [静态函数，第606行]
        │   │       └── getAbsSetOperand()  [静态函数，第620行]
        │   ├── restrictingDepExistInPacket()  [第911行]
        │   │   └── getPredicatedRegister()  [静态函数，第940行]
        │   └── isImplicitDependency()  [静态函数，第827行]
        ├── promoteToDotNew()  [第449行]
        │   ├── arePredicatesComplements()  [第958行]
        │   │   ├── getPredicateSense()  [静态函数，第558行]
        │   │   └── getPredicatedRegister()
        │   └── updateOffset()  [第507行]
        │       └── getPostIncrementOperand()  [静态函数，第567行]
        └── 其他依赖处理
```

### 3.2 指令优化转换函数

```
promoteToDotNew()  [第449行]
    ├── arePredicatesComplements()  [第958行]
    │   ├── getPredicateSense()  [静态函数，第558行]
    │   └── getPredicatedRegister()  [静态函数，第940行]
    └── updateOffset()  [第507行]
        └── getPostIncrementOperand()  [静态函数，第567行]

promoteToDotCur()  [第367行]
    └── (直接修改指令操作码)

demoteToDotOld()  [第462行]
    └── (恢复指令操作码)

cleanUpDotCur()  [第376行]
    └── (清理 .cur 指令标记)
```

### 3.3 资源管理函数

```
addToPacket()  [第1701行]
    ├── producesStall()  [第1931行]
    │   └── calcStall()  [第1870行]
    ├── ResourceTracker->canReserveResources()
    ├── ResourceTracker->reserveResources()
    ├── tryAllocateResourcesForConstExt()  [第285行]
    │   ├── ResourceTracker->canReserveResources()
    │   └── ResourceTracker->reserveResources()
    ├── canReserveResourcesForConstExt()  [第279行]
    │   └── tryAllocateResourcesForConstExt(false)
    └── reserveResourcesForConstExt()  [第274行]
        └── tryAllocateResourcesForConstExt(true)
```

### 3.4 辅助检查函数

```
shouldAddToPacket()  [第1826行]
    ├── producesStall()  [第1931行]
    │   └── calcStall()  [第1870行]
    └── HII->isDuplexPair()  [检查 Duplex 配对]

isSoloInstruction()  [第1057行]
    └── HII->isSolo()  [检查是否为单独指令]

ignorePseudoInstruction()  [第1035行]
    └── (检查伪指令)

cannotCoexist()  [第1152行]
    └── cannotCoexistAsymm()  [第1097行]
        └── isSystemInstr()  [静态函数，第1186行]
```

### 3.5 依赖检查函数

```
hasDeadDependence()  [第1198行]
    └── hasWriteToReadDep()  [静态函数，第140行]

hasControlDependence()  [第1225行]
    ├── isControlFlow()  [静态函数，第335行]
    └── isDirectJump()  [静态函数，第323行]

hasRegMaskDependence()  [第1261行]
    └── doesModifyCalleeSavedReg()  [静态函数，第340行]

hasDualStoreDependence()  [第1291行]
    └── (检查双存储依赖)
```

### 3.6 后处理函数

```
endPacket()  [第1780行]
    ├── foundLSInPacket()  [第1681行]
    └── finalizeBundle()  [创建 BUNDLE 指令]

unpacketizeSoloInstrs()  [第1157行]
    └── moveInstrOut()  [静态函数，第154行]
        └── (将单独指令从包中移出)
```

## 四、关键函数说明

### 4.1 核心决策函数

1. **isLegalToPacketizeTogether()** [第1316行]
   - **作用**：判断两条指令是否可以打包在一起
   - **调用链**：被 `PacketizeMIs()` 主循环调用
   - **关键检查**：
     - 共存性检查
     - 死依赖检查
     - 控制依赖检查
     - 寄存器掩码依赖检查
     - 双存储依赖检查
     - 调度依赖图依赖检查（尝试 .new/.cur 转换）

2. **isLegalToPruneDependencies()** [第1634行]
   - **作用**：判断是否可以剪枝依赖关系
   - **调用链**：当 `isLegalToPacketizeTogether()` 返回 false 时调用
   - **功能**：尝试通过指令转换消除依赖

3. **shouldAddToPacket()** [第1826行]
   - **作用**：判断指令是否应该添加到当前包
   - **调用链**：在资源检查通过后调用
   - **检查**：停顿检查、Duplex 配对检查

### 4.2 指令优化函数

1. **promoteToDotNew()** [第449行]
   - **作用**：将指令提升为 .new 指令（使用前一个包产生的新值）
   - **调用链**：在 `isLegalToPacketizeTogether()` 中，当检测到数据依赖时调用

2. **promoteToDotCur()** [第367行]
   - **作用**：将指令提升为 .cur 指令（使用当前包产生的值）
   - **调用链**：在 `isLegalToPacketizeTogether()` 中，针对 HVX 向量指令调用

3. **canPromoteToNewValueStore()** [第642行]
   - **作用**：检查是否可以转换为新值存储
   - **调用链**：被 `canPromoteToNewValue()` → `canPromoteToDotNew()` 调用

### 4.3 资源管理函数

1. **addToPacket()** [第1701行]
   - **作用**：将指令添加到当前包
   - **调用链**：在 `PacketizeMIs()` 主循环的最后调用
   - **功能**：
     - 预留资源
     - 处理常量扩展器
     - 处理新值跳转

2. **endPacket()** [第1780行]
   - **作用**：结束当前包，创建 BUNDLE 指令
   - **调用链**：在多种情况下调用（资源不足、依赖无法消除、单独指令等）

### 4.4 辅助函数

1. **calcStall()** [第1870行]
   - **作用**：计算指令导致的停顿周期
   - **调用链**：被 `producesStall()` → `shouldAddToPacket()` 和 `addToPacket()` 调用

2. **initPacketizerState()** [第1025行]
   - **作用**：初始化打包器状态
   - **调用链**：在 `PacketizeMIs()` 主循环开始时调用

## 五、函数调用层次总结

```
Level 1: 入口函数
    └── runOnMachineFunction()

Level 2: 核心打包流程
    └── PacketizeMIs()  [基类方法]

Level 3: 主循环中的决策
    ├── shouldAddToPacket()
    ├── isLegalToPacketizeTogether()
    ├── isLegalToPruneDependencies()
    └── addToPacket()

Level 4: 依赖和优化检查
    ├── cannotCoexist()
    ├── hasDeadDependence()
    ├── hasControlDependence()
    ├── hasRegMaskDependence()
    ├── hasDualStoreDependence()
    ├── canPromoteToDotNew()
    ├── canPromoteToDotCur()
    ├── promoteToDotNew()
    └── promoteToDotCur()

Level 5: 底层辅助函数
    ├── producesStall()
    ├── calcStall()
    ├── isCallDependent()
    ├── arePredicatesComplements()
    └── 各种静态辅助函数
```

## 六、关键调用路径示例

### 示例1：正常打包流程
```
PacketizeMIs()
    → shouldAddToPacket()  [检查是否应该添加]
    → isLegalToPacketizeTogether()  [检查依赖]
    → addToPacket()  [添加到包]
        → ResourceTracker->reserveResources()  [预留资源]
```

### 示例2：通过 .new 转换消除依赖
```
PacketizeMIs()
    → isLegalToPacketizeTogether()
        → 检测到数据依赖
        → canPromoteToDotNew()  [检查是否可以转换]
            → canPromoteToNewValue()
                → canPromoteToNewValueStore()
        → promoteToDotNew()  [执行转换]
    → addToPacket()  [添加转换后的指令]
```

### 示例3：资源不足时结束包
```
PacketizeMIs()
    → ResourceTracker->canReserveResources()  [返回 false]
    → endPacket()  [结束当前包]
        → finalizeBundle()  [创建 BUNDLE]
    → 开始新包
```

## 七、静态函数列表

以下函数是文件作用域的静态函数，被多个成员函数调用：

1. `hasWriteToReadDep()` [第140行] - 检查写后读依赖
2. `moveInstrOut()` [第154行] - 将指令从包中移出
3. `isRegDependence()` [第318行] - 检查寄存器依赖类型
4. `isDirectJump()` [第323行] - 检查直接跳转
5. `isSchedBarrier()` [第327行] - 检查调度屏障
6. `isControlFlow()` [第335行] - 检查控制流指令
7. `doesModifyCalleeSavedReg()` [第340行] - 检查是否修改被调用者保存寄存器
8. `getPredicateSense()` [第558行] - 获取谓词方向
9. `getPostIncrementOperand()` [第567行] - 获取后增量操作数
10. `getStoreValueOperand()` [第601行] - 获取存储值操作数
11. `isLoadAbsSet()` [第606行] - 检查加载绝对设置
12. `getAbsSetOperand()` [第620行] - 获取绝对设置操作数
13. `isImplicitDependency()` [第827行] - 检查隐式依赖
14. `getPredicatedRegister()` [第940行] - 获取谓词寄存器
15. `cannotCoexistAsymm()` [第1097行] - 检查非对称共存性
16. `isSystemInstr()` [第1186行] - 检查系统指令

这些静态函数提供了可重用的辅助功能，支持主要的打包决策逻辑。
