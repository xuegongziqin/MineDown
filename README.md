# MineDown

一个用 **Qt 6 / C++20 / GNU g++** 写的 Minecraft Java 版下载器：图形界面、多线程下载、SHA1 校验、断点续传、多镜像切换，并且能装 **Fabric / Forge / NeoForge** 加载器。

做两件事：把游戏文件下到位、把模组加载器装到位（不启动游戏、不下载 JRE）。目标目录结构与官方启动器一致，装完即可被任何启动器识别。

---

## 构建

需要 MSYS2（`D:\msys64`）+ 以下包：

```powershell
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-tools
```

构建（不用 CMake，纯批处理）：

```bat
build.bat            :: Release 构建（-O2），产物 build\MineDown.exe
build.bat fast       :: 调试构建（-O0 -g）
build.bat run        :: 构建后直接启动
build.bat clean      :: 删除 build 目录
```

打包成免安装目录（windeployqt + 递归补全 MinGW 依赖）：

```bat
deploy.bat           :: 产物在 dist\，可直接拷到别的机器运行
```

`build.bat` 会自动发现 `src/` 下所有 `.cpp` 与 `.h`（对所有头跑 moc），新增文件无需改脚本；用 `fc /b` 做内容比对实现增量编译，改一个 `.cpp` 通常 10–20 秒出包。

链接时带 `-s`（strip 符号），并把三类资源编进 exe：

- `res/app.rc` + `res/icon.ico`（由 `icon.png` 转出的 16–256 px 多尺寸 ico）→ 资源管理器/任务栏里的程序图标；
- `res/app.qrc` + `res/icon.png`（缩到 256 px 的副本）→ Qt 窗口图标（`:/icon.png`）；
- `res/app.manifest`（`app.rc` 以 `CREATEPROCESS_MANIFEST_RESOURCE_ID` 引用）→ 进程清单：声明 `asInvoker` 与支持的 Windows 版本，需要管理员时由程序自己申请（见「使用」里的说明）。

当前 Release 产物约 **1.1 MB**（1,164,288 字节）。

---

## 使用

1. 首次启动会强制要求选择**游戏目录**（一般就是 `%APPDATA%\.minecraft`），选择后记入注册表 `HKCU\Software\MineDown\MineDown`（org 与 app 同名，所以路径里有两层 `MineDown`）。
2. 选择镜像源（默认 BMCLAPI），刷新版本列表，左侧选中版本 → **开始下载**（双击版本同样可以）。
3. 下载中可**暂停 / 继续 / 取消**；暂停会把每个在途文件的 `.part` 断点保留，继续时用 HTTP Range 续传。
4. 并发数默认 8，可在 1–32 之间调整（并发过高容易被镜像限流，反而更慢）。
5. 单文件也吃不满带宽：≥ 8 MiB 的文件（`client.jar`、大音效等）会切成最多 4 段并行下载，实测比单连接快一个数量级。

已经存在的文件会先比大小、再算 SHA1，校验通过就跳过 —— 因此重复下载同一版本、或下载共享同一套资源的其它版本，都只会补缺失部分。

**关于管理员权限**：程序默认按普通权限启动（清单里声明 `asInvoker`，不会每次弹 UAC）。当游戏目录里有当前账户动不了的文件时（典型症状是日志里出现「拒绝访问」「目标文件无法替换」「无法把 `.part` 重命名为目标文件」），失败弹窗里会出现 **「以管理员身份重试」** 按钮：点它，程序用 UAC 重新启动自己并自动重试同一版本，窗口标题会带上「（管理员）」。装加载器写不进 `libraries/` 时同样有这个按钮。

如果某个目录是**连管理员都读不了**的（从旧系统拷来的文件带着别的账户的 SID），提权只能解决「写」，读不了的文件还是校验不过 —— 这种情况按「常见问题：报拒绝访问」里的 `takeown` / `icacls` 修一次权限。

命令行参数（除 `--selftest` 外也适合脚本化操作）：

| 参数 | 说明 |
| --- | --- |
| `--root <dir>` | 直接指定游戏目录（跳过首次向导） |
| `--mirror <key>` | `official` / `bmclapi` / `hkmc` / `hkmc-eo` / `custom` |
| `--auto <版本>` | 启动后自动开始下载指定版本 |
| `--custom-base <url>` | 把镜像切到 `custom` 并指向这个根地址（本地 mock 源用） |
| `--install-loader --loader <fabric\|forge\|neoforge> --mc <原版> [--loader-version <版本>] --log <文件>` | 无界面安装加载器（不指定加载器版本时取第一个稳定版） |
| `--selftest --version <版本> --log <文件>` | 无界面跑完整流程，日志写文件，退出码 0 成功 |
| `--shot <png> [--shot-delay <ms>]` | 抓主窗口截图（开发用） |
| `--loader-shot <png> [--loader-kind <key>]` | 抓加载器对话框截图（开发用） |
| `--elevate` | 用 UAC 重新以管理员身份启动（等价于右键「以管理员身份运行」） |

---

## 模组加载器安装

主界面右上角 **安装加载器…**：选加载器（Fabric / Forge / NeoForge）→ 选原版版本（默认挑本地已装的最新正式版）→ 选加载器版本 → 开始安装。装出来的版本会出现在左侧列表最上方，标「已安装」。

三条实现路线：

| 加载器 | 做法 | 需要 Java |
| --- | --- | --- |
| Fabric | Meta API 给的库 + `mainClass` 直接合成版本 JSON | 否 |
| Forge 1.13+ / NeoForge | 下官方安装器 → 解压 → 读 `install_profile.json` → 下安装期依赖 → **顺序执行 processors**（`java -cp`）→ 写版本 JSON → 补启动期库 | 是（自动查找） |
| Forge ≤ 1.12.2 | 老安装器没有 processors，直接把压缩包里的 `maven/**` 解到 `libraries/`，再写版本 JSON | 否 |

关键细节（都在 `src/core/LoaderInstaller.cpp` 里）：

- processor 的主类从 jar 的 `META-INF/MANIFEST.MF` 读 `Main-Class`，不靠猜；`sides` 不含 `client` 的跳过；**严格串行**（并行会把中间产物写坏）。
- 参数里除了 `{KEY}` 数据键，还有内置占位符 `{SIDE}` `{MINECRAFT_JAR}` `{ROOT}` `{INSTALLER}` `{LIBRARY_DIR}` `{MINECRAFT_VERSION}`；`[group:artifact:version@ext]` 形式的坐标会先换成 `libraries/` 下的本地路径（否则 processor 直接 `ClassNotFoundException`）。
- 版本列表：Forge / NeoForge 读 `maven-metadata.xml`（NeoForge 官方 JSON API 在本机网络不可达，改读 maven 元数据），Fabric 读 `meta.fabricmc.net`（该接口没有镜像，只有库走镜像）。
- 加载器 Maven（`maven.minecraftforge.net` / `maven.neoforged.net` / `maven.fabricmc.net`）在镜像上统一映射成 `<镜像>/maven/<路径>`；第三方仓库仍然不替换。
- **只写 `versions/<加载器版本>/` 与 `libraries/`，绝不改原版目录**；如果原版还没下载，装完会提示先下原版。
- 安装器的临时解压目录是 `<游戏目录>/.minedown/loader/<id>`，装完即删。

---

## 落盘规则

全部路径相对所选游戏目录：

| 内容 | 路径 |
| --- | --- |
| 版本清单缓存 | `%LOCALAPPDATA%\MineDown\version_manifest_v2.json` |
| 版本 JSON | `versions\<id>\<id>.json` |
| 客户端 jar | `versions\<id>\<id>.jar` |
| 依赖库 | `libraries\<Maven 路径>` |
| 加载器版本 | `versions\<加载器版本>\<加载器版本>.json`（`inheritsFrom` 原版，自带 `libraries`） |
| 安装器临时目录 | `.minedown\loader\<id>`（安装结束删除） |
| 资源索引 | `assets\indexes\<assetIndex.id>.json` |
| 资源对象 | `assets\objects\<hash 前两位>\<完整 hash>`（无扩展名） |
| 日志配置 | `assets\log_configs\<id>` |

`libraries` 会按 `rules[]` 做操作系统/架构过滤（Windows x64），老版本通过 `natives` + `classifiers` 选 `natives-windows` 包，新版本 natives 已经是普通 artifact。

---

## 镜像地址规则（实测）

镜像**不是**简单地换域名 —— 不同镜像把官方路径映射到了自己的前缀上，所以按「资源类型」重建路径：

| 资源 | 官方地址 | BMCLAPI | MC 光速（HKMC） |
| --- | --- | --- | --- |
| 版本清单/版本 JSON/资源索引 | `piston-meta.mojang.com/v1/packages/…` | `bmclapi2.bangbang93.com/v1/packages/…` | `…/v1/packages/…` |
| 客户端 jar / 日志配置 | `piston-data.mojang.com/v1/objects/…` | `bmclapi2.bangbang93.com/v1/objects/…` | `…/v1/objects/…` |
| 依赖库 | `libraries.minecraft.net/<maven>` | `bmclapi2.bangbang93.com/libraries/<maven>` | `…/maven/<maven>` |
| 资源对象 | `resources.download.minecraft.net/<2>/<hash>` | `bmclapi2.bangbang93.com/assets/<2>/<hash>` | `…/assets/<2>/<hash>` |

`libraries[]` 里指向第三方 Maven 仓库的 `url` 不在替换表内，原样保留（否则会 404）。

镜像覆盖面小于官方源：例如 BMCLAPI 就缺少 `client-1.21.2.xml` 这类日志配置。因此每个地址都带一个**官方源回退**——镜像返回非 2xx 或 TLS/连接失败时，自动换官方地址重来一次，只有官方也拿不到才算失败。

TLS 后端优先使用 Windows 自带的 **SChannel**：部分镜像节点不发送中间证书，OpenSSL 会报 `issuer certificate ... could not be found`，SChannel 能自动补全（见 `src/main.cpp`）。

被限流时（HTTP 429）会按 `Retry-After` 或 5 s × 次数退避，而不是用 1 s/2 s/4 s 的小退避把限流拖得更久。

---

## 分段并行下载

单连接的吞吐被网络压得很低（实测官方源单流约 0.04 MB/s，4 段 0.59 MB/s，8 段 1.04 MB/s），所以大文件走分块：

- 触发条件：文件大小 ≥ 8 MiB、切段数 `size / 4 MiB`（上限 4 段）、且这是本文件的**首次**尝试。
- 每段一条 `Range: bytes=start-end` 请求，写入同一个预分配好的 `.part`（按段内偏移 `seek` 后写，各段互不重叠）。
- 段级失败只重试该段（从该段已写入的位置继续）；整份文件失败后回退单连接重试；服务器忽略 `Range`（回 200）时立刻改回单连接。
- 暂停会丢弃分段的 `.part` 重新下——分段进度不像单流那样有连续前缀，无法直接续传；单连接路径仍保留完整断点续传。
- 分块思路借鉴了 WinHTTP 多线程分块下载的经典做法，但这里是在 Qt 引擎内重写的：分段走 `Range` + 共享文件句柄按偏移写入，并带着取消、SHA1 校验、重定向与镜像回退。

---

## 测试（本地 mock 源）

`tests/mock_test.py` 是一个只用标准库的 Python 脚本：它自己造一套自洽的 Minecraft 目录（24 MiB 的 `client.jar`、12/9/6 MiB 大资源、120 个小资源、带 `natives-windows` 分类器的依赖库、资源索引、版本清单），起一个本地 HTTP 源，然后驱动 `--selftest` 跑七个场景：

```bat
python tests\mock_test.py             :: 跑全部场景
python tests\mock_test.py chunk drop  :: 只跑指定场景
python tests\mock_test.py --serve 18080 700000   :: 手动起 mock 源（端口、每连接限速字节/秒）
```

| 场景 | 验证点 |
| --- | --- |
| `chunk` | 24 MiB 的 jar 分成 4 段并行，落盘完整 |
| `skip` | 二次运行全部命中「已存在 + SHA1 正确」，秒级完成 |
| `resume` | 手工造半个 `.part`，续传后 SHA1 正确 |
| `ratelimit` | 连续注入 24 次 `429`，退避后仍然全绿 |
| `corrupt` | 服务端在响应里翻坏一个字节，必须报「校验失败」且不落盘 |
| `drop` | 服务端写一半就断连，客户端能重连续上完整文件 |
| `perm` | 目录被禁止写入时立刻报「拒绝访问/无法替换」，不空转重试，已校验的 `.part` 保留 |

配合 `--custom-base http://127.0.0.1:18080 --auto 1.0-test` 也能让 GUI 连到 mock 源，做界面/进度的人工验证。

`--root` / `--mirror` / `--custom-base` 这几个参数只在**本次运行内**生效，不会写进注册表，所以跑测试不会把平时的镜像设置改成 mock 源。

---

## 代码结构

```
src/
  main.cpp                 入口、命令行参数、TLS 后端选择
  core/
    Types.h/.cpp           任务、结果、统计等基础类型
    Mirror.h/.cpp          官方/镜像地址映射与回退
    Sha1.h/.cpp            SHA1 计算与文件校验
    Settings.h/.cpp        QSettings 持久化（目录、镜像、并发数…）
    HttpFetcher.h/.cpp     小 JSON 异步抓取（清单/版本 JSON/资源索引）
    DownloadWorker.h/.cpp  单个下载线程：Range 续传、SHA1、重试、回退
    DownloadManager.h/.cpp 线程池调度、队列、统计、暂停/继续/取消
    ManifestParser.*       版本清单解析
    VersionParser.*        版本 JSON 解析（含 rules、natives、Maven 路径）
    RulesEval.*            rules[] 求值
    AssetIndexParser.*     资源索引解析与去重
    InstallPipeline.*      阶段编排：版本 JSON → 资源索引 → 客户端 → 依赖库 → 资源
    MavenUtil.h            Maven 坐标解析（[group:artifact:version@ext] ↔ 本地路径）
    ZipReader.h/.cpp       ZIP 读取（zlib inflate，校验 CRC32，拒绝 zip-slip）
    JavaLocator.h/.cpp     扫描/选择本机 Java（processor 要用）
    LoaderCatalog.h/.cpp   加载器版本列表（Forge/NeoForge 的 maven-metadata、Fabric Meta）
    LoaderInstaller.h/.cpp 加载器安装编排（Fabric 合成 JSON / Forge·NeoForge 跑 processors / 旧版解压）
    Elevation.h/.cpp       管理员检测与 UAC 提权重启（ShellExecuteExW "runas"）
  ui/
    MainWindow.*           主窗口
    LoaderDialog.*         安装加载器对话框
    SetupDialog.*          首次启动选目录
    VersionListModel.*     版本列表模型（搜索、快照过滤、本地已装版本）
    FileTableModel.*       文件明细模型
  dev/
    SelfTest.*             无界面端到端自检（--selftest）
    LoaderTest.*           无界面加载器安装自检（--install-loader）
tests/
  mock_test.py             本地 mock 源 + 七个回归场景（分块/跳过/续传/限流/坏数据/断流/权限）
res/
  app.rc / icon.ico        exe 图标（windres 编译）
  app.manifest             UAC 级别（asInvoker）与支持的 Windows 版本
  app.qrc / icon.png       窗口图标（rcc 编译，:/icon.png）
```

并发模型：K 个常驻 `QThread`，**每个线程独占一个 `QNetworkAccessManager`**（连接复用 + keep-alive，避免几千个资源对象各自重做 TLS 握手）；任务在主线程用 `std::deque` 排队，按 FIFO 派发给空闲线程；进度信号跨线程走 `Qt::QueuedConnection`，100 ms 节流。

---

## 常见问题：报「拒绝访问」/ 目标文件无法替换

现象：某个文件反复失败，日志里出现「目标文件已存在且无法删除」「拒绝访问」（旧版本里
表现为「无法把 `xxx.part` 重命名为目标文件」），而且换个版本、换成官方源都一样。

**先试提权**：失败弹窗里点「以管理员身份重试」（或用 `MineDown.exe --elevate` /
右键「以管理员身份运行」）。绝大多数的 .minecraft 只是需要管理员往 `libraries/`
里补文件，提权后就能装完；已通过 SHA1 的 `.part` 会直接复用。

如果提权后仍然报「拒绝访问」，原因就在游戏目录本身：从旧系统、旧硬盘或备份整包拷来的
`.minecraft` 会把原来的 NTFS 安全描述符一起带过来，那批文件属于**另一个账户的 SID**，
当前用户既没有读权限（所以 SHA1 永远校验不过），也没有删除/改名权限（所以 `.part`
落不了盘）。这类文件的 `LastWriteTime` 往往比其它文件早很多，`icacls <文件>` 会直接报
「拒绝访问」。

修复：用**管理员**身份跑一次下面这段（和具体盘符、用户名、系统语言都无关）。

先打开管理员命令行：开始菜单输入 `cmd` → 右键「以管理员身份运行」；或者把下面的内容
存成 `fix.bat`（记事本另存为时编码选 **ANSI**），右键「以管理员身份运行」。

```bat
rem 1) find the game dir: MineDown's own setting first, vanilla default as fallback
for /f "tokens=2,*" %%A in ('reg query "HKCU\Software\MineDown\MineDown\game" /v rootDir 2^>nul ^| find "rootDir"') do set "MC=%%B"
if not defined MC set "MC=%APPDATA%\.minecraft"
set "MC=%MC:/=\%"
echo [INFO] game dir = "%MC%"

rem 2) if neither of those is your game dir, set it yourself (see the app's "game dir" field)
rem set "MC=D:\Somewhere\.minecraft"

rem 3) take ownership, then grant BUILTIN\Users the Modify right (same as a fresh folder)
takeown /f "%MC%" /r /d y
icacls "%MC%" /grant *S-1-5-32-545:(OI)(CI)M /t /c
```

- 代码块故意全用英文：cmd 按本地代码页逐行读 `.bat`，UTF-8 的中文注释在 GBK 控制台上会被
  截断，连带把下一行命令读坏（`'efined' 不是内部或外部命令`）。中文说明都放在代码块外面。
- `takeown` 把目录树里每个文件的所有者改成 Administrators，`icacls` 再放开写权限；
  `*S-1-5-32-545` 是 `BUILTIN\Users` 的 SID，用它就不受系统语言影响（中文「Users」、
  德文「Benutzer」都一样），也不用先知道当前用户名。
- 直接粘进 cmd 窗口而不是存成 .bat 时，把 `%%A`、`%%B` 改成 `%A`、`%B`（批处理里才需要双写）。
- 跑之前最好先用同一个管理员窗口 `dir "%MC%"` 确认路径对；两个命令都会打印
  `已处理 N 个文件`，失败数为 0 就说明修好了。
- 如果 `icacls` 仍有个别文件报「拒绝访问」（常见于目录本身被移动过、带了互相冲突的
  显式 ACE），补一条 `icacls "%MC%" /reset /t /c` 让它重新继承父目录权限，再重跑第 3 步。

修完重新点「开始下载」即可：已下载并通过 SHA1 的 `.part` 会被保留复用，不会整份重下。
也可以直接用失败弹窗里的「以管理员身份重试」——它会带着当前版本自动重跑。

---

## 已知限制

- 仅 Windows x64。
- 不支持全局限速。
- 不下载 JRE（只读取 `javaVersion` 并提示）；加载器安装需要本机已有 Java：1.17+ 用 17，1.20.5+ 用 21，26.x（新版本号体系）用 25。
- 不支持 OptiFine 自动安装（它没有标准清单，得靠 tweaker 注入）；Forge/Fabric/NeoForge 之外的加载器都不支持。
- 自定义镜像按「只换域名、路径不变」处理；如果对方是 BMCLAPI 风格的前缀布局，资源目录会走官方回退。
- BMCLAPI 要求署名：界面右下角常驻「下载加速由 BMCLAPI 提供」。

---

## 许可证

本项目以 **GNU Lesser General Public License v3.0** 发布（`LGPL-3.0`）：

- [`LICENSE`](LICENSE) 是 LGPL-3.0 的完整文本；
- [`LICENSE.GPL-3.0`](LICENSE.GPL-3.0) 是它引用的 GNU GPL v3 文本（LGPL-3.0 第 3、4 条要求随附）。

也就是：你可以自由使用、修改、再分发本程序（包括商业用途），但要保留版权声明与许可证、修改过的源码要公开；把它作为库被别人链接时，对方程序不必开源。

运行时依赖：Qt 6（LGPL-3.0，动态链接，`dist\` 里的 `Qt6*.dll` 就是这个）、zlib（zlib 许可证）、MinGW 运行库（GPL 例外条款）。`maven.minecraftforge.net` / `maven.neoforged.net` / `maven.fabricmc.net` 上的加载器安装器与库遵循各自的许可证，下载后归各自作者所有。

`GUIDE/` 目录是编写过程中参考的第三方启动器源码（PCL），**不属于本项目**，已在 `.gitignore` 里排除、不随仓库分发。
