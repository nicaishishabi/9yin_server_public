# 九阴真经 C++ 服务端（框架版）

---

## 1. 资源目录

服务端启动时会自动定位「服务器根目录」，然后按下面结构查找资源。

```
<服务器根目录>/                    ← 可设环境变量 NINEYIN_SERVER_ROOT
├── resources/
│   ├── modern/
│   │   ├── share/                 ← ★ 最核心，原客户端 share 目录内容全部放这里
│   │   │   ├── skill/
│   │   │   │   └── qinggong/
│   │   │   │       └── qgdefine.ini
│   │   │   ├── npc/
│   │   │   │   └── npcconfig/     ← 所有 npcconfig 子目录（worldnpc、clone 等）
│   │   │   ├── creator/
│   │   │   │   └── npc_creator/   ← 各场景 creator（city05_chengdu 等）
│   │   │   ├── rule/
│   │   │   │   └── door_data.ini
│   │   │   ├── trade/
│   │   │   │   └── shop.ini
│   │   │   ├── item/              ← 装备/物品相关
│   │   │   └── ...                ← 其他 share 下原有文件
│   │   ├── ini/
│   │   │   └── action/            ← 动作 ini
│   │   └── text/                  ← 文本资源
│   └── legacy/
│       └── Npc_Func.xml           ← 旧版 NPC 服务菜单表
├── data/                          ← 服务端本地数据（角色 JSON 备份等）
│   ├── roles.json
│   └── faculty.json
├── logs/                          ← 运行日志（自动创建）
├── migrations/                    ← MySQL 迁移 SQL
└── （可执行文件 protocol_probe 放在根或 bin 下均可）
```

### 路径解析规则

1. 若设置了环境变量 `NINEYIN_SERVER_ROOT`，直接使用该路径作为根。
2. 否则从当前工作目录 / 可执行文件目录向上查找，直到找到包含 `resources/modern/share` 的目录。
3. 物品路径额外支持 `NINEYIN_SHARE_ROOT`（若设置，则 item 使用 `$NINEYIN_SHARE_ROOT/item`）。

启动时会打印所有关键路径，并标注 `[OK]` / `[MISSING]`

### 常用默认路径一览（相对服务器根）

| 用途              | 相对路径 |
|-------------------|----------|
| 轻功定义          | `resources/modern/share/skill/qinggong/qgdefine.ini` |
| NPC 模板总目录    | `resources/modern/share/npc/npcconfig` |
| 默认城市 NPC 表   | `resources/modern/share/npc/npcconfig/worldnpc/city_commonnpc.txt` |
| NPC 创建器总目录  | `resources/modern/share/creator/npc_creator` |
| 成都功能 NPC      | `resources/modern/share/creator/npc_creator/city05_chengdu/funcnpc.xml` |
| 门路由            | `resources/modern/share/rule/door_data.ini` |
| 商店              | `resources/modern/share/trade/shop.ini` |
| 技能根            | `resources/modern/share/skill` |
| 物品              | `resources/modern/share/item`（或 `$NINEYIN_SHARE_ROOT/item`） |
| 旧 NPC 功能表     | `resources/legacy/Npc_Func.xml` |
| 角色 JSON         | `data/roles.json` |

---

## 2. 构建

```bash
cd 9yin-cpp-server
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . -j$(nproc)
# 运行测试
ctest --output-on-failure
```

依赖：
- C++20 编译器（GCC 10+ / Clang 12+）
- （可选）MySQL 客户端库（`libmysqlclient` 或 `libmariadb`）——后续 Phase 会启用

---

## 3. 运行

```bash
# 方式一：在服务器根目录下直接运行（自动发现 resources）
./build/protocol_probe --listen 127.0.0.1:19061

# 方式二：显式指定根目录
./build/protocol_probe --root /path/to/your/server/root --listen 0.0.0.0:19061

# 或用环境变量
export NINEYIN_SERVER_ROOT=/path/to/your/server/root
./build/protocol_probe
```

---

## 4. 说明 

# 9yin_sever_c++版本，目前为基础框架版本
# MySQL自行配置，文件根目录已包含数据库迁移表及数据库启动脚本
