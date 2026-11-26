# newfs

这是基于 Linux FUSE 2 的课程文件系统。`main` 包含必做任务一、任务二和附加题一至三的实现。

## 环境准备

准备支持 FUSE 挂载的 Linux 环境，安装 GCC、Make、CMake、FUSE 2 开发库、`fusermount` 和 Python 3。确认环境提供 `/dev/fuse`，并允许当前用户挂载。

```bash
git clone --branch main https://github.com/nagi-hikaru/fuse-simple-ext2.git
cd fuse-simple-ext2
make -C driver/user_ddriver all
```

`make` 从源码生成 `$HOME/lib/libddriver.a`。克隆得到的 `fs/newfs` 已是完整工程，后续命令可以直接编译。以下命令均从仓库根目录执行。

任务一和任务二基础测评会覆盖或清空个人默认磁盘 `~/ddriver`。盘上已有数据时，请先备份或用独立的用户环境运行。

## 任务一 · 简单文件系统 demo

demo 从第 500 个逻辑块读取预设目录项，挂载后通过 `ls` 显示对应文件名。全新克隆先生成 4 MiB 的测试镜像，测评通过时显示 `Test Pass!!!`。[demo 源码](../demo/src/main.c)

```bash
(
  cd fs/demo/tests &&
  truncate -s 4194304 disk2 &&
  bash test.sh
)
```

## 任务二 · 基础文件系统

`newfs` 支持挂载、卸载、`mkdir`、`touch`、`ls` 和重挂载恢复。脚本会检查目录和文件操作、磁盘布局及位图。

```bash
lab5_driver_bin=$(mktemp -d)
ln -s "$PWD/driver/ddriver.sh" "$lab5_driver_bin/ddriver"
PATH="$lab5_driver_bin:$PATH" DDRIVER_TYPE=u bash fs/newfs/tests/test.sh
```

按提示输入 `N` 运行四个基础阶段，通过时输出 `Score: 30/30`。

## 附加题一 · 普通文件功能

已实现跨块读写、截断、文件和目录删除、重命名与移动、硬链接和软链接。当前单文件上限为 6 KiB。

下面的独立测评使用临时磁盘，验证跨块读写、硬链接、重命名、删除目录项和重挂载。

```bash
cmake -S fs/newfs -B fs/newfs/build -DCMAKE_BUILD_TYPE=Debug
cmake --build fs/newfs/build -j"$(nproc)"
python3 fs/newfs/bonus/tests/run_bonus.py --suite base
```

## 附加题二 · CheckpointFS

`create` 将工作文件复制成独立检查点。`restore` 复制历史检查点，再通过硬链接把工作文件切换到新分支。测评覆盖重复版本名、空文件、链接关系和重挂载。

```bash
bash fs/newfs/tests/test.sh checkpoint
```

## 附加题三 · VectorFS

访问 `/.search/<查询词>` 会显示文档相似度排名，前十项以虚拟软链接指向 `/documents` 下的原文件。测评覆盖排名、链接目标、文档修改后的缓存刷新和重挂载。

```bash
bash fs/newfs/tests/test.sh vector
```

## 附加题四 · 在文件系统上训练模型

`main` 上仍待完成。课程的 `model` 分支提供训练和评估脚本，运行前需要扩展虚拟磁盘和单文件容量，并准备 PyTorch 环境。

## 联合测评

联合测评会自动编译 `newfs`，在同一块临时磁盘上运行附加题一的普通文件场景、CheckpointFS 和 VectorFS，再重挂载检查持久化。通过时显示 `PASS: all 全部通过，共 25 步。`

```bash
bash fs/newfs/tests/test.sh all
```
