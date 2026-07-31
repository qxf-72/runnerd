# 进程启动器

`process_launcher` 负责创建子进程和返回父进程需要管理的资源，不负责正常
任务生命周期中的 epoll、waitpid、状态结算、超时或取消。如果 fork 后的
父进程配置失败，它会终止并回收刚创建的子进程，避免泄漏子进程资源。

返回的资源由 `ProcessMonitor` 接管：它把三个读端和 `signalfd` 注册到
runnerd 的 epoll，持续采集输出，循环调用 `waitpid`，并在所有 pipe 到达
EOF 且子进程退出后结算任务状态。

## fd 所有权

| 资源 | 父进程保留 | 子进程保留 |
| --- | --- | --- |
| stdout pipe | 读端 | 写端，重定向到 stdout |
| stderr pipe | 读端 | 写端，重定向到 stderr |
| startup error pipe | 读端 | 写端，exec 成功后 CLOEXEC 自动关闭 |
| /dev/null | 不保留 | 重定向到 stdin |

## 子进程启动顺序

1. 设置 `PR_SET_PDEATHSIG` 为 `SIGKILL`。
2. 检查父进程是否已在设置前退出。
3. 创建独立进程组，PGID 等于 PID。
4. 重定向 stdin、stdout、stderr。
5. 直接调用 `execve`。
6. execve 失败时，通过 startup error pipe 写入阶段和 errno，并 `_exit(127)`。

## 约束

- JobSpec 的 argv[0] 必须是绝对路径。
- runnerd 不拼接 shell 命令。
- 父进程读端均为非阻塞 fd，供后续 epoll 注册。
- 父进程必须在任务运行期间持续排空 stdout 和 stderr，不能先阻塞
  `waitpid` 再读取；否则任务输出写满 pipe 后，父子进程会互相等待。
- startup error pipe 同样是非阻塞字节流。读取方需要累积数据并处理
  `EAGAIN`、EOF 和不完整的 `ChildStartupError`，不能假设一次 `read`
  一定返回完整结构。
