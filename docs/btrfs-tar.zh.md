# btrfs-tar

`btrfs-tar` 用于在**不挂载文件系统**的情况下，把未挂载的 Btrfs 块设备或镜像转换成 `tar.gz` 归档。

它适合离线导出场景，例如原始 Btrfs 设备、磁盘镜像、快照盘或 NBD 导出。

## 工作原理

它直接读取 Btrfs 树，在用户态遍历元数据，读取 inode、目录项、符号链接和 extent，然后生成 gzip 压缩的 POSIX ustar tar 包。

基本流程：

1. 只读打开 Btrfs
2. 选择 subvolume
3. 可选解析归档根路径
4. 遍历目录并写 tar header
5. 读取文件数据并写入 tar
6. 可选做路径重映射
7. 写入 tar 结束标记

因此它**不依赖**内核挂载视图，可以直接处理离线文件系统。

## 支持的对象

- 普通文件
- 目录
- 符号链接

稀疏文件空洞会补零写入。默认跳过嵌套 snapshot/subvolume，使用 `--snapshots` 可包含它们。

## subvolume 选择

默认导出 default subvolume；若未设置，则回退到 top-level subvolume（`id 5`）。

可显式指定：

- `--subvolid <id>`
- `--subvol <path>`

`--subvol <path>` 从 top-level Btrfs 树解析。

## `--root-path`

`--root-path <path>` 用于指定所选 subvolume 中哪个目录作为归档根。
例如 `workspace` 中有 `diff/etc/...`，执行：

```bash
btrfs-tar --subvol workspace --root-path diff /dev/nbd100 rwlayer.tar.gz
```

则归档中会得到 `etc/...`。

## `--export-path`

`--export-path <src:dst>` 只导出所选 subvolume 中的某个路径，并把它写到 tar 中的目标路径。

- `src` 从所选 subvolume 根开始解析
- `dst` 是 tar 中的目标路径
- 只要出现任意 `--export-path`，就跳过整棵 subvolume 的常规遍历
- 可指定多次
- 与 `--root-path`、`--path-map` 互斥

示例：

```bash
btrfs-tar \
  --subvol workspace \
  --export-path docker:var/lib/docker \
  /dev/nbd100 workspace.tar.gz
```

这会只导出 `workspace/docker/*`，并在 tar 中写成 `var/lib/docker/*`。

## `--path-map`

`--path-map <src:dst>` 会把一棵额外子树再归档到另一个目标路径。

- `src` 从所选 subvolume 根开始解析
- `dst` 是 tar 中的目标路径
- 写入顺序在主归档树之后，因此通常可覆盖前面的条目
- `src` 不要求位于 `--root-path` 之下

示例：

```bash
btrfs-tar \
  --subvol workspace \
  --root-path diff \
  --path-map docker:var/lib/docker \
  /dev/nbd100 rwlayer.tar.gz
```

## 常用命令

```bash
btrfs-tar /dev/nbd100 fs.tar.gz
btrfs-tar --subvolid 256 /dev/nbd100 workspace.tar.gz
btrfs-tar --subvol workspace /dev/nbd100 workspace.tar.gz
btrfs-tar --subvol workspace --root-path diff /dev/nbd100 rwlayer.tar.gz
btrfs-tar --subvol workspace --export-path docker:var/lib/docker /dev/nbd100 docker.tar.gz
```

## 使用说明

- 转换时源设备应保持离线
- 输出格式为 gzip 压缩的 POSIX ustar tar 包
- 因 `--path-map` 产生的重复路径，通常以后写入的条目为准
- 目标路径前导 `/` 会被忽略
