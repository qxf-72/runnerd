# runnerd 需求说明

## 项目目标

runnerd 是一个面向同一用户、本机运行的任务执行守护服务。

客户端 runnerctl 通过 Unix Domain Socket 向 runnerd 提交任务、
查询任务状态、列出任务以及取消任务。

## 第一阶段核心功能

- Unix Domain Socket 通信
- 长度前缀协议
- fork + execve 启动任务
- stdout 和 stderr 采集
- 最大并发数和等待队列
- 超时和手动取消
- SIGCHLD 子进程回收
- 任务历史持久化
- 自动化测试

## 明确不做

- TCP 远程访问
- HTTP 接口
- 多用户和权限系统
- 容器和 cgroup
- 数据库
- 分布式执行
- 线程池
