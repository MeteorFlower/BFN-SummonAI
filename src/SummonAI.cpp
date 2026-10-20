// ============================================================================
//  SummonAI.dll，BFN 按需召唤 AI 队友（自造路线）
// ----------------------------------------------------------------------------
//  自造 = 把任意角色的模板注入到一个 AI spawner 里再生成，不依赖"关卡里恰好
//  有这个角色的 spawner"：
//      ScanTemplates        扫资产区，枚举所有已加载的 Template 实例（= 所有角色）
//      BuildSummonLists     按阵营分两组（队友/敌人），每组切成 角色，档位 两级
//      SummonWithTemplate   注入模板，调生成函数，全部还原
//  召唤哪个角色由 HUD 的选中项决定。
//
//  调用的是 sub_141D6CFD0(ctx, req) = ctx 的虚方法 vt[232]（AI / 玩家两条路共用，
//  不是"玩家的生成函数"）。req 优先照抄 AI 路径抓来的那一份，抓不到才自己造。
//
//  -- 四条硬规矩（每条在对应代码处都有更详细的注释，这里只给禁忌本身）--
//    1. 不要调 sub_141D6D9B0（玩家出生那条路）：它会让 ctx 把这个 player
//       重新出生，先销毁玩家实体。
//    2. 不要用清零的假 holder（holder 装配处）：ctx 的几个虚方法要读 holder 的
//       +56/+92，而 +64/+80 是 Frostbite 数组，游戏读的是 len = *(u32*)(指针-4)。
//       指针为 0 就是访问违例，而且会被我们自己的 __try 吞掉，外表只是"按键没反应"。
//    3. 不要拿玩家自己的 spawner 当注入载体（ScanPool / ValidCarrier）：玩家会
//       销毁重生成、人直接消失。
//    4. 召唤必须在游戏线程上做（DispatchDetour）：引擎的 TLS 上下文只有游戏线程有，
//       按键线程只置一个待办标志。
//
//  其余细节（req 字段表、sub_140F4EF30 的两道闸门、ctx+840 的偏移规律、血量数组、
//  载体来源优先级、ctx 工厂）都写在 SpawnDetour / SummonWithTemplate /
//  GetAdDescriptor / SpawnHealthOf 的注释里。
//
//  -- 热键（四列 HUD）--
//    第1列 队友角色 F1 / F2     第2列 队友档位 F3 / F4      F5 = 召唤队友
//    第3列 敌人角色 F7 / F8     第4列 敌人档位 F9 / F10     F11 = 召唤敌人
//    F6 = 显隐 HUD
//    F12 = Steam 截图键，永远不占
//
//  日志在 DLL 同目录的 summonai_log.txt
// ============================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <intrin.h>  // _ReturnAddress()，记下"谁调用了这个函数"，静态分析查不出来的东西

// MinHook，x64 指令长度解码 + 跳板重定位。vendored 在 third_party/minhook (BSD-2-Clause)。
#include "third_party/minhook/include/MinHook.h"

// ---------------------------------------------------------------------------
//  日志（Worker / 热键线程 / HUD 线程都会用，必须加锁）
// ---------------------------------------------------------------------------
static FILE *g_log = nullptr;
static std::string g_dir;
static CRITICAL_SECTION g_log_cs;
static volatile LONG g_log_ok = 0;

static void LogInit() {
  InitializeCriticalSection(&g_log_cs);
  InterlockedExchange(&g_log_ok, 1);
}

static void Log(const char *fmt, ...) {
  char body[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(body, sizeof(body), fmt, ap);
  va_end(ap);

  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[1200];
  _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d] %s", st.wHour, st.wMinute,
              st.wSecond, body);

  // g_log_ok 只读这一次。读两遍的话，两次之间 Worker 的 LogInit 会把它从 0 改成 1，
  //   于是出现"进的时候没加锁、出的时候去放锁"：那把锁的记账就此损坏，
  //   要么两个线程同时往日志文件里写，要么它被留成永远被占、游戏线程的 Log 死等。
  const bool locked = g_log_ok;
  if (locked)
    EnterCriticalSection(&g_log_cs);
  if (g_log) {
    fputs(line, g_log);
    fputc('\n', g_log);
    fflush(g_log);  // 每行都刷，就算崩了也能拿到崩溃前的内容
  }
  if (locked)
    LeaveCriticalSection(&g_log_cs);
}

// 开发期才打的日志（模板全扫逐条、列表分组逐条、载体 dump、探针进度心跳），
//   发布构建里编译掉，只有 build_debug.bat 加的 /D_DEV 才编进去。
#ifdef _DEV
#define LOG_DEV(...) Log(__VA_ARGS__)
#else
#define LOG_DEV(...) ((void)0)
#endif

// ---------------------------------------------------------------------------
//  SEH 保护的内存读，我们读的全是游戏内存，任何一个指针都可能是坏的
// ---------------------------------------------------------------------------
static bool Rd32(unsigned long long a, unsigned int *o) {
  __try {
    *o = *(volatile unsigned int *)a;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}
static bool Rd64(unsigned long long a, unsigned long long *o) {
  __try {
    *o = *(volatile unsigned long long *)a;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// 有界字符串拷贝：_snprintf_s 的 _TRUNCATE 加 "%s" 那一套，全项目只写这一个名字。
//   它顺带消掉一类错：这样复制出来的串永远不会被当成格式串用。
static void CopyStr(char *dst, size_t n, const char *src) {
  _snprintf_s(dst, n, _TRUNCATE, "%s", src);
}

// 原子读一个会变的量：借一个"期望值恒不等于现值"的 CAS 把它读出来（CAS 必然失败，返回现值）。
//   名字里必须带 Atomic，读的人才知道它不是普通读取。
//   只用来读，不许顺手拿它做"读到就清"那种事（那是另一个操作，别混进来）。
static LONG AtomicReadL(volatile LONG *p) {
  return (LONG)InterlockedCompareExchange(p, 0, 0);
}
static LONG64 AtomicReadL64(volatile LONG64 *p) {
  return (LONG64)InterlockedCompareExchange64(p, 0, 0);
}

// 最小合法地址：低于它的都是 0 / 小整数，不是指针。
constexpr unsigned long long MIN_VALID_ADDR = 0x10000ull;

// 判断这个指针是不是一个可用的堆对象（排除 0 / 小整数 / 指向模块映像的静态数据）。
static bool HeapPtr(unsigned long long p) {
  if (p < MIN_VALID_ADDR || (p & 7))
    return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (VirtualQuery((LPCVOID)p, &mbi, sizeof(mbi)) != sizeof(mbi))
    return false;
  if (mbi.State != MEM_COMMIT)
    return false;
  return mbi.Type != MEM_IMAGE;
}

// ===========================================================================
//  已验证的游戏地址与偏移（无 ASLR，解密镜像里的地址 == 运行时地址）
// ===========================================================================
// 玩家 slot 表在 ADDR_MANAGER_GLOBAL 的 +176 是 ServerPlayerManager，
//   那个对象的 +16 是表的总项数（运行期从配置读，本作实测 70）。
//   槽表本身是构造时按这个数一次性分配的，所以这个值就是 ctx+244 那个索引的轮换范围。
constexpr unsigned long long ADDR_MANAGER_GLOBAL = 0x1445C4410;  // 全局表；+176 是 manager 的间接层
constexpr int MANAGER_OFF = 176;
constexpr int PLAYER_ARR_OFF = 1784;  // manager，玩家数组
constexpr int MANAGER_SLOTS_OFF =
    16;  // manager，slot 表总项数（运行期从配置读，见 ADDR_MANAGER_GLOBAL）
constexpr int MANAGER_BOTCAP_OFF = 1768;  // manager，其中留给 bot 的上限（回收线 = 前者减它）
constexpr int PL_ISAI_OFF = 72;           // player，是不是 AI（低字节非 0 = AI）
constexpr int PL_ID88_OFF = 88;           // player，队号（1=僵尸 2=植物）
// 载体按队分槽的数组长度。队号只有 1（僵尸）与 2（植物），下标 0 永远不用。
constexpr int CARRIER_TEAMS = 3;
// 池子/数组遍历的防御上限：只用来挡坏数据（真数据远小于它），几处口径必须一致。
constexpr int POOL_DEF_MAX = 512;
// loc ID（TemplateLocId 读出来的那种串）缓冲大小。
constexpr int LOC_ID_MAX = 64;
constexpr int DUMP_MAX_SLOTS = 96;  // 玩家数组最多扫这么多槽

// ===========================================================================
//  召唤列表（第二版 HUD 的数据模型）
//
//  列表全部来自 ScanTemplates 扫出来的模板实例，所以能跟着关卡流式加载变化；
//     "每个角色有哪些档位"由数据自己说，不需要维护一张职业表，那种表只有 23 个
//     "职业名"，既不含各档位（小兵/Champion/精英），也跟不上关卡。
//     （显示名来自 unit_names.inc，用 Frosty 从游戏本地化数据导出后重建；
//       要改名字就改数据重新生成这张表。）
//
//  分组规则（实测对所有样本成立）：
//    短名    = 模板路径剥掉目录 + Template_Plant_/Template_Zombie_ 前缀 + 紧跟的 AI
//    角色 key = 短名第一个 _ 之前的部分
//    档位标签 = 第一个 _ 之后的部分（没有 _，就一个"基础"档）
//
//    比如 Template_Plant_AIPeaShooter_BattleLeader_Elite
// 剥目录/前缀/AI，PeaShooter_BattleLeader_Elite
// key = PeaShooter，档位 = BattleLeader_Elite
//    先剥前缀再找第一个 _，这样切出来的 key 才是角色那一段
//       （路径里第一个 _ 在 Template_ 里面）。
//
//  列表是两级结构。
//    角色列列的是角色 key（PeaShooter），不是每个变体一行；
//    _BattleLeader_Elite / _Champion 这些是档位，只出现在各自的档位列。
//    这样角色列才短，而且按 F1/F2 换角色时不会被同一个角色的十几个变体淹掉。
// ===========================================================================
constexpr int SUM_NAME_MAX = 112;  // 名字缓冲（Template_* 最长约 60，留足余量）
constexpr int SUM_LIST_MAX =
    320;  // 每阵营最多收这么多档位（早期一批日志里约 294 个模板名，两阵营各半）
// 每阵营最多收这么多角色。容量按模板名实际切分来定：模板名里有一堆像
// _Ops_Pumpkin_Champion / _YODEL_Champion / _RareMob_Elite 这样的，
// 每个都会切出自己的 key，角色数远多于 23 个职业。宁大勿小（真溢出了日志会显示条数不对）。
constexpr int SUM_CHAR_MAX = 320;

// 一个档位= 一个模板实例
struct TierEntry {
  unsigned long long v21;    // 模板实例（= 注入用的目标）
  char label[SUM_NAME_MAX];  // 档位标签（"第一个 _ 之后"；无 _，"(base)"）
};

// 一个角色= 一组相邻档位。左/右列显示的就是它。
struct CharEntry {
  char key[SUM_NAME_MAX];  // 角色显示名（优先游戏内显示名，查不到才用剥出来的模板代码名）
  int lo, hi;              // 在所属档位表里的区间 [lo, hi)
};

// 左列 = 玩家阵营（队友），右列 = 敌方阵营。两边在 BuildSummonLists 里按 g_team 分好。
//   只发布角色数（g_allyN/g_enemyN）：档位区间就在 CharEntry::lo/hi 里，读方按需取。
static TierEntry g_allyTiers[SUM_LIST_MAX];
static CharEntry g_ally[SUM_CHAR_MAX];
static volatile LONG g_allyN = 0;
static TierEntry g_enemyTiers[SUM_LIST_MAX];
static CharEntry g_enemy[SUM_CHAR_MAX];
static volatile LONG g_enemyN = 0;

// "这次没建成，需要重试"标志（BuildSummonLists 被并发闸门挡掉、或队号还没到时置上）。
//   没有它就会漏掉一次重建，换关时 ResetInjectState 会先把 g_tplN 清 0、再让 ScanTemplates
//      重扫；若此刻按键线程正占着 BuildSummonLists，Worker 扫完后那次调用会被闸门挡掉，
//      列表就停在一份用"扫到一半的模板表"建的旧数据上，而 RefreshTeam 只在换队时才重建，
//      可能一直错到换局。置上这个标志，Worker 每 50ms 的周期里补建一次。
static volatile LONG g_listWant = 0;

// 列表版本号（seqlock）在 BuildSummonLists 开工前 +1、收工后再 +1，奇数表示"正在重建"。
//   读方（ResolveSelection）读前后各取一次，不相等或为奇数就重读。
//   重建是就地 memcpy 覆盖那几个固定数组的，"先发布角色数"只能保证条数一致，
//   挡不住"读了一半"，读方先用旧的 [lo,hi) 定区间、再去读档位表，中间若正好
//   重建完成，就会用旧区间索引新内容，取到另一个角色/档位的 v21。
//   后果不是崩溃（数组都是定长 320），而是召出来一个和屏幕高亮不符的角色。
static volatile LONG g_listGen = 0;

// 选中状态，四列 = 四个光标（每边各自一个角色光标 + 一个档位光标）。
//
//   每边各配一个角色列和一个档位列（为什么不能合成一列，见下面 HUD 布局那段）。
// 四个光标分别是 F1/F2 队友角色、F3/F4 队友档位、F7/F8 敌人角色、F9/F10 敌人档位。
static volatile LONG g_selAlly = 0;       // 第 1 列：队友角色下标
static volatile LONG g_selTierAlly = 0;   // 第 2 列：队友档位下标（相对该角色区间）
static volatile LONG g_selEnemy = 0;      // 第 3 列：敌人角色下标
static volatile LONG g_selTierEnemy = 0;  // 第 4 列：敌人档位下标

// ===========================================================================
//  运行时状态（全局，HUD 和热键线程共读）
// ===========================================================================
// 三个线程循环的开关。这里没有任何地方会把它清 0：DllMain 也没有 DLL_PROCESS_DETACH
//   分支，这个 DLL 本来就不卸载。留着它只是让线程循环读起来是常规写法，
//   三个循环后面那些 return 0 实际到不了。
static volatile LONG g_running = 1;
static volatile LONG g_team = 0;       // 本地玩家队号（0 = 还没读到）1=僵尸 2=植物
static volatile LONG g_hud_dirty = 1;  // 需要重画
// HUD 是否可见。F6 显隐。
static volatile LONG g_hud_show = 1;
static volatile LONG g_last_ok = 0;  // 上次召唤结果：0=没试过 1=成功 2=失败
static char g_last_msg[128] = {0};   // 上次召唤的结果说明
static DWORD g_last_t = 0;           // 上次召唤的时刻（几秒后不显示）
// 召唤重试：按一次键那一发算第 1 次，失败就等一会儿自己再发一次，最多 SUM_RETRY_MAX 次。
//   中间那几次失败只进日志、不写 HUD：刚换关的窗口里第一次按常会失败，那本来就不是
//   该看的错误，闪一行红字反而像是功能坏了。只有最后一次也失败才写 HUD。
//   次数定 3：按一次最多等两次重试，每次隔 0.5 秒，所以从按键到出结果通常约 1 秒
//   （重试到点由 Worker 的轮询发现，它那一轮要是正在扫模板表，会再多等最多 0.8 秒）。
//   再拖长的话，等结果的这段时间里会以为功能坏了、开始猛按。
constexpr int SUM_RETRY_MAX = 3;
constexpr int SUM_RETRY_DELAY_MS = 500;
static volatile LONG g_retryLeft = 0;  // 还允许试几次（含正在跑的那次），0 = 没在重试
static volatile LONG g_retryAt = 0;    // 下一次重试的时刻，0 = 没有待办
static volatile LONG g_retrySide = 0;  // 重试哪一侧
// 上面三个量属于"哪一次按键"，就是这几个序列号。每按一次召唤键加一，那一发的重试沿用它。
//   键盘线程、Worker、游戏线程都会碰重试状态，而一次召唤从按键到结果回来要几十毫秒以上，
//   这段时间够再按一下：不加序列号的话，前一次的失败会去改后一次按键的预算与待办
//   （把后一次的预算吃掉一格，或者在前一次已经作废之后还排一个重试出来）。
static volatile LONG g_retrySeq = 0;   // 当前是哪一次按键（重试状态的主人）
static volatile LONG g_queuedSeq = 0;  // 排队中那一发属于哪个序列号，随载荷一起发布
// 上次的"成功"是不是弱成功，只有 ctx+244 递增（生成函数返回 0）时置 1。
//   那种情况下世界里确实多了一个单位，但不保证是被请求的那个角色：
//   ATT_ASIS 那一档退回载体 holder 时会造出载体的普通单位。
//   不区分的话 HUD 会写"OK 豌豆射手"而出来的是别的东西。
static volatile LONG g_lastWeakOk = 0;

static const char *TeamName() {
  LONG t = g_team;
  if (t == 1)
    return "Zombies";
  if (t == 2)
    return "Plants";
  return "Unassigned";
}

// -- 召唤列表的读取辅助 ----------------------------------------------------
// side: 1 = 队友（左列）  2 = 敌人（右列）
// 一列的 {角色表, 角色数} 用 side 一次性取出来，不要分两次：
//   换队会让两边整体对调，读表与读队号若分开做，中间一旦换队就会拆出
//   "表是 A 边、条数是 B 边"的组合，下标越界。
static CharEntry *CharsOfSide(int side, int *nOut) {
  if (side == 1) {
    if (nOut)
      *nOut = (int)AtomicReadL(&g_allyN);
    return g_ally;
  }
  if (nOut)
    *nOut = (int)AtomicReadL(&g_enemyN);
  return g_enemy;
}
static TierEntry *TiersOfSide(int side) {
  return side == 1 ? g_allyTiers : g_enemyTiers;
}
static volatile LONG *SelOfSide(int side) {
  return side == 1 ? &g_selAlly : &g_selEnemy;
}
static volatile LONG *TierOfSide(int side) {
  return side == 1 ? &g_selTierAlly : &g_selTierEnemy;
}

// 短名的"角色 key 长度" = 第一个 '_' 之前的部分。没有 '_'，整串都是 key。
static int KeyLenOf(const char *shortName) {
  const char *u = strchr(shortName, '_');
  return u ? (int)(u - shortName) : (int)strlen(shortName);
}

// 取某列当前选中角色的档位区间 [lo, hi) 与其在列表里的下标。
//   角色表建好后区间就是现成的（CharEntry::lo/hi），这里只做下标合法性夹取。
static bool TierRangeOf(int side, int *loOut, int *hiOut, int *selOut) {
  int n = 0;
  CharEntry *chars = CharsOfSide(side, &n);
  if (!chars || n <= 0)
    return false;
  int sel = (int)InterlockedCompareExchange(SelOfSide(side), 0, 0);
  if (sel < 0 || sel >= n)
    sel = 0;
  if (selOut)
    *selOut = sel;
  if (loOut)
    *loOut = chars[sel].lo;
  if (hiOut)
    *hiOut = chars[sel].hi;
  return true;
}

// 列表的 seqlock 读法，档位列与召唤路径共用一套（两边各留一份的话，改一边忘一边就分家了）。
//   读前后各取一次 g_listGen，不相等、或读到奇数（= 正在重建）就重读。
//   机制与撕裂的后果见 g_listGen 的说明。
//   调用方把"读一次"包成 once 传进来，成功后由它自己往外写；
//   once 返回 false 表示"这一列真的没有角色"，原样透传给调用方。
template <typename OnceFn>
static bool ReadListSeqlock(OnceFn once) {
  for (int attempt = 0; attempt < 64; ++attempt) {
    const LONG g1 = (LONG)AtomicReadL(&g_listGen);
    if (g1 & 1) {
      // 正在重建：让出一下再来看，别在这儿空转烧 CPU。
      //   重试盖不住重建那几十到上百毫秒，多数情况最后落到下面那次兜底的读。
      SwitchToThread();
      continue;
    }
    if (!once())
      return false;
    if ((LONG)AtomicReadL(&g_listGen) == g1)
      return true;  // 这一趟没被重建打断，数据自洽
    SwitchToThread();
  }
  // 重建一直没结束（撞满全部重试），退回"读一次就用"，不要返回失败：
  //   万一版本号因为任何原因卡在奇数，返回失败就成了"召唤永远失败"这种最难受的故障。
  //   宁可偶尔取到不是高亮那一档的模板，也不能整个功能哑掉。
  return once();
}

// 取某一列当前的"区间 + 夹好的档位下标"。
//   越界的陈旧档位在这里归一（顺序重要：先归一再走方向，见 MoveTier 里的说明）。
//   返回 false 只在"这一列真的没有角色"时（它和"正在重建"的区别见 ReadTierSel 那段）。
//   两个调用点（档位列的 ReadTierSel、召唤路径的 ResolveSelection）都在 seqlock 的
//   "读一次"里面调它，两处的口径必须一致，所以只留这一份。
static bool ReadTierState(int side, int *loOut, int *hiOut, int *selOut, int *tOut) {
  int lo = 0, hi = 0, sel = 0;
  if (!TierRangeOf(side, &lo, &hi, &sel))
    return false;
  const int cnt = hi - lo;
  int t = (int)InterlockedCompareExchange(TierOfSide(side), 0, 0);
  if (t < 0 || t >= cnt)
    t = 0;
  if (loOut)
    *loOut = lo;
  if (hiOut)
    *hiOut = hi;
  if (selOut)
    *selOut = sel;
  if (tOut)
    *tOut = t;
  return true;
}

// 单独、安全地读一次档位状态（HUD 画档位列、换档键走它）。
//   包这一层就是它的全部意义：底下那次读必须走 g_listGen 的 seqlock，
//   而召唤路径要把"读状态"跟"读 v21"拼在同一次 seqlock 里（见 ResolveSelection 的
//   readOnce），所以里层的 ReadTierState 不能自己带 seqlock，包不包由调用方决定。
//   两个调用点各自写 lambda 的话这段样板要抄两遍，所以留这一个包装。
static bool ReadTierSel(int side, int *loOut, int *hiOut, int *selOut, int *tOut) {
  return ReadListSeqlock([&]() -> bool {
    return ReadTierState(side, loOut, hiOut, selOut, tOut);
  });
}

// 按模板实例表重建左/右两列（按 g_team 分阵营）。定义在 ScanTemplates 旁边。
static void BuildSummonLists();

// 必需的钩子是不是都装好了（SPAWN / WORLD / DISPATCH / TRYSPAWN 四个，见它的定义处）。
//   定义在跳板指针那一段（它们声明得比较晚）。
//   F5/F11 在钩子就绪之前按下去不会有任何反应，必须在 HUD 上当场报出来，
//   否则观感就是"这个键坏了"，而日志里什么都没写。
static bool HooksReady();

// 诊断专用，查模板的 AISpawnData 的 +92（Team），只用来打印，不参与分列。
//   定义在 g_adTab 之后（那里才可见）。
#ifdef _DEV
static unsigned int AdTeamOf(unsigned long long v21, bool *foundOut);
#endif

// 重建列表之后把选中下标夹进各自列表的范围（别越界）。
//   只在越界时才动，合法位置一律留着，重扫不该打断手上的选择。
//   角色光标一旦被夹走，那一边的档位光标也一起归零，
//      因为"换了角色，档位回到第一档"是这个 HUD 的固定规则（见 MoveSel）。
//      不这么做就会出现"角色被夹成第 0 个、档位却还停在旧角色的第 4 档"这种不一致，
//      虽然用的时候还会再夹一次（不会越界），但显示上会莫名其妙。
//   档位光标的上界这里管不了（要问"当前角色有几档"才知道），
//      那个在用到的地方夹，ReadTierSel / ResolveSelection。
static void ClampAllSels() {
  int n = 0;
  CharsOfSide(1, &n);
  if (n > 0 && (g_selAlly < 0 || g_selAlly >= n)) {
    InterlockedExchange(&g_selAlly, 0);
    InterlockedExchange(&g_selTierAlly, 0);
  }
  CharsOfSide(2, &n);
  if (n > 0 && (g_selEnemy < 0 || g_selEnemy >= n)) {
    InterlockedExchange(&g_selEnemy, 0);
    InterlockedExchange(&g_selTierEnemy, 0);
  }
  if (g_selTierAlly < 0)
    InterlockedExchange(&g_selTierAlly, 0);
  if (g_selTierEnemy < 0)
    InterlockedExchange(&g_selTierEnemy, 0);
}

// 换阵营时把四个光标全部归零。
//   夹范围不管用：换边之后左右两组是整体对调的
//   （原来的"我方模板"跑到右边去了），同一个下标现在指向的是另一个角色，
//   高亮会突然跳到无关的角色上（档位光标更糟：那个角色可能根本没有那么多档）。
static void ResetAllSels() {
  InterlockedExchange(&g_selAlly, 0);
  InterlockedExchange(&g_selTierAlly, 0);
  InterlockedExchange(&g_selEnemy, 0);
  InterlockedExchange(&g_selTierEnemy, 0);
}

// 往调用方缓冲里写串之前的前置检查：缓冲拿不动就直接说不，绝不去碰它。
//   n 小于 2 的缓冲连终止符都放不下，一律当参数错。全项目只有这一种写法。
static bool OutStrReady(char *out, int n) {
  if (!out || n < 2)
    return false;
  out[0] = 0;
  return true;
}

// 读一个"像命令名"的 C 串。
// 在任意内存上读，必须 SEH。而且这个函数里不能有 std::string 之类有析构
//    函数的对象，编译器不许那样的函数用 __try（C2712）。
static bool ReadIdent(unsigned long long p, char *out, int n) {
  if (!OutStrReady(out, n))
    return false;
  __try {
    char c0 = *(volatile char *)p;
    if (!((c0 >= 'A' && c0 <= 'Z') || (c0 >= 'a' && c0 <= 'z') || c0 == '_'))
      return false;
    int i = 0;
    for (; i < n - 1; ++i) {
      char c = *(volatile char *)(p + (unsigned long long)i);
      if (!c)
        break;
      if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
        return false;
      out[i] = c;
    }
    if (i == 0 || i >= n - 1)
      return false;
    out[i] = 0;
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = 0;
    return false;
  }
}

// ===========================================================================
//  游戏状态（不用任何断点，纯指针读）
// ===========================================================================
//  manager 的拿法，先试 *(u64*)(*(u64*)0x1445C4410 + 176)，不行再试 *(u64*)(0x1445C4410 + 176)；
//  两条候选都还要验它的玩家数组（见下面 GetManager）。
static unsigned long long GetManager() {
  unsigned long long base = 0, one = 0, two = 0;
  if (!Rd64(ADDR_MANAGER_GLOBAL, &base))
    return 0;
  Rd64(ADDR_MANAGER_GLOBAL + MANAGER_OFF, &one);
  if (base)
    Rd64(base + MANAGER_OFF, &two);
  unsigned long long cand[2] = {two, one};
  for (int k = 0; k < 2; ++k) {
    if (!HeapPtr(cand[k]))
      continue;
    unsigned long long a = 0;
    if (!Rd64(cand[k] + PLAYER_ARR_OFF, &a) || !HeapPtr(a))
      continue;
    return cand[k];
  }
  return 0;
}

// 找本地玩家（+72 低字节 == 0 的那个），顺便数一下槽位。
//   slotCountOut 传空 = 不关心槽位数，这时一找到本地玩家就停：
//     每扫一个槽都要 HeapPtr（一次 VirtualQuery 内核查询），而 RefreshTeam 每 100ms 调一次，
//     全扫满 96 个槽要一百来次查询。真正要那个数的调用方（RefreshTeam）传指针即可。
static unsigned long long FindLocalPlayer(unsigned long long mgr, int *slotCountOut) {
  if (slotCountOut)
    *slotCountOut = -1;
  if (mgr == 0)
    return 0;
  unsigned long long arr = 0;
  if (!Rd64(mgr + PLAYER_ARR_OFF, &arr) || !HeapPtr(arr))
    return 0;
  unsigned int cap = 0;
  Rd32(mgr + MANAGER_SLOTS_OFF, &cap);
  unsigned int lim = (cap >= 8 && cap <= DUMP_MAX_SLOTS) ? cap : DUMP_MAX_SLOTS;
  unsigned long long me = 0;
  int n = 0;
  for (unsigned int i = 0; i < lim; ++i) {
    unsigned long long p = 0;
    if (!Rd64(arr + 8ull * i, &p))
      break;
    if (!HeapPtr(p))
      continue;
    ++n;
    if (me == 0) {
      unsigned int b72 = 0;
      // 必须检查返回值，读失败时 b72 仍是 0，会被当成"这就是本地玩家"，me 是垃圾指针，
      //    而 RefreshTeam 用它会读到 t=0，HUD 永远显示"还没读到你的队号"。
      if (!Rd32(p + PL_ISAI_OFF, &b72))
        continue;
      if ((b72 & 0xFF) == 0) {
        me = p;
        if (!slotCountOut)
          return me;
      }
    }
  }
  if (slotCountOut)
    *slotCountOut = n;
  return me;
}

// "[TEAM] team not read yet" 那行的节流时刻（见 RefreshTeam）。
//   那条分支在菜单/大厅里每 100ms 就会走到一次，不节流的话一分钟刷 600 行，
//   把真正有用的行全淹掉（实测一份 25 分钟的日志里两万多行几乎全是它）。
static volatile LONG g_noteamLogT = 0;

// 重新读一次本地玩家的队伍。
//
// 由 Worker 每 100ms 自动调一次，换队 / 进新局都不需要任何手动操作。
// 周期调用必须 quiet=true，否则"队号没变"会把日志刷满。
// 只有真的变了才置 g_hud_dirty，HUD 不会每秒无条件重画十次。
// "变了"这件事只用来置 g_hud_dirty，本函数不向外返回值。
static void RefreshTeam(const char *why, bool quiet) {
  unsigned long long mgr = GetManager();
  // 槽数只有三条日志用得到，而 FindLocalPlayer 带上计数会把整个槽位池扫一遍
  //   （每槽一次 VirtualQuery）。这条函数每 100ms 跑一次，所以按需再读。
  int slots = -1;
  auto slotCount = [&]() -> int {
    if (slots < 0)
      FindLocalPlayer(mgr, &slots);
    return slots;
  };
  unsigned long long me = FindLocalPlayer(mgr, nullptr);
  unsigned int t = 0;
  if (me && Rd32(me + PL_ID88_OFF, &t))
    t &= 0xFFu;  // 队号只有最低字节（文件里其余读队号的地方都这么取），高位是别的标志

  bool changed = false;
  if (t && (LONG)t != g_team) {
    InterlockedExchange(&g_team, (LONG)t);  // 先定队号，BuildSummonLists 按它分左右两组
    // 换队，左右两组整体对调（"我方模板"变成原来对方那批）。
    // 必须重建列表，并且把四个光标全部归零，同一个下标现在指的是另一个角色，
    //     留着它会让高亮莫名其妙地跳走（见 ResetAllSels 的说明）。
    BuildSummonLists();
    ResetAllSels();
    Log("[TEAM] team = %u (%s)  [%s]  player slots=%d, list rebuilt, cursors reset"
        " (col1 ally char / col2 ally tier / col3 enemy char / col4 enemy tier)",
        t, TeamName(), why, slotCount());
    changed = true;
  } else if (t == 0) {
    // 内容不变，所以按 5 秒一条节流；主动按键那一次（quiet=false）不节流，
    //   那正是"我按了键却没反应"要看的现场。
    const DWORD nowMs = GetTickCount();
    const DWORD lastMs = (DWORD)AtomicReadL(&g_noteamLogT);
    if (!quiet || (DWORD)(nowMs - lastMs) >= 5000) {
      InterlockedExchange(&g_noteamLogT, (LONG)nowMs);
      Log("[TEAM] team not read yet (+88), waiting for assignment. Auto-read within 100ms,"
          " no key needed. mgr=0x%llX me=0x%llX",
          mgr, me);
    }
  } else if (!quiet) {
    Log("[TEAM] team unchanged = %u (%s)  [%s]  player slots=%d", t, TeamName(), why, slotCount());
  }
  if (changed)
    InterlockedExchange(&g_hud_dirty, 1);
}

// ===========================================================================
//  HUD，分层窗口 + 颜色键 + 鼠标穿透 + 不抢焦点（四列版）
//  样式与坐标取实测值（已对着游戏核过）
//
//   四列 = 每边各配一个角色列 + 一个档位列：
//     档位是每个角色各自不同的（有的角色只有基础档，有的有 Champion / Elite 好几档），
//     一个档位列说不清"这是谁的档位"。四个列分别是：
//        第 1 列 队友角色   F1/F2 选    第 2 列 队友档位   F3/F4 选     F5 召唤
//        第 3 列 敌人角色   F7/F8 选    第 4 列 敌人档位   F9/F10 选    F11 召唤
//        F6 = 显隐 HUD
//
//  尺寸固定，列表再长也只开一窗 HUD_ROWS 行，
//    所以窗口高度是常量，HudThread 不需要每次重画都 SetWindowPos。
//  排序 = 可玩置顶 + 其余按显示名字典序。
//    比较时显示名优先、key 其次、全名最后，这样同一角色的各档位必然相邻，
//    档位区间在建表时就切好了（见 BuildSummonLists / SummonRawCmp）。
// ===========================================================================
constexpr int HUD_X = 24;
constexpr int HUD_Y = 24;
constexpr int HUD_ROW_H = 16;
constexpr int HUD_ROWS = 14;  // 每列显示多少行（超出就滚动）
// 四列不等宽，但是两两相等：两个角色列一样宽、两个档位列一样宽。
//   宽度就是下面 HUD_W_CHAR / HUD_W_TIER 这两个数，四个 X 坐标全部由宽度表达式推出来，
//   等宽是构造出来的、不可能错位：手写常量很容易让两个档位列差一两像素（221 / 223 这种），
//   肉眼就能看出不一样宽。
// 这么定是因为两张列的需求差了一大截（序号取自一批日志，里面 338 个真实模板名）：
//   角色名最长 EngineerTurretDeploy = 20 字符，"> " + 20 = 22 字符 ≈ 169px
//   档位标签最长 GiantNightCapBossSporetillery = 29 字符，"> " + 29 = 31 字符 ≈ 239px
//   角色列取 180px，放得下那 22 字符（见 HUD_W_CHAR）。
//   档位列 215px 不是照 29 字符给的，它给的是截断之后的长度：标签一律先截到
//   HUD_TIER_CHARS = 25 字符（"> " + 25 = 27 字符 ≈ 207px），再长的宁可靠 ".." 表示截断，
//   也不让列宽跟着最长的那个标签走。截断由 FitLabel 按字符数做（HUD_CHAR_CHARS / HUD_TIER_CHARS）。
//   字体是 Consolas 14px，字宽约 7.7px。角色列宽下限 180px：再窄长名字就会压到下一列上。
constexpr int HUD_PAD = 8;  // 左边距 / 右边距
// 结果行（"上次召唤结果"）的左起点。比列的左边距多 2 像素，是刻意的：它是独立一行，
//   和四列的左边缘差开一点看着才不像某一列的延续。
constexpr int HUD_MSG_X = 10;
constexpr int HUD_GAP = 8;          // 列间距
constexpr int HUD_W_CHAR = 180;     // 角色列内容宽
constexpr int HUD_CHAR_CHARS = 22;  // 角色名最多画这么多字符，超出截断加 ".."
constexpr int HUD_W_TIER = 215;     // 档位列内容宽
constexpr int HUD_C1X = HUD_PAD;
constexpr int HUD_C2X = (HUD_C1X + HUD_W_CHAR + HUD_GAP);
constexpr int HUD_C3X = (HUD_C2X + HUD_W_TIER + HUD_GAP);
constexpr int HUD_C4X = (HUD_C3X + HUD_W_CHAR + HUD_GAP);
constexpr int HUD_TIER_CHARS = 25;  // 档位标签最多画这么多字符
// 四列表头占两行。
//   第 1 行 = "ALLY 3/31"（位置/总数，溢出时它自己就说明了）
//   第 2 行 = 这个列自己的按键，写全 "Prev / Next"
// 上次召唤结果那一行在屏幕上留多久。HudPaint 按它决定还画不画，HudThread 按它决定
//   还要不要继续置脏重画：少了后半句，那行到点不会自己消失，会一直赖在屏幕上。
//   两边必须是同一个数，所以放这里。
constexpr int HUD_MSG_MS = 5000;
constexpr int HUD_Y0 = 38;  // 四列表头第 1 行（上面是 1 行全局热键 + 1 行队伍）
// 窗口高度按布局公式算：手写常量容易少算几像素，把最底下那行
//   "上次召唤结果"切掉（结果行底边约在 316，窗口高度必须大于它）。
// 结果行的顶部 y = 表头 2 行 + HUD_ROWS 行 + 4 像素间隔。
//   画的时候和算窗口高度的时候都要它，所以只写这一份。
constexpr int HUD_YBOT = (HUD_Y0 + HUD_ROW_H * (HUD_ROWS + 2) + 4);
// 窗口高度 = 结果行顶 + 一行结果 + 4 像素底边距。
constexpr int HUD_H = (HUD_YBOT + HUD_ROW_H * 1 + 4);
constexpr int HUD_W = (HUD_C4X + HUD_W_TIER + HUD_PAD);

static HWND g_hwnd = nullptr;

// 档位标签 = 短名第一个 '_' 之后的部分；没有 '_'，这就是该角色的"基础"档。
static const char *TierLabelOf(const char *shortName) {
  const char *u = strchr(shortName, '_');
  return u ? (u + 1) : "(base)";
}

// 把标签抄进 dst，超过 maxChars 就截断并加 ".."。
//   档位标签实测最长 29 字符（GiantNightCapBossSporetillery），
//   直接画会越过列宽压进右边那一列，看起来像两列叠在一起。
static void FitLabel(char *dst, int cap, const char *src, int maxChars) {
  const int n = (int)strlen(src);
  if (n <= maxChars || maxChars < 4) {
    CopyStr(dst, (size_t)cap, src);
    return;
  }
  _snprintf_s(dst, (size_t)cap, _TRUNCATE, "%.*s..", maxChars - 2, src);
}

// 配色按阵营取，植物绿、僵尸紫。
//   玩家当僵尸时，第 1 列（队友，全是僵尸）是紫的、第 3 列（敌人，全是植物）是绿的，
//   和"1 绿 3 紫"正好相反，所以颜色必须跟着阵营走。
//   阵营编码和 g_team / AISpawnData+92 一致：1=僵尸 2=植物。
static COLORREF FactionColor(int faction) {
  if (faction == 2)
    return RGB(160, 255, 160);  // 植物 = 绿
  if (faction == 1)
    return RGB(200, 140, 255);  // 僵尸 = 紫
  return RGB(200, 200, 200);    // 还没分组，灰（此时两列本来就是空的）
}

// 某一边（1 = 队友/第 1、2 列，2 = 敌人/第 3、4 列）对应的游戏阵营。
//   队友 = 我方阵营；敌人 = 对方阵营。返回 0 = 还没读到队号。
static int FactionOfSide(int side) {
  LONG t = g_team;  // 1=僵尸 2=植物
  if (t != 1 && t != 2)
    return 0;
  if (side == 1)
    return (int)t;
  return (t == 2) ? 1 : 2;
}

// 画一行已经格式化好的串。长度每处都要现算一遍，所以只留这一个入口。
static void OutLine(HDC dc, int x, int y, const char *s) {
  TextOutA(dc, x, y, s, (int)strlen(s));
}

// 画一列可滚动的角色列表。x/y = 第 1 行表头的位置。
//   表头第 1 行 = head1（"ALLY 3/31"），第 2 行 = head2（按键说明），数据从第 3 行起。
//
//   表头第 1 行的 "3/31" 已经说明位置和总数，不再另加符号。
static void DrawCol(HDC dc, int x, int y, const char *head1, const char *head2, CharEntry *list,
                    int n, int sel, COLORREF colName, const char *emptyMsg) {
  char line[256];
  const int yData = y + HUD_ROW_H * 2;  // 数据从表头 2 行之后开始

  if (n <= 0 || !list) {
    SetTextColor(dc, colName);
    CopyStr(line, sizeof(line), head1);
    OutLine(dc, x, y, line);
    CopyStr(line, sizeof(line), head2);
    OutLine(dc, x, y + HUD_ROW_H, line);
    if (emptyMsg) {
      SetTextColor(dc, RGB(255, 170, 170));
      CopyStr(line, sizeof(line), emptyMsg);
      OutLine(dc, x, yData, line);
    }
    return;
  }
  // 开窗是为了让选中项尽量居中
  int first = 0;
  if (n > HUD_ROWS) {
    first = sel - HUD_ROWS / 2;
    if (first < 0)
      first = 0;
    if (first > n - HUD_ROWS)
      first = n - HUD_ROWS;
  }
  int shown = (n - first) < HUD_ROWS ? (n - first) : HUD_ROWS;

  SetTextColor(dc, colName);
  CopyStr(line, sizeof(line), head1);
  OutLine(dc, x, y, line);
  SetTextColor(dc, RGB(255, 255, 255));
  CopyStr(line, sizeof(line), head2);
  OutLine(dc, x, y + HUD_ROW_H, line);

  for (int k = 0; k < shown; ++k) {
    int idx = first + k;
    bool on = (idx == sel);
    SetTextColor(dc, on ? RGB(255, 255, 255) : colName);
    // 和档位列一样按字符数截断，实测最长的角色名已经到 20 字符（"> " + 20 = 169px
    //   顶在 180px 的边上），再长就会画进第 2 列。
    char lab[HUD_CHAR_CHARS * 2 + 8];
    FitLabel(lab, sizeof(lab), list[idx].key, HUD_CHAR_CHARS);
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s", on ? "> " : "  ", lab);
    OutLine(dc, x, yData + HUD_ROW_H * k, line);
  }
}

// 画一个档位列，side = 1 队友列 / 2 敌人列，画的是该列所选角色的全部档位。
//   表头两行，第 1 行 "TIER ally 3/12"，第 2 行按键（"F3 Prev   F4 Next"）。
//   档位列紧挨着它自己的角色列，所以不重复角色名；最右边那一列补角色名还会画出窗口外
//   （HUD_W 兜不住）。
static void DrawTierCol(HDC dc, int x, int y, int side) {
  char line[256];
  const int yData = y + HUD_ROW_H * 2;
  // 这一列的配色 = 它所属阵营的颜色（不是列号）
  const COLORREF fc = FactionColor(FactionOfSide(side));
  const char *keys = (side == 1) ? "F3 Prev   F4 Next" : "F9 Prev   F10 Next";

  int lo = 0, hi = 0, tsel = 0;
  if (!ReadTierSel(side, &lo, &hi, nullptr, &tsel)) {
    SetTextColor(dc, fc);
    _snprintf_s(line, sizeof(line), _TRUNCATE, "TIER %s", side == 1 ? "ally" : "enemy");
    OutLine(dc, x, y, line);
    SetTextColor(dc, RGB(255, 255, 255));
    CopyStr(line, sizeof(line), keys);
    OutLine(dc, x, y + HUD_ROW_H, line);
    SetTextColor(dc, RGB(255, 170, 170));
    _snprintf_s(line, sizeof(line), _TRUNCATE, "no char");
    OutLine(dc, x, yData, line);
    return;
  }
  int cnt = hi - lo;
  // tsel 只用于显示，写回光标的是 MoveTier（按键挪档）与 ClampAllSels（夹回合法范围）：
  //   这里的 cnt 可能已经比按键那一刻旧，写回会把按键刚挪过去的值改回去。
  _snprintf_s(line, sizeof(line), _TRUNCATE, "TIER %s %d/%d", side == 1 ? "ally" : "enemy",
              cnt > 0 ? tsel + 1 : 0, cnt);
  SetTextColor(dc, fc);
  OutLine(dc, x, y, line);
  SetTextColor(dc, RGB(255, 255, 255));
  CopyStr(line, sizeof(line), keys);
  OutLine(dc, x, y + HUD_ROW_H, line);

  // 档位列必须和角色列一样开窗，超过 HUD_ROWS 时不开窗的话，选中的那一行会被画到
  //    窗口外，屏幕上完全没有高亮，看起来像键坏了。
  //    （Chomper 就有 9 档，再多几个角色很容易超过 HUD_ROWS。）
  int first = 0;
  if (cnt > HUD_ROWS) {
    first = tsel - HUD_ROWS / 2;
    if (first < 0)
      first = 0;
    if (first > cnt - HUD_ROWS)
      first = cnt - HUD_ROWS;
  }
  int show = (cnt - first) < HUD_ROWS ? (cnt - first) : HUD_ROWS;
  TierEntry *tl = TiersOfSide(side);
  char lab[SUM_NAME_MAX];
  for (int k = 0; k < show; ++k) {
    const int idx = first + k;  // 注意是 idx 不是 k，开窗后两者不再相等
    bool on = (idx == tsel);
    SetTextColor(dc, on ? RGB(255, 255, 255) : fc);
    FitLabel(lab, sizeof(lab), tl[lo + idx].label, HUD_TIER_CHARS);
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s", on ? "> " : "  ", lab);
    OutLine(dc, x, yData + HUD_ROW_H * k, line);
  }
}

// 四列全部画出来。
static void HudPaint(HWND hwnd) {
  int nA = 0, nE = 0;
  CharEntry *listA = CharsOfSide(1, &nA);
  CharEntry *listE = CharsOfSide(2, &nE);
  LONG tnow = g_team;
  int H = HUD_H;

  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hwnd, &ps);
  RECT rc = {0, 0, HUD_W, H};
  // 这三个 GDI 对象建一次就够。每次重画都新建再删，结果行显示期间每秒 8 次，
  //   纯属给 内核加活。只有 HUD 线程会跑到这里，静态复用不需要额外同步。
  static HBRUSH bg = CreateSolidBrush(RGB(0, 0, 0));  // 黑色 = 颜色键，透明
  static HFONT fB =
      CreateFontA(-15, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Consolas");
  static HFONT fN =
      CreateFontA(-14, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, 0, 0, "Consolas");
  FillRect(dc, &rc, bg);
  SetBkMode(dc, TRANSPARENT);
  HFONT fOld = (HFONT)SelectObject(dc, fB);
  char line[256];

  // 1)  全局热键（只有"不专属某个列"的那几个，每个列自己的按键写在它自己的表头第 2 行）
  SetTextColor(dc, RGB(255, 255, 255));
  _snprintf_s(line, sizeof(line), _TRUNCATE, "F5 Summon ally     F11 Summon enemy     F6 Hide");
  OutLine(dc, HUD_PAD, 6, line);

  // 2)  队伍（同一套阵营配色；还没分组时用红色示警）
  SetTextColor(dc, (tnow == 1 || tnow == 2) ? FactionColor((int)tnow) : RGB(255, 120, 120));
  _snprintf_s(line, sizeof(line), _TRUNCATE, "Team: %s", TeamName());
  OutLine(dc, HUD_PAD, 22, line);

  SelectObject(dc, fN);

  int selA = (int)AtomicReadL(&g_selAlly);
  int selE = (int)AtomicReadL(&g_selEnemy);
  if (nA > 0 && (selA < 0 || selA >= nA))
    selA = 0;
  if (nE > 0 && (selE < 0 || selE >= nE))
    selE = 0;

  // 3)  四列表头
  char h1[64], h3[64];
  _snprintf_s(h1, sizeof(h1), _TRUNCATE, "ALLY %d/%d", nA > 0 ? selA + 1 : 0, nA);
  _snprintf_s(h3, sizeof(h3), _TRUNCATE, "ENEMY %d/%d", nE > 0 ? selE + 1 : 0, nE);

  // 4)  四列内容，配色一律取这一列所属阵营的颜色，不是列号（"当僵尸时第 1 列反而是紫的"
  //     那个例子写在 FactionColor 那处，不再抄一遍）。
  //    表头第 2 行 = 这一列自己的按键（写全 Prev/Next，别用缩写）。
  int y0 = HUD_Y0;
  // 空列表时的提示必须短，角色列只有 HUD_W_CHAR（180px，约 23 个字符），
  //   提示写超了会直接压进右边那一列。
  // 提示按列给：单列空白是完全可能的（一边的角色模板还没流式加载进来，
  //   或全被 AD/_Playable 过滤掉），这时那一列要自己说明原因，
  //   否则它是一块什么都不说的空白，而按键只会回一行 FAIL。
  const char *msgA = (nA > 0) ? nullptr : ((nE <= 0) ? "waiting..." : "no chars");
  const char *msgE = (nE > 0) ? nullptr : ((nA <= 0) ? "waiting..." : "no chars");
  DrawCol(dc, HUD_C1X, y0, h1, "F1 Prev   F2 Next", listA, nA, selA, FactionColor(FactionOfSide(1)),
          msgA);
  DrawTierCol(dc, HUD_C2X, y0, 1);
  DrawCol(dc, HUD_C3X, y0, h3, "F7 Prev   F8 Next", listE, nE, selE, FactionColor(FactionOfSide(2)),
          msgE);
  DrawTierCol(dc, HUD_C4X, y0, 2);

  // 5)  上次召唤结果（几秒后消失），画在整块下面（表头 2 行 + HUD_ROWS 行 + 4 像素间隔）
  int yBot = HUD_YBOT;  // y0 == HUD_Y0
  if (g_last_t && (GetTickCount() - g_last_t) < HUD_MSG_MS) {
    SetTextColor(dc, g_last_ok == 1 ? RGB(120, 255, 120) : RGB(255, 120, 120));
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[ %s ]", g_last_msg);
    OutLine(dc, HUD_MSG_X, yBot, line);
  }

  SelectObject(dc, fOld);
  EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_PAINT) {
    HudPaint(hwnd);
    return 0;
  }
  if (msg == WM_ERASEBKGND)
    return 1;
  return DefWindowProcA(hwnd, msg, wp, lp);
}

static DWORD WINAPI HudThread(LPVOID) {
  WNDCLASSEXA wc = {0};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = HudProc;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "BFNSummonAIHud";
  RegisterClassExA(&wc);
  g_hwnd = CreateWindowExA(
      WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
      "BFNSummonAIHud", "", WS_POPUP, HUD_X, HUD_Y, HUD_W, HUD_H, nullptr, nullptr, wc.hInstance,
      nullptr);
  if (!g_hwnd) {
    Log("!! CreateWindowEx failed (%lu)", GetLastError());
    return 1;
  }
  SetLayeredWindowAttributes(g_hwnd, RGB(0, 0, 0), 217, LWA_COLORKEY | LWA_ALPHA);
  ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);

  MSG msg;
  while (g_running) {
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
    // 召唤结果那行要能自己消失。g_hud_dirty 只在"真有变化"时才置，
    //   所以显示窗口内要主动保持重画，否则那条消息会一直赖在屏幕上。
    if (g_last_t && (DWORD)(GetTickCount() - g_last_t) < HUD_MSG_MS)
      InterlockedExchange(&g_hud_dirty, 1);

    if (g_hud_dirty) {
      InterlockedExchange(&g_hud_dirty, 0);
      ShowWindow(g_hwnd, g_hud_show ? SW_SHOWNOACTIVATE : SW_HIDE);
      InvalidateRect(g_hwnd, nullptr, TRUE);
    }
    Sleep(120);
  }
  return 0;
}

// ===========================================================================
//  热键线程
// ===========================================================================
// 按键边沿。看两个位：0x8000 是"现在按着"，0x0001 是"上一次查询之后按过"。
//   只看 0x8000 会漏按键：这个循环的周期不是固定的 30ms，命中召唤键那几轮要现读队号、
//   必要时还要重建列表（那条路自己估过一百多毫秒），一次轻点就可能整次落在两次采样之间，
//   结果是"按了没反应、日志里也没有任何痕迹"。
//   0x0001 补上这个缺口。它不是保证：MSDN 说别的进程调 GetAsyncKeyState 会把这个位抢走
//   （那种情况只会漏、不会凭空多报一次），所以它是补充，不是替代。
static bool KeyEdge(int vk, bool *prev) {
  const int st = GetAsyncKeyState(vk);
  const bool down = (st & 0x8000) != 0;
  const bool sinceLast = (st & 0x0001) != 0;
  const bool hit = (down && !*prev) || sinceLast;
  *prev = down;
  return hit;
}

// 召唤的实现定义在探针段（要用 ScanTemplates 扫出来的模板表 g_tplTab
// 和载体 g_aiCtx），而按键线程在它前面，所以这里先声明一下。
static void DoSummonSpawner(unsigned long long v21, LONG seq);

// 发布一条结果给 HUD。四个量的顺序固定，不许换：先内容（msg）、再判据（ok）、再时刻、
//   最后置脏。读方以"先看到内容、再看到判据"为前提，换顺序就会看到新判据配旧文案。
//   文案本身留在调用点（HUD 上要靠它分清是哪一次的成功与失败）。
static void SetSummonResult(LONG ok, const char *msg) {
  CopyStr(g_last_msg, sizeof(g_last_msg), msg);
  InterlockedExchange(&g_last_ok, ok);
  g_last_t = GetTickCount();
  InterlockedExchange(&g_hud_dirty, 1);
}

// 直接把失败写进 HUD：给"重试也没用"的那几档用（原因不会在一秒内消失）。
static void SummonFailedNow(const char *why) {
  SetSummonResult(2, why);
}

// 这一发失败了。还有预算就排下一次（0.5 秒后，Worker 到点重发），预算用完才写 HUD。
//   why 是给 HUD 那一行用的短文案（形如 "FAIL expired"）。
//   seq 是这一发属于哪一次按键：只有它还是当前那一次，才动重试状态。
//     过期的那一发（跑着跑着又按了一下键）只写日志，否则会把新一次按键的预算吃掉一格、
//     或者给新一次按键排一个它不要的重试。
static void SummonAttemptFailed(LONG seq, const char *why) {
  const LONG cur = (LONG)AtomicReadL(&g_retrySeq);
  if (seq != cur) {
    Log("[RETRY] attempt failed (%s), but it belongs to press #%ld and the current one is #%ld,"
        " so its retry state was already superseded",
        why, seq, cur);
    return;
  }
  const LONG left = InterlockedExchangeAdd(&g_retryLeft, -1) - 1;
  if (left > 0) {
    InterlockedExchange(&g_retryAt, (LONG)(GetTickCount() + SUM_RETRY_DELAY_MS));
    Log("[RETRY] attempt failed (%s), %ld attempt(s) left, retrying in %d ms", why, left,
        SUM_RETRY_DELAY_MS);
    return;
  }
  Log("[RETRY] out of attempts (%d used), reporting FAIL on the HUD", SUM_RETRY_MAX);
  SummonFailedNow(why);
}

// 撤销重试。给"重试也没用"的那一档用：钩子还没装好，等 0.5 秒再试一次也还是没装好。
//   只有刚按下的那一次按键会调它，所以不用管序列号。
static void SummonRetryClear() {
  InterlockedExchange(&g_retryLeft, 0);
  InterlockedExchange(&g_retryAt, 0);
}

// 召唤必须排到游戏线程上执行，在按键线程里直接调会崩（实测定位）：
//
//   异常 code=0xC0000027  出错指令 RVA=0x305509  访问的地址=0x18。
//   0x1403054F0 反汇编（只有 24 字节）：
//       sub rsp,28h / call sub_14106E100 / test rax,rax / jz loc_140305509
//       ... mov rax,[rax+10h]  / retn
//       loc_140305509: mov rax,[rax+18h]，出错就是这条，rax 正是 0
//   而 sub_14106E100（在 .sdata 段）全部指令是：
//       mov ecx, cs:TlsIndex
//       mov rax, gs:58h                  ; TEB，ThreadLocalStoragePointer
//       mov edx, 548h
//       mov rax, [rax+rcx*8]             ; 本线程的 TLS 块
//       mov rax, [rdx+rax]               ; [TLS块 + 0x548]，纯线程局部状态
//       retn
// 引擎要求调用它的线程得带 Frostbite 的 TLS 上下文。游戏线程有；
//     我们用 CreateThread 起的按键线程没有，返回 NULL，调用方没判空，读 [0+0x18]。
// 所以召唤键（F5/F11）只置一个"待办"标志，真正的注入放到 DispatchDetour 里执行，
//     那个 detour 就在游戏线程上跑（约 28 次/秒，延迟 ~35ms 看不出来），
//     而且它就是 AI 生成那条路，TLS 上下文天然正确。
//   附带好处是同一个线程，我们的注入和 AI 调度器不可能并发碰同一个 ctx
//     （严格串行），比在多线程上改 ctx+840 安全得多。
// 这个标志只负责叫醒，按键线程置 1，DispatchDetour 消费，载荷在 g_queuedV21，
//   消费方用一次原子交换取走；取到 0 说明这次唤醒是多余的（载荷已被取走），跳过。
static volatile LONG g_pendingSummon = 0;
// 排队时刻。没有它的话，在大厅/加载/没有 AI 刷怪的场景里按下的那一次会一直挂着，
//   直到很久之后某次 DISPATCH 才执行，冒出一个早就忘了的目标。
//   超过 QUEUE_TTL_MS 就作废并报给 HUD，不要静默执行。
static volatile LONG g_queuedT = 0;
constexpr int QUEUE_TTL_MS = 2000;
// 排队的载荷（目标模板），同时就是邮箱：消费方用 InterlockedExchange64 取走。
//   排队时就把目标解析好，游戏线程执行时直接用它，不再看 HUD 状态
//   （那中间可能有几百毫秒，期间可能又按了别的键）。
static volatile LONG64 g_queuedV21 = 0;
static char g_queuedName[SUM_NAME_MAX] = {0};

// -- 选中项移动 ------------------------------------------------------------
// 切角色（side: 1=队友 2=敌人；dir: -1 上一个 / +1 下一个）。
//   同时把这一边的档位光标归零，换角色后档位区间整个变了，
//     留在旧下标上会指到另一个档位上去（甚至越界）。
static void MoveSel(int side, int dir) {
  int n = 0;
  CharEntry *list = CharsOfSide(side, &n);
  if (!list || n <= 0) {
    // 列表还空着（没进关卡 / 没扫完），顺手重读一次队号
    RefreshTeam(side == 1 ? "F1/F2" : "F7/F8", false);
    Log("[KEY] %s column still empty (chars = %d)", side == 1 ? "ally" : "enemy", n);
    return;
  }
  volatile LONG *ps = SelOfSide(side);
  int s = (int)InterlockedCompareExchange(ps, 0, 0);
  s = (s + dir + n) % n;  // dir=±1 且 n>0，结果必落在 [0,n-1]
  InterlockedExchange(ps, s);
  InterlockedExchange(TierOfSide(side), 0);  // 换角色，档位回到该角色的第一档
  InterlockedExchange(&g_hud_dirty, 1);
  Log("[KEY] %s char %s, [%d/%d] %s (%d tiers, tier cursor reset)", side == 1 ? "ally" : "enemy",
      dir < 0 ? "prev" : "next", s + 1, n, list[s].key, list[s].hi - list[s].lo);
}

// 切档位，side 指定切哪一边的档位列（1=队友 第2列，2=敌人 第4列）。
//   每边一个独立光标，所以两边可以各停在不同档位上。
//   档位数每个角色不同，所以在当前角色的档位区间里取模。
static void MoveTier(int side, int dir) {
  int lo = 0, hi = 0, t = 0;
  if (!ReadTierSel(side, &lo, &hi, nullptr, &t)) {
    Log("[KEY] switch %s tier, but that column has no char selected", side == 1 ? "ally" : "enemy");
    return;
  }
  int cnt = hi - lo;
  if (cnt <= 0)
    return;
  volatile LONG *pt = TierOfSide(side);
  // 方向在 ReadTierSel 归一之后才算：先走方向的话，换角色后档位区间变小
  //   （旧 t=3、新 cnt=2）时 ((3%2)+1+2)%2 = 0，而屏幕上本来就是夹到 0 显示的，
  //   按键看着像没反应。
  t = (t + dir + cnt) % cnt;
  InterlockedExchange(pt, t);
  InterlockedExchange(&g_hud_dirty, 1);
  Log("[KEY] %s tier %s, [%d/%d] %s", side == 1 ? "ally" : "enemy", dir < 0 ? "prev" : "next",
      t + 1, cnt, TiersOfSide(side)[lo + t].label);
}

// 把"某列当前选中的 角色 + 档位"解析成一个模板实例。
//   nameOut 给"角色_档位"的拼法（只用于日志/提示）。
static bool ResolveSelection(int side, unsigned long long *v21Out, char *nameOut, int nameCap) {
  // 一次读取，取区间、夹档位下标、取 v21、拼"角色_档位"名字。
  //   返回 false 只在"这一列真的没有可选角色"时。
  auto readOnce = [&]() -> bool {
    int lo = 0, hi = 0, sel = 0, t = 0;
    if (!ReadTierState(side, &lo, &hi, &sel, &t))
      return false;
    TierEntry *tl = TiersOfSide(side);
    CharEntry *chars = CharsOfSide(side, nullptr);
    if (v21Out)
      *v21Out = tl[lo + t].v21;
    if (nameOut && nameCap > 0)
      _snprintf_s(nameOut, (size_t)nameCap, _TRUNCATE, "%s_%s", chars ? chars[sel].key : "?",
                  tl[lo + t].label);
    return true;
  };
  return ReadListSeqlock(readOnce);
}

// 排队一次召唤。目标在这里就解析并抄下来，
//   游戏线程执行时（~35ms 后，甚至更久）HUD 状态可能已经被改过。
// isRetry = 这一发是 Worker 到点重发的，不是又按了一次键。
//   又按一次键 = 一个新的序列号（seq 由这里分配），预算重置；重试发出来的沿用传进来的那个。
//   序列号用参数传进去：Worker 发的重试与按键那一发之间会交错，
//   只读全局量的话，重试会把刚按下的那一次覆盖掉（连左右侧都会被带错）。
static void QueueSummon(int side, bool isRetry, LONG seq) {
  // 当前序列号只读一次，判断与日志共用：读两次的话，日志里那个号可能不是让这一支成立的那个。
  const LONG curSeq = (LONG)AtomicReadL(&g_retrySeq);
  if (!isRetry) {
    // 先换序列号再置状态：换完之后，上一次按键还没回来的那些失败就自动过期了。
    seq = InterlockedIncrement(&g_retrySeq);
    InterlockedExchange(&g_retryLeft, SUM_RETRY_MAX);
    InterlockedExchange(&g_retryAt, 0);
    InterlockedExchange(&g_retrySide, side);
  } else if (seq != curSeq) {
    // 排队这段时间里又按了一下键，这一发重试是冲着上一次按键去的，丢掉。
    Log("[RETRY] skipping this retry: it was scheduled for press #%ld and a newer press (#%ld)"
        " has taken over",
        seq, curSeq);
    return;
  }
  if (!HooksReady()) {
    // 这条不重试：钩子要等目标函数解密（外加分配跳板内存），探测器最多等 5 分钟，
    //   等 0.5 秒毫无意义。
    //   当场把情况报给 HUD，别让人再等一秒才知道。
    Log("[KEY] summon ignored: the hooks are not installed yet (see the [PROBE] lines)");
    SummonRetryClear();
    SummonFailedNow("FAIL hooks not ready yet");
    return;
  }
  unsigned long long v21 = 0;
  char nm[SUM_NAME_MAX] = {0};
  if (!ResolveSelection(side, &v21, nm, (int)sizeof(nm)) || v21 == 0) {
    Log("[KEY] summon: %s column has no summonable char (list empty / scan not done)",
        side == 1 ? "ally" : "enemy");
    // 这条也进重试：刚换关时列表会被清空重建，那几百毫秒里按召唤本来就召不出来，
    //   等下一次重试时列表已经好了。
    {
      char why[SUM_NAME_MAX + 8];
      _snprintf_s(why, sizeof(why), _TRUNCATE, "FAIL no selection in %s column",
                  side == 1 ? "ally" : "enemy");
      SummonAttemptFailed(seq, why);
    }
    return;
  }
  // 先把名字、时刻、序列号写好，再发布载荷（g_queuedV21），最后才放唤醒标志。
  //   消费方取走载荷之后才读名字/时刻，读到的通常就是这次按键写下的；
  //   名字只进日志和 HUD，快照不完美不影响目标本身（那是原子交换）。
  CopyStr(g_queuedName, sizeof(g_queuedName), nm);
  InterlockedExchange(&g_queuedT, (LONG)GetTickCount());
  InterlockedExchange(&g_queuedSeq, seq);
  InterlockedExchange64(&g_queuedV21, (long long)v21);
  InterlockedExchange(&g_pendingSummon, 1);
  Log("[KEY] %s summon queued: %s (waits for next AI tick, ~35ms, runs on game thread)",
      side == 1 ? "F5 ally" : "F11 enemy", g_queuedName);
}

// 键位分左右两串，每组都是先选角色、再选档位、最后召唤，两侧位置对称，
//   召唤键放在一组的最外侧（F5 / F11）：它是这条操作流的终点。
//
// 列表不过滤，可玩职业排在最上面（按 kPlayableLocId 的顺序），其余按显示名字典序跟在后面。
//   不可玩角色不筛，它们和可玩角色走的是同一条生成路。
static DWORD WINAPI KeyThread(LPVOID) {
  bool p1 = false, p2 = false, p3 = false, p4 = false, p5 = false, p6 = false, p7 = false,
       p8 = false, p9 = false, p10 = false, p11 = false;
  while (g_running) {
    if (KeyEdge(VK_F1, &p1))
      MoveSel(1, -1);
    if (KeyEdge(VK_F2, &p2))
      MoveSel(1, +1);
    if (KeyEdge(VK_F3, &p3))
      MoveTier(1, -1);
    if (KeyEdge(VK_F4, &p4))
      MoveTier(1, +1);
    if (KeyEdge(VK_F5, &p5)) {
      RefreshTeam("F5 summon", false);  // 每次召唤前现读队号，避免换局后还用旧值
      QueueSummon(1, false, 0);
    }
    if (KeyEdge(VK_F6, &p6)) {
      InterlockedExchange(&g_hud_show, g_hud_show ? 0 : 1);
      InterlockedExchange(&g_hud_dirty, 1);
      Log("[KEY] F6 HUD %s", g_hud_show ? "shown" : "hidden");
    }
    if (KeyEdge(VK_F7, &p7))
      MoveSel(2, -1);
    if (KeyEdge(VK_F8, &p8))
      MoveSel(2, +1);
    if (KeyEdge(VK_F9, &p9))
      MoveTier(2, -1);
    if (KeyEdge(VK_F10, &p10))
      MoveTier(2, +1);
    if (KeyEdge(VK_F11, &p11)) {
      RefreshTeam("F11 summon", false);
      QueueSummon(2, false, 0);
    }
    Sleep(30);
  }
  return 0;
}

// ===========================================================================
//  生成探针挂 4 个函数。四个的职责见下面那张表（四个都带非诊断的活，一个都不能少装）。
// ===========================================================================
//
//  -- 两个关键的被挂函数 --
//
//  sub_141D6CFD0（RVA 0x1D6CFD0）= ctx 的虚方法 vt[232]，不是"玩家的生成函数"。
//    证据是 xrefs_to 只有 2 个数据 xref（0x143740BE0 / 0x14572C4E0）；
//          ctx vtable 0x143740AF8 + 0xE8(232) == 0x143740BE0 精确对上；
//          工厂 sub_141D52090 里 *(v5) = &off_143740AF8（984 字节，+840 清 0）。
//    AI 批量生成器 sub_141DCBD10 调 ctx->vt[232](ctx, v58) 是它，
//    玩家出生 sub_141D6D9B0 内部调的是同一个槽，两边差别不在这个函数里。
//    它做的是 v7 = sub_140F4EF30(ctx, req)（造对象+登记），
//          v9 = sub_141CD9220(v7)（类型过滤：是 ServerPVZCharacterEntity 才返回），
//          再读完 req 做角色/血量/行为树设置。
//
//  sub_140F4EF30（RVA 0xF4EF30）= 造对象 + 登记进 ctx：
//      v20 = *(u64*)sub_140F16220(...)      造出对象
//      sub_140F86C90(v20, ctx+136)
//      ctx+400/408/416 是 vector<ptr>，push_back(v20)
//      if (req+92 == -1) ++*(u32*)(ctx+244)    成功后 ctx+244 才递增
//      *(ctx+432) = *(v20+40)
//    它还对 ctx 调了 6 个虚方法（+256/+280/+288/+296/+304/+376），所以 ctx 必须是完全
//    初始化过的实例，只能用现成的。
//    两道会 return 0 的闸门：A 看 ctx+552/req+72，B 看 ctx+244 与上限。
//    注入失败先看这两条，这也是梯子里 ATT_RESET_ROT（把 ctx+244 清零让 B 放行）的理由。
//
//  -- 4 个 hook 的职责（改这里之前先看代码，别照抄）--
//     SPAWN    功能必需，抓 AI 路径的真实 req（调用者 RVA == 0x1DCC232）写进 g_reqCopy，
//                        顺手把载体 g_aiCtx 定成 AI 路径实测值。Ops 那些没有 AI 生成
//                        事件的关卡则退回自己构造的 req（BuildReq）。
//     WORLD    功能必需，抓 holder 底板（ctx+840，g_holderCopy）。
//                        这是"载体退路"唯一的来源，别处抓不到：SPAWN 调用后
//                        ctx+840 已经被清 0 了。附带 (ctx,entity) 配对给 RoleWatch。
//     DISPATCH 功能必需，注入的执行点就在这里
//                        （消费 g_pendingSummon 后取走 g_queuedV21 再调 DoSummonSpawner，见那里）。
//                        必须在游戏线程上跑，见那个函数里的说明。
//     TRYSPAWN 功能必需，在入口记 playerPath。那是 IsOwnSpawnPoint 唯一的数据来源，
//                        ScanPool 挑载体、ValidCarrierCore 判否、WorldInsertDetour 的来源闸
//                        三处都读它，所以少装这一个会让那三条判据静默失效，
//                        而且 HooksReady 会判否、F5/F11 当场报 FAIL。其余是原样转发。
//
//   四个 detour 都有 dump 日志开关（硬编码的 g_probe_enabled，见定义处；要关就重编译）。
//
//  用 MinHook 挂钩（vendored 在 third_party/minhook，BSD-2-Clause），它负责指令长度
//  解码 + 跳板重定位，所以换挂别的函数只要换 RVA。
//  下面的 k*Prologue 只用作解密门禁：由于 exe 加了壳，磁盘上的代码保持加密，必须等目标处
//  真的是这些字节才说明已解密。四个指纹都和真实二进制逐字节比对过，不要求
//  落在指令边界（MinHook 自己会解码）。
//
// ---------------------------------------------------------------------------
constexpr unsigned SPAWN_PROBE_RVA = 0x1D6CFD0;
constexpr int SPAWN_PROBE_GATE_LEN = 14;

// sub_140F4EF30（造对象 + 登记进 ctx 的那条路，完整说明见上面那张钩子表）。
//   它响不了不等于游戏没在造 AI：Ops 里敌人刷怪不走 ctx->vt[232] 这条路，WORLD 会一次都不响。
//   所以它的沉默不能当否证用。
constexpr unsigned WORLD_INSERT_RVA = 0x0F4EF30;
constexpr int WORLD_GATE_LEN = 16;

static const unsigned char kProbePrologue[SPAWN_PROBE_GATE_LEN] = {
    0x48, 0x8B, 0xC4, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};

// 只当"解密了没有"的指纹用，不要求落在指令边界（理由见上面那段）。
static const unsigned char kWorldPrologue[WORLD_GATE_LEN] = {
    0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC};

// sub_141DCBD10，上层的"批量挑选/生成"调度器。
// a1 看起来是全局单例（"它是候选池"那条假设已实测证伪，见 ScanPool 段），
// 抓它是为了拿常驻的 ctx。
constexpr unsigned DISPATCH_RVA = 0x1DCBD10;
constexpr int DISPATCH_GATE_LEN = 16;

// sub_141D6D9B0(player, ctx, bool)：
//     它自己带队伍判定（ctx+552 非0 && ctx+576 == player+88），
//     判定过了就调 ctx->vtable[29] 生成，返回成功与否。
// hook 它就能同时拿到玩家指针和 ctx 指针，这两样正是我们最缺的。
constexpr unsigned TRYSPAWN_RVA = 0x1D6D9B0;
constexpr int TRYSPAWN_GATE_LEN = 16;

static const unsigned char kDispatchPrologue[DISPATCH_GATE_LEN] = {
    0x40, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x80, 0xBF};
static const unsigned char kTrySpawnPrologue[TRYSPAWN_GATE_LEN] = {
    0x48, 0x8B, 0xC4, 0x41, 0x56, 0x48, 0x81, 0xEC, 0x20, 0x01, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24};

// 不要去 hook ctx 的 vt[456]（sub_141D5EF60，RVA 0x1D5EF60）来"把载体装成野怪"。
//
//   vt[456] 的三个取值都不能正常显示。sub_141D62790（vt[280]）拿它分流：
//       0   走 vt[264] = sub_141D6AFE0 且 a3 = 0，记录那一半不走、也不分配槽位，
//             实体被从 ctx 的实体数组里摘出来、挂回 ctx+472..488 的待生成向量
//             （sub_140F4DB30 里 !a3 那一支），实测表现是"日志报成功、世界里什么都没有"
//       2   跳过 LABEL_35 直接放置。实测会有出土动画、会把玩家挤开，
//             但模型不出现，而且收尾那句 sub_140F55B20(主人, 实体) 被跳过
//       其他情况走 sub_140F56050 分配一个玩家槽位，这才是能正常显示的那条路
//   也就是说"不占玩家槽位"和"能正常显示"在 vt[456] 上没有交集。
//
//   还有两个坑。
//     一是 sub_141D5EF60 第一句就读 ctx+840（v2 = (a1[105] == 0)），
//       而 ctx+840 正是我们塞假 holder 的那一格，所以注入期间它给的值
//       本来就是被我们改过的，不是那个 spawner 的真实配置；
//     二是 req+80 那条 a7 链也会算出一个 v15（v14 非 0 时用 *(v14+16)），
//       而我们把 req+80 清成 0 了，所以链算不出东西、vt[456] 的返回值才是生效值。
//
//   要解决槽位问题得从分配器入手（sub_140F56050），别再从这一格想办法。

// sub_141D6CFD0 静态反编译里的"生成配置"链路，
//     整条链上第一次同时出现"角色"和"分人数血量"的地方，比 spawnRef 更直接：
//         v9  = sub_141CD9220(entity)         // 类型过滤：是 ServerPVZCharacterEntity 才返回自身
//         v10 = *(v9 + 600)                   // 生成配置的上级对象
//         v21 = *(v10 + dword_143FBCDB0 + 6040)   // 生成配置对象（第三优先来源）
//         v47 = v21[6] ? v21[6] : spawnRef+528    // 交给 sub_141D6B640(挂载点, v47)
//         *(v46+2168) = v21[5]    // 写进实体的 +2168（≠"角色路径"；角色路径在 v21[3]）
//         *(v46+2176) = v21[10]               // 变体
//         *(v46+3292) = clamp(spawnRef+552, -1, 2)
//         v23 = v21[11]; 血量 = v23[人数-1] 或 *(float*)(ctx+840+88)  // +88 优先级更高
// WORLD detour 里同时握着 ctx(=mgr) 和 entity(=返回值)，一次就能把两边钉在一起。
constexpr unsigned RVA_DWORD_FBCDB0 = 0x3FBCDB0;  // dword_143FBCDB0，运行时才填的字段偏移
// spawner ctx 上的几个关键偏移。只在 ctx 上成立，别拿同一个数去指别的对象：
//   同一个 +48 在 holder 上是 Template、在 player 记录上是别的字段。
constexpr int CTX_SPAWNCOUNT_OFF =
    244;  // 已生成计数，同时被当 slot 索引用（双重身份见 SummonWithTemplate）
constexpr int CTX_GATE_OFF = 552;    // 游戏用它启停这个 spawner（低字节非 0 = 启用）
constexpr int CTX_TEAM_OFF = 576;    // 队号（游戏自己的闸门就是拿它比 player+88）
constexpr int CTX_HOLDER_OFF = 840;  // 模板持有者槽：注入窗口唯一写的那一格
// 上面那个字段块里，角色链第三来源那个指针字段的偏移：
//     *(对象 + dword_143FBCDB0 + 6040)
//   三处代码读它（生成路径取生成配置对象、Worker 体检比角色、ScanPool 比主人记录里的角色），
//   别各写一个裸数字。上面那段链路注释是 IDA 原文，里面的 6040 照抄不动。
constexpr unsigned ROLE_SRC3_OFF = 6040;
constexpr unsigned RVA_CAST_CHARACTER =
    0x1CD9220;                                   // sub_141CD9220，"是不是 ServerPVZCharacterEntity"
constexpr unsigned RVA_PLAYER_CLEAN = 0xF5CE00;  // sub_140F5CE00，摘记录的前一步
// 读那个"运行时才填的字段偏移"本身（dword_143FBCDB0）。四处调用点里，一处报错返回、
//   两处并进外层条件、一处读不到就按 0 处理，语义判断留在调用点。
static bool ReadFieldOffset(unsigned int *offOut) {
  if (!offOut)
    return false;
  *offOut = 0;
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base)
    return false;
  return Rd32((unsigned long long)(uintptr_t)(base + RVA_DWORD_FBCDB0), offOut);
}
constexpr unsigned RVA_PLAYER_REMOVE = 0xF56720;  // sub_140F56720，从池子摘掉 + 清槽位 + free

// 移除一个 AI 玩家会用到的那对函数：
//     sub_140F5CE00(player)        player+528 = 0，再进 sub_140F5F130
//     sub_140F56720(mgr, player)   从池子向量里摘掉、清位图、清槽位指针、清 per-id 记录，
//                                 最后把玩家对象 free 掉
// 这一对不足以干净地拆掉一个单位：它只销毁玩家记录，不销毁 pawn 实体。
//   游戏自己的四处调用点全都额外做了实体侧的动作，唯一只做这一对的那处是关卡卸载时
//   整个 ServerPlayerManager 重置。删单位要连实体侧一起做。
// 用法见 RecycleOldestTeamUnit：只有在"走完游戏自己的入口、记录却还留在池子里"时，
//   才拿它俩补上游戏没做的那一半（实体侧那半已经做过了）。
//
// 但持久 AI 不能拿这一对去摘，引擎给它们单独留了一条路。
//   sub_141D6AFE0（拆实体，回收时调的就是它）里摘记录那半带守卫
//     if (*(rec+72) && (*(rec+128) & 2) == 0)
//   bit1 = 持久 AI，所以持久 AI 走这条路时实体照拆、记录永远不被摘，留在池子里占着槽位。
//   引擎给持久 AI 准备的入口是另一条，守卫正好反过来：
//     sub_141DB4540(管理器, 记录, 连记录一起摘, 离队事件模式)
//   里面 (槽位表[槽位]+128 & 2) != 0 才摘。
//   它比手搓那一对多做一步 sub_141DB4300：拿记录的 +520/+448 两个实体做组件查表，
//   再调 sub_141DB0920 把实体从管理器的实体表（管理器 +64..+72）里摘掉、
//   把每个队的 AI 计数（管理器 + 4*队号 + 1220）减一。少了这一步，记录是 free 了，
//   管理器那张表还指着已经拆掉的实体。
//   管理器的单例是 qword_14469C858，不是玩家管理器那个 qword_1445C4410+176。
// 两个参数照抄引擎自己的调用点。批量调度器 sub_141DCBD10 里是 1 / 0，
//   批量移除那条 sub_141DB45F0 用的是 1 / 3。我们跟单条那条走，取 1 / 0。
constexpr unsigned RVA_MGR_ENTITY = 0x469C858;  // qword_14469C858，实体/spawner 管理器单例
constexpr unsigned RVA_REMOVE_PERSIST =
    0x1DB4540;  // sub_141DB4540，摘持久 AI 的记录（含实体侧收尾）

typedef __int64(__fastcall *SpawnFn)(void *ctx, void *req);
typedef __int64(__fastcall *WorldInsertFn)(void *mgr, void *req);
typedef __int64(__fastcall *DispatchFn)(void *a1);
typedef __int64(__fastcall *TrySpawnFn)(void *player, void *ctx, char a3);
// ctx 的 vt[57]（槽号 456/8，sub_141D5EF60）：SummonWithTemplate 与 RoleWatch 两处都读它。
typedef long long(__fastcall *VtFn)(void *);
// sub_141CD9220（RVA_CAST_CHARACTER）：问"这个对象是不是角色实体"。
//   两处调用点统一用指针签名：x64 上 void* 与无符号长整型的 ABI 相同。
typedef void *(__fastcall *CastEntityFn)(void *);
// sub_141DA9460（RVA_CAST_APPEARANCE）：同一个套路的另一个目标函数。
typedef unsigned long long(__fastcall *CastAppearanceFn)(unsigned long long);

static SpawnFn g_spawn_orig = nullptr;
// （不要为 sub_141D62790 再搭跳板，也不要缓存"玩家生成变换"，
//   理由见 WorldInsertDetour 上面那段注释。）
static WorldInsertFn g_world_orig = nullptr;
static DispatchFn g_dispatch_orig = nullptr;
static TrySpawnFn g_tryspawn_orig = nullptr;
// （四个 hook 的命中计数是各自 detour 里的函数内静态。用到的只是原子自增的返回值，
//   用来排"前 N 次 dump"的配额，没人读它们的值，不必占文件作用域的名字。）
static volatile LONG g_probe_abort = 0;  // 确定性硬错误置 1，别再重试（和"还没解密"区分开）
// 暂时性挂钩失败（MinHook 分配不到跳板内存、改页保护没做成）的累计次数。
//   它不置 g_probe_abort：那种错误下一轮可能就好了，见 InstallOneHook 的返回约定。
static volatile LONG g_hookTransientN = 0;

// 见前面的前向声明。四个钩子缺一个都不该让按键静默无效；各自的来源与职责见上面的钩子表。
//   宁可整个功能明显不工作（HUD 上当场报出来），也不要留一个能召唤、
//   却可能把玩家弄没的中间状态。
static bool HooksReady() {
  return g_spawn_orig && g_world_orig && g_dispatch_orig && g_tryspawn_orig;
}
// 这两个是硬编码的，要改就重编译。
static int g_probe_enabled = 1;     // 0 = 不打那些 dump 日志（钩子照装，召唤不受影响）
static int g_probe_max_dumps = 30;  // 每个探针各 dump 前若干次，免得刷爆日志

// spawner 切换检测只在"这次用的 spawner 和上一次不一样"时才做完整 dump。
//    WORLD 有几百次调用，但绝大多数是同一个环境小怪（spawnRef+24 恒定），
//    全打出来只会淹掉日志、还会把样本预算吃光。过滤之后日志里只剩"spawner 切换点"。
//    它过滤的是"哪个 spawner 在干活"，不是"造的哪个角色"：
//    spawnRef+24 是 spawner 自己的 ID（实测同一个值先后生成过 AIChomper /
//    AIWeed_Pumpkin / AISunflowerQueen）。
static volatile LONG64 g_last_role = 0;
// 上限取 80。Ops 一波一波的敌人种类很容易超过这个数，超了就不再细打等于白采一轮。
//   万一那个值每次都变（那它就不是稳定的 spawner 标识），也别刷屏。
//   80 次 × ~80 行 ≈ 几千行，日志文件也就几百 KB。
constexpr int ROLE_DUMP_MAX = 80;
// 换关那一刻逐节点 dump 的上限：每行一个节点，两个模式各进一次之后直接对日志。
//   这个上限数的是"描述符正确的 spawner 节点"，不是整条链的节点数，
//   所以别拿"一关有多少个节点"来核它。
//   超过就不再打，免得某个模式真出几百个节点把日志撑爆。
constexpr int SPAWNER_DUMP_MAX = 64;

// v21 的读取时机需要留意。
//     SPAWN(ctx,req) 内部第一件事就是调 WORLD=sub_140F4EF30(ctx,req) 拿到 entity，
//     但那一刻 entity 是刚建出来的，*(entity+600) 很可能还没初始化，
//     在 WORLD detour 里算 v21 会扑空。
//     sub_141D6CFD0 整条链跑完要到 SPAWN 返回。所以：WORLD 里把 (ctx,entity) 存下来，
//     SPAWN 的"调用后"再算一次 v21，那时才是完整的。
//     只影响探针读数，不影响游戏（我们只读）；即便多线程交错导致配对错，
//        最坏结果也只是那份 dump 没用，不会崩。
static volatile LONG64 g_pair_ctx = 0;
static volatile LONG64 g_pair_entity = 0;
static volatile LONG g_role_seq = 0;  // spawner 轮换序号：SPAWN 用它判断"这次是不是换了 spawner"

// "不用打一局 Ops 也能拿到数据"靠两个主动扫描实现：
//     数据本来都靠生成事件触发才拿得到，但角色表是关卡里常驻的，
//     不依赖任何生成事件，Ops 里也能扫出来。
// 真正管用的是（见下面 ScanPool / ScanTemplates）：
//        - ScanTemplates  扫 EBX 资产区，枚举所有已加载的 Template 实例= 全部角色
//        - ScanPool       调 sub_141D5BE30 拿玩家 ctx，再顺链表列出全部 spawner
//
//  a1 的地址确实恒定（实测 30 次都一样，来自 mov rcx,[r14+480h]），但实测
//     它那个数组里只有 1 个元素、而且不是 spawner（虚表 0x143747FA0、+08 是 1.0f）：
// 它不是候选池，遍历它拿不到角色列表。
//
//  一条不变量，绝不要改任何"借来的 ctx"的内存，除非是在 SummonWithTemplate 里、
//     而且进出都成对还原 ctx+840。改错 ctx 会让玩家从世界里消失。

// 过了样本上限之后每 N 次打一行进度。
//   DISPATCH/TRYSPAWN 可能比预想的"热"得多（万一每帧都跑），
//   那 30 个样本会在走到目标场景之前就被前面的噪声吃光，那就是白测一轮。
//   有了这行进度，即使详细样本用尽，也能从日志时间戳看出"这个钩子还在响、大概什么频率"。
//   它必须是 Log 而不是 LOG_DEV：下面那句"progress only from now on"两种构建里都会打，
//   而要看日志的人拿到的正是发布构建的日志，心跳被编掉的话那句话就成了空头支票。
static void LogProgress(const char *tag, LONG n) {
  if (g_probe_enabled && n % 200 == 0)
    Log("[%s] call #%ld (detail quota used up, progress only)", tag, n);
}

// 样本配额用尽之后的两支：第一次用尽报一句，之后按 LogProgress 的节奏报进度。
//   四个 detour 的这两支一模一样，抽在这里，改一处就是改四处。
//   两支都归 g_probe_enabled 管：那一位为 0 的约定是"只关 dump 日志"，这一句也是 dump 日志。
static void ProbeQuotaTail(const char *tag, LONG n) {
  if (!g_probe_enabled)
    return;
  if (n == (LONG)g_probe_max_dumps + 1)
    Log("[%s] (enough samples, progress only from now on)", tag);
  else
    LogProgress(tag, n);
}

// 四个 detour 入口共用的样本配额：原子自增自己的计数，返回"这一发要不要详 dump"。
//   计数器由调用方以 static 局部量持有，一个钩子一份，互不干扰；序号从 nOut 带出来。
//   为什么必须带上 g_probe_enabled：那一位为 0 的约定是"只关 dump 日志"、钩子照装，
//   判据漏了这一句，钩子就会在只想关日志的时候照打。
//   各自的日志不搬进来：那种变参转发的包装会把格式串和实参分家。
static bool ProbeNextDump(volatile LONG *hits, LONG *nOut) {
  const LONG n = InterlockedIncrement(hits);
  if (nOut)
    *nOut = n;
  return g_probe_enabled && (n <= (LONG)g_probe_max_dumps);
}

// ---------------------------------------------------------------------------
//  参数转储
//
//  记下调用者地址（RVA），这是静态分析查不出来、而我们最缺的一块：
//    挂钩的这几个函数都是被虚表间接调用的，光看代码不知道谁在调。
//    有了 RVA 就能回 IDA 直接查调用点（sub_141D6CFD0 的 AI 路径调用者
//    0x1DCC232 就是这么认出来的，那是 req 复制的判据）。
//
//  读的全是静态分析推出来的偏移。偏移本身没错（游戏自己也读同样的位置），
//    但万一那一刻链子不成立，解引用野指针
//    就是访问违例、会把游戏打闪退，所以每一段都各自包在 SEH 里，一个都不裸奔。
//
//  每段各自包一层 __try：__try 嵌在循环里容易踩 MSVC 的范围表/C2712，
//  拆开之后一处偏移猜错只丢那一处的日志，不会把整条 dump 带走。
// ---------------------------------------------------------------------------

// 把地址换算成 RVA 打出来，带模块基址的裸地址回 IDA 对不上，RVA 才有用
static void LogCaller(const char *tag, void *caller) {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  Log("[%s]   caller = %p   RVA 0x%llX", tag, caller,
      (unsigned long long)((unsigned char *)caller - base));
}

// ctx 是有虚表的对象，*(void**)ctx 就是它的 vtable。
//    静态分析认定 sub_141D6CFD0 是"虚表 0x143740AF8 的第 29 槽"，一比就知道对不对。
//
//    从 sub_141D6D9B0 反推出来的判定字段（都长在 ctx 上）：
//        ctx+552  一个 bool，必须非 0
//        ctx+576  一个 dword，和玩家对象的 +88（队号）比较
//      这就是"这个生成器该不该为这个玩家生成"的判据，一并打出来。
static void DumpCtx(const char *tag, void *ctx) {
  __try {
    Log("[%s]   *ctx(vtable) = %p      (static analysis expects 0x143740AF8)", tag, *(void **)ctx);
    Log("[%s]   ctx+552(team-check bool) = %u      ctx+576(team) = %u", tag,
        (unsigned)(*(unsigned char *)((unsigned char *)ctx + CTX_GATE_OFF)),
        *(unsigned int *)((unsigned char *)ctx + CTX_TEAM_OFF));
    // ctx+244 是 sub_140F4EF30 里选"造哪个变体"的索引；req+92 == -1 时每生成一次 +1
    Log("[%s]   ctx+244(rotation index) = %u (low 7 bits=%u)", tag,
        *(unsigned int *)((unsigned char *)ctx + CTX_SPAWNCOUNT_OFF),
        *(unsigned int *)((unsigned char *)ctx + CTX_SPAWNCOUNT_OFF) & 0x7F);
    // ctx+840（= 反编译里的 v3[105]）是取 v21 的第一优先来源：
    //       若它非 0，v21 = *(ctx+840 + 48)，根本轮不到 entity 那条路。
    //     它只在两次生成之间是 0：注入时我们故意把它指向假 holder
    //        （见 SummonWithTemplate），生成期间游戏自己会把它指向真的 AISpawnData。
    // 这个指针应该指向一个 AISpawnData 运行时实例（ebx×2 规律对上：
    //   +48 Template / +56 Blueprint / +64 VisualGroups / +80 BTParams / +88 OverrideMaxHealth /
    //   +92 Team / +96 SnapToGroundDistance）。
    //   把 +48/+56/+88/+92 一起打出来就能当场核对这个判断。
    {
      void *h = *(void **)((unsigned char *)ctx + CTX_HOLDER_OFF);
      Log("[%s]   ctx+840(=v3[105], primary source of v21) = %p", tag, h);
      if (h) {
        unsigned int t92 = 0, t96 = 0;
        Rd32((unsigned long long)(uintptr_t)h + 92, &t92);
        Rd32((unsigned long long)(uintptr_t)h + 96, &t96);
        Log("[%s]     it (the AISpawnData instance) +48(Template)= %p  +56(Blueprint)= %p  "
            "+64= %p  +80= %p  +88(OverrideMaxHealth)= %.1f  +92(Team)= %u  "
            "+96(SnapToGroundDist)= %u",
            tag, *(void **)((unsigned char *)h + 48), *(void **)((unsigned char *)h + 56),
            *(void **)((unsigned char *)h + 64), *(void **)((unsigned char *)h + 80),
            (double)*(float *)((unsigned char *)h + 88), t92, t96);
      }
    }

    // ctx+48 静态分析里当"生成参考数据"用（+528 行为树 / +536 模板）。
    // 实测那两处一直是 0（调用后也是），这条路径确实不用 SpawnTemplate。
    //    同时把 spawnRef 的 8 个 bool（+584..+591）也打出来，其中一个（+588
    //    IsExclusiveToActivePlayers）正是上面那个判定用到的。
    void *sub = *(void **)((unsigned char *)ctx + 48);
    Log("[%s]   ctx+48=%p", tag, sub);
    if (sub) {
      const unsigned char *b = (const unsigned char *)sub;
      Log("[%s]   [ctx+48] vtable=%p   +528=%p   +536=%p", tag, *(void **)sub,
          *(void **)((unsigned char *)sub + 528), *(void **)((unsigned char *)sub + 536));
      Log("[%s]   [ctx+48] bools 584..591 = %u %u %u %u %u %u %u %u   (+588 = "
          "IsExclusiveToActivePlayers)",
          tag, (unsigned)b[584], (unsigned)b[585], (unsigned)b[586], (unsigned)b[587],
          (unsigned)b[588], (unsigned)b[589], (unsigned)b[590], (unsigned)b[591]);
      // +536(SpawnTemplate) 实测一直是 0，但"造什么角色"不是从 spawnRef 来的。
      //   角色在 v21（PVZCharacterSpawnTemplateData 实例）的 +24 处，
      //   而 v21 = *(ctx+840 + 48)。spawnRef 这边只有 spawner 自己的 ID（+24）。
      // 下面这行给的是 spawner 自己的 ID，不是角色（那条实测见上面 g_last_role 那段）。
      //     真正的角色在 v21[3]（Template 路径），链路是 ctx+840，+48，v21[3]。
      //     它和"生成出来的对象 +24"也不相等（那是恒定的 0x223321F）。
      //     留着只为对照，不要再当角色字段用。
      Log("[%s]   [ctx+48] +24(spawner ID, not a character) = 0x%016llX      "
          "+08(type descriptor)=%016llX",
          tag, *(unsigned __int64 *)((unsigned char *)sub + 24),
          *(unsigned __int64 *)((unsigned char *)sub + 8));
      // 解引用 +24 看看它指向什么。已确认 +24 是个恒定的 32 位 ID（不是角色字段），
      // 所以这次解引用多半会直接抛异常（+24 是常量 ID，当地址用基本是未映射）。
      // 后果不止"这行日志不打印"：本函数只有一层 __try（见函数开头），异常会一路跳到末尾的
      //    __except，于是下面那段 0x00..0x7F 的 qword dump 也一起丢掉，
      //    并且打出 "ctx is not the expected object"，那句在这里是误报，ctx 本身是好的。
      {
        void *bp = *(void **)((unsigned char *)sub + 24);
        if (bp) {
          Log("[%s]   [ctx+48] +24 deref, vtable=%p", tag, *(void **)bp);
          // 名字别叫 b，外层作用域已经有一个 b 了（/W4 的 C4456 遮蔽告警）
          const unsigned __int64 *qw = (const unsigned __int64 *)bp;
          Log("[%s]     [+24]+00..+3F: %016llX %016llX %016llX %016llX", tag, qw[0], qw[1], qw[2],
              qw[3]);
          Log("[%s]     [+24]+40..+7F: %016llX %016llX %016llX %016llX", tag, qw[4], qw[5], qw[6],
              qw[7]);
        }
      }

      const unsigned __int64 *q = (const unsigned __int64 *)sub;
      // 每行 4 个 qword = 32 字节，所以范围是 0x00..0x1F / 0x20..0x3F / …（不是 0x40 步进）
      Log("[%s]   [ctx+48] +00..+1F: %016llX %016llX %016llX %016llX", tag, q[0], q[1], q[2], q[3]);
      Log("[%s]   [ctx+48] +20..+3F: %016llX %016llX %016llX %016llX", tag, q[4], q[5], q[6], q[7]);
      Log("[%s]   [ctx+48] +40..+5F: %016llX %016llX %016llX %016llX", tag, q[8], q[9], q[10],
          q[11]);
      Log("[%s]   [ctx+48] +60..+7F: %016llX %016llX %016llX %016llX", tag, q[12], q[13], q[14],
          q[15]);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("[%s]   !! ctx read failed (ctx is not the expected object)", tag);
  }
}

// 盯生成出来的那个对象，参数缓冲区看不到时，结果对象本身就能说明这是什么。
//     返回值的头 8 字节就是它的虚表，拿回 IDA 一比就知道生成的是什么类型。
//
//     角色不在返回对象的 +24，也不在 spawnRef+24（见 DumpCtx 那段）。
// 角色真正在 v21[3]（Template 资产路径），链路 ctx+840，+48，v21[3]。
//     同时把 +0/+8/+16/+32 也打出来，方便对照结构。
static void DumpResult(const char *tag, void *ret) {
  __try {
    if (!ret) {
      Log("[%s]   returned null (nothing spawned this time)", tag);
      return;
    }
    Log("[%s]   returned object = %p   its vtable = %p", tag, ret, *(void **)ret);
    const unsigned __int64 *q = (const unsigned __int64 *)ret;
    // 这个字段不是角色，实测 814 次样本里永远是 0x0223321F（含环境小怪和玩家自己
    //    的两次召唤），是个常量 ID，和"造了什么"无关。留着只为对照，别再看它。
    Log("[%s]   +24 = 0x%016llX  (measured constant ID, not a character)", tag, q[3]);
    Log("[%s]   +00=%016llX +08=%016llX +16=%016llX", tag, q[0], q[1], q[2]);
    Log("[%s]   +32=%016llX +40=%016llX +48=%016llX +56=%016llX", tag, q[4], q[5], q[6], q[7]);

    // 解引用 +24 看看它指向什么。已知 +24 是个恒定的 32 位 ID（不是角色），
    // 所以下面那次解引用多半会直接抛异常（+24 是常量 ID，当地址用基本是未映射），
    //    被外面那层 __try 吞掉，这行"解引用"日志通常不会打印。别指望它。
    void *bp = (void *)q[3];
    if (bp) {
      Log("[%s]   +24 deref, vtable=%p", tag, *(void **)bp);
      const unsigned __int64 *b = (const unsigned __int64 *)bp;
      Log("[%s]     [+24]+00..+3F: %016llX %016llX %016llX %016llX", tag, b[0], b[1], b[2], b[3]);
      Log("[%s]     [+24]+40..+7F: %016llX %016llX %016llX %016llX", tag, b[4], b[5], b[6], b[7]);
    }
    // 同时把 +600 也打出来，sub_141DCBD10 里生成成功后就是读返回对象 +600 的
    Log("[%s]   returned object +600 = %p", tag, *(void **)((unsigned char *)ret + 600));
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("[%s]   !! reading the returned object failed", tag);
  }
}

// 诊断用，这个 8 字节像不像一个指针（只是范围启发式，不是 HeapPtr）。
//   两个区间都要认，低地址那一带（0x10000~0x80000000，BFN 的 EBX 资产实例都在这）
//   和模块映像自己的地址（0x140000000 起，vtable / 类型描述符那种）。
//   只认第一个区间的话，模块指针会被漏标，读日志的人会把它当数据。
//   几个 dump 点共用这一个判据，改的时候一起改。
static bool LooksLikePtr(unsigned long long q) {
  return (q >= MIN_VALID_ADDR && q < 0x80000000ULL) || (q >= 0x140000000ULL && q < 0x146000000ULL);
}

// spawnRef 全量 dump，只打非 0 的 qword，对"像指针"的再试读 ASCII 字符串。
//
//   别从这里读角色，角色在 v21 的 +24 处（见 DumpCtx 那段）。留着是为了对照
//      "不同 spawner 之间还差什么"
//      （比如 +0x6C 那个 ±1.3f）。
//   不能整块打，76 个 qword 会把日志刷爆，所以跳过 0。
static void DumpSpawnRefAll(const char *tag, void *ref) {
  __try {
    const unsigned __int64 *q = (const unsigned __int64 *)ref;
    Log("[%s]   [spawnRef full] non-zero qwords at 0x000..0x25F (offset = EBX field name TBD):",
        tag);
    for (int i = 0; i < 0x260 / 8; ++i) {
      if (!q[i])
        continue;
      const bool ptrish = LooksLikePtr(q[i]);
      Log("[%s]   [spawnRef] +%03X = %016llX%s", tag, i * 8, q[i],
          ptrish ? "   (looks like a pointer)" : "");
      // 万一它是个字符串指针（路径/角色名），直接读出来，这比猜 ID 划算。
      //   ReadIdent 自带 SEH 且只认"首字符是字母、其余全是可打印 ASCII"，
      //   所以二进制数据不会误报。（跨函数调用它，不是嵌套 __try，合法。）
      char s[96];
      if (ptrish && ReadIdent(q[i], s, sizeof(s)))
        Log("[%s]   [spawnRef]        ^ string it points to = \"%s\"", tag, s);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("[%s]   !! spawnRef full dump failed", tag);
  }
}

// 打印一个"模板持有者"的头 0x80 字节，再把"像指针"的那些展开一层。
//   两个筛选口径别混：
//     第一层只做标记，用 LooksLikePtr（不解引用）；
//     第二层要真去解引用，所以必须是 HeapPtr（模块映像里的地址读不了）。
//   跳过全 0 的 qword（纯噪声）。
static void DumpHolderShort(const char *tag, unsigned long long holder) {
  for (int k = 0; k < 16; ++k) {
    unsigned long long q = 0;
    if (!Rd64(holder + 8ull * k, &q))
      break;
    if (q)
      Log("[%s]   [holder]  +%02X = %016llX%s", tag, k * 8, q,
          LooksLikePtr(q) ? "   (looks like a pointer)" : "");
  }
  for (int k = 0; k < 16; ++k) {
    unsigned long long q = 0, a = 0, b = 0;
    if (!Rd64(holder + 8ull * k, &q))
      break;
    if (q < MIN_VALID_ADDR || q >= 0x80000000ULL || !HeapPtr(q))
      continue;
    if (Rd64(q, &a) && Rd64(q + 8, &b))
      Log("[%s]   [holder]  +%02X, %p : +00=%016llX +08=%016llX%s", tag, k * 8, (void *)q, a, b,
          LooksLikePtr(a) ? "   (+00 looks like a pointer, maybe an array)" : "");
  }
}

// 取路径最后一段（最后一个 / 或 反斜杠 之后）。空指针返回空串。
//   资产路径只用 /，但 dump 里出现的东西不保证，所以两种分隔符都认。
static const char *BaseName(const char *path) {
  if (!path)
    return "";
  const char *last = path;
  for (const char *q = path; *q; ++q) {
    if (*q == '/' || *q == '\\')
      last = q + 1;
  }
  return last;
}

// Frostbite 数组在数据前面 4 字节存元素个数，最高位不属于长度（是别处的标志，要掩掉）。
//   指针为 0 或读不出来返回 false。三处读法共用这一份。
static bool ArrCountOf(unsigned long long arr, unsigned int *nOut) {
  if (nOut)
    *nOut = 0;
  unsigned int bits = 0;
  if (arr == 0 || !Rd32(arr - 4, &bits))
    return false;
  if (nOut)
    *nOut = bits & 0x7FFFFFFFu;
  return true;
}

// 生成配置链路（v21）。entity 由 WORLD detour 的返回值给出。
//     这里会调用游戏自己的函数 sub_141CD9220，它是纯类型检查（只读 vtable 和几个
//        全局 u16），签名就是 void*(void*)，调用是安全的；整段仍然套 SEH 兜底。
//     内部一律走 Rd32/Rd64（它们各自带 SEH），不要再嵌套 __try。
static void DumpRoleChain(const char *tag, void *ctx, void *entity) {
  __try {
    unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
    if (!base || !entity)
      return;

    unsigned int off = 0;
    if (!ReadFieldOffset(&off)) {
      Log("[%s]   [role chain] reading dword_143FBCDB0 failed", tag);
      return;
    }
    // off == 0 不一定是"没初始化"，那个字段的偏移本来就可能正好是 0，
    //    所以这里只提示、不中断（直接 return 会把合法情况也拒掉）。
    Log("[%s]   [role chain] dword_143FBCDB0 = %u (0x%X)%s", tag, off, off,
        off ? "" : "   is 0 (may be uninitialized, or it may just be 0)");

    CastEntityFn cast = (CastEntityFn)(base + RVA_CAST_CHARACTER);
    void *v9 = cast(entity);
    Log("[%s]   [role chain] v9 = sub_141CD9220(entity) = %p%s", tag, v9,
        v9 ? "" : "   (type mismatch, 0)");
    if (!v9)
      return;

    void *v10 = nullptr;
    if (!Rd64((unsigned long long)v9 + 600, (unsigned long long *)&v10) || !v10) {
      Log("[%s]   [role chain] *(v9+600) = null, stop", tag);
      return;
    }
    Log("[%s]   [role chain] v10 = *(v9+600) = %p", tag, v10);

    // v21 在反编译里是三级 fallback，三条路都要算出来才敢说走了哪条：
    //       1) ctx+840 非 0，v21 = *(ctx+840 + 48)，第一优先
    //       2) spawnRef+536 非 0，v21 = spawnRef+536
    //       3) 否则，v21 = *(v10 + off + 6040)（兜底那条）
    void *s1 = nullptr, *s2 = nullptr, *s3 = nullptr;
    void *tail = nullptr;  // ctx+840 的"模板持有者"，下面要 dump 它本身
    if (ctx) {
      if (Rd64((unsigned long long)ctx + CTX_HOLDER_OFF, (unsigned long long *)&tail) && tail)
        Rd64((unsigned long long)tail + 48, (unsigned long long *)&s1);
    }

    // 自造要 dump "模板持有者"本身的结构。
    //   ctx+840 在生成期间有值、两次生成之间是 0，它的 +48 就是当前选中的 v21。
    //   字段身份已经核对过（+48 Template / +56 Blueprint / +64 VisualGroups / +80 BTParams /
    //   +88 OverrideMaxHealth / +92 Team / +96 SnapToGroundDistance，见 DumpCtx 那段），
    //   这里要看的是 +48 之外那些还没认过的槽，留着以后要按模板直接造的时候查。
    if (tail) {
      Log("[%s]   [holder] first 0x80 bytes of ctx+840 = %p (template list / asset refs):", tag,
          (void *)tail);
      DumpHolderShort(tag, (unsigned long long)tail);
    }
    if (ctx) {
      void *ref = nullptr;
      if (Rd64((unsigned long long)ctx + 48, (unsigned long long *)&ref) && ref)
        Rd64((unsigned long long)ref + 536, (unsigned long long *)&s2);
    }
    Rd64((unsigned long long)v10 + off + ROLE_SRC3_OFF, (unsigned long long *)&s3);
    Log("[%s]   [role chain] v21 three sources: [1]ctx+840,+48 = %p   [2]spawnRef+536 = %p   "
        "[3]*(v10+off+6040) = %p",
        tag, s1, s2, s3);
    void *v21 = s1 ? s1 : (s2 ? s2 : s3);
    Log("[%s]   [role chain] effective: %s, v21 = %p", tag,
        s1 ? "1: ctx+840" : (s2 ? "2: spawnRef+536" : "3: v10+off+6040"), v21);
    if (!v21)
      return;

    // 这 12 个 qword 就是"角色 + 变体 + 数值"的全部来源，逐条打出来。
    const unsigned __int64 *c = (const unsigned __int64 *)v21;
    for (int i = 0; i < 12; ++i) {
      const bool ptrish = LooksLikePtr(c[i]);
      Log("[%s]   [role chain]   v21[%2d] (+%03d) = %016llX%s", tag, i, i * 8, c[i],
          ptrish ? "   (looks like a pointer)" : "");
      // v21[4] 在反编译里被当 C 字符串用（for(i=v21[4]; *i; ++i); = strlen 后
      //     再 sub_140306620 构造字符串），很可能就是角色名。其余槽位一并试，
      //     反正 ReadIdent 只认纯 ASCII，二进制不会误报。
      char s[96];
      if (ptrish && ReadIdent(c[i], s, sizeof(s)))
        Log("[%s]   [role chain]        ^ string it points to = \"%s\"", tag, s);
    }

    // v21[11] 是分人数血量数组：代码里是 v27 = v23[人数 - 1]，数组前 4 字节是 size。
    //   索引是非 AI 参与人数（1P~4P），不是难度（见上面血量那段）。
    unsigned int n = 0;
    if (ArrCountOf((unsigned long long)(uintptr_t)c[11], &n)) {
      Log("[%s]   [role chain] v21[11] value array size = %u", tag, n);
      for (unsigned int i = 0; i < n && i < 8; ++i) {
        union {
          unsigned int u;
          float f;
        } fu;
        fu.u = 0;
        if (!Rd32((unsigned long long)(uintptr_t)((const unsigned int *)c[11] + i), &fu.u))
          break;
        Log("[%s]   [role chain]     v21[11][%u] = %.4f", tag, i, fu.f);
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("[%s]   !! role chain exception (one of the pointers is bad)", tag);
  }
}

// ---------------------------------------------------------------------------
//  两个主动扫描，不需要生成事件触发，进关卡几秒就能跑完
//
//   ScanPool       从 sub_141D5BE30(player, 0) 拿到玩家自己的 ctx，再顺链表走一圈
//                  （节点在 ctx+144，next 在节点+0、prev 在节点+8），列出全部 spawner。
//                  必须排除"玩家自己的那个"，拿它当载体生成 = 玩家销毁重生成，
//                     人会直接从世界里消失（实测过两次）。
//
//   ScanTemplates  扫 EBX 资产区，找"某 8 字节 == 类型描述符 0x144DD99F0"的实例。
//                  依据是 v21 是一个 Template 一个共享实例，实测同一个 Template
//                  跨多次生成地址不变，一次就能列出所有已加载的角色，
//                  不依赖"游戏先刷出过它"（自造能走得通的前提）。
//
//  spawnRef 的判据是 *(ctx+48) 指向的对象，其 +8 == 0x144DD9980
//  （= PVZCharacterSpawnReferenceObjectData 的运行时类型描述符，已实锤）
//
//  下面两条路都不通（实测），取 ctx 直接调 sub_141D5BE30：
//     - 拿 sub_141DCBD10 的参数 a1 当候选池：实测证伪（数据见 ScanPool 段），
//       遍历它得不到角色列表。
//     - "ctx 指针 = *(player + dword_143FBCEE0 + 208) - 8"：实测那个位置读到 0，
//       语义和静态推断不符。要拿 ctx 就直接调 sub_141D5BE30（见 RVA_FIND_CTX）。
//     （dword_143FBCEE0 / dword_143FBCDB0 这两个运行时偏移在其它用途上仍然有效，
//       比如 sub_141D6CFD0 里的 v10 + dword_143FBCDB0 + 6040。）
// ---------------------------------------------------------------------------
constexpr unsigned RVA_TYPEDESC_SPAWNREF = 0x4DD9980;  // 0x144DD9980 - 0x140000000
// 这个偏移只用于诊断打印：ScanPool 失败时把 *(player+off+208) /
//    *(player+off+216) 打出来，帮判断该往哪查。
constexpr unsigned RVA_DWORD_FBCEE0 = 0x3FBCEE0;  // 0x143FBCEE0 - 0x140000000
// sub_141D5BE30(player, a2)，游戏自己的"找 spawner"函数。
//   a2 == 0 时直接返回玩家自己的 ctx（反编译里 a2==0 那条 return）。
// 用它拿 ctx：手工复刻它内部的偏移计算，算出来的那个位置实测是 0。
constexpr unsigned RVA_FIND_CTX = 0x1D5BE30;  // 0x141D5BE30 - 0x140000000

// 对象 +off 处存着一个字符串指针，把它读出来（资产路径、模板 DisplayName 都是这个形状）。
//   前置检查统一走 OutStrReady（理由见那一处）。
static bool ReadIdentAt(unsigned long long obj, unsigned int off, char *out, int n) {
  if (!OutStrReady(out, n))
    return false;
  if (obj == 0)
    return false;
  unsigned long long s = 0;
  if (!Rd64(obj + off, &s) || s == 0)
    return false;
  return ReadIdent(s, out, n);
}

// 把一个资产对象指针（holder 的 +48 模板 / +56 蓝图，或模板实例本身）解析成路径字符串。
//   对象 +24 是路径字符串指针，这一份读法全项目共用：ReadTplInfo、AdHpOf、
//   ScanTemplates、SummonWithTemplate 和载体特征表的 dump 都走它。
static bool ReadAssetPath(unsigned long long asset, char *out, int n) {
  return ReadIdentAt(asset, 24, out, n);
}

// 读某个 spawner 当前模板的信息（路径 + 血量数组 size）。
//
// 只在生成进行中才有效，实测 ctx+840（模板持有者）在两次生成之间是 0，
//    只有 sub_141D6CFD0 执行期间才有值。所以扫描（间隙里跑）读不到，生成时才读得到。
static bool ReadTplInfo(void *ctx, char *tplOut, int n, unsigned int *arrSizeOut) {
  if (arrSizeOut)
    *arrSizeOut = 0xFFFFFFFFu;
  if (!OutStrReady(tplOut, n))
    return false;
  unsigned long long tail = 0, v21 = 0;
  if (!ctx || !Rd64((unsigned long long)ctx + CTX_HOLDER_OFF, &tail) || tail == 0)
    return false;
  if (!Rd64(tail + 48, &v21) || !v21)
    return false;
  const bool ok = ReadAssetPath(v21, tplOut, n);
  if (arrSizeOut) {
    unsigned long long a11 = 0;
    unsigned int cnt = 0;
    if (Rd64(v21 + 88, &a11) && ArrCountOf(a11, &cnt))
      *arrSizeOut = cnt;
  }
  return ok;
}

// 静默失败是查不出问题的，每个失败点各打一次日志（用位掩码，不刷屏）。
static volatile LONG g_scan_diag = 0;
#define SCAN_DIAG_ONCE(bit, ...)                 \
  do {                                           \
    if (!(g_scan_diag & (1 << (bit)))) {         \
      InterlockedOr(&g_scan_diag, (1 << (bit))); \
      Log(__VA_ARGS__);                          \
    }                                            \
  } while (0)

// 枚举所有已加载的 Template 实例，这才是"不依赖 spawner"的正路。
//
//   依据有两条，都已实测。
//     1)  v21 是每个 Template 资产一个共享实例，同一个 Template 跨多次生成
//        地址不变（日志里 0x35D139F0 出现 4 次、另两个各 2 次，对应各自的 Template）。
//     2)  三个观测到的 v21 地址都落在 0x35D00000~0x36100000（EBX 资产区），
//        而 ctx/spawnRef 在 0xCC~0xCF、entity 在 0xDB，明显是另一个区域。
//
// 实例的 +8 是类型描述符 0x144DD99F0，所以在资产区里找"某个 8 字节 == 该描述符"
//     就等于找"一个 Template 实例"（它就在那个地址 - 8 处）。
//     这样一次就能列出全部已加载的角色，不用等游戏刷出来。
//
//   用 VirtualQuery 只扫已提交且可读的页，并且设总量上限，免得扫到
//      未映射区或把帧率拖垮。
constexpr unsigned RVA_TYPEDESC_TPL = 0x4DD99F0;  // 0x144DD99F0 - 0x140000000
// AISpawnData 的运行时类型描述符。
//
//   实测来源是一次成功的注入把 holder 的 +8 打了出来 = 0x144DD9910。
//   再用 IDA 核实它的身份，0x144DD9910 处是 {名字指针、哈希指针、16 字节哈希} 的结构，
//   顺着名字指针（+0，0x144DD75A0，0x14377E6C8）读出来的字符串正是 "AISpawnData"
//   （旁边 0x144DD9980 是 "PVZCharacterSpawnReferenceObjectData"、0x144DD99F0 是模板那个，
//     三个形状完全一致，同属一张描述符表。）
//
//   必须写死的原因，只靠"从抓到的 holder 的 +8 读"不行：
//     Ops 里敌人刷怪不走 sub_141DCBD10 到 ctx->vt[232] 那条路，于是 WORLD 一次都不响、
//     holder 永远抓不到，描述符永远是 0、扫描找不到任何 AISpawnData，卡在"没有材料"。
//     写死之后扫描不再依赖任何生成事件。
constexpr unsigned RVA_TYPEDESC_AISPAWNDATA = 0x4DD9910;  // 0x144DD9910 - 0x140000000

// BuffData 的运行时类型描述符，用于在扫描时认出已加载的 BuffData 实例。
//   三条独立证据（名字链、全 exe 唯一指针、注册函数同构）：
//     1) 0x144E321C0 +0 = 0x144E2C060，再 +0 是字符串 "BuffData"，形状与上面几个描述符同构；
//     2) 全 exe 只有一处把 0x144E2C060 当 8 字节指针存着，就是它；
//     3) 注册函数 sub_142E26C10 与 AISpawnData 的注册 sub_142D558D0 逐行同形。
constexpr unsigned RVA_TYPEDESC_BUFFDATA = 0x4E321C0;  // 0x144E321C0 - 0x140000000

// 补挂出生 buff 用到的三个引擎入口，全部逐条从反编译复读核对过。
constexpr unsigned RVA_GET_BUFF_COMP = 0x10C1D50;  // sub_1410C1D50(entity, &off_144DBDBC0)
// 上 buff 分两步，自己串：先跑效果，再登记进组件条目表。
//   只跑效果不登记，buff 跑完就没了（实测组件里查不到）。
//   这两步本来由 sub_141DD1190 串起来，但它进门先做一道"这个实体收不收 buff"的检查，
//   那道检查在我们这里会调到坏指针（实测），所以不经过它。
constexpr unsigned RVA_RUN_EFFECTS = 0x1DD47E0;  // sub_141DD47E0(comp, buffData, target, fallback)
constexpr unsigned RVA_REGISTER_BUFF = 0x1DD4330;    // sub_141DD4330(comp, ctx, target, mode)
constexpr unsigned RVA_SERVER_BUFF_KEY = 0x4DBDBC0;  // off_144DBDBC0，ServerBuffComponent 的组件键
// sub_141DD2EA0：在 buff 组件的条目表里按资产指针查找，返回下标，找不到返回 0xFFFFFFFF。
//   挂完之后用它复核，比猜实体身份可靠：它直接回答"这个 buff 在不在这个组件里"。
constexpr unsigned RVA_FIND_BUFF = 0x1DD2EA0;

// 拿"某个对象的世界变换"，游戏自己的通用访问器：
//     sub_141090120(node, out) { sub_1410878E0(*(u64*)(node+24), out); *(u32*)(out+60) |=
//     0xF0F0F0F; }
//   它沿 node 的父链把局部变换乘成世界变换写进 out（64 字节 LinearTransform），
//   位置恒定在 out+48。
//   返回的 64 字节正好就是"要写进 req+0..63"的东西，连 +60 的 0F0F0F0F 标记都该带上
//     （游戏自己填生成变换时就是这么填的）。
constexpr unsigned RVA_GET_WORLD_XFORM = 0x1090120;  // 0x141090120 - 0x140000000
// sub_141D03AF0(player + dword_143FBCDB0) = 从玩家对象拿玩家实体。
// （sub_141D6D9B0 里就是这么用的：v14 = sub_141D03AF0(a1 + dword_143FBCDB0)。）
constexpr unsigned RVA_PLAYER_ENTITY = 0x1D03AF0;  // 0x141D03AF0 - 0x140000000
// sub_141DB2E10，thunk，真身 sub_140F57F70（纯读，只遍历一个链表计数）。
// 见 ProbeDifficultyTier：它数的是非 AI 参与者人数，不是难度。
constexpr unsigned RVA_GET_DIFFICULTY = 0x1DB2E10;  // 0x141DB2E10 - 0x140000000
// sub_141D528B0(ptr)：Frostbite 那套类型判定（IsInstanceOf 一类），是则返回 ptr、否则 0。
//   用它验模板 +40 那个 Behaviour（AI 模板）：sub_141D6CFD0 里正是先过这一关，
//   过了才把 AI 参数套到实体上（sub_141DA9460），验不过就整段跳过，
//   实体只剩注册表 archetype 给的身体。
constexpr unsigned RVA_CAST_APPEARANCE = 0x1D528B0;  // 0x141D528B0 - 0x140000000
// sub_140F5CA10(ptr)：sub_141D6CFD0 开头拿它验 req+64（"玩家上下文"）是否可用。
//   纯检查，可以自己先调一遍看通不通，再决定要不要填 req+64。
constexpr unsigned RVA_OWNER_VALID = 0xF5CA10;  // 0x140F5CA10 - 0x140000000
// sub_141D62790 = ctx 的 vt[280]：把实体真正放进世界那一步
//   （从数组取 a2，sub_140F56050 造实体，sub_140EA8180 放置，sub_140F55B20 收尾）。
//   它开头那条 a7 对象链是崩溃高发处，见 enum ATT_RESET_ROT 后面那段的说明。
//   不要 hook 它去抓 a7 当"玩家位置"：它只在"把玩家放进世界"那一瞬开火，
//   拿到的是出生点快照、跟不上走动。

// 枚举结果是每个已加载的 Template 实例（= 一个可召唤的角色）
// 两张扫描表的容量。某次实机扫下来是 341 个模板、AISpawnData
// 比它多（这个数跟着关卡加载的资产走，不同关卡不一样），
//   900 是留足余量之后定的上限；两张表共用同一个数，改就一起改。
constexpr int SCAN_TAB_MAX = 900;
struct TplEntry {
  unsigned long long v21;
  char path[SUM_NAME_MAX];
};
static TplEntry g_tplTab[SCAN_TAB_MAX];
static volatile LONG g_tplN = 0;

// AISpawnData 实例表：{它用的模板、它自己}，同一次扫描顺带建起来。
//
//   必须要它，只把载体 holder 的 +48 换成目标模板是不够的，+56 还是载体那个角色的 Blueprint，
//   而 sub_141D5E0F0（vt[296]）会把 *(u64*)(holder+56) 返回给 sub_140F4EF30，
//   那个值最终进 v41[0]，sub_140F16220(...)，也就是实体是照 Blueprint 造的。
//   实测表现是豌豆射手的身体出来了（那来自模板的 CustomizeCharacterData），
//   但部件被挂到野生花的骨架/插槽上、名字对不上、没被正确蒙皮，
//   看起来就是带绿色闪烁片状物、不会跟着动、拉成碎片。
// 用目标模板自己的 AISpawnData 当 holder：+48/+56/+64/+72/+80/+92 天生自洽，
//     一个字段都不用改，Blueprint 自然也是豌豆射手的。
//
//   键 = AISpawnData 的 +48（就是它指向的模板），和 sub_141D6CFD0 取 v21 的位置一致。
struct AdEntry {
  unsigned long long tpl;  // 它用的模板（= 它的 +48）
  unsigned long long ad;   // AISpawnData 实例地址
};
static AdEntry g_adTab[SCAN_TAB_MAX];
static volatile LONG g_adN = 0;

// 扫描写这两张临时表，扫完再整份拷进 g_tplTab/g_adTab 并发布计数。
//
//   必须在扫完之后一次性发布，不能在开头就清计数：
//     开头清的话，那 ~600ms 里读方看到的是空表：FindOwnAd 查不到 AISpawnData
//     退回载体 holder，用错 Blueprint 与 AI 参数。
//     换关时只发生一次还能忍，但周期重扫每 30 秒一次，等于每 30 秒复发一次。
//   扫进临时表、扫完一次性发布，读方在任何时刻看到的都是一份完整的表。
//   换关时仍然会先清（ResetInjectState 清 g_tplN/g_adN、并清空召唤列表），
//      那一关的资产已经释放，旧指针是悬垂的，那时候不能让读方看到旧表。
static TplEntry s_scanTpl[SCAN_TAB_MAX];
static AdEntry s_scanAd[SCAN_TAB_MAX];
// 本关是否已经"补扫"过，防每次按召唤键都扫一遍 768MB（只有当某个模板确实没有
// AISpawnData 时才会走到补扫，那种情况下不加闸门就会每按一次都扫一次 + 打几百行 [TPL] 日志）。
// ResetInjectState 会清它（换关后允许再补扫一次）。
static volatile LONG g_adScanTried = 0;

// AISpawnData 条目的自洽校验：这份 AD 的 +48（它配的模板）必须就是查询的模板。
//   表的发布是就地 memcpy，读方在别的线程上，可能读到"新 tpl + 旧 ad"的半新条目；
//   不过滤的话会读到错配的值（别人队伍的 +92、别人的血量、错判"有 AD"）。
static bool AdSelfOk(unsigned long long ad, unsigned long long tpl) {
  unsigned long long self = 0;
  return Rd64(ad + 48, &self) && self == tpl;
}

// 第 i 条属于 tpl 而且自洽吗。三处查表路径的头两步都是这一问。
//   ad 为 0 的空条目在这里一并算"不属于"：不先判它的话，Rd64 会去读地址 48，
//   靠 SEH 接住才返回 false，结果一样但白踩一次异常。
//   FindOwnAd 不用它：那处要把"空条目"和"半新条目"分开计数（理由写在它自己那处）。
static bool AdFits(int i, unsigned long long tpl) {
  return g_adTab[i].tpl == tpl && g_adTab[i].ad && AdSelfOk(g_adTab[i].ad, tpl);
}

// ---------------------------------------------------------------------------
// 召唤后补挂出生 buff
//
//   注入直接调 sub_141D6CFD0，绕过了游戏的生成组通知链，游戏自己那套
//   "AI 出生时按统计类别挂 buff" 的规则不会触发。实测后果是 Just Brad /
//   Sproutrage 只拿到基础近战伤害（约 10），原版是 ×20。
//
//   上哪些、不上哪些按既定政策：只上三个生成组场景与 DreadrootRareMob 的出生无条件两条；
//   其余场景一律不上，所有无敌类 buff 一律不上。
//   规则表是生成物，内容见下面的 include。
//
//   BuffData 的运行时指针不能写死：要在扫描时按类型描述符从资产区里认出来。
// ---------------------------------------------------------------------------
#include "spawn_buff_table.inc"

// 扫到的、本表用得到的 BuffData 实例。只存名字在 kBuffNames 里的那些，
//   数量很少，不会撞上限。同一个资产是共享实例，按下标去重即可。
struct BuffEntry {
  unsigned long long obj;
  unsigned char idx;  // 下标进 kBuffNames
};
constexpr int BUFF_TAB_MAX = 64;
static BuffEntry g_buffTab[BUFF_TAB_MAX];
static volatile LONG g_buffN = 0;
static BuffEntry s_scanBuff[BUFF_TAB_MAX];

// 资产路径的基名（最后一个分隔符之后）与 kBuffNames 比对。
//   路径形如 Gameplay/Weapons/Common/Buffs/<name>，表里存的是 <name>。
static int BuffIndexOfPath(const char *path) {
  const char *basename = BaseName(path);
  for (int i = 0; i < kBuffNameCount; ++i)
    if (strcmp(basename, kBuffNames[i]) == 0)
      return i;
  return -1;
}

// 某个单位（CustomizePVZCharacterData 资产路径）出生该拿哪些 buff。
static const SpawnBuffRule *FindSpawnBuffRule(const char *cust) {
  if (!cust || !cust[0])
    return 0;
  for (int i = 0; i < kSpawnBuffRuleCount; ++i)
    if (strcmp(kSpawnBuffRules[i].cust, cust) == 0)
      return &kSpawnBuffRules[i];
  return 0;
}

// 按 buff 下标从扫出来的实例表里取 BuffData 指针，取不到返回 0。
static unsigned long long BuffObjOf(int idx) {
  const LONG n = AtomicReadL(&g_buffN);
  for (LONG i = 0; i < n && i < BUFF_TAB_MAX; ++i)
    if ((int)g_buffTab[i].idx == idx && g_buffTab[i].obj)
      return g_buffTab[i].obj;
  return 0;
}

// 给刚生成出来的实体补挂它的出生 buff，返回真正挂上几个。
//
//   调用形状都经反编译复核：
//     buffComp = sub_1410C1D50(entity, &off_144DBDBC0)
//     目标是 owner（entity+600，取不到再走 entity+912 的回退链），不是服务器实体本身
//     先 sub_141DD47E0 跑效果，再 sub_141DD4330 登记进组件条目表
//   挂完用 sub_141DD2EA0 查一次，确认它真的进了条目表。
//   取不到组件就整段放弃，不硬来；某个 buff 没被扫到就跳过那一条。
//
//   调用方负责保证在游戏线程上、且这一发生成已被判成功。
//   整段套 SEH：几个 ctx 偏移的语义是按反编译推的，出错只记日志，不影响这次召唤。
// 记录异常码与出错地址。过滤器里只存值不打日志：Log 自己也可能再抛，
//   真正的打印放到 __except 块里。
static unsigned long g_buffExcCode;
static unsigned long long g_buffExcAddr;
static int BuffExcFilter(EXCEPTION_POINTERS *ep) {
  g_buffExcCode = ep->ExceptionRecord->ExceptionCode;
  g_buffExcAddr =
      ep->ExceptionRecord->NumberParameters > 1 ? ep->ExceptionRecord->ExceptionInformation[1] : 0;
  return EXCEPTION_EXECUTE_HANDLER;
}

typedef void *(*GetBuffCompFn)(void *ent, const void *key);
typedef unsigned int (*FindBuffFn)(void *comp, void *buffData, void *owner);
// 返回类型必须是 char：这函数只保证 al 有效，写成 int 会把 eax 高位的垃圾读进来，
//   于是明明返回 0（成功）也会被判成失败。
// ctx 的布局照游戏自己的 Apply 处理器组装：0=BuffData、1=1、2/3=目标实体，其余留 0。
//   条目构造函数只对 5/6/8/10/11 做空判断，给 0 是安全的。
typedef char (*RunEffectsFn)(void *comp, void *buffData, void *target, unsigned long long fallback);
typedef unsigned long long (*RegisterBuffFn)(void *comp, void *ctx, unsigned long long target,
                                             unsigned long long mode);

static int ApplySpawnBuffs(unsigned long long tpl, unsigned long long ent) {
  if (tpl == 0 || ent == 0)
    return 0;
  // 模板的 +56 是 CustomizeCharacterData 的直指针（本文件一直按单指针读它）。
  unsigned long long cust = 0;
  char custPath[SUM_NAME_MAX] = {0};
  if (!Rd64(tpl + 56, &cust) || cust == 0)
    return 0;
  if (!ReadAssetPath(cust, custPath, sizeof(custPath)) || !custPath[0]) {
    // 有外观指针却读不出路径，说明模板 +56 这个前提不成立了。这条只报一次：
    //   真坏了每发召唤都会走到这里，不限流会把日志刷满。
    static volatile LONG once = 0;
    if (!(AtomicReadL(&once) & 1)) {
      InterlockedOr(&once, 1);
      Log("[BUFF] template %p has an appearance pointer but no readable path; buffs skipped",
          (void *)(uintptr_t)tpl);
    }
    return 0;
  }
  const SpawnBuffRule *rule = FindSpawnBuffRule(custPath);
  if (!rule)
    return 0;

  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base)
    return 0;

  // 这两个在 __try 里改、在 __except 之后读，必须 volatile：否则异常展开那一趟
  //   之后读到的可能是寄存器里的旧值。文件里 ScanTemplates 的计数器是同一道理。
  volatile int applied = 0;
  volatile bool noComp = false;
  __try {
    GetBuffCompFn getComp = (GetBuffCompFn)(base + RVA_GET_BUFF_COMP);
    void *comp = getComp((void *)(uintptr_t)ent, base + RVA_SERVER_BUFF_KEY);
    if (!comp) {
      noComp = true;
    } else {
      RunEffectsFn runEffects = (RunEffectsFn)(base + RVA_RUN_EFFECTS);
      RegisterBuffFn registerBuff = (RegisterBuffFn)(base + RVA_REGISTER_BUFF);
      FindBuffFn find = (FindBuffFn)(base + RVA_FIND_BUFF);
      // buff 的作用目标是主人，不是服务器实体。游戏自己也是这么解析的：
      //   先取 entity+600（owner），取不到再走 entity+912 那条回退链。
      unsigned long long owner = 0;
      if (!Rd64(ent + 600, &owner) || owner == 0) {
        unsigned long long fallback = 0, inner = 0;
        // inner 必须大于 8：否则 inner-8 会算出接近 2^64 的地址，交给引擎就是野指针。
        if (Rd64(ent + 912, &fallback) && fallback && Rd64(fallback, &inner) && inner > 8)
          owner = inner - 8;
      }
      if (owner == 0) {
        Log("[BUFF] %s: the entity has no owner to carry the buff, skipped", custPath);
      } else {
        for (int i = 0; i < (int)rule->n; ++i) {
          const int bi = (int)rule->buff[i];
          if (bi < 0 || bi >= kBuffNameCount)
            continue;
          const unsigned long long buffData = BuffObjOf(bi);
          if (!buffData) {
            Log("[BUFF] %s: '%s' is not loaded, skipped", custPath, kBuffNames[bi]);
            continue;
          }
          // 解析出来的对象必须就是这个名字的资产。读不出路径也算不通过：
          //   静默放过等于把"没核对"当成"核对通过"。
          char got[SUM_NAME_MAX] = {0};
          const int gotIdx =
              (ReadAssetPath(buffData, got, sizeof(got)) && got[0]) ? BuffIndexOfPath(got) : -1;
          if (gotIdx != bi) {
            Log("[BUFF] %s: wanted '%s' but resolved '%s' (index %d), skipped", custPath,
                kBuffNames[bi], got, gotIdx);
            continue;
          }
          // 第一步，跑效果。返回 0 才继续登记。
          const char rc =
              runEffects(comp, (void *)(uintptr_t)buffData, (void *)(uintptr_t)owner, 0);
          if (rc != 0) {
            Log("[BUFF] %s: running the effects of '%s' returned %d", custPath, kBuffNames[bi],
                (int)rc);
            continue;
          }
          // 第二步，登记进组件条目表。
          unsigned long long ctx[16] = {0};
          ctx[0] = buffData;
          ctx[1] = 1;
          ctx[2] = owner;
          ctx[3] = owner;
          const unsigned long long entry = registerBuff(comp, ctx, owner, 0);
          if (entry)
            ++applied;
          else
            Log("[BUFF] %s: registering '%s' returned no entry", custPath, kBuffNames[bi]);
          // 挂完复核：登记成功不出声，失败才报。
          const unsigned int present =
              find(comp, (void *)(uintptr_t)buffData, (void *)(uintptr_t)owner);
          if (present == 0xFFFFFFFFu)
            Log("[BUFF] %s: '%s' did not stay in the component", custPath, kBuffNames[bi]);
        }
      }
    }
  } __except (BuffExcFilter(GetExceptionInformation())) {
    // 地址落在模块外时不要减基址：那会算出一个接近 2^64 的"偏移"，比不打还误导。
    const unsigned long long baseVa = (unsigned long long)(uintptr_t)base;
    const unsigned long long off = (g_buffExcAddr >= baseVa) ? g_buffExcAddr - baseVa : 0;
    Log("[BUFF] %s: exception 0x%08lX at 0x%llX (module+0x%llX), this spawn keeps whatever"
        " was set",
        custPath, g_buffExcCode, g_buffExcAddr, off);
  }
  if (noComp)
    Log("[BUFF] %s: no ServerBuffComponent on the entity, skipped", custPath);
  else
    Log("[BUFF] %s: applied %d of %d", custPath, applied, (int)rule->n);
  return applied;
}

//   它不参与分列逻辑，分列规则是"名字里没写 Plant/Zombie 的一律算敌人"。
//   还要它是因为 Template_Snowman 这种到底是"第三方阵营"还是"单纯漏写了前缀"，
//   光看名字判断不了，得看游戏自己给它配的队伍字段。
//   foundOut 收"到底有没有查到这份 AISpawnData"，区分"值是 0"和"根本没有"。
//   发布版里没人读它的结果，整块只在开发构建里编（唯一的调用点在 BuildSummonLists 的
//   _DEV 块里）。
#ifdef _DEV
static unsigned int AdTeamOf(unsigned long long v21, bool *foundOut) {
  if (foundOut)
    *foundOut = false;
  if (v21 == 0)
    return 0;
  LONG n = AtomicReadL(&g_adN);
  for (LONG i = 0; i < n; ++i) {
    if (!AdFits(i, v21))
      continue;
    if (foundOut)
      *foundOut = true;
    unsigned int t = 0;
    if (!Rd32(g_adTab[i].ad + 92, &t))
      return 0;
    return t;  // +92 是 Team 字段：1=僵尸 2=植物（0/其它 = 说不清）
  }
  return 0;
}
#endif

// 血量读数的合理性上界。BFN 最高的 Boss 也就几万，1e7 有 300 倍余量。
//   两个消费者必须用同一个数：AdHpOf 拿它当排序键的合理性筛（筛错会把血牛排到最弱档），
//   SpawnHealthOf 拿它当真正注入的血量的筛。口径一分家，
//   档位列的顺序就会跟实际血量对不上。
constexpr float MAX_PLAUSIBLE_HP = 1.0e7f;

// 血量读数的下限。游戏的 sub_141D6CFD0 是 comiss xmm1, 1e-6：小于它就算"没有覆盖"，
//   改用模板默认血量。这里必须用同一个数：我们写进 holder+88 的值一旦落在 (0, 1e-6)，
//   游戏会把它丢掉、转而去用数组末档（4 人局强度），而排序键还按我们写的那份算，
//   两头就对不上了。同样两个消费者共用。
constexpr float MIN_HP_OVERRIDE = 1.0e-6f;

// 从模板路径里取阵营：看 Template_ 之后到下一个下划线之间那一段。
//   2 = 植物、1 = 僵尸、0 = 第三方或写不出来。编码和 g_team / AISpawnData 的 +92 一致。
//   判据只此一份：HUD 分列（ShortTemplateName）、排序键（AdHpOf）、挑注入用的 AD
//   （FindOwnAd 拿到的是它的结果）三处都走这里。各写一份的话，同一条路径会在
//   一边算第三方、另一边算植物，而"排序键用哪份 AD"和"注入用哪份 AD"会分家。
static unsigned char TemplateFactionOf(const char *path) {
  if (!path || !*path)
    return 0;
  const char *p = BaseName(path);
  if (strncmp(p, "Template_", 9) != 0)
    return 0;
  if (strncmp(p + 9, "Plant_", 6) == 0)
    return 2;
  if (strncmp(p + 9, "Zombie_", 7) == 0)
    return 1;
  return 0;  // Template_Gnome_* / Template_Snowman 这类：第三方，永远归敌人列
}

// 取这个模板自己那份 AISpawnData 的 OverrideMaxHealth（+88）。≤ 1e-6 表示"不覆盖"。
//
//   排序必须用它，因为游戏在 sub_141D6CFD0 里的取值优先级是
//       v27 = *(float*)(holder + 88);                  // (1) AD 的 OverrideMaxHealth
//       if (v27 < 1e-6 && 数组长度 != 0) v27 = 数组[人数-1];   // (2) 才轮到数组
//   模板的血量数组为空时，血量就写在这份 AD 的 +88 里。
//
//   foundOut 把两种"没有值"分开，这个区分是排序对不对的关键（见 TierSortHp）：
//     返回 0 且 *foundOut = true ，找到了这份 AD，它明确说"不覆盖"
//     返回 0 且 *foundOut = false，表里根本没这个模板的 AD，我们不知道
static float AdHpOf(unsigned long long v21, bool *foundOut = nullptr) {
  if (foundOut)
    *foundOut = false;
  if (v21 == 0)
    return 0.0f;
  const LONG n = AtomicReadL(&g_adN);

  // 挑哪一份 AD 必须和 FindOwnAd 用同一条规则：它优先取 +92 与模板阵营一致的那一份，
  //   注入用的 holder 就是它。这里若按"第一份自洽的"挑，排序键和真正注入的血量
  //   会来自两份不同的 AD，档位列的顺序就与实际血量对不上。
  unsigned char wantTeam = 0;
  {
    char tplPath[SUM_NAME_MAX] = {0};
    ReadAssetPath(v21, tplPath, sizeof(tplPath));
    wantTeam = TemplateFactionOf(tplPath);
  }

  // 读一份 AD 的 +88。返回 false 时 *explicitNo 区分两种"没有值"：
  //   false = 读不出来或值离谱，我们不知道；true = 这一份明确说"不覆盖"。
  auto hpOfAd = [&](unsigned long long ad, bool *explicitNo, float *out) -> bool {
    unsigned int bits = 0;
    if (!Rd32(ad + 88, &bits))
      return false;  // 读不出来，还是"不知道"
    float f = *(float *)&bits;
    // 先筛掉"不可能是合法血量"的值：上界以上、以及 NaN。这些一律当"读不出来"，
    //   绝不能算"它明确说不覆盖"。算错的话这一档会被当成最弱档排到最前、成为默认选中，
    //   正好复现"召出来全是血牛"。
    // 写成 !(f < ...) 而不是 f >= ...，是因为要一并接住 NaN：
    //   NaN 跟任何数比较都是假，f >= 上界 那一支判不出来会把它放过去，
    //   取反这一支才为真。放过去的话 NaN 会被当成合法血量当排序键、还会写进 holder+88。
    if (!(f < MAX_PLAUSIBLE_HP))
      return false;
    *explicitNo = true;  // 这一份 AD 读到了
    // 下限与游戏对齐，数本身与理由见 MIN_HP_OVERRIDE（小于它算"没有覆盖"，取默认血量）。
    if (!(f > MIN_HP_OVERRIDE))
      return false;
    *out = f;
    return true;
  };

  // 两趟：先找阵营一致的那一份，找不到（或模板自己没写阵营）再退回第一份自洽的。
  for (int pass = 0; pass < 2; ++pass) {
    for (LONG i = 0; i < n; ++i) {
      if (!AdFits(i, v21))
        continue;
      // 半新条目配出来的 +88 是别人的血量，而这里的返回值直接决定档位列的排序键，
      //   错了就是默认选中的档位也跟着错。
      if (pass == 0) {
        unsigned int team = 0;
        if (wantTeam == 0 || !Rd32(g_adTab[i].ad + 92, &team) || (unsigned char)team != wantTeam)
          continue;
      }
      bool explicitNo = false;
      float f = 0.0f;
      if (!hpOfAd(g_adTab[i].ad, &explicitNo, &f)) {
        if (foundOut && explicitNo)
          *foundOut = true;  // 这一份明确说不覆盖：返回 0 是"已知的最弱档"
        return 0.0f;
      }
      if (foundOut)
        *foundOut = true;
      return f;
    }
  }
  return 0.0f;
}

// 从模板的血量数组里取第一档。
//
//   v21[11]（= +88）指向一个 float 数组，长度存在 arr-4 的低 31 位。
//   游戏在 sub_141D6CFD0 里是 array[sub_140F57F70(...) - 1]。
//
//   那个索引值不是难度，而是"非 AI 参与者的人数"（见 GetAdDescriptor 附近的推断
//      和难度探针 ProbeDifficultyTier）。所以这 4 个数是 1P/2P/3P/4P 的人数缩放：
//      豌豆射手 BattleLeader_Elite = {1500, 1750, 2000, 2250}，值递增，人越多怪越肉。
//
//   永远只取第一档（= 单人局的强度），理由是公平，
//     游戏里单人局敌人的血量就是 array[0]；强制取最大值等于把单人局的怪硬拉成 4 人局。
//   数组为空（那种不分档的普通兵），返回 0，上层不写 holder+88，走模板默认血量。
//
//   outLen 只用于日志（告诉人"数组几项"），可为 null。
static float SpawnHealthOf(unsigned long long v21, unsigned int *outLen = nullptr) {
  if (outLen)
    *outLen = 0;
  if (v21 == 0)
    return 0.0f;
  unsigned long long arr = 0;
  if (!Rd64(v21 + 88, &arr) || arr == 0)
    return 0.0f;
  unsigned int len = 0;
  if (!ArrCountOf(arr, &len))
    return 0.0f;
  if (outLen)
    *outLen = len;
  if (len == 0)
    return 0.0f;  // 不分档，不覆盖
  unsigned int bits = 0;
  if (!Rd32(arr, &bits))
    return 0.0f;
  float f = *(float *)&bits;
  // 合理性判据：下界与上界都和排序键那边共用同一个数（见 MIN_HP_OVERRIDE 与
  //   MAX_PLAUSIBLE_HP）。下界不是 0：落在 (0, 1e-6) 的值游戏会当"没覆盖"丢掉，
  //   我们还写上 holder+88 就成了两头对不上。
  //   两处都写成取反的比法，保持一致，也一并接住 NaN。
  if (!(f > MIN_HP_OVERRIDE) || !(f < MAX_PLAUSIBLE_HP))
    return 0.0f;
  return f;
}

// 档位列排序用的血量键。越小越弱，从小到大排，第 1 档就是最弱的那个。
//
//   取值分三种，别把它们混起来：
//     正数   = 已知的覆盖血量（先看血量数组，空则看 AD 的 +88）
//     0      = 找到了 AD、它自己说"不覆盖"，游戏会用模板自己的默认血量。
//              而默认血量就是基础值，Champion / Elite / BattleLeader 这些档
//              都是在基础值上往上覆盖的，没有哪个档往下覆盖。
//              所以"不覆盖"= 基础档 = 最弱档，排最前。
//     1.0e9  = 真的不知道（连这个模板的 AISpawnData 都没找到），排最后。
//
//   后两种必须分开，不能都当成 0：一旦混起来，"AD 表里查不到"的模板就会被
//   当成"最弱"排到最前，默认选中的第 1 档成了 BOSS，也就是"召出来全是血牛"。
//   判据只能是 AD 找到了、且它明确不覆盖（+88 ≤ 1e-6 且不是坏值），才算确定的最弱档。
//
//   包成函数是因为排序用它、诊断也用它。两处各写一遍必然会分叉，
//   到时候排序用的键和诊断打出来的数不是同一个，日志反而误导人。
static float TierSortHp(unsigned long long v21) {
  float h = SpawnHealthOf(v21, nullptr);
  if (h > 0.0f)
    return h;  // 血量数组第 1 项 = 单人局强度（永远取第一档）
  bool adFound = false;
  float a = AdHpOf(v21, &adFound);
  if (a > 0.0f)
    return a;
  return adFound ? 0.0f : 1.0e9f;
}

// 请求后台重扫（见 SummonWithTemplate 里的补扫分支），
//   绝不能在游戏线程上跑 ScanTemplates：它要扫 650 到 768MB、0.6 到 0.8 秒，
//   会冻住 AI 调度 tick（HUD 卡顿、严重时触发网络超时）；
//   而且它若与 Worker 那次并发重建同一张表，条目会交错重复。
// 游戏线程只置标志，真正的扫描交给 Worker。
static volatile LONG g_wantRescan = 0;

// 取 AISpawnData 的运行时类型描述符。两级来源：
//   1)  抓到过的那份 holder 的 +8（校验用：与写死值不一致时以写死值为准）
//   2)  写死的 RVA_TYPEDESC_AISPAWNDATA。为什么必须有这条退路，见那个常量的定义处。
// 定义在下面（g_holderCopy 在那里才可见），和 ResetInjectState 一样的处理方式。
static unsigned long long GetAdDescriptor();
// 换关卡时清掉所有"上一关对象的快照"（holder / req 的副本）。
// 定义在下面（那些状态变量在那里才可见），和 DoSummonSpawner 一样的处理方式。
//   这些副本里存的全是指向上一关对象的指针，换关后它们就是悬垂的；
//   不清的话换关后第一次召唤 = 拿悬垂指针当底板，访问违例。
static void ResetInjectState();

// AI 侧的 spawner ctx，注入模板时用它当载体。
//
//   首选来源是 SPAWN 探针在 AI 路径（调用者 RVA 0x1DCC232）上看到的那次
//   ctx 实参。这是游戏自己交出来的 AI ctx，不用猜。
//   兜底来源是 ScanPool 的链表遍历，只在游戏还没走过一次 AI 生成时用。
//
//   绝不能用玩家自己的那个，对它调 vt[232] = 玩家销毁重生成，人直接消失（实测过）。
static volatile LONG64 g_aiCtx = 0;
// 失败跟"是哪个载体"无关，是那个场景当时的游戏状态问题
//   （实测：两个不同载体在 4 秒内都四档全崩，而其中一个 15 分钟后在别的场景一次就成）。
//   所以不按"某次失败过"记历史：那种记录会把见过的每个载体都记成坏的，而且不会自己过期。
//   能记的只有"它自己抛过异常"这一条（见 g_recentBad），那是载体的毛病，不是场景的。
// "载体刚被丢掉，请 Worker 立刻重挑一个"。
//   这不是计时器也不是重试计数，就是一次事件通知：放下一个载体的地方在游戏线程，
//   而挑载体的 ScanPool 在 Worker 线程、本来每 2 秒才跑一次。不通知的话就得干等 2 秒。
//   Worker 看到就把它清掉，同一轮里立刻扫一遍。
static volatile LONG g_repickNow = 0;
// g_aiCtx 的来源值，0=没有  1=ScanPool 猜的  2=SPAWN 探针在 AI 路径上看到的（游戏自己给出的）
// 规则是 2 永远赢（可以在 1 之上覆写）；1 只在 0 时填。
static volatile LONG g_aiCtxSrc = 0;

// 丢掉当前载体：值还是它才清，清成功就一并把来源标记清掉。
//   四个丢掉点（ScanPool 的退役清空、四档全崩、召唤时的校验失败、Worker 的 552 体检）
//   都必须成对清这两个量，各写一份的话必然有一处漏。无条件清会把别处刚发布的
//   新载体一起抹掉（见 DoSummonSpawner 那处的说明）。
//   必须 CAS 而不是读完就清：读到值到清之间隔着时间片，值可能已经换人。
//   返回 false 表示值已经换人，那就什么都不做。
static bool DropCarrierIfCurrent(unsigned long long ctx) {
  if (InterlockedCompareExchange64(&g_aiCtx, 0, (long long)(intptr_t)ctx) !=
      (long long)(intptr_t)ctx)
    return false;
  InterlockedExchange(&g_aiCtxSrc, 0);
  return true;
}
// 按阵营各记一个可用的载体，索引 0 不用（队号 1=僵尸 2=植物）。
//
//   游戏自己要求"只能用自己阵营的 spawner 生成"，那条规矩写在
//   sub_141D6D9B0 的闸门里（ctx+552 && ctx+576 == player+88）。我们绕过那道闸门
//   直接调 sub_141D6CFD0，就得自己守这条。实测不守的后果：载体的部件会混到目标身上
//   （用僵尸 spawner 造豌豆，出来的豌豆带着科学家的头发），而且那个实体不算任何一方的击杀。
//
//   g_aiCtx 是"游戏自己交出来的那个 ctx"，它属于哪一队不由我们决定，所以单独记一份
//   按队分槽的候选（ScanPool 每 2 秒的遍历里顺手填）。
static volatile LONG64 g_carrierByTeam[CARRIER_TEAMS] = {0};

// 最近出现过的"玩家自己的 spawner"地址（环形，8 个）。
//   玩家每次重生都换一个新的 spawner，旧的那个不会停用：它继续挂在链表里、
//   通过描述符和 ctx+552 的全部检查，看起来和普通 spawner 没区别；
//   而 SafeFindCtx 只返回当前那个，问不出它。
//   只记一个（prevCtx0）只能排除一轮，第二轮到它又变成合法候选了。
//   拿它当载体就是对它调 vt[232]，玩家实体会被销毁重生成，人从世界里消失。
// 槽数必须是 2 的幂（取模用的是位与）。
//
// 环要小：环越大、旧地址留得越久，堆把退役地址复用给新 spawner 时，
//   新模式的合法载体被当成"玩家自己的"挡掉的概率就越高（实测那一轮连着 4 次召唤全是
//   "no usable AI carrier ctx"）。小环里退役的地址很快被挤出去，
//   真正的保护由 ValidCarrier 当场查询兜底。
constexpr int RECENT_MINE_SLOTS = 8;
static volatile LONG64 g_recentMine[RECENT_MINE_SLOTS] = {0};
// 与上面同下标的"spawner 自己的 ID"：地址加 ID 一起比，才不会被堆复用骗过。
//   实测从 Giddy Park 的传送门进人机模式之后，一个正在正常刷僵尸 AI 的 spawner
//   地址恰好等于上一关玩家 spawner 的地址，于是被当成"刚退役的自己的 spawner"
//   一次次丢掉，而那个模式里唯一的载体就是它，结果是整局召唤失败。
//   加了 ID 之后，地址相同但 ID 不同 = 另一个 spawner，不再误判。
static volatile LONG64 g_recentMineId[RECENT_MINE_SLOTS] = {0};
// 每格带一个写序号，写方推成奇数、写地址和 ID、再推成偶数；读方用它区分
//   "读到的这一格是不是半截"。没有它的话，读方可能读到 (旧地址A, 新ID) 这种组合，
//   把玩家自己的 spawner 判成"不是我的"，挑来当载体，玩家实体就被销毁重生成。
static volatile LONG g_recentMineSeq[RECENT_MINE_SLOTS] = {0};
static volatile LONG g_recentMineN = 0;

// 刚"四档全抛异常"被判死的载体（环形，RECENT_BAD_SLOTS 个）。
//   ScanPool 挑候选是"链表里第一个能用的"，完全排除都不做的话
//   它 50ms 后就把同一个地址原样填回槽里，下一轮换到它、再全抛一遍，永远不收敛，
//   而日志里每次都是同一串"全抛 + 丢掉"，看不出是同一个指针在打转。
//   只有真抛异常的载体才进这里，闸门拦住的（返回 0）是正常载体，不该被排除。
//   换关时清掉，免得地址被复用之后被永久挡住。
constexpr int RECENT_BAD_SLOTS = 4;
static volatile LONG64 g_recentBad[RECENT_BAD_SLOTS] = {0};
static volatile LONG g_recentBadN = 0;

static bool IsRecentBadCarrier(unsigned long long ctx) {
  if (ctx == 0)
    return false;
  for (int i = 0; i < RECENT_BAD_SLOTS; ++i) {
    if ((unsigned long long)AtomicReadL64(&g_recentBad[i]) == ctx)
      return true;
  }
  return false;
}
// spawner 自己的 ID 是 *(*(ctx+48) + 24)，读不到返回 0。
//   它就是 RoleWatch 用来判"换了 spawner"的那个值（日志里的 "spawner ID"）。
static unsigned long long SpawnerIdOf(unsigned long long ctx) {
  unsigned long long ref = 0, id = 0;
  if (ctx == 0 || !Rd64(ctx + 48, &ref) || ref == 0 || !Rd64(ref + 24, &id))
    return 0;
  return id;
}

// a 和 b 是不是同一个 spawner。
//   地址相同只是必要条件，地址会被堆复用，只比地址会把新模式里一个完全合法的
//   spawner 当成"玩家自己退役的那个"挡掉（实测整局没载体可用就是这么来的）。
//   任一侧的 ID 读不出来时按"是同一个"处理（保守，宁可少一个候选也不要误用玩家的）。
static bool SameSpawner(unsigned long long a, unsigned long long b, unsigned long long bId) {
  if (a == 0 || a != b)
    return false;
  unsigned long long aId = SpawnerIdOf(a);
  return aId == 0 || bId == 0 || aId == bId;
}

static bool IsRecentOwnSpawner(unsigned long long ctx) {
  if (ctx == 0)
    return false;
  for (int i = 0; i < RECENT_MINE_SLOTS; ++i) {
    const LONG s1 = (LONG)AtomicReadL(&g_recentMineSeq[i]);
    unsigned long long a = (unsigned long long)AtomicReadL64(&g_recentMine[i]);
    if (a != ctx)
      continue;
    // 地址对上了，下面任何一种"读不干净"都必须判成"是我的"。判成"不是我的"
    //   会让这个 ctx 被挑去当载体，对它调 vt[232] 就是玩家实体销毁重生成。
    if (s1 & 1)
      return true;  // 这一格正在被写
    unsigned long long id = (unsigned long long)AtomicReadL64(&g_recentMineId[i]);
    if ((LONG)AtomicReadL(&g_recentMineSeq[i]) != s1)
      return true;  // 读的中途被改写，这一对值不可信
    if (SameSpawner(ctx, a, id))
      return true;
  }
  return false;
}

// 把一个 spawner 记进"最近退役的玩家 spawner"环。两处发布点共用，
//   发布顺序只在这里维护（见 g_recentMineSeq 的说明）。
static void PushRecentOwnSpawner(unsigned long long ctx) {
  if (ctx == 0)
    return;
  const int slot = InterlockedIncrement(&g_recentMineN) & (RECENT_MINE_SLOTS - 1);
  InterlockedIncrement(&g_recentMineSeq[slot]);  // 变奇数：这一格正在写
  g_recentMineId[slot] = (long long)SpawnerIdOf(ctx);
  g_recentMine[slot] = ctx;
  InterlockedIncrement(&g_recentMineSeq[slot]);  // 变偶数：写完了
}

// 把一个地址从退役环里撤掉（见 NoteCarrierAiSpawn）。
//   照 PushRecentOwnSpawner 的写法走 seq 奇偶，读方要么看到清空后的格子、要么看到干净的一对值。
static bool ForgetRecentOwnSpawner(unsigned long long ctx) {
  if (ctx == 0)
    return false;
  bool cleared = false;
  for (int i = 0; i < RECENT_MINE_SLOTS; ++i) {
    if ((unsigned long long)AtomicReadL64(&g_recentMine[i]) != ctx)
      continue;
    InterlockedIncrement(&g_recentMineSeq[i]);  // 变奇数
    InterlockedExchange64(&g_recentMine[i], 0);
    InterlockedExchange64(&g_recentMineId[i], 0);
    InterlockedIncrement(&g_recentMineSeq[i]);  // 变偶数
    cleared = true;
  }
  return cleared;
}

// 读 spawner 自己的队号。1=僵尸 2=植物 0=第三方；读不到返回 -1。
//   读不到返回 -1：0 是合法的"第三方阵营"，两者混在一起的话，
//   读失败会被当成"这个载体是第三方的"，误判成需要换载体。
//   游戏自己就是拿队号判"这个 spawner 能不能给我生成"的：
//   sub_141D6D9B0 的闸门是 ctx+552 && ctx+576 == player+88。
static int CarrierTeamOf(unsigned long long ctx) {
  unsigned int t = 0;
  if (ctx == 0 || !Rd32(ctx + CTX_TEAM_OFF, &t))
    return -1;
  return (int)(t & 0xFFu);
}
// sub_141D5BE30(player, 0) 的函数指针（ScanPool 里初始化），用来当场问
// "此刻玩家的 spawner 是哪个"，而不是靠一个可能过期的地址去猜。
typedef void *(__fastcall *FindCtxFnT)(void *, char);
static FindCtxFnT g_findCtx = nullptr;

// 把模板路径变成"短名 + 阵营"。剥不出来返回 nullptr。
//   "Gameplay/AI/AI_Templates/Template_Plant_AIPeaShooter_Elite"
// 剥掉目录，"Template_Plant_AIPeaShooter_Elite"
// 剥 Template_<阵营>_，"AIPeaShooter_Elite"，阵营 = 2（植物）
// 剥紧跟的 AI，"PeaShooter_Elite"
//
//   两处实测的坑（有个角色显示成 AI / 档位 Dreadroot）：
//   1)  阵营取 Template_ 之后到下一个 _ 之间那一段：日志里还有 34 个 Template_Gnome_*
//      和 1 个 Template_Snowman，写死 Plant_/Zombie_ 会漏掉它们。
//      是 Plant/Zombie 就顺带定阵营；不是就留 0 = 第三方阵营，永远归到敌人列。
//   2)  AI 前缀有两种写法，游戏自己就不统一：
//        AIPeaShooter，AI 后面直接接大写
//        AI_Dreadroot，AI 后面是下划线（只认第一种的话，key 会变成 "AI"）
// 两种都要剥；AI_ 连下划线一起剥掉，否则剥完剩下 "_Dreadroot"、key 会是空串。
//
//   阵营编码和 g_team / AISpawnData 的 +92 一致：1=僵尸 2=植物。
static const char *ShortTemplateName(const char *path, unsigned char *factionOut) {
  if (factionOut)
    *factionOut = 0;
  if (!path || !*path)
    return nullptr;
  const char *p = BaseName(path);
  // 1)  剥 Template_<阵营>_
  if (strncmp(p, "Template_", 9) == 0) {
    const char *q = strchr(p + 9, '_');  // 阵营那一段的结束
    p = q ? (q + 1) : (p + 9);           // 没有第二个 _，整串就是角色名（如 Template_Snowman）
    if (factionOut)
      *factionOut = TemplateFactionOf(path);
  }
  // 2)  剥 AI / AI_（两种写法都要管）
  if (p[0] == 'A' && p[1] == 'I') {
    if (p[2] >= 'A' && p[2] <= 'Z')
      p += 2;  // AIPeaShooter，PeaShooter
    else if (p[2] == '_' && p[3] >= 'A' && p[3] <= 'Z')
      p += 3;  // AI_Dreadroot，Dreadroot
  }
  if (!*p)
    return nullptr;
  return p;
}

// 个别模板要归并进另一个角色的档位：游戏自己的本地化给它们配了相同的显示名，
//   不归组就会在角色列里出现两行一模一样的名字。
//   两对：
//   1) 向日葵的两个炮台模板：SunflowerTurretHeal 和 SunflowerTurretMegaHP，
//      两条 loc ID（ID_H_WEAPON_SUNFLOWER_HEAL_TURRET / ID_H_WEAPON_SUNFLOWER_TURRET）
//      在游戏里都叫 "Heal Flower"。MegaHP 改写成带后缀的写法，key 就并成同一个，
//      档位标签是 "MegaHP"。
//   2) 沙盒原型里的 HealZombiePayload 和正式 HealZombie，DisplayName 都是
//      ID_M_HEAL_ZOMBIE，外观共用同一份 Zombie_AIHealZombie，区别只在行为和血量
//      （150 对 105）。不归组就是两行 "Heal Zombie"。档位标签写成 "Payload"。
static const char *MergeSpecialShortName(const char *sn) {
  if (sn && strcmp(sn, "SunflowerTurretMegaHP") == 0)
    return "SunflowerTurretHeal_MegaHP";
  if (sn && strcmp(sn, "HealZombiePayload") == 0)
    return "HealZombie_Payload";
  return sn;
}

// 可玩职业的 loc ID（植物 11 + 僵尸 12，共 23 个）。写死，不随版本变。
//   判据取 loc ID 不取模板名：同一个职业的模板名和界面名对不上，
//   实测对不上的有 Night Cap（模板叫 Mushroom）、Space Cadet（Astronaut）、
//   Super Brainz（CaptainBrainz）、Kernel Corn（AssaultCorn）。
//   写模板名名单等于维护一张容易写错的映射表，而 loc ID 是游戏本地化数据里的原名，
//   模板自己的 DisplayName 字段存的就是它（运行时在 *(v21+32)），可以直接比对。
//
// 这个数组的顺序 = 列表要用的"游戏顺序"，可玩角色按这里的下标置顶。
//   它就是游戏角色选择界面的顺序（按职责分组 Attack / Defend / Support / Swarm），
//   wiki 的角色顺序就是游戏内顺序（已对着游戏核过）。
//   不要拿"游戏造 AI 时的生成顺序"当界面顺序：那是内部顺序（WIZARD 打头），完全不同。
//
// 读出来的是"这个档位"的 loc ID，而档位的 DisplayName 是档位专属的
//   （实测 AIMushroom_Hunt_Elite、AIEngineer_RareMob_Elite 都不在名单里，但它们
//    都是可玩职业）。所以序号只能在角色那一级用：组里取命中档位里最小的那个下标
//   再回填给整组（见 BuildSummonLists 的 cut），不能逐档位判，否则可玩职业的档位会被漏掉。
static const char *kPlayableLocId[] = {
    // 植物（队号 2）
    "ID_M_SPAWN_PEA_TITL",          // Peashooter
    "ID_M_SPAWN_CHMP_TITL",         // Chomper
    "ID_M_SPAWN_ASSAULTCORN_TITL",  // Kernel Corn
    "ID_M_SPAWN_MUSHROOM_TITL",     // Night Cap
    "ID_M_SPAWN_FIRE_TITL",         // Snapdragon
    "ID_M_SPAWN_CAC_TITL",          // Cactus
    "ID_M_SPAWN_CITRON_TITL",       // Citron
    "ID_M_SPAWN_ACORN_TITL",        // Acorn
    "ID_M_SPAWN_SUNF_TITL",         // Sunflower
    "ID_M_SPAWN_ROSE_TITL",         // Rose
    "ID_M_CHAR_NAME_WILDFLOWER",    // Wildflower
    // 僵尸（队号 1）
    "ID_M_SPAWN_SLD_TITL",         // Foot Soldier
    "ID_M_CHAR_NAME_IMP",          // Imp
    "ID_M_SPAWN_CAPT_TITL",        // Super Brainz
    "ID_M_SPAWN_ACTIONHERO_TITL",  // 80s Action Hero
    "ID_M_SPAWN_ELECTRIC_TITL",    // Electric Slide
    "ID_M_SPAWN_PIRATE_TITL",      // Captain Deadbeard
    "ID_M_SPAWN_ALLS_TITL",        // All-Star
    "ID_M_SPAWN_VOLTRON_TITL",     // Space Cadet
    "ID_M_SPAWN_SCI_TITL",         // Scientist
    "ID_M_ENGINEER",               // Engineer
    "ID_M_SPAWN_WIZARD_TITL",      // Wizard
    "ID_M_CHAR_NAME_TVHEAD",       // TV Head
};

// 把模板的 DisplayName（= loc ID）读出来。读不到返回 false。
//   +32 存的才是它，+24 是资产路径（ReadAssetPath），别混。
static bool TemplateLocId(unsigned long long v21, char *out, int n) {
  return ReadIdentAt(v21, 32, out, n);
}

// "不在可玩名单里"的排序序号比任何真实下标都大，于是非可玩角色排在可玩角色之后。
constexpr int PLAY_RANK_NONE = 0x7FFFFFFF;

// 单位在游戏里的显示名。表用 Frosty 从游戏本地化数据导出后生成，
//   只收基础档的 DisplayName。查不到的退回模板代码名。
struct UnitNameEntry {
  const char *id;
  const char *text;
};
#include "unit_names.inc"

// 取这个模板的游戏内显示名。成功返回 true 并把文本抄进 out。
static bool UnitDisplayName(unsigned long long v21, char *out, int n) {
  if (!OutStrReady(out, n))
    return false;
  char id[LOC_ID_MAX] = {0};
  if (!TemplateLocId(v21, id, sizeof(id)) || !id[0])
    return false;
  // 少数模板的 DisplayName 直接写的就是文本（实测 Template_Plant_AIIce = "Iceberg Lettuce"）。
  if (strncmp(id, "ID_", 3) != 0) {
    CopyStr(out, (size_t)n, id);
    return true;
  }
  for (int i = 0; i < kUnitNameN; ++i) {
    if (_stricmp(id, kUnitNames[i].id) == 0) {
      CopyStr(out, (size_t)n, kUnitNames[i].text);
      return true;
    }
  }
  return false;
}

// 这个模板的 loc ID 在可玩名单里的下标（= 列表里的游戏顺序）。
//   不在名单里返回 PLAY_RANK_NONE。大小写不敏感：游戏自己拼这些 ID 时大小写是统一的，
//   但没理由依赖它。
static int PlayableRankOf(unsigned long long v21) {
  char id[LOC_ID_MAX] = {0};
  if (!TemplateLocId(v21, id, sizeof(id)))
    return PLAY_RANK_NONE;
  const int n = (int)(sizeof(kPlayableLocId) / sizeof(kPlayableLocId[0]));
  for (int i = 0; i < n; ++i) {
    if (_stricmp(id, kPlayableLocId[i]) == 0)
      return i;
  }
  return PLAY_RANK_NONE;
}

// 临时行，一个模板剥出来的东西（排序/分组用，建完就丢）
struct SummonRaw {
  unsigned long long v21;
  float hp;  // 排序键，见 TierSortHp：正数=覆盖血量，0=不覆盖（最弱），1.0e9=不知道（最后）
  // 组序号，0..22 = 可玩职业在游戏里的顺序，PLAY_RANK_NONE = 非可玩。
  //   建表时先填"这个档位自己的"（多数档位读不出 loc ID，是 PLAY_RANK_NONE），
  //   cut 里再按组回填成"整组里最小的那个下标"。
  int rank;
  char full[SUM_NAME_MAX];  // 短名（PeaShooter_BattleLeader_Elite）
  char key[SUM_NAME_MAX];   // 角色名（PeaShooter）
  // 这一组的显示名，可玩置顶之外就按它排字典序。同一个组里每一项存同一份：
  //   最终排序走 qsort 的裸比较器，只看得见单个元素，问不出"它是哪一组"。
  char disp[SUM_NAME_MAX];
};

// 排序比较器，可玩职业置顶（无视字典序，按游戏顺序 = kPlayableLocId 的下标），
//   其余按显示名的字典序跟在后面。
//   二级键是 disp：它在最终排序之前就按组算好、写进组里的每一项，
//   所以同组的项 disp 与 key 都相同，必然排在一起。
//   同 key 的档位是一段连续区间，这是档位列和分组切分的硬前提：
//   只按 full 排的时候实测 PeaShooterKing 会插进 PeaShooter 与
//   PeaShooter_Champion 中间（'K'(0x4B) < '_'(0x5F)）。
//
// disp 与 key 的比较都不区分大小写。游戏自己拼模板名时大小写不一致，实测
//   Template_Plant_AIRoyalHypnoFlower_Elite 和 ..._AIRoyalHypnoflower[_LWE] 是同一个角色，
//   按字节比会把它拆成两行（其中一个只挂一档）。
// 切段那边用的是同一个大小写不敏感的比较（见 BuildSummonLists 的发射循环），两处必须一致：
//   只改一边的话，同组的项排不到一起，切出来的区间就是错的。
// 本比较器只用于最终排序。按组回填 rank 的那一趟必须用下面的 SummonRawKeyCmp：
//   本比较器先比 rank，会把"基础档命中名单、其余档位读不出 loc ID"的同一个角色
//   拆成两段，回填就只覆盖到段内，角色列出现两行。
static int __cdecl SummonRawCmp(const void *a, const void *b) {
  const SummonRaw *x = (const SummonRaw *)a;
  const SummonRaw *y = (const SummonRaw *)b;
  if (x->rank != y->rank)
    return x->rank < y->rank ? -1 : 1;
  int c = _stricmp(x->disp, y->disp);
  if (c)
    return c;
  c = _stricmp(x->key, y->key);
  return c ? c : strcmp(x->full, y->full);
}

// cut 第一趟（按组回填可玩序号）专用，只按 key 和 full 排，不带 rank。
//   回填的前提是"同 key 的档位连在一起"；可玩角色只有基础档命中名单，
//   其余档位读不出 loc ID，先比 rank 会把同一组拆到两段，回填就失效。
static int __cdecl SummonRawKeyCmp(const void *a, const void *b) {
  const SummonRaw *x = (const SummonRaw *)a;
  const SummonRaw *y = (const SummonRaw *)b;
  int c = _stricmp(x->key, y->key);
  return c ? c : strcmp(x->full, y->full);
}

// 档位列的组内比较器，按血量从小到大。
//   档位名是游戏自己拼的（base / Champion / BattleLeader_Elite / RareMob_Elite…），
//   字典序排出来的强弱顺序是随机的，要的是"从最弱到最强"，所以用血量排序键。
//   hp 是 TierSortHp 给的排序键：正数是覆盖血量，0 是"不覆盖"（= 基础档，最弱），
//   1.0e9 是"连 AD 都没找到"（不知道，排最后）。三种含义见那个函数。
//   同血量的几项退回字典序兜底：qsort 不稳定，没有这个兜底的话每次重建
//      同血量的几项会随机换位。
static int __cdecl TierHpCmp(const void *a, const void *b) {
  const SummonRaw *x = (const SummonRaw *)a;
  const SummonRaw *y = (const SummonRaw *)b;
  if (x->hp < y->hp)
    return -1;
  if (x->hp > y->hp)
    return 1;
  return strcmp(x->full, y->full);
}

// 重建左右两组（队友 / 敌人）。
//   1)  把 g_tplTab 里每个模板剥成 {角色 key, 档位标签, v21, 血量}，按阵营分到两边；
//   2)  两边各自排序，可玩职业按 kPlayableLocId 的游戏顺序置顶，其余按显示名字典序；
//   3)  同 key 的档位必然相邻，切段，段内按血量从小到大重排，再抄成角色表。
// 角色列 = 可玩置顶（组内取最小可玩序号，见 cut）+ 其余按显示名字典序；
//   档位列 = 血量升序，两者是不同的要求，别混。
//
//   必须在一份临时数组里做好再一次性发布，本函数在 Worker 线程跑，
//   而 HUD/热键线程随时可能在读；就地改会让它们读到"改了一半"的表。
//   先写内容、最后才发布角色数（g_allyN/g_enemyN），这样读方要么看到旧的完整表、
//   要么看到新的完整表，不会看到中间态。
//
//   阵营按 g_team 定，我方的模板进左组（队友，HUD 第 1/2 列），对方的进右组
//      （敌人，HUD 第 3/4 列）。换队时 RefreshTeam 会重调本函数，两组整体对调。
// g_listGen 的自愈式翻转。正常一个写者"开始 +1、结束 +1"就够；
//   但超时抢占会引入第二个写者，两人各自 +1 之后奇偶整个反掉，
//   读方从此把"正在写"看成"写完了"。所以两头都改成"打到目标奇偶为止"。
//   置奇与置偶这一对只管"值最后落在奇数还是偶数"，表达不了"还有人在写"：
//   任何一方把它置偶，都可能在另一方还没写完时就把门打开
//   （超时抢占那条路的后果之一）。奇偶自愈不等于两个写者安全。
// 同上的"置奇"，但把"是不是我置的"报出来。这是给第二个写者（ScanTemplates
// 换关时清列表那一段）用的：
//   BuildSummonLists 可能正停在奇数上重建，那种情况下收尾（置偶）的活归它。
//   抢着收尾等于替它开门，读方就能读到半份表（而它正要写的那几张临时表正是被清的对象）。
static bool EnsureListGenOddMine() {
  for (;;) {
    const LONG v = (LONG)AtomicReadL(&g_listGen);
    if (v & 1)
      return false;  // 已经有人在奇数上写着，收尾归他
    if (InterlockedCompareExchange(&g_listGen, v + 1, v) == v)
      return true;
  }
}

static void EnsureListGenParity(bool wantOdd) {
  for (;;) {
    const LONG v = (LONG)AtomicReadL(&g_listGen);
    if (((v & 1) != 0) == wantOdd)
      return;  // 已经是目标奇偶，收工
    if (InterlockedCompareExchange(&g_listGen, v + 1, v) == v)
      return;
  }
}

// 发布一侧的召唤列表：先把内容拷进正式数组，最后写计数。
//   计数是读方的唯一开关，顺序反了读方就会拿新计数去读旧表，所以这条顺序只写这一份。
//   一侧一张档位表加一张角色表，两处的差别全在实参上。
static void PublishSide(TierEntry *tiersDst, const TierEntry *tiers, size_t tierBytes,
                        CharEntry *charsDst, const CharEntry *chars, size_t charBytes,
                        volatile LONG *nOut, int n) {
  memcpy(tiersDst, tiers, tierBytes);
  memcpy(charsDst, chars, charBytes);
  InterlockedExchange(nOut, n);
}

static void BuildSummonLists() {
  // 互斥闸门，必须有。本函数会被两个线程调：
  //     - Worker（每 100ms 的 RefreshTeam、以及 ScanTemplates 扫完之后）
  //     - 按键线程（F1/F2/F7/F8 在列表为空时兜底调 RefreshTeam）
  //   而下面几张临时表是 static 的共享缓冲，并发进入会互相踩，切出乱七八糟的分组。
  //   抢不到就直接跳过（不自旋等待）：真有并发时另一次的结果马上就好，
  //      跳过这次不会丢东西（下次 RefreshTeam 还会再调）。
  // 闸门和"谁什么时候拿的"打包进同一个 64 位值，靠一次 CAS 同时完成"抢"和"记时间"：
  //   两个变量之间那两条指令宽的窗口里，抢到了但时间戳还没写，另一个线程
  //   会读到上一轮（可能几分钟前）的旧时间戳，误判"被占太久"，抢过来，
  //   两个重建并发跑，一起写那几张 static 临时表：正是这道闸门要防的事。
  //   高 32 位 = 拿到闸门那一刻的 tick；低 32 位 = 1（持有中）；整个 0 = 空闲。
  static volatile LONG64 busyGate = 0;
  const LONG64 claim = ((LONG64)(DWORD)GetTickCount() << 32) | 1;
  const LONG64 prev = InterlockedCompareExchange64(&busyGate, claim, 0);
  if (prev) {
    // 正常情况是"另一次重建正在跑"，跳过就好。
    //   但有一种坏情况必须能自愈，持有者抛异常展开出去，闸门永远停在非 0，
    //   之后所有重建都被拒、g_listGen 永远停在奇数，只能重启游戏。
    //   本函数体里游戏内存的读全走 SEH 包装的 Rd32/Rd64（失败即 return，不会展开），
    //   所以这条很难触发；但代价太大，留个兜底。
    //   正常一次重建最坏也就百来毫秒（要写 ~55 行日志、每行一次 fflush），
    //   按住超过 5 秒的一定不是"还在跑"。
    const DWORD held = (DWORD)((unsigned long long)prev >> 32);
    if ((DWORD)(GetTickCount() - held) < 5000) {
      InterlockedExchange(&g_listWant, 1);  // 叫 Worker 下一轮补建（见 g_listWant 的说明）
      return;
    }
    // 抢占也要 CAS，两个线程可能同时判超时，无条件写会把闸门改成"两边都以为持有"。
    if (InterlockedCompareExchange64(&busyGate, claim, prev) != prev) {
      InterlockedExchange(&g_listWant, 1);  // 另一个线程抢赢了，这一轮让给它
      return;
    }
    Log("[LIST] gate held %u ms without release (holder unwound with an exception?), taking over",
        (unsigned)(GetTickCount() - held));
  }
  InterlockedExchange(&g_listWant, 0);  // 拿到闸门，待办清掉
  EnsureListGenParity(true);            // 标记"正在重建"（见 g_listGen 的说明）
  // 过了这一行就是"持有闸门"，每一个 return 出口都必须放掉自己的闸：
  //    InterlockedCompareExchange64(&busyGate, 0, claim)，放闸用 CAS（只放自己的）：
  //    本线程可能是超时抢占接手的，直接写 0 会把接手者的闸清掉。
  //    目前出口有三个，下面"还没分组"那个早退、发布前的抢占检查、函数末尾。

  static SummonRaw tmpA[SUM_LIST_MAX], tmpE[SUM_LIST_MAX];  // 每边 ~110KB，必须 static
  static TierEntry tA[SUM_LIST_MAX], tE[SUM_LIST_MAX];
  static CharEntry cA[SUM_CHAR_MAX], cE[SUM_CHAR_MAX];

  int nA = 0, nE = 0;
#ifdef _DEV
  int nThird = 0;  // 第三方阵营（Gnome/Snowman，名字里没写阵营）的条数（诊断用）
#endif
  LONG total = AtomicReadL(&g_tplN);
  LONG myTeam = AtomicReadL(&g_team);

  // 还不知道自己是哪一队（g_team == 0）时两组都留空：
  //   分阵营靠 f == myTeam，而 myTeam 是 0，于是每个模板都判成"不是友军"，
  //   两边的模板会全部塞进"敌人"那一列（左右全乱）；而且 HUD 因为"两组都非空"
  //   不再显示 "waiting for team"，看到的是一列假数据。
  //   队号每 100ms 自动读；读到之后 RefreshTeam 会重调本函数把两组建好。
  if (myTeam != 1 && myTeam != 2) {
    InterlockedExchange(&g_allyN, 0);
    InterlockedExchange(&g_enemyN, 0);
    InterlockedExchange(&g_hud_dirty, 1);
    EnsureListGenParity(false);                         // 这次重建结束（哪怕什么都没建）
    InterlockedCompareExchange64(&busyGate, 0, claim);  // 只放自己的闸
    // 这里故意不置 g_listWant，队号是要等的（可能等好几分钟），置了的话
    //    Worker 会每 50ms 重试一次、每 50ms 打一行日志，把日志刷满。
    //    队号一到，RefreshTeam 会把它当"换队"处理，自动重建，不需要重试机制。
    Log("[LIST] team not known yet (g_team=0), both sides left empty, rebuilds once team is read");
    return;
  }

  // 只有拿到自己那份 AISpawnData 的模板才进列表。
  //   没拿到的那几个，注入时只能退回"载体 holder 副本"，于是 +56 Blueprint 和
  //     +72 BehaviorTree 都是载体的，召唤出来片状拉丝、而且套着载体的 AI。
  //     Frosty 的类型表也确认了：整个 spawn data 家族里只有 AISpawnData 带
  //     Blueprint/BehaviorTree（VaseAISpawnData 只是指向一份 AD），所以这类角色
  //     在资产里根本没有第二份可用的外观/行为树来源：与其凑合，不如不列。
  //   被滤掉的是约 10 个没有自己 AD 的废稿/道具；BOSS / 盆栽 / 地精都有 AD，全保留。
  const LONG adN = AtomicReadL(&g_adN);

  // 丢弃的条数在发布版里也要数：末尾那行汇总要用它（为什么只留汇总，见发布处那一段）。
  int droppedN = 0;
#ifdef _DEV
  // 判据凭证是第一条能读出来的 loc ID（证明模板 +32 就是 DisplayName）。
  char firstId[64] = {0};
#endif
  for (LONG i = 0; i < total; ++i) {
    const char *path = g_tplTab[i].path;
    const unsigned long long want = g_tplTab[i].v21;
    bool hasOwnAd = false;
    for (LONG k = 0; k < adN; ++k) {
      if (!AdFits(k, want))
        continue;
      // 这里不过滤的话，一个角色会因为一条错配的半新条目被错误地列进/剔出列表。
      hasOwnAd = true;
      break;
    }
    if (!hasOwnAd) {
      ++droppedN;
#ifdef _DEV
      // 逐条明细只在开发构建里打：一场里这一项就有四千多行，发布版留末尾那行汇总。
      if (droppedN <= 20)
        LOG_DEV("[LIST]   dropped (no AISpawnData of its own): %s", path);
#endif
      continue;
    }
    // 跳过 _Playable / _NPC：玩家可玩版 / 对话 NPC，都不是战斗 AI 小兵。
    //   它们确实有自己的 AISpawnData，按"有 AD 就收"本该列出来，但列进去也只会生成出
    //   玩家型实体或纯对话体，进不了战斗。
    //
    //   跳过时把 loc ID 打出来（只打前 10 条）：万一某个角色只有 _Playable 变体被扫到，
    //   它就会被静默丢掉，表现是"一个可玩角色凭空消失"而且没有任何日志。
    //   有这行就能直接对着资产核。
    if (strstr(path, "_Playable") || strstr(path, "_NPC")) {
      static volatile LONG skippedShown = 0;
      if (InterlockedIncrement(&skippedShown) <= 10) {
        char id[LOC_ID_MAX] = {0};
        TemplateLocId(want, id, sizeof(id));
        Log("[LIST]   skipped as a %s variant: %s (DisplayName loc ID = %s)",
            strstr(path, "_Playable") ? "_Playable" : "_NPC", path, id[0] ? id : "(unreadable)");
      }
      continue;
    }
    unsigned char f = 0;
    const char *sn = MergeSpecialShortName(ShortTemplateName(path, &f));
    if (!sn)
      continue;
#ifdef _DEV
    // 留一条判据凭证（只读第一条，代价一次）。
    if (!firstId[0]) {
      char id[LOC_ID_MAX] = {0};
      if (TemplateLocId(want, id, sizeof(id)))
        CopyStr(firstId, sizeof(firstId), id);
    }
#endif
    // 第三方阵营（Gnome / Snowman，名字里没写 Plant/Zombie 的），永远算敌人。
    // 所以"是友军"必须名字里明确写了 Plant/Zombie 且和我方一致，
    //      f == 0 一律落到敌人列（查 +92 是多余的：查出来是什么都得塞敌人）。
    if (f == 0) {
      // 诊断用，把"名字没写阵营"的模板连它的 AISpawnData+92 一起打出来，
      //   方便查证 Template_Snowman 到底是第三方阵营还是单纯漏写前缀。
      //   这些模板一律进敌人列（固定规则），这行日志不影响分列。
      // 整块只在开发构建里编：发布版里它算出来的东西没人读，而 AdTeamOf 是 O(AD 表) 的查表，
      //   每次重建（每 30 秒或每次换队）要为这三十来个模板各查一遍。
#ifdef _DEV
      bool found = false;
      unsigned int t92 = AdTeamOf(g_tplTab[i].v21, &found);
      if (nThird < 20)
        LOG_DEV("[LIST]   no faction in name: %-46s AISpawnData+92=%s", sn,
                found ? (t92 == 1 ? "1(zombie)" : (t92 == 2 ? "2(plant)" : "other"))
                      : "(no AISpawnData)");
      ++nThird;
#endif
    }
    const bool mine = f && (f == (unsigned char)myTeam);
    SummonRaw *dst = mine ? tmpA : tmpE;
    int *dstN = mine ? &nA : &nE;
    if (*dstN >= SUM_LIST_MAX)
      continue;  // 表满就丢（宁少勿越界）
    dst[*dstN].v21 = g_tplTab[i].v21;
    // 排序键现在就取好（后面按它给档位列排序）。纯读、每次重建才跑一遍，开销可忽略。
    //   取法和"为什么未知排最后"都写在 TierSortHp 那边，这里不重复。
    dst[*dstN].hp = TierSortHp(g_tplTab[i].v21);
    // 可玩序号，这里填的是"这个档位自己的"（多数档位读不出 loc ID，留 PLAY_RANK_NONE），
    //    cut 里再按组取最小回填，理由见 kPlayableLocId 上面那段。
    dst[*dstN].rank = PlayableRankOf(g_tplTab[i].v21);
    CopyStr(dst[*dstN].full, sizeof(dst[*dstN].full), sn);
    _snprintf_s(dst[*dstN].key, sizeof(dst[*dstN].key), _TRUNCATE, "%.*s", KeyLenOf(sn), sn);
    ++*dstN;
  }

  // 排序 + 切分分组，全在发布之前做完。
  //   参数有原始表、条数、输出档位表、输出角色表，返回 {角色数, 档位数, 其中可玩角色数}
  struct Out {
    int chars;
    int tiers;
    int playChars;  // 其中可玩职业有几个（只用于日志：置顶的那批）
  };
  // cut 做四件事，按组回填可玩序号、按组算显示名、排序（可玩置顶）、切分发射。
  //   要两趟排序，第一趟用 SummonRawKeyCmp 按 key 把同组排到一起，才谈得上"按组取序号"，
  //   显示名也在这一趟里跟着算好（第二趟的比较器看不见"组"）；
  //   第二趟用 SummonRawCmp 排最终顺序。两边都 ≤320 条、每 30 秒才一次，成本可忽略。
  auto cut = [&](SummonRaw *r, int n, TierEntry *t, CharEntry *c) -> Out {
    if (n > 1)
      qsort(r, (size_t)n, sizeof(SummonRaw), SummonRawKeyCmp);
    int playChars = 0;
    for (int i = 0; i < n;) {
      const int j0 = i;
      // 1)  先找出"同一个角色"的整段（key 相同必然连续，上面刚用 SummonRawKeyCmp 排过）
      //     这里也必须用 _stricmp，和两个比较器保持一致，理由见 SummonRawCmp 那边。
      while (i < n && _stricmp(r[i].key, r[j0].key) == 0)
        ++i;
      // 1.5) 组的可玩序号 = 组里最小的那个档位下标，再回填给整组。
      //      必须按组取，档位的 DisplayName 是档位专属的（实测 AIMushroom_Hunt_Elite、
      //      AIEngineer_RareMob_Elite 都不在那 23 个职业名单里），只有基础档命中。
      int best = PLAY_RANK_NONE;
      for (int a = j0; a < i; ++a) {
        if (r[a].rank < best)
          best = r[a].rank;
      }
      if (best != PLAY_RANK_NONE)
        ++playChars;
      // 1.6) 组的显示名一并算好，写进组里每一项。第二趟排序要按它排字典序，
      //      而那一趟是 qsort 的裸比较器，只看得见单个元素，问不出"它属于哪一组"，
      //      所以只能提前摊到每个元素上。同组各项写的是同一份，组继续连着。
      //
      // 组里的 key 可能有两种大小写（RoyalHypnoFlower / RoyalHypnoflower），
      //   显示哪一个取"这一组里出现次数最多的那个拼法"，平局取排序在前的。
      //   两种都是确定的，循环按排序顺序走，且只在严格更多时才换。
      const char *grpKey = r[j0].key;
      int grpBest = 0;
      for (int a = j0; a < i; ++a) {
        int cnt = 0;
        for (int b = j0; b < i; ++b) {
          if (strcmp(r[a].key, r[b].key) == 0)
            ++cnt;
        }
        if (cnt > grpBest) {
          grpBest = cnt;
          grpKey = r[a].key;
        }
      }
      // 显示名优先用这个角色"基础档"的游戏内名字（Elite 那些档位自己的名字不管）。
      //   基础档 = 没有后缀的那一档（TierLabelOf 给 "(base)"）；基础档没名字就退一步，
      //   组里任何一个档位能查到就用它；整组都查不到才用模板代码名。
      char disp[SUM_NAME_MAX] = {0};
      bool haveDisp = false;
      for (int a = j0; a < i && !haveDisp; ++a) {
        if (strcmp(TierLabelOf(r[a].full), "(base)") == 0)
          haveDisp = UnitDisplayName(r[a].v21, disp, sizeof(disp));
      }
      for (int a = j0; a < i && !haveDisp; ++a)
        haveDisp = UnitDisplayName(r[a].v21, disp, sizeof(disp));
      const char *shown = haveDisp ? disp : grpKey;
      for (int a = j0; a < i; ++a) {
        r[a].rank = best;
        CopyStr(r[a].disp, sizeof(r[a].disp), shown);
      }
    }
    // 2)  最终排序，可玩职业按游戏顺序置顶，其余按显示名字典序跟在后面
    if (n > 1)
      qsort(r, (size_t)n, sizeof(SummonRaw), SummonRawCmp);
    int nc = 0, nt = 0;
    for (int i = 0; i < n && nc < SUM_CHAR_MAX && nt < SUM_LIST_MAX;) {
      const int j0 = i;
      while (i < n && _stricmp(r[i].key, r[j0].key) == 0)
        ++i;
      // 3)  组内按血量从小到大重排（不要字典序）
      qsort(r + j0, (size_t)(i - j0), sizeof(SummonRaw), TierHpCmp);
      // 4)  再原样抄进输出表。角色名用上面 1.6) 算好那份，组里每一项都写的是同一个。
      c[nc].lo = nt;
      CopyStr(c[nc].key, sizeof(c[nc].key), r[j0].disp);
      for (int k = j0; k < i && nt < SUM_LIST_MAX; ++k) {
        t[nt].v21 = r[k].v21;
        CopyStr(t[nt].label, sizeof(t[nt].label), TierLabelOf(r[k].full));
        ++nt;
      }
      c[nc].hi = nt;
      ++nc;
    }
    Out o = {nc, nt, playChars};
    return o;
  };

  Out oA = cut(tmpA, nA, tA, cA);
  Out oE = cut(tmpE, nE, tE, cE);

  // 发布前确认闸门还在自己手里，本线程若是被超时抢占过，这份结果可能是两份写者
  //   交错出来的，发布出去会把坏数据交给读方。被抢过就整份丢弃，也不放闸
  //   （闸现在在接手者手里）。
  if ((LONG64)AtomicReadL64(&busyGate) != claim) {
    Log("[LIST] this rebuild was taken over while it ran; result dropped unpublished");
    // 本线程被抢占之后恢复执行时，上面那句置奇可能已经把接手者补成的偶数
    //   又推成了奇数；这条出口不补偶的话，版本号就会停在奇数上：读方接下来每次都烧完
    //   64 圈再落到兜底的无保护读，直到下一次重建（最迟 30 秒）才自愈。
    //   只在闸门确实空着时补：闸门非空说明接手者还在写，补偶会把读方放进去。
    if (!AtomicReadL64(&busyGate))
      EnsureListGenParity(false);
    return;
  }

  PublishSide(g_allyTiers, tA, sizeof(TierEntry) * (size_t)oA.tiers, g_ally, cA,
              sizeof(CharEntry) * (size_t)oA.chars, &g_allyN, oA.chars);
  PublishSide(g_enemyTiers, tE, sizeof(TierEntry) * (size_t)oE.tiers, g_enemy, cE,
              sizeof(CharEntry) * (size_t)oE.chars, &g_enemyN, oE.chars);

  ClampAllSels();
  InterlockedExchange(&g_hud_dirty, 1);
  // 这一行发布版也要打：进不了列表的模板必须打日志。逐条明细一场四千多行太重
  //   （见上面那个 LOG_DEV），所以发布版只留这一行汇总。
  // 只在丢弃数变了才打：同一关里每 30 秒重建一次，数字不变的那些重复行没有信息量；
  //   而数字一变就说明这一轮又出现了进不了列表的模板，正好当它的信号。
  static volatile LONG droppedNShown = -1;
  if (droppedN != (int)AtomicReadL(&droppedNShown)) {
    InterlockedExchange(&droppedNShown, droppedN);
    Log("[LIST] %ld templates scanned, %d dropped (no AISpawnData of their own), %d kept", total,
        droppedN, oA.chars + oE.chars);
  }
  // 下面这几行是"每次重建一份"的明细，发布构建里整组编译掉。
  LOG_DEV(
      "[LIST] ordering: playable characters first (roster order = the kPlayableLocId list),"
      " everything else after them in display-name order;  first loc ID read = %s",
      firstId[0] ? firstId : "(unreadable)");
  LOG_DEV(
      "[LIST] summon lists rebuilt: ally %d chars/%d tiers (%d playable), enemy %d chars/%d"
      " tiers (%d playable)  (my team=%ld, key sort case-insensitive)%s",
      oA.chars, oA.tiers, oA.playChars, oE.chars, oE.tiers, oE.playChars, myTeam,
      (oA.chars + oE.chars == 0) ? "  no characters at all, template table not scanned yet?" : "");
  // 第三方阵营的条数（Gnome / Snowman 那些，名字里没写 Plant/Zombie，一律算敌人）
  LOG_DEV(
      "[LIST] third faction (Gnome/Snowman etc, no faction in name) %d items, all in enemy"
      " column",
      nThird);
  EnsureListGenParity(false);                         // 重建完成，读方现在可以放心读了
  InterlockedCompareExchange64(&busyGate, 0, claim);  // 放闸（只放自己的）

  // 诊断用，把第一个队友角色的档位顺序连同血量打一行，
  //   用来核对"档位列按血量升序"真的生效了（光看名字看不出顺序对不对）。
  //   整块只在开发构建里编：它拼出来的 sample 只喂下面那行日志，发布版里没人读。
#ifdef _DEV
  if (oA.chars > 0) {
    char sample[400] = {0};
    int off = 0;
    for (int k = g_ally[0].lo; k < g_ally[0].hi && off < 300; ++k) {
      // 打的是排序键本身（TierSortHp），不是原始读数：只有这样才能拿它核对
      //   "档位列真的按血量升序"。两个非数值的含义分开显示，不然会打成 hp0 / hp1000000000。
      const float hk = TierSortHp(g_allyTiers[k].v21);
      char hbuf[24];
      if (hk <= 0.0f)
        _snprintf_s(hbuf, sizeof(hbuf), _TRUNCATE, "default");
      else if (hk >= 1.0e8f)
        _snprintf_s(hbuf, sizeof(hbuf), _TRUNCATE, "unknown");
      else
        _snprintf_s(hbuf, sizeof(hbuf), _TRUNCATE, "hp%.0f", (double)hk);
      int w = _snprintf_s(sample + off, sizeof(sample) - (size_t)off, _TRUNCATE, "%s%s(%s)",
                          (k == g_ally[0].lo) ? "" : "  ", g_allyTiers[k].label, hbuf);
      if (w <= 0)
        break;
      off += w;
    }
    LOG_DEV(
        "[LIST] tier order sample [%s]: %s   "
        "(sort key in parens: hpN = override health; default = no override, template default, "
        "weakest; unknown = no AD found at all, last)",
        g_ally[0].key, sample);
  }
#endif
}

// levelChange = true 表示"换关了，上一关的资产可能已经释放"。
//   它管两件事：
//     1) 要不要先把召唤列表清空：
//          换关，必须清（旧的 v21 会悬垂）
//          同一关内重扫，不清（资产还是那批对象，v21 依然有效；清了反而让 HUD 空一下，
//                             而且那几百毫秒里按召唤键会失败）
//     2) 扫描中途被打断（aborted）时，旧表能不能留着，见下面发布前那一段。
//   要"同一关内重扫"是因为这个函数枚举的是当前已加载的 Template 实例，
//   而 BFN 按关卡流式加载资产：换关时扫一次抓不到"进关之后才加载进来"的东西
//   （比如某些 BOSS 的模板，要走到 BOSS 区才加载）。实测：Dreadwood 那只 BOSS
//   (Template_Plant_AIDreadwood) 从头到尾没被扫到过，而小镇的 BlightCap
//   (Template_Plant_AIGiantNightCapBoss) 正常在列表里：差别就是加载时机。
static void ScanTemplates(bool levelChange) {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base)
    return;
  unsigned long long want = (unsigned long long)(uintptr_t)(base + RVA_TYPEDESC_TPL);
  // 扫到坏页被打断时置 1。见下面发布前那段：同一关内就保留旧表，换关则不能保留。
  bool aborted = false;

  // 扫描窗口和预算。
  //   预算必须一次给够，它是按整块已提交 region 累加的、超额即跳出，
  //   给少了扫描会中途停，一大批模板在 AD 表里查不到，召唤出来就是
  //   "片状拉丝 + 套着载体的 AI"（退回载体 holder，Blueprint/BehaviorTree 都是载体的）。
  //   实测 768MB 够用，走到 0x60B20000（越过上界）且只用了 656/768MB，再扩无益。
  //   "有没有自己的 AISpawnData"并不是"是不是可玩角色"的判据：
  //   扫描补齐后 BOSS / 盆栽 / 地精全都有 AD，被滤掉的只剩 10 个废稿/道具。
  const unsigned long long LO = 0x20000000ULL, HI = 0x60000000ULL;
  // 这几个计数器必须是 volatile，它们在 __try 里被改、在 __except 之后被读。
  //    MSVC 的规定是这种变量必须 volatile：否则编译器可能把它留在寄存器里，
  //    异常走 __except 展开那一趟之后值就丢了（读到的会是异常前的旧值或垃圾）。
  volatile unsigned long long scanned = 0;
  const unsigned long long budget = 768ULL << 20;
  volatile int found = 0, foundAd = 0;
  volatile unsigned long long aScanEnd = LO;  // 扫描停在哪（诊断用）

  // AISpawnData 的运行时类型描述符，只能从抓到过的真 holder 的 +8 上读。
  //   adDesc 通常不是 0，GetAdDescriptor 会退回写死的常量，所以这一轮照样找得到。
  //   下面那句"（还没有，这轮不找它）"只在连模块基址都拿不到时才会出现。
  unsigned long long adDesc = GetAdDescriptor();
  // BuffData 的描述符是写死的常量，不需要等任何运行时事件（理由同 RVA_TYPEDESC_AISPAWNDATA）。
  const unsigned long long buffDesc =
      base ? (unsigned long long)(uintptr_t)(base + RVA_TYPEDESC_BUFFDATA) : 0;

  // 这里不清 g_tplN/g_adN：扫描写进 s_scanTpl/s_scanAd，扫完才一次性发布（见那两张表的说明）。
  //   换关那条路会由 ResetInjectState 先把它们清掉。
  // 扫描期间的本地计数（只有 Worker 在扫，不需要原子）。
  // 同样必须 volatile，它们在 __try 里被改、在 __except 之后被读（见上面那行说明）。
  volatile int scanTplN = 0, scanAdN = 0, scanBuffN = 0;
  // 换关时必须立刻清空两张召唤列表：它们存的是上一关的模板实例指针（v21），
  //   和 g_adN 是同一类悬垂快照。扫描要几百毫秒，这期间 HUD 还挂着旧列表，
  //   按召唤键就会把一个悬垂（甚至已被复用）的 v21 注入进去。
  // 清零让 HUD 立刻变成空列表（召唤键会明确报那一列还没有角色，
  //   日志里是 FAIL no selection … column），扫完由函数末尾的 BuildSummonLists 填回来。
  //   这里不重置选中光标，换关时选好的角色名往往还想沿用，
  //     重建后 ClampAllSels 会把越界的夹回 0。
  //
  // 同一关内的重扫不要清，那种情况下资产还是那批对象，旧 v21 依然有效；
  //    清了只会让 HUD 空几百毫秒，而这段时间按召唤键还会失败。
  if (levelChange) {
    // 清列表也必须走 seqlock。不走的话，一个正在飞行中的召唤读会"前后版本号相同"地
    //   穿过检查，带着 pre-clear 的 v21 去排队：那是上一关的指针，已经悬垂。
    // 加两次不是"变回原样"，数值确实变了，读方比对的是值不是奇偶。
    // 收尾只在自己是"把它置奇的那一个"时做：BuildSummonLists 可能正在按键线程上重建
    //   （它把版本号停在奇数），替它收尾就是替它开门。
    // 拿写权要重试：对方一次重建只有几十毫秒，这里等一小会儿比"清在半截上"划算。
    bool listGenOurs = EnsureListGenOddMine();
    for (int i = 0; i < 64 && !listGenOurs; ++i) {
      SwitchToThread();
      listGenOurs = EnsureListGenOddMine();
    }
    if (listGenOurs) {
      InterlockedExchange(&g_allyN, 0);
      InterlockedExchange(&g_enemyN, 0);
      EnsureListGenParity(false);  // 这次失效结束
    } else {
      // 抢不到写权就不清：清零本身不在同一个快照里，读方仍会看到"新计数 + 旧表"。
      //   改为请求补建，让下一次重建按新表把列表整体换掉。
      InterlockedExchange(&g_listWant, 1);
    }
    InterlockedExchange(&g_hud_dirty, 1);
  }
  LOG_DEV(
      "[TPL] scanning 0x%llX~0x%llX for Template instances (type descriptor %p); "
      "AISpawnData descriptor %p%s",
      LO, HI, (void *)want, (void *)adDesc,
      adDesc ? "" : " (not available yet, skipped this round)");

  // 整个扫描套一层 SEH，里面是直接读内存，万一某页在扫描途中被游戏释放，
  // 就在这里收住，而不是让整个 DLL 崩掉。
  bool capHit = false;    // 模板表撞到 SCAN_TAB_MAX 上限，提前收尾（见下面 ++found 那处）
  bool adCapHit = false;  // AD 表撞上限：不中断扫描，但要出声（见下面存 AD 那处）
  __try {
    for (unsigned long long a = LO; a < HI && scanned < budget && !capHit;) {
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery((LPCVOID)a, &mbi, sizeof(mbi)) != sizeof(mbi))
        break;
      unsigned long long rbase = (unsigned long long)mbi.BaseAddress;
      unsigned long long rsize = (unsigned long long)mbi.RegionSize;
      if (rsize == 0)
        break;

      bool ok = (mbi.State == MEM_COMMIT) && !(mbi.Protect & PAGE_GUARD) &&
                (mbi.Protect &
                 (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE));
      if (ok) {
        unsigned long long s = (rbase < LO) ? LO : rbase;
        unsigned long long e = rbase + rsize;
        if (e > HI)
          e = HI;
        // 从 s+8 起，命中时 obj = p-8，若 p 正好是区域头，obj 会落到前一个区域
        //    （可能未映射，或那里的数据根本不属于本次扫描范围）。
        for (unsigned long long p = s + 8; p + 8 <= e; p += 8) {
          // 这里直接读，不走 Rd64：Rd64 每次都带一次 SEH（约 1µs），
          //    而扫描是千万次级别，那样会让游戏卡住好几秒。
          //    安全性由两点保证，1)  页已用 VirtualQuery 确认 COMMIT 且可读；
          //    2)  整个扫描外面套了一层 SEH（撞到坏页就停）。
          unsigned long long q = *(volatile unsigned long long *)p;
          if (q == want) {
            // 1)  Template 实例：描述符在它的 +8，所以实例地址 = p - 8
            unsigned long long obj = p - 8;
            if (obj < MIN_VALID_ADDR)
              continue;
            char tpl[SUM_NAME_MAX] = {0};
            ReadAssetPath(obj, tpl, sizeof(tpl));
            // 同一关内的重扫只在"这个模板是这轮新出现的"时才打这一行。
            //   否则每 30 秒把 340 个模板全打一遍，日志一小时涨 9MB，把有用的行淹掉；
            //   而重扫本来就是为了抓"进关之后才流式加载进来"的那些：打新增的正好。
            //   换关那次（levelChange）还是全打，那时候全表都是新的，也正需要核对。
            // 判"新不新"直接比 g_tplTab：它在这次扫描结束前还装着上一轮的内容
            //   （发布在函数末尾，同一线程，不会边扫边改）。比对 obj 要线性找一遍上一轮的表，
            //   而发布版里 isNew 唯一的读者就是下面那行日志，所以整段只在开发构建里编。
#ifdef _DEV
            bool isNew = levelChange;
            if (!isNew) {
              LONG pn = AtomicReadL(&g_tplN);
              isNew = true;
              for (LONG k = 0; k < pn; ++k) {
                if (g_tplTab[k].v21 == obj) {
                  isNew = false;
                  break;
                }
              }
            }
            if (isNew)
              LOG_DEV("[TPL]   instance @%p   Template = %s", (void *)obj,
                      tpl[0] ? tpl : "(path unreadable)");
#endif
            // 存进临时表（扫完一次性发布，见 s_scanTpl 的说明）。
            //   扫完后 BuildSummonLists 会把它按阵营分到 HUD 的两列里。
            //
            // 判据是路径里必须含 Template_。
            //   这条启发式是"描述符命中即模板"，而描述符会被非模板对象引用到：
            //   实测混进来过 C 和 PVZCharacterSpawnTemplateData[]（都是类型名，
            //   不是资产实例）。它们现在会被下面那道 AD 过滤挡掉，但列表和 AD 表是
            //   两次独立扫描，理论上存在"假对象的 +48 恰好等于某个真模板"的组合，
            //   那时 FindOwnAd 会把不相干的对象当 AISpawnData 用。
            //   实测 341 个扫到的名字里，不含 Template_ 的只有 3 个（上面列了其中两个），
            //   真模板无一例外，安全。
            if (scanTplN < SCAN_TAB_MAX && tpl[0] && strstr(tpl, "Template_")) {
              s_scanTpl[scanTplN].v21 = obj;
              CopyStr(s_scanTpl[scanTplN].path, sizeof(s_scanTpl[0].path), tpl);
              ++scanTplN;
            }
            ++found;
            if (found >= SCAN_TAB_MAX) {
              // 用标志跳出，不能直接 return：那会跳过函数末尾的 BuildSummonLists，
              // 而列表在函数开头刚被清空，HUD 会一直空着，直到换队或再重扫一次。
              Log("[TPL] (Template count hit the %d cap, stopping)", SCAN_TAB_MAX);
              capHit = true;
              break;
            }
          } else if (adDesc && q == adDesc) {
            // 2)  AISpawnData 实例：同样，描述符在 +8，实例地址 = p - 8。
            //    记下 {它用的模板（+48）、它自己}，召唤时优先用它当 holder（见 g_adTab 的说明）。
            unsigned long long obj = p - 8;
            if (obj < MIN_VALID_ADDR)
              continue;
            unsigned long long tpl = 0;
            if (Rd64(obj + 48, &tpl) && tpl) {
              if (scanAdN < SCAN_TAB_MAX) {
                s_scanAd[scanAdN].tpl = tpl;
                s_scanAd[scanAdN].ad = obj;
                ++scanAdN;
              } else if (!adCapHit) {
                // 撞上限要出声。表满之后没存下的那些 AD，会让对应模板在重建时被记成
                //   "没有自己的 AISpawnData"，那行汇总日志报的就是错的理由。
                //   模板表那一侧撞上限会整个停扫，这里不停，所以只能靠这行日志说清楚。
                adCapHit = true;
                Log("[TPL] (AISpawnData count hit the %d cap, the rest are not stored;"
                    " templates whose AD was not stored will be counted as having none)",
                    SCAN_TAB_MAX);
              }
            }
            ++foundAd;
          } else if (buffDesc && q == buffDesc) {
            // 3)  BuffData 实例：只留出生 buff 表用得到的那些。
            //     同一个资产是共享实例，所以按下标去重，留住第一次见到的地址。
            unsigned long long obj = p - 8;
            if (obj < MIN_VALID_ADDR)
              continue;
            char bpath[SUM_NAME_MAX] = {0};
            if (ReadAssetPath(obj, bpath, sizeof(bpath)) && bpath[0]) {
              const int bi = BuffIndexOfPath(bpath);
              if (bi >= 0) {
                bool dup = false;
                for (int k = 0; k < (int)scanBuffN && k < BUFF_TAB_MAX; ++k)
                  if ((int)s_scanBuff[k].idx == bi) {
                    dup = true;
                    break;
                  }
                if (!dup && scanBuffN < BUFF_TAB_MAX) {
                  s_scanBuff[scanBuffN].obj = obj;
                  s_scanBuff[scanBuffN].idx = (unsigned char)bi;
                  ++scanBuffN;
                }
              }
            }
          } else {
            continue;
          }
        }
        scanned += (e - s);
      }
      a = rbase + rsize;
      aScanEnd = a;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    aborted = true;
    Log("[TPL] !! hit a bad page while scanning, stopping early (%llu MB scanned, "
        "%d Templates / %d AISpawnData)",
        scanned >> 20, found, foundAd);
  }
  // 把"停在哪"打出来，停在 HI 附近 = 窗口扫完了；离 HI 还远 = 预算耗尽（那就还要再放大）。
  LOG_DEV(
      "[TPL] done: %d Templates, %d AISpawnData (%llu MB of committed memory scanned, "
      "reached 0x%llX, window 0x%llX~0x%llX)%s",
      found, foundAd, scanned >> 20, (unsigned long long)aScanEnd, LO, HI,
      (aScanEnd >= HI - 0x1000000ULL) ? "  (window fully scanned)" : "  (budget exhausted)");
  // 中途被打断就整轮作废，保留上一轮的完整表。
  //   关卡流式卸载正好压在扫描窗口里时，扫到一半就会撞到被释放的页；
  //   这时候 publish 的是"扫了一半的份数"，极端情况是 0：表被清空、列表跟着清空，
  //   所有召唤都报 stale target，而且换关那次 ResetInjectState 已经先把表清了，
  //   要等 30 秒后的下一轮才恢复。旧表总比半张表好：它至少是完整的。
  if (aborted && !levelChange) {
    Log("[TPL] scan aborted, keeping the previous table (%d Templates / %d AISpawnData);"
        " the next pass will retry",
        (int)AtomicReadL(&g_tplN), (int)AtomicReadL(&g_adN));
    return;
  }
  // 换关那次不能保留，旧表全是上一关的地址，留着就是把悬垂指针继续发给调用方。
  //   扫到多少就发布多少（可能是 0，那就是真的清空，这是换关应有的结果）。
  if (aborted)
    Log("[TPL] scan aborted during a level change, publishing the partial result"
        " (the old table belongs to the previous level and must not be kept)");
  // 一次性发布：先拷内容，最后才写计数。
  if (scanTplN > 0)
    memcpy(g_tplTab, s_scanTpl, sizeof(TplEntry) * (size_t)scanTplN);
  InterlockedExchange(&g_tplN, scanTplN);
  if (scanAdN > 0)
    memcpy(g_adTab, s_scanAd, sizeof(AdEntry) * (size_t)scanAdN);
  InterlockedExchange(&g_adN, scanAdN);
  // buff 表无条件发布（含 0）：它存的是资产区里的活指针，绝不能让上一关的残留留着。
  //   扫不到只会让补挂跳过，不会误用别的对象的地址。
  if (scanBuffN > 0)
    memcpy(g_buffTab, s_scanBuff, sizeof(BuffEntry) * (size_t)scanBuffN);
  InterlockedExchange(&g_buffN, scanBuffN);
  // 扫到几个 BuffData 直接报出来。补挂要用的资产是流式加载的，进关前后数量会变，
  //   所以只在数字变化时打一行，既能看到"一个都没扫到"这种硬失败，又不会每轮刷屏。
  {
    static volatile LONG lastBuffN = -1;
    if (AtomicReadL(&lastBuffN) != scanBuffN) {
      InterlockedExchange(&lastBuffN, scanBuffN);
      Log("[BUFF] BuffData instances found: %d of %d", (int)scanBuffN, kBuffNameCount);
    }
  }

  // 扫完立刻重建 HUD 的两列（按阵营分 + 可玩置顶，其余按显示名字典序）。这是列表的唯一数据来源。
  BuildSummonLists();
}

// 从我们自己的线程调游戏函数 sub_141D5BE30，必须包 SEH。
//
//   它会走链表和哈希表。并发的 spawner 释放在那里面是常态，另外引擎 TLS 在我们线程上
//   是空的，从非游戏线程调游戏函数曾正是这么崩的（code=0xC0000027，访问的地址=0x18）。
//   不包的话就是未处理异常，整个游戏进程直接死。Worker 那条路上没有任何处理者，
//   而 ValidCarrier 那条更糟：游戏正跑在它自己的 AI 调度 tick 里，把它一起带走。
//
//   抛异常时返回 0，调用方按"没拿到"处理（现有的每个调用点本来就判空）。
static EXCEPTION_POINTERS *g_excFC = nullptr;
static LONG ExcFilterFC(EXCEPTION_POINTERS *p) {
  g_excFC = p;
  return EXCEPTION_EXECUTE_HANDLER;
}

// 从异常记录里取出查问题要用的那几个数：异常代码、出错指令的 RVA、访问的地址。
//   这里只认传进来的 EXCEPTION_POINTERS，不碰 g_exc 与 g_excFC：两个全局各自对应
//   一条异常路径，不能混用。用哪个、要不要顺手清空、日志怎么拼，全部留在调用点。
//   没有异常记录时返回 false 并把各字段留 0。
//   字段按值填，不写有构造函数的成员：这个函数要在 __except 体里被调用，
//   那里多一个有析构的东西就会撞上 C2712。
struct ExcFacts {
  unsigned int code;
  unsigned long long rva;
  unsigned long long accessed;  // 只有带两个参数的异常才有效，见下面那句判断
  void *faultAddr;
};

static bool ExcFactsOf(EXCEPTION_POINTERS *p, ExcFacts *out) {
  out->code = 0;
  out->rva = 0;
  out->accessed = 0;
  out->faultAddr = nullptr;
  // 先判这两层再解引用：这个函数是在异常处理里跑的，在这儿再抛一次没有上层接得住。
  if (!p || !p->ExceptionRecord)
    return false;
  EXCEPTION_RECORD *r = p->ExceptionRecord;
  out->code = (unsigned int)r->ExceptionCode;
  out->faultAddr = r->ExceptionAddress;
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (base && r->ExceptionAddress)
    out->rva = (unsigned long long)((unsigned char *)r->ExceptionAddress - base);
  // 只有"带两个参数"的异常（访问违例那种）第 2 个参数才是出错地址，
  //   别的异常码下读它是未定义的，会打出一个误导人的地址。
  if (r->NumberParameters >= 2)
    out->accessed = (unsigned long long)r->ExceptionInformation[1];
  return true;
}
// idx 的类型跟 FindCtxFnT 对齐（char），别写成 int：那会是一条 int->char 收窄告警
//   （/W4 的 C4244）。调用点传 0 或 1/2（全局按队查找），类型必须一致。
static void *SafeFindCtx(void *player, char idx) {
  if (!g_findCtx)
    return nullptr;
  __try {
    return g_findCtx(player, idx);
  } __except (ExcFilterFC(GetExceptionInformation())) {
    ExcFacts ex;
    ExcFactsOf(g_excFC, &ex);
    Log("[SCAN] !! sub_141D5BE30 threw, code=0x%08X RVA=0x%llX fault address=0x%llX, treating as "
        "no result",
        ex.code, ex.rva, ex.accessed);
    return nullptr;
  }
}

// ---------------------------------------------------------------------------
//  载体特征表，一个 spawner 身上观测得到的事实，换关时一行一个节点打出来（LogCarrierRow）。
//
//   注意 tpl/bp 记的是"最近一次看到的生成"，不是这个节点的固有属性：实测同一个 ctx 在
//   同一关内会一会儿造野怪、一会儿造人机（人机用 Mp 开头的蓝图，野怪用 AI 开头的），
//   因为那一份 AISpawnData 是每次生成各带一份的。所以这一列读法是"它最近在造什么"，
//   不是"它是什么节点"。
//
//   事实分两处观测，写进同一张表：
//     - 原生模板 / 原生蓝图 / 生成期的 vt456 只能在生成进行中抓（那时 ctx+840 才有值），
//       写入点是 WORLD 钩子里那次 NoteCarrierBuild；
//     - 引擎有没有用"把玩家放进世界"那条路走过它，什么时候都能记，
//       写入点是 TRYSPAWN 钩子里的 NoteCarrierPlayerPath。
//   两个写入点都在游戏线程上，读方是 Worker。发布用关卡代数加环形下标两道，见下面。
//
//   ctx         spawner 对象
//   playerPath  被 sub_141D6D9B0（把玩家放进世界那条路）走过
//   vt456       它原生生成时 ctx->vt[456]() 的返回值，也就是 sub_141D62790 拿来分流的
//               v15（0 撤单 / 2 不分配槽位直接放 / 400 持久 AI / 其余走玩家槽位分配）
//   tpl, bp     它最近一次生成在造的模板（holder+48）和蓝图（holder+56）
//   没观测到的字段保持初值（vt456 = -1，其余空），dump 里显示 "-"。
//   已经发布过的行会被原地改写（同一个 ctx 又生成一次），而 Worker 可能在那一刻正在读，
//   最坏读到"一半旧一半新"的路径字符串。vt456 / tpl / bp 这三个是诊断用的，
//   一行对不上不影响判断；但同一张表的 playerPath 有功能读方（IsOwnSpawnPoint 挡载体），
//   那一个字段被撕裂就不再是"不影响判断"，别把这条结论套到它身上。
// ---------------------------------------------------------------------------
// 表的大小要盖得住一关里的 spawner 数（实测一关最多走到 151 个节点），再多就是环形覆盖。
//   给这么大是为了不出"收满之后静默不再记录"那种状态，它只是容量，不是业务上限。
constexpr int CARRIER_MAX = 512;
struct CarrierInfo {
  unsigned long long ctx;
  long long vt456;
  unsigned int playerPath;
  unsigned int gen;  // 这一行属于哪一关，0 表示空行（见 g_carrierGen）
  char tpl[SUM_NAME_MAX];
  char bp[SUM_NAME_MAX];
};
static CarrierInfo g_carriers[CARRIER_MAX];
// 关卡代数。换关只把它加一，上一关的行立刻全部作废，不去清内容：
//   清内容会和游戏线程的写入交错（刚发布的行被清掉、写入方还得回头补偿），
//   而加一只改读方的校验条件，没有中间态。
static volatile LONG g_carrierGen = 1;
// 环形游标。写入点只有游戏线程那两处，取号之外不需要同步；满了覆盖最老的一行，
//   绝不收满就停。当前那个生成点另有 g_recentMine 与 SafeFindCtx 兜着，这张表是补充判据。
static volatile LONG g_carrierNext = 0;

// 查 ctx 在这一关（当前代数）里的行，查不到返回空。
static const CarrierInfo *FindCarrier(unsigned long long ctx) {
  const unsigned int gen = (unsigned int)AtomicReadL(&g_carrierGen);
  for (int i = 0; i < CARRIER_MAX; ++i) {
    if (g_carriers[i].gen == gen && g_carriers[i].ctx == ctx)
      return &g_carriers[i];
  }
  return nullptr;
}

// 找 ctx 在这一关里的行；没有就占一行新的。
//   占下来的行先按 gen = 0 作废，调用方填完字段再调 PublishCarrierRow 一次性发布，
//   读方只认代数对得上的行。
//   查行那一段交给 FindCarrier（同一段循环只留一份），但拿到之后必须再比一次代数：
//   它用的是"当前"代数，而这里要用调用方取好的那一个；中间换了关时两者不等，
//   把它当成本行的应答就等于把上一关的 ctx 标成这一关的行（见 PublishCarrierRow 那处）。
static CarrierInfo *CarrierRowFor(unsigned long long ctx, unsigned int gen) {
  CarrierInfo *found = (CarrierInfo *)(uintptr_t)FindCarrier(ctx);
  if (found && found->gen == gen)
    return found;
  const LONG slot = InterlockedIncrement(&g_carrierNext);
  CarrierInfo *r = &g_carriers[(unsigned int)(slot - 1) % (unsigned int)CARRIER_MAX];
  r->gen = 0;  // 这一行先作废，免得读方拿到半新半旧的
  r->vt456 = -1;
  r->playerPath = 0;
  r->tpl[0] = 0;
  r->bp[0] = 0;
  return r;
}

// 发布。ctx 和代数一起放最后，读方以代数对得上为准。
//   gen 用调用方取好的那一个：中间要是换了关，在这里重读 g_carrierGen
//   会把这行标成新一关的，可那是上一关的 ctx。
static void PublishCarrierRow(CarrierInfo *r, unsigned long long ctx, unsigned int gen) {
  r->ctx = ctx;
  r->gen = gen;
}

// 只在开发构建里编：它填的三个字段（vt456 / tpl / bp）唯一的读方是 LogCarrierRow，
//   而那条 dump 行也只在开发构建里，发布构建里既不写也不读。
#ifdef _DEV
static void NoteCarrierBuild(unsigned long long ctx, const char *tpl, const char *bp,
                             long long vt456) {
  if (ctx == 0)
    return;
  const unsigned int gen = (unsigned int)AtomicReadL(&g_carrierGen);
  CarrierInfo *r = CarrierRowFor(ctx, gen);
  r->vt456 = vt456;
  CopyStr(r->tpl, sizeof(r->tpl), tpl ? tpl : "");
  _snprintf_s(r->bp, sizeof(r->bp), _TRUNCATE, "%s", bp ? bp : "");
  PublishCarrierRow(r, ctx, gen);
}
#endif

static void NoteCarrierPlayerPath(unsigned long long ctx) {
  if (ctx == 0)
    return;
  const unsigned int gen = (unsigned int)AtomicReadL(&g_carrierGen);
  CarrierInfo *r = CarrierRowFor(ctx, gen);
  r->playerPath = 1;
  PublishCarrierRow(r, ctx, gen);
}

// 游戏此刻正在拿这个 ctx 生成 AI（SPAWN 钩子在 AI 路径上抓到它）：把"这个 ctx 当过我们的
//   spawner"的旧记录撤掉，当前观测优先于历史记录。
//   两条记录都是有写无消的（playerPath 整关有效、退役环要等被挤出 8 槽），而 spawner 池子小的
//   模式里玩家的 spawner 会在同一小组地址里反复轮转，于是整池子很快全被记上案底，一个都挑不出来。
//   硬判据（ctx 就是当前玩家的 spawner）不在撤销范围内，仍然照旧拒绝。
//   撤销到了就打一行（限前 8 次），方便对着日志看这条规则到底有没有在起作用。
static void NoteCarrierAiSpawn(unsigned long long ctx) {
  if (ctx == 0)
    return;
  bool cleared = false;
  // 表里的行是就地改写的（见载体特征表那段），这里只清一个标志位。
  CarrierInfo *r = (CarrierInfo *)(uintptr_t)FindCarrier(ctx);
  if (r && r->playerPath) {
    r->playerPath = 0;
    cleared = true;
  }
  // 环里有没有它，由 ForgetRecentOwnSpawner 自己说（它本来就要走一遍这 8 格），
  //   这里不再单独探一遍。
  if (ForgetRecentOwnSpawner(ctx))
    cleared = true;
  static volatile LONG logged = 0;
  if (cleared && InterlockedIncrement(&logged) <= 8) {
    Log("[SCAN] ctx=%p was recorded as one of our spawners, but the game is spawning AI through it"
        " right now; clearing that record (newest observation wins)",
        (void *)ctx);
  }
}

// dump 一行。team 和 gate 现读，其余查表。
//   note 是调用方补的短注（"own，不能借"之类），直接接在行尾。
//   team 这一列在这个场合基本没有信息量：实测凡是表里记到过生成的行，dump 时它读出来
//   都是 0，反倒是没记到过生成的那些有 1/2。它只在生成期间有意义。
//
// 整块只在开发构建里编进去（一次换关 64 行，是查载体问题时才要的观测数据）。
//   表本身照常维护，IsOwnSpawnPoint 那条判据在发布版里也用它。
#ifdef _DEV
static void LogCarrierRow(int idx, unsigned long long ctx, const char *note) {
  unsigned int team = 0, gate = 0;
  Rd32(ctx + CTX_TEAM_OFF, &team);
  Rd32(ctx + CTX_GATE_OFF, &gate);
  const CarrierInfo *r = FindCarrier(ctx);
  Log("[SCAN]   [%d] ctx=%p  team=%u  gate=%u  playerPath=%s  vt456=%lld  builds=%s / %s%s", idx,
      (void *)ctx, team, gate & 0xFFu, (r && r->playerPath) ? "yes" : "no", r ? r->vt456 : -1,
      (r && r->tpl[0]) ? BaseName(r->tpl) : "-", (r && r->bp[0]) ? BaseName(r->bp) : "-",
      note ? note : "");
}
#endif

// 这个 ctx 在本关里有没有被当作过玩家自己的生成点。就是 IsRecentOwnSpawner 那一类，
//   只是判据不同，那条记的是"从玩家对象上问出来的 ctx"（含它退役后那几个），
//   这条记的是"引擎用 sub_141D6D9B0 把玩家放到它上面生成过"。那个函数做的事就是
//   把传进来的玩家对象交给这个 ctx 的生成函数（vt[232]），而且要求队号一致、ctx 启用中。
//   借这种 ctx 一样会把玩家实体销毁重生成。
//
//   只在观测到它出现时才挡（没见过的一律放行）：宁可少挡，也不要把载体挡到没有可用的。
//   判据只用观察到的事实：原生蓝图那个值每次生成都会变，同一个 ctx
//   同一关内一会儿造野怪一会儿造人机（实测），不是节点的属性。
static bool IsOwnSpawnPoint(unsigned long long ctx) {
  const CarrierInfo *r = FindCarrier(ctx);
  return r && r->playerPath;
}

// 换关之后的失效清理。三件事：丢掉旧载体、清掉上一关对象的快照、重新枚举资产。
//
//   抽成函数是因为有两个入口，正常那个（遍历之后判定的换关）和
//   "自己的 spawner 还没就绪"的提前返回。只做正常的那个是不够的：
//   玩家对象已经换了、自己的 spawner 还没建好的这段窗口（实测存在）里，
//   模板表 / holder 副本 / 载体全都还是上一关的指针，这时按 F5 就是拿悬垂指针去注入。
//   那个入口没有 ctx0 可记，传 0。
static void ClearStaleAfterLevelChange(const char *why, unsigned long long newCtx0) {
  Log("[SCAN] level change (%s), clearing carrier and snapshots", why);
  InterlockedExchange64(&g_aiCtx, 0);
  InterlockedExchange(&g_aiCtxSrc, 0);
  // 最近退役的那批玩家 spawner 也一起忘掉：换关后它们的内存会被释放，
  //   地址可能被复用给一个完全合法的 spawner，留着这个排除集就会把它永久挡掉。
  for (int i = 0; i < RECENT_MINE_SLOTS; ++i) {
    InterlockedExchange64(&g_recentMine[i], 0);
    InterlockedExchange64(&g_recentMineId[i], 0);
    InterlockedExchange(&g_recentMineSeq[i], 0);
  }
  // "刚坏掉的载体"同理，换关后地址会被复用，留着就把它永久挡掉了。
  for (int i = 0; i < RECENT_BAD_SLOTS; ++i)
    InterlockedExchange64(&g_recentBad[i], 0);
  // 载体特征表不清内容，只把代数加一，上一关那些行立刻全部作废（理由见 g_carrierGen）。
  InterlockedIncrement(&g_carrierGen);
  // 按队号选出来的候选也一起清：它只在 ScanPool 每轮重发，换关后最多两秒里还指着
  //   上一关的地址，而用它的地方只过一道 ValidCarrier。
  for (int t = 0; t < CARRIER_TEAMS; ++t)
    InterlockedExchange64(&g_carrierByTeam[t], 0);
  // 一次性诊断标记也清掉，每条只报一次，而进入关卡时的加载期最先把它们用掉
  //   （实测那条 "returned %p but it is not a valid spawner" 就是这么被消耗的），
  //   不清的话后面同一类真故障一声不响。
  InterlockedExchange(&g_scan_diag, 0);
  // 清完必须把"当前这个"补回去。它是活着的玩家 spawner，不是上一关的遗留物，
  //   而上面那次记录刚被这一轮清空抹掉，lastCtx0 又已经更新，下一轮不会再补记。
  //   少了这一步，玩家自己的 spawner 永远不在排除集里：它一旦退役（重生会换新的），
  //   就和普通 spawner 没区别，会被挑来当载体，对它调 vt[232] 会把玩家实体销毁重生成。
  if (newCtx0)
    PushRecentOwnSpawner(newCtx0);
  ResetInjectState();
  ScanTemplates(true);  // true = 换关了：必须清空召唤列表（旧 v21 会悬垂）
}

// "这个 ctx 是不是一个 spawner"的第二步：ctx+48 拿到的那份引用，是不是 spawner 的类型。
//   全项目只有这一条判据，免得偏移或口径要改时漏掉一处。
//   "读 +48 再问这一句"那一对由 CtxRefIsSpawner 包着（五处调用）；
//   RoleWatch 要留着 ref 自己接着读 +24，所以它自己读 +48、只调这里。
//   光看 HeapPtr 远远不够：拿个假 ctx 去做 512 步链表遍历 = 从错误基址乱走，轻则垃圾重则崩。
//   ref 为 0（外层没读到）一律判否；"ctx 非空 / HeapPtr(ctx)"由调用方自己判，
//   几个调用点对 ctx 为空的处理不一样。
static bool IsSpawnerRef(unsigned long long ref) {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base || ref == 0 || !HeapPtr(ref))
    return false;
  unsigned long long desc = 0;
  return Rd64(ref + 8, &desc) &&
         desc == (unsigned long long)(uintptr_t)(base + RVA_TYPEDESC_SPAWNREF);
}

// 上面那两步连着走：读 ctx+48，再问 IsSpawnerRef。只要个是非的调用点都用它。
//   ctx 非空 / HeapPtr(ctx) 那两条留在调用点（见上，几处口径不一样）。
static bool CtxRefIsSpawner(unsigned long long ctx) {
  unsigned long long ref = 0;
  return Rd64(ctx + 48, &ref) && IsSpawnerRef(ref);
}

// spawner 链表的"正走 + 反走"遍历（ScanPool 和 FindOwningCtx 共用）。
//   节点在 ctx+144：先沿 +0（next）从起点正走，到头再从起点沿 +8（prev）反走；
//   最多 512 步，绕回起点或读不出下一个节点就算到头。
//   visit(c) 对每个节点调用，返回 false 表示提前停；
//   onFlip() 在"正走到头、准备换方向"时调一次（只给诊断日志用）。
//   返回 true = 两个方向都走完；false = 中途停下（visit 叫停或走满 512 步）。
template <typename Visit, typename OnFlip>
static bool WalkSpawnerList(unsigned long long startCtx, Visit visit, OnFlip onFlip) {
  const unsigned long long head = startCtx + 144;
  unsigned long long node = head;
  int dir = 0;  // 0 = 正走 (*(node))，1 = 反走 (*(node+8))
  for (int k = 0; k < 512; ++k) {
    if (!visit(node - 144))
      return false;
    unsigned long long nx = 0;
    const bool dead = (!Rd64(node + (dir ? 8ull : 0ull), &nx) || !HeapPtr(nx) || nx == head);
    if (dead) {
      if (dir == 0) {
        // 这个方向到头了（或绕回起点），从起点换反方向再走一遍。
        //   反方向从起点往回的第一跳起步：起点在正向那趟已经访问过，从 head 再起步
        //   会把同一个节点访问两次，节点总数与分类计数都跟着虚高。
        dir = 1;
        onFlip();
        unsigned long long back = 0;
        if (!Rd64(head + 8ull, &back) || !HeapPtr(back) || back == head)
          return true;
        node = back;
        continue;
      }
      return true;  // 两个方向都到头了
    }
    node = nx;
  }
  return false;  // 走满上限还没到头
}

// 判否带原因：ValidCarrier 有好几条静默判否，光看调用方那一行日志看不出是哪一条拦下的。
static bool RejectCarrier(const char **whyOut, const char *why) {
  if (whyOut)
    *whyOut = why;
  return false;
}

// ctx+552 的最低字节是"这个 spawner 现在能不能生成"的开关（来龙去脉见 ValidCarrierCore 那处）。
//   三处要判它：载体校验、ScanPool 挑载体、Worker 的 20Hz 体检。只取值不判的几处
//   （dump 要把原值打出来）不用它。
static bool SpawnerGateOn(unsigned long long ctx) {
  unsigned int b552 = 0;
  return Rd32(ctx + CTX_GATE_OFF, &b552) && (b552 & 0xFFu);
}

// 校验的核心：ownSp 传"当前玩家自己的 spawner"（0 = 这一趟问不出来，就不比这一条）。
//   拆出来是为了让 ScanPool 在挑载体时就能用完整判据：它遍历前已经问过一次自己的 spawner，
//   不必每过一个节点再调一次游戏函数。
static bool ValidCarrierCore(unsigned long long ctx, unsigned long long ownSp, int wantTeam,
                             const char **whyOut) {
  if (ctx == 0 || !HeapPtr(ctx) || !CtxRefIsSpawner(ctx))
    return RejectCarrier(whyOut, "not a spawner (bad descriptor or unreadable)");
  // 也不能是玩家自己的 spawner：那会让人物从世界里消失。
  //   这里除了"当场问出来的那个"，还要挡掉最近退役的那批（见 g_recentMine）：
  //   它们和当前的那个一样致命，但 SafeFindCtx 认不出来。
  if (IsRecentOwnSpawner(ctx))
    return RejectCarrier(whyOut, "recently retired own spawner (g_recentMine)");
  // 本关里被当作过我们自己的生成点的，同样不能借，见 IsOwnSpawnPoint。
  if (IsOwnSpawnPoint(ctx))
    return RejectCarrier(whyOut, "used to place our own player this level");
  // 刚被判死的（四档全抛异常）也不能再用，见 g_recentBad 的说明。
  if (IsRecentBadCarrier(ctx))
    return RejectCarrier(whyOut, "in the recently-failed set");
  if (ownSp && ownSp == ctx)
    return RejectCarrier(whyOut, "it is the own spawner right now (SafeFindCtx)");
  // ctx+552 必须是 1。这是"这个 spawner 现在能不能生成"的开关：
  //   sub_140F4EF30 的闸门 (A) 是 if (!ctx+552 && !req+72) return 0，
  //   而我们照抄/自造的 req 恒有 +72 == 0，552 为 0 时四档全都会白跑。
  // 实测（同一局、同一个 ctx 地址，只是时间不同）：
  //   552 == 1 时 1/1 成功；552 == 0 时 0/28 全败，而且连"原样不动"档都返回 0。
  //   它还会随时间变，同一个 ctx 在两次实测之间没有换关也会从 1 变 0
  //   （日志里两条换关判据都没触发）。
  // 这种载体不是"难用"，是用不了。早点判否，让调用方丢掉它去重挑，
  //   比把四档白跑一遍好得多。
  //
  // 静态查证过它为什么会变（IDA，全代码段扫过，ctx 类里 552 只有两个写点）：
  //   它是这个 spawner 当前启用着的开关，由 ctx 自己的消息处理器
  //   sub_140F4D2C0(ctx, msg) 在事件 2662400 上切换：
  //     msg+8 != 0，sub_140F48500(ctx)：552 = 1，并把节点挂回链表
  //     msg+8 == 0，552 = 0，并把节点从链表里摘除（sub_14108A1B0(ctx+144, ctx, 0)）
  //   是游戏主动停用了它，不是内存被复用、也不是我们弄坏的。
  //   这也解释了"停用后的载体再也挑不到"：它已经被摘出链表了，
  //   ScanPool 顺着链表走根本遇不到它（这也是"丢掉坏载体再重挑"这条路成立的根据）。
  if (!SpawnerGateOn(ctx))
    return RejectCarrier(whyOut, "ctx+552 == 0 (the game disabled this spawner)");
  // 队号必须和目标一致。不符就别用，让调用方去找同队的载体（见 g_carrierByTeam）。
  //   读不到队号（返回 -1）也判否：验不了就不能用，宁可少一个候选。
  if (wantTeam > 0 && CarrierTeamOf(ctx) != wantTeam)
    return RejectCarrier(whyOut, "team mismatch (carrier team != wanted team)");
  return true;
}

// 注入前校验载体 ctx 还是不是"那个 spawner"。
//   只靠 HeapPtr 远远不够：HeapPtr 只证明"这块内存已提交、非映像、8 字节对齐"，
//      已释放、被复用成别的对象的堆块照样通过。g_aiCtx 一旦定下来就长期保留，
//      而关卡内 spawner 可能失效（波次结束/区段卸载/玩家重生换掉一批）。
//      那时我们会往别的对象的 +840 写东西，再把它当 ctx 交给生成函数，垃圾虚表，
//      轻则一次异常，重则破坏一个无关对象。
//   判据和 ScanPool 里那套一致（都走 IsSpawnerRef），且它不是玩家自己的。
// wantTeam 非 0 时要求载体的队号和它一致（见 g_carrierByTeam 那段说明）；
//   传 0 表示不挑队（重校验既有载体时用）。
// 召唤路径上的入口：自己问一次玩家的 spawner，再走核心校验。
//   问不出来就判否，不能放行。SafeFindCtx 抛异常时返回 0，而 0 == ctx 恒假，
//   所以直接写 SafeFindCtx(...) == ctx 的话，这一整条检查会静默失效、
//   把玩家自己的 spawner 当载体放进去，正是那条"人直接消失"。
//   ScanPool 那边也是同样的口径：ctx0 拿不到就直接放弃这一轮。
//   这一层判否的代价是已知的：那种"问不出来"的模式会持续，于是整局每次召唤都 FAIL。
//   Worker 那条 20Hz 的体检刻意松得多（它把不确定性当放行），两处分寸不同是有意的：
//   那里误判的代价只是少一个候选，这里误判的代价是人从世界里消失，宁可这一次不用它。
static bool ValidCarrier(unsigned long long ctx, unsigned long long me, int wantTeam,
                         const char **whyOut = nullptr) {
  if (me == 0)
    return ValidCarrierCore(ctx, 0, wantTeam, whyOut);
  const unsigned long long mine = (unsigned long long)(uintptr_t)SafeFindCtx((void *)me, 0);
  if (mine == 0) {
    Log("[INJECT] cannot query own spawner (SafeFindCtx returned 0), rejecting this carrier");
    return RejectCarrier(whyOut, "cannot query own spawner (SafeFindCtx returned 0)");
  }
  return ValidCarrierCore(ctx, mine, wantTeam, whyOut);
}

static void ScanPool() {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base) {
    SCAN_DIAG_ONCE(0, "[SCAN] no module base address");
    return;
  }

  unsigned long long mgr = GetManager();
  unsigned long long me = FindLocalPlayer(mgr, nullptr);
  if (me == 0) {
    SCAN_DIAG_ONCE(2, "[SCAN] local player not found (not grouped / not in a level) mgr=%p",
                   (void *)mgr);
    return;
  }

  // 上一轮的玩家对象 / 玩家 spawner。声明提前到这里：下面 ctx0 无效那个提前 return
  //   也要用它们判换关（见 ClearStaleAfterLevelChange）。
  static unsigned long long lastCtx0 = 0;
  static unsigned long long lastCtx0Id = 0;  // 见 SameSpawner：光比地址会被复用骗过
  static unsigned long long lastMe = 0;

  // 拿玩家 ctx 的正确办法是直接调游戏自己的查找函数 sub_141D5BE30(player, 0)。
  //
  //   让游戏自己去找：a2==0 时它直接返回"玩家自己的 spawner"。
  //   （手工复刻它内部的偏移 *(player + dword_143FBCEE0 + 208) - 8，读出来是 0。）
  //   （它的 a2!=0 分支才是遍历全局哈希表找队号匹配的，那条对我们没用。）
  FindCtxFnT findCtx = (FindCtxFnT)(base + RVA_FIND_CTX);
  g_findCtx = findCtx;  // 存一份给别处用（选载体时要当场复查）
  unsigned long long ctx0 = (unsigned long long)(uintptr_t)SafeFindCtx((void *)me, 0);

  // ctx0 到手之后还要验它真的是 spawner。"非 0 且是堆指针"这一条不够：
  //   sub_141D5BE30 在"玩家自己的 spawner"拿不到时会继续往下走，
  //   先看 *(player+off+216)，再退到全局哈希表遍历。而实测 *(player+off+208) 是 0，
  //   所以它多半走的就是后面那些分支，返回值未必是 ctx（可能是别的对象）。
  {
    if (ctx0 == 0 || !HeapPtr(ctx0) || !CtxRefIsSpawner(ctx0)) {
      // 两条备选路径一起报出来，判断该往哪查就看这里
      unsigned int off = 0;
      unsigned long long a208 = 0, a216 = 0;
      Rd32((unsigned long long)(uintptr_t)(base + RVA_DWORD_FBCEE0), &off);
      Rd64(me + off + 208, &a208);
      Rd64(me + off + 216, &a216);
      SCAN_DIAG_ONCE(3,
                     "[SCAN] sub_141D5BE30(player,0) returned %p but it is not a valid spawner. "
                     "alt: *(player+%u+208)=%p   *(player+%u+216)=%p",
                     (void *)ctx0, off, (void *)a208, off, (void *)a216);
      // 换关的清理不能等"自己的 spawner 建好"。玩家对象已经换了、而它还没就绪的
      //   这段窗口里，g_aiCtx / 模板表 / holder 副本全是上一关的悬垂指针，
      //   这时按 F5 就是拿它们去注入。挑载体照旧跳过这一轮（下面那些判据都要 ctx0）。
      //   记下 lastMe，等到 spawner 就绪那一轮时 meChanged 已为假，不会重复清。
      if (me != lastMe) {
        ClearStaleAfterLevelChange("player changed but own spawner is not ready yet", 0);
        lastMe = me;
        lastCtx0 = 0;
      }
      // 查不到自己的 spawner 时，lastCtx0 必须清掉，不能让上一轮（甚至上一个模式）的
      //   地址留在那里，它会被当成 prevCtx0 用于两处排除，而堆会把同一个地址复用给
      //   新模式的合法 spawner。实测症状：某个模式下 SafeFindCtx 一直返回 0，
      //   AI 路径交出来的载体每次都等于那个残留地址，于是被"刚退役的自己的 spawner"
      //   挡掉，而那个模式里 ScanPool 走不到挑载体那一步，等于整局没有载体可用。
      lastCtx0 = 0;
      return;
    }
  }
  SCAN_DIAG_ONCE(4, "[SCAN] diag OK: mgr=%p  me=%p  ctx0=%p (from sub_141D5BE30)", (void *)mgr,
                 (void *)me, (void *)ctx0);

  // "玩家换人了"的判定。这里只判定 + 打诊断，失效动作放到遍历之后（见函数末尾）。
  //
  //   换关判定看的是玩家对象加上链表判据，ctx0 本身变了不等于换关：
  //   玩家每次重生都会换一个新的 spawner
  //   （实测同一个玩家对象先后配过 0xCF2DFAA0 和 0xCF2DF6C0 两个 spawner），
  //   但关卡没变、资产集也没变。只看 ctx0 的话，重生一次就会把注入材料全清、
  //   召唤列表清空、再触发一次全库扫描：Ops 里死一次就要等 1 秒才有列表可召唤。
  //
  //   要判断"是不是真换关"，得看上一轮那个 spawner 还在不在链表里（见下面遍历），
  //   所以决策必须在遍历之后。
  //
  //   但"玩家对象换了"本身就已经是换关的强信号，不用再等链表判据：
  //   重生时玩家对象是不变的（实测两次都是 0xBEC7CBD0），换的只是 spawner。
  //   而链表那条判据（prevNodeAlive）只看"地址还在不在链表里"、分辨不出是不是同一代对象，
  //   理论上可能被"退役了但内存还在、还挂在链表里"骗过而漏判换关，
  //   新关里还挂着上一关的模板表（悬垂）。所以让 player 这一条能单独定案。
  const unsigned long long prevCtx0 = lastCtx0;
  const unsigned long long prevCtx0Id = lastCtx0Id;
  const unsigned long long prevMe = lastMe;
  const bool meChanged = (me != prevMe);
  const bool changed = (ctx0 != prevCtx0) || meChanged;
  // 用一个环记最近几个退役的自己的 spawner；只记 prevCtx0 一个不够，
  //   道理与实测见 g_recentMine 的定义处。
  if (ctx0 && ctx0 != prevCtx0)
    PushRecentOwnSpawner(ctx0);
  lastCtx0 = ctx0;
  lastCtx0Id = ctx0 ? SpawnerIdOf(ctx0) : 0;
  lastMe = me;
  if (changed)
    Log("[SCAN] own spawner:  ctx=%p  (player=%p%s)", (void *)ctx0, (void *)me,
        meChanged ? ", player object changed too, level change" : "");
  // 遍历里填，上一轮那个 spawner 还在不在链表里。在 = 没换关，只是玩家重生。
  bool prevNodeAlive = false;

  // ctx 是链表节点，反编译里 v21 = v20 - 18（qword 索引 -18 = -144 字节），
  //     而 v20 = *v20 是 next。所以节点就在 ctx+144，从任意一个 ctx 都能顺出去。
  //     上限 512 次节点访问（两个方向合计，不是每方向 256）；遇到回到起点就停。
  // 两个方向都要走。我们是从链中间出发的（玩家自己的 ctx），
  //    如果它是单向链表，只走 next 就只能到尾部、漏掉前面所有节点。
  //    Frostbite 的 List 惯例是 { next, prev }，所以 +0 和 +8 各试一遍。
  //
  // 玩家当前那个 spawner 在遍历前只问一次：答案跟节点无关，循环里再问等于每次遍历
  //   多打最多 512 次游戏函数调用（而且还是在 Worker 线程上）。
  const unsigned long long curMineCtx = (unsigned long long)(uintptr_t)SafeFindCtx((void *)me, 0);
  int n = 0, walked = 0;
  // 不能当载体、又不是玩家自己那些节点的条数（诊断用）。两种原因：gate 关、刚判死过。
  //   两种原因里，"刚判死过"那一类上面有逐节点行（见循环里那处），
  //   "gate 关"那一类只在这儿计数，没有逐节点行。
  int skippedNotUsable = 0;
  // 逐类计数（诊断用）：链里到底有没有"非玩家的 spawner"。这几个数合起来就能回答
  //   "挑不到载体是因为链里只有玩家那几个，还是有别的、但被某条判据挡了"。
  //   同一个节点可能落进好几类（既可能是上一轮那个，又在退役环里），所以各类之和会大于 n。
  int nOwnNow = 0, nOwnPrev = 0, nRecentOwn = 0, nOwnPoint = 0, nBad = 0, nGateOff = 0;
  unsigned long long firstUsable = 0;  // 链表里第一个能用的（也是挑载体的首选）
  // 按队号各记一个候选（下标 1/2，0 不用）。同样先在局部量里攒着，遍历之后才发布，
  //   理由和 firstUsable 一样（见循环之后那段）。
  unsigned long long byTeam[CARRIER_TEAMS] = {0};
  // walkComplete：两个方向都走完才为 true。没走完时不能用"上一轮那个节点不在链表里"
  //   当换关判据，那只是没走到它，不是它没了，误判会导致一次白扫（结果一样但不该发生）。
  const bool walkComplete = WalkSpawnerList(
      ctx0,
      [&](unsigned long long c) -> bool {
        if (CtxRefIsSpawner(c)) {
          // 这里也要比 ID，地址被复用时会误判成"上一轮那个还在"，从而漏判换关。
          if (SameSpawner(c, prevCtx0, prevCtx0Id))
            prevNodeAlive = true;  // 上一轮那个还在，没换关
#ifdef _DEV
          if (changed && n < SPAWNER_DUMP_MAX) {
            // 换关那一刻把每个节点一行打全，两个模式各进一次之后直接对日志。
            LogCarrierRow(
                n, c,
                (c == ctx0)
                    ? "  (own, cannot borrow)"
                    : (SameSpawner(c, prevCtx0, prevCtx0Id)
                           ? "  (previous own, cannot borrow)"
                           : (c == prevCtx0 ? "  (same address as previous own, but a DIFFERENT"
                                              " spawner (id mismatch, pool reuse)"
                                            : "")));
          }
#endif
          // 记一个非玩家的 spawner 当注入载体。
          //
          // 排除玩家 spawner 的理由见上面 PushRecentOwnSpawner 那一段，这里只说判据本身：
          //   不能只靠 c != ctx0，玩家每次重生 ctx0 都会换（实测同一个 spawner 前后两次问出来的
          //   地址不同），于是上一个玩家 spawner 在下一轮扫描里"不再等于 ctx0"、被当成普通
          //   spawner 选中。所以这一轮的、上一轮的、以及遍历前当场问到的当前那个（curMineCtx），
          //   三类都要排除。
          // 比地址分两种，别一律换成 SameSpawner：
          //   比"当场问到的当前那个"（curMineCtx / ctx0）可以直接比地址。那个对象此刻活着，
          //     地址就是它的身份，堆没法把它复用给别人，加 ID 检查只是白读一次。
          //   比"上一轮那个"（prevCtx0）必须走 SameSpawner（地址 + ID）：它可能已经死了，
          //     堆会把同一个地址复用给一个完全合法的 spawner，只比地址就会把它挡掉。
          //     实测后果是整局没载体。
          // 另外两项的判据不在地址上：IsRecentOwnSpawner 看最近退役的玩家 spawner 那张环，
          //   IsOwnSpawnPoint 看本关里被当作过玩家生成点的那张特征表，各见自己的说明。
          // 逐条判据分开算：IsRecentOwnSpawner / IsOwnSpawnPoint 下面还要按类计数，
          //   分开算省掉重复调用，也让"被哪一条挡下的"能报出来（见循环之后那行 breakdown）。
          const bool isOwnNow = (c && c == curMineCtx);
          const bool isOwnPrev = (c == ctx0) || SameSpawner(c, prevCtx0, prevCtx0Id);
          const bool isRecentOwn = IsRecentOwnSpawner(c);
          const bool isOwnPoint = IsOwnSpawnPoint(c);
          const bool isMine = isOwnNow || isOwnPrev || isRecentOwn || isOwnPoint;
          // ctx+552 必须是 1（这道闸门的公式与那组实测数字见 ValidCarrierCore 那一段）。
          //
          //   它在链表语义下是恒真的，永远跳不掉任何节点：留着是为了当一致性哨兵。
          //   真因为这个原因被跳过（上面那个计数里能看出来），说明我们对这条链表的理解
          //   有问题，值得去查（那两个字是什么、为什么摘除就摘得到，见 ValidCarrierCore 那段）。
          const bool gateOn = SpawnerGateOn(c);
          if (isOwnNow)
            ++nOwnNow;
          if (isOwnPrev)
            ++nOwnPrev;
          if (isRecentOwn)
            ++nRecentOwn;
          if (isOwnPoint)
            ++nOwnPoint;
          // 刚四档全抛过的也要跳过（见 g_recentBad）：不跳的话这里会把同一个死载体
          //   原样填回 firstUsable/byTeam，形成"换到它、全抛、丢掉、再填回来"的死循环。
          const bool bad = IsRecentBadCarrier(c);
          if (bad)
            ++nBad;
          if (!gateOn)
            ++nGateOff;
          // 这套条件与 ValidCarrierCore 那一套是同一个口径，只有三处不同，改的时候两边一起改：
          //   1) 描述符在上面那层已经验过，所以那边那一条这里没有；
          //   2) 这里多一条"刚退役的上一个玩家 spawner"（isOwnPrev）；
          //   3) 那边的队号检查在这里不做，同队友是后面按 byTeam 分桶挑的。
          const bool usable = !isMine && !bad && gateOn;
          if (bad && changed)
            LOG_DEV("[SCAN]   [%d] ctx=%p skipped: it is in the recently-failed set", n, (void *)c);
          if (usable) {
            if (firstUsable == 0) {
              // 只记下来，不在这里写进 g_aiCtx：真正落地要等"换关判定"做完，
              //   见循环之后那段（在这里写的话，一次换关会把刚挑到的连带抹掉）。
              firstUsable = c;
            }
            // 按队号各记一个候选（见 g_carrierByTeam）。先攒在局部量里，
            //   遍历之后才发布，在遍历里直接写全局的话，同一轮的换关清理会把刚填的抹掉，
            //   那一轮结束到下次扫描之间（2 秒）同队槽是空的，刚进关就召唤会走降级分支。
            const unsigned int ct = CarrierTeamOf(c);
            if (ct >= 1 && ct <= 2 && byTeam[ct] == 0)
              byTeam[ct] = c;
          } else if (!isMine) {
            ++skippedNotUsable;
          }
          ++n;
        }
        ++walked;
        return true;  // 遍历不停在任何节点上
      },
      [&]() {
        if (changed)
          Log("[SCAN] (forward direction done, walking the other way from the head)");
      });
  // 一个候选都没有的时候，换关那几行不会打，所以这里单独保证"链里长什么样"能被看见：
  //   限流到 30 秒一条，免得每 2 秒刷一行。
  const bool noCandidate = (firstUsable == 0);
  static DWORD lastNoCandLog = 0;
  const DWORD scanNow = GetTickCount();
  if (changed || (noCandidate && (DWORD)(scanNow - lastNoCandLog) >= 30000)) {
    if (noCandidate)
      lastNoCandLog = scanNow;
    Log("[SCAN] visited %d nodes along the list from own ctx, found %d spawners"
        " (%d not usable as carrier: gate off, or recently failed;"
        " character info comes from spawn time)",
        walked, n, skippedNotUsable);
    Log("[SCAN]   breakdown: own-now=%d own-prev=%d recent-own=%d own-spawn-point=%d"
        " recently-failed=%d gate-off=%d   (one node can be in several classes;"
        " all zero means the list holds no non-player spawner)",
        nOwnNow, nOwnPrev, nRecentOwn, nOwnPoint, nBad, nGateOff);
  }

  // 链里一个能用的都没有时，再问一次游戏自己的"按队找 spawner"：
  //   SafeFindCtx(player, a2 != 0) 走的是全局哈希表（见 RVA_FIND_CTX 那段），
  //   它找到的 spawner 可能根本不在我们顺着玩家 ctx 走到的那条链上。
  //   拿到的地址照样过 ValidCarrier（问出来是玩家自己的、或不是 spawner，都会判否），
  //   所以试它没有额外风险。
  if (firstUsable == 0) {
    for (char k = 1; k <= 2 && firstUsable == 0; ++k) {
      const unsigned long long g = (unsigned long long)(uintptr_t)SafeFindCtx((void *)me, k);
      if (g == 0) {
        Log("[SCAN]   global team lookup (a2=%d) returned 0", (int)k);
        continue;
      }
      const char *whyG = nullptr;
      if (ValidCarrier(g, (unsigned long long)(uintptr_t)me, 0, &whyG)) {
        Log("[SCAN]   global team lookup (a2=%d) gave %p, usable, taking it as the carrier", (int)k,
            (void *)g);
        firstUsable = g;
      } else {
        Log("[SCAN]   global team lookup (a2=%d) gave %p, rejected: %s", (int)k, (void *)g,
            whyG ? whyG : "unknown");
      }
    }
  }

  // （挑好的载体在下面"换关判定"之后才真正落地，见那段注释。）

  // 到这儿才知道是不是真换关。判据两层：
  //
  //   第一层（强信号），玩家对象换了，直接判换关，不再看链表。
  //     重生实测玩家对象不变、只有 spawner 变，所以这一条几乎不会误报；
  //     而链表那条判据分辨不出是不是同一代对象，漏判的代价是新关里还挂着
  //     上一关的模板表（悬垂 v21）。
  //
  //   第二层（只换了 spawner）：要"上一轮那个 spawner 已经不在链表里"才算换关。
  //     重生时旧 spawner 仍挂在链表里，不动，列表和载体原样保留（这是想要的）。
  //
  // 确认换关后才做失效。
  //   1)  旧的载体 spawner 已经失效，必须清掉，不然召唤会拿悬垂指针去注入
  //   2)  资产集变了，重新枚举 Template 实例（见 ScanTemplates）
  //   3)  holder / req 的副本也必须一起清，它们是上一关对象的快照，指针早就悬垂了。
  //      不清的话换关后第一次召唤就是拿悬垂指针当底板，直接 AV。
  if (changed && !walkComplete)
    Log("[SCAN] the list walk did not finish (hit the 512-node cap), so \"the previous spawner"
        " left the list\" cannot be concluded from it; only the player-object signal counts"
        " as a level change this round");
  if (changed && (meChanged || (walkComplete && !prevNodeAlive)))
    ClearStaleAfterLevelChange(
        meChanged ? "player object changed" : "previous spawner left the list", ctx0);

  // 载体恰好是刚退役的那个玩家 spawner 时也丢掉：它虽然还挂在链表里，但已经不该用了。
  //   用 CAS 换掉，避免和游戏线程那边抢着写。
  {
    unsigned long long curCarrier = (unsigned long long)(intptr_t)AtomicReadL64(&g_aiCtx);
    if (SameSpawner(curCarrier, prevCtx0, prevCtx0Id) && DropCarrierIfCurrent(curCarrier)) {
      Log("[SCAN] carrier is the just-retired own spawner, dropping it, next pass will re-pick");
    }
  }

  // 挑好的载体到这儿才落地。
  //
  //   不在遍历里就写，是因为换关那一步会把载体清 0，而遍历在它前面：
  //   先写的话，一次换关（尤其是程序刚进来、lastMe 还是 0 的那一次，meChanged 必然为真）
  //   会把这一轮刚挑到的载体连带抹掉，于是 g_aiCtx 空着、要等下一个 2 秒周期才有人再挑，
  //   那两秒里按键全是 "FAIL no carrier ctx"。放这儿就没这个洞。
  //
  //   写的时候用 CAS（只在还是 0 时才写）：游戏线程的 AI 路径可能刚把地面真值塞进来，
  //   那是比我们猜的更好的来源，不能被覆盖掉。
  if (firstUsable && g_aiCtxSrc == 0 && !AtomicReadL64(&g_aiCtx)) {
    if (InterlockedCompareExchange64(&g_aiCtx, (long long)(intptr_t)firstUsable, 0) == 0) {
      InterlockedExchange(&g_aiCtxSrc, 1);
      Log("[SCAN] carrier set to %p (first usable in the list)", (void *)firstUsable);
    }
  }

  // 同队候选槽也在这儿发布。
  //
  //   每一轮都覆盖它，这样槽中毒之后能自己恢复：槽里的那个 spawner 是会失效的
  //   （ctx+552 变 0、被释放、甚至变成玩家自己的 spawner），而失效之后没有任何东西
  //   会来替换它。只写一次的话，一旦某个队号的槽中毒，之后每一次需要那个队的召唤
  //   都会在 ValidCarrier 那步被判否、掉进降级分支用错阵营的载体，
  //   而且这个退化会一直持续到换关。每轮重填就是让它自己恢复。
  //
  //   放在换关判定之后、和 firstUsable 同一处：遍历在换关判定之前跑，
  //   换关那一轮若在遍历里写，刚填的会被清掉（firstUsable 也是这个道理，见上面那段）。
  for (int t = 1; t <= 2; ++t)
    InterlockedExchange64(&g_carrierByTeam[t], (long long)(intptr_t)byTeam[t]);
}

// 自造，把任意角色的模板注入到一个 spawner 里，再让它生成。
//
//   原理（sub_141D6CFD0 的反编译，两条都已实测）：
//       v20 = ctx[105];                      // = *(ctx + 840)
//       if (v20) v21 = *(v20 + 48);          // 第一优先，且只读 +48 这一个字段
//       else     v21 = *(v10 + off + 6040);  // 否则退回 entity 那条
//   实测 ctx+840 在两次生成之间恒为 0（只有生成期间才被设），所以不去管"谁设置它"：
//   我们自己把 ctx+840 指向一块静态内存，在它的 +48 写上想要的 v21，
//   生成函数就只会读到我们的模板；目标 v21 由 ScanTemplates 枚举资产区得到
//   （每个 Template 一个共享实例）。
//
//   假"模板持有者"必须是真 holder 的副本，只改必要字段，这是本文件最容易踩死的一处。
//   sub_141D6CFD0 除了 +48 之外还读 holder 的：
//      +32 / +40  Modifiers（直接读）
//      +64  VisualGroupsOverride    : v34 = *(holder+64); len = *(u32*)(v34-4) & 0x7FFFFFFF
//      +72  BehaviorTreeOverride    （判空）
//      +80  BehaviorTreeParameters  : v57 = *(holder+80); len = *(u32*)(v57-4) & 0x7FFFFFFF
//      +92  （被 vt[296] 当索引查表）
//   +64 / +80 是 Frostbite 数组：长度在指针前面 4 字节。所以不要做"清零骨架 + 只填
//   +48/+88"，那两个指针为 0 会去读 0xFFFFFFFFFFFFFFFC，当场访问违例，而异常被我们自己
//   的 __try 吞掉，外表就是"按召唤键什么也没发生"。正确做法是把游戏自己的 holder
//   抓一份当底板（见 WorldInsertDetour），只改 +48 模板 / +88 血量，其余字段天然全对，
//   用完把 ctx+840 还原。
//
//   绝不要传"玩家自己的 spawner"（会让玩家销毁重生成、人直接消失），调用方负责挑一个
//      AI 的 spawner。必须 16 字节对齐：游戏按 _QWORD 读它的 +48，若将来引擎改用 SSE 读
//      （movaps 要求 16 字节对齐）就会直接崩，加个 alignas 是零成本的保险。
//
//   holder 的身份也确认了，它就是 AISpawnData 的运行时实例，ebx 24 Template 对应 +48、
//     ebx 44 OverrideMaxHealth 对应 +88、ebx 32 VisualGroups 对应 +64、
//     ebx 40 BehaviorTreeParameters 对应 +80，全部满足"运行时偏移 = ebx 偏移 × 2"。
//   大小 128 = ebx 大小 64 × 2，刚好覆盖游戏读到的所有字段（已知最大是 +92 那个 u32，
//     到 +96 结束）。刻意不多读：多读会越界到相邻资产对象上，把垃圾当字段填回游戏反而更危险。
static unsigned char g_holderCopy[128];
static volatile LONG g_holderCopyOk = 0;
// 换关代次，只由 ResetInjectState（Worker）递增。抓 holder 副本的那一侧（游戏线程的
//    WorldInsertDetour）在入口记下它、发布之前比一次：那两拨人不在同一个线程上，
//    清掉与"清完之后又被置回 1"可以交错，见抓取块那处的说明。
static volatile LONG g_injectGen = 0;
// 底板来源值，0=没有  1=别的 ctx 的  2=g_aiCtx（我们注入用的那个载体）自己的。
// 规则同 g_aiCtxSrc，2 永远赢，1 只填在 0 上。
static volatile LONG g_holderSrc = 0;
// 这份副本由 WorldInsertDetour（= sub_141D6CFD0 内部）在游戏线程刷新，
//    而 SummonWithTemplate 也在游戏线程（由 DISPATCH detour 触发），这两个使用者之间严格串行；
//    但把它清空的那一次（ResetInjectState）跑在 Worker 上，所以发布要带上 g_injectGen 的比对。
//    seqlock 仍保留作保险，写的前后各 ++，奇数 = 正在写。
//    （不用 CRITICAL_SECTION 是因为它需要初始化时机保证，而 Interlocked 什么都不需要。）
static volatile LONG g_holderSeq = 0;
alignas(16) static unsigned char g_fakeHolder[0x80];  // 临时"模板持有者"（= 真 holder 的副本）
// 1 = 我们正把 g_fakeHolder 装在某个 ctx 上（注入窗口内）。
//   只给世界钩子做判据用，那个钩子看到 ctx+840 == g_fakeHolder 时，要分清
//   是"我们自己的这次生成"（正常，就在窗口里）还是"游戏自己的生成撞进窗口了"
//   （异常，会产出名字和身体对不上的混搭单位）。
static volatile LONG g_injecting = 0;
// 1 = 我们正同步跑在 g_spawn_orig 那一次调用里面。
//
//   注入窗口里世界钩子会因为两种原因响：a) 我们自己那一次生成
//   （正常，就在这个标志里），b) 别人在同一个窗口里生成（异常，它会读到我们的假 holder，
//   产出"名字取自我们的模板、身体和部件取自它自己的 spawner"的混搭单位）。
//   我们的调用是同步的（世界钩子就在 sub_141D6CFD0 里），所以窗口内不带这个标志的
//   世界调用就是别人。
//
// 判据用 g_inOurCall 这个标志：梯子本来就可能调 2 到 4 次生成函数
//   （第 0 档被闸门拦下返回 0 是预期路径，ATT_RESET_ROT 就是为它准备的），
//   拿"窗口里响了几次"当分母会把正常的退路误报成"别人撞进来了"。
static volatile LONG g_inOurCall = 0;
// 两者必须一样大，一个抓、一个用，两次 memcpy 都按各自 sizeof 走。
static_assert(sizeof(g_holderCopy) == sizeof(g_fakeHolder),
              "holder copy and fake holder must be the same size");

// 写游戏内存时临时改页保护、写完还原。
//
// 走这一层是因为 ctx 本身是可写的普通堆对象，不是只读页：
//    工厂 sub_141D52090 用 sub_1404151D0(984LL, …) 分配，而且游戏自己就一直往 ctx 里写：
//    ++*(u32*)(ctx+244)、往 ctx+400 的数组 push_back、ctx+556 置位……
//    所以这里只是保险，多两次 VirtualProtect，零成本，万一真遇上只读页也不至于当场崩。
//    （"写 ctx 抛异常"不代表 ctx 在只读页上：更可能是手里那个 ctx 本身就是坏指针。）
static bool ForceWriteRaw(void *p, const void *src, size_t n) {
  DWORD old = 0;
  if (!VirtualProtect(p, n, PAGE_READWRITE, &old))
    return false;
  memcpy(p, src, n);
  DWORD tmp = 0;
  // 还原失败要报出来，那意味着这一页被我们留成可写了，而调用方以为已经还原。
  //   不崩，但会静默地削弱保护，事后一点痕迹都没有。
  if (!VirtualProtect(p, n, old, &tmp))
    Log("[MEM] !! failed to restore page protection (addr=%p len=%llu err=%lu)", p,
        (unsigned long long)n, GetLastError());
  return true;
}
static bool ForceWrite32(void *p, unsigned int v) {
  return ForceWriteRaw(p, &v, 4);
}
static bool ForceWrite64(void *p, unsigned long long v) {
  return ForceWriteRaw(p, &v, 8);
}

// 不走 sub_141D6D9B0 的理由（反编译 + 实测）。
//
//   它的语义是"让 ctx 给这个 player 重新出生"。反编译（0x141D6D9B0）：
//       if (!(ctx+552 && ctx+576 == player+88)) return 0;      // 队伍判定
//       v14 = sub_141D03AF0(player + dword_143FBCDB0);          // 拿玩家实体
//       (**(v14 + 272 + 32))(v14 + 272)                         // 一个谓词
//       (*(void(**)(v14, 1))(*(u64*)v14 + 840))(v14, 1);        // 玩家实体的虚表字节偏移
//       840(= 第 105 槽)
//       ...
//       v19 = (*(void(**)(ctx, v27))(*(u64*)ctx + 232))(ctx, v27);   // ctx->vt[232] = 生成
//       return v19 != 0;                                        // 游戏自己用"返回非 0"当成功判据
//
//   记法陷阱，上面那个 840 是虚表里的字节偏移（840/8 = 第 105 槽），
//      和 ctx + 840（= AISpawnData 指针那个字段）毫无关系，只是数字巧合，别当成一处。
//
//   代价是借任何"曾经是玩家出生点"的 ctx，旧实体被销毁、新实体没生出来，
//   玩家从世界里消失、无法操作。而且玩家每次重生都会新建一个出生点 ctx、
//   旧的留在链表里，光排除"当前那个"排不干净。
//
// 正确做法是跳过 sub_141D6D9B0，直接调 ctx->vt[232] = sub_141D6CFD0，
//      req 用抓来的 AI 路径副本（清一色全零是不行的，原因见 SpawnDetour 的字段表）。
//      这样完全不碰玩家实体。
//
//   另外确证了那张字段表，v27 是这里的 _BYTE v27[64] 栈缓冲，紧跟其后的
//      v28/v29/v30/v31/v32 就是 req+64/+72/+80/+88/+96，而代码里是
//      v28 = player; v29 = 0; v30 = 0; v31 = -1; v32 = a3
//      （特别是 req+64 = player、req+92 = -1）。
//
// 把游戏自己构造过的 req 复制一份留着用。
//
//   比全零稳妥，因为 req 是生成请求（sub_140F4EF30 会读它的一些字段），
//   我们并不知道哪些字段是必需的。而 SPAWN detour 每次都能看到游戏传进来的真实 req，
//   复制一份它的内容，就等于拿游戏自己的模板来用。
//
//   全零不能替代，req+92 必须 == -1，ctx+244 才会在成功后递增（= 信号2) ）；
//      全零时 req+92 == 0，ctx+244 不动，最直接那条信号就没了（见 SpawnDetour 的字段表）。
//      信号2) 的另一半是生成函数的返回值非 0（那也是游戏自己的判据，更权威），
//        所以全零并非"完全无法验证"；但 req+72 == 0 会让闸门 (A)/(B) 生效、成功率低得多，
//        照抄 AI 那份是唯一妥当的做法。
static unsigned char g_reqCopy[256];
static volatile LONG g_reqCopyOk = 0;
// 注入时用的两段私有缓冲（放文件作用域，不放函数里当 static 局部，更清楚，
// 也免得依赖编译器对函数内静态对象的初始化保护）：
//    g_reqBase = 从 g_reqCopy 快照一次，此后绝不再碰 g_reqCopy
//    g_reqUse  = 每一轮从 g_reqBase 重来，再按本轮策略改字段
// 要两段是因为 g_reqCopy 会被 SPAWN detour 在游戏线程随时覆盖，而我们的注入
// 也调同一个函数，若直接拿 g_reqCopy 本体改字段，就可能边被游戏重写边被我们改。
static unsigned char g_reqBase[256];
static unsigned char g_reqUse[256];
// holder 的私有快照（形状同 g_reqBase）：seqlock 校验后拷一份，梯子每轮从它重来。
static unsigned char g_holderBase[128];
static_assert(sizeof(g_holderBase) == sizeof(g_holderCopy),
              "holder snapshot and copy must be the same size");

// "尝试梯子"，问的是哪一种 req/ctx 组合真能生成。
//
//   需要梯子是因为 sub_140F4EF30 里有两道会直接 return 0 的闸门，而它们取决于 req/ctx
//   的现场状态，不是"照抄一份 req 就必然通过"：
//     (A) if (!ctx+552 && !req+72) return 0
//     (B) if (!req+72) { … || (req+92 == -1 && !sub_140F475F0(ctx, w60))，return 0 }
//         sub_140F475F0 的核心是 *(u32*)(ctx+244) < 上限 || 上限 < 0
//         注意 (B) 整段在 req+72 != 0 时被整个跳过（代码是 if (v4) goto LABEL_25）
//
//   闸门不是"召唤按了没反应"的常见原因：日志里 WORLD（= sub_140F4EF30）经常被调到，
//      说明 (A)(B) 都放行了。真正卡住的那种，是 holder 用了清零骨架
//      （见 SummonWithTemplate 那段）。
// 所以这一梯子的定位是保底手段，正常情况下照抄就该成功；
//        只有将来真的撞上闸门时才轮到后两档。
enum {
  // 第 0 档 = "在玩家位置生成"。做成第 0 档是因为它就是我们要的效果，
  //   而且它天然也用"自带变换"这条路（同时绕开挑生成点）。
  //   做法是把玩家的世界变换（64 字节）整块抄进 req+0..63，再把 req+97 置 1。
  //   取 +97 不取 +72（实测如此）。
  //     +72 非 0，同样"直接用自带变换"，但有副作用，会把 (player,+88) 插进
  //                ctrl+304 的有序表并触发事件；
  //     +97 非 0，同一个 LABEL_25（反编译：if (*(_BYTE*)a2+97) goto LABEL_25），
  //                无副作用，首选。
  ATT_AT_PLAYER = 0,
  ATT_FAITHFUL = 1,   // 忠实照抄游戏 AI 路径的 req（位置由游戏自己挑）
  ATT_RESET_ROT = 2,  // ctx+244 = 0（重置生成预算，让闸门 (B) 放行）
  // 不要加"req+72 置非 0"这种档：
  //   req+72 非 0 确实会让 (A)(B) 两道闸门整段跳过，但那样会走进 sub_140F4EF30 里
  //   "往 ctx+264 那棵有序表插入"的分支，而停用状态的 ctx 没有那棵表
  //   实测 8 次调用 8 次抛异常，全崩在 code=0xC0000027 RVA=0x1088E75 访问地址=0x1
  //      （sub_141088E60 里 *(a1+280) 存的是 1 而不是合法指针，判空没拦住）。
  //   而 ctx+552 == 0 的载体本来就该丢掉重挑（见 ValidCarrier），不该硬闯。
  // 清 req+80 的原因（梯子每一档都清，见循环里那段）。sub_140F4EF30
  //   把实体放进世界时调 ctx->vt[280]
  //   （= sub_141D62790），而它开头会遍历一条链、逐级取每个元素的首 8 字节
  //   当虚表来调用。
  //       rbx = a7;  do { call *(*(rbx))(rbx); rbx = *(rbx+8); } while (rbx);
  //   这条链是 sub_140F4EF30 自己搭的：
  //       v38[0] = &off_1434C1500;
  //       v38[1] = *((_QWORD *)req + 10);        // 就是 req+80
  //       vt[288] 里再把它接成 v48，v38，req+80
  //   req+80 就是这条链的最后一环。它只要是个非 0 垃圾指针，就会去解引用
  //      那个对象、拿它的首 8 字节当虚表，崩（实测访问地址 0）。
  //   字段表里记过"req+80 在某个分支是 &v53"，也就是它有时是个指向调用者栈帧的指针，
  //   而我们的 req 是从调用方 64 字节栈缓冲刻意越读 256 字节抄来的，那个栈帧早没了。
  //   我们自己 BuildReq 造的 req 里 +80 恒为 0（那一环自然终止），所以只有照抄来的
  //   那份会出问题，而且只在走那个分支的模式里（实测战斗箱子；日志里 req+80
  //   非 0 那几次全是前 3 档抛异常）。
  // 纯诊断档，holder 保持"照抄"，模板一个字段都不动（req 的 +64/+80 例外，见循环里）。
  //   只有在退路分支（底板取自载体 holder 的副本）时，才会生成这个 spawner
  //      平时造的角色。正常分支的底板取自目标模板自己的 AISpawnData，
  //      所以那一档照样会造出被注入的目标角色。
  //   副作用是真的会多刷一个敌兵出来（Giddy Park 本来就一团乱，OPS 会多一个敌人）。
  ATT_ASIS = 3,
  ATT_COUNT = 4,
};
// 两条规矩，改这个 enum 或梯子的时候别破：
//   1) 第 0 档（ATT_AT_PLAYER）既是设计上正确的那一档、也是每次最先试的那一档，
//      见 SummonWithTemplate 里 start 那处注释。
//   2) 梯子开头要把"从哪档开始"打进日志。否则会出现"最想要的那档从头到尾没被执行、
//      日志上却看不出异常"这种极难查的情况。

// 在 __except 里拿到异常细节，这是唯一能问出"到底哪条指令、访问了什么地址"的办法。
//
//   必须这么写是因为 GetExceptionInformation() 只在 __except 的 filter 表达式里有效，
//   handler 体内拿不到。所以用一个 filter 函数把它存进全局，handler 再读。
//   打出来的 RVA 直接拿去 IDA 就能看到出错的那条指令，不用再猜。
static EXCEPTION_POINTERS *g_exc = nullptr;
static LONG ExcFilter(EXCEPTION_POINTERS *p) {
  g_exc = p;
  return EXCEPTION_EXECUTE_HANDLER;
}

// AISpawnData 的运行时类型描述符，见前面的前向声明。
//
// 两个来源：写死的常量，以及抓到的 holder 的 +8（holder 本身就是 AISpawnData 实例，
//   它的 +8 天然就是这个描述符）。
//   两者不一致时留写死的那个（下面把它清成 0，日志写着 keeping the hardcoded one）：
//   实测值没过验证，拿它去扫会把整趟扫描指向错误的类型。实测值在这里只当校验用。
// 写死的那个是 Ops 能工作的关键：Ops 里没有任何 AI 生成事件，holder 永远抓不到，
//   只剩它可用。
static unsigned long long GetAdDescriptor() {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  unsigned long long hard =
      base ? (unsigned long long)(uintptr_t)(base + RVA_TYPEDESC_AISPAWNDATA) : 0;
  unsigned long long d = 0;
  if (g_holderCopyOk)
    Rd64((unsigned long long)(uintptr_t)(g_holderCopy + 8), &d);
  if (d && hard && d != hard) {
    static volatile LONG warned = 0;
    if (!InterlockedExchange(&warned, 1))
      Log("[TPL] AISpawnData descriptor mismatch: measured holder+8=%p, hardcoded const=%p; "
          "keeping the hardcoded one (an unverified value would steer the whole scan at the "
          "wrong type)",
          (void *)d, (void *)hard);
    d = 0;
  }
  return d ? d : hard;
}

// 自己按字段表构造一份 req，Ops 等"没有 AI 生成事件"的关卡靠它。
//
//   Ops 的敌人刷怪不走 sub_141DCBD10 到 ctx->vt[232] 那条路，于是 SPAWN detour
//   从没见过 AI 路径的调用，g_reqCopy 永远是空的，必须能自己造一份。
//   而 req 的字段表已经靠反编译 + 实测完全确定（见 SpawnDetour 的说明）：
//     +0..63  游戏自己也不写（是调用者的栈残留），我们填 0
//     +64 = 0     （owner；AI 路径实测就是 0）
//     +72 = 0
//     +80 = 0
//     +88 = -1（dword）
//     +92 = -1（dword）它 == -1 才会 ++*(u32*)(ctx+244)，我们的成功信号2)
//     +96 = 1（我们填 u16，和 sub_141DCBD10 里的 v63 = 1 一致）
static unsigned char g_reqBuilt[256];
static void BuildReq() {
  memset(g_reqBuilt, 0, sizeof(g_reqBuilt));
  *(unsigned int *)(g_reqBuilt + 88) = 0xFFFFFFFFu;
  *(unsigned int *)(g_reqBuilt + 92) = 0xFFFFFFFFu;
  *(unsigned short *)(g_reqBuilt + 96) = 1;
}

// 难度探针（ProbeDifficultyTier）的配额：每关只跑前几次召唤。整块只在开发构建里编。
//
//   跑它是为了进关时看到池子的构成和每个槽的 +128（bit1 = 持久 AI）。
//   一次要打十来行 [DIFF]，每次召唤都跑只是把日志刷长，
//   所以只留前几次；换关时由 ResetInjectState 清零，每进一关都有新的几次。
#ifdef _DEV
constexpr int DIFF_PROBE_PER_LEVEL = 2;
static volatile LONG g_diffProbeN = 0;
#endif

// 把 holder 副本连同来源标记一起作废，拆开就出事，所以只留这一个入口。
//   异常路径必须清：缓冲区是写了一半的，而 g_holderCopyOk 还留着上一次成功时的 1，
//   读方会把半份 holder 当底板拿去注入（它的 +64/+80 是 Frostbite 数组，按 *(u32*)(ptr-4)
//   读长度，撕裂的指针正好给出那个访问违例）。
//   g_holderSrc 必须跟着清：抓取闸门是 g_holderSrc == 0 || (src == 2 && ...)，
//   只清 Ok 不清 Src 会留下一对不一致的值（Src 停在 1、Ok 是 0），
//   于是后面所有 src=1 的抓取都被闸门挡掉，底板只能等来一次 src=2 才能恢复。
static void ClearHolderCopy() {
  InterlockedExchange(&g_holderCopyOk, 0);
  InterlockedExchange(&g_holderSrc, 0);
}

// 换关时清掉 holder、req、AISpawnData 表和模板表等上一关快照（见前面的前向声明）。
//   它们指向上一关的对象，已悬垂。
static void ResetInjectState() {
  // 先在代次上加一：下面要清的几份副本指的都是上一关的对象，
  //   而游戏线程可能正抓在半途，清完它又会把自己那份无条件置回 1。
  InterlockedIncrement(&g_injectGen);
  ClearHolderCopy();
  // 递增而不是清零，这个计数器是"写方正在改"的 seqlock（奇数=写了一半），
  //   清成 0 就把它的奇偶语义破坏了。加 2 而不是加 1：换关时没有写者，它是偶数，
  //   +2 仍然是偶数、单调性也保住了。真读到奇数只说明有别的写者卡在半途，
  //   那不是这里能解决的，但至少不会把它抹成 0。
  InterlockedAdd(&g_holderSeq, 2);
  InterlockedExchange(&g_reqCopyOk, 0);
  // AISpawnData 表也必须清，它存的是上一关资产实例的地址。
  //    （留着不清，下一次按召唤（F5 / F11）会拿悬垂的 AISpawnData 当底板。）
  InterlockedExchange(&g_adN, 0);
  // 模板表也一样，它存的也是上一关资产实例的地址，同样悬垂。
  //   清掉之后 BuildSummonLists 的"必须有自己 AD"那道过滤会让列表变空（g_adN=0），
  //   所以不会有人拿着上一关的 v21 去注入。
  InterlockedExchange(&g_tplN, 0);
  // buff 表同样存的是上一关资产实例的地址。它只有 11 条，清空的代价是换关后
  //   这一瞬的召唤少挂几个 buff，比把悬垂指针交给引擎划算得多。
  InterlockedExchange(&g_buffN, 0);
  // "补扫过没有"的闸门也一起清，换关后允许再补扫一次。
  InterlockedExchange(&g_adScanTried, 0);
  // 排队待执行的那一次召唤也必须丢。
  //   它是按键那一刻解析出来的 v21，属于上一关。召唤块可能因为加载/换关
  //   迟迟没被调到，等它终于跑起来时载体会被重扫换成一个新对象（于是校验通过），
  //   而 v21 还是旧关的，那就是一个悬垂指针直接进引擎。
  // 邮箱里要是还挂着一发没被取走的请求，那就是"刚换关按下第一发"那种情形：
  //   请求必须作废（旧 v21 是悬垂指针），但不能让它无声消失，交给重试去补。
  //   先清唤醒标志再清载荷：消费方看到标志为 0 就整段跳过，不会拿到半份载荷。
  // 序列号在清载荷之前抄下来：这一发的失败属于它按下时那一次按键。
  const bool dropped = InterlockedExchange(&g_pendingSummon, 0) != 0;
  const LONG droppedSeq = dropped ? (LONG)AtomicReadL(&g_queuedSeq) : 0;
  InterlockedExchange64(&g_queuedV21, 0);
  if (dropped)
    SummonAttemptFailed(droppedSeq, "FAIL request dropped at a level change");
  // 难度探针的配额也一起重置，新的一关允许再打几次（见 g_diffProbeN 的说明）。
#ifdef _DEV
  InterlockedExchange(&g_diffProbeN, 0);
#endif
}

// 玩家的世界变换（64 字节），实测确认过的路径，见 GetPlayerWorldXform 的注释。
//
//   原语是 sub_141090120(h, out) = sub_1410878E0(*(h + 24), out)，位置出在 out+48。
// h 的 +24 必须指向一个场景节点。
//   确定值 +40 是从反编译里读出来的（见下）：h 是哪个对象的哪个成员随类而异
//   （IDA 里见到的偏移有 0 / 32 / 40 / 48），猜偏移就是盲扫。
//   调用套了 SEH，传错对象时 sub_1410878E0 解野指针会当场崩。
//   全部候选都失败时返回 false，上层就不写 req+0..63，退回游戏自己挑生成点
//     （宁可位置不对，也不要把 bot 扔到 NaN 坐标上）。
typedef void *(__fastcall *GetWorldXformFn)(void *node, void *out64);

// 基向量校验，判断"这 64 字节到底是不是一个真正的世界变换"。
//   依据（IDA 实锤，三处互相印证）：LinearTransform = 4×16 字节，
//     前 3 行是基向量，第 4 行是平移。
//     - sub_140EA1DD0：v8[3] 当位置用、*v8/v8[1]/v8[2] 当基用
//     - sub_1410878E0：先把单位阵写进 out，再沿父链逐级右乘
//     - 位置恒在 out+48 = 第 4 行的前 3 个 float
// 真正的世界变换里 3 个基向量两两正交且长度彼此相等。
//   非加不可，光看"位置像不像个合理坐标"挡不住假阳性，
//     任何一段凑巧可读的内存都可能给出量级合理的数（出生点 (-76.6,129.3,-308.4)
//     量级才 514，这个区间太宽了）。而"三个基向量正交 + 等长"是刚体变换的结构性质，
//     垃圾数据几乎不可能同时满足。这条判据与坐标系无关，也不需要知道场景在哪。
//   长度不卡死 = 1，因为 Frostbite 的 LinearTransform 允许带一个统一的单位换算缩放
//     （cm 对应 m 之类）。所以判"三条彼此相等"，比判"都等于 1"更稳、又不丢判别力。
static bool BasisLooksValid(const unsigned char *xf, float *lenOut = nullptr) {
  const float *m = (const float *)xf;
  float len2[3];
  for (int i = 0; i < 3; ++i) {
    const float a = m[i * 4 + 0], b = m[i * 4 + 1], c = m[i * 4 + 2];
    len2[i] = a * a + b * b + c * c;
    // 用取反写法 !(x > k)：NaN 比较恒为 false，取反后才拦得住
    if (!(len2[i] > 1e-6f) || !(len2[i] < 1e12f))
      return false;
  }
  // 三条长度必须彼此相等（误差 2%）
  if (len2[1] < len2[0] * 0.98f || len2[1] > len2[0] * 1.02f)
    return false;
  if (len2[2] < len2[0] * 0.98f || len2[2] > len2[0] * 1.02f)
    return false;
  // 两两正交：点积 / (|a||b|) 的平方要小于 0.01，也就是夹角余弦小于 0.1
  //   （同样用取反写法拦 NaN）
  const float d01 = m[0] * m[4] + m[1] * m[5] + m[2] * m[6];
  const float d02 = m[0] * m[8] + m[1] * m[9] + m[2] * m[10];
  const float d12 = m[4] * m[8] + m[5] * m[9] + m[6] * m[10];
  const float s01 = len2[0] * len2[1], s02 = len2[0] * len2[2], s12 = len2[1] * len2[2];
  if (!(d01 * d01 < 0.01f * s01) || !(d02 * d02 < 0.01f * s02) || !(d12 * d12 < 0.01f * s12))
    return false;
  if (lenOut)
    *lenOut = len2[0];
  return true;
}

// 读一个"持有者"的世界变换。底层原语与 +24 那个前提见上面那一段。
//   玩家的 h = *(ent+40)（见 GetPlayerWorldXform 的注释）。
static bool ReadWorldXform(unsigned long long node, unsigned char out64[64], const char *name,
                           int idx) {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base || node == 0)
    return false;
  GetWorldXformFn fn = (GetWorldXformFn)(base + RVA_GET_WORLD_XFORM);
  bool called = false;
  __try {
    memset(out64, 0, 64);
    fn((void *)node, out64);
    called = true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    called = false;
  }
  if (!called) {
    Log("[POS]   #%d %s=%p, call raised (this object is not a node)", idx, name, (void *)node);
    return false;
  }
  // 先验"这 64 字节是不是真变换"，再看坐标像不像话，顺序很重要（理由见 BasisLooksValid）
  float basis = 0.0f;
  if (!BasisLooksValid(out64, &basis)) {
    Log("[POS]   #%d %s=%p, basis not orthogonal/equal-length, not a real world transform "
        "(false positive)",
        idx, name, (void *)node);
    return false;
  }
  float x = *(float *)(out64 + 48), y = *(float *)(out64 + 52), z = *(float *)(out64 + 56);
  unsigned int marker = *(unsigned int *)(out64 + 60);
  bool finite = (x == x) && (y == y) && (z == z) && (x < 1e38f) && (x > -1e38f) && (y < 1e38f) &&
                (y > -1e38f) && (z < 1e38f) && (z > -1e38f);
  float mag = (x < 0 ? -x : x) + (y < 0 ? -y : y) + (z < 0 ? -z : z);
  if (!finite || mag < 1e-4f || mag > 1e5f) {
    Log("[POS]   #%d %s=%p, implausible (%.1f, %.1f, %.1f) marker=%08X", idx, name, (void *)node, x,
        y, z, marker);
    return false;
  }
  Log("[POS]   #%d %s=%p, world pos (%.1f, %.1f, %.1f)  marker=%08X  basis len^2=%.3f", idx, name,
      (void *)node, x, y, z, marker, basis);
  return true;
}

// 场景节点的布局（要"把 obj 当节点直接读"时才用得上）：节点局部变换在 +32、
//   世界标记在 +92、父链在 +96，位置在 node+80。
// ReadWorldXform 走的不是这条路：sub_141090120(h,out) 内部就是
//   sub_1410878E0(*(h+24), out)，即"先取 h 的持有者、再把它当节点读"，
//   两者是同一个函数的两种入口，不是两种对象。

// 解玩家世界变换。成功返回 true 并填 out64；whichOut 收下用的是哪个候选的描述。
static bool GetPlayerWorldXform(unsigned long long player, unsigned char out64[64],
                                const char **whichOut) {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base || player == 0)
    return false;
  unsigned int off = 0;
  ReadFieldOffset(&off);
  // 玩家实体用 sub_141D03AF0(player + off)，sub_141D6D9B0 里就是这么拿的
  unsigned long long ent = 0;
  if (off) {
    typedef unsigned long long(__fastcall * GetEntFn)(unsigned long long);
    GetEntFn ge = (GetEntFn)(base + RVA_PLAYER_ENTITY);
    __try {
      ent = ge(player + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ent = 0;
    }
  }

  // 实测通过。
  //   玩家的实时世界坐标 = sub_141090120(*(ent + 40), out64)，位置在 out64+48。
  //
  //   实测持有者指针会随玩家走动改变，说明它是实时跟着玩家走的，不是出生时那份快照。
  //
  //   +40 有出处，不是猜的。sub_140F4D2C0(a1=ctx, a2=msg) 里写着
  //     sub_141090120(*(_QWORD*)(a1 + 40), v43)，而它的 a1 就是 spawner ctx 类
  //     （读 a1+552 闸门 / a1+400,408 生成物 vector）；sub_140F49D00 也拿 *(a1+40)。
  // +40 是这类对象的"持有者"成员（它的 +24 指向场景节点）。
  //     但它在玩家实体 ent 上，不在 player 上，ent = sub_141D03AF0(player + off)。
  //
  //   持有者在 ent 上：ent+40 是确认值（所以每次召唤都现读一遍）。
  //      player 与 ent 是两个对象，player+40 那里实测是内联字符串
  //      （0100365F7265776F = "ower_6\0\x01"），含义完全不同。
  //   偏移 K 随类而异（IDA 里见过 0/32/40/48），所以只能用这个确认值。
  unsigned long long holder = 0;
  if (ent == 0 || !Rd64(ent + 40, &holder) || holder == 0) {
    Log("[POS] ent+40 unreadable (ent=%p), falling back to a game-picked spawn point", (void *)ent);
    return false;
  }
  if (!ReadWorldXform(holder, out64, "*(ent+40)", 0)) {
    Log("[POS] *(ent+40)=%p yields no world transform, falling back to a game-picked point",
        (void *)holder);
    return false;
  }
  if (whichOut)
    *whichOut = "*(ent+40) player entity holder";
  return true;
}

// 在 AISpawnData 表里找 v21 对应的那一份。
//   同一模板会匹配到多份（各份 +92 可能不同），所以这里优先取 +92 与模板阵营一致的那一份，
//   没有一致的才退回第一份，
//   并在日志里说明"身体可能是错的"（见下面那段）。
//
// 数据链是 AISpawnData = Gameplay/Teams/<队伍>/<角色>_<档>_SpawnData，
//   它的 Template / Blueprint / Team 是同一份资产里配好的，所以正常情况下不会有歧义。
// 同一个模板会匹配到多份 AISpawnData（实测一份 Acorn 匹配到 7 份）。
//   AISpawnData 是按 spawner 实例化的：一关里同时活着好几个用同一模板的 spawner，
//   就有好几份记录。它们不是同一份数据的副本，字段可以不一样。
//
// 不能挑错，sub_141D5E0F0（= ctx 的 vt[296]，sub_140F4EF30 用它决定造什么）里
//   身体走的是 sub_141E4E7D0(holder+92) 查全局注册表，名字和外观走的是 holder+48 的模板，
//   两条腿分开。取错一份就是"名字是模板的、身体是别人的"。
//
// 挑法是看模板路径里写了自己是哪一方（Template_Plant_* / Template_Zombie_*），
//   而记录里的 +92 就是阵营号（1=僵尸 2=植物，HUD 分列用的也是它），两边对上才算自洽。
//   路径里没写阵营的（Gnome/Snowman 那批）判不出来，退回第一份。
//
// 每一份都要过一道自洽校验（那条规矩、以及表的发布为什么会造出半新条目，
//   写在 AdSelfOk 的定义处）。
//   扫描时 tpl 本来就是从 obj+48 读出来的（见 ScanTemplates），所以正常情况下这是恒等式。
//   过不了的会返回另一个模板的 AISpawnData，+56 Blueprint 是别人的，召出来外观和 AI
//   都是别人的（"拉丝/片状物"那个症状）；"这份 AD 已经悬垂"也走同一支（Rd64 失败），同样跳过。
//   （定点校验就够：它的代价是每次查表多几次 Rd64。）
static unsigned long long FindOwnAd(unsigned long long v21, int wantTeam) {
  LONG n = AtomicReadL(&g_adN);
  unsigned long long first = 0, match = 0;
  int dup = 0, torn = 0;
  for (LONG i = 0; i < n; ++i) {
    if (g_adTab[i].tpl != v21)
      continue;
    // ad 为 0 要单独判，不能让 Rd64 去读地址 48：那样会被 SEH 接住、并计进上面那个
    //   torn 计数，日志会把诊断指向"发布竞态/悬垂"，而实际只是表里有个空条目。
    if (!g_adTab[i].ad)
      continue;
    if (!AdSelfOk(g_adTab[i].ad, v21)) {
      ++torn;  // 半新条目，或这份 AD 已经悬垂，跳过，继续找
      continue;
    }
    ++dup;
    if (first == 0)
      first = g_adTab[i].ad;
    unsigned long long bp = 0;
    unsigned int team = 0;
    Rd64(g_adTab[i].ad + 56, &bp);
    Rd32(g_adTab[i].ad + 92, &team);
    // 多份的时候把每份的判据都打出来：出外观问题时这几行就是唯一的现场
    //   （三份以内才打，避免同一模板被反复召唤时刷屏）。
    if (dup <= 3)
      Log("[TPL]   candidate #%d @%p  +48=%p  +56(Blueprint)=%p  +92(team)=%u", dup,
          (void *)g_adTab[i].ad, (void *)v21, (void *)bp, team);
    // 与 AdHpOf 用同一条口径：+92 只有最低字节是队号，比之前先截成 8 位。
    //   两处必须要么都截、要么都不截，不然同一个值在两处会得出不同的队。
    if (!match && wantTeam > 0 && (unsigned char)team == wantTeam)
      match = g_adTab[i].ad;
  }
  if (torn)
    Log("[TPL] %d AISpawnData entries matching %p are inconsistent (publish race / dangling), "
        "skipped",
        torn, (void *)v21);
  if (match && match != first)
    Log("[TPL] first entry's +92 does not match template team(%d), using a later candidate @%p"
        " (matched %d)",
        wantTeam, (void *)match, dup);
  else if (!match && wantTeam > 0 && dup > 0)
    Log("[TPL] no candidate's +92 matches template team(%d) among %d, using the first, body may "
        "be wrong",
        wantTeam, dup);
  return match ? match : first;
}

// 回收线 = 分配器的真实上限减去这个余量。
//
//   上限是 sub_140F56050 自己判的（反编译），不是我们定的。容器2（bot 池）用
//   *(u32*)(mgr+1768)，容器1（我们的单位走这条）用 *(u32*)(mgr+16) 减掉同一个数。
//   到线它 return 0，sub_141D62790 就按 "if (!a2) return (vt+264)(ctx, entity, 0, 0)"
//   收场，而那个调用 a3 是 0，实体不是从世界摘掉、是挂回 ctx+472..488 的待生成向量，
//   表现是"闪一下就没"。
//   余量要同时留够游戏自己那一头。它的单位进的是同一个容器，而我们只在按下召唤
//   那一刻看一次线，两次之间它加进来多少不由我们管（BOSS 的召唤技能会一次加一批）。
constexpr int SLOT_CAP_MARGIN = 10;

// 读 Frostbite 的 {beg,end} 指针对（向量头那两个字段，相邻 8 字节）。两个都读到才算成功。
//   哪儿空、哪儿反了（end 与 beg 的大小关系）、算不算错，全部由调用方自己判，别搬进来：
//   七处调用点的口径本来就不一样，而且各有各的失败语义（返回 false / 返回 -1 / 记 unknown）。
static bool ReadVecSpan(unsigned long long obj, unsigned int off, unsigned long long *begOut,
                        unsigned long long *endOut) {
  *begOut = 0;
  *endOut = 0;
  return Rd64(obj + off, begOut) && Rd64(obj + off + 8, endOut);
}

// 读玩家槽位池，上限 = total - botCap，已用 = 玩家向量里的元素个数。
//   全是纯读，读不到返回 false（调用方按"不知道"处理，不要按 0 处理）。
static bool ReadSlotPool(unsigned int *capOut, unsigned int *usedOut) {
  if (capOut)
    *capOut = 0;
  if (usedOut)
    *usedOut = 0;
  unsigned long long mgr = GetManager();
  if (mgr == 0)
    return false;
  unsigned int total = 0, botCap = 0;
  if (!Rd32(mgr + MANAGER_SLOTS_OFF, &total) || !Rd32(mgr + MANAGER_BOTCAP_OFF, &botCap))
    return false;
  unsigned long long beg = 0, end = 0;
  if (!ReadVecSpan(mgr, 112, &beg, &end) || end < beg)
    return false;
  const unsigned int cap = (total > botCap) ? (total - botCap) : 0;
  if (capOut)
    *capOut = cap;
  if (usedOut)
    *usedOut = (unsigned int)((end - beg) >> 3);
  return true;
}

// 找出这个实体属于哪个 spawner：遍历 spawner 链表，逐个看它的实体数组（ctx+400..408）。
//
//   必须找它，销毁一个单位要走 ctx 的虚方法 vt[264] = sub_141D6AFE0(ctx, pawn, 1, 0)，
//   而它内部会在 ctx 的实体数组里反查那个实体、把它从数组摘掉（sub_140F4DB30）。
//   传错 ctx 不会写坏内存（子函数在数组里找不到就直接返回），但实体就摘不掉，
//   那个 pawn 会留在世界里变成"不动的单位"。所以按链表找一遍（走 WalkSpawnerList）。
//
//   anyCtx 只要是这条链表上的任意一个节点就行（我们用它当前正借的载体）。
//   找不到返回 0，调用方按"不回收"处理。
static unsigned long long FindOwningCtx(unsigned long long anyCtx, unsigned long long pawn) {
  if (anyCtx == 0 || pawn == 0)
    return 0;
  // 遍历用和 ScanPool 同一份走法（节点在 ctx+144，先正走再反走）。
  unsigned long long found = 0;
  WalkSpawnerList(
      anyCtx,
      [&](unsigned long long c) -> bool {
        unsigned long long beg = 0, end = 0;
        if (HeapPtr(c) && ReadVecSpan(c, 400, &beg, &end) && HeapPtr(beg) && end > beg) {
          const unsigned long long n = (end - beg) >> 3;
          if (n <= 4096) {  // 上限只是防御坏数据
            for (unsigned long long i = 0; i < n; ++i) {
              unsigned long long e = 0;
              if (!Rd64(beg + 8 * i, &e))
                break;
              if (e == pawn) {
                // 返回前补上和 ValidCarrier 同一道类型判据（CtxRefIsSpawner）。这个 c 会被拿去调
                //   vt[264]（摧毁实体），对着一个"形状碰巧像实体数组"的对象调就是任意调用。
                if (!CtxRefIsSpawner(c))
                  return false;  // 类型不对：放弃整个查找
                found = c;
                return false;  // 找到就停
              }
            }
          }
        }
        return true;  // 继续走
      },
      []() {});
  return found;
}

// req+64 指向的玩家记录还活着吗。1 = 在两个池子向量之一里找到了，0 = 两处都没有
// （已经被 free），-1 = 说不清（某一处读不到，或者向量的条数越界，见下面那道上限防御）。
//
//   要查它，因为 req+64 是照抄来的、指向某条玩家记录，而回收那一步可能刚把同一条
//   记录摘掉并 free（持久 AI 走的就是"我们自己补一刀"那条路）。把已释放的指针继续
//   当玩家上下文传给生成函数，会在召唤完当场闪退：实测回收的 player 和 req+64
//   是同一个地址，之后日志直接断掉。
//
//   两个容器都查，因为 AddPlayer 按 player+128 的 bit0 决定进 mgr+112 还是 mgr+664
//   （见 sub_140F554F0 里那两个分支），req+64 可能是任何一种；只查容器1 会把活着的
//   bot 记录误判成"已经没了"，那样我们就会把本来能用的 req+64 白白清掉。
//   只比指针，不解引用 p：它可能已经被 free 掉。
static int RecordAlive(unsigned long long mgr, unsigned long long p) {
  if (p == 0)
    return 0;
  // 拿不到 manager 时返回"说不清"，不要报 0：那是"确认它不在了"，
  //   会让调用方把一条可能还活着的 req+64 清掉，日志也会说成"记录已经没了"。
  if (mgr == 0)
    return -1;
  int unknown = 0;
  const unsigned int baseOff[2] = {112, 664};  // 容器1 / 容器2
  for (int k = 0; k < 2; ++k) {
    unsigned long long beg = 0, end = 0;
    if (!ReadVecSpan(mgr, baseOff[k], &beg, &end)) {
      unknown = 1;
      continue;
    }
    if (end <= beg)
      continue;
    const int n = (int)((end - beg) >> 3);
    if (n <= 0 || n > POOL_DEF_MAX) {
      unknown = 1;
      continue;
    }
    for (int i = 0; i < n; ++i) {
      unsigned long long q = 0;
      if (Rd64(beg + 8ull * (unsigned long long)i, &q) && q == p)
        return 1;
    }
  }
  return unknown ? -1 : 0;
}

// 记录 p 还在不在池子向量（mgr+112..120）里。在的话返回池子条目数，不在返回 0，
// 读不到返回 -1。空池子算"不在"（返回 0），只有读失败或者两个指针反了才是 -1：
//   两者对调用方的含义相反，混一起就会把"拆掉最后一条记录"当成说不清。
// 只比指针，不解引用 p（理由见上面 RecordAlive 那处）。
static int PoolHasRecord(unsigned long long mgr, unsigned long long p) {
  unsigned long long beg = 0, end = 0;
  if (!ReadVecSpan(mgr, 112, &beg, &end) || end < beg)
    return -1;
  const int n = (int)((end - beg) >> 3);
  if (n > POOL_DEF_MAX)
    return -1;
  for (int k = 0; k < n; ++k) {
    unsigned long long q = 0;
    if (Rd64(beg + 8ull * (unsigned long long)k, &q) && q == p)
      return n;
  }
  return 0;
}

// 把一条玩家记录从池子里摘掉。调用前实体那一半必须已经处理过，
// 也就是有实体的先走过那次 vt[264]，没有实体的（player+448 为 0）才直接来这里。
//
//   引擎对"摘记录"这半有两条入口，各自带一道守卫，按记录 +128 的 bit1 分流。
//   sub_141D6AFE0（拆实体那条）要在 (rec+128 & 2) == 0 时才摘，非持久 AI 走它；
//   sub_141DB4540 反过来，只在 (rec+128 & 2) != 0 时摘，持久 AI 走它。
//   后者比前者多一步 sub_141DB4300，它拿记录的 +520/+448 两个实体做组件查表，
//   调 sub_141DB0920 把实体从管理器的实体表（管理器 +64..+72）里摘掉、
//   把每队的 AI 计数（管理器 + 4*队号 + 1220）减一。少了这一步，记录是 free 了，
//   管理器那张表还指着已经拆掉的实体。参数从引擎自己的调用点抄，见 RVA_REMOVE_PERSIST。
//
//   f128 是调用方在读任何东西之前取好的：记录被 free 之后连它都不能再读。
//   返回 true 表示记录确实不在池子里了（这一格算腾出来了）。
static bool RemovePlayerRecord(unsigned long long mgr, unsigned long long rec, unsigned int f128) {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base) {
    Log("[RECYCLE] !! no module base, cannot remove the record");
    return PoolHasRecord(mgr, rec) == 0;
  }
  const bool persistent = (f128 & 2) != 0;
  if (persistent) {
    unsigned long long entMgr = 0;
    if (!Rd64((unsigned long long)(uintptr_t)(base + RVA_MGR_ENTITY), &entMgr) || entMgr == 0) {
      Log("[RECYCLE] !! the spawner manager singleton is unreadable, so the persistent record"
          " cannot be removed the engine's way; leaving it in the pool");
      return PoolHasRecord(mgr, rec) == 0;
    }
    typedef void(__fastcall * RemovePersistFn)(unsigned long long, unsigned long long,
                                               unsigned char, unsigned int);
    __try {
      ((RemovePersistFn)(base + RVA_REMOVE_PERSIST))(entMgr, rec, 1, 0);
    } __except (ExcFilter(GetExceptionInformation())) {
      Log("[RECYCLE] !! the engine's persistent removal threw, leaving it as it is");
      return PoolHasRecord(mgr, rec) == 0;
    }
  } else {
    // 非持久 AI 正常该由 sub_141D6AFE0 自己摘掉；走到这里说明别的原因没摘成，
    //   把它那两步照做一遍。
    typedef void(__fastcall * PlayerFn)(unsigned long long);
    typedef void(__fastcall * RemoveFn)(unsigned long long, unsigned long long);
    __try {
      ((PlayerFn)(base + RVA_PLAYER_CLEAN))(rec);
      ((RemoveFn)(base + RVA_PLAYER_REMOVE))(mgr, rec);
    } __except (ExcFilter(GetExceptionInformation())) {
      Log("[RECYCLE] !! removing the record ourselves threw, leaving it as it is");
      return PoolHasRecord(mgr, rec) == 0;
    }
  }
  // 这里打的是"这条记录还在不在"，不是池子大小：PoolHasRecord 返回
  //   >0 = 还在（值 = 池子条目数）、0 = 已经不在了、-1 = 读不到。
  const int after = PoolHasRecord(mgr, rec);
  Log("[RECYCLE] removed the record with the %s route; the record is now %s",
      persistent ? "engine's persistent-AI entry" : "hand-rolled pair",
      after == 0 ? "gone (the slot is free)"
                 : (after > 0 ? "STILL there (the removal did not take effect)"
                              : "unknown (the pool is unreadable)"));
  return after == 0;  // 只有记录真的没了才算腾出槽位
}

// 回收一个"己方最老的 AI 单位"，把玩家槽位腾出来。
//
//   必须做，玩家槽位池是有上限的，上限 = *(u32*)(mgr+16) - *(u32*)(mgr+1768)，
//   而 sub_140F56050 的判据是
//       occupied >= 上限 就 return 0
//   一满，sub_141D62790 就按 "if (!a2) return (vt+264)(ctx, entity, 0, 0)" 给刚造好的实体
//   撤单（那个 a3 是 0，见下面 a3 那段），而我们的单位不会自己消失、池子会一直被占着，
//   于是每次召唤都在这条边界上，表现就是时而正常、时而停住、时而翻阵营这些怪现象。
//   回收掉一个就把这一格还回去了。
//
//   挑法是从整个池子里挑"己方 + AI + 不是本地玩家"里最老的那个。
//     候选覆盖整个池子：池子是游戏一开始就填满的，只挑"我们自己召过的"话，
//       满员时第一个能回收的就是刚召出来的那个，拆掉它池子还是满的，等于白顶。
//     必须"己方"，顶掉敌方单位是不公平的。
//     本地玩家必须排除，那会让人从世界里消失，是不可挽回的错。
//     持久 AI（player+128 的 bit1）不排除：游戏自己的销毁入口对它们有守卫
//       （sub_141D6AFE0 里 owner+72 是 AI 且 (owner+128 & 2) == 0 才去摘记录），
//       拆完记录会留在池子里、槽位照样占着。这种就交给下面那一步补，见 RemovePlayerRecord。
//     "最老"的判据 = 池子向量（mgr+112..120）的下标顺序：它是纯追加写进去的
//       （sub_140F554F0 里 push_back），删除是 memmove 左移、不排序。
//       槽号（mgr+1784 的下标）由 sub_140472B60(mgr+1800) 发号、可复用，年龄看下标顺序。
//
//   怎么拆，走游戏自己的实体级入口 ctx 的 vt[264] = sub_141D6AFE0(pawnCtx, pawn, 1, 0)。
//   它一次调用做两件事，但只有实体那半是无条件的（从 ctx 实体数组里摘掉、解控制器、
//   发事件）；记录那半带一道持久 AI 守卫，被拦住时记录留在池子里，那时才算后面那一刀。
//   pawn 取 *(player+448)，pawnCtx 由 FindOwningCtx 遍历 spawner 链表找出来。
//
//   anyCtx 只是给 FindOwningCtx 一个链表起点（传我们当前正借的载体即可）。
//   返回 true 表示真的回收掉了一个。
static bool RecycleOldestTeamUnit(unsigned long long anyCtx) {
  unsigned long long mgr = GetManager();
  if (mgr == 0)
    return false;

  const int wantTeam = (int)AtomicReadL(&g_team);
  if (wantTeam <= 0) {
    Log("[RECYCLE] team not read yet, refusing to recycle (cannot tell friend from foe)");
    return false;
  }

  unsigned long long beg = 0, end = 0;
  if (!ReadVecSpan(mgr, 112, &beg, &end) || beg == 0 || end <= beg)
    return false;
  const int count = (int)((end - beg) >> 3);
  // 上限只是防御坏数据，不是业务上限：正常池子只有几十个。
  if (count <= 0 || count > POOL_DEF_MAX) {
    Log("[RECYCLE] pool size looks wrong (%d entries), refusing to touch it", count);
    return false;
  }
  // 过了"己方 AI"筛选的候选数：收尾日志靠它区分"池里没有可回收的"
  //   和"有候选、但都被跳过或拆不掉"。
  int candidates = 0;

  const unsigned long long me = FindLocalPlayer(mgr, nullptr);
  // 池子是追加写进去的，所以从前往后扫就是从老到新。
  for (int i = 0; i < count; ++i) {
    unsigned long long p = 0;
    if (!Rd64(beg + 8ull * (unsigned long long)i, &p) || p == 0 || !HeapPtr(p))
      continue;
    if (me && p == me)
      continue;  // 绝不动本地玩家
    unsigned int ai = 0, team = 0;
    if (!Rd32(p + PL_ISAI_OFF, &ai) || !(ai & 0xFFu))
      continue;  // 不是 AI（人类玩家也在池子里，跳过）
    if (!Rd32(p + PL_ID88_OFF, &team))
      continue;
    if ((int)(team & 0xFFu) != wantTeam)
      continue;  // 不是己方
    ++candidates;

    // 动手时走游戏自己的实体级入口，一次调用把两半都做完。
    //
    //   ctx 的 vt[264] = sub_141D6AFE0(ctx, pawn, a3, a4)，a3 非 0 时它自己做两件事：
    //     1) 从实体入手反查主人（pawn+600），主人是 AI 且不是"持久 AI"（+128 的 bit1）
    //        才动手，sub_140F5CE00 + sub_140F56720，把玩家记录从池子里摘掉、把槽位还回去。
    //        这里自带 PersAI 守卫，不用我们再判一次。
    //     2) sub_140EAED10(ctx, pawn, a3, a4) 再到 sub_140F4DB30：把实体从 ctx 的实体数组
    //        摘掉、解掉控制器、发"单位已移除"事件。
    //   不自己拼第 1 步，因为那样只销毁玩家记录、不销毁 pawn，会留下活体 pawn，
    //     而且 pawn 的 +600 还指着刚被 free 的玩家对象，游戏有三处会拿它再做虚调用
    //     和一次 free（二次释放）。
    unsigned long long pawn = 0;
    const bool hasPawn = Rd64(p + 448, &pawn) && pawn;
    // +128 在任何销毁动作之前读，记录被 free 之后连它都不能再读（位含义见 RemovePlayerRecord）。
    unsigned int f128 = 0;
    Rd32(p + 128, &f128);
    if (!hasPawn) {
      // 没有实体就没有实体侧要拆（vt[264] 要的正是 pawn），只剩记录侧。
      //   这种是"槽位占着、里面没有单位"的记录，还回去不用动任何活着的单位。
      Log("[RECYCLE] pool[%d]=%p has no pawn (player+448 == 0), removing the record itself", i,
          (void *)p);
      if (RemovePlayerRecord(mgr, p, f128))
        return true;  // 这一格腾出来了
      continue;       // 没腾出来，接着看下一个候选
    }
    // 必须先确认它是角色实体，sub_141D6AFE0 里第一句就是
    //   v9 = sub_141CD9220(pawn); v10 = v9[75];，pawn 不是角色实体时 v9 为 0，
    //   那句就是对着 600 号地址读，直接崩。所以过不了这一关就不许调。
    CastEntityFn cast =
        (CastEntityFn)((unsigned char *)GetModuleHandleA(nullptr) + RVA_CAST_CHARACTER);
    bool isChar = false;
    __try {
      isChar = (cast((void *)(uintptr_t)pawn) != nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      isChar = false;
    }
    if (!isChar) {
      Log("[RECYCLE] pool[%d]=%p pawn=%p is not a character entity, skipping it", i, (void *)p,
          (void *)pawn);
      continue;
    }
    const unsigned long long pawnCtx = FindOwningCtx(anyCtx, pawn);
    if (pawnCtx == 0) {
      Log("[RECYCLE] pool[%d]=%p pawn=%p: could not find the spawner that owns it, skipping it"
          " (passing a wrong ctx would leave the pawn in the world)",
          i, (void *)p, (void *)pawn);
      continue;
    }

    // a3 给 1。这个值是从代码语义推出来的，不是试出来的：它整条流向是
    //   它从 sub_141D6AFE0 转发到 sub_140EAED10、再到 sub_140F4DB30，全程只当布尔用：
    //     sub_141D6AFE0 里 if (a3) 才去摘主人玩家；
    //     sub_140F4DB30 里 !a3 时把实体挂回 ctx+472..488 的"待生成"向量（那是"取消"那一支），
    //       a3 非 0 且实体 +24 的 bit21（"在世界里"）置位时才走 sub_141088A20 真正摘掉。
    //   它还会被转发给 vt[352]（sub_140F4AA30，一个纯通知派发器，不分支），所以取值只可能是布尔。
    //   我们要的是"永久移除"，两处分支都指向非 0，所以给 1。
    typedef __int64(__fastcall * DestroyFn)(void *, void *, unsigned char, __int64);
    bool done = false;
    __try {
      ((DestroyFn)(*(void ***)pawnCtx)[33])((void *)pawnCtx, (void *)pawn, 1, 0);
      done = true;
    } __except (ExcFilter(GetExceptionInformation())) {
      done = false;
    }
    if (done) {
      Log("[RECYCLE] recycled the oldest team-%d AI (index %d of %d, player=%p pawn=%p ctx=%p)"
          " to free a player slot",
          wantTeam, i, count, (void *)p, (void *)pawn, (void *)pawnCtx);

      // 拆完立刻再读一次池子，这是"旧单位到底移没移除"唯一的直接判据。
      //
      //   上面那次调用只要没抛异常就会走到这里，但"没抛异常"不等于记录被摘掉了：
      //   游戏那边第一步带一道持久 AI（player+128 的 bit1）守卫，被拦住时只拆实体、
      //   记录留在池子里、槽位照样占着（实测被拦住的那些，同一条记录被连着回收三次、
      //   每次 pawn 都不一样，池子还在长）。
      const int n1 = PoolHasRecord(mgr, p);
      Log("[RECYCLE] after the destroy: the record is %s in the pool (pool %d entries, was %d,"
          " player+128=0x%X read before the call)",
          n1 > 0 ? "STILL" : (n1 == 0 ? "NOT" : "unreadable, cannot tell"), n1, count,
          f128 & 0xFFu);
      if (n1) {
        // n1 大于 0 是还在；小于 0 是池子读不出来（只有读失败、或者两个指针反了才是 -1，
        //   池子里空了那种返回的是 0）。
        //   两种都不算槽位已空，交给 RemovePlayerRecord 去摘并复查，返回它复查出来的结果。
        //   实体侧那半上面那次调用已经无条件做过（摘出 ctx 数组、解控制器、发事件），
        //   所以这里只剩记录侧。
        Log("[RECYCLE] the record is still there, or the pool is unreadable; removing it");
        return RemovePlayerRecord(mgr, p, f128);
      }
      return true;  // n1 == 0，游戏自己那一步就把记录摘掉了，槽位已空
    }
    // 抛异常就停手：这一步抛异常说明游戏那边状态不对，
    //   再对着别的候选调同一个函数只会更糟（也可能已经是半拆状态）。
    Log("[RECYCLE] !! destroying pool[%d]=%p pawn=%p ctx=%p threw, stopping here", i, (void *)p,
        (void *)pawn, (void *)pawnCtx);
    break;
  }
  if (candidates == 0) {
    Log("[RECYCLE] no team-%d AI in the pool to recycle (pool has %d entries, local player %s"
        " found)",
        wantTeam, count, me ? "" : "NOT");
  } else {
    Log("[RECYCLE] recycle failed: %d team-%d AI candidate(s) seen, none could be recycled"
        " (every skip or failure is logged above)",
        candidates, wantTeam);
  }
  return false;
}

// 在指定载体 ctx 上生成一个指定模板 v21。
//   player 参数有三个用处：读世界坐标（"在玩家位置生成"那一档）、读 player+72 并拿它调
//   sub_140F5CA10（诊断 req+64 那条路）、以及作为 me 传给 ValidCarrier（内部拿它问
//   玩家自己的 spawner）。生成函数拿到的是载体的 ctx 与 req 副本，不是 player 本身，
//   生成全程不碰玩家实体。
static bool SummonWithTemplate(void *ctx, unsigned long long v21, unsigned long long player) {
  if (!ctx || v21 == 0 || !g_spawn_orig)
    return false;

  // 载体必须先定下来，再读它身上的任何状态。
  //   oldTail（ctx+840 的旧值）、rotBefore（ctx+244）这些是"从载体读下来、最后要写回去"的，
  //   中途换载体的话，就会把 A 载体的值写进 B 载体，把新载体写坏。
  //
  // 目标模板的路径要读出来，既用来判阵营（挑 AISpawnData 见 FindOwnAd），
  //   也是出外观问题时核对"召的到底是哪个角色"的凭据。
  //   +24 存的才是路径串（扫描那边读的就是它），+32 是另一个串，别混。
  char tplPath[SUM_NAME_MAX] = {0};
  ReadAssetPath(v21, tplPath, sizeof(tplPath));
  // 阵营判据和 HUD 分列那边共用 ShortTemplateName，别在这里另写一份子串匹配：
  //   两套判据分头维护的话，同一条路径可能在一边算第三方、在另一边算植物。
  //   0 = 路径里没写阵营，不筛。
  unsigned char tplFaction = 0;
  ShortTemplateName(tplPath, &tplFaction);
  const int wantTeam = (int)tplFaction;

  // 载体的队号必须和目标一致（那条规矩、以及不守它的实测后果，写在 g_carrierByTeam
  //   的定义处）。这一支就是"当场发现队号不对就换一个同队候选"。
  if (wantTeam > 0) {
    const int cur = CarrierTeamOf((unsigned long long)(uintptr_t)ctx);
    // cur < 0 = 队号读不到，判不了就不动，别拿一个读不出来的值去触发换载体。
    if (cur >= 0 && cur != wantTeam) {
      unsigned long long alt = (unsigned long long)AtomicReadL64(&g_carrierByTeam[wantTeam]);
      if (alt && ValidCarrier(alt, player, wantTeam)) {
        Log("[INJECT] carrier team=%d but target team=%d, switching carrier %p to %p", cur,
            wantTeam, ctx, (void *)alt);
        ctx = (void *)(uintptr_t)alt;
      } else {
        // 槽里那个候选没过校验（或者槽本来就是空的），就当场清掉它、并叫 Worker 立刻重扫。
        //   不清的话它每次都会被判否、每次召唤都掉进下面这个降级分支；
        //   虽然 ScanPool 下一轮会重填，但让它早点发生更好。
        if (alt) {
          Log("[INJECT] !! carrier candidate %p for team %d failed validation, dropping it",
              (void *)alt, wantTeam);
          InterlockedCompareExchange64(&g_carrierByTeam[wantTeam], 0, (long long)(intptr_t)alt);
        }
        InterlockedExchange(&g_repickNow, 1);
        Log("[INJECT] !! carrier team=%d but target team=%d and no %s carrier is available;"
            " parts may come from the wrong faction",
            cur, wantTeam, wantTeam == 2 ? "plant" : "zombie");
      }
    }
  }

  unsigned long long oldTail = 0;
  if (!Rd64((unsigned long long)ctx + CTX_HOLDER_OFF, &oldTail))
    return false;
  // 记下这一刻载体的身份（地址 + spawner ID）。开窗前那次复核只答"这个地址上是不是 spawner"，
  //   答不出"还是同一个"：堆把这个地址复用给新的 spawner 时它照样通过，
  //   而 oldTail 是上一个的 +840（多半已经释放），还原那一步就把它写进新 spawner 的模板槽，
  //   等于让这个新 spawner 以后每次生成都去解一份悬垂的 AISpawnData。
  const unsigned long long carrierId = SpawnerIdOf((unsigned long long)(uintptr_t)ctx);

  // 这两个状态必须声明在 __try 外面：异常路径（__except）里要还原 ctx+244。
  //   只还原 ctx+840 是不够的，ATT_RESET_ROT 那一档会把 spawner 的"已生成计数"清零，
  //   异常一旦发生就永久留在 0，这个 spawner 的预算再也用不完，而且它同时还是"轮换索引"，
  //   游戏后续的刷怪节奏/变体轮换都会跟原版不一样。
  volatile unsigned int rotBefore = 0;
  volatile LONG rotTouched = 0;  // 1 = 我们把 ctx+244 清零过、还没还原
  // 1 = rotBefore 是真读出来的。读不出来时禁止把 rotBefore 写回去（那是 0，
  //   写回去等于把计数改成 0），所有还原点都要先看这个标志。
  volatile LONG rotKnown = 0;
  // 1 = 我们改过 ctx+840、还没还原。异常可能发生在改之前（外面那圈 __try 从更早的地方
  //   就开始了），那时候无条件写回老值会把游戏在这期间自己写进去的东西盖掉。
  //   游戏在生成期间确实会改写 ctx+840 再清掉，所以这个区分不是理论洁癖。
  volatile LONG holderTouched = 0;

  // 先把载体本身的状态记下来，尤其 Ops：那边的载体是 ScanPool 猜的（AI 路径
  //   一次都没见过）。这两个值是排障时最常要看的：552 = 闸门 (A) 的开关，576 = 队号。
  //   （552 为 0 的载体在 ValidCarrier 那一步就被拒了，走不到这儿：见那边的说明。）
  {
    unsigned int b552 = 0, team576 = 0;
    Rd32((unsigned long long)ctx + CTX_GATE_OFF, &b552);
    Rd32((unsigned long long)ctx + CTX_TEAM_OFF, &team576);
    Log("[INJECT] carrier ctx=%p  ctx+552(gate A)=%u  ctx+576(team)=%u   templates=%ld  "
        "AISpawnData entries=%ld",
        ctx, b552 & 0xFFu, team576, (long)g_tplN, (long)g_adN);

    // 把 player slot 表的大小和当前占用打出来（+16 那一段的来历见文件开头 ADDR_MANAGER_GLOBAL
    // 处）。
    //   它是运行期从配置读的，所以不同模式值不同，主线那边如果没上限，这个数会明显更大，
    //   而 ctx+244 那个索引就是拿它对表的，这也是"哪些模式要小心"的判据。
    //   +1768 是 bot 上限，我们的单位进的是容器1（mgr+112，玩家池；AddPlayer 按
    //   player+128 的 bit0 分流），占的是 "总数 - bot上限" 这个池子，和人类玩家同一个池。
    {
      // 走 GetManager()，不按原始偏移自己抠（它多做的两道校验见它的定义处）：
      //   抠出来的"能读但没意义"的指针只会给出垃圾数字，而这一行是"哪些模式要小心"
      //   的判据，错数比没有更坏。
      unsigned long long mgr = GetManager();
      if (mgr) {
        unsigned int total = 0, botCap = 0, used = 0;
        Rd32(mgr + MANAGER_SLOTS_OFF, &total);
        Rd32(mgr + MANAGER_BOTCAP_OFF, &botCap);
        unsigned long long beg = 0, end = 0;
        if (ReadVecSpan(mgr, 112, &beg, &end) && end >= beg)
          used = (unsigned int)((end - beg) >> 3);
        Log("[INJECT] slot table: total=%u botCap=%u occupied=%u (we use the non-bot pool,"
            " same as human players)",
            total, botCap, used);
      } else {
        // 读不到也要留一行，整行消失和"这行被删了"在日志上分不出来。
        Log("[INJECT] slot table unreadable (GetManager returned 0); pool size unknown for"
            " this summon");
      }
    }
  }

  __try {
    // 1) 搭临时持有者，+48 = 目标 v21，+88 = 血量覆盖
    //
    //   +88 是直接血量覆盖，反汇编 sub_141D6CFD0 0x141D6D270 一带：
    //       movss xmm1, [holder+58h]      ; 先取 holder+88
    //       comiss xmm1, 1e-6
    //       jnb，跳过整个 v21[11] 人数数组查表，只要 >= 1e-6 就不用数组
    //       ...  否则才用 array[人数 - 1]
    //       call sub_141E010C0(entity+608, /*xmm1=血量*/)
    //     所以把血量直接写在这里，完全绕过那个数组，与"当前几个人在打"无关。
    //     取哪一档由 SpawnHealthOf 决定：固定取第一档 = 单人局强度。
    unsigned int hpTiers = 0;
    float useHp = SpawnHealthOf(v21, &hpTiers);  // 取第一档

    // 底板绝不能用清零骨架，sub_140F4EF30 的两个 ctx 虚方法会读它。
    //     - vt[256] = sub_141D60630 ：if (a1[105]) {…} 走 holder 分支
    //     - vt[296] = sub_141D5E0F0 ：*a3 = sub_141E4E7D0(*(u32*)(holder+92))
    //                                return *(u64*)(holder+56)
    //   而 sub_141E4E7D0/760 是拿 holder+92 当索引查表、结果再被当指针用。
    //   清零，+92=0、+56=0，查出来的东西是垃圾，访问违例（实测过）。
    //
    //   来源按优先级排。
    //     1)  目标模板自己的 AISpawnData，+48/+56/+64/+72/+80/+92 天生自洽，
    //        而且不依赖任何生成事件，Ops 只能靠它。
    //     2)  退路，载体的 holder 副本。+48 换成目标模板，但 +56 Blueprint 仍是载体的，
    //        部件会挂到别人的骨架上，外观出问题（实测的绿色闪烁片状物）。
    //
    // tplPath / wantTeam 在函数开头就算好了（换载体要用），这里直接用。
    unsigned long long ownAd = FindOwnAd(v21, wantTeam);
    if (ownAd == 0 && !InterlockedExchange(&g_adScanTried, 1)) {
      // 表里没有，请 Worker 补扫。g_adScanTried 是闸门：每关最多请求一次，
      //   否则若某个模板确实没有 AISpawnData，每按一次召唤键都会请求一次重扫。
      //   （ResetInjectState 会把它清掉，所以换关后允许再试一次。）
      InterlockedExchange(&g_wantRescan, 1);
      Log("[TPL] no AISpawnData for this template in the table, asked the Worker to rescan"
          " (the game thread must not run the 650MB scan); using the carrier holder fallback now");
    }
    // 目标模板的字段快照。按角色分裂的外观问题（同一个模板有的正常有的不对）
    //   只能靠把字段摆出来比。
    //
    // 偏移和 EBX 字段名的对应是从模板资产逐条对出来的（PVZCharacterSpawnTemplateData）：
    //   +24 = Name（路径串）        +32 = DisplayName（loc ID，游戏拿它当实体名）
    //   +40 = Behaviour（AI 模板）  +48 = BehaviorTreeOverride
    //   +56 = CustomizeCharacterData（外观）  +64 = TargetingSettings
    //   +72 = BehaviorTreeParametersOverride  +80 = MovementStylesOverride
    //   +88 = HealthOverride（血量数组）
    // 判据是 +48 和 +80 在资产里是 nullptr、运行时读出来也正好是 0。
    {
      unsigned long long f[9] = {0};
      for (int k = 0; k < 9; ++k)
        Rd64(v21 + 24 + 8 * (unsigned long long)k, &f[k]);
      Log("[TPL] target template %s", tplPath[0] ? tplPath : "(unreadable path)");
      Log("[TPL]   +24=%p +32=%p +40(AI tpl)=%p +48=%p +56(appearance)=%p +64=%p +72=%p "
          "+80=%p +88(health)=%p",
          (void *)f[0], (void *)f[1], (void *)f[2], (void *)f[3], (void *)f[4], (void *)f[5],
          (void *)f[6], (void *)f[7], (void *)f[8]);
      // 直接问游戏，+40（Behaviour，AI 模板）过不过得了它自己的类型判定。
      //   过不了的话 sub_141D6CFD0 里 sub_141DA9460(v46, v48) 那一步不执行，
      //   AI 参数套不上去。
      unsigned long long appCast = 0;
      if (f[2]) {
        CastAppearanceFn cast =
            (CastAppearanceFn)((unsigned char *)GetModuleHandleA(nullptr) + RVA_CAST_APPEARANCE);
        __try {
          appCast = cast(f[2]);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          appCast = 0;
        }
      }
      Log("[TPL]   +40 passes type check = %s", appCast ? "yes" : "no (the Behaviour cast failed)");
    }
    if (ownAd == 0) {
      // 正常不会走到这里，列表只收"有自己的 AISpawnData"的模板。
      //   留着它是因为列表和 AD 表是两次独立读取，万一中间变了，退路总比直接崩好。
      //   真出现这行就说明列表和 AD 表不同步了，值得查。
      //
      // 换过载体时要把来源点明：holder 快照按"我们自己的载体优先、别的 ctx 先垫着"抓
      //   （来源有 1 和 2 两种，见 WorldInsertDetour 里的抓取闸门），
      //   所以换到同队候选之后 ctx != g_aiCtx，这份底板未必还是 g_aiCtx 的，
      //   +56 Blueprint 和 +92 阵营号都跟着它走。不说的话外观出错时会被归因到别处。
      Log("[TPL] !! template from the list has no own AISpawnData (list and AD table out of "
          "sync?), falling back to the carrier holder, appearance/AI will be wrong%s",
          (ctx != (void *)(uintptr_t)AtomicReadL64(&g_aiCtx))
              ? ".  NOTE: this summon swapped to a same-team carrier, so that holder snapshot"
                " comes from the game's carrier, a different spawner"
              : "");
    }

    bool gotHolder = false;
    if (ownAd) {
      // 直接从资产实例读一份（它不走我们的 g_holderCopy 刷新，所以不需要 seqlock）。
      memcpy(g_holderBase, (void *)ownAd, sizeof(g_holderBase));
      gotHolder = true;
      Log("[INJECT] base = the target's own AISpawnData %p, Blueprint matches too (+48=%p "
          "+56=%p +92=%u)",
          (void *)ownAd, *(void **)(g_holderBase + 48), *(void **)(g_holderBase + 56),
          *(unsigned int *)(g_holderBase + 92));
    } else if (g_holderCopyOk) {
      // 退路是用载体的 holder（+48 会被换成目标模板，但 +56 Blueprint 仍是载体的，
      // 外观会有问题）
      Log("[INJECT] no own AISpawnData for the target, falling back to the carrier holder "
          "(appearance may be wrong)");
      for (int t = 0; t < 8 && !gotHolder; ++t) {
        LONG s1 = g_holderSeq;
        if (s1 & 1) {
          SwitchToThread();  // 正在写，让一手再试，别把 8 次重试全耗在同一个窗口里
          continue;
        }
        memcpy(g_holderBase, g_holderCopy, sizeof(g_holderBase));
        if (g_holderSeq == s1)
          gotHolder = true;
      }
    } else {
      Log("[INJECT] neither the target's own AISpawnData nor a carrier holder was captured, "
          "giving up. (Both sources must be dead to get here; normally the scan finds the "
          "AISpawnData directly and no spawn event is needed.)");
      return false;
    }
    // 这个检查必需。
    //   8 次尝试都没拿到一致快照时（全部撞上正在写，或者每次都在 memcpy 之后被改写），
    //   gotHolder 仍是 false，g_holderBase 里可能留着一次撕裂的副本。+64/+80 是
    //   Frostbite 数组指针，撕裂值会让
    //   游戏去读 指针-4 当长度，访问违例，又被我们自己的 __try 吞掉
    // 外表正是"按 F5 / F11 毫无反应"（和清零骨架是同一类症状）。
    if (!gotHolder) {
      Log("[INJECT] holder snapshot never read cleanly in 8 tries, giving up "
          "(never use a torn copy as the base)");
      return false;
    }
    Log("[INJECT] holder base (%s)  +8=%p +48=%p +56=%p +64=%p +80=%p +88=%.1f +92=%u",
        ownAd ? "the target's own AISpawnData"
              : (g_holderSrc == 2 ? "carrier's (fallback)" : "other ctx's (fallback)"),
        *(void **)(g_holderBase + 8), *(void **)(g_holderBase + 48), *(void **)(g_holderBase + 56),
        *(void **)(g_holderBase + 64), *(void **)(g_holderBase + 80),
        (double)*(float *)(g_holderBase + 88), *(unsigned int *)(g_holderBase + 92));
    // 把 holder 的 +16..+47 当成 float 全打出来，用来查难度到底改不改攻击力。
    //   已知（静态），AISpawnData 的 Modifiers = DynamicDifficultyModifiers
    //   { GlobalResistance, GlobalDamage, GlobalHealth }，是唯一"设计上管攻击力"的难度旋钮；
    //   而抽样的两个资产里三个值都是 1。
    //   资产里的是静态默认值，如果运行时有个"动态难度导演"在改它，看资产就看不出来。
    // 这段日志就是判据，哪三个连续的 float 恰好是 1.0/1.0/1.0，那就是 Modifiers 的运行时位置；
    //     如果它们不是 1，说明运行时确实被改过。
    {
      float f[8] = {0};  // 必须先清零：下面 Rd32 失败时不至于打印未初始化的值
      for (int k = 0; k < 8; ++k)
        Rd32((unsigned long long)(uintptr_t)(g_holderBase + 16 + 4 * k), (unsigned int *)&f[k]);
      Log("[INJECT] holder +16..+47 as float: %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f"
          "  (three consecutive 1.000 = runtime Modifiers)",
          (double)f[0], (double)f[1], (double)f[2], (double)f[3], (double)f[4], (double)f[5],
          (double)f[6], (double)f[7]);
    }
    // +48 / +88 的写入放在梯子循环里：ATT_ASIS 那一档要求"一个字段都不改"，
    //    而每轮从 g_holderBase 重来才能保证改动不累积。

    // 2) req 优先用游戏那份的副本（最保真）；没有就用自己按字段表构造的。
    //    但绝不用"全零"，那是另一回事：全零时 req+92 = 0（不是 -1），ctx+244
    //       不递增，信号2) 直接没了；而且 req+64 = 0 会被当 owner 传进
    //       (*(vt[288]))(ctx, req+64, v40, …)。我们构造的那份是按字段表填过的，
    //       +88/+92 = -1、+96 = 1，Ops 只能靠它（那边一次 AI 生成事件都没有）。
    //    （两段缓冲都是本 DLL 的静态内存，memcpy 不会出错，所以不需要 SEH。）
    //
    //  顺序要求，所有"放弃"分支必须跑在下面那次 ForceWrite 之前。
    //     否则 return false 会把 ctx+840 永久留在我们的 g_fakeHolder 上
    //     （那条路不抛异常，__except 的还原不会执行），这个 spawner 之后每次生成
    //     都读我们的假 holder。
    BuildReq();  // 幂等，成本可忽略
    const unsigned char *reqSrc = nullptr;
    if (g_reqCopyOk) {
      reqSrc = g_reqCopy;
    } else {
      reqSrc = g_reqBuilt;
      Log("[INJECT] no game-side req copy, using the self-built one (+88/+92=-1, +96=1)"
          ", Ops levels with no AI spawn event depend on it");
    }
    memcpy(g_reqBase, reqSrc, sizeof(g_reqBase));

    // req+64 是"玩家上下文"。填不填要看下面两道闸能不能过，见这段的最后。
    //
    //   游戏自己是填的，sub_141D6D9B0 调 vt[232] 之前把第一个参数（玩家）写进 req+64
    //   （反编译里 v28 = a1，而 v28 正好落在 req+64）。它在下游被当玩家上下文用：
    //     - sub_141D60630（vt[256]）读 *(req+64) 判有没有玩家
    //     - sub_141D5E0F0（vt[296]）在 holder 为空时拿 *(u32*)(*(req+64) + 88) 当查表 key，
    //       那是 player+88，也就是玩家自己的队伍
    //   留 0 的代价是这些地方拿不到玩家上下文，生成出来的单位最后落到载体/默认的阵营上
    //   （实测，植物角色却算僵尸方，会治疗僵尸、打死不给任何一方加分）。
    //
    //   但填上会更糟，sub_141D6CFD0 开头有一段会因此生效
    //       v4 = *(req+64);
    //       if (v4) {
    //           v5 = (ctx->vt[456])(ctx);
    //           if (!*(BYTE*)(v4 + 72)) { if (v5) return 0; }   // 第一道
    //           if (!sub_140F5CA10(v4)) return 0;               // 第二道
    //       }
    //   player+72 那把"已就绪"标志只有游戏自己的重生流程才置起来，我们注入时它是 0，
    //   于是第一道就落到 vt[456] 上：它非 0 就 return 0（实测我们注入时四档全返回 0、
    //   连主线都召不出来）。
    //   第二道没法自己验，所以就算绕开第一道也不保险。
    //
    //   下面把这两个闸的实际值打出来，只作诊断；能不能填见后面那段结论。
    unsigned int p72 = 0;
    Rd32(player + PL_ISAI_OFF, &p72);
    bool gate2 = false;
    long long vt456 = -1;
    if (player) {
      // 第一道是 player+72 非 0 时整条跳过；为 0 时要 (ctx->vt[456])(ctx) 也非 0 才拦。
      //   vt[456] 是槽号 57（456/8）。它是游戏自己的虚方法，包 SEH 调。
      //
      //   注意这个值是在注入窗口之外读的，只能用于判断上面那两道闸，别拿它推断
      //   "这次生成会走哪条路"，sub_141D5EF60 第一句读的就是 ctx+840，而窗口里那一格
      //   是我们的假 holder，游戏实际看到的不是这个数（实测外面 0、里面 1）。
      //
      //   调用之前先自己认一遍对象：从 DoSummonSpawner 那次校验到这里隔着几次游戏调用
      //   （难度探针、回收），ctx 可能已经被释放或复用，而这一次调用要用它的虚表。
      //   下面开窗前那次复核在这之后才发生，而且那次是给写内存把关的，拦不住这里。
      //   认不过就不读，vt456 停在 -1（和抛异常同一种显示）。
      if (CtxRefIsSpawner((unsigned long long)(uintptr_t)ctx)) {
        __try {
          vt456 = ((VtFn)(*(void ***)ctx)[57])(ctx);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
          vt456 = -1;
        }
      }
      // 第二道是 sub_140F5CA10(player)。同样是纯检查，包 SEH 调。
      typedef long long(__fastcall * ChkFn)(unsigned long long);
      ChkFn chk = (ChkFn)((unsigned char *)GetModuleHandleA(nullptr) + RVA_OWNER_VALID);
      __try {
        gate2 = chk(player) != 0;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        gate2 = false;
      }
    }
    // 不要往 req+64 里填玩家指针（实测两次都失败）：
    //   填上之后 sub_141D6CFD0 开头那两道闸确实能过（实测 player+72=0、vt456=0、gate2=ok
    //   三个条件全满足），但下游还有一处会拦：sub_141D60630（vt[256]）里
    //       v6 = *(req+64);
    //       if (v6 && sub_141D03B50(v6 + off)) return 0LL;    // 玩家实体有效就直接返回 0
    //   填了玩家指针之后它就返回 0，sub_140F4EF30 里的 v5 跟着变 0，四档全部生成失败。
    //   能过哪几道闸不足以判断能不能填，这条依赖链比现在建模出来的长。
    //
    // 但"不动它"和"填玩家指针"是两回事：照抄来的原值要原样带过去，别清零也别改，
    //   它决定走野怪还是走 AI 玩家（见梯子开头那段）。
    // 这行把实际值打出来，它是"某个模式为什么出来的是 AI 玩家"的第一手判据。
    Log("[INJECT] req+64 = %p (kept as captured; nonzero means the game treats this as a mob"
        " placement, zero means the v15 branch: player slot when nonzero, cancellation when"
        " zero)   [player=%p, player+72=0x%X,"
        " vt456(outside the window)=%lld, gate2=%s]",
        *(void **)(g_reqBase + 64), (void *)(uintptr_t)player, p72 & 0xFFu, vt456,
        gate2 ? "ok" : "blocked");

    void *req = g_reqUse;  // 内容由梯子循环每轮从 g_reqBase 拷（见循环开头）

    // 3) 到线了就回收一个己方单位腾地方。
    //
    //   "线"是用分配器自己的上限（total - botCap）减掉 SLOT_CAP_MARGIN 得来的，
    //   不是另外拍一个数（理由见 SLOT_CAP_MARGIN 那段）。
    //   挑谁、怎么拆，见 RecycleOldestTeamUnit。
    //
    //   这一行同时是"我们的单位到底落进哪个容器"的判据：容器1（mgr+112）用
    //   total - botCap 卡、容器2（mgr+664）用 botCap 卡，而 AddPlayer 按 player+128 的
    //   bit0 决定进哪个。实测每次召唤 used 都会 +1，说明我们的单位进的是容器1，前提成立。
    // 槽位读数后面核对 req+64 那段还要用（判断还有没有空位可走），所以提到外面来。
    unsigned int slotCap = 0, slotUsed = 0, slotLimit = 0;
    bool slotKnown = ReadSlotPool(&slotCap, &slotUsed);
    bool slotFreed = false;
    if (slotKnown) {
      // 留出余量，别让分配器自己撞到 return 0（那条路是"实体挂回待生成向量"）。
      //   上限本身不比余量大时给不出余量，只能按上限本身管：那种模式里余量要防的那条
      //   边界确实防不住，这是它的固有限制，退化支不假装留了余量。
      slotLimit = (slotCap > SLOT_CAP_MARGIN) ? slotCap - SLOT_CAP_MARGIN : slotCap;
      Log("[INJECT] player slot pool: %u used, allocator cap %u, effective cap %u", slotUsed,
          slotCap, slotLimit);
      if (slotLimit > 0 && slotUsed >= slotLimit)
        slotFreed = RecycleOldestTeamUnit((unsigned long long)(uintptr_t)ctx);
    } else {
      // 同上：读不到也要出声。
      Log("[INJECT] player slot pool unreadable for this summon (not in a level?)");
    }

    // 3b) 照抄来的 req+64 先核对一次：它是"这具身体接到哪个玩家记录上"的凭据。
    //
    //   记录自己存着一个角色（v10 + dword_143FBCDB0 + 6040，正是角色链的第三来源），
    //   生成完之后实体走的就是它，记录里是什么角色，出来就是什么角色。
    //   实测（战斗箱子），召 Scientist_Champion 那次，链里 [1] 是我们注入的 Scientist、
    //   [3] 是记录里存的 Astronaut；召 Acorn 那次记录里是 SnapDragon，
    //   出来一只模型拉丝的火龙草，技能组也是火龙草的。
    //   req+64 为 0 的那几次反而是对的：游戏新分配一个空记录，角色由 [1] 那份填。
    //
    //   所以这里一共五种结局。
    //     1) 记录不在池子里（回收那一步刚把它 free 掉）或者池子读不出来（说不清）：无条件清 +64。
    //        传悬垂指针是闪退（实测），这一支不看有没有空位，失败也比崩强。
    //        判据是"在不在池子里"而不是"字段值对不对"：free 之后字段读出来是 0，
    //        只看值的话正好绕过去；"说不清"也按"不在"处理，宁可这次退化成新分配，
    //        也不拿一个可能已 free 的地址去赌。
    //     2) 记录里存的角色不是目标、而且还有空位可走，清 +64，改走新分配那条路。
    //     3) 记录里就是目标，不清：那是游戏自己的重生流程。
    //     4) 没空位可走，不清：清了也没地方放。
    //     5) 记录活着但字段读不出来（不知道里面是谁），保守留着并明确打一行日志，
    //        免得排查时把它当成"字段真的是 0"。
    {
      unsigned long long rec = 0;
      Rd64((unsigned long long)(uintptr_t)(g_reqBase + 64), &rec);
      if (rec) {
        const int alive = RecordAlive(GetManager(), rec);
        unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
        unsigned int roleOff = 0;
        unsigned long long recChar = 0, recPawn = 0;
        bool read = false;
        // 只有"确实还活着"才去读它的字段：记录可能刚被回收那一步 free 掉，
        //   那块内存要么是空闲块、要么已经被分配器给了别人，读出来的是垃圾
        //   （实测读到的是一串 0，那正是"不再能用"的样子）。
        if (alive == 1 && base && HeapPtr(rec) && ReadFieldOffset(&roleOff))
          read = Rd64(rec + roleOff + ROLE_SRC3_OFF, &recChar);
        if (read)
          Rd64(rec + 448, &recPawn);
        // 回收成功也算空出一格，口径和上面的判断一致。
        const bool freeSlot = slotKnown && slotLimit > 0 && (slotUsed < slotLimit || slotFreed);
        Log("[INJECT] req+64 record %p: %s, character %p%s, pawn %p (we are spawning %p)",
            (void *)rec,
            alive == 1 ? "alive in the pool"
                       : (alive == 0 ? "NO LONGER in the pool"
                                     : "pool unreadable, cannot tell whether it is alive"),
            (void *)recChar, read ? "" : " (fields unreadable, not real zeros)", (void *)recPawn,
            (void *)v21);
        if (alive != 1) {
          // 这一支不需要 freeSlot：传悬垂指针是闪退（实测），失败也比崩强。
          *(unsigned long long *)(g_reqBase + 64) = 0;
          Log("[INJECT] req+64 cleared: %s. Passing it on is a crash risk (a freed record is taken"
              " as the player context); this spawn takes a freshly allocated record instead",
              alive == 0 ? "the record is gone from the pool (freed by the recycle step?)"
                         : "the pool could not be read, so it cannot be proven alive");
        } else if (read && recChar && recChar != (unsigned long long)(uintptr_t)v21 && freeSlot) {
          *(unsigned long long *)(g_reqBase + 64) = 0;
          Log("[INJECT] that record holds a different character, so the game would rebuild that "
              "unit"
              " instead of ours; cleared req+64%s",
              recPawn ? " (the record still had a live pawn)" : "");
        } else if (!read) {
          // 这一支的处置见上面那张清单的第 5 条。
          Log("[INJECT] req+64 record is alive but its fields could not be read; keeping it"
              " (cannot tell which character it holds)");
        }
      }
    }
    // 4) 把 spawner 的模板槽指向它，从这里开始才动游戏内存。
    //    走 ForceWrite 是保险不是必需：ctx 本身是普通堆对象（工厂与分配尺寸见上面那段），
    //       花两次 VirtualProtect 换"万一碰上只读页也不当场崩"，值得。
    // 标志先抬起来再改内存，假 holder 必须在窗口之内，不能先装再开窗。
    //   反过来的话，那一瞬间撞进来的、游戏自己的生成会被世界钩子报成
    //   "窗口外有人读了我们的假 holder"，方向和事实正好相反，查的时候被带偏。
    // 开窗之前再验一次载体，DoSummonSpawner 那次校验之后隔着几次游戏调用
    //   （难度探针、回收），ctx 可能已经被游戏释放或复用。这里失败还没写任何东西，
    //   直接退回就是安全的。
    const char *whyWin = nullptr;
    if (!ValidCarrier((unsigned long long)(uintptr_t)ctx, player, 0, &whyWin)) {
      Log("[INJECT] carrier failed the re-check right before the injection window (%s), aborting",
          whyWin ? whyWin : "unknown");
      return false;
    }
    //   复核过了还要再问一句"还是不是同一个 spawner"，用的是 SameSpawner 那条判据
    //   （地址 + spawner ID，两边只要有一个 ID 读不出来就按"是同一个"放行），
    //   与取 oldTail 时记下的 carrierId 比。理由见 carrierId 那处。
    if (!SameSpawner((unsigned long long)(uintptr_t)ctx, (unsigned long long)(uintptr_t)ctx,
                     carrierId)) {
      Log("[INJECT] the spawner at this address changed while we were working (id mismatch),"
          " aborting before the injection window");
      return false;
    }
    InterlockedExchange(&g_injecting, 1);  // 注入窗口开始（给世界钩子做判据）
    if (!ForceWrite64((unsigned char *)ctx + CTX_HOLDER_OFF,
                      (unsigned long long)(uintptr_t)g_fakeHolder)) {
      Log("[INJECT] failed to change page protection for ctx+840, giving up");
      InterlockedExchange(&g_injecting, 0);  // 窗口还没真正开成，关掉
      return false;
    }
    holderTouched = 1;  // 从这一行起才需要还原
    // 从这一行到还原之后的关窗口之间，不许出现 return（普通 return 不触发 __except，
    //   会静默把标志泄漏成 1，之后真出问题反而报不出来，还会报成"别人撞进窗口"那条错故事）。

    unsigned int rotAfter = 0;
    // rotBefore 是 volatile（见它的声明）：过一下临时量才能传给 Rd32 的 unsigned int*
    unsigned int rbTmp = 0;
    if (Rd32((unsigned long long)ctx + CTX_SPAWNCOUNT_OFF, &rbTmp)) {
      rotBefore = rbTmp;
      rotKnown = 1;
    } else {
      Log("[INJECT] ctx+244 unreadable at entry; its restore is skipped for this summon");
    }

    // 5) 依次试梯子上的几种组合，调的都是 ctx 的虚方法 vt[232] = sub_141D6CFD0，
    //    不经过 sub_141D6D9B0，那一层会先销毁/重置玩家实体。
    //    一次最多成功一个（!ok 守住）。
    // 顺序必须和上面 enum 的取值一一对应（0 玩家位置 / 1 忠实照抄 / 2 ctx+244重置 /
    //   3 原样不动）。条数也要一致：漏一个就是 nullptr，日志里 %s 打它会出事。
    static const char *kAttName[ATT_COUNT] = {"at player", "faithful copy", "reset ctx+244",
                                              "as-is (diag)"};
    bool ok = false;
    // 成功的两条腿里是哪一条成立的。见 g_lastWeakOk 的说明。
    bool lastOkByRet = false;
    // 永远从第 0 档（玩家位置）开始试，也就是第 0 档是唯一入口。
    //   第 0 档是设计上正确的那一档，而且它天然用"自带变换"，不依赖游戏挑生成点。
    //   记忆式优化（"上次成功的档"）会把一次偶发失败固化成永久降级：某次读不到玩家坐标
    //   （换关瞬间 / ent+40 暂时无效）就会记下 ATT_FAITHFUL，之后每次召唤都跳过
    //   "生成在玩家位置"，而观感只是"偶尔位置不对"，极难排查。
    //   梯子的下半段要保留，第 0 档真失败时还能退到别的档把 bot 生出来（位置随游戏挑）。
    const LONG start = ATT_AT_PLAYER;
    // 数一下"真正调了生成函数的有几档"和"其中抛异常的有几档"。
    //   整条梯子跑完还一场空、而且每一个真正调过的档都抛异常，这个载体基本可以判死
    //   （正常的载体就算被闸门拦着也只是返回 0，不会抛异常）。见循环之后那段。
    // 要有 calledN，因为有些档会因为 skipCall 提前 continue、根本不调生成函数
    //   （比如读不到玩家坐标时 ATT_AT_PLAYER 就会跳过），
    //   拿 threwN 去比 ATT_COUNT 就永远凑不够数，判死逻辑会静默失效。
    int threwN = 0;
    int calledN = 0;
    // 把"从哪一档开始"打出来。没这行日志的话，"玩家位置那一档根本没被执行"是看不出来的
    //   （枚举重排 + 初始值写死名字就会变成这样）。
    Log("[INJECT] ladder starts at [%s] (%d attempts)", kAttName[start], ATT_COUNT);

    for (int step = 0; step < ATT_COUNT && !ok; ++step) {
      int att = (int)((start + step) % ATT_COUNT);

      // 每轮都从快照重来，否则上一轮改的字段会累积到下一轮
      memcpy(g_reqUse, g_reqBase, sizeof(g_reqUse));
      memcpy(g_fakeHolder, g_holderBase, sizeof(g_fakeHolder));
      // req+64 在这里不再改动，照抄来的原值怎么来就怎么用（唯一的例外是 3b) 那段：
      //   记录里存着别的角色时，那边会在进梯子之前把它清 0）。
      //   它是 sub_141D62790（vt[280]）的第二个参数，也就是 a2，直接决定走哪条路：
      //       if (a2) goto LABEL_35;   // 非 0：不分配玩家槽位，直接放置 = 野怪
      //       if (v15 == 2) goto LABEL_36;
      //       if (!v15) return (vt+264)(ctx, entity, 0, 0);   // a3=0，撤单挂回待生成向量
      //       ...其余走 sub_140F56050 分配一个玩家槽位 = AI 玩家
      //   实测同一套注入代码，主线出来的是野怪、填充人机的模式出来的是 AI 玩家，
      //   差别就在这里，游戏把 req+64 填成什么，我们就得原样带过去。
      //
      //   这里必须保持原值，不许清零：清零会把主线本来走得好好的野怪那条路一起掐掉。
      //   被 vt[256]（sub_141D60630）拒绝的是"往里填玩家指针"那条路，不是"保持原值"。
      //
      //   注意 req+64 只可能在 g_reqBase 里（= 从 AI 路径照抄的那份）；自己造的
      //   BuildReq 里它恒为 0。也不要往回填任何指针，理由见上面那条实测。
      if (att != ATT_ASIS) {
        // 目标模板方面，只有"退路底板"才需要换 +48，目标自己的 AISpawnData 里
        // +48 本来就是我们要的模板，动了反而错。
        if (ownAd == 0)
          *(volatile unsigned long long *)(g_fakeHolder + 48) = v21;
        // 血量覆盖与底板无关，除 ATT_ASIS 那一档（一个字段都不动）以外，只在 useHp > 0
        //   时才写；useHp 为 0 表示模板不分档，保留底板原值。
        if (useHp > 0.0f)
          *(volatile float *)(g_fakeHolder + 88) = useHp;
      }

      // 判据基准 = "我们这轮动完手之后，ctx+244 应该是多少"：
      //   不碰它的档，rotBefore；ATT_RESET_ROT 写成功，0；写失败，仍是 rotBefore。
      //   两边都按本轮实际动手结果填：填错任何一边都会把"没变"判成"变了"，假成功。
      unsigned int rotStart = rotBefore;
      bool resetRot = false;
      bool skipCall = false;
      switch (att) {
        case ATT_FAITHFUL:
          // 什么都不改就是照抄，和 default 的行为一样。显式写出来是因为
          //   kAttName 里它是一档有名字的档，不写这个 case 读起来像是漏了。
          break;
        case ATT_RESET_ROT:
          // 把生成计数归零，sub_140F475F0 的 ctx+244 < 上限 重新成立。
          // resetRot 只在写成功之后才置位：写失败时值还是 rotBefore，
          //   置位的话下面"归零过就还原"会打出一行"预算仍处于归零状态"，
          //   而它根本没被归零过，日志把人往错的方向带。
          if (ForceWrite32((unsigned char *)ctx + CTX_SPAWNCOUNT_OFF, 0)) {
            resetRot = true;
            rotTouched = 1;  // 记下来：异常路径也要还原
            // 判据基准 = "我们写完之后这个值应该是什么"，也就是 0。
            //   写成功就必须把基准改成 0；否则 rotBefore 3，写进去的 0 会被判成
            //   "递增了"，假成功、梯子提前收工、HUD 报 OK 而实际什么都没生成。
            rotStart = 0;
          } else {
            // 写失败，值仍然是 rotBefore，基准不能动：改成 0 的话，真值非 0 时
            //   会把这个"没变"误判成"变了"。
            Log("[INJECT] ctx+244 zeroing failed (page protection), this attempt is wasted");
          }
          break;
        case ATT_AT_PLAYER: {
          // 把玩家的世界变换整块抄进 req+0..63，再把 req+97 置 1。
          //   变换从哪来见 GetPlayerWorldXform（确认值 *(ent+40)，每次现读）。
          //   解不出来就什么都不写，这一档自然失败，梯子退到 ATT_FAITHFUL，
          //     也就是"位置由游戏自己挑"。宁可位置不对，也不要把 bot 扔到 NaN 坐标上。
          //
          //   +97 = 1 不只是"用自带变换"：sub_140F4EF30 里那个分支是
          //     if (req+97) { 变换直接从 req 抄 }  else if (!(vt[288])(...)) return 0;
          //   设了它，vt[288]（挑生成点那套）根本不会被调用。
          //   所以这一档也必须设它，清 req+80 但不设 +97 的话，
          //     vt[288] 照跑，位置服从那个模式自己的生成规矩（实测战斗箱子里就是），
          //     而它交回来的变换在 GigaTorchwood 这种大体型上还会出片状模型。
          //   两者一起做，位置和模型都回到和其它模式一样的路子。
          static unsigned char xf[64];
          const char *which = nullptr;
          bool gotXf = false;
          // 唯一来源 = 现场读玩家世界变换（每次召唤都重读一遍）。
          //   缓存玩家生成变换拿到的是出生点快照：实测 4 次召唤跨 56 秒、
          //   坐标恒为 (-76.6, 129.3, -308.4)，玩家走动后完全跟不上，
          //   而且它一命中就把别的读法短路掉，日志里连一行 [POS] 都不会有。
          if (player && GetPlayerWorldXform(player, xf, &which)) {
            gotXf = true;
            Log("[INJECT] using live-read player world transform (from %s)", which);
          }
          if (gotXf) {
            memcpy(g_reqUse, xf, 64);
            *(unsigned char *)(g_reqUse + 97) = 1;
            Log("[INJECT] pos (%.1f, %.1f, %.1f) + req+97=1", *(float *)(xf + 48),
                *(float *)(xf + 52), *(float *)(xf + 56));
          } else {
            // 什么都不写，这一档的 req/holder 与下一档逐字节相同，
            // 所以直接跳过调用，别白烧一次生成。
            skipCall = true;
            Log("[INJECT] no player world transform, skipping this call"
                " (fall through to the next tier; game picks the position)");
          }
          break;
        }
        case ATT_ASIS:
          // 纯诊断档：holder 一个字段都不动（req 侧另有两处例外，见循环开头那段）。
          break;
        default:
          break;
      }

      // req+80 是 vt[280] 那条链的最后一环（见 enum 那档的说明）：照抄来的那份可能是个
      //   已经失效的栈指针，链会去解引用它，于是抛异常。
      //
      // 每一档都要清，不能只让最后一档兜底：抛异常时 sub_141D6CFD0 是"造到一半"被
      //   打断的，实体已经出来了（名字在早期就设好），但后面的行为树和外观都没来得及套。
      //   那个半成品会留在世界里，外观是"站着不动、名字对但身体不对"。
      //   只留最后一档清的话，前面每失败一档就多留一个半成品（实测 61 次第 1 档就成功、
      //   19 次前三档全抛，后者正好对应"一按召唤出来好几个、有的动有的不动"）。
      //
      // ATT_ASIS 也照样清。它虽然叫"原样不动"，但同样会真的调生成函数，
      //   而 req+80 不是模板字段、是那条链的终止环，留着失效指针换不来任何诊断价值，
      //   只换来一次 call [-1] 崩。
      *(unsigned long long *)(g_reqUse + 80) = 0;

      if (skipCall)
        continue;  // 这一档没有可写的东西（和下一档相同），省一次生成

      // 每一档单独套一层 SEH。
      //   整条梯子只有一个外层 __try 的话，某一档崩了会直接跳到外层 __except，
      //   后面的档（尤其是 ATT_RESET_ROT 那个"清零生成预算"的保底档）根本没机会跑。
      //   实测会崩在 sub_141D62790（ctx 的 vt[280]）的 call [-1]，
      //   于是 4 档一个都不再被尝试，变成"召唤永远失败"。
      //   把异常关在这一档里，让它只作废自己。
      ++calledN;  // 从这一行往下才是真的调了生成函数（判死逻辑用它当分母）
      __int64 r = 0;
      bool threw = false;
      __try {
        // 标出"这一响是我们自己的"，世界钩子会在这个调用内部同步触发，
        //   它拿这个标志区分"我们"和"别人撞进窗口"（见 g_inOurCall 的说明）。
        InterlockedExchange(&g_inOurCall, 1);
        r = g_spawn_orig(ctx, req);
        InterlockedExchange(&g_inOurCall, 0);
      } __except (ExcFilter(GetExceptionInformation())) {
        InterlockedExchange(&g_inOurCall, 0);  // 异常展开也要清，不然会一直挂着
        threw = true;
      }
      if (threw) {
        // 这一档抛异常，ctx+244 也可能已经变了，不只有"我们清零过"这一种情况：
        //   sub_140F4EF30 在造完实体之后会 ++ctx+244，那之后才抛的话增量就留下了。
        //   所以这里不拿 resetRot 当唯一判据，读一次比对、不同就写回原值。
        //   只看 resetRot 的话，那道"把索引还原"的修复就只覆盖成功路径，
        //   而抛异常恰好是最容易留下增量的那条路，索引照样漂到槽表外。
        unsigned int now244 = 0;
        bool know244 = Rd32((unsigned long long)ctx + CTX_SPAWNCOUNT_OFF, &now244);
        if (resetRot || (know244 && now244 != rotBefore)) {
          if (rotKnown == 0) {
            // 进门就读不出原值，还原不了：写 rotBefore（0）只会把计数改得更错。
            rotTouched = 0;
            Log("[INJECT] !! ctx+244 unreadable at entry, cannot restore after the exception");
          } else if (ForceWrite32((unsigned char *)ctx + CTX_SPAWNCOUNT_OFF, rotBefore)) {
            rotTouched = 0;
          } else {
            Log("[INJECT] !! ctx+244 restore failed (exception path), the spawn index keeps"
                " drifting");
          }
        } else if (know244) {
          rotTouched = 0;  // 值没变，没什么可还原的，别让外层再试一遍
        }
        // 这里故意是 continue 而不是 break，别改成 break。
        //   抛异常确实说明游戏可能已经做了一半（实体建了、ctx+400/408/416 的数组
        //   push 了），那些副作用我们还原不了，这是这条路的固有代价。
        //   但"只作废这一档、继续试下一档"是防"召唤永远失败"的关键：
        //   实测第 0 档会因为 req 的某个字段触发 call [-1]（code=0xC0000027，
        //   RVA=0x1D627F6，访问 0xFFFFFFFFFFFFFFFF）而抛异常，那条路是常见的，
        //   改成 break 就等于每次召唤都在第 0 档上死掉，整个功能哑掉。
        //   两害相权，宁可偶尔让游戏带着半初始化的状态继续跑，也不能让召唤永远失败。
        // 把异常细节打出来。
        //   失败全发生在游戏代码里，没有 code/RVA 就无从查起（外层那个 __except
        //   在这条路上永远不会执行，它只兜住没被这一层接住的异常）。
        ++threwN;
        ExcFacts ex;
        ExcFactsOf(g_exc, &ex);
        Log("[INJECT] tier %d [%s] threw during the call (code=0x%08X RVA=0x%llX "
            "accessed=0x%llX)"
            ", voiding this tier only, trying the next one",
            step + 1, kAttName[att], ex.code, ex.rva, ex.accessed);
        continue;
      }
      // 先把 rotAfter 置成 rotStart 再读，万一 Rd32 失败（读不到），
      //    它就保持 rotStart，ok 判为 false，而不是拿上一轮的残留值误报成功。
      // 读成功与否单独记下来，还原那一步要区分"值没变"和"读不出来"
      //    （读不出来时按"可能变过"处理，宁可多还原一次）。
      rotAfter = rotStart;
      const bool rotAfterKnown = Rd32((unsigned long long)ctx + CTX_SPAWNCOUNT_OFF, &rotAfter);

      // 两个互相独立的成功信号，任一成立即算成功：
      //   1)  r != 0，sub_141D6CFD0 的返回值就是类型过滤后的实体
      //      （v9 = sub_141CD9220(v7)，是 ServerPVZCharacterEntity 才非 0）。
      //      这正是游戏自己的判据，sub_141D6D9B0 结尾就是 return v19 != 0。
      //   2)  ctx+244 递增，造出并登记进 ctx 数组成功后 +1（sub_140F4EF30 的 LABEL_51）。
      //      它即使在"造出来的不是角色"（1) 为 0）时也能说明闸门有没有放行。
      bool okByRet = (r != 0);
      bool okByRot = (rotAfter != rotStart);
      // spawned = 游戏那边有没有真的动过手（两个原始信号）。ok 是在它之后再被事后存活检查
      //   往下压的，而下面 ctx+244 的还原要看的是"游戏有没有 +1"，与那个检查无关：
      //   实体造出来又被拆掉时，计数照样前进过一格，不还原就还是越界那条老路。
      const bool spawned = okByRet || okByRot;
      ok = spawned;
      if (ok)
        lastOkByRet = okByRet;

      // 事后存活检查必须有的理由。ret != 0 只说明 sub_141D6CFD0 造出了一个角色实体，
      //   不说明它被接进了世界。实体是在 sub_140F4EF30 里先造出来的，sub_141D62790
      //   之后才决定放不放。它在"池子满了"或 v15 == 0 时会直接调 (vt+264) 把刚造好的
      //   实体拆掉，而那种情况下我们这边 ret 照样非 0、照样报 "a spawn really happened"。
      //   所以"卡着不动的单位"完全可能是"造出来了、随后被拆掉"，日志上却毫无异常。
      //   这里查两件事。
      //     1) 它还在不在某个 spawner 的实体数组里（被拆掉的话就不在了）；
      //     2) 它有没有主人（*(entity+600)），没有主人的单位不会动。
      if (okByRet) {
        unsigned long long ent = (unsigned long long)r;
        const bool inCtx = (FindOwningCtx((unsigned long long)(uintptr_t)ctx, ent) != 0);
        unsigned long long owner = 0;
        unsigned int f24 = 0;
        Rd64(ent + 600, &owner);
        Rd32(ent + 24, &f24);
        Log("[INJECT] post-spawn check: entity=%p still in a spawner's list=%s  owner=%p"
            "  entity+24=0x%X%s",
            (void *)ent, inCtx ? "yes" : "NO", (void *)owner, f24,
            (inCtx ? (owner ? "" : "  (no owner, it will not move)") : "  (TORN DOWN)"));
        // 这次检查也参与判成败：上面两个条件任一不成立，世界里就没有（或有一个不会动的）
        //   单位，报成功会让操作者看到"绿字但没东西"。
        //   注意压成失败之后梯子并不会停：循环条件是 !ok，所以这里判否会让它接着试下一档，
        //   同一发召唤里最多把四档都试一遍。只有整条梯子跑完都失败，外层那次重试（最多三次）
        //   才会再来一发。两种"再多试一次"的代价不一样，改这里之前先想清楚要哪一种。
        if (!inCtx) {
          ok = false;
          Log("[INJECT] the entity was torn down right after the spawn, counting this attempt"
              " as a failure");
        } else if (owner == 0) {
          ok = false;
          Log("[INJECT] the entity has no owner and will not move, counting this attempt as a"
              " failure");
        }

        // 主人记录里存着的角色，才是这具身体生成完之后算哪个角色（见前面核对 req+64 那段）。
        //   和目标不一致就说明它被接到了别的记录上，出来会是那个记录的角色。
        if (owner) {
          unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
          unsigned int roleOff = 0, f128 = 0;
          unsigned long long recChar = 0;
          if (base && ReadFieldOffset(&roleOff) &&
              Rd64(owner + roleOff + ROLE_SRC3_OFF, &recChar)) {
            Rd32(owner + 128, &f128);
            // record+128 一并打出来：bit1 = 持久 AI（游戏不会替我们摘它的记录）。
            //   我们自己召出来的单位是不是持久 AI，靠这一个数就能判。
            Log("[INJECT] post-spawn: its record %p holds character %p (we asked for %p)  %s,"
                " record+128=0x%X",
                (void *)owner, (void *)recChar, (void *)v21,
                recChar == (unsigned long long)(uintptr_t)v21 ? "match" : "MISMATCH", f128 & 0xFFu);
            if (recChar && recChar != (unsigned long long)(uintptr_t)v21) {
              ok = false;
              Log("[INJECT] the record holds a different character, so this body comes out as"
                  " that one; counting this attempt as a failure");
            }
          }
        }

        // 补挂出生 buff：只有这一发真的成功、单位确实活着才挂。
        //   放在存活检查和角色核对之后，和游戏自己的生成组一致：只有真正进世界的单位才该拿到。
        if (ok)
          ApplySpawnBuffs(v21, ent);
      }

      Log("[INJECT] try[%s]: v21=%p hp=%.1f(%s) ret=%lld  ctx+244 %u "
          "to "
          "%u  "
          "ret!=0=%s rot244-inc=%s  %s",
          kAttName[att], (void *)v21, (double)useHp,
          hpTiers ? "hp array present, taking #1" : "no hp array", r, rotStart, rotAfter,
          okByRet ? "yes" : "no", okByRot ? "yes" : "no",
          ok ? "a spawn really happened"
             : (spawned ? "the game spawned something but the unit did not survive"
                        : "nothing spawned"));

      // 把载体的轮换索引还原成我们进来时的值：这一档归零过要还预算，
      //   真生成成功过要撤销游戏自己 +1 的那一格漂移。
      //
      //   必须还，因为 ctx+244 是 spawner 的 SpawnCount，同时被当 slot 索引：
      //       v15 = req+92;  if (v15 == -1) v15 = *(u32*)(ctx+244) & 0x7F;
      //   而 v15 被 sub_140F56050 直接当 slot id 用，没有任何边界检查：
      //       *(u32*)(*(mgr+1808) + 4*(v15>>5)) |= 1 << (v15&0x1F);   // 位图置位
      //       *(u64*)(*(mgr+1784) + 8*v15) = 新槽位;                    // 指针数组写入
      //   槽表只有 *(u32*)(mgr+16) 项（本作实测 70）。我们每召一次它就 +1 且永不回落，
      //   攒够几十次就索引越界，而且越界是写不是读，会踩坏内存：
      //   于是出现"偶尔翻边、停住、不给分"这些杂症，而且召唤越多越容易犯。
      //
      //   不要改成"绕回表内"（取模），那样索引会真的指到活单位的槽位，游戏就会走
      //      "复用槽位"那条路，可能把别人顶掉。我们不做任何顶替。
      //   还原成原值就没这个问题，索引不前进，每次都是同一个下标。那个槽里是上次召的单位、
      //      还活着（游戏判定为"忙"），于是游戏自己改成新分配，一直新分配到池满为止，
      //      既没有复用也没有顶替，和现在的行为一致，只是不再越界。
      //   也不要清零，清零等于每次都用下标 0，和还原的区别只是"用哪个固定值"，
      //      而还原用的是游戏自己给的值，最不打扰它。
      // 还原发生在两种情况下，这一档归零过（把预算还回去），或者游戏这次真的生成了、
      //   计数被 +1 了（撤销那一格增量）。归零档不看 spawned：这一档已经动过预算，
      //   所以不管这一发生成成功没有，都要把它还回去。
      //   这里用 spawned 不用 ok：事后存活检查压下来的那些失败，计数同样前进过。
      const bool needRestore =
          rotKnown && (resetRot || (spawned && (!rotAfterKnown || rotAfter != rotBefore)));
      if (needRestore) {
        if (ForceWrite32((unsigned char *)ctx + CTX_SPAWNCOUNT_OFF, rotBefore)) {
          Log("[INJECT] ctx+244 restored to %u (was %u at restore time; stops the spawn index"
              " drifting out of the slot table)",
              rotBefore, rotAfter);
          rotTouched = 0;  // 归零档也算收尾了
        } else {
          Log("[INJECT] !! ctx+244 restore failed, the spawn index keeps drifting");
        }
      } else if (rotKnown == 0 && (resetRot || spawned)) {
        // 进门时读不出原值，没法还原：写回去的只会是 0，比不写更糟。
        rotTouched = 0;  // 别让外层异常路径再试一遍
        Log("[INJECT] ctx+244 unreadable at entry, the restore is skipped for this summon");
      }
    }

    // 全部档都抛异常，这个载体判死，丢掉它。
    //
    //   判据取"每一档都抛异常"不取"召唤失败"：
    //     正常的载体就算被闸门拦住，也只是 sub_140F4EF30 返回 0，不会抛异常。
    //     而这里连 ATT_ASIS（一个字段都不改、纯把 ctx 交回给游戏）都抛，
    //     说明这个 ctx 本身已经不能用了：实测就是"spawner 已经退役但内存还在"。
    //
    //   清掉是必须的，不清的话它会被一直用下去，因为
    //     src=1（ScanPool 猜的）：ScanPool 只在 src==0 时才换，不会覆盖
    //     src=2（AI 路径实测）：AI 路径那句是 if (g_aiCtxSrc != 2)，也不会覆盖
    //   实测症状（战斗箱子），连续 4 档全抛异常、HUD 报 FAIL，之后要等 4 秒
    //   AI 路径碰巧生成一次才换到新载体。丢掉之后 ScanPool 2 秒内就会重挑。
    if (!ok && calledN > 0 && threwN == calledN) {
      // 结论里带上 req+64。所有档共用同一份 req，而 req+64 是"上一次生成时抓的指针、
      //   随时可能失效"（字段表见 SpawnDetour），所以"档档都抛"也有可能是 req 坏了、
      //   不是载体坏了。看上面每档的 RVA 就能分辨：都落在同一处，更可能是 req，
      //   尤其 req+64 非 0 的时候。这里把值打出来，省得回头再猜。
      Log("[INJECT] all %d called tiers threw (of %d tiers, %d skipped; req+64=%p  req+80=%p)"
          ", dropping carrier %p",
          threwN, ATT_COUNT, ATT_COUNT - calledN, *(void **)(g_reqUse + 64),
          *(void **)(g_reqUse + 80), ctx);
      // 丢掉"实际用的那个"载体。有两处可能指着它，都要按"是不是同一个对象"各判一次：
      //   同队槽（换过载体时指着它）和 g_aiCtx（没换时就是它本身）。
      //   写成二选一的话，没换的那一次只清了槽，把已经判死的 ctx 留在 g_aiCtx 里，
      //   之后每次召唤都重新验证它、再全抛一遍；Ops 里没有 AI 路径来覆盖 g_aiCtx，
      //   于是永久 FAIL。反过来也不能无条件清 g_aiCtx：换过载体时那个是健康的。
      if (wantTeam >= 1 && wantTeam <= 2 &&
          InterlockedCompareExchange64(&g_carrierByTeam[wantTeam], 0, (long long)(intptr_t)ctx) ==
              (long long)(intptr_t)ctx)
        Log("[INJECT] dropped the team-%d candidate (it was the one in use)", wantTeam);
      if (DropCarrierIfCurrent((unsigned long long)(uintptr_t)ctx))
        Log("[INJECT] dropped %p from g_aiCtx (it was the carrier in use)", ctx);
      // 记进"刚坏掉的载体"排除集，ScanPool 的候选是"链表里第一个能用的"，完全确定，
      //   不排除的话它 50ms 后会被原样重挑、再全抛一遍（机制见 g_recentBad 的说明）。
      //   排除集在换关时清掉，免得地址被复用后被永久挡掉。
      g_recentBad[InterlockedIncrement(&g_recentBadN) & (RECENT_BAD_SLOTS - 1)] =
          (unsigned long long)(uintptr_t)ctx;
      InterlockedExchange(&g_repickNow, 1);  // 通知 Worker 别等 2 秒节拍，立刻重挑
    }

    // 6) 还原模板槽，不还原的话这个 spawner 之后每次生成都用我们的模板。
    //    还原失败必须报出来，那意味着这个 spawner 永久指着 g_fakeHolder，而
    //    g_fakeHolder 会被下一次注入改写：它以后生成的兵会变成我们上次召唤的
    //    模板 + 我们的血量，而且没有任何别的痕迹。
    // 窗口标志在还原之后才关，反过来先关的话，会出现一个"假 holder 还装着、
    //   标志已经说窗口关了"的瞬间，那时若真有人读它，报警的方向就反了。
    if (!ForceWrite64((unsigned char *)ctx + CTX_HOLDER_OFF, oldTail))
      Log("[INJECT] !! ctx+840 restore failed, this spawner keeps using our fake holder"
          " (everything it spawns later will be the player-summoned template)");
    InterlockedExchange(&g_injecting, 0);  // 注入窗口结束
    // "成功"是哪一种，给 HUD 用。只有 ctx+244 动了（生成函数的返回值是 0）时算弱成功：
    //   那种情况下世界里确实多了一个单位，但它不保证是被请求的那个角色
    //   （ATT_ASIS 退回载体 holder 时造出来的是载体的普通单位）。
    InterlockedExchange(&g_lastWeakOk, (ok && !lastOkByRet) ? 1 : 0);
    return ok;
  } __except (ExcFilter(GetExceptionInformation())) {
    // 异常路径也必须还原，否则 ctx+840 会一直指着我们的 g_fakeHolder。
    //    只在自己真改过时才写回，外面那圈 __try 从更早的地方就开始了，异常可能发生在
    //    我们动手之前，那时无条件写回会把游戏自己在这期间写进去的值盖掉。
    if (holderTouched && !ForceWrite64((unsigned char *)ctx + CTX_HOLDER_OFF, oldTail))
      Log("[INJECT] !! exception path: ctx+840 restore failed, this spawner keeps our fake holder");
    InterlockedExchange(&g_injecting, 0);  // 异常展开也要关掉窗口标志
    // 异常路径同样要还原 ctx+244（见 rotTouched 的说明）
    if (rotTouched) {
      if (rotKnown) {
        // 写回失败要和成功分开报：失败时这个 spawner 的预算就永久停在 0，
        //   日志说成 restored 会把后续"这个 spawner 不再出怪"的错误归因到别处。
        if (ForceWrite32((unsigned char *)ctx + CTX_SPAWNCOUNT_OFF, rotBefore))
          Log("[INJECT] !! exception, ctx+244 restored to %u (otherwise this spawner's budget"
              " stays zeroed)",
              rotBefore);
        else
          Log("[INJECT] !! exception, ctx+244 restore FAILED; this spawner's budget stays zeroed");
      } else {
        Log("[INJECT] !! exception with ctx+244 unreadable at entry; the zeroed budget cannot be"
            " restored");
      }
    }

    // 把异常的出错指令 RVA 和访问的地址打出来，
    //    这两个数拿去 IDA 一比，就知道是哪条指令、哪个指针坏了。不用再猜。
    // ExceptionRecord 也要判：下面两处解引用它，而这个函数体本身就在异常处理里，
    //   在那儿再抛一次没有任何上层接得住，进程直接没了、日志也留不下。
    ExcFacts ex;
    if (g_exc && ExcFactsOf(g_exc, &ex)) {
      Log("[INJECT] !! exception code=0x%08lX  fault RVA=0x%llX  (base=%p)  accessed=0x%llX  "
          "ctx=%p",
          (unsigned long)ex.code, ex.rva, ex.faultAddr, ex.accessed, ctx);
      g_exc = nullptr;
    } else {
      Log("[INJECT] !! exception (no exception record available), template slot force-restored");
    }
    return false;
  }
}

// 难度探针要落锤的那个问题：那个"档位索引"是难度还是人数。
//
//   必须查，它决定血量数组那 4 个数该叫什么。已知的是 sub_140F57F70（= sub_141DB2E10
//   的真身）签名的形状是"遍历一个链表、数符合条件的元素"（计数，不是读枚举），
//   而条件里的 *P+72 恰好就是用来判"是不是 AI"的那个偏移。
//
//   结论（判据见 SpawnHealthOf）：它数的是"非 AI 参与者人数"，那 4 个血量是
//   1P~4P 的人数缩放，不是难度。所以现在只有在每关前几次召唤时才跑（g_diffProbeN），
//   留着的价值是它顺带把池子每个槽的 +72 / +128 打出来。
//
//   判据很硬，把那个链表的元素指针，和我们自己找到的本地玩家对象比，
//   一样，它就是玩家表，"人数"成立；不一样，这个推断就错了，得重找。
//   sub_140F57F70 是纯读（只遍历链表）而且只带一个参数，调用是安全的；仍然套 SEH。
//   纯诊断，发布版里整块编掉（调用点在 DoSummonSpawner 的 _DEV 块里）。
#ifdef _DEV
static void ProbeDifficultyTier() {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base)
    return;
  // 必须走 GetManager()，不能自己 Rd64(ADDR_MANAGER_GLOBAL + MANAGER_OFF)。
  //   那是 GetManager() 的第二条候选、且没经过"玩家数组"校验；下面还要拿
  //   GetManager() 的玩家对象跟这儿的链表元素比对，混两个来源的话那个"人数落锤"
  //   的结论就是无意义的（而且可能对一个没校验的指针调游戏函数）。
  unsigned long long mgr = GetManager();
  if (mgr == 0) {
    Log("[DIFF] mgr=0 (not in a level yet), cannot probe");
    return;
  }
  unsigned int r = 0xFFFFFFFFu;
  __try {
    typedef unsigned int(__fastcall * DiffFn)(unsigned long long);
    r = ((DiffFn)(base + RVA_GET_DIFFICULTY))(mgr);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    r = 0xFFFFFFFFu;
  }
  unsigned long long beg = 0, end = 0;
  ReadVecSpan(mgr, 112, &beg, &end);
  long cnt = (beg && end > beg) ? (long)((end - beg) >> 3) : 0;
  Log("[DIFF] sub_140F57F70(mgr) returned %u; list mgr+112..120 = %p..%p, %ld elements", r,
      (void *)beg, (void *)end, cnt);

  int nslots = 0;
  unsigned long long me = FindLocalPlayer(mgr, &nslots);
  Log("[DIFF]   local player object = %p (%d player slots)", (void *)me, nslots);
  for (long i = 0; i < cnt && i < 8; ++i) {
    unsigned long long p = 0;
    if (!Rd64(beg + 8ull * i, &p) || p == 0)
      continue;
    unsigned int b72 = 0, b128 = 0;
    Rd32(p + PL_ISAI_OFF, &b72);
    Rd32(p + 128, &b128);
    // 整个 +128 都打出来，不是只看 bit0：bit0 = 进哪个容器，bit1 = 持久 AI（PersAI）。
    //   只打 bit0 的话，回收那边"游戏自己不肯摘这条记录"的原因在日志里看不见。
    Log("[DIFF]   [%ld] elem=%p  +72(AI test)=%u  +128=0x%X (bit0=pool, bit1=PersAI)   %s", i,
        (void *)p, b72 & 0xFFu, b128 & 0xFFu,
        (p == me) ? "this is the local player object, player count confirmed" : "");
  }
}
#endif

// 目标模板是不是当前这一关的活模板。
//
//   注入前必须再验一次，排队里的 v21 可能是上一关解析出来的
//   （召唤块被加载/换关拖了很久才轮到），也可能是列表刚被清空那一刻从旧列表读出来的。
//   那种指针已经悬垂，交给游戏就是"拿一块已释放的内存当模板"：
//   轻则召出个片状物，重则崩在游戏里，而且日志上看不出它是从哪来的。
//
//   具体判据，以及为什么宁可单读一个字段也不查表，见函数体开头那两段。
static bool TemplateIsLive(unsigned long long v21) {
  if (v21 == 0)
    return false;
  // 判据 = 直接问这个对象自己，类型描述符还在不在。
  //   扫描发现 Template 实例用的就是这一条（命中位置 - 8 就是实例，描述符在它的 +8），
  //   所以"能列在表里"和"过得了这一关"是同一个条件，不会误伤。
  //
  //   判据不查 g_tplTab，因为那张表最长 30 秒才刷新一次，而关卡资产是流式卸载的。
  //   模板被卸载之后表里的 v21 就是悬垂指针，而这一次召唤读的其它东西（AD 表、召唤列表）
  //   也是同一批旧的，互相自洽、谁都发现不了。这一条不依赖表的时效性，代价一次 SEH 读。
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base)
    return false;
  unsigned long long desc = 0;
  if (!Rd64(v21 + 8, &desc) || desc != (unsigned long long)(uintptr_t)(base + RVA_TYPEDESC_TPL)) {
    // 顺便把表怎么说打出来，表认它而描述符不认，就是"表过期"这件事被当场抓到了一次。
    bool inTable = false;
    LONG n = AtomicReadL(&g_tplN);
    for (LONG i = 0; i < n; ++i) {
      if (g_tplTab[i].v21 == v21) {
        inTable = true;
        break;
      }
    }
    Log("[INJECT] target %p is not a live template any more (descriptor=%p, want=%p,"
        " the table says: %s)",
        (void *)v21, (void *)desc, (void *)(uintptr_t)(base + RVA_TYPEDESC_TPL),
        inTable ? "still listed, so the table was stale" : "not listed either");
    return false;
  }
  return true;
}

// F5 / F11 召唤，自造：把目标角色的模板注入到一个 spawner 里，再直接调生成函数。
//
//  分三步。
//    1. ScanTemplates 已把所有已加载的 Template 实例（= 所有角色）扫进 g_tplTab，
//       并由 BuildSummonLists 切成左右两组（队友/敌人，各含角色+档位两级）
//    2. 目标由按键那一刻解析好并存进 g_queuedV21（见 QueueSummon），由 DispatchDetour 取走传进来
//    3. SummonWithTemplate 注入到载体的模板槽（ctx+840），调生成函数，还原
//
//  seq 是这一发属于哪一次按键（重试状态的主人，见 g_retrySeq）。由 DispatchDetour 从载荷里
//    带进来，中途不再取全局量：跑这一发的几十毫秒里，按键线程可能已经按了下一发。
//
//  不要走 sub_141D6D9B0：它那一层会先销毁/重置玩家实体，借任何在本关当过
//     玩家出生点的 ctx 都会让人物直接消失（机制见 DumpTrySpawn 里持有者那一段）。
static void DoSummonSpawner(unsigned long long v21, LONG seq) {
  unsigned long long me0 = FindLocalPlayer(GetManager(), nullptr);
  if (me0 == 0) {
    // 不在关卡里（或还没分组）：这一发不算"失败了"，是这时按了不该有反应。
    //   不进重试，也不写 HUD，一行日志就够。
    Log("[SUMMON] local player unreadable (not grouped yet?)");
    return;
  }

  // 目标由 DispatchDetour 从 g_queuedV21 取走后传进来，来自 QueueSummon 排队那一刻。
  //     目标固定为 QueueSummon 那一刻（按 F5 / F11 的瞬间）选中的那个，
  //        否则的症状是"HUD 高亮换人、召唤键却永远召同一个角色"。
  {
    // 名字要抄下来，不能只存指针：g_queuedName 是按键线程下一次按键就会改写的全局缓冲，
    //   连按两下时它会把新角色名盖到这次召唤的日志和 HUD 结果上（v21 本身是快照过的，不受影响）。
    char tplName[SUM_NAME_MAX] = {0};
    CopyStr(tplName, sizeof(tplName), g_queuedName);
    // 排队太久就作废，别执行。没有任何 AI 刷怪的地方（大厅、加载中、某些模式）
    //   DISPATCH 一声不响，那次请求会一直挂着，之后突然冒出一个早忘了的角色，
    //   日志上还归因不到当时那一次按键。
    LONG qt = (LONG)AtomicReadL(&g_queuedT);
    LONG age = (LONG)(GetTickCount() - (DWORD)qt);
    if (qt && age > QUEUE_TTL_MS) {
      Log("[SUMMON] queued request expired (%ld ms old, over %d ms), dropping it", age,
          QUEUE_TTL_MS);
      SummonAttemptFailed(seq, "FAIL expired");
      // 载荷已经在调用方取走了，这里不能再动 g_queuedV21：
      //   再清一次会把这段窗口里新按下的一发一起清掉。
      return;
    }
    unsigned long long aiCtx = (unsigned long long)AtomicReadL64(&g_aiCtx);
    const char *whyBad = nullptr;
    if (aiCtx && !ValidCarrier(aiCtx, me0, 0, &whyBad)) {
      // 悬垂/被复用/是玩家自己的/在本关被当作过玩家的生成点/刚判死过/已停用
      //   （ctx+552 == 0），丢掉它。
      // 正常情况下走不到这儿，Worker 每 50ms 只读 ctx+552 看一眼（廉价），
      //   而完整的 ValidCarrier 只在这一次召唤时做，所以这里就是那道完整校验。
      // 原因一定要打出来：这几条判否里只有三条自己有日志，其余全靠这一行。
      Log("[INJECT] carrier ctx=%p failed validation (%s), dropping it and asking Worker to"
          " repick, press again",
          (void *)aiCtx, whyBad ? whyBad : "unknown");
      // 这里只发通知，让 Worker 不等那 2 秒节拍、这一轮就重挑：
      //   判否多数是暂时的状态（刚被玩家占用、552 刚变 0），下一轮重挑时它可能又能用了，
      //   所以不把它记进排除集。
      // 通知放在这里而不是靠 Worker 的轮询：轮询只读 ctx+552，看不出"描述符变了/成了玩家的"
      //   这类失效，那类只能在这儿被发现。
      // 丢掉它用 DropCarrierIfCurrent：从上面读到这个值到这一行，中间夹着一整段
      //   ValidCarrier（要调游戏函数、多次 VirtualQuery）加一次带 fflush 的日志，
      //   几十到几百微秒；这期间 Worker 的 ScanPool 可能刚发布了一个新载体，
      //   无条件清会把它连同来源标记一起抹掉，而那个新载体还没被校验过、
      //   甚至可能是 AI 路径的地面真值。CAS 失败说明值已换人，交给下一轮重挑。
      DropCarrierIfCurrent(aiCtx);
      InterlockedExchange(&g_repickNow, 1);
      aiCtx = 0;
    }
    if (aiCtx == 0) {
      // 手上没有载体。正常情况下不会发生：Worker 每 50ms 主动验一次、失效就重挑，
      //   所以按键时它应该是有效的。真到这儿就是"那一刻确实没有可用 spawner"
      //   （不在关卡里 / 链表里一个启用中的都没有）。
      // 顺手要求立刻重扫：重挑的正常延迟是 50ms，不要求的话要等下一次 2 秒巡检，
      //   那这三次重试（总共 1 秒）会全落空。
      //
      // 这一支同时是"载体被清空、来源标记却没清"那个非法状态的修复点。
      //   两个量是手工配对的，各自都是原子写、配在一起不是，所以能落到
      //   "ctx 空着、来源标记停在 1 或 2"上。产生点有三个，形状都是"写完 ctx、
      //   还没写来源标记的那两条指令之间被别的清空插进来"：
      //     1) ScanPool 的 CAS 清空（与 SpawnDetour 的两次写交错）；
      //     2) Worker 的 552 体检 CAS 清空（同上）；
      //     3) ClearStaleAfterLevelChange 的清空（它自己那两个量是配对写的，
      //        但夹在 SpawnDetour 那两步之间时照样能留下 0 配 2）。
      //   这一支按状态修，三个产生点都覆盖。
      //   那个状态自己没有出口：重扫的填值要求来源是 0，AI 路径的复写要求来源不是 2，
      //   ValidCarrier 那条清空又要求载体非 0，三条路全进不去，只有换关会无条件清。
      //   落进去就是这一关剩下的召唤一路 FAIL。
      //   修法刻意选在这里，而不是改写入顺序或把两个量并成一个：
      //   这里是唯一"既看得见这个状态、又正好要重挑"的地方，就地修回来最短。
      //   清之前再看一眼 ctx，免得游戏线程刚填进来的合法组合被抹掉来源。
      Log("[INJECT] no usable AI carrier ctx (not in a level / no enabled spawner this match)");
      if (!AtomicReadL64(&g_aiCtx))
        InterlockedExchange(&g_aiCtxSrc, 0);
      InterlockedExchange(&g_repickNow, 1);
      SummonAttemptFailed(seq, "FAIL no carrier ctx");
    } else if (!TemplateIsLive(v21)) {
      Log("[INJECT] target v21=%p not in the current template table (left over from the last level?"
          " list just cleared?), refusing to inject, wait for the rescan",
          (void *)v21);
      SummonAttemptFailed(seq, "FAIL stale target");
    } else {
      Log("[INJECT] self-made: target %s   v21=%p   carrier ctx=%p (src: %s)", tplName, (void *)v21,
          (void *)aiCtx, g_aiCtxSrc == 2 ? "AI path (observed)" : "ScanPool (guess)");
      // 难度探针只在前几次召唤时跑（纯读、在游戏线程上，见 g_diffProbeN 的说明）。
#ifdef _DEV
      if (InterlockedIncrement(&g_diffProbeN) <= DIFF_PROBE_PER_LEVEL)
        ProbeDifficultyTier();
#endif
      bool ok = SummonWithTemplate((void *)aiCtx, v21, me0);
      // 把结果写给 HUD 那一行（g_last_ok / g_last_msg / g_last_t），
      //   先写内容、最后才写 g_last_t：HUD 线程用 g_last_t 当"有没有新消息"的判据，
      //      这样它要么看到完整的新内容、要么看到旧时间戳（最坏是晚一帧）。
      bool weak = ok && ((LONG)AtomicReadL(&g_lastWeakOk) != 0);
      if (ok) {
        // 这里不动重试预算与待办：重试状态只有本序列号那一发的失败会去写
        //   （见 SummonAttemptFailed 的 seq 判据），而同一序列号里的尝试是串行的
        //   （下一次只在这一次的结果回来后才排），所以此刻待办必然是 0。
        //   这里若去清一次，反而会误伤并发的那一发：跑这一发时又按了一下，那次刚把预算重置好，
        //   被这一清就变成"失败一次都不重试"。
        char okMsg[SUM_NAME_MAX + 8] = {0};
        _snprintf_s(okMsg, sizeof(okMsg), _TRUNCATE, "%s %s", weak ? "OK?" : "OK", tplName);
        SetSummonResult(1, okMsg);
        if (weak)
          Log("[INJECT] result is a weak OK: the spawn counter moved (so something appeared), but"
              " the game's own return value was 0, so it may not be the requested character");
        return;
      }
      Log("[INJECT] injection failed, see the [INJECT] lines above (return value and ctx+244 say "
          "where it stuck)");
      {
        char why[SUM_NAME_MAX + 8];
        _snprintf_s(why, sizeof(why), _TRUNCATE, "FAIL %s", tplName);
        SummonAttemptFailed(seq, why);
      }
    }
  }
  Log("[SUMMON] this attempt failed; the [INJECT] / [TPL] lines above point at the step");
}

// spawner 切换检测，见 g_last_role 的说明。这是探针的主过滤器。
static void RoleWatch(LONG n, void *ctx, void *entity) {
  unsigned long long ref = 0, r24 = 0;
  if (!ctx || !Rd64((unsigned long long)ctx + 48, &ref) || ref == 0)
    return;
  if (!Rd64(ref + 24, &r24))
    return;

  // 这个 spawner 这次在造什么，只在这里读得到（ctx+840 两次生成之间是 0）。
  //   读一次两处共用，下面的载体特征表，以及"换了一个 spawner"那条摘要日志。
  //   两个附带值也只有此刻读得对，所以一起取：
  //     vt456 = sub_141D5EF60 = ctx 的 vt[456]，它第一句看 ctx+840 空不空，两支兜底
  //       不同（空那支 0、不空那支 1），两次生成之间读出来恒是 0；
  //     蓝图 = holder+56。
  //   我们自己注入那几次 ctx+840 塞的是假 holder，会把 vt456 和蓝图都带偏，那两个跳过。
  // 同一个 spawner 会反复通知，而读模板路径是四次 SEH 读加整串复制。发布版这里没有
  //   人需要它，先比 r24 就能省掉；开发构建不能提前退，下面的载体特征表每次生成都要更新
  //   （同一个 spawner 会造不同模板）。
  const unsigned long long prevRole = (unsigned long long)AtomicReadL64(&g_last_role);
#ifndef _DEV
  if (r24 == prevRole)
    return;
#endif
  char tpl[SUM_NAME_MAX] = {0};
  unsigned int asz = 0;
  const bool gotTpl = ReadTplInfo(ctx, tpl, sizeof(tpl), &asz);
  // 载体特征表那三样（生成期的 vt456、原生模板、原生蓝图）只有开发构建里有人读：发布版
  //   唯一的消费者 IsOwnSpawnPoint 只读 playerPath，而那条由 TRYSPAWN 钩子里的另一个
  //   写入点写。所以整段只在开发构建里跑，省掉游戏线程上每次生成的那次虚调用、一次路径读
  //   和一次内核查询（HeapPtr 走 VirtualQuery）。
#ifdef _DEV
  //   调那个虚方法之前先认一下对象（ref 就是上面刚读到的那个，不用再读一遍）。
  //   调虚方法前必须先认对象：实参不是 spawner 类时，槽 57 就是一次任意虚调用。
  const bool isSpawner = IsSpawnerRef(ref);
  if (gotTpl && g_inOurCall == 0 && isSpawner) {
    long long vt = -1;
    __try {
      vt = ((VtFn)(*(void ***)ctx)[57])(ctx);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      vt = -1;
    }
    char bp[SUM_NAME_MAX] = {0};
    unsigned long long holder = 0, bpPtr = 0;
    if (Rd64((unsigned long long)(uintptr_t)ctx + CTX_HOLDER_OFF, &holder) && holder &&
        Rd64(holder + 56, &bpPtr))
      ReadAssetPath(bpPtr, bp, sizeof(bp));
    NoteCarrierBuild((unsigned long long)(uintptr_t)ctx, tpl, bp, vt);
  }
#endif

  if (r24 == prevRole)
    return;  // 还是同一个 spawner，不刷屏（开发构建走到这里才判，见上面的说明）
  InterlockedExchange64(&g_last_role, (long long)r24);

  static volatile LONG roleDumps = 0;  // 只用来比上限，没别处读，放函数内
  LONG k = InterlockedIncrement(&roleDumps);
  if (k > ROLE_DUMP_MAX) {
    // 超过上限就只留个心跳，说明"那个值确实在变"，那它多半不是稳定的 spawner 标识。
    if (g_probe_enabled && k % 200 == 0)
      Log("[WORLD] (spawner value changed %ld times, over limit %d, heartbeat only)", k,
          ROLE_DUMP_MAX);
    return;
  }
  // 通知 SPAWN detour"这次确实出了新角色"，放在上限判断之后：
  //   超过上限的那些生成不再复算 v21，日志也不会被刷爆。
  //   这一句是功能，不受下面的探针开关管。
  InterlockedIncrement(&g_role_seq);
  // 下面整段都是 dump，和四个 detour 一样受 g_probe_enabled 管。
  //   这一段的量是全文件最大的（一次一百多行、上限 80 次），漏判开关就等于关不掉它。
  if (g_probe_enabled) {
    Log("[WORLD] spawner switch #%ld (WORLD #%ld): spawnRef=%p   spawnRef+24(spawner ID) = "
        "0x%016llX   (prev = 0x%016llX)",
        k, n, (void *)ref, r24, prevRole);

    // 一行摘要，这个 spawner 这次在造什么角色（详细结构由下面的 DumpRoleChain 打）。
    if (gotTpl)
      Log("[WORLD] this spawner is building %s   (hp array size=%u)", tpl, asz);

    if (ctx)
      DumpCtx("WORLD-role", ctx);
    DumpSpawnRefAll("WORLD-role", (void *)ref);
    if (entity)
      DumpRoleChain("WORLD-role", ctx, entity);
  }
}

// ---------------------------------------------------------------------------
//  四个 detour
//
//  四个 detour 一律原样转发：
//    游戏拿到的返回值必须和没挂钩时一模一样，探针才是"只观察"。
// ---------------------------------------------------------------------------

// sub_141D6CFD0，ctx 的生成虚方法 vt[232]。见上面 SPAWN_PROBE_RVA 那段说明。
//
// req 在调用之后 dump：
//     调用者 sub_141DCBD10 传进来的是 char v58[64]（一个出参栈缓冲区，
//     函数进去时里面是未初始化的栈残留），入口读到的只会是同一组
//     NaN/1e18 垃圾值，那些数据毫无意义。
// 所以调用前只记指针（留个对照），调用后 dump 真实内容。
static __int64 __fastcall SpawnDetour(void *ctx, void *req) {
  // 调用者 RVA，_ReturnAddress() 在函数体内恒定，入口取一次即可。
  //   用途有两处，日志记调用者；以及下面决定 req 该不该复制。
  unsigned char *imgBase = (unsigned char *)GetModuleHandleA(nullptr);
  unsigned long long callerRva =
      imgBase ? (unsigned long long)((unsigned char *)_ReturnAddress() - imgBase) : 0;

  static volatile LONG hits = 0;  // 只用来排"前 N 次 dump"的配额
  LONG n = 0;
  const bool dump = ProbeNextDump(&hits, &n);
  LONG seq0 = g_role_seq;  // 入口时的角色变化序号，用来判断"这次 spawn 带来了新角色"

  if (dump) {
    Log("[SPAWN] #%ld  ctx=%p  req=%p, before the call (req is an uninitialized buffer here)", n,
        ctx, req);
    LogCaller("SPAWN", _ReturnAddress());
  } else {
    ProbeQuotaTail("SPAWN", n);
  }

  __int64 ret = g_spawn_orig(ctx, req);

  if (dump) {
    Log("[SPAWN] #%ld, after the call", n);
    DumpResult("SPAWN-after", (void *)ret);  // 先看结果对象，这个才是有信息量的
    if (ctx)
      DumpCtx("SPAWN-after", ctx);
  }

  // 把游戏自己构造过的 req 复制一份留着，自造召唤要用它（字段表在下面）。
  //
  // 只复制 AI 路径的 req，判据是调用者 RVA，不是 req 的内容。
  //
  //   sub_141D6CFD0 就是 ctx 的虚方法 vt[232]（IDA 静态确认，不是猜的）：
  //     - 它只有 2 个数据 xref：0x143740BE0 和 0x14572C4E0
  //     - ctx 的 vtable 在 0x143740AF8，而 0x143740AF8 + 0xE8(232) == 0x143740BE0  对上了
  //   也就是说它不是"玩家的生成函数"，而是 ctx 自己的生成虚方法，两条路都在调它：
  //     - RVA 0x1DCC232，AI 批量生成 sub_141DCBD10 里的 ctx->vt[232](ctx, v58)
  //                        （IDA 里 v58 是 char v58[64] 栈缓冲区）我们要的是这条
  //     - RVA 0x1D6DB26，玩家出生 sub_141D6D9B0 内部（那个函数会先销毁玩家实体）
  //   两条路传进来的 req 内容不同（后者带 player 等玩家字段）。拿"玩家请求"去生成
  //   AI，很可能生成出玩家型的东西，必须只取 AI 那条。
  //
  //  req 的字段表（IDA 反编译 sub_141DCBD10 得到）
  //   调用点是 v43 = ctx->vt[232](ctx, v58)，而 v58 之前从未被写入，
  //   也就是说 req+0..63 是未初始化的栈残留（游戏自己也不管），真正有意义的是
  //   紧随其后的几个局部变量（它们正好落在 req+64 往后）：
  //     req+64 取 v59 = v55[2]：sub_141D6CFD0 拿它当 v4 做校验（sub_140F5CA10）。
  //                              校验不过就 return 0；但它 == 0 会整段跳过那段校验
  //                              （代码是 v4 = req+64; if (v4) {…}），不是"为 0 就退出"。
  //                              副本里这个指针是上一次生成时抓的，随时可能失效。
  //     req+72 取 v60 = 0：闸门 (A)：if (!ctx+552 && !req+72) return 0。
  //                              而且 req+72 非 0 会让 sub_140F4EF30 里那道闸门 (B)
  //                                整段跳过（if (v4) goto LABEL_25）；代价是游戏会自己
  //                                调 sub_140F4EB20(ctx,16,1) 给 ctx 置位。
  //     req+80 取 v61 = 0：被 sub_140F4EF30 包成 w60={vtable, req+80} 传下去
  //                              （sub_140F475F0(ctx, w60) 会读它）。
  //     req+88 取 v62 = -1：req+92 是它的高 32 位。== -1 才会
  //          ++*(u32*)(ctx+244)（在 LABEL_51，即造成功后才执行），
  //          这正是"生成真的发生了"的判据。
  //          同时它还被当 slot 索引：v15 = req+92; if (v15==-1) v15 =
  //                              ctx+244 & 0x7F
  //     req+96 取 v63 = 1
  // 全零不能替代照抄（req+92 不是 -1，ctx+244 不递增，连成功与否都看不出；
  //     而且 req+72 == 0 会让闸门 (A)/(B) 生效），必须照抄这一份。
  //   但"照抄"也不保证通过，闸门 (B) 里的 sub_140F475F0 看的是 ctx 自己的状态
  //      （*(u32*)(ctx+244) < 上限）。这就是 SummonWithTemplate 里那道具尝试梯子存在的理由
  //      （退到 ATT_RESET_ROT 把 ctx+244 清零，让闸门 (B) 放行）。
  if (req) {
    if (callerRva == 0x1DCC232ull) {
      // AI 路径的一次真实生成，这个 ctx 就是 AI 侧的 spawner。
      //   顺手记下来当载体，这是地面真值，永远压过 ScanPool 猜的。
      if (ctx)
        NoteCarrierAiSpawn((unsigned long long)(uintptr_t)ctx);
      if (ctx && g_aiCtxSrc != 2) {
        InterlockedExchange64(&g_aiCtx, (long long)(intptr_t)ctx);
        InterlockedExchange(&g_aiCtxSrc, 2);
        Log("[REQ] carrier ctx set to %p (source: AI path, ground truth)", ctx);
      }

      __try {
        memcpy(g_reqCopy, req, sizeof(g_reqCopy));
        if (g_reqCopyOk == 0) {
          InterlockedExchange(&g_reqCopyOk, 1);
          Log("[REQ] captured the AI path req (%u bytes, caller RVA 0x%llX), self-made summon uses "
              "it",
              (unsigned)sizeof(g_reqCopy), callerRva);
          Log("[REQ]   ctx=%p  req+64=%p  +72=%lld  +80=%p  +88=%lld(+92=%d)", ctx,
              *(void **)((unsigned char *)req + 64), *(long long *)((unsigned char *)req + 72),
              *(void **)((unsigned char *)req + 80), *(long long *)((unsigned char *)req + 88),
              *(int *)((unsigned char *)req + 92));
        }
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        // 复制中途崩了，缓冲区是半份的。处理体不清 g_reqCopyOk 的话，
        //   它只会被置真、从不重新校验，读方就会把半份 req 喂给游戏：+64 是游戏会
        //   拿去校验的指针，+72/+88/+92 驱动闸门和"生成了没"的信号，
        //   解引用一个陈旧指针，或者造出畸形实体，而且"生成了没"读的是另一个请求的值。
        //   （这次是刻意越读，调用方的缓冲只有 64 字节，我们拷 256，见字段表。）
        InterlockedExchange(&g_reqCopyOk, 0);
      }

      // （holder 底板的捕获不在这里：调用者收尾时已经把 *(ctx+840) 清成 0 了，
      //   这一步永远抓不到。要到 WorldInsertDetour 里抓，那里是 sub_141D6CFD0
      //   内部、ctx+840 一定有效。详见那边。）
    } else if (g_reqCopyOk == 0 && n <= 6) {
      // 只在最开始几次提示"还没抓到"，避免刷屏。若一直抓不到，说明 AI 路径的
      // 调用者 RVA 和上面写的不一致（换版本/走别的 vtable），照着这条日志改判据。
      Log("[REQ] caller RVA=0x%llX is not the AI path (0x1DCC232), req not copied yet", callerRva);
    }
  }

  // v21 必须在 if(dump) 外面读：
  //     前面 30 个样本很可能全是同一批敌人（角色没变），等真出现新角色时早就过了
  //     样本上限；放在 if(dump) 里就等于永远不触发。频率由 g_role_seq 控制
  //     （RoleWatch 每次认出"spawner 换了角色"就 +1，封顶 ROLE_DUMP_MAX 次之后不再加，
  //     那一句是功能、不受探针开关管），所以不会刷屏。
  //     这里 sub_141D6CFD0 已经跑完，entity 的 +600 一定就绪了，WORLD 里那次是早读。
  if (ctx && g_role_seq != seq0) {
    void *pe = (void *)(uintptr_t)(unsigned long long)AtomicReadL64(&g_pair_entity);
    void *pc = (void *)(uintptr_t)(unsigned long long)AtomicReadL64(&g_pair_ctx);
    Log("[SPAWN-after]   new role appeared, recomputing v21 from the entity WORLD stored (chain "
        "finished, only now is the read valid)");
    if (pe && pc == ctx)
      DumpRoleChain("SPAWN-after-v21", ctx, pe);
    else
      Log("[SPAWN-after]   (stored entity=%p ctx=%p does not match this one, skipping)", pe, pc);
  }
  return ret;
}

// sub_140F4EF30（造对象 + 登记进 ctx，两道闸门与那段身世见上面那张钩子表）。
//   闸门不是"召唤按了没反应"的常见原因：日志里 WORLD 经常被调到，说明两道都放行了。
//   真正卡住的那种是 holder 用了清零骨架（见 SummonWithTemplate 那段）。
//   现在尝试梯子（尤其 ATT_RESET_ROT）只是保底手段，不是主路径。
// 玩家实时位置从 *(ent+40) 现读（见 GetPlayerWorldXform）。sub_141D62790 那条路有三个问题：
//     - 它只在"把玩家放进世界"那一瞬开火，拿到的是出生点的快照，不是实时位置，
//       玩家走动后完全跟不上。
//     - 挂上它还会让 ATT_AT_PLAYER 那档的候选探测被短路，[POS] 一行都不会打，
//       等于把"候选全灭"这个判断整个遮住。
//     - 它的 a7+16 就是一份普通 LinearTransform（+16 vtable / +40..+87 变换 / +76 标记），
//       和 sub_1410878E0 输出的、以及 sub_140F4D2C0 里 *(msg+8) 拿到的都是同一种东西，
//       没有任何独家信息。
//
static __int64 __fastcall WorldInsertDetour(void *mgr, void *req) {
  static volatile LONG hits = 0;  // 只用来排"前 N 次 dump"的配额
  LONG n = 0;
  const bool dump = ProbeNextDump(&hits, &n);
  // 抓 holder 副本时要比的换关代次（发布那处用）。必须在读 ctx+840 之前取下来：
  //   要挡的正是"读完 ctx+840 之后、发布之前被换关清掉"这一段。
  const LONG genAtCapture = (LONG)AtomicReadL(&g_injectGen);

  if (dump) {
    Log("[WORLD] #%ld  mgr=%p  req=%p, before the call", n, mgr, req);
    LogCaller("WORLD", _ReturnAddress());
  } else {
    ProbeQuotaTail("WORLD", n);
  }

  __int64 ret = g_world_orig(mgr, req);

  // 把 (ctx, entity) 留给外层的 SPAWN detour，那时整条链才跑完，v21 才读得到。
  if (ret) {
    InterlockedExchange64(&g_pair_ctx, (long long)(intptr_t)mgr);
    InterlockedExchange64(&g_pair_entity, (long long)(intptr_t)ret);
  }

  // holder 底板必须在这里抓，不能在 SPAWN 的"调用后"抓，实测：
  //     SPAWN 的调用后 *(ctx+840) 已经是 0（调用者收尾时清掉了），那边一次都抓不到。
  //
  //     而 WORLD = sub_140F4EF30 是在 sub_141D6CFD0 内部被调的（日志实测调用者
  //     RVA = 0x1D6D052，正是 sub_141D6CFD0 里 call sub_140F4EF30 的下一条指令），
  //     那一刻 ctx+840 一定是有效的，游戏自己在同一个调用里后面还要读它。
  //
  //     来源分两种，抓的时候记下来（g_holderSrc）：
  //       2 = mgr 就是我们的载体 g_aiCtx，它的 holder 正是我们要的底板，最优先；
  //       1 = 别处的一次生成，先垫着：载体还没被 AI 路径生成过一次时（Ops 这类没有
  //           AI 生成事件的关卡），这是唯一能拿到一份真底板的机会，比用清零骨架强。
  //     来源 1 必须排掉玩家自己的 spawner：换关后 ResetInjectState 刚清空底板，
  //       而第一发 WORLD 常常就是玩家重生，拿它的 AISpawnData 当底板，
  //       退路那一支生成出来的单位名字与外观会更乱。
  //       TrySpawnDetour 在入口就把 playerPath 记上了，所以这里问得出来。
  //     还要排除"我们自己注入时设的假 holder"，那时 mgr 也是 g_aiCtx。
  if (mgr) {
    const unsigned long long mgrU = (unsigned long long)(uintptr_t)mgr;
    unsigned long long aiCtx = (unsigned long long)AtomicReadL64(&g_aiCtx);
    unsigned long long h = 0;
    LONG src = (aiCtx && mgrU == aiCtx) ? 2 : 1;
    if (src == 1 && (IsOwnSpawnPoint(mgrU) || IsRecentOwnSpawner(mgrU)))
      src = 0;  // 玩家自己的 spawner，不用
    const bool hRead = Rd64(mgrU + CTX_HOLDER_OFF, &h);

    // 判据是游戏自己的一次生成，读到的 ctx+840 正好是我们的假 holder。
    //   那说明它是在我们的注入窗口里跑的：这个 spawner 的模板槽还指着 g_fakeHolder，
    //   于是它会生成一个"名字取自我们注入的模板、身体和部件取自它自己的 spawner"的单位，
    //   既没有行为树、也不算任何一方的击杀。
    //   这一行出现就是实锤；只出现几次也正常（要正好撞上那个窗口）。
    //   分两种情形报，都要报出来：
    //     a) 窗口外读到假 holder：说明窗口关掉之后它还装在上面（还原失败，或者装到了别的 ctx）。
    //     b) 窗口内读到假 holder，但这次 WORLD 不是我们那次同步调用发出来的
    //        （判据是 g_inOurCall，不是"第几次"）：说明别人在这个窗口里生成。
    //        我们自己的梯子一次召唤最多调生成函数四档，所以"次数"分不出是谁。
    if (hRead && h == (unsigned long long)(uintptr_t)g_fakeHolder) {
      // 限流是因为还原失败之后这个 ctx 每次生成都会命中，不限流会刷屏
      //   （AI 刷怪频率下就是每秒数行，而 Log 每行都要拿锁 + fflush）。
      static volatile LONG fakeHolderLogs = 0;  // 和外层那个 dump 配额的 hits 是两回事
      if (InterlockedIncrement(&fakeHolderLogs) <= 20) {
        if (!AtomicReadL(&g_injecting)) {
          Log("[WORLD] !! a spawn outside our injection window read OUR fake holder (ctx=%p),"
              " it produces a mixed unit: name from our template, body and parts from this"
              " spawner",
              (void *)mgr);
        } else if (!AtomicReadL(&g_inOurCall)) {
          // 窗口开着、但不是在"我们那次同步调用"里，说明别人在这个窗口里生成。
          Log("[WORLD] !! a spawn inside our injection window but NOT from our own call"
              " (ctx=%p): someone else is generating while our fake holder is installed",
              (void *)mgr);
        }
      }
    }

    // 只要是真 holder 就抓（来源 2 = 我们的载体自己，最优先；1 = 别的 ctx，先垫着）。
    if (src && (g_holderSrc == 0 || (src == 2 && g_holderSrc != 2)) && hRead &&
        h >= MIN_VALID_ADDR && (h & 7) == 0 && h != (unsigned long long)(uintptr_t)g_fakeHolder) {
      bool hOk = false;
      __try {
        InterlockedIncrement(&g_holderSeq);  // 奇数：正在写，读者回避
        memcpy(g_holderCopy, (void *)h, sizeof(g_holderCopy));
        InterlockedIncrement(&g_holderSeq);  // 偶数：写完了
        hOk = true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        InterlockedIncrement(&g_holderSeq);  // 异常也要把奇数补回偶数
        // 写成一半的那份必须作废（连来源标记一起，理由见 ClearHolderCopy）。
        ClearHolderCopy();
      }
      // 代次没变才发布。换关时 Worker 的 ResetInjectState 会把这份副本作废（它指着上一关的
      //   对象），而这次抓取可能正跑在半途：作废之后又被这里无条件置回 1，
      //   下一关就会拿上一关的 holder 当注入底板，而那块内存可能已经释放。
      if (hOk && (LONG)AtomicReadL(&g_injectGen) == genAtCapture) {
        InterlockedExchange(&g_holderSrc, src);
        InterlockedExchange(&g_holderCopyOk, 1);
        // 顺手把它的 +48（= 这个 spawner 平时在造的角色模板）以及 +56/+64/+80/+88/+92
        // 都读出来，一看就知道抓的是不是真东西。
        char tpl[SUM_NAME_MAX] = {0};
        unsigned long long t21 = 0, p3 = 0;
        if (Rd64(h + 48, &t21) && t21 && Rd64(t21 + 24, &p3) && p3)
          ReadIdent(p3, tpl, sizeof(tpl));
        // 这几个字段走 Rd64/Rd32 读，不直接解引用：那两行 memcpy 虽然刚成功过，
        //   但"刚才能读"不等于"这一瞬间还能读"，而这里没有别的 SEH 兜着。
        unsigned long long h56 = 0, h64 = 0, h80 = 0;
        unsigned int h92 = 0, h88bits = 0;
        Rd64((unsigned long long)(uintptr_t)h + 56, &h56);
        Rd64((unsigned long long)(uintptr_t)h + 64, &h64);
        Rd64((unsigned long long)(uintptr_t)h + 80, &h80);
        Rd32((unsigned long long)(uintptr_t)h + 88, &h88bits);
        Rd32((unsigned long long)(uintptr_t)h + 92, &h92);
        Log("[HOLDER] captured holder base (inside WORLD, ctx+840 = %p, source=%s):"
            "+48=%p(%s) +56=%p +64=%p +80=%p +88=%.1f +92=%u",
            (void *)h, src == 2 ? "the AI carrier itself" : "another ctx", (void *)t21,
            tpl[0] ? tpl : "unreadable", (void *)h56, (void *)h64, (void *)h80,
            (double)*(float *)&h88bits, h92);
      } else if (hOk) {
        // 抓到了却因为换关被丢掉，这一支不能是静默的：不然"下一关的底板还是上一关那份"
        //   这条线在日志里没有落点，查起来只能靠猜。
        Log("[HOLDER] captured holder discarded: a level change reset the inject state while it"
            " was being copied (ctx+840 = %p, source=%s)",
            (void *)h, src == 2 ? "the AI carrier itself" : "another ctx");
      }
    }
  }

  if (dump) {
    Log("[WORLD] #%ld, after the call", n);
    DumpResult("WORLD-after", (void *)ret);
  }
  // 主过滤器里 mgr 就是 SPAWN 的 ctx，返回值就是 entity，两边都全。
  //     它不受 dump 样本预算限制：角色可能在我们用完 30 个样本之后才切换。
  RoleWatch(n, mgr, (void *)ret);
  return ret;
}

// sub_141D6D9B0(player, ctx, a3)，把"队伍判定的两个输入"直接打出来。
//     ctx+576 应该等于 player+88（队号）；ctx+552 是个必须非 0 的 bool。
//     返回值非 0 = 这次真的生成了。
static void DumpTrySpawn(const char *tag, void *player, void *ctx, int a3, void *ret) {
  __try {
    Log("[%s]   player=%p   ctx=%p   a3=%d   ret=%p", tag, player, ctx, a3, ret);
    if (player)
      Log("[%s]   player+88(team) = %u", tag, *(unsigned int *)((unsigned char *)player + 88));

    // 通向"自造"的门直接 dump 这个 ctx 的模板持有者 ctx+840 及其 +48（= v21）。
    //
    //   在这里做是因为 TRYSPAWN 的 detour 里游戏自己把 ctx 送进来了，用不着扫描
    //   去找 spawner。而 ctx+840、+48 那条链就是上面反复提到的"当前模板"来源。
    //   留着它是因为查模板或蓝图取值的问题时，这里是唯一能当场看到持有者结构的地方。
    {
      unsigned long long tail = 0, v21 = 0;
      if (Rd64((unsigned long long)ctx + CTX_HOLDER_OFF, &tail)) {
        Log("[%s]   [holder] ctx+840 = %p", tag, (void *)tail);
        if (tail) {
          if (Rd64(tail + 48, &v21))
            Log("[%s]   [holder] *(it+48) = v21 = %p", tag, (void *)v21);
          DumpHolderShort(tag, tail);
        }
      }
      if (v21) {
        Log("[%s]   [holder] first 0x60 bytes of v21:", tag);
        for (int k = 0; k < 12; ++k) {
          unsigned long long q = 0;
          if (!Rd64(v21 + 8ull * k, &q))
            break;
          Log("[%s]   [holder]   v21[%2d] = %016llX", tag, k, q);
        }
      }
      // 选注入载体时必须排除这个 ctx：拿它当载体会让玩家从世界里消失。
      //
      //   实测 TRYSPAWN 传进来的这个 ctx 是玩家自己出生用的
      //   spawner。对它调 sub_141D6D9B0 = 让玩家销毁旧实体、重新出生，
      //   旧的没了、新的没生出来，玩家从世界里消失、无法操作（游戏没崩，逻辑照跑）。
      //
      //   而且不能只靠"地址 != ctx0"来排除：玩家每次重生 ctx0 都会换，
      //      上一次的玩家 spawner 会留在链表里冒充普通 spawner（实测 ctx0=0xCF2DFAA0
      //      十几秒后就变成 0xCF2DF6C0，前者随即被当成普通 spawner 选中当载体，
      //      玩家又消失，恶性循环）。
      // 每次选载体时当场重新调 sub_141D5BE30(player, 0) 问一次，
      //      看它现在返回的是不是这个，见 ScanPool 里的 isMine 那段。
    }

    if (ctx) {
      Log("[%s]   ctx+552 = %u      ctx+576(team) = %u", tag,
          (unsigned)(*(unsigned char *)((unsigned char *)ctx + CTX_GATE_OFF)),
          *(unsigned int *)((unsigned char *)ctx + CTX_TEAM_OFF));
      // 这一行就是"我们要调的这个 ctx 会生成哪个角色"的直接答案：
      //     ctx+48 = spawnRef。spawnRef+24 是 spawner 自己的 ID，不是角色，
      //     角色在 v21[3]（Template 路径），不是这个 +24。
      void *ref = *(void **)((unsigned char *)ctx + 48);
      Log("[%s]   ctx+48(spawnRef)=%p   spawnRef+24(spawner ID, not a role)=0x%016llX", tag, ref,
          ref ? *(unsigned __int64 *)((unsigned char *)ref + 24) : 0);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    Log("[%s]   !! failed to read player/ctx", tag);
  }
}

// sub_141DCBD10，批量调度器。抓它的 a1（疑似全局单例）。
//
// 只在"真要干活"时才详 dump：反编译里它进门先看 if (count && *(_DWORD *)(a1 + 80))，
//   a1+80 非 0 才是"真要干活"。为什么必须这么筛，见下面配额那处。
static __int64 __fastcall DispatchDetour(void *a1) {
  unsigned int workFlag = 0;
  __try {
    if (a1)
      workFlag = *(unsigned int *)((unsigned char *)a1 + 80);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    workFlag = 0;
  }

  // 只有"真干活"的调用才占用样本配额。
  //    每个调用都计数的话，空转调用会把 30 个名额吃光，
  //    等有意思的调用到来时早就过了上限，这轮又白跑。
  bool dump = false;
  LONG n = 0;
  static volatile LONG hits = 0;  // 只用来排"前 N 次 dump"的配额
  if (workFlag) {
    dump = ProbeNextDump(&hits, &n);
    if (dump) {
      Log("[DISPATCH] #%ld  a1=%p, before the call (a1+80=%u non-zero, real work this time)", n, a1,
          workFlag);
      LogCaller("DISPATCH", _ReturnAddress());
    } else {
      ProbeQuotaTail("DISPATCH", n);
    }
  }

  __int64 ret = g_dispatch_orig(a1);

  if (dump)
    Log("[DISPATCH] #%ld, returned %p", n, (void *)ret);

  // 在这里执行排队的召唤，这是游戏线程，也是 AI 生成那条路。
  //   必须在这里是因为引擎在生成链上会读 [TLS 块 + 0x548]（sub_14106E100），
  //   而那条链在我们自己的按键线程上没有 TLS 上下文，返回 NULL，调用方没判空，
  //   读 [0+0x18] 访问违例（那次崩溃的现场分析在 QueueSummon 那处）。
  //   放在原调用之后，让调度器这一 tick 自己的活先干完，再动 ctx。
  if (InterlockedExchange(&g_pendingSummon, 0)) {
    // 载荷用一次原子交换取走，取哪次按键和清邮箱是同一个动作，连续两次消费
    //   不会把同一份载荷执行两遍（第二次取到 0，直接跳过）。
    unsigned long long queued = (unsigned long long)InterlockedExchange64(&g_queuedV21, 0);
    // 序列号紧接着载荷一起取：QueueSummon 是先写序列号再写载荷的，所以这里读到的
    //   就是这一份载荷所属的那一次按键。这一发的结果（成功或失败）要用它，
    //   而按键线程在它跑着的这几十毫秒里可能已经按下一次新的了。
    // 载荷与序列号是两个量，取载荷与读序列号之间还隔着两条指令，理论上能撞进一次新按键
    //   （读到"上一发的载荷配这一发的序列号"）。后果只是那一发的失败会消耗新一次按键的
    //   一格重试预算，窗口两条指令，不再为它上 16 字节原子交换。
    // 同一个窗口对 g_queuedT 也成立（DoSummonSpawner 拿它算"这一发排了多久"）：
    //   撞进一次新按键时读到的是新那一次的时刻，于是一条本该过期的载荷会被执行一次。
    //   后果只是"突然冒出一个早先按下的角色"。换读序也修不掉：先读时刻再取载荷的话，
    //   反过来会把一份刚按下的新请求误判成过期丢掉，那比现在这个更难查。
    const LONG seq = (LONG)AtomicReadL(&g_queuedSeq);
    if (queued) {
      Log("[SUMMON] running the injection on the game thread (triggered by a DISPATCH tick)");
      DoSummonSpawner(queued, seq);
    } else {
      // 唤醒标志为真、载荷却是 0。只有一种来源：换关清理（ResetInjectState）正好插在
      //   上面那两步之间。它清标志时读回 0，于是自己判定成"没丢东西"、不报；
      //   这里再看一次载荷也已经没了。不补这一句，那一发就既不执行也没有任何日志。
      // 交给重试去补：算它失败一次，跟其它失败口一样从预算里出。
      Log("[SUMMON] the queued request was cleared by a level change right as this tick took it,"
          " counting it as a failed attempt so the retry covers it");
      SummonAttemptFailed(seq, "FAIL request dropped at a level change");
    }
  }
  return ret;
}

// sub_141D6D9B0，队伍判定 + 生成。这个同时给出 player 和 ctx 两个指针。
static __int64 __fastcall TrySpawnDetour(void *player, void *ctx, char a3) {
  // 走过这条路的 ctx 记进载体特征表（挑载体时要避开它，见 IsOwnSpawnPoint）。
  //   放在入口、不受下面那个 dump 配额影响：配额只管打不打日志，这个必须每次都记。
  NoteCarrierPlayerPath((unsigned long long)(uintptr_t)ctx);

  static volatile LONG hits = 0;  // 只用来排"前 N 次 dump"的配额
  LONG n = 0;
  const bool dump = ProbeNextDump(&hits, &n);
  if (dump) {
    Log("[TRYSPAWN] #%ld, before the call", n);
    LogCaller("TRYSPAWN", _ReturnAddress());
  } else {
    ProbeQuotaTail("TRYSPAWN", n);
  }

  __int64 ret = g_tryspawn_orig(player, ctx, a3);

  if (dump) {
    DumpTrySpawn("TRYSPAWN-after", player, ctx, (int)a3, (void *)ret);
    // TRYSPAWN 只响个位数次，这几次必须把 spawnRef 看全，别省日志。
    if (ctx) {
      void *ref = nullptr;
      if (Rd64((unsigned long long)ctx + 48, (unsigned long long *)&ref) && ref)
        DumpSpawnRefAll("TRYSPAWN", ref);
    }
  }
  return ret;
}

// MinHook 的分工与局限，以及"这两种错误算暂时性"这条判断的依据。
//
// 分工：它负责指令长度解码和跳板重定位，所以以后想改挂别的函数，只要换 RVA 就行，
//   不必再逐个函数人工核对序言。
// 局限：它不解决"写指令时的竞态"，x64 上它写的同样是 14 字节绝对跳转
//    (FF 25 00000000 + imm64)，而且同样不挂起其它线程，
//    "别的线程正好执行到函数头、看到撕了一半的指令"这个窗口两边一样存在。
// MinHook 给跳板找地方的办法是：在目标 ±1GB 窗口里按 64KB 粒度扫一遍，找一块空闲页
//   （buffer.c 的 GetMemoryBlock，MAX_MEMORY_RANGE = 0x40000000），扫不到就报
//   MH_ERROR_MEMORY_ALLOC。改页保护没做成则是 MH_ERROR_MEMORY_PROTECT。
//   这两种都是"这次的资源状况"，不是"这个钩子永远装不上"：窗口里的空闲页是游戏自己在
//   分配释放的，这一轮没有不等于下一轮没有。
static bool MinHookErrorIsTransient(MH_STATUS st) {
  return st == MH_ERROR_MEMORY_ALLOC || st == MH_ERROR_MEMORY_PROTECT;
}

// 分配失败时把现场量一次（只打一次）：目标 ±1GB 窗口里还有多少空闲地址、最大的一块在哪、
//   系统提交量还剩多少。光看"分配失败"分辨不出是窗口里真没地方了还是系统在缺提交量。
static void LogTrampolineWindow(void *target) {
  static volatile LONG shown = 0;
  if (InterlockedIncrement(&shown) > 1)
    return;

  const unsigned long long kRange = 0x40000000ull;  // 和 MinHook 的 MAX_MEMORY_RANGE 一致
  const unsigned long long t = (unsigned long long)(uintptr_t)target;
  const unsigned long long lo = (t > kRange) ? (t - kRange) : 0;
  const unsigned long long hi = t + kRange;
  unsigned long long cur = lo & ~0xFFFFull;
  unsigned long long freeTotal = 0, bestRun = 0, bestAt = 0;
  int freeRegions = 0;
  while (cur < hi) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)(uintptr_t)cur, &mbi, sizeof(mbi)))
      break;
    unsigned long long end =
        (unsigned long long)(uintptr_t)mbi.BaseAddress + (unsigned long long)mbi.RegionSize;
    if (end <= cur)
      break;  // 坏数据，别死循环
    if (end > hi)
      end = hi;
    if (mbi.State == MEM_FREE) {
      const unsigned long long run = end - cur;
      ++freeRegions;
      freeTotal += run;
      if (run > bestRun) {
        bestRun = run;
        bestAt = cur;
      }
    }
    cur = end;
  }
  MEMORYSTATUSEX ms = {0};
  ms.dwLength = sizeof(ms);
  GlobalMemoryStatusEx(&ms);
  Log("[PROBE] no free page for a trampoline near %p: in its +/-1GB window there are %d free"
      " region(s), %llu MB in total, largest %llu MB at %p; system commit headroom = %llu MB of "
      "%llu"
      " MB",
      target, freeRegions, freeTotal >> 20, bestRun >> 20, (void *)(uintptr_t)bestAt,
      (unsigned long long)(ms.ullAvailPageFile >> 20),
      (unsigned long long)(ms.ullTotalPageFile >> 20));
}

// 暂时性失败的日志要限流：窗口真满了的话，每轮 4 个钩子各一条，
//   按 5 秒退避算，5 分钟就是两百多条。
static void LogMinHookTransient(const char *tag, void *target, MH_STATUS st, const char *where) {
  InterlockedIncrement(&g_hookTransientN);
  static volatile LONG logged = 0;
  if (InterlockedIncrement(&logged) <= 8) {
    Log("[%s] !! %s failed with a transient MinHook error (%s), retrying later", tag, where,
        MH_StatusToString(st));
    LogTrampolineWindow(target);
  }
}

// 返回：1 = 装好了；0 = 指纹还没对上（等解密）；-1 = 确定性硬错误（再等也不会好）；
//   -2 = 暂时性错误（分配跳板/改页保护），下一轮再试。
static int InstallOneHook(void *target, const unsigned char *fingerprint, int fpLen, LPVOID detour,
                          void **ppOrigGlobal, const char *tag) {
  // 由于加壳，磁盘上的代码是加密的，运行到某一步才解密。
  // 必须等目标处真的是预期字节，否则 MinHook 会对着还没解密的乱码做指令解码，
  // 搭出来的跳板是垃圾，一调用就崩。
  // 包 SEH 是因为万一某个还没解密的页是不可访问的（而不是"可读但内容不对"），
  //   这一次 memcmp 就是访问违例，而这里是探针线程、外面没有别的东西接得住，
  //   进程当场就没了。读不到按"还没就绪"处理，等下一轮。
  bool notReady = false;
  __try {
    notReady = (memcmp(target, fingerprint, fpLen) != 0);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    notReady = true;
  }
  if (notReady)
    return 0;

  LPVOID orig = nullptr;
  MH_STATUS st = MH_CreateHook(target, detour, &orig);
  if (st != MH_OK) {
    if (MinHookErrorIsTransient(st)) {
      LogMinHookTransient(tag, target, st, "MH_CreateHook");
      return -2;
    }
    Log("[%s] !! MH_CreateHook failed: %s", tag, MH_StatusToString(st));
    return -1;
  }
  if (!orig) {
    // MinHook 说成功却没给跳板，装上就等于让 detour 调空指针，宁可不装
    Log("[%s] !! MH_CreateHook succeeded but the trampoline is null, giving up", tag);
    MH_RemoveHook(target);
    return -1;
  }

  // 顺序铁律，先把跳板发布到全局，再 EnableHook，反了就是崩溃级竞态。
  //   MH_EnableHook 一返回，钩子就已经生效，游戏随时可能调到目标函数；
  //   而 detour 里对跳板是裸解引用（g_spawn_orig(ctx, req) 之类），
  //   那一刻它若还是 nullptr，call [0]。detour 自己没套 SEH，游戏线程访问违例、
  //   当场闪退，日志恰好停在"挂钩成功"那一行之前。
  //   所以顺序必须是"先发布跳板、再 EnableHook"：反过来（先启用、再 Log、再发布）
  //   中间隔着一次 fputs + fflush + 抢锁（0.1~1ms），而这些钩子都在每秒几十次的
  //   高频路径上，每次装钩子都有可观的撞上概率。
  *ppOrigGlobal = orig;

  st = MH_EnableHook(target);
  if (st != MH_OK) {
    // 没启用成功，先撤销发布，免得 detour 以为跳板可用。
    *ppOrigGlobal = nullptr;
    MH_RemoveHook(target);
    if (MinHookErrorIsTransient(st)) {
      LogMinHookTransient(tag, target, st, "MH_EnableHook");
      return -2;  // 钩子已撤掉，下一轮照常重来
    }
    Log("[%s] !! MH_EnableHook failed: %s", tag, MH_StatusToString(st));
    return -1;
  }

  Log("[%s] hooked: %p   trampoline=%p", tag, target, orig);
  return 1;
}

static bool InstallProbes() {
  unsigned char *base = (unsigned char *)GetModuleHandleA(nullptr);
  if (!base)
    return false;

  // 表驱动，以后要加挂钩点，往这张表加一行即可
  struct Spec {
    unsigned int rva;
    const unsigned char *fp;
    int fpLen;
    LPVOID detour;
    void **ppOrig;
    const char *tag;
  };
  // 数组不要写死长度。写死成 [5] 而表里只剩 4 项时，第 5 项会被零初始化，
  //   而按 [5] 算出来的条数照样是 5：那一项现在会被下面"空项按硬错误处理"那道拦下（不会 AV），
  //   但那是靠另一道检查兜住的，写死了就得永远记住两处必须一致。
  //   用不定长数组 + sizeof，增删钩子都自动跟随，不可能再失配。
  static const Spec specs[] = {
      {SPAWN_PROBE_RVA, kProbePrologue, SPAWN_PROBE_GATE_LEN, (LPVOID)&SpawnDetour,
       (void **)&g_spawn_orig, "SPAWN"},
      {WORLD_INSERT_RVA, kWorldPrologue, WORLD_GATE_LEN, (LPVOID)&WorldInsertDetour,
       (void **)&g_world_orig, "WORLD"},
      {DISPATCH_RVA, kDispatchPrologue, DISPATCH_GATE_LEN, (LPVOID)&DispatchDetour,
       (void **)&g_dispatch_orig, "DISPATCH"},
      {TRYSPAWN_RVA, kTrySpawnPrologue, TRYSPAWN_GATE_LEN, (LPVOID)&TrySpawnDetour,
       (void **)&g_tryspawn_orig, "TRYSPAWN"},
  };
  const int kCount = (int)(sizeof(specs) / sizeof(specs[0]));

  bool hardFail = false;
  int done = 0, need = 0;
  for (int i = 0; i < kCount; ++i) {
    if (!specs[i].ppOrig || !specs[i].detour) {
      // 表里出现空项是改表时的手滑，不是运行时状况，所以当硬错误处理：
      //   跳过它的话那条会被连带从 need 里剔除，"必需钩子"静默降级成"不要求"，
      //   done >= need 照样成立、日志照样报"全部装好"，而那条根本没装。
      Log("[PROBE] !! spec %d has a null pointer (table typo), treating it as a hard error", i);
      hardFail = true;
      continue;
    }
    ++need;
    if (*specs[i].ppOrig) {  // 已经装好了，别重复 CreateHook
      ++done;
      continue;
    }
    int r = InstallOneHook(base + specs[i].rva, specs[i].fp, specs[i].fpLen, specs[i].detour,
                           specs[i].ppOrig, specs[i].tag);
    if (r == 1) {
      ++done;  // 跳板已在 InstallOneHook 里发布过（必须先于 EnableHook）
    } else if (r == -2) {
      // 暂时性错误（分配不到跳板内存 / 改页保护没做成）：不算硬错误，下一轮再试。
      //   这一支不能并进下面的 r < 0：并进去就是"分配失败一次、这一局再也不装钩子"。
    } else if (r < 0) {
      hardFail = true;
    } else {
      // r == 0 是指纹还没对上。这一条本身是正常的（等解密），但"还没解密"和
      //   "指纹常量写错了"在日志上完全一样，而后者会让这里白跑满 600 轮才打超时。
      //   报一次实测字节，两种情况的区别就一眼可见。限流，别刷屏。
      static volatile LONG mismatchLogged = 0;
      if (InterlockedIncrement(&mismatchLogged) <= 6) {
        unsigned long long got = 0;
        Rd64((unsigned long long)(uintptr_t)(base + specs[i].rva), &got);
        Log("[PROBE] %s at %p does not match its fingerprint yet; first 8 bytes = %016llX"
            " (want %02X %02X %02X %02X ...)",
            specs[i].tag, base + specs[i].rva, got, specs[i].fp[0], specs[i].fp[1], specs[i].fp[2],
            specs[i].fp[3]);
      }
    }
  }

  // 任一硬失败，放弃重试（和"还没解密"区分开，否则会刷 600 条错误日志）
  if (hardFail) {
    InterlockedExchange(&g_probe_abort, 1);
    // 打清楚剩下几个还会不会装，装了哪些、缺哪些，一看就知道。
    //   不打的话，唯一证据是 InstallOneHook 里那一行，而它可能被日志还没开的竞态吃掉，
    //   于是"钩子安装失败"可以是零日志的。
    Log("[PROBE] !! giving up (a hook reported a hard error): %d of %d installed so far"
        " (SPAWN=%d WORLD=%d DISPATCH=%d TRYSPAWN=%d)",
        done, need, g_spawn_orig ? 1 : 0, g_world_orig ? 1 : 0, g_dispatch_orig ? 1 : 0,
        g_tryspawn_orig ? 1 : 0);
    return false;
  }

  if (done >= need) {
    Log("[PROBE] all %d hooks installed", need);
    return true;
  }
  return false;
}

static DWORD WINAPI SpawnProbeThread(LPVOID) {
  // 等日志文件真的开好再打第一条。DllMain 里是先起 Worker、再起这个线程，
  //   但 CreateThread 的先后不构成任何顺序保证：抢在 fopen 前面的话，
  //   "等待解密""挂钩成功"甚至"挂钩失败"这几行会被静默丢掉（Log 在文件没开时直接跳过），
  //   而"钩子到底装上没装上"全靠它们。最多等 2 秒，等不到也照常往下走。
  for (int i = 0; i < 200 && !g_log; ++i)
    Sleep(10);
  // 绝不能因为 g_probe_enabled==0 就 return，那会把召唤功能整个关掉：
  //    SPAWN 是载体 ctx + req 的唯一来源，DISPATCH 是注入的执行点（见 DispatchDetour），
  //    WORLD 是 holder 底板的兜底来源，TRYSPAWN 是 playerPath 的唯一来源（见 HooksReady）。
  //    少装一个，g_spawn_orig 之类的跳板就是空，或者那条判据静默失效，按 F5/F11 要么没反应、
  //    要么埋一个"可能把玩家弄没"的状态。

  MH_STATUS st = MH_Initialize();
  if (st != MH_OK && st != MH_ERROR_ALREADY_INITIALIZED) {
    Log("[PROBE] !! MH_Initialize failed: %s", MH_StatusToString(st));
    return 0;
  }

  Log("[PROBE] waiting for the target functions to decrypt... (SPAWN 0x%X / WORLD 0x%X / DISPATCH "
      "0x%X / TRYSPAWN 0x%X)",
      SPAWN_PROBE_RVA, WORLD_INSERT_RVA, DISPATCH_RVA, TRYSPAWN_RVA);
  // 最多等 5 分钟。每轮的间隔是变的（见下），所以按时间收口。
  const DWORD deadline = GetTickCount() + 5 * 60 * 1000;
  while ((LONG)(deadline - GetTickCount()) > 0) {
    const LONG transBefore = (LONG)AtomicReadL(&g_hookTransientN);
    if (InstallProbes())
      return 0;
    if (g_probe_abort)  // 确定性硬错误：MinHook 那边再等也不会好
      return 0;
    // 暂时性失败要退避：MinHook 找空位是把目标 ±1GB 窗口整段扫一遍（每 64KB 一次
    //   VirtualQuery），四个钩子一轮十万次上下的系统调用，按 500ms 的节奏重试等于自己
    //   烧掉小半个核。这种失败 5 秒一轮，其它等待仍然是 500ms 一轮。
    const bool transient = (LONG)AtomicReadL(&g_hookTransientN) != transBefore;
    Sleep(transient ? 5000 : 500);
  }
  // 超时也要说清装上了哪些，4 个钩子各自独立，
  // 只报"仍未解密"会让人以为全盘失败，而别的钩子可能早就装好、一直在打日志了。
  Log("[PROBE] !! wait timed out: SPAWN=%d WORLD=%d DISPATCH=%d TRYSPAWN=%d (1 = installed);"
      " %ld transient hook error(s) seen in total",
      g_spawn_orig ? 1 : 0, g_world_orig ? 1 : 0, g_dispatch_orig ? 1 : 0, g_tryspawn_orig ? 1 : 0,
      (LONG)AtomicReadL(&g_hookTransientN));
  return 0;
}

// ===========================================================================
//  启动
// ===========================================================================
static std::string DirOfSelf(HMODULE self) {
  char p[MAX_PATH] = {0};
  DWORD n = GetModuleFileNameA(self, p, MAX_PATH);
  // n == MAX_PATH 表示被截断了（返回值不保证带终止符）。截断的话算出来的"目录"是错的，
  //   日志会被悄悄写到别处去，与其那样，不如退回"."，至少行为可预期。
  if (n == 0 || n >= MAX_PATH)
    return std::string(".");
  std::string s = p;
  size_t k = s.find_last_of("\\/");
  return (k == std::string::npos) ? std::string(".") : s.substr(0, k);
}

static DWORD WINAPI Worker(LPVOID self) {
  LogInit();
  g_dir = DirOfSelf((HMODULE)self);
  std::string lp = g_dir + "\\summonai_log.txt";
  // 日志是 "w" 模式，一次运行一份，直接覆盖上一份（不需要归档成带时间戳的副本）。
  g_log = fopen(lp.c_str(), "w");

  Log("=== BFN SummonAI (self-made summon route) ===");
  Log("dir = %s", g_dir.c_str());
  Log("four columns: 1 ally role, 2 ally tier, 3 enemy role, 4 enemy tier (role columns: playable"
      " characters first in roster order, then the rest in display-name order; tier columns in hp"
      " order)");
  Log("Keys: ally F1/F2 role  F3/F4 tier  F5 summon   |   enemy F7/F8 role  F9/F10 tier  F11 "
      "summon");
  Log("(F6 = show/hide HUD; F12 is reserved for the Steam screenshot;"
      " team id is auto-read every 100ms)");
  Log("[CFG] dump log = %s (hardcoded, at most %d dumps)", g_probe_enabled ? "on" : "off",
      g_probe_max_dumps);

  RefreshTeam("startup", false);

  while (g_running) {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    // 100ms 自动重读队号，换队/进新局后要尽快跟上，不能等好几秒。
    //   成本很低，GetManager + 扫玩家数组 ≈ 100 次 VirtualQuery，量级 0.1ms。
    if ((DWORD)(now - last) >= 100) {
      last = now;
      RefreshTeam("periodic", true);
    }
    // 主动扫描，每 2 秒认一次 spawner。
    //     入口是游戏自己的 sub_141D5BE30(player, 0)（拿玩家 ctx），再顺链表走一圈。
    //     关卡里新增 spawner 时会自动列出新角色，不用打一局关。
    //     （防刷屏靠的是"只在玩家 ctx 变化时详打" + SCAN_DIAG_ONCE；
    //       ScanPool 自己不做去重，走两个方向会对同一个 spawner 计数两次。）
    static DWORD lastScan = 0;
    if (InterlockedExchange(&g_wantRescan, 0)) {
      Log("[TPL] Worker got a background rescan request, scanning now (game thread unaffected)");
      ScanTemplates(false);  // 同一关内重扫：不清列表
    }
    // 周期重扫（每 30 秒一次，只在关卡里做）。
    //
    //   ScanTemplates 枚举的是当前已加载的 Template 实例，而 BFN 按关卡
    //   流式加载资产，换关那一刻扫一次，抓不到"进关之后才加载进来"的模板。
    //   代价是一次全扫约 0.6~0.8 秒（扫 650MB 已提交内存）。30 秒一次 = 约 2.3% 单核，
    //   10 秒一次是 7%，再密不值当：这一扫唯一的收益是"进关之后才加载进来的模板早一点
    //   被列出来"，晚 30 秒出现对实际操作没有影响。
    //   全在 Worker 上跑，游戏线程不受影响；而且同一关内重扫不清召唤列表、写的是临时表
    //   （见 s_scanTpl 的说明），所以 HUD 不会闪、召唤也不会中途失效。
    //   "在不在关卡里"要真去问一次。不能用 g_team != 0 代替：g_team 一旦读到队伍号
    //   就再也不会清 0，那个条件等于恒真，离开关卡（大厅/结算/加载中）之后
    //   每 30 秒照样全扫 650MB，还往日志里灌 400 行。
    //   这里 30 秒才问一次，不是每 tick，开销无所谓；FindLocalPlayer 内部全走
    //   SEH 包装的读，和 ScanPool 每 2 秒已经在做的是同一件事。
    static DWORD lastTplScan = 0;
    if (lastTplScan == 0) {
      lastTplScan = now;  // 首轮不立刻扫（启动时那次已经扫过了）
    } else if ((DWORD)(now - lastTplScan) >= 30000) {
      lastTplScan = now;
      if (FindLocalPlayer(GetManager(), nullptr)) {
        Log("[TPL] periodic rescan (every 30s): catch templates streamed in after level entry");
        ScanTemplates(false);
      }
    }
    // 补建召唤列表（见 g_listWant）：被并发闸门挡掉、或当时队号还没读到，在这里补。
    //    本循环每 50ms 一轮，最坏 50ms 后就一致了。
    if (InterlockedExchange(&g_listWant, 0)) {
      Log("[LIST] Worker rebuilding the summon list (the last build did not happen)");
      BuildSummonLists();
    }
    // 主动看一眼手上的载体还在不在（每 50ms 一次）。
    //
    //   这不是"失败后重试"，是主动维护一个必须始终有效的东西：spawner 会被游戏停用
    //   （ctx+552 变 0、节点被摘出链表，见 ValidCarrier 的说明）。不主动看的话，
    //   要等按下去才发现，那一次就必然白按。现在它被停用后最多 50ms 就被换掉。
    //
    //   这条 20Hz 的路径上只读一个字段：ctx+552 是唯一被证实过的失效方式
    //   （日志里那个载体 552 从 1 变 0 之后就再没好过）。
    {
      unsigned long long cur = (unsigned long long)AtomicReadL64(&g_aiCtx);
      // 只看一件事，游戏把它停用了没有（ctx+552 变 0）。
      //
      //   这条 20Hz 的路径刻意不做完整身份检查（连"是不是玩家自己的"都不查）：
      //   在这里把载体删掉之后不一定还有下一个，误判的代价是整局没有载体
      //   （实测在 "SafeFindCtx 返回 0" 的模式里，连着 4 次召唤全是 "no usable AI carrier ctx"）。
      //   完整检查由 ValidCarrier 在真正用它之前当场做：那一步判错只是这一次不用它，
      //   下一次还能重挑，拦在这一层不划算。那一层在"问不出玩家自己的 spawner"时是判否的，
      //   于是这条松口径换来的是"整局没有载体"也可能发生：两处是有意不同，别统成一个。
      const bool bad = cur && !SpawnerGateOn(cur);
      if (bad) {
        // 必须 CAS，不能"读完就无条件清"：读到 cur 之后、清之前隔着一整个时间片
        //   （Rd32 可能进 SEH，Worker 也随时被抢占），这段时间里游戏线程的 AI 路径
        //   可能刚把新的载体写进 g_aiCtx。无条件清会把它连同 src=2 的"实测"标记
        //   一起抹掉，之后最多 2 秒里按键全是 FAIL no carrier。
        //   CAS 失败说明值已经换人，那就什么都不用做，下一轮再体检。
        if (DropCarrierIfCurrent(cur)) {
          Log("[SCAN] carrier %p was disabled by the game (ctx+552 became 0), dropping it,"
              " repicking now",
              (void *)cur);
          // 把"下一次周期扫描"的计时清零，下面那次判断立刻为真、这一轮就重挑。
          //   不这么做的话重挑要等最多 2 秒，这 2 秒里按下去还是白按。
          lastScan = 0;
        }
      }
      // 游戏线程那边刚丢掉一个载体（阶梯四档全撞崩），同样立刻重挑
      if (InterlockedExchange(&g_repickNow, 0))
        lastScan = 0;
      // 召唤重试到点了就重发一次，等同再按一下召唤键。
      //   at 用 Wrap 安全的差值比较：GetTickCount 会在 49.7 天回绕。
      //   侧别与序列号在这里一并快照下来交给 QueueSummon：
      //     它们和待办是三个量，等 QueueSummon 自己去读的话，读到的可能已经是新按下的那一次。
      //   待办用 CAS 清：这期间游戏线程可能为另一发失败排了一个新的时刻，无条件写会把它抹掉。
      {
        const LONG at = (LONG)AtomicReadL(&g_retryAt);
        if (at && (LONG)(now - (DWORD)at) >= 0) {
          const LONG seq = (LONG)AtomicReadL(&g_retrySeq);
          const int side = (int)AtomicReadL(&g_retrySide);
          if (InterlockedCompareExchange(&g_retryAt, 0, at) == at)
            QueueSummon(side, true, seq);
        }
      }
    }
    if ((DWORD)(now - lastScan) >= 2000) {
      lastScan = now;
      ScanPool();
    }
    Sleep(50);
  }
  return 0;
}

BOOL APIENTRY DllMain(HMODULE self, DWORD reason, LPVOID) {
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(self);
    CreateThread(nullptr, 0, Worker, (LPVOID)self, 0, nullptr);
    CreateThread(nullptr, 0, KeyThread, nullptr, 0, nullptr);
    CreateThread(nullptr, 0, HudThread, nullptr, 0, nullptr);
    // 生成探针必须在 Worker 之后起，它依赖 g_dir / 日志 / 配置都已就绪
    CreateThread(nullptr, 0, SpawnProbeThread, nullptr, 0, nullptr);
  }
  return TRUE;
}
