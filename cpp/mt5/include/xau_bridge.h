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
 *   - Every decision is advisory except the guards, which are absolute. The EA
 *     may ignore a signal. It may not ignore a flatten.
 *
 * Threading: one context per symbol, single-threaded. MT5 calls OnTick from one
 * thread; nothing here is safe to call concurrently on the same context.
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
#define XAU_BRIDGE_ABI_VERSION 1

typedef enum {
    XAU_ACTION_NONE  = 0,
    XAU_ACTION_BUY   = 1,
    XAU_ACTION_SELL  = 2,
    XAU_ACTION_CLOSE = 3,
    /* Not a suggestion. Close everything and stop trading until re-armed. */
    XAU_ACTION_FLATTEN_AND_HALT = 4
} xau_action;

typedef enum {
    XAU_OK                    = 0,
    XAU_ERR_ABI_MISMATCH      = 1,
    XAU_ERR_BAD_CONTEXT       = 2,
    XAU_ERR_BAD_ARGUMENT      = 3,
    XAU_ERR_NOT_INITIALISED   = 4,
    XAU_ERR_INTERNAL          = 5
} xau_status;

/* Why trading stopped. Reported so the operator sees a reason, not a silence. */
typedef enum {
    XAU_HALT_NONE            = 0,
    XAU_HALT_DAILY_LOSS      = 1,
    XAU_HALT_MAX_DRAWDOWN    = 2,
    XAU_HALT_KILL_FILE       = 3,
    XAU_HALT_STALE_QUOTES    = 4,
    XAU_HALT_SPREAD_BLOWOUT  = 5,
    XAU_HALT_MANUAL          = 6,
    XAU_HALT_RECONCILE_DRIFT = 7
} xau_halt_reason;

/* Market state, pushed in by the EA on every tick. */
typedef struct {
    int64_t time_ms;        /* broker server time, ms since epoch      */
    double  bid;
    double  ask;
    double  equity;
    double  balance;
    double  day_start_equity;
    double  peak_equity;
    int32_t open_positions;
    double  open_lots;      /* signed: positive long, negative short   */
} xau_market;

/* What the EA should do. */
typedef struct {
    int32_t action;         /* xau_action                              */
    double  lots;
    double  sl_price;       /* 0 = none                                */
    double  tp_price;       /* 0 = none                                */
    int32_t halt_reason;    /* xau_halt_reason, when halting           */
    /* Fixed buffer, not a pointer: the caller owns no memory and there is
     * nothing to free, which removes a whole class of cross-language leak. */
    char    reason[64];
} xau_decision;

/* Hard limits. These are checked before any signal is considered, and a breach
 * produces FLATTEN_AND_HALT regardless of what the strategy wants. */
typedef struct {
    double  max_daily_loss_frac;   /* e.g. 0.04 = stop at -4% on the day     */
    double  max_drawdown_frac;     /* from peak equity                        */
    double  max_spread_usd;        /* refuse to trade wider than this         */
    double  max_lots;
    int32_t max_open_positions;
    int32_t max_quote_age_ms;      /* stale feed = halt, never trade blind    */
    /* A path the operator can create to stop everything from outside the
     * process, without attaching a debugger or killing the terminal. */
    char    kill_file[260];
} xau_limits;

/* --- lifecycle ---------------------------------------------------------- */

/* Returns XAU_BRIDGE_ABI_VERSION. The EA calls this FIRST and refuses to
 * proceed on a mismatch. */
XAU_API int32_t XAU_CALL xau_abi_version(void);

/* Create a trading context. Returns NULL on failure. symbol is copied. */
XAU_API void* XAU_CALL xau_create(const char* symbol, int32_t abi_version,
                                  const xau_limits* limits);

XAU_API void XAU_CALL xau_destroy(void* ctx);

/* --- per-tick ----------------------------------------------------------- */

/* The main entry point. Evaluates guards first, then the strategy.
 * Writes into *out. Never throws. Returns XAU_OK or an error code; on error
 * *out is set to a safe FLATTEN_AND_HALT rather than left undefined, because a
 * bridge that fails open places orders it cannot explain. */
XAU_API int32_t XAU_CALL xau_on_tick(void* ctx, const xau_market* mkt, xau_decision* out);

/* --- control ------------------------------------------------------------ */

/* Halt immediately. Idempotent. */
XAU_API int32_t XAU_CALL xau_halt(void* ctx, int32_t reason);

/* Clear a halt. Deliberately separate from xau_halt and never automatic: a
 * system that re-arms itself after hitting a loss limit does not have a loss
 * limit. */
XAU_API int32_t XAU_CALL xau_resume(void* ctx);

XAU_API int32_t XAU_CALL xau_is_halted(void* ctx);

/* Last human-readable message, for the EA to print into the Experts log.
 * Copies at most buf_len bytes and always null-terminates. */
XAU_API int32_t XAU_CALL xau_last_message(void* ctx, char* buf, int32_t buf_len);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* XAU_BRIDGE_H */
