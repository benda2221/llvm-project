# Dandelion 浅依赖编译器实验记录

## 1. 实验信息

- 日期：2026-08-26
- 分支：`shallow-dependency`
- 核心实现提交：`131a0b5d0`（`[RISCV] Add shallow dependency packetization`）
- 构建目录：`build-debug`
- 目标：`riscv32`，`-mcpu=dandelion`，`-mattr=+m,+f,-d`
- 对照方法：同一编译器使用隐藏选项
  `-riscv-disable-shallow-dependency` 关闭包内 RAW，避免由不同构建或其他
  后端改动引入干扰。

## 2. 已实现内容

1. `isShallowDepOK()` 只依据 Dandelion 行程类的槽位掩码、stage 周期和
   def 可用周期判定资格；当前只接受槽 1～7、EX1 单周期执行、EX2 得到
   结果的整数 ALU。
2. Packetizer 直接消费调度 DAG 的 `SDep::Data/Output/Anti`，不从
   MachineOperand 二次推导 RAW/WAW/WAR。
3. 包内所有 Data 边只能连接同一有序生产者/消费者端点对。WAW 始终保
   序；只有双方均具备浅依赖资格的 WAR 才保序。
4. 每次候选入包都复制包状态并对全包重新求解槽位。依赖或槽位失败时丢
   弃试探状态，新指令从下一包重新开始，不残留 `InternalRead`。
5. Packetizer 将“原顺序成员到目标槽位”的映射编码在 BUNDLE 第一个
   immediate 中，不物理移动成员。
6. PackPadding 只校验并消费映射、按槽位重排、填 NOP，并写回规范化
   `[0,1,2,3,4,5,6,7]` 映射；缺失、数量不符、重复或行程类不合法的
   映射会直接报错。
7. 浅依赖消费者的 `InternalRead` 会经过 Padding 和 bundle 生命周期
   重建保留下来；internal kill 会转换为正确的包级 dead def。
8. Relayout 新建的完整包立即记录规范化槽位映射。

为实验计数增加了四个 LLVM `STATISTIC`：

- `NumShallowDataEdges`
- `NumShallowEndpointPairs`
- `NumRejectedSecondDataPair`
- `NumSlotConstraintSplits`

## 3. 定向测试

测试覆盖：

- 一对 `ADDI -> ADD` 浅 RAW 同包及 `InternalRead`；
- 两组 RAW、三级 RAW 链、单生产者多消费者和多生产者单消费者拆包；
- MUL 等多周期 RAW 拒绝；
- 两条灵活 ALU 的 WAW 递增槽位；
- ALU/ALU WAR 保序且不误标为 RAW；
- 非浅依赖 IntToFP/FEQ_S WAR 的非单调 `[2,0,7]` 映射；
- Packetizer-only 保持成员原顺序，PackPadding 按映射重排；
- 损坏映射诊断和重复执行 PackPadding；
- 现有 WAR 快照、inline asm/fence 和最终发射回归。

执行命令：

```bash
cmake --build build-debug --target llc -j2

build-debug/bin/llvm-lit -sv \
  llvm/test/CodeGen/RISCV/dandelion-shallow-dependency.ll \
  llvm/test/CodeGen/RISCV/dandelion-shallow-dependency.mir \
  llvm/test/CodeGen/RISCV/dandelion-bundle-slot-map.mir \
  llvm/test/CodeGen/RISCV/dandelion-bundle-dependencies.mir \
  llvm/test/CodeGen/RISCV/dandelion-bundle-snapshot-war.ll \
  llvm/test/CodeGen/RISCV/dandelion-inlineasm-fence.ll
```

结果：`6/6 PASS`。使用过滤器运行全部 Dandelion/feature 相关用例时为
`7/7 PASS`。

完整 RISC-V CodeGen 回归：

```bash
build-debug/bin/llvm-lit -sv -j8 llvm/test/CodeGen/RISCV
```

结果：`2211/2211 PASS`，耗时 485.45 秒。

定向 MIR 的 `-stats` 结果：接受 4 条浅 Data 边和 4 个端点对；4 次候选
因引入第二个 Data 端点对被拒绝；槽位约束无解拆包为 0 次。

## 4. 静态 A/B 实验

统计口径：在 `riscv-pack-padding` 后统计 BUNDLE 数；每包固定 8 槽，
`FEQ_S x0,f0,f0` 和 `ADDI x0,x0,0` 计为填充 NOP；代码尺寸由
`llvm-size` 的 `text` 列给出。

| 用例 | 模式 | 包数 | 非 NOP 指令 | 槽位利用率 | internal use | text |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| LLVM 两函数微基准 | 关闭浅依赖 | 7 | 7 | 12.50% | 0 | 284 B |
| LLVM 两函数微基准 | 开启浅依赖 | 5 | 7 | 17.50% | 2 | 220 B |
| `functest/add` 主 IR | 关闭浅依赖 | 64 | 91 | 17.77% | 0 | 2400 B |
| `functest/add` 主 IR | 开启浅依赖 | 55 | 91 | 20.68% | 10 | 2112 B |
| NMSIS `conv_fast_q15` | 关闭浅依赖 | 44 | 64 | 18.18% | 0 | 1526 B |
| NMSIS `conv_fast_q15` | 开启浅依赖 | 32 | 64 | 25.00% | 14 | 1142 B |

变化摘要：

- LLVM 微基准：包数减少 28.57%，目标发射文本由 224 B 减到 160 B；
  `llvm-size` text 减少 22.54%（该列还包含 60 B unwind 数据）。
- `functest/add` 主 IR：包数减少 14.06%，text 减少 12.00%。
- NMSIS `conv_fast_q15`：包数减少 27.27%，槽位利用率提高 6.82 个百分点，
  text 减少 25.16%。其统计为接受 14 条 Data 边/14 个端点对，8 次因第二
  个端点对拆包，0 次因槽位约束无解拆包。

## 5. ZirconSim 对照结果与当前限制

使用当前 `ZirconSim` 的 simulator-only 模式运行同一个 `functest/add`：

```bash
make -B NAMES=add run USE_SIMULATOR_ONLY_MODE=1 \
  LLCFLAGS='--fp-contract=off -riscv-disable-shallow-dependency'
```

关闭浅依赖的对照二进制运行成功：

```text
SIMULATION ENDED SUCCESSFULLY.
Total cycles: 519, Total insts: 4152, IPC: 8
add: ACCEPT
```

开启浅依赖的二进制在现有模拟器上运行失败：

```text
SIMULATION ENDED WITH a0 != 0.
Total cycles: 25, Total insts: 200, IPC: 8
add: FAILED
```

这是当前软硬件协同实验的明确边界，不应把失败数据当作浅依赖性能结果。
编译器现在会把消费者标在同一包的高槽，但现有硬件/模拟器仍让各槽指令
在 EX1 同时执行，没有实现实验方案要求的“浅消费者 EX1 不执行，在 EX2
接收低槽 ALU 结果后执行”。关闭浅依赖后同一工具链和程序通过，说明普通
打包、链接和模拟路径没有回归。完成处理器的包内浅前递和消费者 EX2 执行
后，必须重新运行该功能测试及 NMSIS 动态周期/结果正确性实验，才能完成端
到端验收和给出可信的动态加速比。

## 6. 结论

编译器侧的资格判定、唯一 Data 端点对、WAW/WAR 顺序、事务式全包槽位
求解、BUNDLE 槽位映射、Padding 物化和 `InternalRead` 生命周期均已通过
定向及现有 Dandelion 回归。三个静态样本都减少了指令包和代码尺寸。当前
尚未满足的是处理器/模拟器执行语义；在该硬件功能完成前，浅依赖默认开启
生成的程序不能宣称具备端到端运行正确性。
