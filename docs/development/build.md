# 构建与工具链

QoderZ 用 Nix flake 固定整套工具链。目标很简单：换一台 Linux 机器，执行 `nix develop`，就能得到与 CI 一致的 GCC 14、CMake、Ninja 和 clang-format，不需要在系统里手工拼环境。

## 工具链一览

| 组件 | 来源 | 用途 |
| --- | --- | --- |
| GCC 14 | `nixpkgs.gcc14` | C++23 编译 |
| CMake / Ninja | devShell | 配置与构建 |
| Boost | devShell | Asio / Beast / URL（HTTP 客户端与异步协程） |
| OpenSSL | devShell | TLS、AES、RSA、SHA/MD5 与安全随机 |
| nlohmann_json | devShell | JSON |
| CLI11 | devShell | 命令行解析 |
| Catch2 v3 | devShell | 单元测试 |
| clang-tools | devShell | clang-format 格式门禁 |
| gdb / curl / jq | devShell | 调试与排查 |

`flake.nix` 的 devShell 通过 `gcc14Stdenv` 固定编译器，并在 `shellHook` 里显式导出 `CC` / `CXX`。`nix build` 同样使用 GCC 14，不会悄悄换成系统默认编译器。

## 第一次进入

```sh
nix develop
```

首次执行会下载或构建依赖，耗时取决于本地缓存和网络。进入后应看到类似：

```text
qoderz dev shell: 14.4.0 / cmake 4.4.4
```

也可以不进入交互 shell，直接执行单条命令：

```sh
nix develop --command cmake --version
```

## 配置、构建、测试

```sh
nix develop --command cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
nix develop --command cmake --build build
nix develop --command ctest --test-dir build --output-on-failure
```

`scripts/build.sh` 把上面三步的配置与构建合起来，并按机器的核数和内存挑一个安全的并行度：

```sh
scripts/build.sh              # 配置并构建 build/
scripts/build.sh build-fast    # 换一个构建目录
```

要点：

- 构建目录固定为 `build/`，源码树保持干净。
- `-DBUILD_TESTING=ON` 才会配置 Catch2 测试；关闭时只构建 `qoderz`。
- `CMakeLists.txt` 会把 `project(qoderz VERSION ...)` 写入 `build/generated/common/version.hpp`。这个文件由构建系统生成，不要手改，也不要提交。
- `CMAKE_EXPORT_COMPILE_COMMANDS=ON`，`build/compile_commands.json` 可供 clangd 使用。

### 构建速度

一次冷构建约一分钟，其中大半时间花在 `src/common/net/detail/connection.cpp`：它把 Beast 的
HTTP 模板对明文和 TLS 两种 stream 各实例化一遍，单独一个编译单元就要 70 秒左右，
因此它是整个构建的关键路径，再高的并行度也压不下去。

`CMakeLists.txt` 为此做了两件事：

- 关闭 GCC 的 C++ 模块依赖扫描（`CMAKE_CXX_SCAN_FOR_MODULES=OFF`）。项目不使用 C++20 模块，
  而 `-fmodules-ts` 会让 ccache 把每一次编译判为不可缓存（"Unsupported compiler option"），
  顺带每个编译单元还要多跑一遍扫描。关掉之后可缓存率从 0% 升到 90% 以上。
- 自动探测 `ccache` 并用 `CMAKE_CXX_COMPILER_LAUNCHER` 接上。缓存默认在
  `~/.cache/qoderz/ccache`（遵循 XDG，**不在源码树里**），上限由 devShell 的
  `CCACHE_MAXSIZE` 控制（默认 5 GiB），可用 `CCACHE_DIR` 覆盖。

  缓存不能放源码树：`nix build` 每次都会把源码树复制进 store，几 GB 的缓存目录会让每次
  flake 操作都变慢，而且 ccache 一直在写入新对象、源码指纹随之移动。也不能放 `.git/`：
  linked worktree 里 `.git` 是个文件，而且版本控制元数据本该与构建产物分开。

  升级过配置的旧 shell 里可能还留着指向源码树的 `CCACHE_DIR`，它会让
  `${CCACHE_DIR:-默认}` 的回退失效；`unset CCACHE_DIR` 或重开终端即可。

实测（28 核 / 13 GiB 内存的机器）：

| 场景 | 关扫描前 | 关扫描 + ccache |
| --- | --- | --- |
| 冷构建 | 81 s | 66 s |
| 清空产物后重建 | 75 s | 18 s |
| 改一个头文件 | 30 s+ | 22 s |

### `nix build` 不经过 ccache

`nix build .#` 在沙箱里构建，只把 `buildInputs` 与 `nativeBuildInputs` 加进 `PATH`，
而 `ccache` 不在其中，也不继承 devShell 的 `shellHook`。配置阶段会明确打印：

```text
-- qoderz: ccache not found; compiling directly
```

这是有意的取舍，不是配置遗漏。Nix 的缓存单位是**整个 derivation**：源码不变时直接复用 store
里的结果、一次编译都不做；源码一改则全部重建。让 ccache 在沙箱里生效需要把缓存目录挂进沙箱，
那会破坏可复现性，而可复现性正是用 Nix 构建的理由。

所以两者分工明确：

| 目的 | 命令 | 缓存行为 |
| --- | --- | --- |
| 日常开发循环 | `scripts/build.sh` | 走 ccache，改一个文件通常数秒 |
| 发布产物 | `nix build .#` | 走 Nix store，源码不变则秒回，改了则全量重建 |

### 并行度与内存

一个 Beast 编译单元的峰值常驻内存接近 1 GiB，`-j8` 在 13 GiB 的机器上会摸到尖峰并开始换页。
`scripts/build.sh` 因此取两个上限的较小值：每个编译单元 1 GiB、总编译器内存不超过 6 GiB；
同时给系统留 2 个核（核数大于 4 时）。默认结果是 `-j6` 左右。

需要手动控制时，直接给 `cmake --build build --parallel N`；N 不要超过 `内存 GiB 数` 的一半，
否则容易 OOM。
运行：

```sh
./build/qoderz --version
./build/qoderz --help
```

输出应包含 `qoderz 0.1.2`。

## Nix 构建与运行

```sh
nix build .#
./result/bin/qoderz --version
nix run .# -- --version
```

- `packages.default` 与 `packages.qoderz` 指向同一个推导。
- `apps.default` 直接运行包里的 `bin/qoderz`。
- 安装规则把可执行文件放到 `bin/`；`nix build` 的结果因此是 `result/bin/qoderz`。

本地 git 仓库有一个容易踩的坑：Nix 只把 **Git 跟踪的文件** 放进 flake 源。新文件在第一次 `nix build .#` 前需要 `git add`（只暂存即可，不必提交）；否则可能出现：

```text
Path 'flake.nix' in the repository ... is not tracked by Git.
```

## 安装到自定义前缀

```sh
nix develop --command cmake --install build --prefix "$PWD/out"
./out/bin/qoderz --version
```

CMake 使用 `GNUInstallDirs`，所以 `--prefix` 下的可执行文件位于 `bin/`。打包或部署时不要手工复制二进制，优先使用安装规则，避免漏掉运行时依赖。

## CMake 结构

| 目标 | 内容 |
| --- | --- |
| `qoderz_core` | `src/**/*.cpp`（`GLOB_RECURSE`，去掉 `main.cpp`），公共头文件目录 `src/` 与生成目录 |
| `qoderz` | `src/main.cpp`，链接 `qoderz_core`、CLI11、Boost、OpenSSL |
| `qoderz_tests` | Catch2 v3 测试，只在 `BUILD_TESTING=ON` 时创建 |
| `qoderz_e2e` | 进程级端到端测试，通过 `QODERZ_BINARY` / `QODERZ_STUB_UPSTREAM` 编译定义拿到被测二进制路径 |
| `qoderz_stub_upstream` | loopback stub 上游，只在 `BUILD_TESTING=ON` 时构建，不安装 |

依赖使用 target-based 写法，例如 `Boost::headers`、`OpenSSL::SSL`、`nlohmann_json::nlohmann_json`、`CLI11::CLI11`。不要退回到全局 `include_directories()` 或 `link_libraries()`，那会让依赖边界迅速失控。

## 故障排查

### `nix develop` 很慢，或卡在下载

首次进入 devShell、第一次 `nix build` 都要拉依赖。先确认网络与 substituter 可用：

```sh
nix config show | grep substituters
nix develop --command cmake --version
```

如果只是偶发下载失败，重试一次通常就好；持续失败时把完整错误和 `nix config show` 的相关输出记录下来，不要用关闭沙箱或跳过哈希校验的方式绕过。

### Nix 报 `Path 'flake.nix' ... is not tracked by Git`

本地仓库没有提交，或新文件还没进索引。执行 `git add`（无需 commit）后重试。这是 Nix 对本地 git 仓库的源过滤规则，不是 CMake 的问题。

### CMake 找不到 Boost / OpenSSL / Catch2

先确认是在 devShell 里执行：

```sh
nix develop --command bash -lc 'echo $CMAKE_PREFIX_PATH; cmake -S . -B build -G Ninja -DBUILD_TESTING=ON'
```

不要在系统 shell 里直接运行 `cmake`。`Boost` 使用配置模式查找（`find_package(Boost REQUIRED CONFIG)`），devShell 已经提供 `BoostConfig.cmake`。

### 编译器不是 GCC 14

CMake 会缓存第一次配置时的编译器。检查并清理：

```sh
echo "$CC"; echo "$CXX"
rm -rf build
nix develop --command cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
```

`CMakeLists.txt` 对 GCC 有最低版本检查；如果显示 GCC 版本过低，说明没有在 devShell 里配置。

### `CMakeCache.txt` 目录不一致

常见于把带 `build/` 的源码树复制到别处，或 Nix 构建把旧构建目录带进源。删除本地 `build/` 后重新配置；`flake.nix` 的源过滤已经排除了 `build/`、`result/`、`.git/` 和 `.direnv/`。

### `ctest` 显示没有测试

确认配置时带了 `-DBUILD_TESTING=ON`，并检查 `build/tests/qoderz_tests` 是否存在。CMake 的 `BUILD_TESTING` 默认开启，但显式传入更稳妥。修改测试列表后重新运行 CMake，而不是只运行 `cmake --build`。

### clang-format 版本不一致

格式门禁只认 devShell 中的版本：

```sh
nix develop --command clang-format --version
nix develop --command scripts/format.sh --check
```

不要用编辑器内置的 clang-format 批量改写文件，容易产生与仓库配置不同的缩进或换行。

### `result` 还是旧的

`nix build .#` 会更新当前目录下的 `result` 符号链接；如果看到旧版本，重新构建并确认：

```sh
readlink -f result
./result/bin/qoderz --version
```

## 清理

```sh
rm -rf build result
```

需要连 Nix 评估缓存一起清理时，使用 `nix flake check` / `nix build` 的自然重建即可；不要删除 `.git` 或仓库外的共享缓存。
