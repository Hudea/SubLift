# Build Directory Layout

所有 CMake 构建输出统一放置在 `build/` 子目录下，**不要在仓库根目录使用 `build-*` 后缀创建平级构建目录**。

## 目录约定

- `build/debug/`：Debug 构建目录（默认开发调试、单元测试、冒烟测试）
- `build/release/`：Release 构建目录（端到端基准测试、质量/耗时/RSS 门禁验证）
- `build/<preset>/`：其他专项构建（如 `build/asan/`、`build/review/` 等）

## 常用命令

```bash
# Debug 配置、编译与测试
cmake -B build/debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/debug
ctest --test-dir build/debug -L unit --output-on-failure   # 日常开发：仅跑 unit
ctest --test-dir build/debug --output-on-failure           # 全量：unit + parity + adapter + e2e

# Release 配置、编译与测试
cmake -B build/release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/release
ctest --test-dir build/release --output-on-failure

# 端到端分层验证脚本（默认优先自动探测 build/release 与 build/debug）
./scripts/run_e2e_tests.sh --all
```

> 注：`.gitignore` 忽略 `build/*` 下的全部编译产物，仅保留本 `build/README.md` 随仓库跟踪。
