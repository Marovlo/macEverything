# MacEverything

macOS 下的文件名即时搜索工具，灵感来自 Windows 上的 [Everything](https://www.voidtools.com/)。

输入文件名片段，毫秒级列出全盘匹配结果；文件系统变化实时进入索引，无需手动刷新。

## 功能

- **即时搜索**：基于内存索引与 mmap 只读映射，百万级文件名查询毫秒级返回
- **搜索语法**：空格分词（AND）、`!词` 排除、`*` `?` 通配符；大小写、音调、全半角不敏感
- **排序**：点击表头按名称 / 路径 / 大小 / 修改时间排序，点击切换升降序；按大小排序时文件夹置底
- **命中高亮**：搜索命中的文件名片段加粗高亮
- **实时索引**：FSEvents 监听文件系统变化增量更新，支持增量补偿与损坏区域重扫
- **文件操作**：双击打开、右键 / ⌘R 在 Finder 中显示、⌘C 拷贝路径
- **状态栏常驻**：⌃⌥Space 全局呼出搜索窗口，左键图标搜索，右键菜单
- **搜索历史**：在结果里打开或显示过的文件，会按词条自动记录在首页；点击词条即可重新搜索
- **自动更新**：定期检查 GitHub Releases，发现新版本弹窗提醒

## 安装

从 [Releases](https://github.com/Marovlo/macEverything/releases/latest) 下载 `MacEverything.dmg`，打开后把 MacEverything 拖到「应用程序」。

> 首次打开若被 Gatekeeper 拦截：在「应用程序」中右键 MacEverything → 「打开」→ 再点「打开」。

## 使用

| 操作 | 方式 |
|---|---|
| 呼出搜索窗口 | ⌃⌥Space 或点击菜单栏 🔍 ME 图标 |
| 打开文件 | 双击 |
| 在 Finder 中显示 | ⌘R 或右键菜单 |
| 拷贝路径 | ⌘C 或右键菜单 |
| 关闭窗口 | ⌘W（应用驻留状态栏） |
| 退出 | ⌘Q 或状态栏菜单 |

**完全磁盘访问**：桌面、文稿、下载等目录受 TCC 保护，需要授予完全磁盘访问才能纳入索引——状态栏菜单 → 「打开完全磁盘访问权限」，授权后按提示重新打开应用。

**搜索示例**：

```
readme              文件名包含 readme
readme md           同时包含 readme 和 md
!test readme        包含 readme 但不含 test
*.log 2024          所有 .log 文件且包含 2024
doc?                doc 后跟任意一个字符
```

## 构建

需要 Xcode 命令行工具（clang + macOS SDK）。

```bash
make          # 构建命令行工具、测试与应用
make test     # 运行单元测试与冒烟测试
make dmg      # 打包 MacEverything.dmg
```

产物在 `build/MacEverything.app` 与 `build/MacEverything.dmg`。

推送到 `main` 会触发 CI 构建校验；推送 `v*` 标签（如 `v1.1.0`）会自动构建并发布 Release。

## 工作原理

- **索引**：树形节点 + 名字 blob + 哈希表；扫描使用 `getattrlistbulk` 多线程遍历，`openat` 相对路径打开规避 TCC 阻塞
- **查询**：NFD 折叠后的大小写/音调不敏感子串匹配，NEON 加速；结果可按名称/路径/大小/时间全量排序
- **持久化**：索引文件 mmap 只读加载，零拷贝查询；修改后原子写盘（临时文件 + rename）
- **更新**：FSEvents 增量维护；索引旁的 `.eid` 旁路文件记录事件位点，重启不丢事件

## License

MIT
