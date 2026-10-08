/* xau_bridge - the C ABI that MetaTrader 5 imports.
 *
 * This is the only surface where our code turns into real orders, so it is
 * built to be boring and hard to misuse:
 *
 *   - Pure C. No C++ types cross the boundary. MQL5 marshals structs by layout,
 *     and a std::string or a vtable pointer in a struct is undefined behaviour
 *     dressed as a field.
 *   - No exceptions escape. Every entry point is noexcept in effect; a throw
 *     that unwinds into MQL5 terminates the terminal.
 *   - Explicit sizes and a version. MQL5 cannot see our headers, so struct
 *     layout is a contract enforced only by agreement. A version mismatch must
 *     be detected and refused, not discovered through corrupted fields.
 *   - Layout that cannot depend on packing. MQL5 packs structs at 1 byte by
 *     default; C aligns a double to 8. Every struct below therefore orders its
 *     fields 8-byte, then 4-byte, then byte arrays, and pads its size to a
 *     multiple of 8, so the layout is the same under either rule. bridge.cpp
 *     pins every offset with static_assert. (ABI v1 put an int32 before a
 *     double, which C pads and MQL5 does not: every later field was misread.)
 *   - Every decision is advisory except the guards, which are absolute. The EA
 *     may fail to fill a signal. It may not ignore a flatten.
 *
 * Times are UTC milliseconds since the epoch throughout. The EA converts from
 * broker server time once, at the edge, so nothing in here knows about GMT+2.
 *
 * Threading: one context per symbol, single-threaded. MT5 calls OnTick and
 * OnTimer from one thread; nothing here is safe to call concurrently on the
 * same context. Contexts on different threads (one per EA) are independent:
 * the only state they share is the registry of state files in use, which is
 * locked.
 */

#ifndef XAU_BRIDGE_H
#define XAU_BRIDGE_H

#include <stdint.h>

/* Three build modes, and getting this wrong is a link error rather than a
 * silent bug, which is the good kind of wrong:
 *   XAU_BRIDGE_BUILD  - compiling the DLL itself      -> dllexport
 *   XAU_BRIDGE_STATIC - linking the static lib (tests) -> plain
 *   neither           - importing the DLL (MQL5, apps) -> dllimport */
#if defined(_WIN32) && !defined(XAU_BRIDGE_STATIC)
#  if defined(XAU_BRIDGE_BUILD)
#    define XAU_API __declspec(dllexport)
#  else
#    define XAU_API __declspec(dllimport)
#  endif
#  define XAU_CALL __stdcall
#elif defined(_WIN32)
#  define XAU_API
#  define XAU_CALL __stdcall
#else
#  define XAU_API
#  define XAU_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Bump on ANY change to a struct below or to a function signature. The EA
 * checks this on init and refuses to run on a mismatch. Silently running a new
 * DLL against an old EA is how a "lots" field becomes a "price" field. */
#define XAU_BRIDGE_ABI_VERSION 3

typedef enum {
    XAU_ACTION_NONE  = 0,
    XAU_ACTION_BUY   = 1,   /* market buy, lots / sl_price / tp_price set   */
    XAU_ACTION_SELL  = 2,   /* market sell, likewise                          */
    XAU_ACTION_CLOSE = 3,   /* close this EA's position on the symbol         */
    /* Not a suggestion. Close everything on the symbol and stop trading until
     * a person resumes. */
    XAU_ACTION_FLATTEN_AND_HALT = 4
} xau_action;

typedef enum {
    XAU_OK                    = 0,
    XAU_ERR_ABI_MISMATCH      = 1,
    XAU_ERR_BAD_CONTEXT       = 2,
    XAU_ERR_BAD_ARGUMENT      = 3,
    XAU_ERR_NOT_INITIALISED   = 4,
    XAU_ERR_INTERNAL          = 5,
    XAU_ERR_REFUSED           = 6    /* e.g. resume while the kill file exists */
} xau_status;

/* Why trading stopped, or why an entry was refused. Reported so the operator
 * sees a reason, not a silence. */
typedef enum {
    XAU_HALT_NONE            = 0,
    XAU_HALT_DAILY_LOSS      = 1,
    XAU_HALT_MAX_DRAWDOWN    = 2,
    XAU_HALT_KILL_FILE       = 3,
    XAU_HALT_STALE_QUOTES    = 4,   /* broken quote halts; a stale one refuses entry */
    XAU_HALT_SPREAD_BLOWOUT  = 5,   /* refuses entry, never halts                    */
    XAU_HALT_MANUAL          = 6,
    XAU_HALT_RECONCILE_DRIFT = 7,
    XAU_HALT_STATE_FILE      = 8    /* persisted state unreadable: fail closed       */
} xau_halt_reason;

/* Bar lengths, as xau::Timeframe numbers them. */
typedef enum {
    XAU_TF_M1 = 0, XAU_TF_M5 = 1, XAU_TF_M15 = 2, XAU_TF_M30 = 3,
    XAU_TF_H1 = 4, XAU_TF_H4 = 5, XAU_TF_D1 = 6
} xau_timeframe;

/* Market and account state, pushed in by the EA on every tick. 128 bytes.
 *
 * "Ours" means positions carrying this EA's magic number on this symbol.
 * Everything else on the symbol is foreign: a manual trade or another EA,
 * which the bridge refuses to trade alongside. */
typedef struct {
    int64_t tick_time_ms;       /* UTC time of this quote                      */
    int64_t now_ms;             /* UTC time now, by the broker's clock          */
    int64_t pos_open_time_ms;   /* UTC time our position opened; 0 when flat    */
    double  bid;
    double  ask;
    double  equity;
    double  balance;
    double  pos_lots;           /* our position's volume, unsigned              */
    double  pos_open_price;
    double  pos_sl;             /* 0 = none                                     */
    double  pos_tp;             /* 0 = none                                     */
    double  gross_lots;         /* |volume| of EVERY position on the symbol      */
    double  value_per_price_lot;/* ACCOUNT currency per 1.0 move in price, per lot
                                   (tick value / tick size, loss side). Sizing by
                                   risk needs it: 0 refuses risk-sized entries   */
    int32_t pos_side;           /* 0 flat, 1 long, -1 short                     */
    int32_t own_positions;
    int32_t foreign_positions;
    int32_t server_day;         /* broker trading day as yyyymmdd               */
    int32_t session_open;       /* 1 when the broker's trade session is open     */
    int32_t own_orders;         /* working (unfilled) orders of ours            */
} xau_market;

/* What the EA should do. 112 bytes. */
typedef struct {
    double  lots;
    double  sl_price;           /* 0 = none                                     */
    double  tp_price;           /* 0 = none                                     */
    /* The same stop and target as distances from the fill, price units. After a
     * fill the EA re-anchors SL/TP to the actual fill price, as the backtest
     * does, so slippage does not widen the loss at the stop. 0 = none. */
    double  sl_dist;
    double  tp_dist;
    int32_t action;             /* xau_action                                   */
    int32_t halt_reason;        /* xau_halt_reason, when halting or refusing     */
    /* Fixed buffer, not a pointer: the caller owns no memory and there is
     * nothing to free, which removes a whole class of cross-language leak. */
    char    reason[64];
} xau_decision;

/* Hard limits. Checked before any signal is considered; a breach produces
 * FLATTEN_AND_HALT regardless of what the strategy wants. 576 bytes. */
typedef struct {
    double  max_daily_loss_frac;   /* e.g. 0.04 = halt at -4% on the day          */
    double  max_drawdown_frac;     /* from the equity peak, or initial_balance    */
    double  initial_balance;       /* >0: drawdown measured from this, statically,
                                      as funded-trader firms do; 0: trailing peak */
    double  max_spread;            /* price units; refuse to ENTER wider than this */
    double  max_lots;              /* hard ceiling on gross size on the symbol     */
    int32_t max_open_positions;    /* of ours                                      */
    int32_t max_quote_age_ms;      /* older quote while the session is open: no entry */
    int32_t pending_timeout_ms;    /* order sent, no result or no position after
                                      this long: the book is not understood, halt */
    int32_t reserved;
    /* A path the operator can create to stop everything from outside the
     * process, without attaching a debugger or killing the terminal. */
    char    kill_file[260];
    /* Where halts and the daily anchors persist, so restarting MetaTrader does
     * not clear a halt or hand back a fresh daily-loss allowance. Empty: no
     * persistence (tests only). Both paths are UTF-8. */
    char    state_file[260];
} xau_limits;

/* What to trade. Passed to xau_arm; until then the bridge runs the guards and
 * trades nothing. 128 bytes. */
typedef struct {
    double  fixed_lots;            /* >0: trade exactly this size                 */
    double  risk_per_trade;        /* else: fraction of equity lost at the stop    */
    double  contract_size;         /* units of the asset per lot (gold: 100)       */
    double  volume_min;
    double  volume_max;
    double  volume_step;
    int32_t timeframe;             /* xau_timeframe the strategy was validated on  */
    int32_t point_den;             /* internal price units per 1.0 of price: 1000
                                      for gold (the tick store's), 0 = 1000        */
    int32_t stops_level_pts;       /* broker minimum stop distance, internal units */
    int32_t max_history_bars;      /* must match the validated backtest; 0 = all   */
    char    strategy[64];          /* registry name, e.g. "TimeSeriesMomentum"     */
} xau_strategy_config;

/* One completed M1 bar of history, for warm-up. 56 bytes. */
typedef struct {
    int64_t time_ms;               /* UTC time the minute opened                  */
    double  open;
    double  high;
    double  low;
    double  close;
    double  spread;                /* price units                                  */
    int64_t tick_volume;
} xau_rate;

/* --- lifecycle ---------------------------------------------------------- */

/* Returns XAU_BRIDGE_ABI_VERSION. The EA calls this FIRST and refuses to
 * proceed on a mismatch. */
XAU_API int32_t XAU_CALL xau_abi_version(void);

/* sizeof each struct as this DLL was compiled. The EA compares against its own
 * sizeof and refuses to run on any difference -- the version number cannot
 * catch a struct edited on one side only. */
XAU_API int32_t XAU_CALL xau_struct_size(int32_t which);   /* 0 market, 1 decision,
                                                              2 limits, 3 strategy
                                                              config, 4 rate */

/* Create a trading context. Returns NULL on failure. symbol is copied.
 * Loads the state file: a persisted halt survives, an unreadable file halts.
 * A kill file whose folder does not exist halts too: that switch cannot work.
 * NULL also when another live context already uses the same state file: two
 * EAs sharing one file would overwrite each other's halts. */
XAU_API void* XAU_CALL xau_create(const char* symbol, int32_t abi_version,
                                  const xau_limits* limits);

XAU_API void XAU_CALL xau_destroy(void* ctx);

/* Arm a strategy from the shared registry. Until this succeeds the bridge
 * never enters a trade. Refused (XAU_ERR_BAD_ARGUMENT) for an unknown name,
 * timeframe or contract. Arming twice is refused: re-arm by re-creating. */
XAU_API int32_t XAU_CALL xau_arm(void* ctx, const xau_strategy_config* cfg);

/* Feed completed M1 bars, oldest first, so a strategy with a twelve-month
 * lookback is warm on its first live tick. Each minute becomes four quotes
 * (open, the nearer extreme, the farther extreme, close). Decisions made during
 * warm-up are discarded, exactly as a backtest discards them before its
 * trade_from date. Call after xau_arm and before the first xau_on_tick. */
XAU_API int32_t XAU_CALL xau_warmup(void* ctx, const xau_rate* rates, int32_t n);

/* --- per event ---------------------------------------------------------- */

/* The main entry point. Guards first, then the strategy. Writes into *out.
 * Never throws. On error *out is a safe FLATTEN_AND_HALT rather than left
 * undefined, because a bridge that fails open places orders it cannot explain. */
XAU_API int32_t XAU_CALL xau_on_tick(void* ctx, const xau_market* mkt, xau_decision* out);

/* Call once a second from OnTimer. Checks what must not wait for a tick: the
 * kill file, and an order whose result never came back. */
XAU_API int32_t XAU_CALL xau_on_timer(void* ctx, int64_t now_ms, xau_decision* out);

/* Report what happened to the last BUY, SELL or CLOSE. Until this arrives the
 * bridge sends nothing new, so one decision can never become two orders. A
 * failed ENTRY is not retried: the bar that produced it has passed. A failed
 * CLOSE is retried on the next ticks, and halts and flattens if it cannot get
 * through: an exit the strategy asked for is never silently dropped.
 * ok = 1 for TRADE_RETCODE_PLACED too: the order then shows in own_orders. */
XAU_API int32_t XAU_CALL xau_order_result(void* ctx, int32_t ok, int32_t retcode,
                                          double fill_price, double lots);

/* --- control ------------------------------------------------------------ */

/* Halt immediately and persist it. Idempotent. */
XAU_API int32_t XAU_CALL xau_halt(void* ctx, int32_t reason);

/* Clear a halt. Deliberately separate from xau_halt and never automatic: a
 * system that re-arms itself after hitting a loss limit does not have a loss
 * limit. Refused while the kill file exists, and after a STATE_FILE halt while
 * the file still cannot be read (fix or delete it first). */
XAU_API int32_t XAU_CALL xau_resume(void* ctx);

XAU_API int32_t XAU_CALL xau_is_halted(void* ctx);

/* Last human-readable event, for the EA to print into the Experts log.
 * Copies at most buf_len bytes and always null-terminates. */
XAU_API int32_t XAU_CALL xau_last_message(void* ctx, char* buf, int32_t buf_len);

/* A few lines of status for the chart: armed strategy, halt state, daily
 * anchor and loss, pending order. Same buffer contract as xau_last_message. */
XAU_API int32_t XAU_CALL xau_status_text(void* ctx, char* buf, int32_t buf_len);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* XAU_BRIDGE_H */
