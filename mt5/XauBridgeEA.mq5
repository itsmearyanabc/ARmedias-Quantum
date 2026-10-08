//+------------------------------------------------------------------+
//| XauBridgeEA.mq5                                                   |
//|                                                                   |
//| The MetaTrader side of the bridge. Deliberately thin: this file    |
//| decides nothing. It reports market state to xaubridge.dll, carries |
//| out what comes back, and reports what happened.                    |
//|                                                                   |
//| Keeping the logic on the C++ side is not tidiness. It is the only  |
//| way the thing that trades live is the same thing that was          |
//| backtested -- and test_bridge.cpp checks exactly that, trade for   |
//| trade, against the backtest engine.                                |
//|                                                                   |
//| Requires: Tools > Options > Expert Advisors > Allow DLL imports,  |
//|           and Algo Trading enabled.                                |
//|                                                                   |
//| Runs on DEMO accounts only unless InpAllowRealAccount is set.      |
//| No strategy in this project has passed its validation gates; the  |
//| plan's next step is six weeks on a demo account, not real money.  |
//+------------------------------------------------------------------+
#property copyright "ARmedias Quantum"
#property version   "2.00"
#property description "Runs the ARmedias Quantum engine (xaubridge.dll) on this chart."

#include <Trade\Trade.mqh>

//--- must match XAU_BRIDGE_ABI_VERSION in xau_bridge.h
#define XAU_ABI_EXPECTED 2

//--- xau_action
#define ACT_NONE              0
#define ACT_BUY               1
#define ACT_SELL              2
#define ACT_CLOSE             3
#define ACT_FLATTEN_AND_HALT  4

#define HALT_MANUAL           6

//--- Struct layout must match the C header EXACTLY. MQL5 marshals by layout
//--- with no type checking whatsoever. Every struct is ordered 8-byte, 4-byte,
//--- then byte fields, with a size that is a multiple of 8, so MQL5's 1-byte
//--- packing and C's natural alignment produce the same layout. OnInit also
//--- compares every sizeof with the DLL's and refuses to run on a difference.
struct XauMarket
  {
   long              tick_time_ms;
   long              now_ms;
   long              pos_open_time_ms;
   double            bid;
   double            ask;
   double            equity;
   double            balance;
   double            pos_lots;
   double            pos_open_price;
   double            pos_sl;
   double            pos_tp;
   double            gross_lots;
   int               pos_side;
   int               own_positions;
   int               foreign_positions;
   int               server_day;
   int               session_open;
   int               reserved;
  };

struct XauDecision
  {
   double            lots;
   double            sl_price;
   double            tp_price;
   int               action;
   int               halt_reason;
   uchar             reason[64];
  };

struct XauLimits
  {
   double            max_daily_loss_frac;
   double            max_drawdown_frac;
   double            initial_balance;
   double            max_spread;
   double            max_lots;
   int               max_open_positions;
   int               max_quote_age_ms;
   int               pending_timeout_ms;
   int               reserved;
   uchar             kill_file[260];
   uchar             state_file[260];
  };

struct XauStrategyConfig
  {
   double            fixed_lots;
   double            risk_per_trade;
   double            contract_size;
   double            volume_min;
   double            volume_max;
   double            volume_step;
   int               timeframe;
   int               point_den;
   int               stops_level_pts;
   int               max_history_bars;
   uchar             strategy[64];
  };

struct XauRate
  {
   long              time_ms;
   double            open;
   double            high;
   double            low;
   double            close;
   double            spread;
   long              tick_volume;
  };

#import "xaubridge.dll"
int  xau_abi_version(void);
int  xau_struct_size(int which);
long xau_create(uchar &symbol[], int abi_version, XauLimits &limits);
void xau_destroy(long ctx);
int  xau_arm(long ctx, XauStrategyConfig &cfg);
int  xau_warmup(long ctx, XauRate &rates[], int n);
int  xau_on_tick(long ctx, XauMarket &mkt, XauDecision &out);
int  xau_on_timer(long ctx, long now_ms, XauDecision &out);
int  xau_order_result(long ctx, int ok, int retcode, double fill_price, double lots);
int  xau_halt(long ctx, int reason);
int  xau_resume(long ctx);
int  xau_is_halted(long ctx);
int  xau_last_message(long ctx, uchar &buf[], int buf_len);
int  xau_status_text(long ctx, uchar &buf[], int buf_len);
#import

//--- the bar lengths the engine knows, numbered as xau::Timeframe
enum XauTimeframe
  {
   XAU_M1  = 0,   // M1
   XAU_M5  = 1,   // M5
   XAU_M15 = 2,   // M15
   XAU_M30 = 3,   // M30
   XAU_H1  = 4,   // H1
   XAU_H4  = 5,   // H4
   XAU_D1  = 6    // D1
  };

//--- inputs: what to trade
input string       InpStrategy         = "";       // Strategy (registry name; empty = guards only)
input XauTimeframe InpTimeframe        = XAU_D1;   // Bar length the strategy was tested on
input double       InpFixedLots        = 0.01;     // Fixed lots (0 = size by risk)
input double       InpRiskPct          = 0.0;      // Else: % of equity lost at the stop
input int          InpWarmupDays       = 400;      // History fed in at start-up (days)
//--- inputs: hard limits
input double       InpMaxDailyLossPct  = 4.0;      // Halt at this loss on the broker day (%)
input double       InpMaxDrawdownPct   = 8.0;      // Halt at this drawdown (%)
input double       InpInitialBalance   = 0.0;      // >0: drawdown from this, not the peak
input double       InpMaxSpread        = 1.00;     // Refuse entries wider than this (price)
input double       InpMaxLots          = 0.10;     // Hard ceiling on open lots
input int          InpMaxQuoteAgeSec   = 10;       // Refuse entries on older quotes (s)
input string       InpKillFile         = "XAU_STOP.txt"; // Create this in MQL5\Files to halt
//--- inputs: execution
input int          InpMagic            = 990101;
input int          InpDeviationPts     = 30;       // Max slippage accepted (broker points)
input bool         InpDryRun           = false;    // Log orders instead of sending them
input bool         InpAllowRealAccount = false;    // Allow a REAL account (default: demo only)

//--- state
long    g_ctx        = 0;
long    g_offset_ms  = 0;      // broker server time minus UTC
int     g_point_den  = 1000;
int     g_last_halt  = -1;
string  g_last_msg   = "";
CTrade  g_trade;

#define BTN_HALT   "XAU_BTN_HALT"
#define BTN_RESUME "XAU_BTN_RESUME"

//+------------------------------------------------------------------+
//| helpers                                                          |
//+------------------------------------------------------------------+
void CopyToFixed(const string s, uchar &dst[], const int cap)
  {
   ArrayInitialize(dst, 0);
   uchar tmp[];
   int n = StringToCharArray(s, tmp, 0, WHOLE_ARRAY, CP_UTF8);
   int m = MathMin(n, cap - 1);
   for(int i = 0; i < m; i++)
      dst[i] = tmp[i];
   dst[cap - 1] = 0;   // always terminated, whatever the input
  }
//+------------------------------------------------------------------+
string FixedToString(const uchar &buf[])
  {
   return CharArrayToString(buf, 0, WHOLE_ARRAY, CP_UTF8);
  }
//+------------------------------------------------------------------+
//| A bare file name goes in this terminal's MQL5\Files, where the     |
//| operator can find it; a full path is used as given.                |
//+------------------------------------------------------------------+
string FilesPath(const string name)
  {
   if(StringFind(name, ":") >= 0 || StringFind(name, "\\") == 0)
      return name;
   return TerminalInfoString(TERMINAL_DATA_PATH) + "\\MQL5\\Files\\" + name;
  }
//+------------------------------------------------------------------+
//| Server time minus UTC, rounded to the half hour. Brokers run on    |
//| GMT+2/+3 and shift with DST, so this is re-measured every minute.  |
//+------------------------------------------------------------------+
void RefreshOffset()
  {
   long secs = (long)(TimeTradeServer() - TimeGMT());
   g_offset_ms = (long)MathRound(secs / 1800.0) * 1800 * 1000;
  }
//+------------------------------------------------------------------+
long NowUtcMs()
  {
   return (long)TimeTradeServer() * 1000 - g_offset_ms;
  }
//+------------------------------------------------------------------+
int ServerDay()
  {
   MqlDateTime dt;
   TimeToStruct(TimeTradeServer(), dt);
   return dt.year * 10000 + dt.mon * 100 + dt.day;
  }
//+------------------------------------------------------------------+
//| Whether the broker's trade session is open right now. A missing    |
//| quote is only stale while the market should be quoting.            |
//+------------------------------------------------------------------+
bool SessionOpen()
  {
   datetime now = TimeTradeServer();
   MqlDateTime dt;
   TimeToStruct(now, dt);
   long secs = dt.hour * 3600 + dt.min * 60 + dt.sec;
   for(uint i = 0; i < 16; i++)
     {
      datetime from, to;
      if(!SymbolInfoSessionTrade(_Symbol, (ENUM_DAY_OF_WEEK)dt.day_of_week, i, from, to))
         break;
      if(secs >= (long)from && secs < (long)to)
         return(true);
     }
   return(false);
  }
//+------------------------------------------------------------------+
double NormalizeVolume(const double v)
  {
   double step = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_STEP);
   double mn   = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN);
   double mx   = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MAX);
   if(step <= 0.0)
      return(0.0);
   double out = MathFloor(v / step + 1e-9) * step;   // down, never up
   if(out < mn)
      return(0.0);
   return(NormalizeDouble(MathMin(out, mx), 8));
  }
//+------------------------------------------------------------------+
void LogBridge()
  {
   uchar buf[];
   ArrayResize(buf, 512);
   ArrayInitialize(buf, 0);
   xau_last_message(g_ctx, buf, 512);
   string msg = CharArrayToString(buf, 0, WHOLE_ARRAY, CP_UTF8);
   if(msg != g_last_msg)
     {
      g_last_msg = msg;
      Print("bridge: ", msg);
     }
  }
//+------------------------------------------------------------------+
void ShowStatus()
  {
   uchar buf[];
   ArrayResize(buf, 1024);
   ArrayInitialize(buf, 0);
   xau_status_text(g_ctx, buf, 1024);
   string mode = (AccountInfoInteger(ACCOUNT_TRADE_MODE) == ACCOUNT_TRADE_MODE_DEMO) ? "DEMO" : "REAL";
   Comment(CharArrayToString(buf, 0, WHOLE_ARRAY, CP_UTF8),
           "\naccount ", mode, (InpDryRun ? "  |  DRY RUN: no orders sent" : ""));
  }
//+------------------------------------------------------------------+
void MakeButton(const string name, const string text, const int y, const color bg)
  {
   if(ObjectFind(0, name) < 0)
      ObjectCreate(0, name, OBJ_BUTTON, 0, 0, 0);
   ObjectSetInteger(0, name, OBJPROP_CORNER, CORNER_RIGHT_UPPER);
   ObjectSetInteger(0, name, OBJPROP_XDISTANCE, 130);
   ObjectSetInteger(0, name, OBJPROP_YDISTANCE, y);
   ObjectSetInteger(0, name, OBJPROP_XSIZE, 120);
   ObjectSetInteger(0, name, OBJPROP_YSIZE, 28);
   ObjectSetInteger(0, name, OBJPROP_BGCOLOR, bg);
   ObjectSetInteger(0, name, OBJPROP_COLOR, clrWhite);
   ObjectSetInteger(0, name, OBJPROP_FONTSIZE, 10);
   ObjectSetInteger(0, name, OBJPROP_SELECTABLE, false);
   ObjectSetInteger(0, name, OBJPROP_STATE, false);
   ObjectSetString(0, name, OBJPROP_TEXT, text);
  }
//+------------------------------------------------------------------+
//| our position (magic) vs everything else on the symbol             |
//+------------------------------------------------------------------+
void FillPositions(XauMarket &m)
  {
   for(int i = PositionsTotal() - 1; i >= 0; i--)
     {
      ulong ticket = PositionGetTicket(i);
      if(ticket == 0)
         continue;
      if(PositionGetString(POSITION_SYMBOL) != _Symbol)
         continue;
      double vol = PositionGetDouble(POSITION_VOLUME);
      m.gross_lots += vol;
      //--- Every position on the symbol is counted. A manual trade or a second
      //--- EA is exactly the drift the bridge must see, so it is reported as
      //--- foreign rather than filtered out.
      if(PositionGetInteger(POSITION_MAGIC) != InpMagic)
        {
         m.foreign_positions++;
         continue;
        }
      m.own_positions++;
      if(m.own_positions == 1)
        {
         bool buy = (PositionGetInteger(POSITION_TYPE) == POSITION_TYPE_BUY);
         m.pos_side         = buy ? 1 : -1;
         m.pos_lots         = vol;
         m.pos_open_price   = PositionGetDouble(POSITION_PRICE_OPEN);
         m.pos_sl           = PositionGetDouble(POSITION_SL);
         m.pos_tp           = PositionGetDouble(POSITION_TP);
         m.pos_open_time_ms = PositionGetInteger(POSITION_TIME_MSC) - g_offset_ms;
        }
     }
  }
//+------------------------------------------------------------------+
//| Close positions on the symbol: ours only, or everything.          |
//+------------------------------------------------------------------+
bool ClosePositions(const bool everything)
  {
   bool all_ok = true;
   for(int i = PositionsTotal() - 1; i >= 0; i--)
     {
      ulong ticket = PositionGetTicket(i);
      if(ticket == 0)
         continue;
      if(PositionGetString(POSITION_SYMBOL) != _Symbol)
         continue;
      if(!everything && PositionGetInteger(POSITION_MAGIC) != InpMagic)
         continue;
      if(InpDryRun)
        {
         PrintFormat("DRY RUN would close #%I64u %.2f lots", ticket,
                     PositionGetDouble(POSITION_VOLUME));
         continue;
        }
      if(!g_trade.PositionClose(ticket, (ulong)InpDeviationPts))
        {
         all_ok = false;
         PrintFormat("close #%I64u FAILED retcode=%u %s", ticket, g_trade.ResultRetcode(),
                     g_trade.ResultRetcodeDescription());
        }
     }
   return(all_ok);
  }
//+------------------------------------------------------------------+
//| Carry out a decision, and always tell the bridge what happened:   |
//| it sends nothing new until it hears.                              |
//+------------------------------------------------------------------+
void Execute(const XauDecision &d)
  {
   switch(d.action)
     {
      case ACT_FLATTEN_AND_HALT:
        {
         if(d.halt_reason != g_last_halt)
           {
            g_last_halt = d.halt_reason;
            PrintFormat("HALT (%d): %s -- flattening %s", d.halt_reason,
                        FixedToString(d.reason), _Symbol);
           }
         ClosePositions(true);
         return;
        }
      case ACT_CLOSE:
        {
         bool ok = ClosePositions(false);
         xau_order_result(g_ctx, (ok && !InpDryRun) ? 1 : 0, (int)g_trade.ResultRetcode(),
                          g_trade.ResultPrice(), 0.0);
         return;
        }
      case ACT_BUY:
      case ACT_SELL:
        {
         bool   buy  = (d.action == ACT_BUY);
         double lots = NormalizeVolume(d.lots);
         double sl   = (d.sl_price > 0.0) ? NormalizeDouble(d.sl_price, _Digits) : 0.0;
         double tp   = (d.tp_price > 0.0) ? NormalizeDouble(d.tp_price, _Digits) : 0.0;
         string why  = "xau " + FixedToString(d.reason);
         if(InpDryRun || lots <= 0.0)
           {
            PrintFormat("%s %s %.2f lots sl %.5f tp %.5f (%s)",
                        (InpDryRun ? "DRY RUN would" : "SKIPPED: volume rounds to 0 for"),
                        (buy ? "BUY" : "SELL"), d.lots, sl, tp, why);
            xau_order_result(g_ctx, 0, -1, 0.0, 0.0);   // not filled; never retried
            return;
           }
         //--- The stop and target ride on the order, so the broker holds the
         //--- protection even if this terminal, this EA or the DLL goes away.
         bool sent = buy ? g_trade.Buy(lots, _Symbol, 0.0, sl, tp, why)
                         : g_trade.Sell(lots, _Symbol, 0.0, sl, tp, why);
         uint rc   = g_trade.ResultRetcode();
         bool filled = sent && (rc == TRADE_RETCODE_DONE || rc == TRADE_RETCODE_DONE_PARTIAL ||
                                rc == TRADE_RETCODE_PLACED);
         PrintFormat("%s %.2f lots sl %.5f tp %.5f -> %s (retcode %u %s)", (buy ? "BUY" : "SELL"),
                     lots, sl, tp, (filled ? "FILLED" : "FAILED"), rc,
                     g_trade.ResultRetcodeDescription());
         xau_order_result(g_ctx, filled ? 1 : 0, (int)rc, g_trade.ResultPrice(),
                          g_trade.ResultVolume());
         return;
        }
      default:
         return;
     }
  }
//+------------------------------------------------------------------+
//| Feed `tf` bars from `from` up to (not including) `upto`, oldest    |
//| first. Only completed bars: the forming one is still changing.     |
//+------------------------------------------------------------------+
int FeedHistory(const ENUM_TIMEFRAMES tf, const datetime from, const datetime upto)
  {
   if(upto <= from)
      return(0);
   MqlRates r[];
   ArraySetAsSeries(r, false);
   int n = -1;
   for(int attempt = 0; attempt < 5 && n < 0; attempt++)
     {
      n = CopyRates(_Symbol, tf, from, upto - 1, r);
      if(n < 0)
         Sleep(500);   // history still synchronising with the server
     }
   if(n <= 0)
      return(0);
   XauRate xr[];
   ArrayResize(xr, n);
   for(int i = 0; i < n; i++)
     {
      xr[i].time_ms     = ((long)r[i].time) * 1000 - g_offset_ms;
      xr[i].open        = r[i].open;
      xr[i].high        = r[i].high;
      xr[i].low         = r[i].low;
      xr[i].close       = r[i].close;
      xr[i].spread      = r[i].spread * _Point;
      xr[i].tick_volume = r[i].tick_volume;
     }
   if(xau_warmup(g_ctx, xr, n) != 0)
      return(-1);
   return(n);
  }
//+------------------------------------------------------------------+
//| Warm the strategy on history so it trades on its first live bar.   |
//| H1 bars for the long past -- they line up with UTC hours, because  |
//| broker offsets are whole hours -- then M1 for the current hour.    |
//+------------------------------------------------------------------+
void Warmup()
  {
   ENUM_TIMEFRAMES coarse = PERIOD_H1;
   switch(InpTimeframe)
     {
      case XAU_M1:  coarse = PERIOD_M1;  break;
      case XAU_M5:  coarse = PERIOD_M5;  break;
      case XAU_M15: coarse = PERIOD_M15; break;
      case XAU_M30: coarse = PERIOD_M30; break;
      default:      coarse = PERIOD_H1;  break;
     }
   datetime now    = TimeTradeServer();
   datetime from   = now - (datetime)(MathMax(InpWarmupDays, 1) * 86400);
   datetime coarse_open = iTime(_Symbol, coarse, 0);   // the forming bar
   datetime minute_open = iTime(_Symbol, PERIOD_M1, 0);
   int n1 = FeedHistory(coarse, from, coarse_open);
   int n2 = (coarse != PERIOD_M1) ? FeedHistory(PERIOD_M1, coarse_open, minute_open) : 0;
   PrintFormat("warm-up: %d %s bars + %d M1 bars from %s", n1, EnumToString(coarse), n2,
               TimeToString(from));
   if(n1 <= 0)
      Print("WARNING: no history loaded. The strategy will hold until it has enough bars.");
   LogBridge();
  }
//+------------------------------------------------------------------+
int OnInit()
  {
//--- Version and layout FIRST, before anything else touches the DLL.
   ResetLastError();
   int abi = xau_abi_version();
   if(_LastError != 0)
     {
      Print("xaubridge.dll not loadable (error ", _LastError,
            "). Enable Allow DLL imports and put the DLL in MQL5\\Libraries.");
      return(INIT_FAILED);
     }
   if(abi != XAU_ABI_EXPECTED)
     {
      PrintFormat("ABI mismatch: DLL %d, EA %d. Refusing to run.", abi, XAU_ABI_EXPECTED);
      return(INIT_FAILED);
     }
   if(xau_struct_size(0) != sizeof(XauMarket) || xau_struct_size(1) != sizeof(XauDecision) ||
      xau_struct_size(2) != sizeof(XauLimits) || xau_struct_size(3) != sizeof(XauStrategyConfig) ||
      xau_struct_size(4) != sizeof(XauRate))
     {
      PrintFormat("struct layout mismatch: DLL %d/%d/%d/%d/%d, EA %d/%d/%d/%d/%d. Refusing to run.",
                  xau_struct_size(0), xau_struct_size(1), xau_struct_size(2), xau_struct_size(3),
                  xau_struct_size(4), sizeof(XauMarket), sizeof(XauDecision), sizeof(XauLimits),
                  sizeof(XauStrategyConfig), sizeof(XauRate));
      return(INIT_FAILED);
     }

//--- Demo only, unless the operator has said otherwise in so many words.
   ENUM_ACCOUNT_TRADE_MODE mode = (ENUM_ACCOUNT_TRADE_MODE)AccountInfoInteger(ACCOUNT_TRADE_MODE);
   if(mode != ACCOUNT_TRADE_MODE_DEMO && !InpAllowRealAccount)
     {
      Print("Refusing to run on a non-demo account. No strategy has passed validation; ",
            "set InpAllowRealAccount only after the six-week demo trial.");
      return(INIT_FAILED);
     }

   RefreshOffset();
   g_point_den = (int)MathMax(1000.0, MathPow(10.0, _Digits));

   XauLimits lim;
   ZeroMemory(lim);
   lim.max_daily_loss_frac = InpMaxDailyLossPct / 100.0;
   lim.max_drawdown_frac   = InpMaxDrawdownPct / 100.0;
   lim.initial_balance     = InpInitialBalance;
   lim.max_spread          = InpMaxSpread;
   lim.max_lots            = InpMaxLots;
   lim.max_open_positions  = 1;
   lim.max_quote_age_ms    = InpMaxQuoteAgeSec * 1000;
   lim.pending_timeout_ms  = 30000;
   CopyToFixed((InpKillFile == "") ? "" : FilesPath(InpKillFile), lim.kill_file, 260);
   CopyToFixed(FilesPath(StringFormat("xau_state_%s_%d.txt", _Symbol, InpMagic)),
               lim.state_file, 260);

   uchar sym[];
   ArrayResize(sym, 64);
   CopyToFixed(_Symbol, sym, 64);
   g_ctx = xau_create(sym, XAU_ABI_EXPECTED, lim);
   if(g_ctx == 0)
     {
      Print("xau_create failed");
      return(INIT_FAILED);
     }
   LogBridge();

   g_trade.SetExpertMagicNumber((ulong)InpMagic);
   g_trade.SetDeviationInPoints((ulong)InpDeviationPts);
   g_trade.SetTypeFillingBySymbol(_Symbol);

   if(InpStrategy == "")
     {
      Print("No strategy set (InpStrategy): running the guards only, no entries.");
     }
   else
     {
      XauStrategyConfig cfg;
      ZeroMemory(cfg);
      cfg.fixed_lots      = InpFixedLots;
      cfg.risk_per_trade  = (InpFixedLots > 0.0) ? 0.0 : InpRiskPct / 100.0;
      cfg.contract_size   = SymbolInfoDouble(_Symbol, SYMBOL_TRADE_CONTRACT_SIZE);
      cfg.volume_min      = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MIN);
      cfg.volume_max      = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_MAX);
      cfg.volume_step     = SymbolInfoDouble(_Symbol, SYMBOL_VOLUME_STEP);
      cfg.timeframe       = (int)InpTimeframe;
      cfg.point_den       = g_point_den;
      cfg.stops_level_pts = (int)MathRound(SymbolInfoInteger(_Symbol, SYMBOL_TRADE_STOPS_LEVEL) *
                                           _Point * g_point_den);
      cfg.max_history_bars = 0;
      CopyToFixed(InpStrategy, cfg.strategy, 64);
      if(xau_arm(g_ctx, cfg) != 0)
        {
         LogBridge();
         Print("Could not arm '", InpStrategy, "'. Refusing to run with a strategy that did not load.");
         xau_destroy(g_ctx);
         g_ctx = 0;
         return(INIT_FAILED);
        }
      LogBridge();
      Warmup();
     }

   MakeButton(BTN_HALT, "HALT", 40, clrFireBrick);
   MakeButton(BTN_RESUME, "RESUME", 74, clrDarkGreen);
   EventSetTimer(1);

   PrintFormat("XauBridgeEA ready on %s (%s account%s) | strategy: %s | daily %.1f%% dd %.1f%%",
               _Symbol, (mode == ACCOUNT_TRADE_MODE_DEMO ? "DEMO" : "REAL"),
               (InpDryRun ? ", DRY RUN" : ""), (InpStrategy == "" ? "none" : InpStrategy),
               InpMaxDailyLossPct, InpMaxDrawdownPct);
   ShowStatus();
   return(INIT_SUCCEEDED);
  }
//+------------------------------------------------------------------+
void OnDeinit(const int reason)
  {
   EventKillTimer();
   ObjectDelete(0, BTN_HALT);
   ObjectDelete(0, BTN_RESUME);
   Comment("");
   if(g_ctx != 0)
     {
      xau_destroy(g_ctx);
      g_ctx = 0;
     }
  }
//+------------------------------------------------------------------+
void OnTick()
  {
   if(g_ctx == 0)
      return;
   MqlTick tk;
   if(!SymbolInfoTick(_Symbol, tk))
      return;

   XauMarket m;
   ZeroMemory(m);
   m.tick_time_ms = tk.time_msc - g_offset_ms;
   m.now_ms       = NowUtcMs();
   m.bid          = tk.bid;
   m.ask          = tk.ask;
   m.equity       = AccountInfoDouble(ACCOUNT_EQUITY);
   m.balance      = AccountInfoDouble(ACCOUNT_BALANCE);
   m.server_day   = ServerDay();
   m.session_open = SessionOpen() ? 1 : 0;
   FillPositions(m);

   XauDecision d;
   ZeroMemory(d);
   int rc = xau_on_tick(g_ctx, m, d);
//--- A non-zero return is not a reason to carry on cautiously: the bridge
//--- fails closed and has already written a FLATTEN into d. Obey it.
   if(rc != 0)
      PrintFormat("bridge returned %d: %s", rc, FixedToString(d.reason));
   Execute(d);
   LogBridge();
  }
//+------------------------------------------------------------------+
void OnTimer()
  {
   if(g_ctx == 0)
      return;
   static int ticks = 0;
   if(++ticks % 60 == 0)
      RefreshOffset();   // DST changes the broker's offset twice a year

   XauDecision d;
   ZeroMemory(d);
   xau_on_timer(g_ctx, NowUtcMs(), d);
   if(d.action == ACT_FLATTEN_AND_HALT)
      Execute(d);   // the kill file lands even with the market closed
   LogBridge();
   ShowStatus();
  }
//+------------------------------------------------------------------+
void OnChartEvent(const int id, const long &lparam, const double &dparam, const string &sparam)
  {
   if(id != CHARTEVENT_OBJECT_CLICK || g_ctx == 0)
      return;
   if(sparam == BTN_HALT)
     {
      xau_halt(g_ctx, HALT_MANUAL);
      Print("HALT pressed: flattening ", _Symbol);
      ClosePositions(true);
      ObjectSetInteger(0, BTN_HALT, OBJPROP_STATE, false);
     }
   else if(sparam == BTN_RESUME)
     {
      ObjectSetInteger(0, BTN_RESUME, OBJPROP_STATE, false);
      //--- Never automatic and never one click: a system that re-arms itself
      //--- after a loss limit does not have a loss limit.
      if(MessageBox("Resume trading on " + _Symbol + "?\n\nCheck why it halted first.",
                    "XAU bridge", MB_YESNO | MB_ICONWARNING) == IDYES)
        {
         if(xau_resume(g_ctx) == 0)
           {
            g_last_halt = -1;
            Print("RESUMED by the operator");
           }
         else
            Print("resume refused (is the kill file still there?)");
        }
     }
   LogBridge();
   ShowStatus();
  }
//+------------------------------------------------------------------+
