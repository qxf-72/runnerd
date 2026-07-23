# runnerd

Linux 本地任务执行守护服务。

## 当前状态

项目初始化阶段，核心功能尚未实现。

## 构建

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

## 运行
```bash
./build/runnerd
./build/runnerctl
cd build
ctest --output-on-failure
```
