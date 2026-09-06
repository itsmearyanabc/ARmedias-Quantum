//+------------------------------------------------------------------+
//| XauBridgeEA.mq5                                                   |
//|                                                                   |
//| The MetaTrader side of the bridge. Deliberately thin: this file    |
//| decides nothing. It reports market state to xaubridge.dll, and     |
//| carries out what comes back.                                       |
//|                                                                   |
//| Keeping the logic on the C++ side is not tidiness. It is the only  |
//| way the thing that trades live is the same thing that was          |
//| backtested. Reimplementing a strategy in MQL5 means maintaining two |
//| versions of it, and the day they disagree is the day you find out  |
//| the backtest was describing a program you are not running.         |
//|                                                                   |
//| Requires: Tools > Options > Expert Advisors > Allow DLL imports    |
//+------------------------------------------------------------------+
#property copyright "ARmedias Quantum"
#property version   "1.00"
#property strict

//--- must match XAU_BRIDGE_ABI_VERSION in xau_bridge.h
#define XAU_ABI_EXPECTED 1

//--- xau_action
#define ACT_NONE              0
#define ACT_BUY               1
#define ACT_SELL              2
#define ACT_CLOSE             3
#define ACT_FLATTEN_AND_HALT  4

//--- Struct layout must match the C header EXACTLY. MQL5 marshals by layout
//--- with no type checking whatsoever: a field of the wrong width here does
//--- not fail, it silently shifts every field after it. That is why the DLL
//--- exposes a version and this EA refuses to run on a mismatch.
struct XauMarket
  {
   long              time_ms;
   double            bid;
   double            ask;
   double            equity;
   double            balance;
   double            day_start_equity;
   double            peak_equity;
   int               open_positions;
   double            open_lots;
  };

struct XauDecision
  {
   int               action;
   double            lots;
   double            sl_price;
   double            tp_price;
   int               halt_reason;
   uchar             reason[64];
  };

struct XauLimits
  {
   double            max_daily_loss_frac;
   double            max_drawdown_frac;
   double            max_spread_usd;
   double            max_lots;
   int               max_open_positions;
   int               max_quote_age_ms;
   uchar             kill_file[260];
  };

#import "xaubridge.dll"
int  xau_abi_version(void);
long xau_create(uchar &symbol[], int abi_version, XauLimits &limits);
void xau_destroy(long ctx);
int  xau_on_tick(long ctx, XauMarket &mkt, XauDecision &out);
int  xau_halt(long ctx, int reason);
int  xau_resume(long ctx);
int  xau_is_halted(long ctx);
int  xau_last_message(long ctx, uchar &buf[], int buf_len);
#import

//--- inputs
input double InpMaxDailyLossPct   = 4.0;    // halt at this daily loss (%)
input double InpMaxDrawdownPct    = 8.0;    // halt at this drawdown from peak (%)
input double InpMaxSpreadUsd      = 1.00;   // refuse entry above this spread
input double InpMaxLots           = 0.10;   // hard ceiling on open size
input int    InpMaxPositions      = 1;
input string InpKillFile          = "";     // create this file to stop everything
input int    InpMagic             = 990101;
input bool   InpDryRun            = true;   // log orders instead of placing them

//--- state
long   g_ctx        = 0;
double g_day_start  = 0.0;
double g_peak       = 0.0;
int    g_day        = -1;

//+------------------------------------------------------------------+
void StringToUchar(const string s, uchar &buf[], const int size)
  {
   ArrayResize(buf, size);
   ArrayInitialize(buf, 0);
   int n = StringToCharArray(s, buf, 0, StringLen(s), CP_UTF8);
   if(n >= size)
      buf[size - 1] = 0;   // always terminated, whatever the caller passed
  }
//+------------------------------------------------------------------+
string UcharToString(const uchar &buf[])
  {
   return CharArrayToString(buf, 0, WHOLE_ARRAY, CP_UTF8);
  }
//+------------------------------------------------------------------+
int OnInit()
  {
//--- Version check FIRST, before anything else touches the DLL. A struct
//--- layout change against a stale EA reinterprets fields silently: a lots
//--- value read as a price places an order a thousand times too large.
   int abi = 0;
   ResetLastError();
   abi = xau_abi_version();
   if(_LastError != 0)
     {
      Print("xaubridge.dll not loadable (error ", _LastError,
            "). Enable Allow DLL imports and place the DLL in MQL5/Libraries.");
      return(INIT_FAILED);
     }
   if(abi != XAU_ABI_EXPECTED)
     {
      Print("ABI mismatch: DLL reports ", abi, ", EA expects ", XAU_ABI_EXPECTED,
            ". Refusing to run.");
      return(INIT_FAILED);
     }

   XauLimits lim;
   lim.max_daily_loss_frac = InpMaxDailyLossPct / 100.0;
   lim.max_drawdown_frac   = InpMaxDrawdownPct / 100.0;
   lim.max_spread_usd      = InpMaxSpreadUsd;
   lim.max_lots            = InpMaxLots;
   lim.max_open_positions  = InpMaxPositions;
   lim.max_quote_age_ms    = 10000;
   StringToUchar(InpKillFile, lim.kill_file, 260);

   uchar sym[];
   StringToUchar(_Symbol, sym, 32);

   g_ctx = xau_create(sym, XAU_ABI_EXPECTED, lim);
   if(g_ctx == 0)
     {
      Print("xau_create failed");
      return(INIT_FAILED);
     }

   g_day_start = AccountInfoDouble(ACCOUNT_EQUITY);
   g_peak      = g_day_start;

   PrintFormat("XauBridgeEA ready on %s | dry run: %s | daily %.1f%% dd %.1f%%",
               _Symbol, (InpDryRun ? "YES" : "NO"),
               InpMaxDailyLossPct, InpMaxDrawdownPct);
   if(InpDryRun)
      Print("DRY RUN: decisions are logged, no orders are sent.");
   return(INIT_SUCCEEDED);
  }
//+------------------------------------------------------------------+
void OnDeinit(const int reason)
  {
   if(g_ctx != 0)
     {
      xau_destroy(g_ctx);
      g_ctx = 0;
     }
  }
//+------------------------------------------------------------------+
int CountOwnPositions(double &net_lots)
  {
   int n = 0;
   net_lots = 0.0;
   for(int i = PositionsTotal() - 1; i >= 0; i--)
     {
      ulong ticket = PositionGetTicket(i);
      if(ticket == 0)
         continue;
      if(PositionGetString(POSITION_SYMBOL) != _Symbol)
         continue;
      //--- Count EVERY position on this symbol, not just ours. A manual trade
      //--- or a second EA is exactly the drift the bridge needs to see;
      //--- filtering by magic would hide it and let us add on top.
      n++;
      double v = PositionGetDouble(POSITION_VOLUME);
      net_lots += (PositionGetInteger(POSITION_TYPE) == POSITION_TYPE_BUY) ? v : -v;
     }
   return(n);
  }
//+------------------------------------------------------------------+
void CloseAll()
  {
   for(int i = PositionsTotal() - 1; i >= 0; i--)
     {
      ulong ticket = PositionGetTicket(i);
      if(ticket == 0)
         continue;
      if(PositionGetString(POSITION_SYMBOL) != _Symbol)
         continue;

      MqlTradeRequest req;
      MqlTradeResult  res;
      ZeroMemory(req);
      ZeroMemory(res);
      req.action   = TRADE_ACTION_DEAL;
      req.position = ticket;
      req.symbol   = _Symbol;
      req.volume   = PositionGetDouble(POSITION_VOLUME);
      req.deviation= 20;
      req.magic    = InpMagic;
      req.type     = (PositionGetInteger(POSITION_TYPE) == POSITION_TYPE_BUY)
                     ? ORDER_TYPE_SELL : ORDER_TYPE_BUY;
      req.price    = (req.type == ORDER_TYPE_SELL)
                     ? SymbolInfoDouble(_Symbol, SYMBOL_BID)
                     : SymbolInfoDouble(_Symbol, SYMBOL_ASK);

      if(InpDryRun)
        {
         PrintFormat("DRY RUN would close #%I64u %.2f lots", ticket, req.volume);
         continue;
        }
      if(!OrderSend(req, res))
         PrintFormat("close #%I64u FAILED retcode=%u", ticket, res.retcode);
     }
  }
//+------------------------------------------------------------------+
void OnTick()
  {
   if(g_ctx == 0)
      return;

//--- Roll the daily anchor at the broker's date, not the local one. Using
//--- local midnight would reset the daily loss limit at the wrong moment and,
//--- on the wrong side of a timezone, hand back a fresh allowance mid-session.
   MqlDateTime dt;
   TimeToStruct(TimeCurrent(), dt);
   if(dt.day != g_day)
     {
      g_day       = dt.day;
      g_day_start = AccountInfoDouble(ACCOUNT_EQUITY);
     }

   double equity = AccountInfoDouble(ACCOUNT_EQUITY);
   if(equity > g_peak)
      g_peak = equity;

   double net_lots = 0.0;
   int    n_pos    = CountOwnPositions(net_lots);

   XauMarket mkt;
   mkt.time_ms          = (long)TimeCurrent() * 1000;
   mkt.bid              = SymbolInfoDouble(_Symbol, SYMBOL_BID);
   mkt.ask              = SymbolInfoDouble(_Symbol, SYMBOL_ASK);
   mkt.equity           = equity;
   mkt.balance          = AccountInfoDouble(ACCOUNT_BALANCE);
   mkt.day_start_equity = g_day_start;
   mkt.peak_equity      = g_peak;
   mkt.open_positions   = n_pos;
   mkt.open_lots        = net_lots;

   XauDecision dec;
   ZeroMemory(dec);
   int rc = xau_on_tick(g_ctx, mkt, dec);

//--- A non-zero return is not a reason to carry on cautiously. The bridge
//--- fails closed and has already written a FLATTEN into dec; the EA's job is
//--- to obey it, not to second-guess it.
   if(rc != 0)
      PrintFormat("bridge returned %d: %s", rc, UcharToString(dec.reason));

   switch(dec.action)
     {
      case ACT_FLATTEN_AND_HALT:
        {
         static int last_reason = -1;
         if(dec.halt_reason != last_reason)
           {
            last_reason = dec.halt_reason;
            PrintFormat("HALT (%d): %s", dec.halt_reason, UcharToString(dec.reason));
           }
         if(n_pos > 0)
            CloseAll();
         return;
        }
      case ACT_CLOSE:
         if(n_pos > 0)
            CloseAll();
         return;

      case ACT_BUY:
      case ACT_SELL:
         //--- Entries are not wired up. The bridge returns NONE until a
         //--- strategy clears validation, so reaching here means the DLL and
         //--- this EA disagree about what is armed -- which is a bug, and a
         //--- bug at this boundary is a bug that sends orders.
         PrintFormat("unexpected entry action %d; ignoring", dec.action);
         return;

      default:
         return;
     }
  }
//+------------------------------------------------------------------+
