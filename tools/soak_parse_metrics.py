#!/usr/bin/env python3
"""Parse soak metrics JSONL into key=value summary fields."""

import argparse
import json
import math
import sys
from datetime import datetime


def update_min(cur, val):
    if val is None:
        return cur
    if cur is None or val < cur:
        return val
    return cur


def update_max(cur, val):
    if val is None:
        return cur
    if cur is None or val > cur:
        return val
    return cur


def num(v):
    if isinstance(v, bool):
        return int(v)
    if isinstance(v, (int, float)):
        return v
    return None


def emit(key, val):
    if val is None:
        print(f"{key}=")
    else:
        print(f"{key}={val}")


def parse_ts_epoch(ts):
    if not isinstance(ts, str) or not ts:
        return None
    try:
        if ts.endswith("Z"):
            return datetime.fromisoformat(ts.replace("Z", "+00:00")).timestamp()
        return datetime.fromisoformat(ts).timestamp()
    except ValueError:
        return None


def percentile(values, pct):
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return float(ordered[0])
    rank = (pct / 100.0) * (len(ordered) - 1)
    lo = int(math.floor(rank))
    hi = int(math.ceil(rank))
    if lo == hi:
        return float(ordered[lo])
    frac = rank - lo
    return float(ordered[lo] + (ordered[hi] - ordered[lo]) * frac)


def parse_args(argv):
    parser = argparse.ArgumentParser(description="Parse soak metrics JSONL into key=value fields")
    parser.add_argument("metrics_jsonl", help="Path to metrics.jsonl")
    parser.add_argument(
        "--wifi-threshold",
        type=float,
        default=None,
        help="Optional wifiMaxUs threshold used to emit over-limit sample counts",
    )
    parser.add_argument(
        "--disp-threshold",
        type=float,
        default=None,
        help="Optional dispPipeMaxUs threshold used to emit over-limit sample counts",
    )
    parser.add_argument(
        "--skip-first-wifi-samples",
        type=int,
        default=2,
        help="Number of initial wifi samples to exclude for warmup-adjusted robust metrics",
    )
    return parser.parse_args(argv)


def main() -> int:
    args = parse_args(sys.argv[1:])
    path = args.metrics_jsonl
    samples = 0
    ok_samples = 0

    heap_free_min = None
    heap_min_free_min = None
    heap_dma_min = None
    heap_dma_largest_min = None
    latency_max_peak = None
    proxy_drop_peak = None
    display_updates_first = None
    display_updates_last = None
    display_skips_first = None
    display_skips_last = None
    flush_max_peak = None
    loop_max_peak = None
    wifi_max_peak = None
    wifi_max_peak_excluding_first = None
    ble_drain_max_peak = None
    loop_peak_ts = ""
    loop_peak_wifi = None
    loop_peak_flush = None
    loop_peak_ble_drain = None
    loop_peak_display_updates = None
    loop_peak_rx_packets = None
    wifi_peak_ts = ""
    wifi_peak_excluding_first_ts = ""
    wifi_peak_loop = None
    wifi_peak_flush = None
    wifi_peak_ble_drain = None
    wifi_peak_display_updates = None
    wifi_peak_rx_packets = None
    flush_peak_ts = ""
    ble_drain_peak_ts = ""
    rx_packets_first = None
    rx_packets_last = None
    parse_successes_first = None
    parse_successes_last = None
    parse_failures_first = None
    parse_failures_last = None
    queue_drops_first = None
    queue_drops_last = None
    perf_drop_first = None
    perf_drop_last = None

    event_publish_first = None
    event_publish_last = None
    event_drop_first = None
    event_drop_last = None
    event_size_peak = None
    core_guard_tripped_count = 0

    # Additional SLO-aligned metrics (available in debug/metrics API)
    oversize_drops_first = None
    oversize_drops_last = None
    sd_max_peak = None
    fs_max_peak = None
    queue_high_water_first = None
    queue_high_water_peak = None
    wifi_connect_deferred_first = None
    wifi_connect_deferred_last = None
    reconnects_first = None
    reconnects_last = None
    disconnects_first = None
    disconnects_last = None
    dma_free_min_val = None
    dma_largest_min_val = None
    ble_process_max_peak = None
    disp_pipe_max_peak = None
    ble_mutex_timeout_first = None
    ble_mutex_timeout_last = None
    camera_budget_exceeded_first = None
    camera_budget_exceeded_last = None
    camera_load_failures_first = None
    camera_load_failures_last = None
    camera_index_swap_failures_first = None
    camera_index_swap_failures_last = None
    camera_max_tick_peak = None
    camera_max_window_hz_peak = None
    camera_max_window_hz_peak_ts = ""
    # Ring buffer of (epoch, cameraTicks) for sliding-window Hz computation.
    # Min window of 15 s prevents poll-interval aliasing (firmware is 5 Hz but
    # narrow poll windows can bunch ticks above 5 Hz in consecutive pairs).
    camera_tick_ring = []  # list of (epoch, ticks)
    CAMERA_HZ_MIN_WINDOW_S = 15.0
    gps_obs_drops_first = None
    gps_obs_drops_last = None
    wifi_samples = []
    disp_pipe_samples = []

    try:
        with open(path, "r", encoding="utf-8") as f:
            for raw in f:
                line = raw.strip()
                if not line:
                    continue
                samples += 1
                try:
                    rec = json.loads(line)
                except Exception:
                    continue
                if not rec.get("ok"):
                    continue
                data = rec.get("data")
                if not isinstance(data, dict):
                    continue
                ok_samples += 1

                heap_free_min = update_min(heap_free_min, num(data.get("heapFree")))
                heap_min_free_min = update_min(heap_min_free_min, num(data.get("heapMinFree")))
                heap_dma_min = update_min(heap_dma_min, num(data.get("heapDma")))
                heap_dma_largest_min = update_min(heap_dma_largest_min, num(data.get("heapDmaLargest")))
                latency_max_peak = update_max(latency_max_peak, num(data.get("latencyMaxUs")))
                flush_val = num(data.get("flushMaxUs"))
                loop_val = num(data.get("loopMaxUs"))
                wifi_val = num(data.get("wifiMaxUs"))
                ble_drain_val = num(data.get("bleDrainMaxUs"))
                disp_pipe_val = num(data.get("dispPipeMaxUs"))
                sample_ts = rec.get("ts") if isinstance(rec.get("ts"), str) else ""
                # Prefer firmware uptime for per-device rate calculations; host wall-clock
                # sampling jitter can inflate short-window Hz estimates under load.
                uptime_ms_val = num(data.get("uptimeMs"))
                sample_epoch = (uptime_ms_val / 1000.0) if uptime_ms_val is not None else parse_ts_epoch(sample_ts)

                if flush_val is not None and (flush_max_peak is None or flush_val > flush_max_peak):
                    flush_max_peak = flush_val
                    flush_peak_ts = sample_ts

                if loop_val is not None and (loop_max_peak is None or loop_val > loop_max_peak):
                    loop_max_peak = loop_val
                    loop_peak_ts = sample_ts
                    loop_peak_wifi = wifi_val
                    loop_peak_flush = flush_val
                    loop_peak_ble_drain = ble_drain_val
                    loop_peak_display_updates = num(data.get("displayUpdates"))
                    loop_peak_rx_packets = num(data.get("rxPackets"))

                if wifi_val is not None and (wifi_max_peak is None or wifi_val > wifi_max_peak):
                    wifi_max_peak = wifi_val
                    wifi_peak_ts = sample_ts
                    wifi_peak_loop = loop_val
                    wifi_peak_flush = flush_val
                    wifi_peak_ble_drain = ble_drain_val
                    wifi_peak_display_updates = num(data.get("displayUpdates"))
                    wifi_peak_rx_packets = num(data.get("rxPackets"))
                if wifi_val is not None:
                    wifi_samples.append(wifi_val)

                if ok_samples > 2 and wifi_val is not None and (
                    wifi_max_peak_excluding_first is None or wifi_val > wifi_max_peak_excluding_first
                ):
                    wifi_max_peak_excluding_first = wifi_val
                    wifi_peak_excluding_first_ts = sample_ts

                if ble_drain_val is not None and (ble_drain_max_peak is None or ble_drain_val > ble_drain_max_peak):
                    ble_drain_max_peak = ble_drain_val
                    ble_drain_peak_ts = sample_ts

                display_updates = num(data.get("displayUpdates"))
                if display_updates_first is None and display_updates is not None:
                    display_updates_first = display_updates
                if display_updates is not None:
                    display_updates_last = display_updates

                display_skips = num(data.get("displaySkips"))
                if display_skips_first is None and display_skips is not None:
                    display_skips_first = display_skips
                if display_skips is not None:
                    display_skips_last = display_skips

                rx_packets = num(data.get("rxPackets"))
                if rx_packets_first is None and rx_packets is not None:
                    rx_packets_first = rx_packets
                if rx_packets is not None:
                    rx_packets_last = rx_packets

                parse_successes = num(data.get("parseSuccesses"))
                if parse_successes_first is None and parse_successes is not None:
                    parse_successes_first = parse_successes
                if parse_successes is not None:
                    parse_successes_last = parse_successes

                parse_failures = num(data.get("parseFailures"))
                if parse_failures_first is None and parse_failures is not None:
                    parse_failures_first = parse_failures
                if parse_failures is not None:
                    parse_failures_last = parse_failures

                queue_drops = num(data.get("queueDrops"))
                if queue_drops_first is None and queue_drops is not None:
                    queue_drops_first = queue_drops
                if queue_drops is not None:
                    queue_drops_last = queue_drops

                perf_drop = num(data.get("perfDrop"))
                if perf_drop_first is None and perf_drop is not None:
                    perf_drop_first = perf_drop
                if perf_drop is not None:
                    perf_drop_last = perf_drop

                proxy = data.get("proxy")
                if isinstance(proxy, dict):
                    proxy_drop_peak = update_max(proxy_drop_peak, num(proxy.get("dropCount")))

                event_bus = data.get("eventBus")
                if isinstance(event_bus, dict):
                    pub = num(event_bus.get("publishCount"))
                    drp = num(event_bus.get("dropCount"))
                    siz = num(event_bus.get("size"))
                    if event_publish_first is None and pub is not None:
                        event_publish_first = pub
                    if pub is not None:
                        event_publish_last = pub
                    if event_drop_first is None and drp is not None:
                        event_drop_first = drp
                    if drp is not None:
                        event_drop_last = drp
                    event_size_peak = update_max(event_size_peak, siz)

                lockout = data.get("lockout")
                if isinstance(lockout, dict) and lockout.get("coreGuardTripped") is True:
                    core_guard_tripped_count += 1

                # Additional SLO-aligned metrics
                oversize_drops = num(data.get("oversizeDrops"))
                if oversize_drops_first is None and oversize_drops is not None:
                    oversize_drops_first = oversize_drops
                if oversize_drops is not None:
                    oversize_drops_last = oversize_drops

                sd_max_peak = update_max(sd_max_peak, num(data.get("sdMaxUs")))
                fs_max_peak = update_max(fs_max_peak, num(data.get("fsMaxUs")))
                queue_high_water = num(data.get("queueHighWater"))
                if queue_high_water_first is None and queue_high_water is not None:
                    queue_high_water_first = queue_high_water
                queue_high_water_peak = update_max(queue_high_water_peak, queue_high_water)

                wifi_connect_deferred = num(data.get("wifiConnectDeferred"))
                if wifi_connect_deferred_first is None and wifi_connect_deferred is not None:
                    wifi_connect_deferred_first = wifi_connect_deferred
                if wifi_connect_deferred is not None:
                    wifi_connect_deferred_last = wifi_connect_deferred

                reconnects_val = num(data.get("reconnects"))
                if reconnects_first is None and reconnects_val is not None:
                    reconnects_first = reconnects_val
                if reconnects_val is not None:
                    reconnects_last = reconnects_val

                disconnects_val = num(data.get("disconnects"))
                if disconnects_first is None and disconnects_val is not None:
                    disconnects_first = disconnects_val
                if disconnects_val is not None:
                    disconnects_last = disconnects_val

                dma_free_min_val = update_min(dma_free_min_val, num(data.get("heapDmaMin")))
                dma_largest_min_val = update_min(dma_largest_min_val, num(data.get("heapDmaLargestMin")))

                ble_process_max_peak = update_max(ble_process_max_peak, num(data.get("bleProcessMaxUs")))
                disp_pipe_max_peak = update_max(disp_pipe_max_peak, disp_pipe_val)
                if disp_pipe_val is not None:
                    disp_pipe_samples.append(disp_pipe_val)

                ble_mutex_timeout = num(data.get("bleMutexTimeout"))
                if ble_mutex_timeout_first is None and ble_mutex_timeout is not None:
                    ble_mutex_timeout_first = ble_mutex_timeout
                if ble_mutex_timeout is not None:
                    ble_mutex_timeout_last = ble_mutex_timeout

                camera_budget_exceeded = num(data.get("cameraBudgetExceeded"))
                if camera_budget_exceeded_first is None and camera_budget_exceeded is not None:
                    camera_budget_exceeded_first = camera_budget_exceeded
                if camera_budget_exceeded is not None:
                    camera_budget_exceeded_last = camera_budget_exceeded

                camera_load_failures = num(data.get("cameraLoadFailures"))
                if camera_load_failures_first is None and camera_load_failures is not None:
                    camera_load_failures_first = camera_load_failures
                if camera_load_failures is not None:
                    camera_load_failures_last = camera_load_failures

                camera_index_swap_failures = num(data.get("cameraIndexSwapFailures"))
                if camera_index_swap_failures_first is None and camera_index_swap_failures is not None:
                    camera_index_swap_failures_first = camera_index_swap_failures
                if camera_index_swap_failures is not None:
                    camera_index_swap_failures_last = camera_index_swap_failures

                camera_max_tick_peak = update_max(camera_max_tick_peak, num(data.get("cameraMaxTickUs")))

                camera_ticks = num(data.get("cameraTicks"))
                if camera_ticks is not None and sample_epoch is not None:
                    camera_tick_ring.append((sample_epoch, camera_ticks))
                    # Find the oldest entry that gives us >= CAMERA_HZ_MIN_WINDOW_S
                    best_start = None
                    for si in range(len(camera_tick_ring) - 2, -1, -1):
                        span = sample_epoch - camera_tick_ring[si][0]
                        if span >= CAMERA_HZ_MIN_WINDOW_S:
                            best_start = si
                            break
                    if best_start is not None:
                        ref_epoch, ref_ticks = camera_tick_ring[best_start]
                        dt_seconds = sample_epoch - ref_epoch
                        tick_inc = camera_ticks - ref_ticks
                        if dt_seconds > 0 and tick_inc >= 0:
                            hz = tick_inc / dt_seconds
                            if camera_max_window_hz_peak is None or hz > camera_max_window_hz_peak:
                                camera_max_window_hz_peak = hz
                                camera_max_window_hz_peak_ts = sample_ts

                gps_obs_drops = num(data.get("gpsObsDrops"))
                if gps_obs_drops_first is None and gps_obs_drops is not None:
                    gps_obs_drops_first = gps_obs_drops
                if gps_obs_drops is not None:
                    gps_obs_drops_last = gps_obs_drops
    except FileNotFoundError:
        pass

    wifi_skip = max(args.skip_first_wifi_samples, 0)
    wifi_samples_excluding_first = wifi_samples[wifi_skip:] if wifi_skip > 0 else list(wifi_samples)

    wifi_p95_raw = percentile(wifi_samples, 95.0)
    wifi_p95_excluding_first = percentile(wifi_samples_excluding_first, 95.0)
    disp_pipe_p95 = percentile(disp_pipe_samples, 95.0)

    wifi_over_limit_count_raw = None
    wifi_over_limit_count_excluding_first = None
    if args.wifi_threshold is not None:
        wifi_over_limit_count_raw = sum(1 for val in wifi_samples if val > args.wifi_threshold)
        wifi_over_limit_count_excluding_first = sum(
            1 for val in wifi_samples_excluding_first if val > args.wifi_threshold
        )

    disp_pipe_over_limit_count = None
    if args.disp_threshold is not None:
        disp_pipe_over_limit_count = sum(1 for val in disp_pipe_samples if val > args.disp_threshold)

    emit("samples", samples)
    emit("ok_samples", ok_samples)
    emit("heap_free_min", heap_free_min)
    emit("heap_min_free_min", heap_min_free_min)
    emit("heap_dma_min", heap_dma_min)
    emit("heap_dma_largest_min", heap_dma_largest_min)
    emit("latency_max_peak", latency_max_peak)
    emit("proxy_drop_peak", proxy_drop_peak)
    emit("display_updates_first", display_updates_first)
    emit("display_updates_last", display_updates_last)
    emit("display_skips_first", display_skips_first)
    emit("display_skips_last", display_skips_last)
    emit("flush_max_peak", flush_max_peak)
    emit("loop_max_peak", loop_max_peak)
    emit("wifi_max_peak", wifi_max_peak)
    emit("wifi_max_peak_excluding_first", wifi_max_peak_excluding_first)
    emit("ble_drain_max_peak", ble_drain_max_peak)
    emit("loop_peak_ts", loop_peak_ts)
    emit("loop_peak_wifi", loop_peak_wifi)
    emit("loop_peak_flush", loop_peak_flush)
    emit("loop_peak_ble_drain", loop_peak_ble_drain)
    emit("loop_peak_display_updates", loop_peak_display_updates)
    emit("loop_peak_rx_packets", loop_peak_rx_packets)
    emit("wifi_peak_ts", wifi_peak_ts)
    emit("wifi_peak_excluding_first_ts", wifi_peak_excluding_first_ts)
    emit("wifi_peak_loop", wifi_peak_loop)
    emit("wifi_peak_flush", wifi_peak_flush)
    emit("wifi_peak_ble_drain", wifi_peak_ble_drain)
    emit("wifi_peak_display_updates", wifi_peak_display_updates)
    emit("wifi_peak_rx_packets", wifi_peak_rx_packets)
    emit("flush_peak_ts", flush_peak_ts)
    emit("ble_drain_peak_ts", ble_drain_peak_ts)
    emit("rx_packets_first", rx_packets_first)
    emit("rx_packets_last", rx_packets_last)
    emit("parse_successes_first", parse_successes_first)
    emit("parse_successes_last", parse_successes_last)
    emit("parse_failures_first", parse_failures_first)
    emit("parse_failures_last", parse_failures_last)
    emit("queue_drops_first", queue_drops_first)
    emit("queue_drops_last", queue_drops_last)
    emit("perf_drop_first", perf_drop_first)
    emit("perf_drop_last", perf_drop_last)
    emit("event_publish_first", event_publish_first)
    emit("event_publish_last", event_publish_last)
    emit("event_drop_first", event_drop_first)
    emit("event_drop_last", event_drop_last)
    emit("event_size_peak", event_size_peak)
    emit("core_guard_tripped_count", core_guard_tripped_count)

    # Additional SLO-aligned metrics
    emit("oversize_drops_first", oversize_drops_first)
    emit("oversize_drops_last", oversize_drops_last)
    emit("sd_max_peak", sd_max_peak)
    emit("fs_max_peak", fs_max_peak)
    emit("queue_high_water_first", queue_high_water_first)
    emit("queue_high_water_peak", queue_high_water_peak)
    emit("wifi_connect_deferred_first", wifi_connect_deferred_first)
    emit("wifi_connect_deferred_last", wifi_connect_deferred_last)
    emit("reconnects_first", reconnects_first)
    emit("reconnects_last", reconnects_last)
    emit("disconnects_first", disconnects_first)
    emit("disconnects_last", disconnects_last)
    emit("dma_free_min", dma_free_min_val)
    emit("dma_largest_min", dma_largest_min_val)
    emit("ble_process_max_peak", ble_process_max_peak)
    emit("disp_pipe_max_peak", disp_pipe_max_peak)
    emit("wifi_sample_count", len(wifi_samples))
    emit("wifi_sample_count_excluding_first", len(wifi_samples_excluding_first))
    emit("wifi_p95_raw", round(wifi_p95_raw, 3) if wifi_p95_raw is not None else None)
    emit(
        "wifi_p95_excluding_first",
        round(wifi_p95_excluding_first, 3) if wifi_p95_excluding_first is not None else None,
    )
    emit("wifi_over_limit_count_raw", wifi_over_limit_count_raw)
    emit("wifi_over_limit_count_excluding_first", wifi_over_limit_count_excluding_first)
    emit("disp_pipe_sample_count", len(disp_pipe_samples))
    emit("disp_pipe_p95", round(disp_pipe_p95, 3) if disp_pipe_p95 is not None else None)
    emit("disp_pipe_over_limit_count", disp_pipe_over_limit_count)
    emit("camera_max_tick_peak", camera_max_tick_peak)
    emit("camera_max_window_hz_peak", round(camera_max_window_hz_peak, 3) if camera_max_window_hz_peak is not None else None)
    emit("camera_max_window_hz_peak_ts", camera_max_window_hz_peak_ts)

    inherited_counter_suspect = 0
    for first_val in (queue_drops_first, perf_drop_first, event_drop_first):
        if first_val is not None and first_val > 0:
            inherited_counter_suspect = 1
            break
    emit("inherited_counter_suspect", inherited_counter_suspect)

    if oversize_drops_first is None or oversize_drops_last is None:
        print("oversize_drops_delta=")
    else:
        print(f"oversize_drops_delta={oversize_drops_last - oversize_drops_first}")

    if wifi_connect_deferred_first is None or wifi_connect_deferred_last is None:
        print("wifi_connect_deferred_delta=")
    else:
        print(f"wifi_connect_deferred_delta={wifi_connect_deferred_last - wifi_connect_deferred_first}")

    if reconnects_first is None or reconnects_last is None:
        print("reconnects_delta=")
    else:
        print(f"reconnects_delta={reconnects_last - reconnects_first}")

    if disconnects_first is None or disconnects_last is None:
        print("disconnects_delta=")
    else:
        print(f"disconnects_delta={disconnects_last - disconnects_first}")

    if ble_mutex_timeout_first is None or ble_mutex_timeout_last is None:
        print("ble_mutex_timeout_delta=")
    else:
        print(f"ble_mutex_timeout_delta={ble_mutex_timeout_last - ble_mutex_timeout_first}")

    if camera_budget_exceeded_first is None or camera_budget_exceeded_last is None:
        print("camera_budget_exceeded_delta=")
    else:
        print(
            "camera_budget_exceeded_delta="
            f"{camera_budget_exceeded_last - camera_budget_exceeded_first}"
        )

    if camera_load_failures_first is None or camera_load_failures_last is None:
        print("camera_load_failures_delta=")
    else:
        print(f"camera_load_failures_delta={camera_load_failures_last - camera_load_failures_first}")

    if camera_index_swap_failures_first is None or camera_index_swap_failures_last is None:
        print("camera_index_swap_failures_delta=")
    else:
        print(
            "camera_index_swap_failures_delta="
            f"{camera_index_swap_failures_last - camera_index_swap_failures_first}"
        )

    if gps_obs_drops_first is None or gps_obs_drops_last is None:
        print("gps_obs_drops_delta=")
    else:
        print(f"gps_obs_drops_delta={gps_obs_drops_last - gps_obs_drops_first}")

    if event_publish_first is None or event_publish_last is None:
        print("event_publish_delta=")
    else:
        print(f"event_publish_delta={event_publish_last - event_publish_first}")

    if event_drop_first is None or event_drop_last is None:
        print("event_drop_delta=")
    else:
        print(f"event_drop_delta={event_drop_last - event_drop_first}")

    if display_updates_first is None or display_updates_last is None:
        print("display_updates_delta=")
    else:
        print(f"display_updates_delta={display_updates_last - display_updates_first}")

    if display_skips_first is None or display_skips_last is None:
        print("display_skips_delta=")
    else:
        print(f"display_skips_delta={display_skips_last - display_skips_first}")

    if rx_packets_first is None or rx_packets_last is None:
        print("rx_packets_delta=")
    else:
        print(f"rx_packets_delta={rx_packets_last - rx_packets_first}")

    if parse_successes_first is None or parse_successes_last is None:
        print("parse_successes_delta=")
    else:
        print(f"parse_successes_delta={parse_successes_last - parse_successes_first}")

    if parse_failures_first is None or parse_failures_last is None:
        print("parse_failures_delta=")
    else:
        print(f"parse_failures_delta={parse_failures_last - parse_failures_first}")

    if queue_drops_first is None or queue_drops_last is None:
        print("queue_drops_delta=")
    else:
        print(f"queue_drops_delta={queue_drops_last - queue_drops_first}")

    if perf_drop_first is None or perf_drop_last is None:
        print("perf_drop_delta=")
    else:
        print(f"perf_drop_delta={perf_drop_last - perf_drop_first}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
