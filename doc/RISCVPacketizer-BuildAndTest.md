# RISCVPacketizer 编译与测试指南

本文档说明如何编译修改后的 RISCVPacketizer Pass，以及如何运行相关测试进行验证。

## 一、前置条件

- 已安装 CMake、Ninja（推荐）或 Unix Makefiles
- 已安装 C++ 编译器（如 GCC 或 Clang）
- 本仓库为 `llvm-project` 根目录（包含 `llvm/`、`clang/` 等子目录）

## 二、配置与编译

### 2.1 首次构建（尚无 build 目录时）

在 **llvm-project 根目录** 下执行：

```bash
# 使用 Ninja，仅构建 LLVM（不含 Clang 等）：
cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# 若需同时构建 Clang 以便用 clang 生成 RISCV 代码并跑完整测试，可加：
# cmake -S llvm -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug \
#   -DLLVM_ENABLE_PROJECTS="clang;lld"
```

其中 `-B build` 可改为你的构建目录名（例如 `-B build-debug`）。

### 2.2 编译（含 RISCVPacketizer 修改）

RISCVPacketizer 源码位于：

- `llvm/lib/Target/RISCV/RISCVPacketizer.cpp`
- `llvm/lib/Target/RISCV/RISCVPacketizer.h`

修改上述文件后，在 **llvm-project 根目录** 下执行：

```bash
# 使用你的实际构建目录，例如 build 或 build-debug
ninja -C build
```

该命令会检测变更并重新编译 `LLVMRISCVCodeGen` 及依赖它的目标（如 `llc`）。若只想触发 RISCV 后端相关编译，可指定库目标：

```bash
ninja -C build LLVMRISCVCodeGen
```

然后再编译会用到该库的工具（例如 `llc`）：

```bash
ninja -C build llc
```

## 三、运行测试

### 3.1 使用 lit 运行 RISCV CodeGen 测试（推荐）

RISCVPacketizer 属于 RISCV 后端 CodeGen 的一部分，会参与所有面向 RISCV 的编译。运行 RISCV CodeGen 测试可验证后端（含 Packetizer）是否正常：

在 **构建目录** 下执行（或从项目根目录指定构建目录下的 `llvm-lit`）：

```bash
# 在项目根目录下，构建目录为 build 时：
./build/bin/llvm-lit llvm/test/CodeGen/RISCV -v

# 若构建目录为 build-debug：
./build-debug/bin/llvm-lit llvm/test/CodeGen/RISCV -v
```

- `-v` 为可选，表示输出更详细的测试信息。
- 测试源路径为 **相对于当前工作目录** 的 `llvm/test/CodeGen/RISCV`，请保证在 **llvm-project 根目录** 下执行，或为 `llvm-lit` 传入正确的绝对路径。

仅运行单个测试文件示例：

```bash
./build/bin/llvm-lit llvm/test/CodeGen/RISCV/add-imm.ll -v
```

### 3.2 使用 Ninja 的 check 目标

在构建目录中执行：

```bash
# 运行整个 LLVM 测试套件（耗时较长）：
ninja -C build check-llvm

# 若启用了 Clang，可运行全部测试：
ninja -C build check-all
```

这会间接覆盖到 RISCV CodeGen（含 RISCVPacketizer），但不会只跑 RISCV 子集。

## 四、手动验证 Packetizer Pass（可选）

若想单独观察 `riscv-packetizer` Pass 对 MIR 的作用，可使用 `llc` 的 `-run-pass`：

```bash
# 使用构建好的 llc，对某 IR 只运行 riscv-packetizer：
./build/bin/llc -march=riscv64 -mcpu=generic -run-pass=riscv-packetizer -o - your.ll
```

**打开 riscv-packetizer 的 debug 输出：**

在命令中加上 `-debug-only=packets`（与源码中的 `DEBUG_TYPE "packets"` 对应），仅打印该 Pass 的调试信息：

```bash
./build-debug/bin/llc -march=riscv32 -mcpu=generic -run-pass=riscv-packetizer -debug-only=packets -o - your.ll
```

若需打印所有 Pass 的 debug 信息，使用 `-debug`（输出会很多）。

**注意：** `-debug-only` / `-debug` 仅在 **Debug 构建**（或启用 `LLVM_ENABLE_ASSERTIONS` 的构建）下生效；Release 构建中 `LLVM_DEBUG()` 会被编译掉。

或从 IR 生成 MIR 后再对 MIR 跑该 Pass，以检查 packet 划分结果。

## 五、常见问题

- **lit 报错找不到测试或配置**  
  请确认：  
  1）在 **llvm-project 根目录** 下执行；  
  2）使用的 `llvm-lit` 来自当前构建目录（`build/bin/llvm-lit` 或 `build-debug/bin/llvm-lit`）；  
  3）传入的测试路径为 `llvm/test/CodeGen/RISCV`（或具体 `.ll`/`.mir` 文件）。

- **RISCV 测试被跳过（UNSUPPORTED）**  
  `llvm/test/CodeGen/RISCV/lit.local.cfg` 中会检查 `config.root.targets` 是否包含 `"RISCV"`。若 CMake 未启用 RISCV 目标，这些测试会被标记为不支持。请确保使用默认或已启用 RISCV 的 LLVM 配置重新配置并编译。

- **只想快速验证编译是否通过**  
  修改 RISCVPacketizer 后执行：  
  `ninja -C build LLVMRISCVCodeGen llc`  
  无报错即表示编译通过；功能验证仍需按第三节运行测试。

---

**相关源码与配置：**

- Pass 实现：`llvm/lib/Target/RISCV/RISCVPacketizer.cpp`，头文件：`RISCVPacketizer.h`
- Pass 注册与调用：`llvm/lib/Target/RISCV/RISCVTargetMachine.cpp`（`addPass(createRISCVPacketizerPass())`）
- RISCV CodeGen 测试目录：`llvm/test/CodeGen/RISCV/`
