package.path = package.path ..";?.lua;test/?.lua;app/?.lua;"
require "Pktgen"

-- Latency-versus-offered-load curve against an echo DUT (RFC 2544 style).
--
-- For each frame size we walk a ladder of offered rates and, at each step,
-- record throughput, loss, and round-trip latency. pktgen injects timestamped
-- probe packets at latency_rate_us intervals and reports min/avg/max plus a
-- count of probes exceeding jitter_threshold.
--
-- Note on metrics: pktgen exposes no percentiles, so the tail is represented
-- by max and by jitter_count (fraction of probes over a threshold). For a
-- bimodal latency distribution -- which this platform has -- a
-- fraction-over-threshold is a more stable tail statistic than a quantile
-- sitting between the two modes.
--
-- The settle delay before reading counters is load-bearing: sampling too soon
-- after stop() counts in-flight echoes as loss and yields impossible
-- non-monotonic loss curves.

local PORT      = "0"
local DUT_MAC   = os.getenv("DUT_MAC")  or "00:0f:53:28:54:90"
local SRC_IP    = os.getenv("SRC_IP")   or "10.0.1.1/24"
local DST_IP    = os.getenv("DST_IP")   or "10.0.1.2"
local DPORT     = tonumber(os.getenv("DPORT")     or "11111")
local TRIAL_MS  = tonumber(os.getenv("TRIAL_MS")  or "20000")
local SETTLE_MS = tonumber(os.getenv("SETTLE_MS") or "4000")
local LAT_US    = tonumber(os.getenv("LAT_US")    or "1000")  -- probe every 1ms
local JIT_US    = tonumber(os.getenv("JIT_US")    or "50")    -- tail threshold
local LABEL     = os.getenv("LABEL") or "dut"

local SIZES, RATES = {}, {}
for s in string.gmatch(os.getenv("SIZES") or "64,512,1518", "([^,]+)") do
    SIZES[#SIZES+1] = tonumber(s)
end
for r in string.gmatch(os.getenv("RATES") or "1,2,3,4,5,6,8,10,12,15,20,30", "([^,]+)") do
    RATES[#RATES+1] = tonumber(r)
end

local function step(size, pct)
    pktgen.set(PORT, "count", 0)
    pktgen.set(PORT, "size", size)
    pktgen.set(PORT, "rate", pct)
    pktgen.set_proto(PORT, "udp")
    pktgen.set_ipaddr(PORT, "src", SRC_IP)
    pktgen.set_ipaddr(PORT, "dst", DST_IP)
    pktgen.set_mac(PORT, "dst", DUT_MAC)
    pktgen.set(PORT, "sport", 26000)
    pktgen.set(PORT, "dport", DPORT)

    pktgen.latency(PORT, "rate", LAT_US)
    pktgen.latency(PORT, "entropy", 0)
    pktgen.latency(PORT, "enable")

    pktgen.clear("all")
    pktgen.delay(500)
    pktgen.start(PORT)
    pktgen.delay(TRIAL_MS / 2)
    local mid = pktgen.portStats(PORT, "rate")[0]     -- steady-state offered rate
    pktgen.delay(TRIAL_MS / 2)
    pktgen.stop(PORT)
    pktgen.delay(SETTLE_MS)

    local s = pktgen.portStats(PORT, "port")[0]
    local l = pktgen.pktStats(PORT)[0].latency   -- latency lives in pktStats, NOT portStats("port")
    local tx, rx = s.opackets, s.ipackets
    local loss = 0.0
    if tx > 0 then loss = (tx - rx) * 100.0 / tx end
    if loss < 0 then loss = 0.0 end

    local lmin, lavg, lmax, lnum, ljit = 0, 0, 0, 0, 0
    local p50, p90, p99, p999, hn = 0, 0, 0, 0, 0
    local lskip, stx = 0, 0
    if l ~= nil then
        lmin, lavg, lmax = l.min_us or 0, l.avg_us or 0, l.max_us or 0
        lnum, ljit = l.num_pkts or 0, l.jitter_count or 0
        lskip = l.num_skipped or 0
        p50, p90, p99, p999 = l.p50_us or 0, l.p90_us or 0, l.p99_us or 0, l.p999_us or 0
        hn = l.hist_count or 0
        stx = l.stamps_tx or 0
    end

    printf("LATLOAD %s size=%d pct=%.2f off_pps=%s tx=%d rx=%d loss=%.4f "..
           "lat_min=%.3f lat_avg=%.3f lat_max=%.3f "..
           "p50=%.3f p90=%.3f p99=%.3f p999=%.3f lat_n=%d skipped=%d hist_n=%d stx=%d jitter_n=%d "..
           "imissed=%d ierrors=%d oerrors=%d\n",
           LABEL, size, pct, tostring(mid.pkts_tx), tx, rx, loss,
           lmin, lavg, lmax, p50, p90, p99, p999, lnum, lskip, hn, stx, ljit,
           s.imissed, s.ierrors, s.oerrors)

    pktgen.latency(PORT, "disable")
end

-- Warmup: a freshly started DUT daemon drops everything for the first few
-- seconds (observed on the raw CAPIO echo: first trial 100% loss, second
-- clean). Blast moderate-rate traffic and discard the results.
-- Answer ARP/ping on the port: the F-Stack DUTs must resolve the client IP
-- before they can address their replies; without this their echoes go nowhere
-- and the arm reads as 100% loss.
pktgen.process(PORT, "on")

printf("WARMUP begin\n")
pktgen.set(PORT, "count", 0); pktgen.set(PORT, "size", 64); pktgen.set(PORT, "rate", tonumber(os.getenv("WARM_RATE") or "8"))
pktgen.set_proto(PORT, "udp")
pktgen.set_ipaddr(PORT, "src", SRC_IP); pktgen.set_ipaddr(PORT, "dst", DST_IP)
pktgen.set_mac(PORT, "dst", DUT_MAC)
pktgen.set(PORT, "sport", 26000); pktgen.set(PORT, "dport", DPORT)
pktgen.start(PORT); pktgen.delay(20000); pktgen.stop(PORT); pktgen.delay(4000)
pktgen.clear("all")
printf("WARMUP done\n")

printf("LATLOAD_BEGIN label=%s trial_ms=%d lat_us=%d jit_us=%d\n",
       LABEL, TRIAL_MS, LAT_US, JIT_US)
for _, size in ipairs(SIZES) do
    for _, pct in ipairs(RATES) do
        step(size, pct)
    end
end
printf("LATLOAD_END label=%s\n", LABEL)
pktgen.quit()
